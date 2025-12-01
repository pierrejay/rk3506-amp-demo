/*
 * MCU DMX512 Gateway - Entry Point
 *
 * Copyright Pierre Jay (c) 2025
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Description:
 *   DMX512 gateway for MCU (Cortex-M0+) on RK3506.
 *   - RPMSG communication with Linux (/dev/ttyRPMSG1)
 *   - DMX512 output on UART2 (RM_IO6)
 *   - Debug output on UART5 (GPIO1_D2/D3)
 *
 *   Protocol 100% compatible with CPU2 (RT-Thread) implementation.
 */

#include "hal_bsp.h"
#include "hal_base.h"

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"

#include "dmx_protocol_mcu.h"
#include "dmx_driver.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define RPMSG_CHANNEL_NAME   "rpmsg-tty"
#define RPMSG_EPT_ADDR       0x3005U
#define MASTER_ID            0U
#define REMOTE_ID            4U
#define RPMSG_MEM_BASE       0x03c20000U

/* Buffers */
#define RX_BUF_SIZE          64    /* RPMSG RX ring buffer */
#define CMD_BUF_SIZE         600   /* Protocol parser buffer */

/* Timing */
#define TIMER_FREQ_MHZ       24    /* PLL_INPUT_OSC_RATE = 24MHz */
#define HEARTBEAT_INTERVAL   5000000  /* 5 seconds in microseconds */

/* ============================================================================
 * Global State
 * ============================================================================ */

/* RPMSG */
static struct rpmsg_lite_instance *g_rpmsg_instance = NULL;
static struct rpmsg_lite_endpoint *g_rpmsg_ept = NULL;

/* RPMSG RX ring buffer */
static volatile uint8_t g_rx_buf[RX_BUF_SIZE];
static volatile uint32_t g_rx_head = 0;
static volatile uint32_t g_rx_tail = 0;
static volatile uint32_t g_rx_src = 0;

/* Protocol parser */
typedef enum {
    PARSE_IDLE,
    PARSE_CMD,
    PARSE_LEN_LO,
    PARSE_LEN_HI,
    PARSE_DATA,
    PARSE_CHECKSUM
} parse_state_t;

static parse_state_t g_parse_state = PARSE_IDLE;
static uint8_t g_cmd_buf[CMD_BUF_SIZE];
static uint16_t g_cmd_idx = 0;
static uint16_t g_cmd_payload_len = 0;

/* Statistics */
static volatile uint32_t g_rx_count = 0;
static volatile uint32_t g_tx_count = 0;
static volatile uint32_t g_cmd_count = 0;

/* Timing - using HAL_GetSysTimerCount() @ 24MHz */
static inline uint64_t get_time_us(void)
{
    return HAL_GetSysTimerCount() / TIMER_FREQ_MHZ;
}

/* Debug UART */
static struct UART_REG *pUartDebug = UART5;

/* ============================================================================
 * Debug Output (UART5)
 * ============================================================================ */

#ifdef __GNUC__
__USED int _write(int fd, char *ptr, int len)
{
    int i = 0;
    if (fd > 2) return -1;
    while (*ptr && (i < len)) {
        if (*ptr == '\n') HAL_UART_SerialOutChar(pUartDebug, '\r');
        HAL_UART_SerialOutChar(pUartDebug, *ptr);
        i++;
        ptr++;
    }
    return i;
}
#endif

/* ============================================================================
 * RPMSG Response
 * ============================================================================ */

static void send_response(uint8_t status, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t resp[32];
    uint16_t total_len;

    if (g_rx_src == 0) {
        return;
    }

    resp[0] = DMX_MAGIC_RESP;
    resp[1] = status;
    resp[2] = payload_len & 0xFF;
    resp[3] = (payload_len >> 8) & 0xFF;

    if (payload_len > 0 && payload != NULL) {
        for (uint16_t i = 0; i < payload_len && i < sizeof(resp) - 5; i++) {
            resp[4 + i] = payload[i];
        }
    }

    total_len = 4 + payload_len;
    resp[total_len] = dmx_calc_checksum(resp, total_len);
    total_len++;

    int32_t ret = rpmsg_lite_send(g_rpmsg_instance, g_rpmsg_ept,
                                   g_rx_src, (char *)resp, total_len, RL_BLOCK);
    if (ret == RL_SUCCESS) {
        g_tx_count++;
    }
}

/* ============================================================================
 * Command Handlers
 * ============================================================================ */

