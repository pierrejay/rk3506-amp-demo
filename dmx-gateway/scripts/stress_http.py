#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""
DMX Gateway - HTTP API Stress Test

Tests the HTTP REST API and Unified JSON API with concurrent clients.
Measures latency, throughput, and error rates.

Usage:
    python3 stress_http.py --target 192.168.0.132:8080 --clients 10 --requests 100
"""

import argparse
import json
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from typing import List
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError

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


def http_request(url: str, method: str = "GET", data: bytes = None, timeout: float = 5.0) -> tuple:
    """Execute HTTP request, return (success, latency_ms)"""
    start = time.perf_counter()
    try:
        req = Request(url, data=data, method=method)
        req.add_header("Content-Type", "application/json")
        with urlopen(req, timeout=timeout) as resp:
            resp.read()
        latency = (time.perf_counter() - start) * 1000
        return True, latency
    except (URLError, HTTPError, TimeoutError) as e:
        latency = (time.perf_counter() - start) * 1000
        return False, latency


def run_client(client_id: int, target: str, requests: int, test_type: str) -> Stats:
    """Run a single client, return its stats"""
    stats = Stats()
    base_url = f"http://{target}"

    for i in range(requests):
        stats.requests += 1

        if test_type == "status":
            # GET /api/status
            ok, lat = http_request(f"{base_url}/api/status")
        elif test_type == "set":
            # POST /api with set command (rotating channel values)
            ch = (i % 12) + 1
            val = i % 256
            payload = json.dumps({"cmd": "set", "target": "rack1/level1", "values": {"blue": val}}).encode()
            ok, lat = http_request(f"{base_url}/api", method="POST", data=payload)
        elif test_type == "lights":
            # GET /api/lights
            ok, lat = http_request(f"{base_url}/api/lights")
        else:
            # Mixed workload
            if i % 3 == 0:
                ok, lat = http_request(f"{base_url}/api/status")
            elif i % 3 == 1:
                payload = json.dumps({"cmd": "set", "target": "rack1/level1", "values": {"blue": i % 256}}).encode()
                ok, lat = http_request(f"{base_url}/api", method="POST", data=payload)
            else:
                ok, lat = http_request(f"{base_url}/api/lights")

        if ok:
            stats.success += 1
            stats.add_latency(lat)
        else:
            stats.errors += 1

    return stats


def percentile(data: List[float], p: float) -> float:
    """Calculate percentile"""
    if not data:
        return 0
    k = (len(data) - 1) * p / 100
    f = int(k)
    c = f + 1 if f + 1 < len(data) else f
    return data[f] + (k - f) * (data[c] - data[f])


def print_report(stats: Stats, duration: float, clients: int, test_type: str):
    """Print formatted report"""
    print("\n" + "=" * 60)
    print(f"  HTTP Stress Test Results ({test_type})")
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
    parser = argparse.ArgumentParser(description="DMX Gateway HTTP Stress Test")
    parser.add_argument("--target", default="localhost:8080", help="Target host:port")
    parser.add_argument("--clients", type=int, default=10, help="Number of concurrent clients")
    parser.add_argument("--requests", type=int, default=100, help="Requests per client")
    parser.add_argument("--type", choices=["status", "set", "lights", "mixed"], default="mixed",
                        help="Test type: status, set, lights, or mixed")
    args = parser.parse_args()

    total_requests = args.clients * args.requests
    print(f"Starting HTTP Stress Test")
    print(f"Target:           {args.target}")
    print(f"Clients:          {args.clients}")
    print(f"Requests/Client:  {args.requests}")
    print(f"Total Requests:   {total_requests}")
    print(f"Test Type:        {args.type}")
    print("-" * 40)

    start_time = time.perf_counter()
    global_stats = Stats()

    with ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [
            executor.submit(run_client, i, args.target, args.requests, args.type)
            for i in range(args.clients)
        ]

        for future in as_completed(futures):
            client_stats = future.result()
            global_stats.merge(client_stats)

    duration = time.perf_counter() - start_time
    print_report(global_stats, duration, args.clients, args.type)


if __name__ == "__main__":
    main()
