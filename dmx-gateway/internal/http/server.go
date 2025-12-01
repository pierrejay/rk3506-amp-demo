// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package http

import (
	"context"
	"embed"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net/http"
	"os"
	"runtime"
	"strings"
	"time"

	"github.com/gorilla/websocket"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	"dmx-gateway/internal/api"
	"dmx-gateway/internal/config"
	"dmx-gateway/internal/dmx"
	"dmx-gateway/internal/scheduler"
)

var startTime = time.Now()

//go:embed static/*
var staticFiles embed.FS

// Server is the HTTP/WebSocket server
type Server struct {
	cfg       *config.Config
	state     *dmx.State
	api       *api.Handler
	scheduler *scheduler.Scheduler
	logger    *slog.Logger
	server    *http.Server
	upgrader  websocket.Upgrader
}

// NewServer creates a new HTTP server
func NewServer(cfg *config.Config, state *dmx.State, logger *slog.Logger) *Server {
	s := &Server{
		cfg:    cfg,
		state:  state,
		api:    api.NewHandler(state),
		logger: logger,
		upgrader: websocket.Upgrader{
			CheckOrigin: func(r *http.Request) bool { return true },
		},
	}

	mux := http.NewServeMux()

	// WebSocket endpoint
	mux.HandleFunc("/ws", s.handleWebSocket)

	// Unified API endpoint (JSON POST)
	mux.HandleFunc("/api", s.handleAPI)

	// Legacy REST API (kept for compatibility)
	mux.HandleFunc("/api/status", s.handleStatus)
	mux.HandleFunc("/api/enable", s.handleEnable)
	mux.HandleFunc("/api/disable", s.handleDisable)
	mux.HandleFunc("/api/blackout", s.handleBlackout)
	mux.HandleFunc("/api/lights", s.handleLights)
	mux.HandleFunc("/api/lights/", s.handleLight)
	mux.HandleFunc("/api/groups", s.handleGroups)
	mux.HandleFunc("/api/groups/", s.handleGroup)
	mux.HandleFunc("/api/schedule", s.handleSchedule)
	mux.HandleFunc("/api/schedule/next", s.handleScheduleNext)
	mux.HandleFunc("/api/health", s.handleHealth)

	// Prometheus metrics
	mux.Handle("/metrics", promhttp.Handler())

	// Static files
	staticFS, _ := fs.Sub(staticFiles, "static")
	mux.Handle("/", http.FileServer(http.FS(staticFS)))

	s.server = &http.Server{
		Addr:    cfg.Server.HTTP,
		Handler: mux,
	}

	return s
}

// Start starts the HTTP server
func (s *Server) Start() error {
	s.logger.Info("Starting HTTP server", "addr", s.cfg.Server.HTTP)
	go func() {
		if err := s.server.ListenAndServe(); err != http.ErrServerClosed {
			s.logger.Error("HTTP server error", "error", err)
		}
	}()
	return nil
}

// Shutdown gracefully shuts down the server
func (s *Server) Shutdown(ctx context.Context) error {
	return s.server.Shutdown(ctx)
}

// handleWebSocket handles WebSocket connections
func (s *Server) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		s.logger.Error("WebSocket upgrade failed", "error", err)
		return
	}
	defer conn.Close()

	s.logger.Debug("WebSocket client connected", "remote", r.RemoteAddr)

	// Subscribe to state updates
	updates := s.state.Subscribe()
	defer s.state.Unsubscribe(updates)

	// Channel for outgoing messages (serializes all writes to avoid concurrent write panic)
	outgoing := make(chan []byte, 100)
	done := make(chan struct{})

	// Send initial state via outgoing channel
	s.sendInitialStateAsync(outgoing)

	// Read from client
	go func() {
		defer close(done)
		for {
			_, message, err := conn.ReadMessage()
			if err != nil {
				if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
					s.logger.Debug("WebSocket read error", "error", err)
				}
				return
			}
			s.handleWSMessageAsync(message, outgoing)
		}
	}()

	// Write loop - all writes go through here
	for {
		select {
		case data := <-outgoing:
			if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
				s.logger.Debug("WebSocket write error", "error", err)
				return
			}
		case data, ok := <-updates:
			if !ok {
				return
			}
			// data is pre-marshaled JSON from broadcastState
			if err := conn.WriteMessage(websocket.TextMessage, data); err != nil {
				s.logger.Debug("WebSocket write error", "error", err)
				return
			}
		case <-done:
			return
		}
	}
}

