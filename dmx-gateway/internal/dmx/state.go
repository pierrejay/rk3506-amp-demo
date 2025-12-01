// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

import (
	"encoding/json"
	"log/slog"
	"sync"
	"time"

	"dmx-gateway/internal/config"
)

// State manages DMX channel state and coordinates updates
// Zero-allocation design: all data structures are pre-allocated at startup
type State struct {
	cfg      *config.Config
	client   *Client
	logger   *slog.Logger

	mu       sync.RWMutex
	channels [512]uint8 // Raw DMX channels (index 0 = DMX ch 1)
	enabled  bool
	throttle time.Duration

	// Pre-computed lights data (allocated ONCE at startup)
	// Key: "group/name", Value: pointer to pre-allocated LightState
	lights      map[string]*LightState
	lightKeys   []string // Ordered list of light keys for iteration
	groupNames  []string // Pre-computed group names

	// Channel to light mapping for fast updates
	// channelToLight[dmxCh-1] = list of (lightKey, channelIndex) pairs
	channelToLight [512][]channelMapping

	// Subscribers for state changes (WebSocket clients)
	// Channel sends pre-marshaled JSON []byte to avoid race conditions
	subsMu sync.RWMutex
	subs   map[chan []byte]struct{}

	// Pre-allocated values map for broadcasts (avoids alloc per broadcast)
	valuesCache map[string]map[string]uint8

	// Refresh goroutine
	stopRefresh chan struct{}
}

// channelMapping maps a DMX channel to a light's channel index
type channelMapping struct {
	lightKey     string
	channelIndex int
}

// StateUpdate is the single event type sent to subscribers
// Contains full state values (not config) for simplicity
type StateUpdate struct {
	Type    string                      `json:"type"` // always "state"
	Enabled bool                        `json:"enabled"`
	Values  map[string]map[string]uint8 `json:"values"` // light key -> channel name -> value
}

// NewState creates a new state manager with pre-allocated data structures
func NewState(cfg *config.Config, client *Client, logger *slog.Logger) *State {
	s := &State{
		cfg:      cfg,
		client:   client,
		logger:   logger,
		throttle: time.Duration(cfg.DMX.ThrottleMs) * time.Millisecond,
		subs:     make(map[chan []byte]struct{}),
		lights:   make(map[string]*LightState),
	}

	// Pre-compute all light structures (ONCE at startup - zero runtime allocation)
	s.buildLightsCache()

	return s
}

// buildLightsCache pre-allocates all light structures at startup
// This eliminates all allocations in GetLights/GetLight hot paths
func (s *State) buildLightsCache() {
	resolved := s.cfg.ResolveLights()

	// Pre-allocate light keys slice
	s.lightKeys = make([]string, 0, len(resolved))

	// Track groups for groupNames
	groupSet := make(map[string]struct{})

	for _, light := range resolved {
		key := config.LightKey(light.Group, light.Name)
		s.lightKeys = append(s.lightKeys, key)
		groupSet[light.Group] = struct{}{}

		// Pre-allocate LightState with all channels
		ls := &LightState{
			Key:      key,
			Group:    light.Group,
			Name:     light.Name,
			Channels: make([]ChannelState, len(light.Channels)),
			Values:   make(map[string]uint8, len(light.Channels)),
		}

		for i, ch := range light.Channels {
			ls.Channels[i] = ChannelState{
				Ch:    ch.Ch,
				Color: ch.Color,
				Name:  ch.Name,
				Value: 0, // Will be updated in-place
			}
			ls.Values[ch.Name] = 0

			// Build reverse mapping: DMX channel → light
			mapping := channelMapping{
				lightKey:     key,
				channelIndex: i,
			}
			s.channelToLight[ch.Ch-1] = append(s.channelToLight[ch.Ch-1], mapping)
		}

		s.lights[key] = ls
	}

	// Pre-allocate values cache (points to same maps as lights - zero copy)
	s.valuesCache = make(map[string]map[string]uint8, len(s.lights))
	for key, ls := range s.lights {
		s.valuesCache[key] = ls.Values
	}

	// Pre-compute group names (sorted would require import, keep insertion order)
	s.groupNames = make([]string, 0, len(groupSet))
	for g := range groupSet {
		s.groupNames = append(s.groupNames, g)
	}

	s.logger.Info("Lights cache built",
		"lights", len(s.lights),
		"groups", len(s.groupNames))
}

