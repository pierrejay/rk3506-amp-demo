// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

import (
	"context"
	"fmt"
	"log/slog"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"

	"dmx-gateway/internal/config"
)

// Client wraps the dmx_client subprocess
type Client struct {
	clientPath string
	device     string // RPMSG device path (empty = use client default)
	timeout    time.Duration
	mu         sync.Mutex
	logger     *slog.Logger
}

// NewClient creates a new DMX client wrapper
func NewClient(cfg config.DMXConfig, logger *slog.Logger) (*Client, error) {
	c := &Client{
		clientPath: cfg.Client,
		device:     cfg.Device,
		timeout:    time.Duration(cfg.TimeoutMs) * time.Millisecond,
		logger:     logger,
	}

	// Test that client exists and is executable
	if _, err := exec.LookPath(cfg.Client); err != nil {
		// In dev mode, we might not have the actual client
		logger.Warn("DMX client not found, running in simulation mode", "path", cfg.Client)
	}

	if c.device != "" {
		logger.Info("Using custom RPMSG device", "device", c.device)
	}

	return c, nil
}

// exec runs a dmx_client command
func (c *Client) exec(args ...string) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), c.timeout)
	defer cancel()

	// Prepend device flag if configured
	if c.device != "" {
		args = append([]string{"-d", c.device}, args...)
	}

	cmd := exec.CommandContext(ctx, c.clientPath, args...)
	output, err := cmd.CombinedOutput()

	if ctx.Err() == context.DeadlineExceeded {
		return "", fmt.Errorf("command timeout after %v", c.timeout)
	}

	if err != nil {
		return "", fmt.Errorf("dmx_client %v: %w (output: %s)", args, err, string(output))
	}

	return strings.TrimSpace(string(output)), nil
}

// Enable starts DMX transmission
func (c *Client) Enable() error {
	c.logger.Debug("DMX enable")
	_, err := c.exec("enable")
	return err
}

// Disable stops DMX transmission
func (c *Client) Disable() error {
	c.logger.Debug("DMX disable")
	_, err := c.exec("disable")
	return err
}

// Blackout sets all channels to 0
func (c *Client) Blackout() error {
	c.logger.Debug("DMX blackout")
	_, err := c.exec("blackout")
	return err
}

// SetChannel sets a single DMX channel value
func (c *Client) SetChannel(channel int, value uint8) error {
	c.logger.Debug("DMX set channel", "channel", channel, "value", value)
	_, err := c.exec("set", strconv.Itoa(channel), strconv.Itoa(int(value)))
	return err
}

// SetChannels sets multiple consecutive DMX channels starting from startChannel
func (c *Client) SetChannels(startChannel int, values []uint8) error {
	if len(values) == 0 {
		return nil
	}

	// Format: dmx_client set <start> <v1>,<v2>,<v3>,...
	valStrs := make([]string, len(values))
	for i, v := range values {
		valStrs[i] = strconv.Itoa(int(v))
	}

	c.logger.Debug("DMX set channels", "start", startChannel, "count", len(values))
	_, err := c.exec("set", strconv.Itoa(startChannel), strings.Join(valStrs, ","))
	return err
}

// Status returns the current DMX status
func (c *Client) Status() (*Status, error) {
	output, err := c.exec("--json", "status")
	if err != nil {
		return nil, err
	}

	// Parse JSON output
	// Expected: {"enabled":true,"frame_count":1234,"fps":44.00}
	status := &Status{}

	// Simple parsing without json package for minimal deps
	// This is a simplified parser - in production use encoding/json
	if strings.Contains(output, `"enabled":true`) {
		status.Enabled = true
	}

	// Extract fps
	if idx := strings.Index(output, `"fps":`); idx >= 0 {
		rest := output[idx+6:]
		if end := strings.IndexAny(rest, ",}"); end >= 0 {
			if fps, err := strconv.ParseFloat(rest[:end], 64); err == nil {
				status.FPS = fps
			}
		}
	}

	// Extract frame_count
	if idx := strings.Index(output, `"frame_count":`); idx >= 0 {
		rest := output[idx+14:]
		if end := strings.IndexAny(rest, ",}"); end >= 0 {
			if count, err := strconv.ParseUint(rest[:end], 10, 64); err == nil {
				status.FrameCount = count
			}
		}
	}

	return status, nil
}

// Status represents DMX status
type Status struct {
	Enabled    bool    `json:"enabled"`
	FPS        float64 `json:"fps"`
	FrameCount uint64  `json:"frame_count"`
	Errors     uint64  `json:"errors"`
}
