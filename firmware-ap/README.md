# DMX Firmware (RT-Thread AMP)

Real-time code for DMX512 generation. Runs on CPU2 (dedicated), communicates with Linux via RPMSG.

## Context: Why AMP?

### The DMX problem

DMX512 has two timing challenges:

**1. BREAK/MAB signals** (microsecond precision)

| Signal | Transmitter       | Receiver               | Typical                |
|--------|-------------------|------------------------|------------------------|
| BREAK  | ≥92 µs            | ≥88 µs                 | 176 µs                 |
| MAB    | ≥12 µs            | ≥8 µs                  | 12 µs                  |

Some receivers are picky and reject frames with marginal timing.

**2. Frame rate consistency** (millisecond precision)

DMX refreshes at up to 44 Hz (23ms/frame, spec standard). For smooth lighting transitions (fades, chases), the frame rate must be rock-solid: any jitter causes visible flickering or jerky motion. This is especially critical for:
- LED dimmers (PWM artifacts amplify timing issues)
- Moving heads (smooth pan/tilt requires consistent updates)
- Video-synced lighting (frame drops = visible glitches)

From Linux userspace, achieving this tight timing is problematic:
- `usleep(100)` → ~1ms precision at best (scheduler, interrupts)
- `nanosleep` + `SCHED_FIFO` → better but not guaranteed
- `PREEMPT_RT` kernel → reduced jitter but still ~50-100µs
- **Worse**: high jitter means inconsistent frame timing → unreliable DMX output

Difficult to achieve required precision & repeatability without offloading DMX signal generation...

### How I found this

Digging through the Luckfox SDK:
```
rtos/bsp/rockchip/rk3506-32/    # RT-Thread BSP for RK3506
├── applications/               # Application code (our code goes here)
├── board/                      # Hardware init
└── ...

device/rockchip/.chips/rk3506/                # Board configs
├── rk3506g_buildroot_spinand_amp_defconfig   # AMP config (SPI NAND)
├── rk3506b_buildroot_emmc_amp_defconfig      # AMP config (eMMC)
└── ...
```

The `rk3506g_buildroot_spinand_amp` SDK config:
- Boots Linux on CPU0+CPU1
- Boots RT-Thread on CPU2
- Configures RPMSG (shared memory)
- Reserves UART3/UART4 for the AMP core
- Uses DTS: `rk3506g-luckfox-lyra-amp-spinand.dts`

**Note (Lyra Ultra vs Plus):** The AMP DTS includes `rk3506-luckfox-lyra-ultra.dtsi`, but it works fine on **Lyra Plus** boards:
- **RAM is identical**: Both use RK3506G with **128MB DDR3L on-chip** (fixed hardware, auto-detected by bootloader - not DTS-configured)
- **Differences are minor**: The Ultra dtsi only adds display timing tweaks and storage controller configs
- **AMP overrides what matters**: `rk3506-amp.dtsi` properly configures all AMP-specific sections (reserved memory, RPMSG, CPU affinity, peripheral routing)

The RK3506**B** variant (external DDR interface) would need different handling, but that's not used here.

## Architecture

```
┌───────────────────────────────────┐       ┌─────────────────────────┐
│         RT-Thread (CPU2)          │       │   Linux (CPU0 + CPU1)   │
│                                   │       │                         │
│  ┌───────────┐   ┌──────────────┐ │       │  ┌───────────────────┐  │
│  │  main.c   │──▶│ rpmsg_uart_  │◀├───────┼─▶│    DMX Client     │  │
│  │           │   │    dmx.c     │ │ RPMSG │  │  /dev/ttyRPMSG0   │  │
│  └───────────┘   └──────┬───────┘ │       │  └───────────────────┘  │
│                         │         │       │            ▲            │
│                ┌────────▼───────┐ │       │  ┌─────────┴─────────┐  │
│                │  dmx_driver.c  │ │       │  │        DMX        │  │
│                │  UART3 + timer │ │       │  │    applications   │  │
│                └────────┬───────┘ │       │  └───────────────────┘  │
│                         │         │       │                         │
└─────────────────────────┼─────────┘       └─────────────────────────┘
                          ▼
                    DMX512 Output
```

## Code

DMX512 allows refresh rates from ~1 Hz to 44 Hz (limited by frame duration). Default is **44 Hz** (23ms period) for smooth lighting transitions. The frame rate is configurable at runtime via `dmx timing`.

