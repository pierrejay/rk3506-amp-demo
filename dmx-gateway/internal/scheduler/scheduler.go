// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package scheduler

import (
	"log/slog"
	"sort"
	"strings"
	"sync"
	"time"

	"dmx-gateway/internal/config"
	"dmx-gateway/internal/dmx"
)

// Event is a parsed schedule event with time components
type Event struct {
	Hour     int
	Minute   int
	Second   int
	Set      map[string]map[string]uint8
	Blackout bool
}

// Scheduler runs scheduled lighting events
type Scheduler struct {
	events   []Event
	state    *dmx.State
	logger   *slog.Logger
	location *time.Location

	mu          sync.RWMutex
	lastRun     string // "HH:MM:SS" of last executed event
	stopChan    chan struct{}
	running     bool
}

// New creates a new scheduler
func New(cfg *config.ScheduleConfig, state *dmx.State, logger *slog.Logger) (*Scheduler, error) {
	loc := time.Local
	if cfg.Timezone != "" {
		var err error
		loc, err = time.LoadLocation(cfg.Timezone)
		if err != nil {
			return nil, err
		}
	}

	events := make([]Event, 0, len(cfg.Events))
	for _, e := range cfg.Events {
		parsed, err := parseTime(e.Time)
		if err != nil {
			logger.Warn("Invalid schedule time", "time", e.Time, "error", err)
			continue
		}
		parsed.Set = e.Set
		parsed.Blackout = e.Blackout
		events = append(events, parsed)
	}

	// Sort by time
	sort.Slice(events, func(i, j int) bool {
		return timeToSeconds(events[i]) < timeToSeconds(events[j])
	})

	return &Scheduler{
		events:   events,
		state:    state,
		logger:   logger,
		location: loc,
		stopChan: make(chan struct{}),
	}, nil
}

// Start begins the scheduler loop
func (s *Scheduler) Start() {
	s.mu.Lock()
	if s.running {
		s.mu.Unlock()
		return
	}
	s.running = true
	s.mu.Unlock()

	go s.loop()
	s.logger.Info("Scheduler started", "events", len(s.events), "timezone", s.location.String())
}

// Stop stops the scheduler
func (s *Scheduler) Stop() {
	s.mu.Lock()
	if !s.running {
		s.mu.Unlock()
		return
	}
	s.running = false
	s.mu.Unlock()

	close(s.stopChan)
	s.logger.Info("Scheduler stopped")
}

// loop checks every second for events to execute
func (s *Scheduler) loop() {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			s.check()
		case <-s.stopChan:
			return
		}
	}
}

// check executes any event matching current time
func (s *Scheduler) check() {
	now := time.Now().In(s.location)
	nowStr := now.Format("15:04:05")

	s.mu.Lock()
	if s.lastRun == nowStr {
		s.mu.Unlock()
		return
	}
	s.mu.Unlock()

	h, m, sec := now.Hour(), now.Minute(), now.Second()

	for _, e := range s.events {
		if e.Hour == h && e.Minute == m && e.Second == sec {
			s.execute(e)
			s.mu.Lock()
			s.lastRun = nowStr
			s.mu.Unlock()
			return
		}
	}
}

// execute runs a scheduled event
func (s *Scheduler) execute(e Event) {
	s.logger.Info("Executing scheduled event", "time", formatTime(e))

	if e.Blackout {
		if err := s.state.Blackout(); err != nil {
			s.logger.Error("Schedule blackout failed", "error", err)
		}
		return
	}

	for target, values := range e.Set {
		group, light := parseTarget(target)
		if light == "" {
			// Set entire group
			if err := s.state.SetGroup(group, values); err != nil {
				s.logger.Error("Schedule set group failed", "target", target, "error", err)
			}
		} else {
			// Set specific light
			if err := s.state.SetLight(group, light, values); err != nil {
				s.logger.Error("Schedule set light failed", "target", target, "error", err)
			}
		}
	}
}

// NextEvent returns the next scheduled event
func (s *Scheduler) NextEvent() *NextEventInfo {
	if len(s.events) == 0 {
		return nil
	}

	now := time.Now().In(s.location)
	nowSec := now.Hour()*3600 + now.Minute()*60 + now.Second()

	// Find next event today
	for _, e := range s.events {
		eSec := timeToSeconds(e)
		if eSec > nowSec {
			return &NextEventInfo{
				Time:     formatTime(e),
				In:       time.Duration(eSec-nowSec) * time.Second,
				Blackout: e.Blackout,
				Targets:  targetList(e.Set),
			}
		}
	}

	// Wrap to first event tomorrow
	if len(s.events) > 0 {
		e := s.events[0]
		eSec := timeToSeconds(e)
		secsUntil := (24*3600 - nowSec) + eSec
		return &NextEventInfo{
			Time:     formatTime(e),
			In:       time.Duration(secsUntil) * time.Second,
			Blackout: e.Blackout,
			Targets:  targetList(e.Set),
		}
	}

	return nil
}

// Events returns all scheduled events
func (s *Scheduler) Events() []EventInfo {
	result := make([]EventInfo, len(s.events))
	for i, e := range s.events {
		result[i] = EventInfo{
			Time:     formatTime(e),
			Blackout: e.Blackout,
			Targets:  targetList(e.Set),
		}
	}
	return result
}

// NextEventInfo describes the next scheduled event
type NextEventInfo struct {
	Time     string        `json:"time"`
	In       time.Duration `json:"in"`
	InStr    string        `json:"in_str"`
	Blackout bool          `json:"blackout"`
	Targets  []string      `json:"targets,omitempty"`
}

// EventInfo describes a scheduled event
type EventInfo struct {
	Time     string   `json:"time"`
	Blackout bool     `json:"blackout"`
	Targets  []string `json:"targets,omitempty"`
}

// Helper functions

func parseTime(s string) (Event, error) {
	t, err := time.Parse("15:04:05", s)
	if err != nil {
		// Try without seconds
		t, err = time.Parse("15:04", s)
		if err != nil {
			return Event{}, err
		}
	}
	return Event{
		Hour:   t.Hour(),
		Minute: t.Minute(),
		Second: t.Second(),
	}, nil
}

func formatTime(e Event) string {
	return time.Date(0, 1, 1, e.Hour, e.Minute, e.Second, 0, time.UTC).Format("15:04:05")
}

func timeToSeconds(e Event) int {
	return e.Hour*3600 + e.Minute*60 + e.Second
}

func parseTarget(target string) (group, light string) {
	parts := strings.SplitN(target, "/", 2)
	group = parts[0]
	if len(parts) == 2 {
		light = parts[1]
	}
	return
}

func targetList(set map[string]map[string]uint8) []string {
	targets := make([]string, 0, len(set))
	for t := range set {
		targets = append(targets, t)
	}
	sort.Strings(targets)
	return targets
}
