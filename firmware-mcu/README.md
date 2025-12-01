# DMX Firmware (MCU - Cortex-M0+)

Bare-metal real-time code for DMX512 generation on the RK3506's integrated Cortex-M0+ MCU.

**Status: EXPERIMENTAL** - Works, but requires physical power cycle after reboot. See [Limitations](#limitations).

> **Note: Dual-AMP configuration**
>
> This example runs **both AP (CPU2) and MCU simultaneously**, each with its own RPMSG channel:
> - AP (RT-Thread): `/dev/ttyRPMSG0` - uses `MBOX0`/`MBOX2`
> - MCU (bare-metal): `/dev/ttyRPMSG1` - uses `MBOX1`/`MBOX3`
>
> I built this on top of the existing Rockchip AMP defconfig/DTS for simplicity. If you only need the MCU (without CPU2), you'll need to adapt the DTS and defconfig to remove the AP components and have MCU use RPMSG0 instead.

## Why MCU?

For DMX timing requirements, see [firmware-ap/README.md](../firmware-ap/README.md#context-why-amp).

The MCU approach lets you **keep all 3 Cortex-A7 cores for Linux** while offloading real-time work to the dedicated M0+ microcontroller. Sounds great, but comes with challenges.

### MCU vs AP comparison

| Aspect | AP (CPU2) | MCU (M0+) |
|--------|-----------|-----------|
| Documentation | Exists | None |
| Memory | 1MB+ DDR | 32KB SRAM |
| RTOS | RT-Thread | Bare-metal (or lightweight RTOS if it fits) |
| Hot reload | Reboot works | Power cycle required |
| Debug | UART console + shell | UART (printf only) - no JTAG/SWD |
| Complexity | Low | Higher |

## Challenges overview

Before diving in, understand what you're signing up for:

1. **Bare-metal by default** - We use polling loops, but a lightweight RTOS could fit if needed
2. **No RPMSG library** - Unlike RT-Thread (AP), there's no ready-made RPMSG stack for M0+. I provide a custom implementation (~400 lines) based on mailbox registers and shared memory
3. **Same `dmx` client** - 100% compatible protocol, just use `-d /dev/ttyRPMSG1` for MCU
4. **DTS configuration required** - Peripherals used by MCU must be declared: change firmware = rebuild Buildroot image (partial build, but still bothersome)
5. **Hot reload doesn't work** - MCU doesn't survive Linux `reboot` (cause unknown), power cycle required
6. **HAL bloat** - Rockchip HAL can eat 25KB+ if not careful, I got the code size down to ~19KB

See [LESSONS_LEARNED.md](LESSONS_LEARNED.md) for the full journey.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    RK3506                                   │
├────────────────────────────────┬────────────────────────────┤
│   Linux (CPU0 + CPU1 + CPU2)   │    MCU (Cortex-M0+)        │
│                                │                            │
│  ┌──────────────────────┐      │  ┌──────────────────────┐  │
│  │     DMX Client       │◄─────┼─▶│      main.c          │  │
│  │   /dev/ttyRPMSG1     │ RPMSG│  │  - RPMSG RX loop     │  │
│  │   (or RPMSG0 if      │      │  │  - Protocol parser   │  │
│  │    MCU-only config)  │      │  │  - DMX TX loop       │  │
│  └──────────────────────┘      │  └──────────┬───────────┘  │
│                                │             │              │
│  ┌──────────────────────┐      │  ┌──────────▼───────────┐  │
│  │   rockchip-amp drv   │      │  │    dmx_driver.c      │  │
│  │   (prevents clk off) │      │  │   UART2 @ 250kbaud   │  │
│  └──────────────────────┘      │  │   Break via LCR reg  │  │
│                                │  └──────────┬───────────┘  │
└────────────────────────────────┴─────────────│──────────────┘
                                               ▼
                                         DMX512 Output
                                          (GPIO0_A6)
```

### RPMSG device assignment

| Configuration | CPU2 (AP) | MCU | Linux devices |
|---------------|-----------|-----|---------------|
| AP only | RPMSG0 | - | `/dev/ttyRPMSG0` |
| MCU only | - | RPMSG0 | `/dev/ttyRPMSG0` |
| AP + MCU | RPMSG0 | RPMSG1 | `/dev/ttyRPMSG0`, `/dev/ttyRPMSG1` |

The AP always starts first and takes RPMSG0. In dual-AMP mode, MCU gets RPMSG1.

### Memory

**SRAM (32KB total @ 0xFFF84000):**
- Code + RO data: ~19-21KB
- Stack + heap + BSS: remaining

**DDR (shared with Linux):**
- RPMSG buffers: 0x03C00000 - 0x03DFFFFF (2MB)
- RT-Thread reserved (if dual-AMP): 0x03E00000+

## Protocol

Same binary protocol as AP firmware - the `dmx` client works with both:

```bash
# For MCU (when both AP and MCU are enabled)
dmx -d /dev/ttyRPMSG1 status
dmx -d /dev/ttyRPMSG1 enable
dmx -d /dev/ttyRPMSG1 set 1 255

# For AP (or MCU-only config)
dmx status                      # defaults to /dev/ttyRPMSG0
```

**Protocol format:**
```
Command:  [0xAA] [cmd] [len:2] [payload] [checksum]
Response: [0xBB] [status] [len:2] [payload] [checksum]
```

| Cmd | Name | Description |
|-----|------|-------------|
| 0x01 | SET_CHANNELS | Set channel values |
| 0x02 | GET_STATUS | Get DMX status |
| 0x03 | ENABLE | Enable DMX output |
| 0x04 | DISABLE | Disable DMX output |
| 0x05 | BLACKOUT | All channels to 0 |
| 0x06 | SET_TIMING | Set refresh rate |
| 0x07 | GET_TIMING | Get timing config |

See `dmx_protocol_mcu.h` for full definitions.

## Quick Start

### 1. Copy source files to SDK

```bash
SDK_DIR=/path/to/luckfox-lyra-sdk

# MCU firmware source
cp firmware-mcu/src/*.c firmware-mcu/src/*.h \
   $SDK_DIR/hal/project/rk3506-mcu/src/

# Makefile with optimization flags (CRITICAL - see LESSONS_LEARNED.md)
cp firmware-mcu/gcc/Makefile \
   $SDK_DIR/hal/project/rk3506-mcu/GCC/

# RPMSG platform layer (I created this - doesn't exist in stock SDK)
mkdir -p $SDK_DIR/hal/middleware/rpmsg-lite/lib/rpmsg_lite/porting/platform/RK3506-MCU
mkdir -p $SDK_DIR/hal/middleware/rpmsg-lite/lib/include/platform/RK3506-MCU
cp firmware-mcu/platform/rpmsg_platform.c \
   $SDK_DIR/hal/middleware/rpmsg-lite/lib/rpmsg_lite/porting/platform/RK3506-MCU/
cp firmware-mcu/platform/rpmsg_platform.h firmware-mcu/platform/rpmsg_config.h \
   $SDK_DIR/hal/middleware/rpmsg-lite/lib/include/platform/RK3506-MCU/

# DTS files
cp firmware-mcu/dts/*.dts firmware-mcu/dts/*.dtsi \
   $SDK_DIR/kernel-6.1/arch/arm/boot/dts/

# Defconfig (enables MCU in AMP build)
cp firmware-mcu/defconfig/rk3506g_buildroot_spinand_amp_mcu_defconfig \
   $SDK_DIR/device/rockchip/.chips/rk3506/
```

### 2. Select MCU-enabled defconfig

```bash
./build.sh rk3506g_buildroot_spinand_amp_mcu_defconfig
```

This selects:
- DTS: `rk3506g-luckfox-lyra-amp-mcu-spinand.dts`
- ITS: `amp_linux_mcu.its` (includes both RT-Thread + MCU binaries)

### 3. Build

The build script requires Ubuntu 22.04 on x86 machine (or 24.04 + Docker - see [build-docker/](../build-docker/)).

```bash
./build.sh
```

### 4. Flash & Test

```bash
# Initial flash (maskrom mode)
sudo ./rkflash.sh update

# Fast iteration (via SSH)
scp rockdev/amp.img root@<ip>:/tmp/
ssh root@<ip> "flash_erase /dev/mtd2 0 0 && nandwrite -p /dev/mtd2 /tmp/amp.img"
ssh root@<ip> "poweroff"
# IMPORTANT: Power cycle the board (unplug/replug power)
```

**`reboot` doesn't work!** Always use `poweroff` then physical power cycle.

### 5. Verify

```bash
# Check RPMSG devices
ls /dev/ttyRPMSG*
# Dual-AMP: /dev/ttyRPMSG0 (CPU2) and /dev/ttyRPMSG1 (MCU)

# Test MCU
dmx -d /dev/ttyRPMSG1 status
dmx -d /dev/ttyRPMSG1 enable
dmx -d /dev/ttyRPMSG1 set 1 255
```

## Hardware

### DMX Output Pin

| Signal | RMIO | GPIO | Baud | Config |
|--------|------|------|------|--------|
| UART2_TX (DMX) | RM_IO6 | GPIO0_A6 | 250000 | 8N2 |

Different from AP firmware (UART3) to allow both to coexist.

### Debug Output (optional)

| Signal | GPIO | Baud |
|--------|------|------|
| UART5_TX | GPIO1_D2 | 115200 |
| UART5_RX | GPIO1_D3 | 115200 |

Connect serial adapter at 115200 baud to see MCU boot messages:

```
========================================
  DMX512 Gateway on RK3506 MCU
  Debug: UART5 @ 115200
  DMX:   UART2 @ 250000
========================================
[MCU] Waiting for Linux...

[MCU] Link UP
[MCU] Channel 'rpmsg-tty' ready
[MCU] Waiting for commands...
```

## Code structure

```
firmware-mcu/
├── src/
│   ├── main.c              # Entry + main loop + RPMSG + protocol
│   ├── dmx_driver.c        # DMX output (UART2 + break timing)
│   ├── dmx_driver.h        # Driver API
│   ├── dmx_protocol_mcu.h  # Protocol definitions
│   └── hal_conf.h          # HAL config (minimal)
├── gcc/
│   └── Makefile            # CRITICAL: optimization flags (see below)
├── platform/
│   ├── rpmsg_platform.c    # Custom RPMSG for MCU (MBOX1/MBOX3)
│   ├── rpmsg_platform.h
│   └── rpmsg_config.h
├── dts/
│   ├── rk3506-amp-mcu.dtsi     # MCU-specific AMP overlay
│   └── rk3506g-luckfox-lyra-amp-mcu-spinand.dts
├── defconfig/
│   └── rk3506g_buildroot_spinand_amp_mcu_defconfig
├── README.md               # This file
└── LESSONS_LEARNED.md      # The painful journey
```

### Key implementation details

#### `main.c` - Polling loop with state machine

Unlike AP version (RT-Thread threads), MCU runs a tight polling loop:

```c
int main(void) {
    HAL_Init();
    rpmsg_init();
    dmx_init();

    while (1) {
        /* 1. Process RPMSG RX (non-blocking) */
        while (rx_available()) {
            parse_rx_byte(rx_read());
        }

        /* 2. Poll DMX state machine (non-blocking) */
        dmx_poll(get_time_us());
    }
}
```

The key is `dmx_poll()` - a **non-blocking state machine**:

```c
void dmx_poll(uint64_t now) {
    switch (g_dmx_state) {
        case DMX_STATE_IDLE:
            if (now - g_last_frame >= frame_interval) {
                send_break_mab();      /* ~162µs, IRQs disabled */
                g_tx_idx = 0;
                g_dmx_state = DMX_STATE_TX_DATA;
                /* FALL THROUGH - start TX immediately after break/MAB */
            } else {
                break;
            }
            /* fall through */

        case DMX_STATE_TX_DATA:
            /* Fill FIFO incrementally (64 bytes max per call) */
            while (tx_idx < 513 && fifo_not_full()) {
                UART->THR = frame[tx_idx++];
            }
            if (tx_idx >= 513) {
                g_dmx_state = DMX_STATE_IDLE;
            }
            break;
    }
}
```

The fall-through ensures TX starts immediately after break/MAB (no gap). Then between FIFO refills, control returns to main loop to process RPMSG - keeping latency low.

#### DMX timing (same as AP)

```c
/* Break via LCR register */
static void dmx_send_break(void) {
    __disable_irq();
    uint32_t lcr = UART2->LCR;
    UART2->LCR = lcr | UART_LCR_BREAK;  // Force TX low
    __DSB();
    HAL_DelayUs(150);                   // 150µs break
    UART2->LCR = lcr;                   // Release
    __DSB();
    HAL_DelayUs(12);                    // 12µs MAB
    __enable_irq();
}
```

#### Size optimization (critical!)

The Rockchip SDK doesn't enable dead code elimination by default. With stock settings, a simple firmware is **77KB** - way over the 32KB limit.

**Progression:**
| Optimization | Size |
|--------------|------|
| Baseline (stock Makefile) | 77 KB |
| + `--specs=nano.specs` | 39 KB |
| + `-ffunction-sections -Wl,--gc-sections` | 25.8 KB |
| + Disable `HAL_CRU_MODULE_ENABLED` | **19 KB** |

**Two things to copy:**

1. **Makefile** (`gcc/Makefile`) - Adds missing flags:
```makefile
CFLAGS := -Os -ffunction-sections -fdata-sections -fno-unwind-tables
LDFLAGS := --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
```

2. **hal_conf.h** - Minimal HAL:
```c
#define HAL_TIMER_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_MBOX_MODULE_ENABLED
#define HAL_INTMUX_MODULE_ENABLED
// Everything else DISABLED - especially CRU!
```

See [LESSONS_LEARNED.md](LESSONS_LEARNED.md) for the full story.

**Current firmware: ~21KB** (code + data), leaving ~11KB headroom.

## DTS Configuration

The MCU DTS is critical - it prevents Linux from disabling peripherals the MCU needs.

### Key rule

**Every peripheral used by MCU code MUST be declared in `rockchip_amp` clocks.**

If you add code that uses a new peripheral, add its clocks to DTS. Otherwise Linux will gate the clock and MCU crashes.

### Example from `rk3506-amp-mcu.dtsi`

```dts
&rockchip_amp {
    clocks = <&cru PCLK_MAILBOX>,   /* RPMSG */
             <&cru PCLK_INTMUX>,    /* IRQ routing */
             <&cru PCLK_UART2>,     /* DMX output */
             <&cru SCLK_UART2>,
             <&cru PCLK_UART5>,     /* Debug console */
             <&cru SCLK_UART5>,
             <&cru PCLK_MBOX_MCU>,  /* MCU mailbox */
             <&cru CLK_M0>,         /* MCU clock */
             <&cru PCLK_M0>,
             <&cru HCLK_M0>;
};
```

## Limitations

### Hot reload doesn't work

The MCU **doesn't survive Linux `reboot`**. After reboot, MCU is dead and doesn't recover.

**What happens (observed):**
1. Linux `reboot` command
2. MCU stops responding
3. U-Boot loads firmware
4. MCU doesn't start
5. Power cycle required

**Root cause:** Unknown. I tried many approaches (see below), none worked. Without more documentation from Rockchip, I can't determine the exact cause.

**What I tried (all failed):**
- U-Boot MCU reset sequences (various CRU register combinations)
- SPL "sniper" (kill MCU before DDR init)
- MCU suicide command (`NVIC_SystemReset`) - MCU restarted too fast
- MCU "coma" (`WFI` with IRQs disabled)
- Load MCU code in DDR (instead of SRAM) - boots fine, but crashes on reboot

See [LESSONS_LEARNED.md](LESSONS_LEARNED.md) for detailed attempts.

**Workaround:** Use `poweroff` then physical power cycle.

### Alternative: remoteproc

[nvitya's repo](https://github.com/nvitya/rk3506-mcu) implements Linux `remoteproc` to load MCU firmware **after** Linux boots. This is the "correct" way to do it and would avoid the reboot issue.

However, for applications that need the MCU available immediately at power-on (< 1 second), or remain up even if the OS crashes, waiting for `remoteproc` to start the MCU can be a deal-breaker.

## Firmware update workflow

### Development (fast iteration)

```bash
# 1. Edit code
nano $SDK_DIR/hal/project/rk3506-mcu/src/main.c

