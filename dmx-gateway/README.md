# DMX Gateway

Lightweight Go server for DMX512 control on Luckfox Lyra (RK3506 AMP).

- Protocols: HTTP REST, WebSocket, Modbus TCP, MQTT, Prometheus metrics
- Embedded Web UI
- Scheduling
- Single YAML config file

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Linux (CPU0+1)              │  RT-Thread (CPU2)    │
│                              │                      │
│  ┌─────────────┐             │                      │
│  │ dmx-gw      │             │                      │
│  │   (Go)      │             │                      │
│  └─────────────┘             │                      │
│        │ subprocess          │                      │
│  ┌─────▼───────┐             │  ┌────────────────┐  │
│  │ DMX Client  │◄────RPMSG───┼─▶│ DMX Driver     │  │
│  │    (C)      │             │  │ (RPMSG→UART)   │  │
│  └─────────────┘             │  └───────┬────────┘  │
└─────────────────────────────────────────┼───────────┘
                                          ▼
                                     DMX512 Out
```

**Runtime dependency**: `dmx` CLI (see `../dmx-client/`)

## Development (local machine)

Build and run locally with a mock DMX client for testing without hardware.

```bash
# Build for local platform
make build

# Run with mock DMX client (uses config.dev.yaml)
make run

# Or manually:
./dmx-gw -config config.dev.yaml -log-level DEBUG
```

`config.dev.yaml` points to `scripts/mock_dmx_client.sh` which simulates DMX responses.

### Unit tests

```bash
make test
```

### Manual API tests

```bash
# Status
curl localhost:8080/api/status
curl localhost:8080/api/health

# Commands
curl -X POST localhost:8080/api -d '{"cmd":"enable"}'
curl -X POST localhost:8080/api -d '{"cmd":"set","target":"rack1","values":{"blue":200}}'

# Modbus
python3 scripts/test_modbus_local.py
```

## Deployment on target

```bash
# Build for ARMv7
make build-arm

# Copy binary + config
scp dmx-gw-arm root@<ip>:/usr/bin/dmx-gw
scp config.yaml root@<ip>:/etc/dmx-gw/

# Start
ssh root@<ip> "dmx-gw -config /etc/dmx-gw/config.yaml &"
```

## Stress Testing

Standalone Python scripts in `scripts/` (no dependencies, raw sockets).

```bash
# HTTP
python3 scripts/stress_http.py --target <ip>:8080 --clients 5 --requests 100

# WebSocket
python3 scripts/stress_websocket.py --target <ip>:8080 --clients 5 --messages 100

# Modbus TCP
python3 scripts/stress_modbus.py --target <ip> --port 502 --clients 5 --requests 100

# All protocols at once
./scripts/stress_all.sh <ip> 8080 502
```

## Configuration

```yaml
server:
  http: ":8080"

dmx:
  client: "./dmx"        # Path to dmx CLI
  device: "/dev/ttyRPMSG1"  # RPMSG device (optional, defaults to /dev/ttyRPMSG0)
  throttle_ms: 25        # Min delay between DMX updates
  timeout_ms: 500        # Command timeout
  refresh_ms: 1000       # Status polling interval
  auto_enable: true      # Enable DMX output on startup (default: false)

# Modbus TCP (optional - presence enables it)
modbus:
  port: ":502"

# MQTT (optional - presence enables it)
mqtt:
  broker: "tcp://localhost:1883"
  topic_prefix: "dmx"

# Light definitions for UI, API & scheduler
lights:
  rack1:                        # Group (e.g. zone)
    level1:                     # Luminaire
      - { ch: 1, color: blue }  # Channels
      - { ch: 2, color: white }
      - ...
    ...
  ...

# Scheduler (optional)
schedule:
  timezone: "Europe/Paris"
  events:
    - { time: "08:00", set: { rack1: { blue: 200 } } }
    - { time: "22:00", blackout: true }
    - ...
