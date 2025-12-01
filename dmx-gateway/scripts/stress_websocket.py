#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""
DMX Gateway - WebSocket Stress Test (STANDALONE - no dependencies)

Uses raw sockets to implement WebSocket protocol.

Usage:
    python3 stress_websocket.py --target 192.168.0.132:8080 --clients 5 --messages 100
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import statistics
import struct
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import List, Optional

WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


@dataclass
class Stats:
    requests: int = 0
    success: int = 0
    errors: int = 0
    latencies: List[float] = field(default_factory=list)
    messages_received: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)

    def add_latency(self, ms: float):
        with self.lock:
            self.latencies.append(ms)

    def merge(self, other: 'Stats'):
        self.requests += other.requests
        self.success += other.success
        self.errors += other.errors
        self.latencies.extend(other.latencies)
        self.messages_received += other.messages_received


class WebSocketClient:
    """Minimal WebSocket client using raw sockets"""

    def __init__(self, host: str, port: int, path: str = "/ws", timeout: float = 10.0):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.sock = None

    def connect(self) -> bool:
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect((self.host, self.port))

            # WebSocket handshake
            key = base64.b64encode(os.urandom(16)).decode()
            request = (
                f"GET {self.path} HTTP/1.1\r\n"
                f"Host: {self.host}:{self.port}\r\n"
                f"Upgrade: websocket\r\n"
                f"Connection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n"
                f"\r\n"
            )
            self.sock.sendall(request.encode())

            # Read response headers
            response = b""
            while b"\r\n\r\n" not in response:
                chunk = self.sock.recv(1024)
                if not chunk:
                    return False
                response += chunk

            # Verify upgrade accepted
            if b"101" not in response.split(b"\r\n")[0]:
                return False

            return True
        except Exception as e:
            return False

    def close(self):
        if self.sock:
            try:
                # Send close frame
                self._send_frame(0x08, b"")
            except:
                pass
            self.sock.close()
            self.sock = None

    def send(self, message: str):
        """Send text message"""
        self._send_frame(0x01, message.encode())

    def recv(self, timeout: float = 5.0) -> Optional[str]:
        """Receive text message"""
        self.sock.settimeout(timeout)
        try:
            opcode, data = self._recv_frame()
            if opcode == 0x01:  # Text frame
                return data.decode()
            elif opcode == 0x08:  # Close frame
                return None
            return data.decode() if data else None
        except socket.timeout:
            return None
        except Exception:
            return None

    def _send_frame(self, opcode: int, data: bytes):
        """Send WebSocket frame (client must mask data)"""
        frame = bytearray()

        # FIN + opcode
        frame.append(0x80 | opcode)

        # Mask bit + length
        length = len(data)
        if length < 126:
            frame.append(0x80 | length)
        elif length < 65536:
            frame.append(0x80 | 126)
            frame.extend(struct.pack(">H", length))
        else:
            frame.append(0x80 | 127)
            frame.extend(struct.pack(">Q", length))

        # Masking key
        mask = os.urandom(4)
        frame.extend(mask)

        # Masked data
        masked_data = bytearray(len(data))
        for i, b in enumerate(data):
            masked_data[i] = b ^ mask[i % 4]
        frame.extend(masked_data)

        self.sock.sendall(frame)

    def _recv_frame(self) -> tuple:
        """Receive WebSocket frame"""
        # Read header
        header = self._recv_exact(2)
        if not header:
            raise Exception("Connection closed")

        fin = (header[0] & 0x80) != 0
        opcode = header[0] & 0x0F
        masked = (header[1] & 0x80) != 0
        length = header[1] & 0x7F

        if length == 126:
            length = struct.unpack(">H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._recv_exact(8))[0]

        if masked:
            mask = self._recv_exact(4)
            data = bytearray(self._recv_exact(length))
            for i in range(len(data)):
                data[i] ^= mask[i % 4]
        else:
            data = self._recv_exact(length)

        return opcode, bytes(data)

    def _recv_exact(self, n: int) -> bytes:
        """Receive exactly n bytes"""
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise Exception("Connection closed")
            data += chunk
        return data


def run_client(client_id: int, host: str, port: int, messages: int, test_type: str) -> Stats:
    """Run a single WebSocket client, return its stats"""
    stats = Stats()

    client = WebSocketClient(host, port)
    if not client.connect():
        stats.errors = messages
        stats.requests = messages
        return stats

    try:
        # Drain initial state messages (status + lights + groups)
        for _ in range(50):
            msg = client.recv(timeout=0.1)
            if msg:
                stats.messages_received += 1
            else:
                break

        for i in range(messages):
            stats.requests += 1
            start = time.perf_counter()

            try:
                if test_type == "status":
                    client.send(json.dumps({"cmd": "status"}))
                elif test_type == "set":
                    val = i % 256
                    client.send(json.dumps({
                        "cmd": "set",
                        "target": "rack1/level1",
                        "values": {"blue": val}
                    }))
                elif test_type == "get":
                    client.send(json.dumps({
                        "cmd": "get",
                        "target": "rack1/level1"
                    }))
                else:  # mixed
                    if i % 3 == 0:
                        client.send(json.dumps({"cmd": "status"}))
                    elif i % 3 == 1:
                        client.send(json.dumps({
                            "cmd": "set",
                            "target": "rack1/level1",
                            "values": {"blue": i % 256}
                        }))
                    else:
                        client.send(json.dumps({
                            "cmd": "get",
                            "target": "rack1/level1"
                        }))

                # Wait for response - with multiple clients we may get broadcasts too
                # Accept any response as success for latency measurement
                response = client.recv(timeout=2.0)
                latency = (time.perf_counter() - start) * 1000

                if response:
                    stats.success += 1
                    stats.messages_received += 1
                    stats.add_latency(latency)
                    # Drain any extra broadcast messages (only if multiple clients)
                    while True:
                        extra = client.recv(timeout=0.001)  # 1ms timeout
                        if extra:
                            stats.messages_received += 1
                        else:
                            break
                else:
                    stats.errors += 1

            except Exception:
                stats.errors += 1

    finally:
        client.close()

    return stats


def percentile(data: List[float], p: float) -> float:
    if not data:
        return 0
    k = (len(data) - 1) * p / 100
    f = int(k)
    c = f + 1 if f + 1 < len(data) else f
    return data[f] + (k - f) * (data[c] - data[f])


def print_report(stats: Stats, duration: float, clients: int, test_type: str):
    print("\n" + "=" * 60)
    print(f"  WebSocket Stress Test Results ({test_type})")
    print("=" * 60)
    print(f"Duration:         {duration:.2f}s")
    print(f"Clients:          {clients}")
    print(f"Total Messages:   {stats.requests}")
    print(f"Successful:       {stats.success}")
    print(f"Failed:           {stats.errors}")
    print(f"Total Received:   {stats.messages_received}")

    if stats.requests > 0:
        error_rate = (stats.errors / stats.requests) * 100
        print(f"Error Rate:       {error_rate:.2f}%")

    rps = stats.requests / duration if duration > 0 else 0
    print(f"Throughput:       {rps:.2f} msg/sec")

    if stats.latencies:
        stats.latencies.sort()
        print("\n  Latency Distribution (ms)")
        print("-" * 40)
        print(f"Min:    {min(stats.latencies):.2f}")
        print(f"Avg:    {statistics.mean(stats.latencies):.2f}")
        print(f"Max:    {max(stats.latencies):.2f}")
        print(f"P50:    {percentile(stats.latencies, 50):.2f}")
        print(f"P95:    {percentile(stats.latencies, 95):.2f}")
        print(f"P99:    {percentile(stats.latencies, 99):.2f}")
        if len(stats.latencies) > 1:
            print(f"StdDev: {statistics.stdev(stats.latencies):.2f}")

    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="DMX Gateway WebSocket Stress Test (standalone)")
    parser.add_argument("--target", default="localhost:8080", help="Target host:port")
    parser.add_argument("--clients", type=int, default=5, help="Number of concurrent clients")
    parser.add_argument("--messages", type=int, default=100, help="Messages per client")
    parser.add_argument("--type", choices=["status", "set", "get", "mixed"], default="mixed",
                        help="Test type")
    args = parser.parse_args()

    # Parse target
    if ":" in args.target:
        host, port = args.target.split(":")
        port = int(port)
    else:
        host = args.target
        port = 8080

    total_messages = args.clients * args.messages
    print(f"Starting WebSocket Stress Test (standalone)")
    print(f"Target:           ws://{host}:{port}/ws")
    print(f"Clients:          {args.clients}")
    print(f"Messages/Client:  {args.messages}")
    print(f"Total Messages:   {total_messages}")
    print(f"Test Type:        {args.type}")
    print("-" * 40)

    start_time = time.perf_counter()
    global_stats = Stats()

    with ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [
            executor.submit(run_client, i, host, port, args.messages, args.type)
            for i in range(args.clients)
        ]

        for future in as_completed(futures):
            client_stats = future.result()
            global_stats.merge(client_stats)

    duration = time.perf_counter() - start_time
    print_report(global_stats, duration, args.clients, args.type)


if __name__ == "__main__":
    main()
