// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package api

import (
	"encoding/json"
	"strings"

	"dmx-gateway/internal/dmx"
	"dmx-gateway/internal/metrics"
)

// Request is the unified JSON request format for all protocols
// Used by: HTTP POST /api, WebSocket, MQTT
type Request struct {
	Cmd    string           `json:"cmd"`              // enable, disable, blackout, set, get, status
	Target string           `json:"target,omitempty"` // "group" or "group/light"
	Values map[string]uint8 `json:"values,omitempty"` // channel values
}

// Response is the unified JSON response format
type Response struct {
	Type   string      `json:"type"`             // status, light, lights, groups, error, ok
	Target string      `json:"target,omitempty"` // echoes request target
	Data   interface{} `json:"data,omitempty"`
	Error  string      `json:"error,omitempty"`
}

// Handler processes unified API requests
type Handler struct {
	state *dmx.State
}

// NewHandler creates a new API handler
func NewHandler(state *dmx.State) *Handler {
	return &Handler{state: state}
}

// Handle processes a request and returns a response
func (h *Handler) Handle(req *Request) *Response {
	switch req.Cmd {
	case "enable":
		return h.handleEnable()
	case "disable":
		return h.handleDisable()
	case "blackout":
		return h.handleBlackout()
	case "set":
		return h.handleSet(req.Target, req.Values)
	case "get":
		return h.handleGet(req.Target)
	case "status":
		return h.handleStatus()
	case "lights":
		return h.handleLights()
	case "groups":
		return h.handleGroups()
	default:
		return &Response{Type: "error", Error: "unknown command: " + req.Cmd}
	}
}

// HandleJSON parses JSON and returns JSON response
func (h *Handler) HandleJSON(data []byte) []byte {
	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		resp := &Response{Type: "error", Error: "invalid JSON: " + err.Error()}
		out, _ := json.Marshal(resp)
		return out
	}
	resp := h.Handle(&req)
	out, _ := json.Marshal(resp)
	return out
}

// Pre-allocated response data (avoid allocations for common responses)
var (
	dataEnabled  = dmx.StatusResponse{Enabled: true}
	dataDisabled = dmx.StatusResponse{Enabled: false}
)

func (h *Handler) handleEnable() *Response {
	if err := h.state.Enable(); err != nil {
		metrics.ErrorsTotal.WithLabelValues("enable").Inc()
		return &Response{Type: "error", Error: err.Error()}
	}
	metrics.SetEnabled(true)
	metrics.CommandsTotal.WithLabelValues("enable").Inc()
	return &Response{Type: "ok", Data: dataEnabled}
}

func (h *Handler) handleDisable() *Response {
	if err := h.state.Disable(); err != nil {
		metrics.ErrorsTotal.WithLabelValues("disable").Inc()
		return &Response{Type: "error", Error: err.Error()}
	}
	metrics.SetEnabled(false)
	metrics.CommandsTotal.WithLabelValues("disable").Inc()
	return &Response{Type: "ok", Data: dataDisabled}
}

func (h *Handler) handleBlackout() *Response {
	if err := h.state.Blackout(); err != nil {
		metrics.ErrorsTotal.WithLabelValues("blackout").Inc()
		return &Response{Type: "error", Error: err.Error()}
	}
	metrics.CommandsTotal.WithLabelValues("blackout").Inc()
	return &Response{Type: "ok"}
}

func (h *Handler) handleSet(target string, values map[string]uint8) *Response {
	if target == "" {
		return &Response{Type: "error", Error: "target required"}
	}
	if len(values) == 0 {
		return &Response{Type: "error", Error: "values required"}
	}

	group, light := parseTarget(target)

	var err error
	if light == "" {
		// Set entire group
		err = h.state.SetGroup(group, values)
	} else {
		// Set specific light
		err = h.state.SetLight(group, light, values)
	}

	if err != nil {
		metrics.ErrorsTotal.WithLabelValues("set").Inc()
		return &Response{Type: "error", Target: target, Error: err.Error()}
	}

	metrics.CommandsTotal.WithLabelValues("set").Inc()

	// Update metrics for each channel
	h.updateChannelMetrics(target, values)

	return &Response{Type: "ok", Target: target}
}

func (h *Handler) handleGet(target string) *Response {
	if target == "" {
		// Return all lights (zero allocation - returns pre-allocated map)
		return &Response{Type: "lights", Data: h.state.GetLights()}
	}

	group, light := parseTarget(target)

	if light == "" {
		// Get all lights in group - build minimal response
		lights := h.state.GetConfig().GetGroupLights(group)
		if lights == nil {
			return &Response{Type: "error", Target: target, Error: "group not found"}
		}
		// Only allocate the result map (lights themselves are pre-allocated)
		result := make(map[string]*dmx.LightState, len(lights))
		for _, name := range lights {
			key := group + "/" + name
			result[key] = h.state.GetLight(group, name)
		}
		return &Response{Type: "lights", Target: target, Data: result}
	}

	// Get specific light (zero allocation - returns pre-allocated struct)
	data := h.state.GetLight(group, light)
	if data == nil {
		return &Response{Type: "error", Target: target, Error: "light not found"}
	}
	return &Response{Type: "light", Target: target, Data: data}
}

func (h *Handler) handleStatus() *Response {
	status := h.state.GetStatus()

	// Update FPS metric
	if status.FPS > 0 {
		metrics.FPS.Set(status.FPS)
	}

	return &Response{Type: "status", Data: status}
}

func (h *Handler) handleLights() *Response {
	// Zero allocation - returns reference to pre-allocated map
	return &Response{Type: "lights", Data: h.state.GetLights()}
}

func (h *Handler) handleGroups() *Response {
	// Zero allocation - returns pre-allocated slice
	return &Response{Type: "groups", Data: h.state.GetGroups()}
}

// parseTarget splits "group/light" or returns (group, "")
func parseTarget(target string) (group, light string) {
	parts := strings.SplitN(target, "/", 2)
	group = parts[0]
	if len(parts) == 2 {
		light = parts[1]
	}
	return
}

// updateChannelMetrics updates Prometheus metrics for channels
func (h *Handler) updateChannelMetrics(target string, values map[string]uint8) {
	group, light := parseTarget(target)

	if light == "" {
		// Group - update all lights
		for _, lightName := range h.state.GetConfig().GetGroupLights(group) {
			channels := h.state.GetConfig().GetLight(group, lightName)
			for _, ch := range channels {
				if val, ok := values[ch.Name]; ok {
					metrics.SetChannelValue(ch.Ch, group, lightName, ch.Name, val)
				}
			}
		}
	} else {
		// Single light
		channels := h.state.GetConfig().GetLight(group, light)
		for _, ch := range channels {
			if val, ok := values[ch.Name]; ok {
				metrics.SetChannelValue(ch.Ch, group, light, ch.Name, val)
			}
		}
	}
}
