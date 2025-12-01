// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

// Zero-allocation response types for DMX Gateway
// These typed structs replace map[string]interface{} to eliminate heap allocations

// StatusResponse is the typed response for status queries
type StatusResponse struct {
	Enabled    bool    `json:"enabled"`
	FPS        float64 `json:"fps,omitempty"`
	FrameCount uint64  `json:"frame_count,omitempty"`
}

// ChannelState represents a single channel's current state (pre-allocated)
type ChannelState struct {
	Ch    int    `json:"ch"`
	Color string `json:"color"`
	Name  string `json:"name"`
	Value uint8  `json:"value"`
}

// LightState represents a light's full state (pre-allocated at startup)
type LightState struct {
	Key      string            `json:"key"`
	Group    string            `json:"group"`
	Name     string            `json:"name"`
	Channels []ChannelState    `json:"channels"` // Pre-allocated slice
	Values   map[string]uint8  `json:"values"`   // Pre-allocated map
}

// LightUpdate is sent when a light changes (minimal allocation)
type LightUpdate struct {
	Key    string           `json:"key"`
	Group  string           `json:"group"`
	Name   string           `json:"name"`
	Values map[string]uint8 `json:"values"` // Only changed values
}

// ChannelUpdate is sent when a single channel changes
type ChannelUpdate struct {
	Ch    int   `json:"ch"`
	Value uint8 `json:"value"`
}

// WSInitMessage sent once on connection (full config)
type WSInitMessage struct {
	Type    string                 `json:"type"` // "init"
	Enabled bool                   `json:"enabled"`
	Groups  []string               `json:"groups"`
	Lights  map[string]*LightState `json:"lights"` // Full config with channels
}

// WSStateMessage sent on every state change (values only)
type WSStateMessage struct {
	Type    string                  `json:"type"` // "state"
	Enabled bool                    `json:"enabled"`
	Values  map[string]map[string]uint8 `json:"values"` // light key -> channel name -> value
}

// HealthResponse for /api/health endpoint (typed to avoid map allocation)
type HealthResponse struct {
	UptimeSec   int     `json:"uptime_sec"`
	UptimeStr   string  `json:"uptime_str"`
	Goroutines  int     `json:"goroutines"`
	CPULoad1m   float64 `json:"cpu_load_1m"`
	CPULoad5m   float64 `json:"cpu_load_5m"`
	CPULoad15m  float64 `json:"cpu_load_15m"`
	MemAllocMB  float64 `json:"mem_alloc_mb"`
	MemSysMB    float64 `json:"mem_sys_mb"`
	MemHeapMB   float64 `json:"mem_heap_mb"`
	GCRuns      uint32  `json:"gc_runs"`
	GoVersion   string  `json:"go_version"`
	NumCPU      int     `json:"num_cpu"`
}

// Pre-serialized responses (computed once at startup)
var (
	// These will be initialized by InitStaticResponses()
	ResponseOK       []byte
	ResponseEnabled  []byte
	ResponseDisabled []byte
	ResponseBlackout []byte
)