// Subscribe returns a channel that receives pre-marshaled JSON state updates
func (s *State) Subscribe() chan []byte {
	ch := make(chan []byte, 100)
	s.subsMu.Lock()
	s.subs[ch] = struct{}{}
	s.subsMu.Unlock()
	return ch
}

// Unsubscribe removes a subscriber
func (s *State) Unsubscribe(ch chan []byte) {
	s.subsMu.Lock()
	delete(s.subs, ch)
	close(ch)
	s.subsMu.Unlock()
}

// broadcastState sends current state to all subscribers
// Marshals JSON under lock to prevent race conditions
func (s *State) broadcastState() {
	s.subsMu.RLock()
	if len(s.subs) == 0 {
		s.subsMu.RUnlock()
		return
	}
	s.subsMu.RUnlock()

	// Marshal under state lock to prevent race with SetLight/SetChannel
	s.mu.RLock()
	data, _ := json.Marshal(StateUpdate{
		Type:    "state",
		Enabled: s.enabled,
		Values:  s.valuesCache,
	})
	s.mu.RUnlock()

	s.subsMu.RLock()
	defer s.subsMu.RUnlock()

	for ch := range s.subs {
		select {
		case ch <- data:
		default:
			// Channel full, skip
		}
	}
}

// Enable enables DMX output
func (s *State) Enable() error {
	if err := s.client.Enable(); err != nil {
		return err
	}
	s.mu.Lock()
	s.enabled = true
	s.mu.Unlock()

	s.broadcastState()
	return nil
}

// Disable disables DMX output
func (s *State) Disable() error {
	if err := s.client.Disable(); err != nil {
		return err
	}
	s.mu.Lock()
	s.enabled = false
	s.mu.Unlock()

	s.broadcastState()
	return nil
}

// Blackout sets all channels to 0
func (s *State) Blackout() error {
	if err := s.client.Blackout(); err != nil {
		return err
	}

	s.mu.Lock()
	// Zero all channels
	for i := range s.channels {
		s.channels[i] = 0
	}
	// Update all pre-allocated light values in-place
	for _, ls := range s.lights {
		for i := range ls.Channels {
			ls.Channels[i].Value = 0
		}
		for k := range ls.Values {
			ls.Values[k] = 0
		}
	}
	s.mu.Unlock()

	s.broadcastState()
	return nil
}

// SetChannel sets a single DMX channel (updates pre-allocated structures in-place)
func (s *State) SetChannel(channel int, value uint8) error {
	if channel < 1 || channel > 512 {
		return nil
	}

	s.mu.Lock()
	s.channels[channel-1] = value

	// Update pre-allocated light structures in-place (zero allocation)
	for _, mapping := range s.channelToLight[channel-1] {
		if ls, ok := s.lights[mapping.lightKey]; ok {
			ls.Channels[mapping.channelIndex].Value = value
			ls.Values[ls.Channels[mapping.channelIndex].Name] = value
		}
	}
	s.mu.Unlock()

	if err := s.client.SetChannel(channel, value); err != nil {
		return err
	}

	s.broadcastState()
	return nil
}

// SetLight sets a light's channel values by group/name
func (s *State) SetLight(group, name string, values map[string]uint8) error {
	key := config.LightKey(group, name)

	s.mu.Lock()
	ls, ok := s.lights[key]
	if !ok {
		s.mu.Unlock()
		return nil
	}

	// Update channels array and pre-allocated light structures in-place
	for i := range ls.Channels {
		ch := &ls.Channels[i]
		if val, exists := values[ch.Name]; exists {
			s.channels[ch.Ch-1] = val
			ch.Value = val
			ls.Values[ch.Name] = val
		}
	}
	s.mu.Unlock()

	// Send to DMX client
	channels := s.cfg.GetLight(group, name)
	for _, ch := range channels {
		if val, exists := values[ch.Name]; exists {
			if err := s.client.SetChannel(ch.Ch, val); err != nil {
				s.logger.Warn("Failed to set channel", "ch", ch.Ch, "error", err)
			}
		}
	}

	s.broadcastState()
	return nil
}

