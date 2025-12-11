#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""
DMX Animation Test - Via Modbus TCP
Tests that lights actually change by cycling through colors using Modbus registers.

Usage:
    python3 dmx_animate_modbus.py --target 192.168.0.249 --port 502 --cycles 3
"""

import argparse
import socket
import struct
import time

# Modbus Function Codes
FC_WRITE_SINGLE_REGISTER = 0x06
FC_WRITE_MULTIPLE_REGISTERS = 0x10
FC_WRITE_SINGLE_COIL = 0x05


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
        except Exception as e:
            print(f"Connection failed: {e}")
            return False

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _send_request(self, unit_id: int, function_code: int, data: bytes) -> bytes:
        """Send Modbus TCP request and return response data"""
        self.transaction_id = (self.transaction_id + 1) % 65536

        pdu = bytes([function_code]) + data
        length = len(pdu) + 1

        request = struct.pack('>HHHB', self.transaction_id, 0, length, unit_id) + pdu
        self.sock.sendall(request)

        header = self._recv_exact(7)
        if not header:
            raise Exception("No response")

        resp_tid, proto_id, resp_len, resp_unit = struct.unpack('>HHHB', header)

        pdu_data = self._recv_exact(resp_len - 1)
        if not pdu_data:
            raise Exception("Incomplete response")

        if pdu_data[0] & 0x80:
            raise Exception(f"Modbus error: {pdu_data[1]}")

        return pdu_data

    def _recv_exact(self, n: int) -> bytes:
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def write_register(self, address: int, value: int, unit_id: int = 1) -> bool:
        """Write single register (FC 0x06)"""
        data = struct.pack('>HH', address, value)
        self._send_request(unit_id, FC_WRITE_SINGLE_REGISTER, data)
        return True

    def write_registers(self, address: int, values: list, unit_id: int = 1) -> bool:
        """Write multiple registers (FC 0x10)"""
        count = len(values)
        byte_count = count * 2
        data = struct.pack('>HHB', address, count, byte_count)
        for v in values:
            data += struct.pack('>H', v)
        self._send_request(unit_id, FC_WRITE_MULTIPLE_REGISTERS, data)
        return True

    def write_coil(self, address: int, value: bool, unit_id: int = 1) -> bool:
        """Write single coil (FC 0x05)"""
        coil_value = 0xFF00 if value else 0x0000
        data = struct.pack('>HH', address, coil_value)
        self._send_request(unit_id, FC_WRITE_SINGLE_COIL, data)
        return True


def main():
    parser = argparse.ArgumentParser(description="DMX Animation via Modbus TCP")
    parser.add_argument("--target", default="192.168.0.249", help="Target host")
    parser.add_argument("--port", type=int, default=502, help="Modbus TCP port")
    parser.add_argument("--speed", type=float, default=0.5, help="Seconds per step")
    parser.add_argument("--cycles", type=int, default=3, help="Number of cycles")
    parser.add_argument("--channels", type=int, default=4, help="Number of DMX channels to use")
    args = parser.parse_args()

    print(f"DMX Animation via Modbus TCP")
    print(f"Target: {args.target}:{args.port}")
    print(f"Channels: 1-{args.channels}")
    print("-" * 40)

    client = ModbusTCPClient(args.target, args.port)
    if not client.connect():
        print("Failed to connect!")
        return

    # DMX channel mapping (holding registers):
    # Register 0 = DMX channel 1
    # Register 1 = DMX channel 2
    # etc.

    # Animation sequences: (name, channel_values)
    # Assuming 4-channel RGBW fixture: ch1=Red, ch2=Blue, ch3=White, ch4=FarRed
    sequences = [
        ("All OFF",      [0, 0, 0, 0]),
        ("Red 100%",     [255, 0, 0, 0]),
        ("Blue 100%",    [0, 255, 0, 0]),
        ("White 100%",   [0, 0, 255, 0]),
        ("Far Red 100%", [0, 0, 0, 255]),
        ("Red+Blue",     [255, 255, 0, 0]),
        ("All 50%",      [128, 128, 128, 128]),
        ("All 100%",     [255, 255, 255, 255]),
        ("Fade down",    [192, 192, 192, 192]),
        ("Fade down",    [128, 128, 128, 128]),
        ("Fade down",    [64, 64, 64, 64]),
        ("All OFF",      [0, 0, 0, 0]),
    ]

    # Enable DMX output (coil 0)
    print("Enabling DMX output...")
    try:
        client.write_coil(0, True)
    except Exception as e:
        print(f"Warning: Could not enable DMX via coil: {e}")

    try:
        for cycle in range(args.cycles):
            print(f"\n=== Cycle {cycle + 1}/{args.cycles} ===")
            for name, values in sequences:
                # Truncate or pad values to match channel count
                vals = values[:args.channels]
                while len(vals) < args.channels:
                    vals.append(0)

                print(f"  {name}: {vals}")
                try:
                    # Write all channels at once
                    client.write_registers(0, vals)
                except Exception as e:
                    print(f"    Error: {e}")

                time.sleep(args.speed)

        print("\n=== Done ===")
        # Blackout
        client.write_registers(0, [0] * args.channels)
        print("Blackout sent")

    except KeyboardInterrupt:
        print("\nInterrupted - sending blackout")
        try:
            client.write_registers(0, [0] * args.channels)
        except:
            pass

    finally:
        client.close()


if __name__ == "__main__":
    main()
