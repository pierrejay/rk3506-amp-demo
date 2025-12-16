/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * DMX512 Driver for RT-Thread on RK3506.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "hal_base.h"
#include "iomux.h"
#include "dmx_driver.h"

/* SystemCoreClock from CMSIS */
extern uint32_t SystemCoreClock;

/* ============================================================================
 * Hardware Definitions
 * ============================================================================ */

/* DEBUG MODE: Uncomment to send "HELLO\n" instead of DMX frames */
// #define DMX_DEBUG_TEXT_MODE

#define UART_DEVICE_NAME    "uart3"
#define DMX_BAUDRATE        250000      /* DMX512 uses 250kbaud */

/* ============================================================================
 * Global State
 * ============================================================================ */

static struct {
    rt_device_t uart_dev;               /* UART3 device handle */
    volatile struct UART_REG *uart_hw;  /* Direct hardware access (VALIDATED!) */
    rt_thread_t tx_thread;              /* DMX transmission thread */
    rt_mutex_t mutex;                   /* Buffer access mutex */

    uint8_t channels[DMX_UNIVERSE_SIZE]; /* DMX channel values */
    uint8_t frame_buf[DMX_FRAME_SIZE];  /* TX buffer (start code + channels) */

    volatile bool enabled;              /* Transmission enabled */
    volatile bool running;              /* Thread running flag */

    uint16_t refresh_hz;                /* Frame rate in Hz (1-44) */
    uint16_t break_us;                  /* BREAK duration (actual µs) */
    uint16_t mab_us;                    /* MAB duration (actual µs) */

    uint32_t frame_count;               /* Total frames sent */
    uint32_t last_fps_time;             /* For FPS calculation */
    uint32_t last_frame_count;          /* For FPS calculation */
    uint32_t fps;                       /* Current FPS × 100 */
    uint32_t errors;                    /* Error count */
} g_dmx;

/* ============================================================================
 * UART Break Generation - LCR METHOD
 * ============================================================================ */

/* Register Bit Masks */
#define UART_LCR_BREAK      (1 << 6)  /* Bit 6: Break Control */
#define UART_LCR_8N2        0x07      /* 8 data bits, 2 stop bits, no parity, DLAB=0, BREAK=0 */
#define UART_FCR_FIFO_EN    0x07      /* Enable FIFO + clear RX/TX FIFOs */
#define UART_USR_BUSY       (1 << 0)  /* Bit 0: UART Busy */
#define UART_USR_TFNF       (1 << 1)  /* Bit 1: TX FIFO Not Full */
#define UART_USR_TFE        (1 << 2)  /* Bit 2: TX FIFO Empty */

/**
 * Wait for UART to be completely idle (CRITICAL for DMX!)
 *
 * TODO: Timeout is silent - if we timeout, we continue anyway which could
 * cause weird states. Consider: errors++, log once, recovery sequence
 * (LCR=8N2, FCR=0x07 purge, re-check idle).
 */
static void uart_wait_idle(volatile struct UART_REG *uart)
{
    int timeout = 100000;  /* Safety timeout */

    while (timeout--) {
        uint32_t usr = uart->USR;

        /* Wait for TX FIFO empty (TFE) AND not busy (shift register done) */
        if ((usr & UART_USR_TFE) && !(usr & UART_USR_BUSY)) {
            return;
        }
    }
}

/**
 * Direct polling TX - bypass RT-Thread serial driver entirely.
 *
 * CRITICAL: Forces LCR to known good 8N2 state to ensure:
 * - DLAB=0 (access THR, not DLL)
 * - BREAK=0 (release line if stuck from previous break)
 * - Correct frame format (8N2)
 */
static void uart_tx_poll(volatile struct UART_REG *uart, const uint8_t *buf, size_t len)
{
    /*
     * 1. FORCE LCR to clean 8N2 state (no RMW!)
     * This clears BREAK bit if stuck, sets DLAB=0, ensures 8N2 format.
     */
    uart->LCR = UART_LCR_8N2;
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * 2. Enable FIFO (required for USR.TFNF to be valid on DW_apb_uart)
     * FCR is write-only at offset 0x08 (same as IIR read)
     *
     * TODO: FCR=0x07 resets TX/RX FIFOs every frame (overkill).
     * Could do FCR=0x01 once at init, FCR=0x07 only on error recovery.
     */
    uart->FCR = UART_FCR_FIFO_EN;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 3. Blast data into FIFO */
    for (size_t i = 0; i < len; i++) {
        /* Wait for TX FIFO to have space */
        while (!(uart->USR & UART_USR_TFNF)) {
            /* spin */
        }
        uart->THR = buf[i];
    }

    /* 4. Wait for transmission to fully complete */
    while (!((uart->USR & UART_USR_TFE) && !(uart->USR & UART_USR_BUSY))) {
        /* spin */
    }
}

