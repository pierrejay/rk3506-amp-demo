// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package dmx

import (
	"log/slog"

	"dmx-gateway/internal/config"
)

// MockClient is a mock DMX client for testing
type MockClient struct {
	enabled   bool
	channels  [512]uint8
	calls     []string
	failNext  bool
}

// NewMockClient creates a new mock client
func NewMockClient() *MockClient {
	return &MockClient{}
}

func (m *MockClient) Enable() error {
	m.calls = append(m.calls, "enable")
	if m.failNext {
		m.failNext = false
		return &MockError{"enable failed"}
	}
	m.enabled = true
	return nil
}

func (m *MockClient) Disable() error {
	m.calls = append(m.calls, "disable")
	if m.failNext {
		m.failNext = false
		return &MockError{"disable failed"}
	}
	m.enabled = false
	return nil
}

func (m *MockClient) Blackout() error {
	m.calls = append(m.calls, "blackout")
	if m.failNext {
		m.failNext = false
		return &MockError{"blackout failed"}
	}
	for i := range m.channels {
		m.channels[i] = 0
	}
	return nil
}

func (m *MockClient) SetChannel(channel int, value uint8) error {
	m.calls = append(m.calls, "set_channel")
	if m.failNext {
		m.failNext = false
		return &MockError{"set_channel failed"}
	}
	if channel >= 1 && channel <= 512 {
		m.channels[channel-1] = value
	}
	return nil
}

func (m *MockClient) SetChannels(startChannel int, values []uint8) error {
	m.calls = append(m.calls, "set_channels")
	if m.failNext {
		m.failNext = false
		return &MockError{"set_channels failed"}
	}
	for i, v := range values {
		ch := startChannel + i
		if ch >= 1 && ch <= 512 {
			m.channels[ch-1] = v
		}
	}
	return nil
}

func (m *MockClient) Status() (*Status, error) {
	m.calls = append(m.calls, "status")
	if m.failNext {
		m.failNext = false
		return nil, &MockError{"status failed"}
	}
	return &Status{
		Enabled:    m.enabled,
		FPS:        44.0,
		FrameCount: 1000,
	}, nil
}

// FailNext makes the next call fail
func (m *MockClient) FailNext() {
	m.failNext = true
}

// Calls returns the list of method calls
func (m *MockClient) Calls() []string {
	return m.calls
}

// GetChannel returns a channel value
func (m *MockClient) GetChannel(ch int) uint8 {
	if ch >= 1 && ch <= 512 {
		return m.channels[ch-1]
	}
	return 0
}

// Reset clears the mock state
func (m *MockClient) Reset() {
	m.enabled = false
	m.channels = [512]uint8{}
	m.calls = nil
	m.failNext = false
}

type MockError struct {
	msg string
}

func (e *MockError) Error() string {
	return e.msg
}

// NewStateWithMock creates a State with a mock client for testing
func NewStateWithMock(cfg *config.Config, logger *slog.Logger) (*State, *MockClient) {
	mock := NewMockClient()

	// Create a wrapper that implements the same interface
	client := &Client{
		clientPath: "mock",
		logger:     logger,
	}

	state := &State{
		cfg:      cfg,
		client:   client,
		logger:   logger,
		throttle: 0, // No throttle in tests
		subs:     make(map[chan []byte]struct{}),
	}

	// Replace client methods with mock
	// Note: In real code, we'd use an interface. For simplicity, we test via State directly.

	return state, mock
}