```
Frame timing breakdown:
├── BREAK:        150 µs
├── MAB:           12 µs
├── Start code:    44 µs (1 byte @ 250kbaud)
├── Data:       22528 µs (512 bytes × 44µs)
├── Margin:      ~266 µs
└── Total:        ~23 ms → 44 Hz
```

**Implementation**: A dedicated RT-Thread thread (`dmx_tx`) runs in an infinite loop:
1. Checks values from frame buffer
2. Send BREAK + MAB (timing-critical, IRQs disabled)
3. Send 513 bytes via UART (start code + 512 channels)
4. Sleep until next frame period (23ms total)

This approach ensures **consistent frame-to-frame timing** - the sleep compensates for any variation in transmission time.

### `dmx_driver.c` - UART control and timing

```c
// BREAK generation via LCR register (force TX low)
// Uses hardware timer (TIMER5 @ 24MHz) for accurate timing
void uart_send_break_mab(volatile struct UART_REG *uart, uint32_t break_us, uint32_t mab_us) {
    rt_base_t level = rt_hw_interrupt_disable();  // Prevent jitter from ISRs

    // BREAK: Set break control bit
    uint32_t lcr = uart->LCR;
    uart->LCR = lcr | UART_LCR_BREAK;
    __asm__ volatile("dsb sy" ::: "memory");      // Flush to hardware
    rt_hw_us_delay(break_us);                     // 150µs (timer-based, accurate)

    // Clear break
    uart->LCR = lcr;
    __asm__ volatile("dsb sy" ::: "memory");

    // MAB: Mark After Break
    rt_hw_us_delay(mab_us);                       // 12µs (timer-based, accurate)

    rt_hw_interrupt_enable(level);
}

// DMX TX thread (runs continuously)
static void dmx_tx_thread_entry(void *parameter) {
    while (g_dmx.running) {
        if (!g_dmx.enabled) {
            rt_thread_mdelay(100);
            continue;
        }

        uart_wait_idle(g_dmx.uart_hw);
        uart_send_break_mab(g_dmx.uart_hw, g_dmx.break_us, g_dmx.mab_us);

        // Send frame via RT-Thread driver (start code + 512 channels)
        rt_device_write(g_dmx.uart_dev, 0, g_dmx.frame_buf, DMX_FRAME_SIZE);

        // Maintain frame rate
        rt_thread_delay(target_ticks - elapsed_ticks);
    }
}
```

**Timing implementation**: `rt_hw_us_delay()` uses hardware TIMER5 (24MHz crystal) via `HAL_DelayUs()`. The timer counts independently of CPU frequency, providing accurate microsecond delays even with interrupts disabled (polling-based).

**Defaults: 44 Hz, BREAK=150µs, MAB=12µs** - safe margin above spec minimums. All timing parameters are adjustable at runtime using the `dmx` CLI:

```bash
dmx timing                    # Show current timing
dmx timing 30                 # Set frame rate to 30 Hz
dmx timing 30 150 12          # Set fps=30Hz, break=150µs, mab=12µs
dmx timing 0 200 0            # Set break=200µs only (0=unchanged)
```

Lower frame rates (e.g., 22 Hz) can improve compatibility with picky DMX receivers.

### `rt_hw_us_delay.c` - Custom microsecond delay

**Important**: I had to implement `rt_hw_us_delay()` myself. The Rockchip RK3506 BSP doesn't provide one!

RT-Thread's default implementation (`kservice.c`) is a weak stub that does nothing:

```c
RT_WEAK void rt_hw_us_delay(rt_uint32_t us)
{
    (void) us;
    RT_DEBUG_LOG(..., ("rt_hw_us_delay() doesn't support for this board."));
}
```

My implementation uses the Rockchip HAL hardware timer:

```c
#include <rtthread.h>
#include "hal_base.h"

void rt_hw_us_delay(rt_uint32_t us)
{
    HAL_DelayUs(us);  /* Timer-based, not CPU loop */
}
```

The HAL configures `SYS_TIMER = TIMER5` in `hal_conf.h`. `HAL_DelayUs()` polls the timer counter directly - highly precise (confirmed on oscilloscope), no interrupts needed, works in critical sections.

Don't use `HAL_CPUDelayUs()`, it's based on an inaccurate CPU cycle loop.