/**
 * Send UART Break - Direct LCR register access
 *
 * Uses hardware timer (TIMER5 @ 24MHz) for accurate timing.
 * IRQs disabled to prevent jitter during timing-critical section.
 *
 * IMPORTANT: Uses absolute LCR writes (no RMW) to avoid BREAK bit getting stuck.
 */
static void uart_send_break_mab(volatile struct UART_REG *uart3, uint32_t break_us, uint32_t mab_us)
{
    /* CRITICAL SECTION: Break + MAB must be atomic */
    rt_base_t level = rt_hw_interrupt_disable();

    /* BREAK: Set LCR to 8N2 + BREAK bit (absolute write, no RMW) */
    uart3->LCR = UART_LCR_8N2 | UART_LCR_BREAK;
    __asm__ volatile("dsb sy" ::: "memory");
    rt_hw_us_delay(break_us);

    /* Clear break: restore clean 8N2 (absolute write) */
    uart3->LCR = UART_LCR_8N2;
    __asm__ volatile("dsb sy" ::: "memory");

    /* MAB: Mark After Break */
    rt_hw_us_delay(mab_us);

    rt_hw_interrupt_enable(level);
}

/* ============================================================================
 * DMX Transmission Thread (SIMPLE INFINITE LOOP)
 * ============================================================================ */

/**
 * DMX transmission thread
 *
 * Runs in infinite loop:
 * - If enabled: send frame at fixed period (dmx.refresh_hz)
 * - If disabled: sleep 100ms
 *
 * 100% CPU usage is OK - we have CPU2 dedicated!
 */
