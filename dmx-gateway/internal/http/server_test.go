// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025 Pierre Jay

package http

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"dmx-gateway/internal/config"
	"dmx-gateway/internal/dmx"
)

func testConfig() *config.Config {
	return &config.Config{
		Server: config.ServerConfig{HTTP: ":8080"},
		DMX:    config.DMXConfig{Client: "mock", ThrottleMs: 0, TimeoutMs: 100},
		Lights: map[string]map[string][]config.Channel{
			"rack1": {
				"level1": {
					{Ch: 1, Color: "blue"},
					{Ch: 2, Color: "red"},
				},
				"level2": {
					{Ch: 3, Color: "white"},
				},
			},
		},
	}
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelError}))
}

func setupServer(t *testing.T) *Server {
	cfg := testConfig()
	logger := testLogger()

	client, err := dmx.NewClient(config.DMXConfig{Client: "mock", TimeoutMs: 100}, logger)
	if err != nil {
		t.Fatalf("failed to create client: %v", err)
	}

	state := dmx.NewState(cfg, client, logger)
	return NewServer(cfg, state, logger)
}

func TestHandleStatus(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/status", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	var result map[string]interface{}
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
		t.Fatalf("failed to parse response: %v", err)
	}

	if _, ok := result["enabled"]; !ok {
		t.Error("response should contain 'enabled'")
	}
}

func TestHandleEnableMethodNotAllowed(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/enable", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("expected status 405, got %d", w.Code)
	}
}

func TestHandleLights(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/lights", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	var result map[string]interface{}
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
		t.Fatalf("failed to parse response: %v", err)
	}

	if len(result) != 2 {
		t.Errorf("expected 2 lights, got %d", len(result))
	}
}

func TestHandleLightGet(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/lights/rack1/level1", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	var result map[string]interface{}
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
		t.Fatalf("failed to parse response: %v", err)
	}

	if result["group"] != "rack1" {
		t.Errorf("expected group 'rack1', got %v", result["group"])
	}
	if result["name"] != "level1" {
		t.Errorf("expected name 'level1', got %v", result["name"])
	}
}

func TestHandleLightNotFound(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/lights/rack1/nonexistent", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Errorf("expected status 404, got %d", w.Code)
	}
}

func TestHandleLightPut(t *testing.T) {
	server := setupServer(t)

	body := `{"blue": 128, "red": 255}`
	req := httptest.NewRequest("PUT", "/api/lights/rack1/level1", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}
}

func TestHandleGroups(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/groups", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	var result []string
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
		t.Fatalf("failed to parse response: %v", err)
	}

	if len(result) != 1 {
		t.Errorf("expected 1 group, got %d", len(result))
	}
}

func TestHandleGroupGet(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/groups/rack1", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	var result map[string]interface{}
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil {
		t.Fatalf("failed to parse response: %v", err)
	}

	if result["name"] != "rack1" {
		t.Errorf("expected name 'rack1', got %v", result["name"])
	}
}

func TestHandleGroupNotFound(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/api/groups/nonexistent", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Errorf("expected status 404, got %d", w.Code)
	}
}

func TestHandleGroupPut(t *testing.T) {
	server := setupServer(t)

	body := `{"blue": 100, "red": 100, "white": 100}`
	req := httptest.NewRequest("PUT", "/api/groups/rack1", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}
}

func TestStaticFiles(t *testing.T) {
	server := setupServer(t)

	req := httptest.NewRequest("GET", "/", nil)
	w := httptest.NewRecorder()

	server.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("expected status 200, got %d", w.Code)
	}

	if !strings.Contains(w.Body.String(), "DMX Gateway") {
		t.Error("index.html should contain 'DMX Gateway'")
	}
}

func TestParseValues(t *testing.T) {
	raw := map[string]interface{}{
		"red":   float64(128),
		"green": float64(255),
		"blue":  float64(0),
	}

	values := parseValues(raw)

	if values["red"] != 128 {
		t.Errorf("expected red=128, got %d", values["red"])
	}
	if values["green"] != 255 {
		t.Errorf("expected green=255, got %d", values["green"])
	}
	if values["blue"] != 0 {
		t.Errorf("expected blue=0, got %d", values["blue"])
	}
}

func TestParseKey(t *testing.T) {
	group, name := parseKey("rack1/level1")
	if group != "rack1" || name != "level1" {
		t.Errorf("expected rack1/level1, got %s/%s", group, name)
	}

	group, name = parseKey("invalid")
	if group != "" || name != "" {
		t.Error("expected empty for invalid key")
	}
}
