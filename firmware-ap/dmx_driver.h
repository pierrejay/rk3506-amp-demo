/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * DMX512 Driver for RT-Thread on RK3506.
 */

#ifndef DMX_DRIVER_H
#define DMX_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * DMX512 Constants
 * ============================================================================ */

#define DMX_UNIVERSE_SIZE    512    /* 512 channels standard DMX512 */
#define DMX_FRAME_SIZE       513    /* Start code (1) + channels (512) */

/* DMX512 break/MAB timing (microseconds) */
#define DMX_BREAK_US_DEFAULT 150    /* Break: 88-200µs (using 100µs) */
#define DMX_BREAK_US_MIN     88
#define DMX_BREAK_US_MAX     1000
#define DMX_MAB_US_DEFAULT   12     /* Mark After Break: 8-16µs (using 12µs) */
#define DMX_MAB_US_MIN       8
#define DMX_MAB_US_MAX       100

/* Frame rate */
#define DMX_REFRESH_HZ_DEFAULT  44  /* Default: 44 Hz = 22.7ms per frame */
#define DMX_REFRESH_HZ_MIN      1   /* Minimum: 1 Hz */
#define DMX_REFRESH_HZ_MAX      44  /* Maximum: 44 Hz (limited by 512ch frame time) */

/* ============================================================================
 * DMX Driver Status Structure
 * ============================================================================ */

typedef struct {
    bool enabled;           /* DMX transmission enabled */
    uint32_t frame_count;   /* Total frames transmitted */
    uint32_t fps;           /* Current FPS × 100 (e.g., 4400 = 44.00 Hz) */
    uint32_t errors;        /* Error count */
} dmx_driver_status_t;

/* ============================================================================
 * DMX API
 * ============================================================================ */

/**
 * Initialize DMX driver
 *
 * Sets up UART3, DMA, and timer
 *
 * Returns:
 *   0 on success
 *   -1 on error
 */
int dmx_init(void);

/**
 * Enable DMX transmission
 *
 * Starts periodic frame transmission at 44 Hz
 */
void dmx_enable(void);

/**
 * Disable DMX transmission
 *
 * Stops frame transmission
 */
void dmx_disable(void);

/**
 * Set single DMX channel
 *
 * Args:
 *   channel: Channel number (0-511)
 *   value: Channel value (0-255)
 *
 * Returns:
 *   0 on success
 *   -1 if channel out of range
 */
int dmx_set_channel(uint16_t channel, uint8_t value);

/**
 * Set multiple DMX channels
 *
 * Args:
 *   start: First channel (0-511)
 *   values: Array of channel values
 *   count: Number of channels to set
 *
 * Returns:
 *   0 on success
 *   -1 if range invalid
 */
int dmx_set_channels(uint16_t start, const uint8_t *values, uint16_t count);

/**
 * Blackout - Set all channels to 0
 */
void dmx_blackout(void);

/**
 * Get DMX status
 *
 * Args:
 *   status: Pointer to status structure to fill
 */
void dmx_get_status(dmx_driver_status_t *status);

/**
 * Send one DMX frame immediately (manual trigger)
 *
 * For testing/debugging
 */
void dmx_send_frame_now(void);

/**
 * Set DMX timing (frame rate, BREAK, MAB)
 *
 * Use 0 for any parameter to keep current value unchanged.
 *
 * Args:
 *   refresh_hz: Frame rate in Hz (1-44, 0=unchanged)
 *   break_us: BREAK duration in microseconds (88-1000, 0=unchanged)
 *   mab_us: MAB duration in microseconds (8-100, 0=unchanged)
 *
 * Returns:
 *   0 on success
 *   -1 if invalid range
 */
int dmx_set_timing(uint16_t refresh_hz, uint16_t break_us, uint16_t mab_us);

/**
 * Get current DMX timing
 *
 * Args:
 *   refresh_hz: Pointer to store frame rate (can be NULL)
 *   break_us: Pointer to store BREAK duration (can be NULL)
 *   mab_us: Pointer to store MAB duration (can be NULL)
 */
void dmx_get_timing(uint16_t *refresh_hz, uint16_t *break_us, uint16_t *mab_us);

#endif /* DMX_DRIVER_H */
