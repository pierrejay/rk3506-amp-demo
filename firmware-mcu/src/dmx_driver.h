/*
 * DMX512 Driver for MCU (Cortex-M0+)
 *
 * Copyright Pierre Jay (c) 2025
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Description:
 *   Low-level DMX512 driver using UART2 on RK3506 MCU.
 *   Generates break/MAB via LCR register manipulation.
 */

#ifndef DMX_DRIVER_H
#define DMX_DRIVER_H

#include <stdint.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define DMX_UNIVERSE_SIZE    512
#define DMX_FRAME_SIZE       513   /* Start code (1) + channels (512) */

/* Default timing */
#define DMX_DEFAULT_REFRESH_HZ  44
#define DMX_DEFAULT_BREAK_US    150   /* Spec: 92-176us */
#define DMX_DEFAULT_MAB_US      12    /* Spec: >8us */

/* ============================================================================
 * Status Structure
 * ============================================================================ */

typedef struct {
    uint8_t enabled;
    uint32_t frame_count;
    uint16_t refresh_hz;
    uint16_t break_us;
    uint16_t mab_us;
} dmx_driver_status_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize DMX driver
 * Must be called after UART2 is initialized
 */
void dmx_init(void);

/**
 * Enable DMX output
 */
void dmx_enable(void);

/**
 * Disable DMX output
 */
void dmx_disable(void);

/**
 * Check if DMX output is enabled
 */
uint8_t dmx_is_enabled(void);

/**
 * Set channel values
 * @param start_channel First channel (0-511)
 * @param values Array of values
 * @param count Number of channels to set
 * @return 0 on success, -1 on error
 */
int dmx_set_channels(uint16_t start_channel, const uint8_t *values, uint16_t count);

/**
 * Get single channel value
 * @param channel Channel number (0-511)
 * @return Channel value (0-255)
 */
uint8_t dmx_get_channel(uint16_t channel);

/**
 * Set all channels to 0 (blackout)
 */
void dmx_blackout(void);

/**
 * Set timing parameters
 * @param refresh_hz Refresh rate (1-44 Hz, 0 = unchanged)
 * @param break_us Break duration in us (0 = unchanged)
 * @param mab_us MAB duration in us (0 = unchanged)
 * @return 0 on success, -1 on error
 */
int dmx_set_timing(uint16_t refresh_hz, uint16_t break_us, uint16_t mab_us);

/**
 * Get timing parameters
 */
void dmx_get_timing(uint16_t *refresh_hz, uint16_t *break_us, uint16_t *mab_us);

/**
 * Get DMX status
 */
void dmx_get_status(dmx_driver_status_t *status);

/**
 * Get frame interval in microseconds
 */
uint32_t dmx_get_frame_interval_us(void);

/**
 * Poll DMX TX state machine (non-blocking)
 * Call from main loop. Handles:
 *   - Frame timing check
 *   - Break+MAB (atomic, ~162µs with IRQ disabled)
 *   - FIFO stuffing (non-blocking, fills 64-byte FIFO)
 *
 * @param now Current time in microseconds
 */
void dmx_poll(uint64_t now);

/**
 * Check if DMX TX is busy (frame in progress)
 */
uint8_t dmx_is_busy(void);

/**
 * Get frame count
 */
uint32_t dmx_get_frame_count(void);

#endif /* DMX_DRIVER_H */