### `rpmsg_uart_dmx.c` - Linux communication

Receives commands from `/dev/ttyRPMSG0` on the Linux side. Initialization is automatic via `INIT_APP_EXPORT()`.

```c
/* Called automatically at boot via INIT_APP_EXPORT() */
static int rpmsg_dmx_init(void) {
    // Init DMX driver first
    dmx_init();

    // Init RPMSG as remote
    g_rpmsg_instance = rpmsg_lite_remote_init(...);
    rpmsg_lite_wait_for_link_up(g_rpmsg_instance, RL_BLOCK);

    // Create endpoint and reception thread
    g_rpmsg_ept = rpmsg_lite_create_ept(...);
    rt_thread_create("rpmsg_rx", rpmsg_recv_thread, ...);

    return RT_EOK;
}
INIT_APP_EXPORT(rpmsg_dmx_init);

/* Command dispatcher (called from reception thread) */
static void parse_command(uint8_t *rx_buf, uint32_t rx_len, uint32_t src_addr) {
    uint8_t cmd = rx_buf[1];

    switch (cmd) {
        case CMD_DMX_ENABLE:  handle_cmd_enable(src_addr);  break;
        case CMD_DMX_DISABLE: handle_cmd_disable(src_addr); break;
        case CMD_DMX_SET_CHANNELS: handle_cmd_set_channels(...); break;
        // ...
    }
}
```

### `main.c` - RT-Thread entry point

Minimal entry point - just prints boot info. All initialization happens via `INIT_APP_EXPORT()` in `rpmsg_uart_dmx.c`.

```c
int main(int argc, char **argv) {
    rt_kprintf("========================================\n");
    rt_kprintf("  RT-Thread on RK3506G2 CPU2\n");
    rt_kprintf("  AMP Mode: Linux (CPU0+1) + RTOS (CPU2)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Application: DMX512 Gateway\n");

    /* rpmsg_dmx_init() is called automatically via INIT_APP_EXPORT() */

    return 0;
}
```

## RPMSG protocol

Binary format over `/dev/ttyRPMSG0`:

```
Command:  [0xAA] [cmd] [len:2] [payload] [checksum]
Response: [0xBB] [status] [len:2] [payload] [checksum]
```

| Cmd | Name | Payload |
|-----|------|---------|
| 0x01 | SET_CHANNELS | start_ch:2, values:N |
| 0x02 | GET_STATUS | - |
| 0x03 | ENABLE | - |
| 0x04 | DISABLE | - |
| 0x05 | BLACKOUT | - |
| 0x06 | SET_TIMING | refresh_hz:2, break_us:2, mab_us:2 |
| 0x07 | GET_TIMING | - |

See `dmx_protocol.h` in `dmx-client/` for full definitions.

## Build & deploy

### Prerequisites

- Luckfox Lyra SDK with AMP support (version used: `Luckfox_Lyra_SDK_250815`)
- x86 PC/VM with Ubuntu 22.04 (or 24.04 + Docker - see [build-docker/](../build-docker/))

### RT-Thread configuration (scons menuconfig)

Before building, configure RT-Thread to enable RPMSG and UART3:

```bash
cd rtos/bsp/rockchip/rk3506-32
scons --menuconfig
```

**Enable these options:**
```
RT-Thread rockchip common drivers
  → RT-Thread rockchip RPMSG driver
    → [*] Enable RPMSG LITE
      → [*] Enable Linux RPMSG Support

RT-Thread bsp drivers
  → Board Configuration
    → Enable Peripheral Drivers
      → Enable UART
        → [*] Enable UART3
```

**IMPORTANT: Do NOT enable "test" options** in "RT-Thread bsp test case". These are Rockchip examples that will conflict with our RPMSG endpoint:

```bash
# Verify configuration:
grep -E "RT_USING_LINUX_RPMSG|RT_USING_UART3|TEST.*RPMSG" .config

# Expected:
# CONFIG_RT_USING_RPMSG_LITE=y
# CONFIG_RT_USING_LINUX_RPMSG=y
# CONFIG_RT_USING_UART3=y
# # CONFIG_RT_USING_COMMON_TEST_LINUX_RPMSG_LITE is not set    ← Must be disabled!
# # CONFIG_RT_USING_COMMON_TEST_LINUX_TTY_RPMSG_LITE is not set ← Must be disabled!
```

