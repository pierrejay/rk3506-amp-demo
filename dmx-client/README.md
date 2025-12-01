# DMX Client

Linux CLI client for DMX512 control via RPMSG.

Communicates with the RT-Thread (AP) or bare-metal (MCU) DMX driver running on a dedicated core.

## Build

```bash
# Set SDK path (once per session)
export SDK_DIR=/path/to/luckfox-lyra-sdk

# Build
make

# Install to SDK overlay (for Buildroot inclusion)
make install
```

## Usage

```bash
dmx enable                    # Start DMX transmission
dmx disable                   # Stop
dmx set 1 255                 # Channel 1 = 255
dmx set 1 255,128,64          # Channels 1-3
dmx status                    # Get status (fps, frame count)
dmx blackout                  # All channels to 0
dmx timing                    # Get timing config
dmx timing 30                 # Set frame rate to 30 Hz
dmx timing 30 150 12          # Set fps=30Hz, break=150µs, mab=12µs
dmx timing 0 200 0            # Set break=200µs only (0=unchanged)
```

**Flags:**
```bash
dmx -d /dev/ttyRPMSG1 status  # Use MCU endpoint (default: /dev/ttyRPMSG0)
dmx enable --json             # JSON output for scripts
dmx enable --quiet            # Exit code only
```

## Protocol

Binary protocol over RPMSG (default `/dev/ttyRPMSG0`, overridable with `-d` arg):

| Byte | Field | Description |
|------|-------|-------------|
| 0 | Magic | 0xAA (cmd) / 0xBB (resp) |
| 1 | Cmd/Status | Command type or status code |
| 2-3 | Length | Payload length (LE) |
| 4..N | Data | Payload |
| N+1 | Checksum | XOR of bytes 0..N |

See `dmx_protocol.h` for command definitions.

The client uses `select()` with a 1-second timeout to avoid blocking indefinitely if the firmware doesn't respond.

## Performance

End-to-end latency from userspace (subprocess call → RPMSG → AP/MCU → response):

- With AP (RT-Thread): ~400 µs
- With MCU (single thread): ~250 µs

=> Much faster than UART or USB CDC at full speed!
