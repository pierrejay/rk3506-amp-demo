// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package config

// Config is the root configuration structure
// Lights are organized as: group -> light -> channels
type Config struct {
	Server   ServerConfig                      `yaml:"server"`
	DMX      DMXConfig                         `yaml:"dmx"`
	Modbus   *ModbusConfig                     `yaml:"modbus,omitempty"`
	MQTT     *MQTTConfig                       `yaml:"mqtt,omitempty"`
	Schedule *ScheduleConfig                   `yaml:"schedule,omitempty"`
	Lights   map[string]map[string][]Channel   `yaml:"lights"` // group -> light -> channels
}

// ScheduleConfig defines scheduler settings
type ScheduleConfig struct {
	Timezone string          `yaml:"timezone"` // e.g. "Europe/Paris", defaults to local
	Events   []ScheduleEvent `yaml:"events"`
}

// ScheduleEvent defines a scheduled action
type ScheduleEvent struct {
	Time     string                       `yaml:"time"`              // "HH:MM:SS"
	Set      map[string]map[string]uint8  `yaml:"set,omitempty"`     // target -> color -> value
	Blackout bool                         `yaml:"blackout,omitempty"`
}

// ModbusConfig defines Modbus TCP server settings
// Presence of this section enables Modbus
type ModbusConfig struct {
	Port string `yaml:"port"` // ":502" or ":5020"
}

// MQTTConfig defines MQTT client settings
// Presence of this section enables MQTT
type MQTTConfig struct {
	Broker      string `yaml:"broker"`       // tcp://host:1883
	ClientID    string `yaml:"client_id"`    // optional
	Username    string `yaml:"username"`     // optional
	Password    string `yaml:"password"`     // optional
	TopicPrefix string `yaml:"topic_prefix"` // defaults to "dmx"
}

// ServerConfig defines server endpoints
type ServerConfig struct {
	HTTP string `yaml:"http"`
}

// DMXConfig defines DMX backend settings
type DMXConfig struct {
	Client     string `yaml:"client"`
	Device     string `yaml:"device,omitempty"` // RPMSG device (e.g. /dev/ttyRPMSG1), empty = client default
	ThrottleMs int    `yaml:"throttle_ms"`
	TimeoutMs  int    `yaml:"timeout_ms"`
	RefreshMs  int    `yaml:"refresh_ms"` // Periodic state refresh (0 = disabled)
}

// Channel defines a single DMX channel with color
type Channel struct {
	Ch    int    `yaml:"ch"`
	Color string `yaml:"color"`
	Name  string `yaml:"name,omitempty"` // Optional, defaults to color
}

// ResolvedChannel is a channel with resolved color hex and name
type ResolvedChannel struct {
	Ch    int    `json:"ch"`
	Color string `json:"color"` // Hex color
	Name  string `json:"name"`
	Value uint8  `json:"value"`
}

// ResolvedLight is a light with all channels resolved
type ResolvedLight struct {
	Group    string            `json:"group"`
	Name     string            `json:"name"`
	Channels []ResolvedChannel `json:"channels"`
}

// ColorPalette maps color names to hex values
var ColorPalette = map[string]string{
	// Horticulture spectrum
	"uv":       "#7F00FF",
	"blue":     "#0047AB",
	"cyan":     "#00CED1",
	"green":    "#32CD32",
	"yellow":   "#FFD700",
	"red":      "#FF2400",
	"far_red":  "#8B0000",
	"ir":       "#300000",

	// White temperatures
	"warm":    "#FFE4B5",
	"white":   "#FFFAF0",
	"cool":    "#F0F8FF",

	// Stage basics
	"amber":   "#FFBF00",
	"magenta": "#FF00FF",
	"pink":    "#FF69B4",
}
