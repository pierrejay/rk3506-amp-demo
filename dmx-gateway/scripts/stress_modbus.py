#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""
DMX Gateway - Modbus TCP Stress Test (STANDALONE - no dependencies)

Uses raw sockets to implement Modbus TCP protocol.

Usage:
    python3 stress_modbus.py --target 192.168.0.132 --port 502 --clients 5 --requests 100
"""

import argparse
import socket
import struct
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import List

# Modbus Function Codes
FC_READ_HOLDING_REGISTERS = 0x03
FC_WRITE_SINGLE_REGISTER = 0x06
FC_READ_COILS = 0x01
FC_WRITE_SINGLE_COIL = 0x05


@dataclass
class Stats:
    requests: int = 0
    success: int = 0
    errors: int = 0
    latencies: List[float] = field(default_factory=list)
    lock: threading.Lock = field(default_factory=threading.Lock)

    def add_latency(self, ms: float):
        with self.lock:
            self.latencies.append(ms)

    def merge(self, other: 'Stats'):
        self.requests += other.requests
        self.success += other.success
        self.errors += other.errors
        self.latencies.extend(other.latencies)


class ModbusTCPClient:
    """Minimal Modbus TCP client using raw sockets"""

    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.transaction_id = 0

    def connect(self) -> bool:
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect((self.host, self.port))
            return True
        except Exception:
            return False

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _send_request(self, unit_id: int, function_code: int, data: bytes) -> bytes:
        """Send Modbus TCP request and return response data"""
        self.transaction_id = (self.transaction_id + 1) % 65536

        # Build MBAP header + PDU
        # MBAP: Transaction ID (2) + Protocol ID (2) + Length (2) + Unit ID (1)
        pdu = bytes([function_code]) + data
        length = len(pdu) + 1  # PDU + Unit ID

        request = struct.pack('>HHHB', self.transaction_id, 0, length, unit_id) + pdu

        self.sock.sendall(request)

        # Read response header (7 bytes MBAP)
        header = self._recv_exact(7)
        if not header:
            raise Exception("No response")

        resp_tid, proto_id, resp_len, resp_unit = struct.unpack('>HHHB', header)

        # Read response PDU
        pdu_data = self._recv_exact(resp_len - 1)
        if not pdu_data:
            raise Exception("Incomplete response")

        # Check for error response
        if pdu_data[0] & 0x80:
            raise Exception(f"Modbus error: {pdu_data[1]}")

        return pdu_data

    def _recv_exact(self, n: int) -> bytes:
        """Receive exactly n bytes"""
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def read_holding_registers(self, address: int, count: int = 1, unit_id: int = 1) -> List[int]:
        """Read holding registers (FC 0x03)"""
        data = struct.pack('>HH', address, count)
        response = self._send_request(unit_id, FC_READ_HOLDING_REGISTERS, data)

        # Parse response: FC (1) + Byte count (1) + Data (2*count)
        byte_count = response[1]
        values = []
        for i in range(count):
            val = struct.unpack('>H', response[2 + i*2:4 + i*2])[0]
            values.append(val)
        return values

    def write_register(self, address: int, value: int, unit_id: int = 1) -> bool:
        """Write single register (FC 0x06)"""
        data = struct.pack('>HH', address, value)
        response = self._send_request(unit_id, FC_WRITE_SINGLE_REGISTER, data)
        return True

    def read_coils(self, address: int, count: int = 1, unit_id: int = 1) -> List[bool]:
        """Read coils (FC 0x01)"""
        data = struct.pack('>HH', address, count)
        response = self._send_request(unit_id, FC_READ_COILS, data)

        # Parse response: FC (1) + Byte count (1) + Data
        byte_count = response[1]
        coils = []
        for i in range(count):
            byte_idx = i // 8
            bit_idx = i % 8
            coils.append(bool(response[2 + byte_idx] & (1 << bit_idx)))
        return coils

    def write_coil(self, address: int, value: bool, unit_id: int = 1) -> bool:
        """Write single coil (FC 0x05)"""
        coil_value = 0xFF00 if value else 0x0000
        data = struct.pack('>HH', address, coil_value)
        response = self._send_request(unit_id, FC_WRITE_SINGLE_COIL, data)
        return True


def run_client(client_id: int, host: str, port: int, requests: int, test_type: str) -> Stats:
    """Run a single Modbus client, return its stats"""
    stats = Stats()

    client = ModbusTCPClient(host, port)
    if not client.connect():
        stats.errors = requests
        stats.requests = requests
        return stats

    try:
        for i in range(requests):
            stats.requests += 1
            start = time.perf_counter()

            try:
                if test_type == "read":
                    addr = i % 512
                    client.read_holding_registers(addr, 1)
                elif test_type == "write":
                    addr = i % 512
                    value = i % 256
                    client.write_register(addr, value)
                elif test_type == "coil":
                    if i % 2 == 0:
                        client.read_coils(0, 1)
                    else:
                        client.write_coil(0, i % 2 == 1)
                else:  # mixed
                    if i % 3 == 0:
                        client.read_holding_registers(i % 512, 1)
                    elif i % 3 == 1:
                        client.write_register(i % 512, i % 256)
                    else:
                        client.read_coils(0, 2)

                latency = (time.perf_counter() - start) * 1000
                stats.success += 1
                stats.add_latency(latency)

            except Exception as e:
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
    print(f"  Modbus TCP Stress Test Results ({test_type})")
    print("=" * 60)
    print(f"Duration:         {duration:.2f}s")
    print(f"Clients:          {clients}")
    print(f"Total Requests:   {stats.requests}")
    print(f"Successful:       {stats.success}")
    print(f"Failed:           {stats.errors}")

    if stats.requests > 0:
        error_rate = (stats.errors / stats.requests) * 100
        print(f"Error Rate:       {error_rate:.2f}%")

    rps = stats.requests / duration if duration > 0 else 0
    print(f"Throughput:       {rps:.2f} req/sec")

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
    parser = argparse.ArgumentParser(description="DMX Gateway Modbus TCP Stress Test (standalone)")
    parser.add_argument("--target", default="localhost", help="Target host")
    parser.add_argument("--port", type=int, default=502, help="Modbus TCP port")
    parser.add_argument("--clients", type=int, default=5, help="Number of concurrent clients")
    parser.add_argument("--requests", type=int, default=100, help="Requests per client")
    parser.add_argument("--type", choices=["read", "write", "coil", "mixed"], default="mixed",
                        help="Test type")
    args = parser.parse_args()

    total_requests = args.clients * args.requests
    print(f"Starting Modbus TCP Stress Test (standalone)")
    print(f"Target:           {args.target}:{args.port}")
    print(f"Clients:          {args.clients}")
    print(f"Requests/Client:  {args.requests}")
    print(f"Total Requests:   {total_requests}")
    print(f"Test Type:        {args.type}")
    print("-" * 40)

    start_time = time.perf_counter()
    global_stats = Stats()

    with ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [
            executor.submit(run_client, i, args.target, args.port, args.requests, args.type)
            for i in range(args.clients)
        ]

        for future in as_completed(futures):
            client_stats = future.result()
            global_stats.merge(client_stats)

    duration = time.perf_counter() - start_time
    print_report(global_stats, duration, args.clients, args.type)


if __name__ == "__main__":
    main()
