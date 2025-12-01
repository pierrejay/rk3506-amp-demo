/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * DMX Client - RPMSG bridge utility for Linux.
 *
 * Usage:
 *   ./dmx_client enable                    # Human-friendly output
 *   ./dmx_client enable --json             # JSON output for scripts
 *   ./dmx_client enable --quiet            # Minimal output (exit code only)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/select.h>
#include <termios.h>
#include "dmx_protocol.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define DEFAULT_DEV     "/dev/ttyRPMSG0"
#define TIMEOUT_MS      1000            /* Response timeout */

/* Device path (can be overridden with --device) */
static const char *g_device = DEFAULT_DEV;

/* Output formats */
typedef enum {
    OUTPUT_HUMAN,   /* Human-friendly (colors, emojis) */
    OUTPUT_JSON,    /* JSON for machine parsing */
    OUTPUT_QUIET    /* Minimal (exit code only) */
} OutputFormat;

/* Global output format */
static OutputFormat g_output_format = OUTPUT_HUMAN;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Get current time in microseconds
 */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/*
 * Wait for data with timeout using select()
 * Returns: 1 if data available, 0 on timeout, -1 on error
 */
static int wait_for_data(int fd, int timeout_ms)
{
    fd_set read_fds;
    struct timeval tv;

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    return select(fd + 1, &read_fds, NULL, NULL, &tv);
}

/*
 * Configure TTY for raw binary mode
 * Essential for binary protocols - prevents control char interpretation
 */
static int configure_tty_raw(int fd)
{
    struct termios tty;
    if (tcgetattr(fd, &tty) < 0) return -1;

    cfmakeraw(&tty);

    /* VMIN=1: read() blocks until at least 1 byte available */
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) < 0) return -1;
    return 0;
}

/*
 * Read exactly 'count' bytes via polling loop
 * Returns: number of bytes read (should equal count), or -1 on error/timeout
 */
static int read_exact(int fd, uint8_t *buf, int count, int timeout_ms)
{
    int total_read = 0;
    while (total_read < count) {
        int sel_ret = wait_for_data(fd, timeout_ms);
        if (sel_ret <= 0) return -1;  /* Timeout or error */

        int n = read(fd, buf + total_read, count - total_read);
        if (n < 0) return -1;
        if (n == 0) return total_read;  /* Unexpected EOF */

        total_read += n;
    }
    return total_read;
}

/*
 * Build command packet
 *
 * Returns: total packet size
 */
static int build_cmd_packet(uint8_t *buf, uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    buf[0] = DMX_MAGIC_CMD;
    buf[1] = cmd;
    buf[2] = payload_len & 0xFF;         /* Little-endian length */
    buf[3] = (payload_len >> 8) & 0xFF;

    if (payload_len > 0) {
        memcpy(&buf[4], payload, payload_len);
    }

    /* Calculate checksum (entire packet except checksum byte) */
    uint8_t checksum = dmx_calc_checksum(buf, 4 + payload_len);
    buf[4 + payload_len] = checksum;

    return 4 + payload_len + 1;  /* header + payload + checksum */
}

/*
 * Send command and receive response
 *
 * Returns: 0 on success, -1 on error
 */