// sendInitialState sends init message to new WebSocket client (deprecated, use Async)
func (s *Server) sendInitialState(conn *websocket.Conn) {
	s.sendJSON(conn, s.state.GetInitMessage())
}

func (s *Server) sendJSON(conn *websocket.Conn, v interface{}) {
	data, _ := json.Marshal(v)
	conn.WriteMessage(websocket.TextMessage, data)
}

// sendInitialStateAsync sends init message (full config) to new client
func (s *Server) sendInitialStateAsync(outgoing chan<- []byte) {
	// Single init message with full config (sent once per connection)
	data, _ := json.Marshal(s.state.GetInitMessage())
	outgoing <- data
}

// handleWSMessageAsync handles incoming WebSocket message and sends response via outgoing channel
func (s *Server) handleWSMessageAsync(message []byte, outgoing chan<- []byte) {
	// Try unified API format first (has "cmd" field)
	var unified struct {
		Cmd string `json:"cmd"`
	}
	if err := json.Unmarshal(message, &unified); err == nil && unified.Cmd != "" {
		// Use unified API handler
		resp := s.api.HandleJSON(message)
		outgoing <- resp
		return
	}

	// Legacy format - no response needed (state changes broadcast via updates channel)
	var msg struct {
		Type    string                 `json:"type"`
		Key     string                 `json:"key,omitempty"`
		Group   string                 `json:"group,omitempty"`
		Channel int                    `json:"ch,omitempty"`
		Value   uint8                  `json:"value,omitempty"`
		Values  map[string]interface{} `json:"values,omitempty"`
	}

	if err := json.Unmarshal(message, &msg); err != nil {
		s.logger.Debug("Invalid WebSocket message", "error", err)
		return
	}

	switch msg.Type {
	case "enable":
		s.state.Enable()
	case "disable":
		s.state.Disable()
	case "blackout":
		s.state.Blackout()
	case "set_channel":
		s.state.SetChannel(msg.Channel, msg.Value)
	case "set_light":
		group, name := parseKey(msg.Key)
		if group != "" && name != "" {
			values := parseValues(msg.Values)
			s.state.SetLight(group, name, values)
		}
	case "set_group":
		values := parseValues(msg.Values)
		s.state.SetGroup(msg.Group, values)
	}
}

// handleAPI handles the unified JSON API endpoint
func (s *Server) handleAPI(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "Failed to read body", http.StatusBadRequest)
		return
	}

	resp := s.api.HandleJSON(body)
	w.Header().Set("Content-Type", "application/json")
	w.Write(resp)
}

// handleWSMessage handles an incoming WebSocket message
// Supports both legacy format and unified API format
func (s *Server) handleWSMessage(conn *websocket.Conn, message []byte) {
	// Try unified API format first (has "cmd" field)
	var unified struct {
		Cmd string `json:"cmd"`
	}
	if err := json.Unmarshal(message, &unified); err == nil && unified.Cmd != "" {
		// Use unified API handler
		resp := s.api.HandleJSON(message)
		conn.WriteMessage(websocket.TextMessage, resp)
		return
	}

	// Legacy format (has "type" field)
	var msg struct {
		Type    string                 `json:"type"`
		Key     string                 `json:"key,omitempty"`
		Group   string                 `json:"group,omitempty"`
		Channel int                    `json:"ch,omitempty"`
		Value   uint8                  `json:"value,omitempty"`
		Values  map[string]interface{} `json:"values,omitempty"`
	}

	if err := json.Unmarshal(message, &msg); err != nil {
		s.logger.Debug("Invalid WebSocket message", "error", err)
		return
	}

	switch msg.Type {
	case "enable":
		s.state.Enable()

	case "disable":
		s.state.Disable()

	case "blackout":
		s.state.Blackout()

	case "set_channel":
		s.state.SetChannel(msg.Channel, msg.Value)

	case "set_light":
		group, name := parseKey(msg.Key)
		if group != "" && name != "" {
			values := parseValues(msg.Values)
			s.state.SetLight(group, name, values)
		}

	case "set_group":
		values := parseValues(msg.Values)
		s.state.SetGroup(msg.Group, values)
	}
}

// parseKey splits "group/name" into parts
func parseKey(key string) (group, name string) {
	parts := strings.SplitN(key, "/", 2)
	if len(parts) == 2 {
		return parts[0], parts[1]
	}
	return "", ""
}

func parseValues(raw map[string]interface{}) map[string]uint8 {
	values := make(map[string]uint8)
	for k, v := range raw {
		switch val := v.(type) {
		case float64:
			values[k] = uint8(val)
		case int:
			values[k] = uint8(val)
		}
	}
	return values
}

