// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package metrics

import (
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

var (
	// ChannelValue is a gauge for DMX channel values (0-255)
	ChannelValue = promauto.NewGaugeVec(
		prometheus.GaugeOpts{
			Name: "dmx_channel_value",
			Help: "Current DMX channel value (0-255)",
		},
		[]string{"channel", "group", "light", "color"},
	)

	// Enabled indicates if DMX output is enabled
	Enabled = promauto.NewGauge(
		prometheus.GaugeOpts{
			Name: "dmx_enabled",
			Help: "DMX output enabled (1) or disabled (0)",
		},
	)

	// FPS is the current frame rate
	FPS = promauto.NewGauge(
		prometheus.GaugeOpts{
			Name: "dmx_fps",
			Help: "DMX frames per second",
		},
	)

	// FrameCount is total frames sent
	FrameCount = promauto.NewCounter(
		prometheus.CounterOpts{
			Name: "dmx_frames_total",
			Help: "Total DMX frames sent",
		},
	)

	// CommandsTotal counts DMX commands by type
	CommandsTotal = promauto.NewCounterVec(
		prometheus.CounterOpts{
			Name: "dmx_commands_total",
			Help: "Total DMX commands by type",
		},
		[]string{"command"},
	)

	// ErrorsTotal counts errors by type
	ErrorsTotal = promauto.NewCounterVec(
		prometheus.CounterOpts{
			Name: "dmx_errors_total",
			Help: "Total errors by type",
		},
		[]string{"type"},
	)
)

// SetEnabled updates the enabled metric
func SetEnabled(enabled bool) {
	if enabled {
		Enabled.Set(1)
	} else {
		Enabled.Set(0)
	}
}

// SetChannelValue updates a channel value metric
func SetChannelValue(channel int, group, light, color string, value uint8) {
	ChannelValue.WithLabelValues(
		itoa(channel),
		group,
		light,
		color,
	).Set(float64(value))
}

// itoa is a simple int to string conversion
func itoa(i int) string {
	if i < 10 {
		return string(rune('0' + i))
	}
	return itoa(i/10) + string(rune('0'+i%10))
}
