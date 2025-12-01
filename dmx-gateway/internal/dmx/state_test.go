// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

import (
	"log/slog"
	"os"
	"testing"
	"time"

	"dmx-gateway/internal/config"
)

func testConfig() *config.Config {
	return &config.Config{
		Server: config.ServerConfig{HTTP: ":8080"},
		DMX:    config.DMXConfig{Client: "mock", ThrottleMs: 0, TimeoutMs: 100},
		Lights: map[string]map[string][]config.Channel{
			"rack1": {
				"level1": {
					{Ch: 1, Color: "blue", Name: ""},
					{Ch: 2, Color: "red", Name: ""},
				},
				"level2": {
					{Ch: 3, Color: "white", Name: ""},
				},
			},
		},
	}
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelError}))
}

func TestStateSubscribe(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	ch := state.Subscribe()
	defer state.Unsubscribe(ch)

	select {
	case <-ch:
		t.Error("channel should be empty initially")
	default:
		// OK
	}
}

func TestStateBroadcast(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	ch := state.Subscribe()
	defer state.Unsubscribe(ch)

	// Trigger broadcast via SetLight
	state.SetLight("rack1", "level1", map[string]uint8{"blue": 100})

	select {
	case data := <-ch:
		// Channel now receives pre-marshaled JSON
		if len(data) == 0 {
			t.Error("expected non-empty JSON data")
		}
		// Verify it's valid JSON with expected structure
		if string(data)[0] != '{' {
			t.Errorf("expected JSON object, got %s", string(data)[:20])
		}
	case <-time.After(100 * time.Millisecond):
		t.Error("timeout waiting for broadcast")
	}
}

func TestStateGetLights(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	lights := state.GetLights()

	if len(lights) != 2 {
		t.Errorf("expected 2 lights, got %d", len(lights))
	}

	light1 := lights["rack1/level1"]
	if light1 == nil {
		t.Fatal("rack1/level1 not found")
	}

	// Now uses typed struct
	if len(light1.Channels) != 2 {
		t.Errorf("expected 2 channels in rack1/level1, got %d", len(light1.Channels))
	}
}

func TestStateGetLight(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	light := state.GetLight("rack1", "level1")
	if light == nil {
		t.Fatal("rack1/level1 not found")
	}

	// Now uses typed struct
	if light.Group != "rack1" {
		t.Errorf("expected group 'rack1', got %v", light.Group)
	}
	if light.Name != "level1" {
		t.Errorf("expected name 'level1', got %v", light.Name)
	}

	// Non-existent light
	light = state.GetLight("rack1", "nonexistent")
	if light != nil {
		t.Error("expected nil for nonexistent light")
	}
}

func TestStateGetChannels(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	channels := state.GetChannels()

	for i, v := range channels {
		if v != 0 {
			t.Errorf("channel %d should be 0, got %d", i+1, v)
		}
	}
}

func TestStateSetChannelUpdatesState(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	_ = state.SetChannel(1, 128)

	channels := state.GetChannels()
	if channels[0] != 128 {
		t.Errorf("expected channel 1 to be 128, got %d", channels[0])
	}
}

func TestStateSetChannelBounds(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	_ = state.SetChannel(0, 100)
	channels := state.GetChannels()
	if channels[0] != 0 {
		t.Errorf("channel 0 should not update channel 1")
	}

	_ = state.SetChannel(513, 100)
	// No crash = pass
}

func TestStateGetConfig(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	if state.GetConfig() != cfg {
		t.Error("GetConfig should return the config")
	}
}

func TestStateIsEnabled(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	if state.IsEnabled() {
		t.Error("should be disabled initially")
	}
}

func TestStateGetGroups(t *testing.T) {
	cfg := testConfig()
	logger := testLogger()

	client, _ := NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	state := NewState(cfg, client, logger)

	groups := state.GetGroups()
	if len(groups) != 1 {
		t.Errorf("expected 1 group, got %d", len(groups))
	}
}