# 2. Build AMP only (~10 sec)
./build.sh amp && ./build.sh firmware

# 3. Flash via SSH + power cycle (~30 sec total)
scp rockdev/amp.img root@<ip>:/tmp/
ssh root@<ip> "flash_erase /dev/mtd2 0 0 && nandwrite -p /dev/mtd2 /tmp/amp.img && poweroff"
# Unplug and replug power
```

### Production

For devices with physical power switches: works fine, user just toggles power.

For devices without power switch: **recommend AP approach instead** ([firmware-ap/](../firmware-ap/)).

## Performance

| Metric | Value |
|--------|-------|
| Firmware size | ~21 KB |
| SRAM available | 32 KB |
| SRAM remaining | ~11 KB |
| RPMSG RTT | ~250 µs |
| DMX frame time | ~23 ms |
| DMX refresh rate | 44 Hz |
| DMX frame jitter | < 1 µs |

RPMSG latency is noticeably better on the MCU in tight loop than on AP running RTOS with the stock `rpmsg-lite` implementation (250µs vs 400µs).

## Credits

Thanks to [nvitya's rk3506-mcu repo](https://github.com/nvitya/rk3506-mcu) - the only public resource on RK3506 MCU. His research saved me significant debugging time by understanding clock gating issues & limitations for hot rebooting the MCU.