static int send_cmd_and_recv(int fd, uint8_t cmd, const uint8_t *payload, uint16_t payload_len,
                             uint8_t *resp_buf, uint16_t resp_buf_size, uint16_t *resp_len)
{
    uint8_t tx_buf[DMX_MAX_PAYLOAD];
    int tx_len = build_cmd_packet(tx_buf, cmd, payload, payload_len);

    /* Send command */
    if (write(fd, tx_buf, tx_len) != tx_len) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: write failed: %s\n", strerror(errno));
        }
        return -1;
    }

    /* Read response header (4 bytes) with timeout */
    uint8_t hdr[4];
    int n = read_exact(fd, hdr, 4, TIMEOUT_MS);
    if (n < 4) {
        if (g_output_format == OUTPUT_JSON) {
            printf("{\"status\":\"error\",\"error\":\"Timeout waiting for response\"}\n");
        } else if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Timeout or incomplete header (got %d bytes)\n", n < 0 ? 0 : n);
        }
        return -1;
    }

    /* Verify magic */
    if (hdr[0] != DMX_MAGIC_RESP) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Invalid response magic 0x%02x (expected 0x%02x)\n",
                    hdr[0], DMX_MAGIC_RESP);
        }
        return -1;
    }

    uint8_t status = hdr[1];
    uint16_t data_len = hdr[2] | (hdr[3] << 8);

    /* Read payload if any */
    if (data_len > 0) {
        if (data_len > resp_buf_size) {
            if (g_output_format != OUTPUT_QUIET) {
                fprintf(stderr, "Error: Response data too large (%d > %d)\n", data_len, resp_buf_size);
            }
            return -1;
        }
        n = read_exact(fd, resp_buf, data_len, TIMEOUT_MS);
        if (n != data_len) {
            if (g_output_format != OUTPUT_QUIET) {
                fprintf(stderr, "Error: Incomplete payload (got %d, expected %d)\n", n < 0 ? 0 : n, data_len);
            }
            return -1;
        }
    }

    /* Read checksum */
    uint8_t checksum;
    if (read_exact(fd, &checksum, 1, TIMEOUT_MS) != 1) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Failed to read checksum\n");
        }
        return -1;
    }

    /* Verify checksum */
    uint8_t full_packet[4 + data_len + 1];
    memcpy(full_packet, hdr, 4);
    if (data_len > 0) memcpy(&full_packet[4], resp_buf, data_len);
    full_packet[4 + data_len] = checksum;

    if (!dmx_verify_checksum(full_packet, 4 + data_len + 1)) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Invalid checksum\n");
        }
        return -1;
    }

    /* Check status */
    if (status != STATUS_OK) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Command failed with status 0x%02x\n", status);
        }
        return -1;
    }

    *resp_len = data_len;
    return 0;
}

/* ============================================================================
 * API Functions
 * ============================================================================ */

/*
 * Enable DMX transmission
 */
static int dmx_enable(int fd)
{
    uint8_t resp_buf[16];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_ENABLE, NULL, 0, resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0) {
        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"enable\",\"latency_us\":%lu}\n", latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ DMX enabled (latency: %lu µs)\n", latency);
                break;
        }
    }

    return ret;
}

/*
 * Disable DMX transmission
 */
static int dmx_disable(int fd)
{
    uint8_t resp_buf[16];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_DISABLE, NULL, 0, resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0) {
        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"disable\",\"latency_us\":%lu}\n", latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ DMX disabled (latency: %lu µs)\n", latency);
                break;
        }
    }

    return ret;
}

/*
 * Set DMX channels
 */
static int dmx_set_channels(int fd, uint16_t start_channel, const uint8_t *values, uint16_t count)
{
    uint8_t payload[2 + DMX_MAX_CHANNELS];
    payload[0] = start_channel & 0xFF;
    payload[1] = (start_channel >> 8) & 0xFF;
    memcpy(&payload[2], values, count);

    uint8_t resp_buf[16];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_SET_CHANNELS, payload, 2 + count,
                                resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0) {
        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"set\",\"start_channel\":%d,\"count\":%d,\"latency_us\":%lu}\n",
                       start_channel, count, latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ Channels %d-%d set (latency: %lu µs)\n",
                       start_channel, start_channel + count - 1, latency);
                break;
        }
    }

    return ret;
}

/*
 * Get DMX status
 */