### Custom code integration

RT-Thread application code goes in:
```
rtos/bsp/rockchip/rk3506-32/applications/
├── dmx_driver.c      # DMX driver (UART + timing)
├── dmx_driver.h      # Driver API
├── dmx_protocol.h    # RPMSG protocol (shared with dmx-client)
├── rpmsg_uart_dmx.c # RPMSG handler
├── rt_hw_us_delay.c  # Microsecond delay (REQUIRED - see above)
└── main.c            # Entry point
```

The BSP Makefile/Kconfig automatically includes `.c` files from this folder.

### Firmware image build with SDK

```bash
# 1. Copy DMX source code to RT-Thread applications folder
cp firmware/*.c firmware/*.h rtos/bsp/rockchip/rk3506-32/applications/

# 2. Configure RT-Thread (enable RPMSG + UART3) - see details below
cd rtos/bsp/rockchip/rk3506-32
scons --menuconfig
cd -

# 3. Select AMP defconfig
./build.sh rk3506g_buildroot_spinand_amp_defconfig

# 4. (Optional) Customize Linux kernel or Buildroot
./build.sh kernel-config      # Add kernel modules, drivers...
./build.sh buildroot-config   # Add packages (curl, nano, strace...)

# 5. Full build (U-Boot + Kernel + Buildroot + RT-Thread)
./build.sh

# 6. Output
ls rockdev/
# → update.img (complete flashable image)
```

### Flash (initial)

```bash
# Put board in maskrom mode:
# 1. Hold BOOT button
# 2. Connect USB-C
# 3. Release

# Flash complete image
sudo ./rkflash.sh update
```

### Fast firmware update (development)

After the initial flash, you can update **only the AMP firmware** without rebuilding the full image. This is much faster for iterating on the RT-Thread code.

**Why this works:**
- The AMP firmware lives in a separate partition (`mtd2`)
- `./build.sh amp` rebuilds only RT-Thread (~10 sec)
- Flash via SSH while Linux is running (no maskrom mode needed)
- Reboot loads the new firmware (U-Boot reads `amp.img` at boot)

**Workflow:**

```bash
# 1. Modify RT-Thread code
nano $SDK_DIR/rtos/bsp/rockchip/rk3506-32/applications/dmx_driver.c

# 2. Rebuild AMP only (~10 sec)
cd $SDK_DIR
./build.sh amp

# 3. Flash via SSH + reboot (~20 sec)
scp $SDK_DIR/rockdev/amp.img root@<ip>:/tmp/
ssh root@<ip> "flash_erase /dev/mtd2 0 0 && nandwrite -p /dev/mtd2 /tmp/amp.img && reboot"
```

**Total iteration time: ~30 seconds** (vs ~10 min for full rebuild + maskrom flash)

**Requirements:**
- `mtd-utils` package in Buildroot (provides `flash_erase`, `nandwrite`) - included in default config
- Network access to the target

**Partition layout** (for reference):
```
mtd0: uboot
mtd1: boot    # Linux kernel
mtd2: amp     # <- RT-Thread firmware
mtd3: rootfs
```

## Hardware

### Pinout

| Signal | RMIO | GPIO | Notes |
|--------|------|------|-------|
| UART3_TX (DMX Out) | RM_IO4 | GPIO0_A4 | 250kbaud, 8N2 |
| UART3_RX | RM_IO5 | GPIO0_A5 | (unused for DMX) |
| UART4_TX (RT-Thread console) | RM_IO2 | GPIO0_A2 | 1.5Mbps, Debug only |
| UART4_RX | RM_IO3 | GPIO0_A3 | 1.5Mbps, Debug only |

Both UARTs are pre-configured for RT-Thread (CPU2) in `rk3506-amp.dtsi`.

In production, UART4 console can be disabled to save a peripheral - all communication can go through RPMSG instead.

### RS485 transceiver

```
Luckfox          MAX485           DMX
────────         ──────           ───
UART3_TX ─────▶  DI
3V3_OUT ──────▶  DE ──┐
                 /RE ─┘
                 RO  (unused)
                 A   ─────────▶  Data+
                 B   ─────────▶  Data-
GND ──────────▶  GND ─────────▶  GND
```

### Verifying AMP is active

After booting, Linux should only see 2 CPUs. You can check it out by running `htop` which will only display `CPU0` & `CPU1`.

