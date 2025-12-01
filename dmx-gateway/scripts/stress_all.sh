#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

# DMX Gateway - Run All Stress Tests
#
# Usage: ./stress_all.sh [target_ip] [port]
# Default: localhost:8080

TARGET="${1:-localhost}"
PORT="${2:-8080}"
MODBUS_PORT="${3:-5020}"

echo "=============================================="
echo "  DMX Gateway Stress Test Suite"
echo "=============================================="
echo "Target: $TARGET:$PORT"
echo "Modbus: $TARGET:$MODBUS_PORT"
echo ""

cd "$(dirname "$0")"

echo ""
echo "[1/3] HTTP API Stress Test"
echo "----------------------------------------------"
python3 stress_http.py --target "$TARGET:$PORT" --clients 10 --requests 100 --type mixed

echo ""
echo "[2/3] WebSocket Stress Test"
echo "----------------------------------------------"
python3 stress_websocket.py --target "$TARGET:$PORT" --clients 5 --messages 100 --type mixed

echo ""
echo "[3/3] Modbus TCP Stress Test"
echo "----------------------------------------------"
python3 stress_modbus.py --target "$TARGET" --port "$MODBUS_PORT" --clients 5 --requests 100 --type mixed

echo ""
echo "=============================================="
echo "  All Stress Tests Complete"
echo "=============================================="