static int dmx_get_status(int fd)
{
    uint8_t resp_buf[sizeof(dmx_status_payload_t)];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_GET_STATUS, NULL, 0,
                                resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0 && resp_len == sizeof(dmx_status_payload_t)) {
        dmx_status_payload_t *status = (dmx_status_payload_t *)resp_buf;
        float fps = (float)status->fps / 100.0f;

        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"get_status\",\"enabled\":%s,\"frame_count\":%u,\"fps\":%.2f,\"latency_us\":%lu}\n",
                       status->enabled ? "true" : "false",
                       status->frame_count,
                       fps,
                       latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ DMX Status (latency: %lu µs):\n", latency);
                printf("   Enabled:      %s\n", status->enabled ? "Yes" : "No");
                printf("   Frame count:  %u\n", status->frame_count);
                printf("   FPS:          %.2f Hz\n", fps);
                break;
        }
    }

    return ret;
}

/*
 * Blackout (all channels to 0)
 */
static int dmx_blackout(int fd)
{
    uint8_t resp_buf[16];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_BLACKOUT, NULL, 0,
                                resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0) {
        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"blackout\",\"latency_us\":%lu}\n", latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ Blackout applied (latency: %lu µs)\n", latency);
                break;
        }
    }

    return ret;
}

/*
 * Set DMX timing (frame rate, BREAK, MAB)
 * Use 0 for any parameter to keep unchanged
 */
static int dmx_set_timing(int fd, uint16_t refresh_hz, uint16_t break_us, uint16_t mab_us)
{
    dmx_timing_t timing;
    timing.refresh_hz = refresh_hz;
    timing.break_us = break_us;
    timing.mab_us = mab_us;

    uint8_t resp_buf[16];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_SET_TIMING, (uint8_t *)&timing, sizeof(timing),
                                resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0) {
        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"set_timing\",\"refresh_hz\":%d,\"break_us\":%d,\"mab_us\":%d,\"latency_us\":%lu}\n",
                       refresh_hz, break_us, mab_us, latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ Timing set: %dHz, BREAK=%dµs, MAB=%dµs (0=unchanged) (latency: %lu µs)\n",
                       refresh_hz, break_us, mab_us, latency);
                break;
        }
    }

    return ret;
}

/*
 * Get DMX timing configuration
 */
static int dmx_get_timing(int fd)
{
    uint8_t resp_buf[sizeof(dmx_timing_payload_t)];
    uint16_t resp_len;

    uint64_t t0 = get_time_us();
    int ret = send_cmd_and_recv(fd, CMD_DMX_GET_TIMING, NULL, 0,
                                resp_buf, sizeof(resp_buf), &resp_len);
    uint64_t t1 = get_time_us();
    unsigned long latency = (unsigned long)(t1 - t0);

    if (ret == 0 && resp_len == sizeof(dmx_timing_payload_t)) {
        dmx_timing_payload_t *timing = (dmx_timing_payload_t *)resp_buf;

        switch (g_output_format) {
            case OUTPUT_JSON:
                printf("{\"status\":\"ok\",\"command\":\"get_timing\",\"refresh_hz\":%u,\"break_us\":%u,\"mab_us\":%u,\"latency_us\":%lu}\n",
                       timing->refresh_hz, timing->break_us, timing->mab_us, latency);
                break;
            case OUTPUT_QUIET:
                /* Silent success */
                break;
            case OUTPUT_HUMAN:
            default:
                printf("✅ DMX Timing (latency: %lu µs):\n", latency);
                printf("   Refresh: %u Hz\n", timing->refresh_hz);
                printf("   BREAK:   %u µs\n", timing->break_us);
                printf("   MAB:     %u µs\n", timing->mab_us);
                break;
        }
    }

    return ret;
}

/* ============================================================================
 * CLI Interface
 * ============================================================================ */

