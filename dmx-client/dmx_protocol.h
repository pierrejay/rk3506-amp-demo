/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * DMX Protocol Definitions
 *
 * Binary protocol for RPMSG communication between Linux and RT-Thread/MCU.
 */

#ifndef DMX_PROTOCOL_H
#define DMX_PROTOCOL_H

#include <stdint.h>

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

#define DMX_MAGIC_CMD       0xAA    /* Magic byte for commands (Linux → RT-Thread) */
#define DMX_MAGIC_RESP      0xBB    /* Magic byte for responses (RT-Thread → Linux) */

#define DMX_MAX_CHANNELS    512     /* DMX512 standard */
#define DMX_MAX_PAYLOAD     1024    /* Max payload size */

/* ============================================================================
 * Command Types
 * ============================================================================ */

typedef enum {
    CMD_DMX_SET_CHANNELS    = 0x01,  /* Set DMX channel values */
    CMD_DMX_GET_STATUS      = 0x02,  /* Get DMX status */
    CMD_DMX_ENABLE          = 0x03,  /* Enable DMX transmission */
    CMD_DMX_DISABLE         = 0x04,  /* Disable DMX transmission */
    CMD_DMX_BLACKOUT        = 0x05,  /* Set all channels to 0 */
    CMD_DMX_SET_TIMING      = 0x06,  /* Set FPS/BREAK/MAB timing (Hz/µs) */
    CMD_DMX_GET_TIMING      = 0x07,  /* Get current timing config */
} dmx_cmd_type_t;

/* ============================================================================
 * Response Status Codes
 * ============================================================================ */

typedef enum {
    STATUS_OK               = 0x00,  /* Command executed successfully */
    STATUS_ERROR            = 0xFF,  /* Generic error */
    STATUS_INVALID_MAGIC    = 0x01,  /* Invalid magic byte */
    STATUS_INVALID_CHECKSUM = 0x02,  /* Checksum mismatch */
    STATUS_INVALID_CMD      = 0x03,  /* Unknown command */
    STATUS_INVALID_LENGTH   = 0x04,  /* Invalid payload length */
} dmx_status_t;

/* ============================================================================
 * Protocol Structures
 * ============================================================================ */

/*
 * Command packet structure (Linux → RT-Thread)
 *
 * Layout:
 *   [magic:1] [cmd:1] [length:2] [data:N] [checksum:1]
 *
 * Example:
 *   0xAA 0x01 0x0A 0x00 [10 bytes data] 0xXX
 */
typedef struct {
    uint8_t magic;          /* Magic byte (0xAA) */
    uint8_t cmd;            /* Command type (dmx_cmd_type_t) */
    uint16_t length;        /* Payload length (little-endian) */
    uint8_t data[];         /* Variable payload */
    /* uint8_t checksum follows data */
} __attribute__((packed)) dmx_cmd_t;

/*
 * Response packet structure (RT-Thread → Linux)
 *
 * Layout:
 *   [magic:1] [status:1] [length:2] [data:N] [checksum:1]
 *
 * Example:
 *   0xBB 0x00 0x04 0x00 [4 bytes data] 0xXX
 */
typedef struct {
    uint8_t magic;          /* Magic byte (0xBB) */
    uint8_t status;         /* Status code (dmx_status_t) */
    uint16_t length;        /* Payload length (little-endian) */
    uint8_t data[];         /* Variable payload */
    /* uint8_t checksum follows data */
} __attribute__((packed)) dmx_resp_t;

/* ============================================================================
 * Command Payloads
 * ============================================================================ */

/*
 * CMD_DMX_SET_CHANNELS payload
 *
 * Sets multiple DMX channels starting from channel_start
 *
 * Layout:
 *   [channel_start:2] [values:N]
 *
 * Example: Set channels 1-10 to [255, 128, ..., 0]
 *   channel_start = 0x0001 (little-endian: 0x01 0x00)
 *   values = [255, 128, 64, 32, 16, 8, 4, 2, 1, 0]
 */
typedef struct {
    uint16_t channel_start;  /* First channel (0-511, little-endian) */
    uint8_t values[];        /* Channel values (0-255) */
} __attribute__((packed)) dmx_set_channels_t;

/*
 * CMD_DMX_GET_STATUS response payload
 *
 * Returns current DMX status
 *
 * Layout:
 *   [enabled:1] [frame_count:4] [fps:4]
 */
typedef struct {
    uint8_t enabled;        /* 0=disabled, 1=enabled */
    uint32_t frame_count;   /* Total frames sent (little-endian) */
    uint32_t fps;           /* Frames per second × 100 (e.g., 4400 = 44.00 Hz) */
} __attribute__((packed)) dmx_status_payload_t;

/*
 * CMD_DMX_SET_TIMING payload
 *
 * Sets DMX timing: frame rate, BREAK, and MAB
 * Use 0 for any field to keep current value unchanged.
 *
 * Frame rate: 1-44 Hz (DMX512 spec allows any rate, 44 Hz is max with 512 channels)
 * BREAK: min 92µs (TX spec), receivers accept 88µs
 * MAB: min 12µs (TX spec), receivers accept 8µs
 *
 * Defaults: 44 Hz, BREAK=150µs, MAB=12µs
 *
 * Layout:
 *   [refresh_hz:2] [break_us:2] [mab_us:2]
 */
typedef struct {
    uint16_t refresh_hz;    /* Frame rate in Hz (1-44, 0=unchanged) */
    uint16_t break_us;      /* BREAK duration in µs (0=unchanged) */
    uint16_t mab_us;        /* MAB duration in µs (0=unchanged) */
} __attribute__((packed)) dmx_timing_t;

/*
 * CMD_DMX_GET_TIMING response payload
 *
 * Returns current timing configuration
 *
 * Layout:
 *   [refresh_hz:2] [break_us:2] [mab_us:2]
 */
typedef dmx_timing_t dmx_timing_payload_t;

/* ============================================================================
 * Helper Functions (inline for header-only)
 * ============================================================================ */

/*
 * Calculate XOR checksum
 *
 * Checksums entire packet except the checksum byte itself
 *
 * Usage:
 *   uint8_t sum = dmx_calc_checksum(buffer, length);
 */
static inline uint8_t dmx_calc_checksum(const uint8_t *data, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum ^= data[i];
    }
    return sum;
}

/*
 * Verify packet checksum
 *
 * Returns:
 *   1 if checksum valid
 *   0 if checksum invalid
 *
 * Usage:
 *   if (dmx_verify_checksum(buffer, total_length)) { ... }
 */
static inline int dmx_verify_checksum(const uint8_t *packet, uint16_t total_len)
{
    if (total_len < 5) return 0;  /* Minimum: magic(1) + cmd(1) + len(2) + checksum(1) */

    uint8_t expected = dmx_calc_checksum(packet, total_len - 1);
    uint8_t actual = packet[total_len - 1];

    return (expected == actual);
}

/* ============================================================================
 * Size Calculation Macros
 * ============================================================================ */

/* Total command packet size (header + payload + checksum) */
#define DMX_CMD_SIZE(payload_len) (sizeof(dmx_cmd_t) + (payload_len) + 1)

/* Total response packet size (header + payload + checksum) */
#define DMX_RESP_SIZE(payload_len) (sizeof(dmx_resp_t) + (payload_len) + 1)

#endif /* DMX_PROTOCOL_H */
