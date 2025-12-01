// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package mqtt

import (
	"encoding/json"
	"log/slog"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"dmx-gateway/internal/api"
	"dmx-gateway/internal/dmx"
)

// Config for MQTT client
type Config struct {
	Broker   string `yaml:"broker"`       // tcp://host:1883
	ClientID string `yaml:"client_id"`    // optional, defaults to "dmx-gateway"
	Username string `yaml:"username"`     // optional
	Password string `yaml:"password"`     // optional
	Prefix   string `yaml:"topic_prefix"` // topic prefix, defaults to "dmx"
}

// Client is the MQTT client for DMX gateway
type Client struct {
	cfg       *Config
	api       *api.Handler
	state     *dmx.State
	logger    *slog.Logger
	client    mqtt.Client
	stopChan  chan struct{}
}

// NewClient creates a new MQTT client
func NewClient(cfg *Config, state *dmx.State, logger *slog.Logger) *Client {
	if cfg.Prefix == "" {
		cfg.Prefix = "dmx"
	}
	if cfg.ClientID == "" {
		cfg.ClientID = "dmx-gateway"
	}

	return &Client{
		cfg:      cfg,
		api:      api.NewHandler(state),
		state:    state,
		logger:   logger,
		stopChan: make(chan struct{}),
	}
}

// Start connects to broker and subscribes to topics
func (c *Client) Start() error {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(c.cfg.Broker)
	opts.SetClientID(c.cfg.ClientID)
	opts.SetAutoReconnect(true)
	opts.SetConnectRetry(true)
	opts.SetConnectRetryInterval(5 * time.Second)

	if c.cfg.Username != "" {
		opts.SetUsername(c.cfg.Username)
		opts.SetPassword(c.cfg.Password)
	}

	opts.SetOnConnectHandler(c.onConnect)
	opts.SetConnectionLostHandler(c.onConnectionLost)

	c.client = mqtt.NewClient(opts)
	token := c.client.Connect()
	token.Wait()
	if err := token.Error(); err != nil {
		return err
	}

	// Start event forwarder
	go c.forwardEvents()

	c.logger.Info("MQTT client started", "broker", c.cfg.Broker, "prefix", c.cfg.Prefix)
	return nil
}

// Stop disconnects from broker
func (c *Client) Stop() {
	close(c.stopChan)
	if c.client != nil && c.client.IsConnected() {
		c.client.Disconnect(1000)
	}
	c.logger.Info("MQTT client stopped")
}

func (c *Client) onConnect(client mqtt.Client) {
	c.logger.Info("MQTT connected")

	// Subscribe to command topic
	cmdTopic := c.cfg.Prefix + "/cmd"
	client.Subscribe(cmdTopic, 1, c.handleCommand)
	c.logger.Debug("MQTT subscribed", "topic", cmdTopic)

	// Publish initial status
	c.publishStatus()
}

func (c *Client) onConnectionLost(client mqtt.Client, err error) {
	c.logger.Warn("MQTT connection lost", "error", err)
}

// handleCommand processes incoming MQTT commands
func (c *Client) handleCommand(client mqtt.Client, msg mqtt.Message) {
	c.logger.Debug("MQTT command received", "topic", msg.Topic(), "payload", string(msg.Payload()))

	// Use unified API handler
	resp := c.api.HandleJSON(msg.Payload())

	// Publish response
	respTopic := c.cfg.Prefix + "/response"
	client.Publish(respTopic, 0, false, resp)
}

// forwardEvents forwards DMX state changes to MQTT
func (c *Client) forwardEvents() {
	updates := c.state.Subscribe()
	defer c.state.Unsubscribe(updates)

	for {
		select {
		case data, ok := <-updates:
			if !ok {
				return
			}
			c.publishEvent(data)
		case <-c.stopChan:
			return
		}
	}
}

// publishEvent publishes a state change event (data is pre-marshaled JSON)
func (c *Client) publishEvent(data []byte) {
	if c.client == nil || !c.client.IsConnected() {
		return
	}

	topic := c.cfg.Prefix + "/event"
	c.client.Publish(topic, 0, false, data)
}

// MQTTStatusMessage for status publish (typed to avoid map allocation)
type MQTTStatusMessage struct {
	Type string             `json:"type"`
	Data dmx.StatusResponse `json:"data"`
}

// publishStatus publishes current status
func (c *Client) publishStatus() {
	if c.client == nil || !c.client.IsConnected() {
		return
	}

	data, _ := json.Marshal(MQTTStatusMessage{
		Type: "status",
		Data: c.state.GetStatus(),
	})
	topic := c.cfg.Prefix + "/status"
	c.client.Publish(topic, 0, true, data) // retained
}