// SetGroup sets all lights in a group
func (s *State) SetGroup(groupName string, values map[string]uint8) error {
	lightNames := s.cfg.GetGroupLights(groupName)
	if lightNames == nil {
		return nil
	}

	for _, name := range lightNames {
		if err := s.SetLight(groupName, name, values); err != nil {
			s.logger.Warn("Failed to set light in group", "light", name, "error", err)
		}
	}
	return nil
}

// GetStatus returns current DMX status (typed struct, minimal allocation)
func (s *State) GetStatus() StatusResponse {
	s.mu.RLock()
	enabled := s.enabled
	s.mu.RUnlock()

	resp := StatusResponse{Enabled: enabled}

	if status, err := s.client.Status(); err == nil && status != nil {
		resp.FPS = status.FPS
		resp.FrameCount = status.FrameCount
	}

	return resp
}

// GetLights returns all lights (returns reference to pre-allocated map - ZERO allocation)
func (s *State) GetLights() map[string]*LightState {
	s.mu.RLock()
	defer s.mu.RUnlock()

	// Return direct reference to pre-allocated map
	// Values are already up-to-date (updated in-place by Set* methods)
	return s.lights
}

// GetLight returns a single light state (returns reference - ZERO allocation)
func (s *State) GetLight(group, name string) *LightState {
	key := config.LightKey(group, name)

	s.mu.RLock()
	defer s.mu.RUnlock()

	return s.lights[key] // May be nil if not found
}

// GetLightKeys returns ordered list of light keys (pre-allocated)
func (s *State) GetLightKeys() []string {
	return s.lightKeys
}

// GetChannels returns all 512 channel values
func (s *State) GetChannels() [512]uint8 {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.channels
}

// GetConfig returns the configuration
func (s *State) GetConfig() *config.Config {
	return s.cfg
}

// IsEnabled returns whether DMX is enabled
func (s *State) IsEnabled() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.enabled
}

// GetGroups returns all group names (pre-allocated slice)
func (s *State) GetGroups() []string {
	return s.groupNames
}

// GetInitMessage returns the full init message for new WebSocket clients
func (s *State) GetInitMessage() WSInitMessage {
	s.mu.RLock()
	defer s.mu.RUnlock()

	return WSInitMessage{
		Type:    "init",
		Enabled: s.enabled,
		Groups:  s.groupNames,
		Lights:  s.lights, // Reference to pre-allocated map
	}
}

// StartRefresh starts periodic refresh of DMX state (resync with hardware)
func (s *State) StartRefresh(interval time.Duration) {
	if interval <= 0 {
		return
	}

	s.stopRefresh = make(chan struct{})
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		s.logger.Info("DMX refresh started", "interval", interval)

		for {
			select {
			case <-ticker.C:
				s.refresh()
			case <-s.stopRefresh:
				s.logger.Info("DMX refresh stopped")
				return
			}
		}
	}()
}

// StopRefresh stops the periodic refresh
func (s *State) StopRefresh() {
	if s.stopRefresh != nil {
		close(s.stopRefresh)
		s.stopRefresh = nil
	}
}

// refresh resends all configured channels to DMX client and syncs WebSocket clients
func (s *State) refresh() {
	s.mu.RLock()
	enabled := s.enabled
	s.mu.RUnlock()

	// Always broadcast state to WebSocket clients (keeps UI in sync)
	s.broadcastState()

	// Only refresh hardware if enabled
	if !enabled {
		return
	}

	// Iterate pre-allocated lights (no allocation)
	s.mu.RLock()
	for _, ls := range s.lights {
		for _, ch := range ls.Channels {
			if err := s.client.SetChannel(ch.Ch, ch.Value); err != nil {
				s.logger.Warn("Refresh failed", "ch", ch.Ch, "error", err)
			}
		}
	}
	s.mu.RUnlock()

	s.logger.Debug("DMX state refreshed")
}
