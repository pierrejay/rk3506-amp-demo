#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""
DMX Animation Test - Remote via HTTP API
Tests that lights actually change by cycling through colors.
"""

import argparse
import json
import socket
import time

def http_post(host, port, path, data):
    """Simple HTTP POST without dependencies"""
    body = json.dumps(data)
    request = (
        f"POST {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
        f"{body}"
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((host, port))
    sock.sendall(request.encode())

    response = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    sock.close()

    # Parse response
    parts = response.split(b"\r\n\r\n", 1)
    if len(parts) > 1:
        return parts[1].decode()
    return ""

def set_light(host, port, target, values):
    """Set light values"""
    data = {"cmd": "set", "target": target, "values": values}
    return http_post(host, port, "/api", data)

def blackout(host, port):
    """Blackout all lights"""
    return http_post(host, port, "/api", {"cmd": "blackout"})

def enable(host, port):
    """Enable DMX output"""
    return http_post(host, port, "/api", {"cmd": "enable"})

def get_lights(host, port):
    """Get all lights"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((host, port))

    request = (
        f"GET /api/lights HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    )
    sock.sendall(request.encode())

    response = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    sock.close()

    parts = response.split(b"\r\n\r\n", 1)
    if len(parts) > 1:
        body = parts[1]
        # Handle chunked transfer encoding
        if b"Transfer-Encoding: chunked" in parts[0]:
            # Parse chunked body: size\r\ndata\r\n...0\r\n\r\n
            decoded = b""
            while body:
                size_end = body.find(b"\r\n")
                if size_end == -1:
                    break
                size = int(body[:size_end], 16)
                if size == 0:
                    break
                decoded += body[size_end+2:size_end+2+size]
                body = body[size_end+2+size+2:]
            return json.loads(decoded.decode())
        return json.loads(body.decode())
    return {}

def main():
    parser = argparse.ArgumentParser(description="DMX Remote Animation Test")
    parser.add_argument("--target", default="192.168.0.132:8080", help="Host:port")
    parser.add_argument("--light", default="rack1/level1", help="Light to animate")
    parser.add_argument("--speed", type=float, default=0.5, help="Seconds per step")
    parser.add_argument("--cycles", type=int, default=3, help="Number of cycles")
    args = parser.parse_args()

    host, port = args.target.split(":")
    port = int(port)

    print(f"DMX Remote Animation Test")
    print(f"Target: {host}:{port}")
    print(f"Light: {args.light}")
    print("-" * 40)

    # Enable DMX
    print("Enabling DMX...")
    enable(host, port)
    time.sleep(0.2)

    # Get available lights
    lights = get_lights(host, port)
    print(f"Available lights: {list(lights.keys())}")

    if args.light not in lights:
        print(f"WARNING: {args.light} not found, using first light")
        if lights:
            args.light = list(lights.keys())[0]
        else:
            print("No lights configured!")
            return

    # Get channels for this light
    light_info = lights[args.light]
    channels = [ch["name"] for ch in light_info["channels"]]
    print(f"Channels: {channels}")
    print("-" * 40)

    # Animation sequence
    sequences = [
        ("All OFF", {ch: 0 for ch in channels}),
        ("Blue 100%", {"blue": 255} if "blue" in channels else {channels[0]: 255}),
        ("Blue 50%", {"blue": 128} if "blue" in channels else {channels[0]: 128}),
        ("White 100%", {"white": 255} if "white" in channels else {}),
        ("Red 100%", {"red": 255} if "red" in channels else {}),
        ("All 50%", {ch: 128 for ch in channels}),
        ("All 100%", {ch: 255 for ch in channels}),
        ("All OFF", {ch: 0 for ch in channels}),
    ]

    try:
        for cycle in range(args.cycles):
            print(f"\n=== Cycle {cycle + 1}/{args.cycles} ===")
            for name, values in sequences:
                if not values:
                    continue
                print(f"  {name}: {values}")
                set_light(host, port, args.light, values)
                time.sleep(args.speed)

        print("\n=== Done ===")
        blackout(host, port)
        print("Blackout sent")

    except KeyboardInterrupt:
        print("\nInterrupted - sending blackout")
        blackout(host, port)

if __name__ == "__main__":
    main()
