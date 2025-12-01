# RK3506 AMP Demo - DMX Gateway

Demonstrates **AMP (Asymmetric Multiprocessing)** on RK3506 (Luckfox Lyra dev board): Linux + real-time on a single SoC.

## The opportunity

Many embedded applications need **both**:
- **High-level capabilities**: networking, web UI, SSH, scripting, easy updates
- **Hard real-time**: high availability, precise timing, deterministic runtime

Traditional approaches force a choice:
- **Linux only** → great ecosystem, but no timing guarantees (even `PREEMPT_RT` has limits)
- **Bare-metal/RTOS only** → precise timing, but painful networking, no modern tooling
- **External MCU** → extra hardware, communication overhead, two firmware & toolchains to maintain

**AMP lets us have both**: dedicate one core to real-time stuff while Linux handles the rest.

## Why DMX as a demo?

A good proof-of-concept for AMP: real timing constraints on one side, easy interfacing on the other. With a single SoC, we handle both signal generation (UART with µs-level timing) and high-level APIs.

Probably overkill in real life (a standalone MCU with Ethernet could handle this easily), but it's easy to understand, quick to implement, and gives visible results.

## Why this matters

**Linux for comfort** (CPU0 + CPU1):
- First-class networking: full TCP/IP stack, WiFi/Ethernet, VPN, routing, etc.
- USB that actually works: host/gadget, hubs, composite devices
- Modern languages fully supported: Go, Python, Rust...
- Easy device management: SSH, Ansible, git

**Coprocessor for precision** (CPU2 or MCU):
- Guaranteed timing, no jitter
- Direct hardware access
- Lightweight, application-specific code

**Applicable elsewhere**: servo drives, process control, fieldbus protocols, fast ADC sampling...

See [firmware-ap/README.md](firmware-ap/) for DMX-specific timing details.

## Two AMP approaches

The RK3506 offers two options for running real-time code alongside Linux:

| | **Cortex-A7 (AP)** | **Cortex-M0+ (MCU)** |
|--|----------------------|----------------------|
| **Core specs** | 1.2GHz Cortex-A7 | 200MHz Cortex-M0+ |
| **Memory** | 1MB+ reserved DDR | 32KB SRAM total |
| **Environment** | RT-Thread (full) | Bare-metal + HAL |

Both have access to more or less the same peripherals: UART, I2C, SPI, PWM, Timer, SAR ADC & CAN

Running an RTOS on the MCU might be possible (provided it fits into the 32KB constraint) but it's not covered in this introduction.

### Recommended: AP (Application Processor) - CPU2

The RK3506 has 3 Cortex-A7 cores. We "steal" one from Linux to run RT-Thread (RTOS). With RPMSG, we can communicate between the OS and the spare processor via shared memory without wasting an UART, very fast (<100µs raw latency in each direction) and conveniently (it appears like a `tty` device in Linux).

```
┌─────────────────────────────────────────────────────────────┐
│                    RK3506 (3x Cortex-A7)                    │
├─────────────────────────────────┬───────────────────────────┤
│      Linux (CPU0 + CPU1)        │    RT-Thread (CPU2)       │
│                                 │                           │
│  ┌───────────────────────┐      │                           │
│  │      DMX Client       │◄─────┼─── RPMSG ────┐            │
│  │   (subprocess call)   │      │  (shared     │            │
│  └───────────▲───────────┘      │   memory)    │            │
│              │                  │              │            │
│  ┌───────────┴───────────┐      │     ┌────────▼────────┐   │
│  │     DMX Gateway       │      │     │   DMX Driver    │   │
│  │  HTTP/WS/Modbus/MQTT  │      │     │  UART + timing  │   │
│  └───────────────────────┘      │     └────────┬────────┘   │
└─────────────────────────────────┴──────────────│────────────┘
                                                 │
                                                 ▼
                                           DMX512 Output
                                            (UART3_TX)
```

**This is the "clean" approach**: works reliably, documented, easy to debug, RT-Thread handles threading/timers/messaging. The tradeoff is losing one powerful A7 core for a simple 44Hz loop.

See [firmware-ap/README.md](firmware-ap/) for implementation details.

### Alternative: Cortex-M0+ (MCU)

The RK3506 also has an integrated **Cortex-M0+ microcontroller**. Tempting to use, but comes with significant challenges:

- **Zero Rockchip documentation** - figured out from SDK code + reverse engineering
- **Only 32KB SRAM** for code + data (no flash!) - tight fit for anything beyond simple routines
- **Hot reload broken** - MCU doesn't survive Linux `reboot`, requires physical power cycle (unknown root cause)
- **DTS dependency** - peripherals used by MCU must be declared in DTS to prevent Linux from disabling their clocks (defeats part of the "separate processor" benefits)

The approach is the same: the bootloader loads MCU code in its dedicated SRAM at startup just before launching the Linux kernel, and then we can communicate with it from the OS using RPMSG to send commands or fetch data just by reading/writing to `tty`-like device.

**When MCU makes sense:**
- You need all 3 A7 cores for heavy Linux workloads
- Your hardware has a **physical switch** on power supply or the SoC Enable pin (power cycle = no problem)
- 32KB is enough for the whole code and RAM of your application (bare-metal or lightweight RTOS)

See [firmware-mcu/README.md](firmware-mcu/) for the (painful) implementation journey.

### Credits

The MCU approach was greatly helped by [nvitya's rk3506-mcu repo](https://github.com/nvitya/rk3506-mcu) - the only public resource on RK3506 MCU.

## Project structure

```
rk3506-amp-dmx/
├── firmware-ap/   # RT-Thread code (CPU2) - recommended approach
├── firmware-mcu/  # Bare-metal code (M0+ MCU) - experimental
├── dmx-client/    # Linux C client - RPMSG bridge util
├── dmx-gateway/   # Go server - HTTP/WS/Modbus/MQTT (binary: dmx-gw)
├── build-docker/  # Docker environment for SDK build
└── README.md      # This file
```

| Component | Language | Role |
|-----------|----------|------|
| [firmware-ap/](firmware-ap/) | C (RT-Thread) | DMX512 on CPU2 - stable, recommended |
| [firmware-mcu/](firmware-mcu/) | C (Rockchip HAL) | DMX512 on M0+ MCU - experimental |
| [dmx-client/](dmx-client/) | C (Linux) | CLI for DMX commands via RPMSG |
| [dmx-gateway/](dmx-gateway/) | Go | Multi-protocol API, Web UI, scheduler |
| [build-docker/](build-docker/) | Docker | SDK build environment (Ubuntu 22.04) |

## Hardware

**Dev Board**: Luckfox Lyra (RK3506G2)
- 3x Cortex-A7 @ 1.2GHz
- 256MB NAND Flash
- 128MB on-chip RAM

**Transceiver**: MAX485 or equivalent (UART<>RS485)

## Quick Start (recommended approach with AP: spare A7 core)

### 1. Build & Flash Firmware

```bash
# Set SDK path (once per session)
export SDK_DIR=/path/to/luckfox-lyra-sdk

# Copy RT-Thread code to SDK (see firmware-ap/README.md)
cp firmware-ap/*.c firmware-ap/*.h $SDK_DIR/rtos/bsp/rockchip/rk3506-32/applications/

# Configure RT-Thread (enable RPMSG + UART3)
cd $SDK_DIR/rtos/bsp/rockchip/rk3506-32 && scons --menuconfig && cd -

# Build full SDK
./build.sh rk3506g_buildroot_spinand_amp_defconfig
./build.sh

# Flash (board in maskrom mode)
sudo ./rkflash.sh update
```

See [firmware-ap/README.md](firmware-ap/) for RT-Thread configuration details and optional `kernel-config` / `buildroot-config` customization.

For the MCU approach, see [firmware-mcu/README.md](firmware-mcu/) - requires different SDK config and DTS modifications.

### 2. Deploy Linux Applications

```bash
# Set SDK path (if not done previously)
export SDK_DIR=/path/to/luckfox-lyra-sdk

# Build clients (see dmx-client/ and dmx-gateway/ READMEs)
cd dmx-client && make && cd -
cd dmx-gateway && make build-arm && cd -

# Copy to target (executables + GW config file)
scp dmx-client/dmx dmx-gateway/dmx-gw-arm root@<ip>:/usr/bin/
scp dmx-gateway/config.yaml root@<ip>:/etc/dmx-gw/

# Run
ssh root@<ip> "dmx-gw -config /etc/dmx-gw/config.yaml &"
```

### 3. Test

```bash
ssh root@<ip>
dmx enable
dmx set 1 255
dmx timing                # Show frame rate, break, mab
dmx timing 30             # Lower frame rate for picky receivers
curl localhost:8080/api/status
```