You can also verify via:
```bash
# Should show 2 CPUs (cpu0, cpu1)
ls /sys/devices/system/cpu/ | grep "cpu[0-9]"

# Or check dmesg
dmesg | grep -i "smp\|cpu"
```

### Customizing pin/peripheral assignment

**Don't use `luckfox-config`** for AMP builds. This runtime utility (normally available on standard Luckfox images) is intentionally disabled in AMP configurations (`# RK_LUCKFOX_CONFIG is not set`). Using it would risk conflicts with peripherals reserved for the RTOS core (UART3, UART4, I2C0, TIMER5...).

**To customize the Linux/RTOS peripheral split**, create your own DTS:

```bash
# 1. Copy the AMP DTS as a starting point
cd kernel-6.1/arch/arm/boot/dts/
cp rk3506g-luckfox-lyra-amp-spinand.dts rk3506g-luckfox-lyra-amp-custom.dts

# 2. Edit your custom DTS
```

**DTS structure explained:**

```
rk3506g-luckfox-lyra-amp-spinand.dts    ← Your board's AMP config
    │
    ├── #include "rk3506-luckfox-lyra-ultra.dtsi"   ← Base board config (pins, regulators, displays...)
    │       │
    │       └── #include "rk3506.dtsi"              ← SoC definitions (all peripherals)
    │
    └── #include "rk3506-amp.dtsi"                  ← AMP-specific config
            │
            ├── /delete-node/ cpu@f02               ← Removes CPU2 from Linux
            ├── rockchip-amp { ... }                ← IRQ routing to CPU2
            ├── rpmsg { ... }                       ← Inter-processor communication
            └── &reserved_memory { ... }            ← Memory regions for RTOS
```

**After editing:**

```bash
# 3. Update defconfig to use your DTS
#    Edit: device/rockchip/.chips/rk3506/rk3506g_buildroot_spinand_amp_defconfig
#    Change: RK_KERNEL_DTS_NAME="rk3506g-luckfox-lyra-amp-custom"

# 4. Rebuild
./build.sh rk3506g_buildroot_spinand_amp_defconfig
./build.sh
```

## Debugging

### RT-Thread console

By plugging a serial USB dongle (RX pin) to `RM_IO2` with default baudrate of 1 500 000 bps, you will see the RTOS booting immediately after power is set:

```
 \ | /
- RT -     Thread Operating System
 / | \     4.1.1 build Nov 27 2025 21:34:24
 2006 - 2022 Copyright by RT-Thread team

========================================
         RPMSG DMX512 Driver
========================================

[RPMSG] Shared memory: 0x3c00000 - 0x3d00000

[DMX] Initializing DMX512 driver (simple version)...
[DMX] SystemCoreClock = 816000000 Hz (816 MHz)
[DMX] UART3 opened successfully
[DMX] Driver initialized (250kbaud, 8N2, polling mode)
[DMX] UART3 TX = GPIO0_A4 (RM_IO4)
[DMX] Timing: BREAK=150µs, MAB=12µs (TIMER5 @ 24MHz)
[DMX] TX thread running (100% CPU2 OK - dedicated core)
[RPMSG] Remote core CPU ID: 2 (AP)
[RPMSG] Initializing as REMOTE (link=0x02)...
[RPMSG] Waiting for link up...
[RPMSG] Link UP!
[RPMSG] Endpoint created (addr=0x32)
[RPMSG] Announcing channel 'rpmsg-tty'...
[RPMSG] Reception thread created

========================================
  DMX512 Driver Ready!
  UART3 TX: 250kbaud, 8N2, 44Hz
  Waiting for commands...
========================================

[DMX] TX thread started (CPU2 dedicated)
```
The RT-Thread console offers a few shell-like commands:

```
msh> ps              # List threads
msh> free            # Available memory
msh> list_device     # Devices (look for uart5)
```

### Oscilloscope

To validate timings, measure on UART3_TX (GPIO0_A4):
- BREAK: 150µs (low)
- MAB: 12µs (high)
- Start code: 44µs
- Data: 44µs/byte × 512

## Metrics

| Metric | Value |
|--------|-------|
| Firmware size | ~190 KB |
| RAM usage | ~32 KB |
| RPMSG latency | ~500 µs (from Linux `dmx` util) |
| Timing precision | < 1 µs (hardware timer) |