static void print_usage(const char *prog)
{
    printf("DMX512 Client CLI\n\n");

    printf("USAGE:\n");
    printf("  %s <command> [options] [--json|--quiet]\n\n", prog);

    printf("COMMANDS:\n");
    printf("  enable                          Enable DMX transmission\n");
    printf("  disable                         Disable DMX transmission\n");
    printf("  set <ch> <val>                  Set single channel (1-512, 0-255)\n");
    printf("  set <ch> <v1,v2,...>            Set multiple channels\n");
    printf("  status                          Get DMX status\n");
    printf("  blackout                        Set all channels to 0\n");
    printf("  timing [fps] [break] [mab]      Set timing (0=unchanged)\n");
    printf("  timing                          Get current timing config\n\n");

    printf("FLAGS:\n");
    printf("  -d, --device <path>             Device path (default: %s)\n", DEFAULT_DEV);
    printf("  --json                          Output JSON (for scripts/subprocess)\n");
    printf("  --quiet, -q                     Minimal output (exit code only)\n\n");

    printf("EXAMPLES:\n");
    printf("  # Human-friendly output (default)\n");
    printf("  %s enable\n", prog);
    printf("  %s set 1 255\n", prog);
    printf("  %s set 1 255,128,64,32,16\n\n", prog);

    printf("  # Use MCU universe (ttyRPMSG1)\n");
    printf("  %s -d /dev/ttyRPMSG1 status\n", prog);
    printf("  %s --device /dev/ttyRPMSG1 set 1 255\n\n", prog);

    printf("  # JSON output for scripts\n");
    printf("  %s enable --json\n", prog);
    printf("  %s status --json\n\n", prog);

    printf("  # Quiet mode (exit code only)\n");
    printf("  %s enable --quiet && echo Success\n\n", prog);

    printf("JSON RESPONSE FORMATS:\n");
    printf("  enable/disable/set/blackout:\n");
    printf("    {\"status\":\"ok\",\"command\":\"enable\",\"latency_us\":245}\n\n");

    printf("  status:\n");
    printf("    {\"status\":\"ok\",\"command\":\"get_status\",\"enabled\":true,\n");
    printf("     \"frame_count\":1523,\"fps\":44.00,\"latency_us\":238}\n\n");

    printf("  timing (get):\n");
    printf("    {\"status\":\"ok\",\"command\":\"get_timing\",\"break_us\":400,\n");
    printf("     \"mab_us\":40,\"latency_us\":251}\n\n");

    printf("  timing (set):\n");
    printf("    {\"status\":\"ok\",\"command\":\"set_timing\",\"break_us\":400,\n");
    printf("     \"mab_us\":40,\"latency_us\":247}\n\n");

    printf("EXIT CODES:\n");
    printf("  0   Success\n");
    printf("  1   Error (see stderr for details)\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse global flags (--json, --quiet, --device) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            g_output_format = OUTPUT_JSON;
            /* Remove flag from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            g_output_format = OUTPUT_QUIET;
            /* Remove flag from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if ((strcmp(argv[i], "--device") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            g_device = argv[i + 1];
            /* Remove flag and value from argv */
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        }
    }

    /* Check we still have a command */
    if (argc < 2) {
        if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: No command specified\n");
        }
        return 1;
    }

    /* Handle help before opening device */
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    /* Open RPMSG device */
    int fd = open(g_device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        if (g_output_format == OUTPUT_JSON) {
            printf("{\"status\":\"error\",\"error\":\"Failed to open %s: %s\"}\n",
                   g_device, strerror(errno));
        } else if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Failed to open %s: %s\n", g_device, strerror(errno));
        }
        return 1;
    }

    /* Configure TTY for raw binary mode */
    if (configure_tty_raw(fd) < 0) {
        if (g_output_format == OUTPUT_JSON) {
            printf("{\"status\":\"error\",\"error\":\"Failed to configure raw mode\"}\n");
        } else if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Failed to configure raw mode\n");
        }
        close(fd);
        return 1;
    }

    int ret = 0;
    const char *cmd = argv[1];

    if (strcmp(cmd, "enable") == 0) {
        ret = dmx_enable(fd);
    }
    else if (strcmp(cmd, "disable") == 0) {
        ret = dmx_disable(fd);
    }
    else if (strcmp(cmd, "status") == 0) {
        ret = dmx_get_status(fd);
    }
    else if (strcmp(cmd, "blackout") == 0) {
        ret = dmx_blackout(fd);
    }
    else if (strcmp(cmd, "timing") == 0) {
        if (argc >= 3) {
            /* Set timing: timing [fps] [break] [mab] - 0 means unchanged */
            int refresh_hz = atoi(argv[2]);
            int break_us = (argc >= 4) ? atoi(argv[3]) : 0;
            int mab_us = (argc >= 5) ? atoi(argv[4]) : 0;

            /* Validate ranges (0 = unchanged, skip validation) */
            if (refresh_hz != 0 && (refresh_hz < 1 || refresh_hz > 44)) {
                if (g_output_format == OUTPUT_JSON) {
                    printf("{\"status\":\"error\",\"error\":\"Refresh must be 1-44 Hz (or 0=unchanged)\"}\n");
                } else if (g_output_format != OUTPUT_QUIET) {
                    fprintf(stderr, "Error: Refresh must be 1-44 Hz (or 0=unchanged)\n");
                }
                ret = 1;
                goto cleanup;
            }
            if (break_us != 0 && (break_us < 88 || break_us > 1000)) {
                if (g_output_format == OUTPUT_JSON) {
                    printf("{\"status\":\"error\",\"error\":\"BREAK must be 88-1000 µs (or 0=unchanged)\"}\n");
                } else if (g_output_format != OUTPUT_QUIET) {
                    fprintf(stderr, "Error: BREAK must be 88-1000 µs (or 0=unchanged)\n");
                }
                ret = 1;
                goto cleanup;
            }
            if (mab_us != 0 && (mab_us < 8 || mab_us > 100)) {
                if (g_output_format == OUTPUT_JSON) {
                    printf("{\"status\":\"error\",\"error\":\"MAB must be 8-100 µs (or 0=unchanged)\"}\n");
                } else if (g_output_format != OUTPUT_QUIET) {
                    fprintf(stderr, "Error: MAB must be 8-100 µs (or 0=unchanged)\n");
                }
                ret = 1;
                goto cleanup;
            }

            ret = dmx_set_timing(fd, refresh_hz, break_us, mab_us);
        } else {
            /* Get timing: timing (no args) */
            ret = dmx_get_timing(fd);
        }
    }
    else if (strcmp(cmd, "set") == 0 && argc >= 4) {
        /* Parse channel number */
        int channel = atoi(argv[2]);
        if (channel < 1 || channel > DMX_MAX_CHANNELS) {
            if (g_output_format == OUTPUT_JSON) {
                printf("{\"status\":\"error\",\"error\":\"Channel must be 1-%d\"}\n", DMX_MAX_CHANNELS);
            } else if (g_output_format != OUTPUT_QUIET) {
                fprintf(stderr, "Error: Channel must be 1-%d\n", DMX_MAX_CHANNELS);
            }
            ret = 1;
            goto cleanup;
        }

        /* Parse values (comma-separated or single) */
        uint8_t values[DMX_MAX_CHANNELS];
        int count = 0;

        char *values_str = argv[3];
        char *token = strtok(values_str, ",");
        while (token && count < DMX_MAX_CHANNELS) {
            int val = atoi(token);
            if (val < 0 || val > 255) {
                if (g_output_format == OUTPUT_JSON) {
                    printf("{\"status\":\"error\",\"error\":\"Value must be 0-255\"}\n");
                } else if (g_output_format != OUTPUT_QUIET) {
                    fprintf(stderr, "Error: Value must be 0-255\n");
                }
                ret = 1;
                goto cleanup;
            }
            values[count++] = (uint8_t)val;
            token = strtok(NULL, ",");
        }

        ret = dmx_set_channels(fd, channel - 1, values, count);  /* 0-indexed internally */
    }
    else {
        if (g_output_format == OUTPUT_JSON) {
            printf("{\"status\":\"error\",\"error\":\"Unknown command: %s\"}\n", cmd);
        } else if (g_output_format != OUTPUT_QUIET) {
            fprintf(stderr, "Error: Unknown command '%s'\n", cmd);
            print_usage(argv[0]);
        }
        ret = 1;
    }

cleanup:
    close(fd);
    return ret;
}
