# MCU Lessons Learned

This document chronicles my journey to get the RK3506 Cortex-M0+ MCU working. Every pitfall and hard-won insight is documented here.

## TL;DR

1. **Bloated MCU firmware** - Stock Makefile lacks `-ffunction-sections --gc-sections` and `nano.specs` → 77KB instead of 19KB + unnecessary HAL includes by default
2. **Clock gating** - Declare ALL peripherals in DTS or Linux will disable them
3. **RPMSG platform** - Need custom implementation for MCU, HAL MBOX is buggy (bypass it)
4. **PINCTRL** - Don't call HAL_PINCTRL_SetRMIO, DTS handles pinmux
5. **Hot reload broken** - No known fix, power cycle required

---

## 1. Firmware size: 77KB → 19KB

The 32KB SRAM constraint forced aggressive optimization. Here's what actually mattered:

### The problem

With default Rockchip SDK settings, a simple "hello world + RPMSG" firmware was **77KB** - way over the 32KB limit.

### The solution (in order of impact)

| Optimization | Size after | Savings |
|--------------|------------|---------|
| Baseline (libc standard) | 77 KB | - |
| `--specs=nano.specs` | 39 KB | **38 KB** |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` | 25.8 KB | **13 KB** |
| Disable `HAL_CRU_MODULE_ENABLED` | 19 KB | **7 KB** |

**Total savings: 58KB (77KB → 19KB)**

### Key insight: Makefile flags were MISSING

The biggest surprise: **Rockchip's `Cortex-M.mk` doesn't enable dead code elimination!**

Original Makefile:
```makefile
CFLAGS := -Os
LDFLAGS := --specs=nosys.specs
```

Our Makefile:
```makefile
CFLAGS := -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
LDFLAGS := --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
```

Without `-ffunction-sections` + `--gc-sections`, **unused HAL code stays in the binary**.

### Why CRU still matters

Even with `--gc-sections`, disabling `HAL_CRU_MODULE_ENABLED` saves 7KB because:
- `HAL_UART_Init()` calls CRU functions
- These calls create a dependency chain
- `--gc-sections` can't remove code that's actually called

By disabling CRU at compile time, those calls are `#ifdef`'ed out entirely.

## 2. Clock gating: Linux kills MCU peripherals

### The problem

MCU boots fine, runs for 2-3 seconds, then looks stalled. No obvious cause.

### Root cause

Linux's `clk` framework disables "unused" clocks during late boot. If a peripheral isn't claimed by a Linux driver, its clock gets gated.

### The solution

Declare all MCU peripherals in the `rockchip_amp` DTS node:

```dts
&rockchip_amp {
    clocks = <&cru CLK_M0>,         /* MCU core */
             <&cru PCLK_M0>,
             <&cru HCLK_M0>,
             <&cru PCLK_MAILBOX>,   /* RPMSG */
             <&cru PCLK_INTMUX>,    /* IRQ routing */
             <&cru PCLK_UART2>,     /* DMX output */
             <&cru SCLK_UART2>,
             /* ... etc ... */
};
```

**Critical rule:** Every peripheral used by MCU code MUST be listed, event `INTMUX` or the mailbox! Add one, forget the clock → erratic behaviour.

## 3. RPMSG platform layer for MCU

### The problem

rpmsg-lite is designed for RT-Thread. On MCU:
- Different mailbox assignments (`MBOX1`/`MBOX3` vs `MBOX0`/`MBOX2`)
- Different interrupt routing (`INTMUX`)
- No OS primitives (mutexes, queues)

### The solution

Custom `rpmsg_platform.c` for MCU:

1. Use `MBOX1` (TX) and `MBOX3` (RX)
2. Configure `INTMUX` to route interrupts
3. Bare-metal environment (polling, no queues)

### HAL MBOX is buggy - bypass it

The HAL's mailbox functions have broken semantics. We read registers directly:

```c
static void rpmsg_mbox_isr(void) {
    /* Read status DIRECTLY - HAL is broken */
    uint32_t status = MBOX3->A2B_STATUS;

    if (status & 0x1) {
        msg.CMD = MBOX3->A2B_CMD;
        msg.DATA = MBOX3->A2B_DATA;
        MBOX3->A2B_STATUS = 0x1;  /* W1C clear */
        rpmsg_remote_cb(&msg, NULL);
    }
}
```

### Force `A2B_INTEN`

HAL doesn't properly enable the A2B interrupt. Do it manually:

```c
MBOX3->A2B_INTEN = (1UL << 16) | (1UL << 0);  /* Write-enable + enable */
```

## 4. PINCTRL: Don't call `HAL_PINCTRL_SetRMIO`

### The problem

After adding UART2, MBOX stopped working entirely.

### Root cause

Calling `HAL_PINCTRL_SetRMIO()` from MCU code somehow corrupts the mailbox subsystem. The exact mechanism is unclear.

### The solution

Let DTS handle all pinmux. MCU code just uses the peripheral - pins are already configured:

```dts
/* In DTS - Linux configures pins at boot */
&pinctrl {
    rm_io6_uart2_tx: rm-io6-uart2-tx {
        rockchip,pins = <0 RK_PA6 7 &pcfg_pull_none>;
    };
};
```

**Rule:** Never call `HAL_PINCTRL` from MCU code.

## 5. Hot reload: Everything I tried (all failed)

The MCU doesn't survive Linux `reboot`. After extensive testing, I couldn't find a solution.

### Observed behavior

1. Linux `reboot` command
2. MCU stops responding
3. U-Boot loads firmware
4. MCU doesn't start
5. Power cycle required

### What I tried

| Attempt | Description | Result |
|---------|-------------|--------|
| U-Boot reset v1 | CRU_SOFTRST_CON5 bit 10 | Only bus reset, not core |
| U-Boot reset v2 | Bits 10+11 (bus + core) | Reset doesn't propagate |
| U-Boot reset v3 | Enable clocks before reset | Too late, after memcpy |
| SPL Sniper | Kill MCU in SPL before DDR init | Still doesn't work |
| MCU suicide | NVIC_SystemReset on command | Restarts too fast, HardFaults |
| MCU coma | WFI with IRQs disabled | U-Boot doesn't wake it |
| DDR loading | Load MCU to DDR instead of SRAM | Boots fine, but same problem when rebooting |
| DDR loading + cache flush | flush_dcache_all() before start | Still broken |

### Root cause: Unknown

Without hint from Rockchip, we can't determine the exact cause. Possibilities:
- TrustZone/ATF configuration
- OP-TEE state machine
- Hidden reset dependencies
- Hardware limitation

### Workaround

Use `poweroff` then physical power cycle or toggle SoC Enable pin. For devices without switches, use the AP approach instead.

### Alternative: `remoteproc`

[nvitya's repo](https://github.com/nvitya/rk3506-mcu) implements Linux `remoteproc` to load MCU firmware after Linux boots. This would avoid the reboot issue but means MCU isn't available until Linux is up (5-10 seconds vs instant-on).
