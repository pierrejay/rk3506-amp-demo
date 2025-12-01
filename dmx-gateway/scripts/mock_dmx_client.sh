#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay

# Mock dmx_client for local testing
# Simulates the real dmx_client interface

STATE_FILE="/tmp/dmx_mock_state"
CHANNELS_FILE="/tmp/dmx_mock_channels"

# Initialize state if not exists
if [ ! -f "$STATE_FILE" ]; then
    echo "disabled" > "$STATE_FILE"
    echo "0" >> "$STATE_FILE"  # frame_count
fi

# Initialize channels (512 bytes, all zeros)
if [ ! -f "$CHANNELS_FILE" ]; then
    dd if=/dev/zero bs=1 count=512 2>/dev/null | od -An -tu1 -w512 | tr -s ' ' '\n' | head -512 > "$CHANNELS_FILE"
fi

read_state() {
    head -1 "$STATE_FILE"
}

read_frame_count() {
    tail -1 "$STATE_FILE"
}

write_state() {
    local state=$1
    local count=$(read_frame_count)
    echo "$state" > "$STATE_FILE"
    echo "$count" >> "$STATE_FILE"
}

inc_frame_count() {
    local state=$(read_state)
    local count=$(read_frame_count)
    count=$((count + 1))
    echo "$state" > "$STATE_FILE"
    echo "$count" >> "$STATE_FILE"
}

case "$1" in
    enable)
        write_state "enabled"
        echo "OK"
        ;;
    disable)
        write_state "disabled"
        echo "OK"
        ;;
    blackout)
        # Set all channels to 0
        for i in $(seq 1 512); do echo 0; done > "$CHANNELS_FILE"
        inc_frame_count
        echo "OK"
        ;;
    set)
        # set <channel> <value> or set <start> <v1,v2,v3,...>
        if [ -z "$2" ] || [ -z "$3" ]; then
            echo "Usage: set <channel> <value>" >&2
            exit 1
        fi
        channel=$2
        values=$3

        # Read current channels
        mapfile -t channels < "$CHANNELS_FILE"

        # Handle comma-separated values
        IFS=',' read -ra vals <<< "$values"
        for i in "${!vals[@]}"; do
            ch=$((channel + i - 1))  # 0-indexed array
            if [ $ch -ge 0 ] && [ $ch -lt 512 ]; then
                channels[$ch]=${vals[$i]}
            fi
        done

        # Write back
        printf '%s\n' "${channels[@]}" > "$CHANNELS_FILE"
        inc_frame_count
        echo "OK"
        ;;
    status|--json)
        # Handle both "status" and "--json status"
        if [ "$1" = "--json" ] && [ "$2" = "status" ]; then
            :
        elif [ "$1" = "status" ]; then
            :
        else
            echo "Unknown command" >&2
            exit 1
        fi

        state=$(read_state)
        count=$(read_frame_count)
        enabled="false"
        [ "$state" = "enabled" ] && enabled="true"

        # Simulate ~44 FPS
        echo "{\"enabled\":$enabled,\"frame_count\":$count,\"fps\":44.0}"
        ;;
    get)
        # get <channel> - for debugging
        if [ -z "$2" ]; then
            echo "Usage: get <channel>" >&2
            exit 1
        fi
        channel=$2
        idx=$((channel - 1))
        mapfile -t channels < "$CHANNELS_FILE"
        echo "${channels[$idx]:-0}"
        ;;
    dump)
        # dump - show all non-zero channels (for debugging)
        mapfile -t channels < "$CHANNELS_FILE"
        for i in "${!channels[@]}"; do
            val="${channels[$i]}"
            if [ "$val" != "0" ] && [ -n "$val" ]; then
                echo "CH$((i+1)): $val"
            fi
        done
        ;;
    reset)
        # reset - clear all state
        rm -f "$STATE_FILE" "$CHANNELS_FILE"
        echo "State cleared"
        ;;
    *)
        echo "Mock DMX Client - Commands: enable, disable, blackout, set, status, get, dump, reset" >&2
        exit 1
        ;;
esac
