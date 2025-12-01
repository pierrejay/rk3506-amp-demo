// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadValidConfig(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 1, color: blue }
      - { ch: 2, color: red }
`
	cfg := loadFromString(t, yaml)

	if cfg.Server.HTTP != ":8080" {
		t.Errorf("expected http :8080, got %s", cfg.Server.HTTP)
	}

	if len(cfg.Lights) != 1 {
		t.Errorf("expected 1 group, got %d", len(cfg.Lights))
	}

	if len(cfg.Lights["rack1"]["level1"]) != 2 {
		t.Errorf("expected 2 channels, got %d", len(cfg.Lights["rack1"]["level1"]))
	}
}

func TestLoadDefaultValues(t *testing.T) {
	yaml := `
lights:
  test:
    light1:
      - { ch: 1, color: white }
`
	cfg := loadFromString(t, yaml)

	if cfg.Server.HTTP != ":8080" {
		t.Errorf("expected default http :8080, got %s", cfg.Server.HTTP)
	}

	if cfg.DMX.Client != "/usr/bin/dmx_client" {
		t.Errorf("expected default dmx client, got %s", cfg.DMX.Client)
	}

	if cfg.DMX.ThrottleMs != 25 {
		t.Errorf("expected default throttle 25, got %d", cfg.DMX.ThrottleMs)
	}
}

func TestValidateNoLights(t *testing.T) {
	yaml := `
server:
  http: ":8080"
`
	_, err := loadFromStringErr(yaml)
	if err == nil {
		t.Error("expected error for config with no lights")
	}
}

func TestValidateChannelOutOfRange(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 0, color: blue }
`
	_, err := loadFromStringErr(yaml)
	if err == nil {
		t.Error("expected error for channel 0")
	}

	yaml = `
lights:
  rack1:
    level1:
      - { ch: 513, color: blue }
`
	_, err = loadFromStringErr(yaml)
	if err == nil {
		t.Error("expected error for channel 513")
	}
}

func TestValidateMissingColor(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 1 }
`
	_, err := loadFromStringErr(yaml)
	if err == nil {
		t.Error("expected error for missing color")
	}
}

func TestValidateDuplicateChannel(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 1, color: blue }
    level2:
      - { ch: 1, color: red }
`
	_, err := loadFromStringErr(yaml)
	if err == nil {
		t.Error("expected error for duplicate channel")
	}
}

func TestResolveColor(t *testing.T) {
	tests := []struct {
		input    string
		expected string
	}{
		{"blue", "#0047AB"},
		{"red", "#FF2400"},
		{"far_red", "#8B0000"},
		{"white", "#FFFAF0"},
		{"#AABBCC", "#AABBCC"},
		{"unknown", "#FFFFFF"},
		{"", "#FFFFFF"},
	}

	for _, tc := range tests {
		result := ResolveColor(tc.input)
		if result != tc.expected {
			t.Errorf("ResolveColor(%q) = %q, want %q", tc.input, result, tc.expected)
		}
	}
}

func TestResolveLights(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 1, color: blue }
      - { ch: 2, color: red, name: custom_name }
`
	cfg := loadFromString(t, yaml)
	lights := cfg.ResolveLights()

	if len(lights) != 1 {
		t.Fatalf("expected 1 light, got %d", len(lights))
	}

	light := lights[0]
	if light.Group != "rack1" {
		t.Errorf("expected group 'rack1', got %s", light.Group)
	}
	if light.Name != "level1" {
		t.Errorf("expected name 'level1', got %s", light.Name)
	}

	if len(light.Channels) != 2 {
		t.Fatalf("expected 2 channels, got %d", len(light.Channels))
	}

	// First channel: name defaults to color
	if light.Channels[0].Name != "blue" {
		t.Errorf("expected channel name 'blue', got %s", light.Channels[0].Name)
	}
	if light.Channels[0].Color != "#0047AB" {
		t.Errorf("expected color #0047AB, got %s", light.Channels[0].Color)
	}

	// Second channel: custom name
	if light.Channels[1].Name != "custom_name" {
		t.Errorf("expected channel name 'custom_name', got %s", light.Channels[1].Name)
	}
}

func TestGetLight(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 5, color: green }
`
	cfg := loadFromString(t, yaml)

	channels := cfg.GetLight("rack1", "level1")
	if len(channels) != 1 {
		t.Fatalf("expected 1 channel, got %d", len(channels))
	}

	if channels[0].Ch != 5 {
		t.Errorf("expected ch 5, got %d", channels[0].Ch)
	}

	// Non-existent light
	channels = cfg.GetLight("rack1", "nonexistent")
	if channels != nil {
		t.Error("expected nil for nonexistent light")
	}

	// Non-existent group
	channels = cfg.GetLight("nonexistent", "level1")
	if channels != nil {
		t.Error("expected nil for nonexistent group")
	}
}

func TestGetGroupLights(t *testing.T) {
	yaml := `
lights:
  rack1:
    level1:
      - { ch: 1, color: blue }
    level2:
      - { ch: 2, color: red }
`
	cfg := loadFromString(t, yaml)

	lights := cfg.GetGroupLights("rack1")
	if len(lights) != 2 {
		t.Errorf("expected 2 lights in group, got %d", len(lights))
	}

	// Non-existent group
	lights = cfg.GetGroupLights("nonexistent")
	if lights != nil {
		t.Error("expected nil for nonexistent group")
	}
}

func TestLightKey(t *testing.T) {
	key := LightKey("rack1", "level1")
	if key != "rack1/level1" {
		t.Errorf("expected 'rack1/level1', got %s", key)
	}
}

// Helper functions

func loadFromString(t *testing.T, yaml string) *Config {
	t.Helper()
	cfg, err := loadFromStringErr(yaml)
	if err != nil {
		t.Fatalf("failed to load config: %v", err)
	}
	return cfg
}

func loadFromStringErr(yaml string) (*Config, error) {
	dir, err := os.MkdirTemp("", "config_test")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)

	path := filepath.Join(dir, "config.yaml")
	if err := os.WriteFile(path, []byte(yaml), 0644); err != nil {
		return nil, err
	}

	return Load(path)
}
