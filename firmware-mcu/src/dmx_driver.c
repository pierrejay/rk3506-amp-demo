/*
 * DMX512 Driver for MCU (Cortex-M0+)
 *
 * Copyright Pierre Jay (c) 2025
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dmx_driver.h"
#include "hal_base.h"

/* ============================================================================
 * UART Register Bits (not all defined in HAL)
 * ============================================================================ */

#define UART_LCR_BREAK   (1 << 6)   /* Break control bit */
#define UART_USR_TFE     (1 << 2)   /* TX FIFO empty */
/* Note: UART_USR_BUSY is already defined in hal_uart.h */

/* ============================================================================
 * Private State
 * ============================================================================ */

static struct UART_REG *pUart = UART2;

static uint8_t g_dmx_frame[DMX_FRAME_SIZE];
static volatile uint32_t g_frame_count = 0;
static volatile uint8_t g_enabled = 0;  /* Start disabled, enable via CMD_DMX_ENABLE */

static uint16_t g_refresh_hz = DMX_DEFAULT_REFRESH_HZ;
static uint16_t g_break_us = DMX_DEFAULT_BREAK_US;
static uint16_t g_mab_us = DMX_DEFAULT_MAB_US;

/* Non-blocking TX state machine */
typedef enum {
    DMX_STATE_IDLE,
    DMX_STATE_TX_DATA
} dmx_tx_state_t;

static dmx_tx_state_t g_dmx_state = DMX_STATE_IDLE;
static uint16_t g_tx_idx = 0;
static uint64_t g_last_frame_time = 0;

/* ============================================================================
 * Private Functions
 * ============================================================================ */

/**
 * Wait for UART TX to be completely idle
 */
static void uart_wait_idle(void)
{
    int timeout = 100000;
    while (timeout--) {
        uint32_t usr = pUart->USR;
        if ((usr & UART_USR_TFE) && !(usr & UART_USR_BUSY)) {
            return;
        }
    }
}

/**
 * Send DMX Break + Mark After Break
 * Uses LCR register to force TX low (break condition)
 */
static void send_break_mab(void)
{
    uint32_t lcr;

    __disable_irq();

    /* Set break (force TX low) */
    lcr = pUart->LCR;
    pUart->LCR = lcr | UART_LCR_BREAK;
    __DSB();

    /* Hold break */
    HAL_DelayUs(g_break_us);

    /* Clear break (release TX) */
    pUart->LCR = lcr;
    __DSB();

    /* Mark After Break */
    HAL_DelayUs(g_mab_us);

    __enable_irq();
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void dmx_init(void)
{
    /* Initialize frame to 0 (start code + all channels) */
    for (int i = 0; i < DMX_FRAME_SIZE; i++) {
        g_dmx_frame[i] = 0;
    }

    g_frame_count = 0;
    g_enabled = 0;  /* Start disabled - user must call dmx enable */
}

void dmx_enable(void)
{
    if (g_enabled) {
        return;  /* Already enabled */
    }
    g_enabled = 1;
    g_frame_count = 0;
}

void dmx_disable(void)
{
    if (!g_enabled) {
        return;  /* Already disabled */
    }
    g_enabled = 0;
}

uint8_t dmx_is_enabled(void)
{
    return g_enabled;
}

int dmx_set_channels(uint16_t start_channel, const uint8_t *values, uint16_t count)
{
    if (start_channel >= DMX_UNIVERSE_SIZE) {
        return -1;
    }
    if (start_channel + count > DMX_UNIVERSE_SIZE) {
        return -1;
    }
    if (values == NULL || count == 0) {
        return -1;
    }

    /* Frame layout: [0]=start_code, [1-512]=channels */
    for (uint16_t i = 0; i < count; i++) {
        g_dmx_frame[start_channel + 1 + i] = values[i];
    }

    return 0;
}

uint8_t dmx_get_channel(uint16_t channel)
{
    if (channel >= DMX_UNIVERSE_SIZE) {
        return 0;
    }
    return g_dmx_frame[channel + 1];
}

void dmx_blackout(void)
{
    /* Keep start code at 0, set all channels to 0 */
    for (int i = 1; i < DMX_FRAME_SIZE; i++) {
        g_dmx_frame[i] = 0;
    }
}

int dmx_set_timing(uint16_t refresh_hz, uint16_t break_us, uint16_t mab_us)
{
    if (refresh_hz > 0) {
        if (refresh_hz > 44) {
            return -1;  /* Max 44Hz for full 512 channels */
        }
        g_refresh_hz = refresh_hz;
    }

    if (break_us > 0) {
        if (break_us < 88 || break_us > 1000) {
            return -1;  /* Sanity check */
        }
        g_break_us = break_us;
    }

    if (mab_us > 0) {
        if (mab_us < 8 || mab_us > 1000) {
            return -1;  /* Sanity check */
        }
        g_mab_us = mab_us;
    }

    return 0;
}

void dmx_get_timing(uint16_t *refresh_hz, uint16_t *break_us, uint16_t *mab_us)
{
    if (refresh_hz) *refresh_hz = g_refresh_hz;
    if (break_us) *break_us = g_break_us;
    if (mab_us) *mab_us = g_mab_us;
}

void dmx_get_status(dmx_driver_status_t *status)
{
    if (status == NULL) return;

    status->enabled = g_enabled;
    status->frame_count = g_frame_count;
    status->refresh_hz = g_refresh_hz;
    status->break_us = g_break_us;
    status->mab_us = g_mab_us;
}

uint32_t dmx_get_frame_interval_us(void)
{
    if (g_refresh_hz == 0) return 1000000;  /* 1Hz fallback */
    return 1000000 / g_refresh_hz;
}

void dmx_poll(uint64_t now)
{
    if (!g_enabled) {
        return;
    }

    switch (g_dmx_state) {
        case DMX_STATE_IDLE:
            if (now - g_last_frame_time >= dmx_get_frame_interval_us()) {
                /* Wait for previous TX to complete */
                uart_wait_idle();
                /* Atomic break + MAB (~162µs) */
                send_break_mab();
                g_tx_idx = 0;
                g_dmx_state = DMX_STATE_TX_DATA;
                /* FALL THROUGH - start TX immediately */
            } else {
                break;
            }
            /* fall through */

        case DMX_STATE_TX_DATA:
            /* Stuff FIFO while there's room (64-byte FIFO) */
            while (g_tx_idx < DMX_FRAME_SIZE &&
                   (pUart->USR & UART_USR_TX_FIFO_NOT_FULL)) {
                pUart->THR = g_dmx_frame[g_tx_idx++];
            }
            /* Frame complete? */
            if (g_tx_idx >= DMX_FRAME_SIZE) {
                g_frame_count++;
                g_last_frame_time = now;
                g_dmx_state = DMX_STATE_IDLE;
            }
            break;
    }
}

uint8_t dmx_is_busy(void)
{
    return (g_dmx_state != DMX_STATE_IDLE) ? 1 : 0;
}

uint32_t dmx_get_frame_count(void)
{
    return g_frame_count;
}
