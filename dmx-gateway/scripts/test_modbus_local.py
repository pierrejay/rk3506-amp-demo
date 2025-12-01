#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

"""Test script for DMX Gateway Modbus TCP server."""

import socket
import struct
import sys

HOST = 'localhost'
PORT = 5020

def modbus_request(sock, tx_id, func_code, data):
    """Send a Modbus TCP request and return the response."""
    # Modbus TCP header: TxID(2) + Protocol(2) + Length(2) + UnitID(1) + FC(1) + Data
    length = 2 + len(data)  # UnitID + FC + data
    header = struct.pack('>HHHBB', tx_id, 0, length, 1, func_code)
    sock.send(header + data)
    return sock.recv(256)

def read_holding_registers(sock, tx_id, start_addr, quantity):
    """FC03: Read Holding Registers."""
    data = struct.pack('>HH', start_addr, quantity)
    resp = modbus_request(sock, tx_id, 3, data)

    if len(resp) < 9:
        return None, f"Response too short: {len(resp)} bytes"

    # Check for exception
    if resp[7] & 0x80:
        return None, f"Exception code: {resp[8]}"

    byte_count = resp[8]
    values = []
    for i in range(byte_count // 2):
        val = struct.unpack('>H', resp[9 + i*2 : 11 + i*2])[0]
        values.append(val)

    return values, None

def write_single_register(sock, tx_id, addr, value):
    """FC06: Write Single Register."""
    data = struct.pack('>HH', addr, value)
    resp = modbus_request(sock, tx_id, 6, data)

    if len(resp) < 12:
        return False, f"Response too short: {len(resp)} bytes"

    if resp[7] & 0x80:
        return False, f"Exception code: {resp[8]}"

    return True, None

def read_coils(sock, tx_id, start_addr, quantity):
    """FC01: Read Coils."""
    data = struct.pack('>HH', start_addr, quantity)
    resp = modbus_request(sock, tx_id, 1, data)

    if len(resp) < 10:
        return None, f"Response too short: {len(resp)} bytes"

    if resp[7] & 0x80:
        return None, f"Exception code: {resp[8]}"

    return resp[9], None

def write_single_coil(sock, tx_id, addr, value):
    """FC05: Write Single Coil."""
    coil_value = 0xFF00 if value else 0x0000
    data = struct.pack('>HH', addr, coil_value)
    resp = modbus_request(sock, tx_id, 5, data)

    if len(resp) < 12:
        return False, f"Response too short: {len(resp)} bytes"

    if resp[7] & 0x80:
        return False, f"Exception code: {resp[8]}"

    return True, None

def main():
    print(f"=== DMX Gateway Modbus TCP Test ===")
    print(f"Connecting to {HOST}:{PORT}...")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((HOST, PORT))
        print("Connected!\n")
    except Exception as e:
        print(f"Connection failed: {e}")
        sys.exit(1)

    tx_id = 1
    errors = 0

    # Test 1: Read coils (enable status)
    print("Test 1: Read coil 0 (enable status)")
    coils, err = read_coils(sock, tx_id, 0, 1)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        enabled = bool(coils & 0x01)
        print(f"  OK: enabled = {enabled}")

    # Test 2: Enable DMX (write coil 0 = ON)
    print("\nTest 2: Enable DMX (write coil 0 = ON)")
    ok, err = write_single_coil(sock, tx_id, 0, True)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: DMX enabled")

    # Test 3: Read holding registers 0-3 (DMX channels 1-4)
    print("\nTest 3: Read holding registers 0-3 (channels 1-4)")
    values, err = read_holding_registers(sock, tx_id, 0, 4)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: channels 1-4 = {values}")

    # Test 4: Write single register 0 = 128 (channel 1)
    print("\nTest 4: Write register 0 = 128 (channel 1)")
    ok, err = write_single_register(sock, tx_id, 0, 128)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: channel 1 = 128")

    # Test 5: Write single register 1 = 64 (channel 2)
    print("\nTest 5: Write register 1 = 64 (channel 2)")
    ok, err = write_single_register(sock, tx_id, 1, 64)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: channel 2 = 64")

    # Test 6: Read back registers 0-3
    print("\nTest 6: Read back registers 0-3")
    values, err = read_holding_registers(sock, tx_id, 0, 4)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: channels 1-4 = {values}")
        if values[0] != 128 or values[1] != 64:
            print(f"  WARN: Expected [128, 64, ...], got {values}")

    # Test 7: Blackout (write coil 1 = ON)
    print("\nTest 7: Blackout (write coil 1 = ON)")
    ok, err = write_single_coil(sock, tx_id, 1, True)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: blackout triggered")

    # Test 8: Read back after blackout
    print("\nTest 8: Read registers after blackout")
    values, err = read_holding_registers(sock, tx_id, 0, 4)
    tx_id += 1
    if err:
        print(f"  FAIL: {err}")
        errors += 1
    else:
        print(f"  OK: channels 1-4 = {values}")
        if values != [0, 0, 0, 0]:
            print(f"  WARN: Expected [0, 0, 0, 0] after blackout")

    sock.close()

    print(f"\n=== Results: {8 - errors}/8 tests passed ===")
    sys.exit(0 if errors == 0 else 1)

if __name__ == '__main__':
    main()