static void handle_cmd_set_channels(const uint8_t *data, uint16_t len)
{
    if (len < 3) {
        printf("[CMD] SET_CHANNELS: too short\n");
        send_response(STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    uint16_t start = data[0] | (data[1] << 8);
    uint16_t count = len - 2;

    if (dmx_set_channels(start, &data[2], count) < 0) {
        printf("[CMD] SET_CHANNELS: error (start=%d, count=%d)\n", start, count);
        send_response(STATUS_ERROR, NULL, 0);
        return;
    }

    printf("[CMD] SET_CHANNELS: start=%d, count=%d\n", start, count);
    send_response(STATUS_OK, NULL, 0);
}

static void handle_cmd_get_status(void)
{
    dmx_driver_status_t st;
    dmx_get_status(&st);

    dmx_status_payload_t payload;
    payload.enabled = st.enabled;
    payload.frame_count = st.frame_count;
    payload.fps = st.refresh_hz * 100;

    printf("[CMD] GET_STATUS: en=%d, frames=%lu\n",
           st.enabled, (unsigned long)st.frame_count);

    send_response(STATUS_OK, (uint8_t *)&payload, sizeof(payload));
}

static void handle_cmd_enable(void)
{
    dmx_enable();
    printf("[CMD] ENABLE\n");
    send_response(STATUS_OK, NULL, 0);
}

static void handle_cmd_disable(void)
{
    dmx_disable();
    printf("[CMD] DISABLE\n");
    send_response(STATUS_OK, NULL, 0);
}

static void handle_cmd_blackout(void)
{
    dmx_blackout();
    printf("[CMD] BLACKOUT\n");
    send_response(STATUS_OK, NULL, 0);
}

static void handle_cmd_set_timing(const uint8_t *data, uint16_t len)
{
    if (len != sizeof(dmx_timing_t)) {
        printf("[CMD] SET_TIMING: bad length\n");
        send_response(STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    const dmx_timing_t *t = (const dmx_timing_t *)data;

    if (dmx_set_timing(t->refresh_hz, t->break_us, t->mab_us) < 0) {
        printf("[CMD] SET_TIMING: error\n");
        send_response(STATUS_ERROR, NULL, 0);
        return;
    }

    printf("[CMD] SET_TIMING: %dHz, brk=%dus, mab=%dus\n",
           t->refresh_hz, t->break_us, t->mab_us);
    send_response(STATUS_OK, NULL, 0);
}

static void handle_cmd_get_timing(void)
{
    dmx_timing_t t;
    dmx_get_timing(&t.refresh_hz, &t.break_us, &t.mab_us);

    printf("[CMD] GET_TIMING: %dHz, brk=%dus, mab=%dus\n",
           t.refresh_hz, t.break_us, t.mab_us);

    send_response(STATUS_OK, (uint8_t *)&t, sizeof(t));
}

static void handle_cmd_system_reset(const uint8_t *data, uint16_t len)
{
    /*
     * Graceful MCU reset for Linux shutdown/reboot.
     *
     * This command requires a 4-byte magic (0xDEADBEEF) to prevent
     * accidental resets. When received, the MCU will:
     * 1. Stop DMX transmission
     * 2. Disable all interrupts
     * 3. Perform hardware reset via NVIC
     *
     * The MCU restarts immediately from reset vector.
     *
     * NOTE: With SRAM, this causes issues because MCU restarts before
     * Linux finishes dying. With DDR-based firmware loading, this
     * should work because U-Boot reloads fresh code on each boot.
     */

    if (len != 4) {
        printf("[CMD] SYSTEM_RESET: bad length %d (expected 4)\n", len);
        send_response(STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    /* Extract magic (little-endian) */
    uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

    if (magic != SYSTEM_RESET_MAGIC) {
        printf("[CMD] SYSTEM_RESET: bad magic 0x%08lX\n", (unsigned long)magic);
        send_response(STATUS_ERROR, NULL, 0);
        return;
    }

    printf("[CMD] SYSTEM_RESET: Resetting MCU. Goodbye!\n");

    /* Send OK response before reset (best effort) */
    send_response(STATUS_OK, NULL, 0);

    /* Wait for printf and response to flush */
    HAL_DelayMs(50);

    /* Stop DMX transmission */
    dmx_disable();

    /* ========== POINT OF NO RETURN ========== */

    /* Disable all interrupts */
    __disable_irq();

    /* Hardware reset via NVIC */
    NVIC_SystemReset();

    /* Never reached */
    while (1) {
        __asm volatile ("nop");
    }
}

/* ============================================================================
 * Protocol Parser
 * ============================================================================ */

static void handle_complete_packet(void)
{
    uint8_t *buf = g_cmd_buf;
    uint16_t total_len = g_cmd_idx;

    g_cmd_count++;

    /* Verify magic */
    if (buf[0] != DMX_MAGIC_CMD) {
        printf("[PARSE] Bad magic: 0x%02x\n", buf[0]);
        send_response(STATUS_INVALID_MAGIC, NULL, 0);
        return;
    }

    /* Verify checksum */
    if (!dmx_verify_checksum(buf, total_len)) {
        printf("[PARSE] Bad checksum\n");
        send_response(STATUS_INVALID_CHECKSUM, NULL, 0);
        return;
    }

    uint8_t cmd = buf[1];
    uint16_t payload_len = buf[2] | (buf[3] << 8);
    uint8_t *payload = (payload_len > 0) ? &buf[4] : NULL;

    switch (cmd) {
        case CMD_DMX_SET_CHANNELS:
            handle_cmd_set_channels(payload, payload_len);
            break;
        case CMD_DMX_GET_STATUS:
            handle_cmd_get_status();
            break;
        case CMD_DMX_ENABLE:
            handle_cmd_enable();
            break;
        case CMD_DMX_DISABLE:
            handle_cmd_disable();
            break;
        case CMD_DMX_BLACKOUT:
            handle_cmd_blackout();
            break;
        case CMD_DMX_SET_TIMING:
            handle_cmd_set_timing(payload, payload_len);
            break;
        case CMD_DMX_GET_TIMING:
            handle_cmd_get_timing();
            break;
        case CMD_SYSTEM_RESET:
            handle_cmd_system_reset(payload, payload_len);
            break;
        default:
            printf("[PARSE] Unknown cmd: 0x%02x\n", cmd);
            send_response(STATUS_INVALID_CMD, NULL, 0);
            break;
    }
}

static void parse_rx_byte(uint8_t byte)
{
    switch (g_parse_state) {
        case PARSE_IDLE:
            if (byte == DMX_MAGIC_CMD) {
                g_cmd_buf[0] = byte;
                g_cmd_idx = 1;
                g_parse_state = PARSE_CMD;
            }
            break;

        case PARSE_CMD:
            g_cmd_buf[g_cmd_idx++] = byte;
            g_parse_state = PARSE_LEN_LO;
            break;

        case PARSE_LEN_LO:
            g_cmd_buf[g_cmd_idx++] = byte;
            g_cmd_payload_len = byte;
            g_parse_state = PARSE_LEN_HI;
            break;

        case PARSE_LEN_HI:
            g_cmd_buf[g_cmd_idx++] = byte;
            g_cmd_payload_len |= (byte << 8);

            if (g_cmd_payload_len > CMD_BUF_SIZE - 5) {
                printf("[PARSE] Payload too large: %d\n", g_cmd_payload_len);
                g_parse_state = PARSE_IDLE;
            } else if (g_cmd_payload_len == 0) {
                g_parse_state = PARSE_CHECKSUM;
            } else {
                g_parse_state = PARSE_DATA;
            }
            break;

        case PARSE_DATA:
            g_cmd_buf[g_cmd_idx++] = byte;
            if (g_cmd_idx >= 4 + g_cmd_payload_len) {
                g_parse_state = PARSE_CHECKSUM;
            }
            break;

        case PARSE_CHECKSUM:
            g_cmd_buf[g_cmd_idx++] = byte;
            handle_complete_packet();
            g_parse_state = PARSE_IDLE;
            break;
    }
}

/* ============================================================================
 * RPMSG Callback (IRQ context)
 * ============================================================================ */

static int32_t rpmsg_rx_callback(void *payload, uint32_t payload_len,
                                  uint32_t src, void *priv)
{
    (void)priv;

    g_rx_src = src;

    for (uint32_t i = 0; i < payload_len; i++) {
        uint32_t next = (g_rx_head + 1) % RX_BUF_SIZE;
        if (next != g_rx_tail) {
            g_rx_buf[g_rx_head] = ((uint8_t*)payload)[i];
            g_rx_head = next;
            g_rx_count++;
        }
    }
    return RL_RELEASE;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    struct HAL_UART_CONFIG uart5_config = {
        .baudRate = UART_BR_115200,
        .dataBit = UART_DATA_8B,
        .stopBit = UART_ONE_STOPBIT,
        .parity = UART_PARITY_DISABLE,
    };
    struct HAL_UART_CONFIG uart2_config = {
        .baudRate = 250000,
        .dataBit = UART_DATA_8B,
        .stopBit = UART_ONE_AND_HALF_OR_TWO_STOPBIT,
        .parity = UART_PARITY_DISABLE,
    };
    uint32_t link_id;
    uint64_t last_heartbeat_time = 0;

    /* HAL/BSP Init */
    HAL_Init();
    BSP_Init();
    HAL_INTMUX_Init();

    /* UART5 Init for debug */
    HAL_PINCTRL_SetIOMUX(GPIO_BANK1, GPIO_PIN_D2, PIN_CONFIG_MUX_FUNC6);
    HAL_PINCTRL_SetIOMUX(GPIO_BANK1, GPIO_PIN_D3, PIN_CONFIG_MUX_FUNC6);
    HAL_UART_Init(&g_uart5Dev, &uart5_config);

    /* UART2 Init for DMX (pinctrl done by Linux DTS) */
    HAL_UART_Init(&g_uart2Dev, &uart2_config);

    /* DMX driver init */
    dmx_init();

    printf("\n");
    printf("========================================\n");
    printf("  DMX512 Gateway on RK3506 MCU\n");
    printf("  Debug: UART5 @ 115200\n");
    printf("  DMX:   UART2 @ 250000\n");
    printf("========================================\n");

    /* RPMSG init */
    link_id = RL_PLATFORM_SET_LINK_ID(MASTER_ID, REMOTE_ID);

    g_rpmsg_instance = rpmsg_lite_remote_init(
        (void *)RPMSG_MEM_BASE,
        link_id,
        RL_NO_FLAGS
    );

    if (!g_rpmsg_instance) {
        printf("[ERR] RPMSG init failed\n");
        goto fallback;
    }

    printf("[MCU] Waiting for Linux...\n");
    int link_check = 0;
    while (!rpmsg_lite_is_link_up(g_rpmsg_instance)) {
        link_check++;
        if ((link_check % 10) == 0) printf(".");
        HAL_DelayUs(500000);
        if (link_check > 60) {
            printf("\n[ERR] Link timeout\n");
            goto fallback;
        }
    }
    printf("\n[MCU] Link UP\n");

    g_rpmsg_ept = rpmsg_lite_create_ept(
        g_rpmsg_instance,
        RPMSG_EPT_ADDR,
        rpmsg_rx_callback,
        NULL
    );

    if (!g_rpmsg_ept) {
        printf("[ERR] Endpoint failed\n");
        goto fallback;
    }

    rpmsg_ns_announce(g_rpmsg_instance, g_rpmsg_ept,
                      RPMSG_CHANNEL_NAME, RL_NS_CREATE);

    printf("[MCU] Channel '%s' ready\n", RPMSG_CHANNEL_NAME);
    printf("[MCU] Waiting for commands...\n\n");

    /* Main loop - non-blocking, polls DMX state machine
     * Note: CPU runs at 100% - for power optimization, implement WFI with timer IRQ
     */
    while (1) {
        uint64_t now = get_time_us();

        /* 1. Process pending RPMSG bytes */
        while (g_rx_head != g_rx_tail) {
            uint8_t c;

            __disable_irq();
            c = g_rx_buf[g_rx_tail];
            g_rx_tail = (g_rx_tail + 1) % RX_BUF_SIZE;
            __enable_irq();

            parse_rx_byte(c);
        }

        /* 2. Poll DMX TX state machine (non-blocking) */
        dmx_poll(now);

        /* 3. Heartbeat every 5 seconds */
        if (now - last_heartbeat_time >= HEARTBEAT_INTERVAL) {
            printf("[HB] rx=%lu tx=%lu cmd=%lu dmx=%lu\n",
                   (unsigned long)g_rx_count,
                   (unsigned long)g_tx_count,
                   (unsigned long)g_cmd_count,
                   (unsigned long)dmx_get_frame_count());
            last_heartbeat_time = now;
        }
    }

fallback:
    printf("\n[ERR] Fallback mode - DMX only\n");
    while (1) {
        uint64_t now = get_time_us();
        dmx_poll(now);
        if ((now % 1000000) < 1000) {  /* ~1ms window every second */
            printf("[FB] dmx=%lu\n", (unsigned long)dmx_get_frame_count());
        }
    }

    return 0;
}

int entry(void)
{
    return main();
}
