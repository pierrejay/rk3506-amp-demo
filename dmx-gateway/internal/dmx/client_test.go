// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

import (
	"log/slog"
	"os"
	"testing"

	"dmx-gateway/internal/config"
)

func TestNewClient(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelError}))

	cfg := config.DMXConfig{
		Client:     "/nonexistent/dmx_client",
		ThrottleMs: 25,
		TimeoutMs:  500,
	}

	// Should not error even if client doesn't exist (simulation mode)
	client, err := NewClient(cfg, logger)
	if err != nil {
		t.Fatalf("NewClient failed: %v", err)
	}

	if client.clientPath != cfg.Client {
		t.Errorf("expected client path %s, got %s", cfg.Client, client.clientPath)
	}
}

func TestClientTimeout(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelError}))

	cfg := config.DMXConfig{
		Client:     "/nonexistent/dmx_client",
		ThrottleMs: 25,
		TimeoutMs:  100, // Short timeout
	}

	client, _ := NewClient(cfg, logger)

	// Should fail because client doesn't exist
	err := client.Enable()
	if err == nil {
		t.Error("expected error for nonexistent client")
	}
}

func TestStatus(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelError}))

	cfg := config.DMXConfig{
		Client:     "/nonexistent/dmx_client",
		TimeoutMs:  100,
	}

	client, _ := NewClient(cfg, logger)

	// Should fail because client doesn't exist
	_, err := client.Status()
	if err == nil {
		t.Error("expected error for nonexistent client")
	}
}