```

## API Reference

### Unified JSON API

Same format for HTTP POST `/api`, WebSocket, and MQTT (`{prefix}/cmd`):

| Command | Example |
|---------|---------|
| Enable | `{"cmd": "enable"}` |
| Disable | `{"cmd": "disable"}` |
| Blackout | `{"cmd": "blackout"}` |
| Set group | `{"cmd": "set", "target": "rack1", "values": {"blue": 200}}` |
| Set light | `{"cmd": "set", "target": "rack1/level1", "values": {"blue": 100}}` |
| Get status | `{"cmd": "status"}` |
| Get light | `{"cmd": "get", "target": "rack1/level1"}` |

### HTTP Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI |
| `/ws` | GET | WebSocket |
| `/api` | POST | Unified JSON API |
| `/api/status` | GET | DMX status |
| `/api/enable` | POST | Enable output |
| `/api/disable` | POST | Disable output |
| `/api/blackout` | POST | All channels to 0 |
| `/api/lights` | GET | All lights state |
| `/api/lights/{group}/{name}` | GET/PUT | Single light |
| `/api/groups` | GET | List groups |
| `/api/groups/{name}` | GET/PUT | Group control |
| `/api/health` | GET | System health |
| `/api/schedule` | GET | Scheduled events |
| `/api/schedule/next` | GET | Next scheduled event |
| `/metrics` | GET | Prometheus metrics |

### Modbus TCP

| Type | Address | Description |
|------|---------|-------------|
| Holding Register | 0-511 | DMX channels 1-512 (value: 0-255) |
| Coil | 0 | Enable/disable (R/W) |
| Coil | 1 | Blackout (W only) |

### WebSocket & MQTT

Both use the **same unified JSON API** as HTTP POST `/api`.

| Protocol | Send commands | Receive updates |
|----------|---------------|-----------------|
| WebSocket | `ws://<host>:8080/ws` | Push on connect + state changes |
| MQTT | Publish to `{prefix}/cmd` | Subscribe to `{prefix}/event` |

**Push messages** (WebSocket + MQTT `{prefix}/event`):

| Type | Payload |
|------|---------|
| `init` | Full state on connect (WS only) |
| `status` | `{"type":"status", "data":{enabled, fps, frame_count}}` |
| `light` | `{"type":"light", "key":"rack1/level1", "values":{...}}` |
| `blackout` | `{"type":"blackout"}` |

**MQTT topics** (default prefix: `dmx`):

| Topic | Direction | Description |
|-------|-----------|-------------|
| `dmx/cmd` | Subscribe | Send commands |
| `dmx/response` | Publish | `{"type":"ok"}` or `{"type":"error",...}` |
| `dmx/event` | Publish | State changes (same as WS push) |
| `dmx/status` | Publish | Retained current status |

**Examples**:
```bash
# WebSocket
websocat ws://192.168.0.132:8080/ws
# → {"type":"init","status":{...},"lights":{...},"groups":[...]}

# MQTT
mosquitto_sub -h <broker> -t "dmx/#" -v
mosquitto_pub -h <broker> -t "dmx/cmd" -m '{"cmd":"enable"}'
```

## Web UI

Embedded single-page app served at `/`:

- Enable/Disable/Blackout controls
- Live connection status
- Lights organized by group (from config)
- Per-channel sliders with color indicators
- Real-time updates via WebSocket

## CLI Options

```bash
./dmx-gw -config config.yaml           # Run with config
./dmx-gw -config config.yaml -dry-run  # Validate config only
./dmx-gw -log-level DEBUG              # Verbose logging
```

## Benchmarks

Tested on Luckfox Lyra (RK3506, 2 Linux cores + 1 RTOS core, 128MB RAM) with stress tests scripts.

### Single client (baseline latency)

| Protocol | Throughput | Avg | P50 | P99 |
|----------|------------|-----|-----|-----|
| Modbus TCP | 264 req/s | 3.8ms | 2.2ms | 9.3ms |
| WebSocket | 170 msg/s | 4.5ms | 5.1ms | 11.3ms |
| HTTP | 110 req/s | 9.0ms | 9.3ms | 15.5ms |

### 5 concurrent clients

| Protocol | Throughput | Avg | P50 | P99 |
|----------|------------|-----|-----|-----|
| WebSocket | 698 msg/s | 5.7ms | 4.8ms | 16.4ms |
| Modbus TCP | 563 req/s | 8.6ms | 8.2ms | 21.4ms |
| HTTP | 270 req/s | 18.5ms | 19.9ms | 38.1ms |

### 15 clients (all protocols)

| Protocol | Throughput | Avg | P99 | Errors |
|----------|------------|-----|-----|--------|
| WebSocket | 549 msg/s | 7.1ms | 24.5ms | 0.16% |
| HTTP | 181 req/s | 27.6ms | 64.2ms | 0% |
| Modbus TCP | 157 req/s | 31.7ms | 144.6ms | 0% |

**Total: ~900 req/s** (80% CPU load on last test)

### Memory profile

| Metric | Value |
|--------|-------|
| Heap (active) | < 1 MB |
| System (Go runtime) | ~9 MB |
| RSS (total) | ~13 MB |
| Goroutines | 12 (stable) |

No heap in hot paths + pre-allocated data structures at startup => stable RAM & minimal GC pressure :
- ~20 runs after 7500 requests
- 1ms every 120s when idle (automatic scavenging)