// REST API Handlers

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	s.jsonResponse(w, s.state.GetStatus())
}

func (s *Server) handleEnable(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := s.state.Enable(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	s.jsonResponse(w, map[string]string{"status": "ok"})
}

func (s *Server) handleDisable(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := s.state.Disable(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	s.jsonResponse(w, map[string]string{"status": "ok"})
}

func (s *Server) handleBlackout(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := s.state.Blackout(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	s.jsonResponse(w, map[string]string{"status": "ok"})
}

func (s *Server) handleLights(w http.ResponseWriter, r *http.Request) {
	s.jsonResponse(w, s.state.GetLights())
}

func (s *Server) handleLight(w http.ResponseWriter, r *http.Request) {
	// Path: /api/lights/group/name
	path := strings.TrimPrefix(r.URL.Path, "/api/lights/")
	group, name := parseKey(path)
	if group == "" || name == "" {
		http.Error(w, "Invalid path, use /api/lights/group/name", http.StatusBadRequest)
		return
	}

	if r.Method == http.MethodPut {
		var body map[string]interface{}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		values := parseValues(body)
		if err := s.state.SetLight(group, name, values); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		s.jsonResponse(w, map[string]string{"status": "ok"})
	} else {
		light := s.state.GetLight(group, name)
		if light == nil {
			http.Error(w, "Light not found", http.StatusNotFound)
			return
		}
		s.jsonResponse(w, light)
	}
}

func (s *Server) handleGroups(w http.ResponseWriter, r *http.Request) {
	s.jsonResponse(w, s.state.GetGroups())
}

func (s *Server) handleGroup(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/api/groups/")
	if name == "" {
		http.Error(w, "Missing group name", http.StatusBadRequest)
		return
	}

	if r.Method == http.MethodPut {
		var body map[string]interface{}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		values := parseValues(body)
		if err := s.state.SetGroup(name, values); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		s.jsonResponse(w, map[string]string{"status": "ok"})
	} else {
		lights := s.cfg.GetGroupLights(name)
		if lights == nil {
			http.Error(w, "Group not found", http.StatusNotFound)
			return
		}
		s.jsonResponse(w, map[string]interface{}{
			"name":   name,
			"lights": lights,
		})
	}
}

func (s *Server) jsonResponse(w http.ResponseWriter, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

// Helper for tests
func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.server.Handler.ServeHTTP(w, r)
}

// Addr returns the server address
func (s *Server) Addr() string {
	return s.cfg.Server.HTTP
}

// SetScheduler sets the scheduler for API endpoints
func (s *Server) SetScheduler(sched *scheduler.Scheduler) {
	s.scheduler = sched
}

func (s *Server) handleSchedule(w http.ResponseWriter, r *http.Request) {
	if s.scheduler == nil {
		s.jsonResponse(w, map[string]interface{}{"events": []interface{}{}})
		return
	}
	s.jsonResponse(w, map[string]interface{}{"events": s.scheduler.Events()})
}

func (s *Server) handleScheduleNext(w http.ResponseWriter, r *http.Request) {
	if s.scheduler == nil {
		s.jsonResponse(w, nil)
		return
	}
	next := s.scheduler.NextEvent()
	if next != nil {
		next.InStr = next.In.String()
	}
	s.jsonResponse(w, next)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)

	// Read CPU load from /proc/loadavg (Linux only)
	var load1, load5, load15 float64
	if data, err := os.ReadFile("/proc/loadavg"); err == nil {
		fmt.Sscanf(string(data), "%f %f %f", &load1, &load5, &load15)
	}

	// Use typed struct (zero map allocation)
	health := dmx.HealthResponse{
		UptimeSec:   int(time.Since(startTime).Seconds()),
		UptimeStr:   time.Since(startTime).Round(time.Second).String(),
		Goroutines:  runtime.NumGoroutine(),
		CPULoad1m:   load1,
		CPULoad5m:   load5,
		CPULoad15m:  load15,
		MemAllocMB:  float64(m.Alloc) / 1024 / 1024,
		MemSysMB:    float64(m.Sys) / 1024 / 1024,
		MemHeapMB:   float64(m.HeapAlloc) / 1024 / 1024,
		GCRuns:      m.NumGC,
		GoVersion:   runtime.Version(),
		NumCPU:      runtime.NumCPU(),
	}

	s.jsonResponse(w, health)
}
