// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"dmx-gateway/internal/config"
	"dmx-gateway/internal/dmx"
	"dmx-gateway/internal/http"
	"dmx-gateway/internal/modbus"
	"dmx-gateway/internal/mqtt"
	"dmx-gateway/internal/scheduler"
)

func main() {
	var (
		configPath = flag.String("config", "config.yaml", "Path to configuration file")
		logLevel   = flag.String("log-level", "INFO", "Log level (DEBUG, INFO, WARN, ERROR)")
		dryRun     = flag.Bool("dry-run", false, "Validate config and exit")
	)
	flag.Parse()

	// Setup slog
	level := parseLogLevel(*logLevel)
	opts := &slog.HandlerOptions{Level: level}
	handler := slog.NewTextHandler(os.Stdout, opts)
	logger := slog.New(handler)
	slog.SetDefault(logger)

	logger.Info("DMX Gateway starting", "version", "1.0.0")

	// Load configuration
	cfg, err := config.Load(*configPath)
	if err != nil {
		logger.Error("Failed to load configuration", "error", err, "path", *configPath)
		os.Exit(1)
	}

	// Count total lights
	totalLights := 0
	for _, group := range cfg.Lights {
		totalLights += len(group)
	}
	logger.Info("Configuration loaded",
		"groups", len(cfg.Lights),
		"lights", totalLights,
		"http", cfg.Server.HTTP)

	if *dryRun {
		logger.Info("Dry run mode - configuration is valid")
		os.Exit(0)
	}

	// Setup context with signal handling
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		sig := <-sigChan
		logger.Info("Received signal, shutting down", "signal", sig)
		cancel()
	}()

	// Initialize DMX client
	dmxClient, err := dmx.NewClient(cfg.DMX, logger)
	if err != nil {
		logger.Error("Failed to initialize DMX client", "error", err)
		os.Exit(1)
	}

	// Initialize state manager
	state := dmx.NewState(cfg, dmxClient, logger)

	// Auto-enable DMX if configured
	if cfg.DMX.AutoEnable {
		if err := state.Enable(); err != nil {
			logger.Warn("Failed to auto-enable DMX", "error", err)
		} else {
			logger.Info("DMX auto-enabled on startup")
		}
	}

	// Start periodic refresh if configured
	if cfg.DMX.RefreshMs > 0 {
		state.StartRefresh(time.Duration(cfg.DMX.RefreshMs) * time.Millisecond)
	}

	// Start HTTP server with WebSocket
	httpServer := http.NewServer(cfg, state, logger)
	if err := httpServer.Start(); err != nil {
		logger.Error("Failed to start HTTP server", "error", err)
		os.Exit(1)
	}

	// Start Modbus TCP server if configured
	var modbusServer *modbus.Server
	if cfg.Modbus != nil {
		modbusServer = modbus.NewServer(&modbus.Config{
			Port: cfg.Modbus.Port,
		}, state, logger)
		if err := modbusServer.Start(); err != nil {
			logger.Error("Failed to start Modbus server", "error", err)
			os.Exit(1)
		}
	}

	// Start MQTT client if configured
	var mqttClient *mqtt.Client
	if cfg.MQTT != nil {
		mqttClient = mqtt.NewClient(&mqtt.Config{
			Broker:   cfg.MQTT.Broker,
			ClientID: cfg.MQTT.ClientID,
			Username: cfg.MQTT.Username,
			Password: cfg.MQTT.Password,
			Prefix:   cfg.MQTT.TopicPrefix,
		}, state, logger)
		if err := mqttClient.Start(); err != nil {
			logger.Error("Failed to start MQTT client", "error", err)
			os.Exit(1)
		}
	}

	// Start scheduler if configured
	var sched *scheduler.Scheduler
	if cfg.Schedule != nil && len(cfg.Schedule.Events) > 0 {
		var err error
		sched, err = scheduler.New(cfg.Schedule, state, logger)
		if err != nil {
			logger.Error("Failed to create scheduler", "error", err)
			os.Exit(1)
		}
		sched.Start()
		httpServer.SetScheduler(sched)
	}

	logger.Info("DMX Gateway ready",
		"http", cfg.Server.HTTP,
		"dmx_client", cfg.DMX.Client,
		"modbus", cfg.Modbus != nil,
		"mqtt", cfg.MQTT != nil,
		"schedule", cfg.Schedule != nil)

	// Wait for shutdown
	<-ctx.Done()

	// Graceful shutdown
	logger.Info("Initiating graceful shutdown...")

	// Stop refresh goroutine
	state.StopRefresh()

	// Stop scheduler
	if sched != nil {
		sched.Stop()
	}

	// Stop MQTT client
	if mqttClient != nil {
		mqttClient.Stop()
	}

	// Stop Modbus server
	if modbusServer != nil {
		modbusServer.Stop()
	}

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()

	// Stop HTTP server
	if err := httpServer.Shutdown(shutdownCtx); err != nil {
		logger.Error("HTTP server shutdown error", "error", err)
	}

	// Disable DMX output
	if err := dmxClient.Disable(); err != nil {
		logger.Warn("Failed to disable DMX on shutdown", "error", err)
	}

	logger.Info("DMX Gateway stopped")
}

func parseLogLevel(level string) slog.Level {
	switch strings.ToUpper(level) {
	case "DEBUG":
		return slog.LevelDebug
	case "INFO":
		return slog.LevelInfo
	case "WARN":
		return slog.LevelWarn
	case "ERROR":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}