static void dmx_tx_thread_entry(void *parameter)
{
    uint32_t frame_start;

#ifdef DMX_DEBUG_TEXT_MODE
    rt_kprintf("[DMX] TX thread started - DEBUG TEXT MODE\n");
#else
    rt_kprintf("[DMX] TX thread started (CPU2 dedicated)\n");
#endif

    while (g_dmx.running) {
        if (!g_dmx.enabled) {
            /* DMX disabled - sleep */
            rt_thread_mdelay(100);
            continue;
        }

#ifdef DMX_DEBUG_TEXT_MODE
        /* DEBUG MODE: Send "HELLO\n" every 100ms */
        const uint8_t test_msg[] = "HELLO\n";
        uart_tx_poll(g_dmx.uart_hw, test_msg, 6);
        g_dmx.frame_count++;
        rt_thread_mdelay(100);  /* 10 Hz for readability */
        continue;
#endif

        frame_start = rt_tick_get();

        /* CRITICAL: Wait for previous frame to finish! */
        uart_wait_idle(g_dmx.uart_hw);

        /* Lock buffer and prepare frame */
        rt_mutex_take(g_dmx.mutex, RT_WAITING_FOREVER);
        g_dmx.frame_buf[0] = 0x00;  /* Start code */
        rt_memcpy(&g_dmx.frame_buf[1], g_dmx.channels, DMX_UNIVERSE_SIZE);
        rt_mutex_release(g_dmx.mutex);

        /* Send BREAK + MAB (atomic, timer-based timing) */
        uart_send_break_mab(g_dmx.uart_hw, g_dmx.break_us, g_dmx.mab_us);

        /* Send DATA via direct polling (bypass RT-Thread serial driver) */
        uart_tx_poll(g_dmx.uart_hw, g_dmx.frame_buf, DMX_FRAME_SIZE);
        g_dmx.frame_count++;

        /* Calculate FPS every second */
        uint32_t now = rt_tick_get();
        if (now - g_dmx.last_fps_time >= RT_TICK_PER_SECOND) {
            uint32_t frames_sent = g_dmx.frame_count - g_dmx.last_frame_count;
            uint32_t time_ms = (now - g_dmx.last_fps_time) * 1000 / RT_TICK_PER_SECOND;

            /* FPS × 100 for precision */
            if (time_ms > 0) {
                g_dmx.fps = (frames_sent * 100000) / time_ms;
            }

            g_dmx.last_fps_time = now;
            g_dmx.last_frame_count = g_dmx.frame_count;
        }

        /*
         * Pacing: At 44Hz we're at physical limit (~22.7ms/frame), just loop.
         * For lower rates, add delay in µs after frame completion.
         */
        if (g_dmx.refresh_hz < DMX_REFRESH_HZ_MAX) {
            uint32_t frame_period_us = 1000000 / g_dmx.refresh_hz;
            uint32_t elapsed_us = (rt_tick_get() - frame_start) * 1000000 / RT_TICK_PER_SECOND;
            if (elapsed_us < frame_period_us) {
                rt_hw_us_delay(frame_period_us - elapsed_us);
            }
        }
        /* At max rate (44Hz), continue immediately - no delay needed */
    }

    rt_kprintf("[DMX] TX thread stopped\n");
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int dmx_init(void)
{
    rt_err_t ret;
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    rt_kprintf("[DMX] Initializing DMX512 driver (simple version)...\n");
    rt_kprintf("[DMX] SystemCoreClock = %u Hz (%u MHz)\n",
               SystemCoreClock, SystemCoreClock / 1000000);

    /* Initialize state */
    rt_memset(&g_dmx, 0, sizeof(g_dmx));

    /* DMX timing defaults (timer-based delay is accurate) */
    g_dmx.refresh_hz = DMX_REFRESH_HZ_DEFAULT;  /* 44 Hz */
    g_dmx.break_us   = DMX_BREAK_US_DEFAULT;    /* DMX spec: 88-176µs (receiver), using 150µs */
    g_dmx.mab_us     = DMX_MAB_US_DEFAULT;      /* DMX spec: >8µs (receiver), using 12µs */

    /* Direct hardware access (VALIDATED!) */
    g_dmx.uart_hw = (volatile struct UART_REG *)UART3_BASE;

    /* Find UART3 device */
    g_dmx.uart_dev = rt_device_find(UART_DEVICE_NAME);
    if (!g_dmx.uart_dev) {
        rt_kprintf("[DMX] ERROR: UART3 not found\n");
        return -RT_ERROR;
    }

    /* Configure UART for DMX512 */
    config.baud_rate = DMX_BAUDRATE;    /* 250kbaud */
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_2;     /* DMX uses 2 stop bits */
    config.parity    = PARITY_NONE;
    config.bit_order = BIT_ORDER_LSB;
    config.invert    = NRZ_NORMAL;
    config.bufsz     = 1024;

    ret = rt_device_control(g_dmx.uart_dev, RT_DEVICE_CTRL_CONFIG, &config);
    if (ret != RT_EOK) {
        rt_kprintf("[DMX] ERROR: Failed to configure UART (ret=%d)\n", ret);
        return ret;
    }

    /* Open UART via RT-Thread (for init/config), but TX is done via direct register polling */
    ret = rt_device_open(g_dmx.uart_dev, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        rt_kprintf("[DMX] ERROR: Failed to open UART (ret=%d)\n", ret);
        return ret;
    }

    rt_kprintf("[DMX] UART3 opened successfully\n");

    /*
     * HARDWARE SYNC FIX: Force baud rate latch via DLAB toggle + DLL read.
     * Was added to fix 25Hz issue, but root cause was RMW on LCR (BREAK stuck).
     *
     * TODO: Now that uart_tx_poll() forces LCR=0x07 before every TX, this
     * block may be redundant. Test removing it once stability is confirmed.
     */
    {
        volatile struct UART_REG *reg = g_dmx.uart_hw;
        uint32_t lcr_save = reg->LCR;
        volatile uint32_t dummy;

        reg->LCR = lcr_save | 0x80;  /* Set DLAB */
        __asm__ volatile("dsb sy" ::: "memory");
        dummy = reg->DLL;            /* Dummy read for bus sync */
        (void)dummy;
        reg->LCR = lcr_save;         /* Clear DLAB */
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* Create mutex */
    g_dmx.mutex = rt_mutex_create("dmx_mtx", RT_IPC_FLAG_PRIO);
    if (!g_dmx.mutex) {
        rt_kprintf("[DMX] ERROR: Failed to create mutex\n");
        return -RT_ERROR;
    }

    /* Create DMX transmission thread */
    g_dmx.running = RT_TRUE;

    g_dmx.tx_thread = rt_thread_create("dmx_tx",
                                       dmx_tx_thread_entry,
                                       RT_NULL,
                                       2048,
                                       RT_THREAD_PRIORITY_MAX / 2,
                                       10);

    if (!g_dmx.tx_thread) {
        rt_kprintf("[DMX] ERROR: Failed to create TX thread\n");
        return -RT_ERROR;
    }

    rt_thread_startup(g_dmx.tx_thread);

    /* Initialize FPS tracking */
    g_dmx.last_fps_time = rt_tick_get();
    g_dmx.last_frame_count = 0;

#ifdef DMX_DEBUG_TEXT_MODE
    rt_kprintf("[DMX] *** DEBUG TEXT MODE ENABLED ***\n");
    rt_kprintf("[DMX] Will send 'HELLO\\n' every 100ms at 250kbaud\n");
#else
    rt_kprintf("[DMX] Driver initialized (250kbaud, 8N2, polling mode)\n");
    rt_kprintf("[DMX] UART3 TX = GPIO0_A4 (RM_IO4)\n");
    rt_kprintf("[DMX] Timing: BREAK=%dµs, MAB=%dµs (TIMER5 @ 24MHz)\n",
               g_dmx.break_us, g_dmx.mab_us);
    rt_kprintf("[DMX] TX thread running (100%% CPU2 OK - dedicated core)\n");
#endif

    return RT_EOK;
}

void dmx_enable(void)
{
    if (g_dmx.enabled) {
        return;  /* Already enabled */
    }

    rt_kprintf("[DMX] Enabling transmission (44 Hz)\n");

    g_dmx.enabled = RT_TRUE;
    g_dmx.frame_count = 0;
    g_dmx.last_fps_time = rt_tick_get();
    g_dmx.last_frame_count = 0;
}

void dmx_disable(void)
{
    if (!g_dmx.enabled) {
        return;
    }

    rt_kprintf("[DMX] Disabling transmission\n");
    g_dmx.enabled = RT_FALSE;
}

int dmx_set_channel(uint16_t channel, uint8_t value)
{
    if (channel >= DMX_UNIVERSE_SIZE) {
        return -RT_ERROR;
    }

    rt_mutex_take(g_dmx.mutex, RT_WAITING_FOREVER);
    g_dmx.channels[channel] = value;
    rt_mutex_release(g_dmx.mutex);

    return RT_EOK;
}

int dmx_set_channels(uint16_t start, const uint8_t *values, uint16_t count)
{
    if (start + count > DMX_UNIVERSE_SIZE) {
        return -RT_ERROR;
    }

    rt_mutex_take(g_dmx.mutex, RT_WAITING_FOREVER);
    rt_memcpy(&g_dmx.channels[start], values, count);
    rt_mutex_release(g_dmx.mutex);

    return RT_EOK;
}

void dmx_blackout(void)
{
    rt_mutex_take(g_dmx.mutex, RT_WAITING_FOREVER);
    rt_memset(g_dmx.channels, 0, DMX_UNIVERSE_SIZE);
    rt_mutex_release(g_dmx.mutex);

    rt_kprintf("[DMX] Blackout applied\n");
}

void dmx_get_status(dmx_driver_status_t *status)
{
    if (!status) {
        return;
    }

    status->enabled = g_dmx.enabled;
    status->frame_count = g_dmx.frame_count;
    status->fps = g_dmx.fps;
    status->errors = g_dmx.errors;
}

void dmx_send_frame_now(void)
{
    /* Not needed in simple version - thread handles everything */
    rt_kprintf("[DMX] Manual frame trigger (not needed in thread mode)\n");
}

int dmx_set_timing(uint16_t refresh_hz, uint16_t break_us, uint16_t mab_us)
{
    /* Validate ranges (0 = unchanged)
     * DMX512 spec (ANSI E1.11):
     *   Frame rate: Any rate is valid, max ~44Hz with 512 channels
     *   BREAK: TX min 92µs, RX must accept 88µs
     *   MAB:   TX min 12µs, RX must accept 8µs
     */
    if (refresh_hz != 0) {
        if (refresh_hz < DMX_REFRESH_HZ_MIN || refresh_hz > DMX_REFRESH_HZ_MAX) {
            rt_kprintf("[DMX] ERR: Invalid refresh %d Hz (range: %d-%d)\n",
                       refresh_hz, DMX_REFRESH_HZ_MIN, DMX_REFRESH_HZ_MAX);
            return -1;
        }
        g_dmx.refresh_hz = refresh_hz;
    }

    if (break_us != 0) {
        if (break_us < DMX_BREAK_US_MIN || break_us > DMX_BREAK_US_MAX) {
            rt_kprintf("[DMX] ERR: Invalid BREAK %dµs (range: %d-%d)\n",
                        break_us, DMX_BREAK_US_MIN, DMX_BREAK_US_MAX);
            return -1;
        }
        g_dmx.break_us = break_us;
    }

    if (mab_us != 0) {
        if (mab_us < DMX_MAB_US_MIN || mab_us > DMX_MAB_US_MAX) {
            rt_kprintf("[DMX] ERR: Invalid MAB %dµs (range: %d-%d)\n", 
                        mab_us, DMX_MAB_US_MIN, DMX_MAB_US_MAX);
            return -1;
        }
        g_dmx.mab_us = mab_us;
    }

    rt_kprintf("[DMX] Timing updated: %d Hz, BREAK=%dµs, MAB=%dµs\n",
               g_dmx.refresh_hz, g_dmx.break_us, g_dmx.mab_us);

    return 0;
}

void dmx_get_timing(uint16_t *refresh_hz, uint16_t *break_us, uint16_t *mab_us)
{
    if (refresh_hz) {
        *refresh_hz = g_dmx.refresh_hz;
    }
    if (break_us) {
        *break_us = g_dmx.break_us;
    }
    if (mab_us) {
        *mab_us = g_dmx.mab_us;
    }
}
