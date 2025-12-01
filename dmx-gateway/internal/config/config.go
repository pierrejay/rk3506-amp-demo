// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package config

import (
	"fmt"
	"os"
	"strings"

	"gopkg.in/yaml.v3"
)

// Load reads and parses the configuration file
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config file: %w", err)
	}

	var cfg Config
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("parse config: %w", err)
	}

	cfg.applyDefaults()

	if err := cfg.Validate(); err != nil {
		return nil, fmt.Errorf("validate config: %w", err)
	}

	return &cfg, nil
}

// applyDefaults sets default values for missing config
func (c *Config) applyDefaults() {
	if c.Server.HTTP == "" {
		c.Server.HTTP = ":8080"
	}
	if c.DMX.Client == "" {
		c.DMX.Client = "/usr/bin/dmx_client"
	}
	if c.DMX.ThrottleMs == 0 {
		c.DMX.ThrottleMs = 25
	}
	if c.DMX.TimeoutMs == 0 {
		c.DMX.TimeoutMs = 500
	}
}

// Validate checks the configuration for errors
func (c *Config) Validate() error {
	if len(c.Lights) == 0 {
		return fmt.Errorf("no lights defined")
	}

	usedChannels := make(map[int]string)

	for groupName, lights := range c.Lights {
		if len(lights) == 0 {
			return fmt.Errorf("group %q has no lights", groupName)
		}

		for lightName, channels := range lights {
			fullName := groupName + "/" + lightName
			if len(channels) == 0 {
				return fmt.Errorf("light %q has no channels", fullName)
			}

			for _, ch := range channels {
				if ch.Ch < 1 || ch.Ch > 512 {
					return fmt.Errorf("light %q: channel %d out of range (1-512)", fullName, ch.Ch)
				}

				if ch.Color == "" {
					return fmt.Errorf("light %q: channel %d missing color", fullName, ch.Ch)
				}

				if existing, ok := usedChannels[ch.Ch]; ok {
					return fmt.Errorf("channel %d used by both %q and %q", ch.Ch, existing, fullName)
				}
				usedChannels[ch.Ch] = fullName
			}
		}
	}

	return nil
}

// ResolveColor converts a color name to hex, or returns hex as-is
func ResolveColor(color string) string {
	if strings.HasPrefix(color, "#") {
		return color
	}
	if hex, ok := ColorPalette[color]; ok {
		return hex
	}
	return "#FFFFFF"
}

// ResolveLights returns all lights with resolved channels
func (c *Config) ResolveLights() []ResolvedLight {
	var result []ResolvedLight

	for groupName, lights := range c.Lights {
		for lightName, channels := range lights {
			rl := ResolvedLight{
				Group:    groupName,
				Name:     lightName,
				Channels: make([]ResolvedChannel, len(channels)),
			}

			for i, ch := range channels {
				channelName := ch.Name
				if channelName == "" {
					channelName = ch.Color
				}

				rl.Channels[i] = ResolvedChannel{
					Ch:    ch.Ch,
					Color: ResolveColor(ch.Color),
					Name:  channelName,
					Value: 0,
				}
			}

			result = append(result, rl)
		}
	}

	return result
}

// GetLight returns resolved channels for a light (group/light format)
func (c *Config) GetLight(group, name string) []ResolvedChannel {
	lights, ok := c.Lights[group]
	if !ok {
		return nil
	}
	channels, ok := lights[name]
	if !ok {
		return nil
	}

	result := make([]ResolvedChannel, len(channels))
	for i, ch := range channels {
		channelName := ch.Name
		if channelName == "" {
			channelName = ch.Color
		}
		result[i] = ResolvedChannel{
			Ch:    ch.Ch,
			Color: ResolveColor(ch.Color),
			Name:  channelName,
		}
	}
	return result
}

// GetGroupLights returns the light names in a group
func (c *Config) GetGroupLights(groupName string) []string {
	lights, ok := c.Lights[groupName]
	if !ok {
		return nil
	}
	names := make([]string, 0, len(lights))
	for name := range lights {
		names = append(names, name)
	}
	return names
}

// GroupNames returns all group names
func (c *Config) GroupNames() []string {
	names := make([]string, 0, len(c.Lights))
	for name := range c.Lights {
		names = append(names, name)
	}
	return names
}

// LightKey returns "group/light" key
func LightKey(group, light string) string {
	return group + "/" + light
}
