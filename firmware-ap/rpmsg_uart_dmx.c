/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * RPMSG protocol handler for DMX512 commands.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "hal_base.h"

#ifdef RT_USING_LINUX_RPMSG

#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_ns.h"

/* Include protocol and DMX driver */
#include "dmx_protocol.h"
#include "dmx_driver.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define RPMSG_CHANNEL_NAME   "rpmsg-tty"
#define RPMSG_EPT_ADDR       0x3004U
#define MASTER_ID            ((uint32_t)0)

extern uint32_t __linux_share_rpmsg_start__[];
extern uint32_t __linux_share_rpmsg_end__[];

#define RPMSG_MEM_BASE ((uint32_t)&__linux_share_rpmsg_start__)
#define RPMSG_MEM_END  ((uint32_t)&__linux_share_rpmsg_end__)
#define RPMSG_MEM_SIZE (2UL * RL_VRING_OVERHEAD)

/* ============================================================================
 * Global Variables
 * ============================================================================ */

static struct rpmsg_lite_instance *g_rpmsg_instance = NULL;
static struct rpmsg_lite_endpoint *g_rpmsg_ept = NULL;
static rpmsg_queue_handle g_rpmsg_queue = NULL;

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

static void send_response(uint32_t dst_addr, uint8_t status, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t resp_buf[DMX_MAX_PAYLOAD];

    resp_buf[0] = DMX_MAGIC_RESP;
    resp_buf[1] = status;
    resp_buf[2] = payload_len & 0xFF;
    resp_buf[3] = (payload_len >> 8) & 0xFF;

    if (payload_len > 0 && payload) {
        rt_memcpy(&resp_buf[4], payload, payload_len);
    }

    uint8_t checksum = dmx_calc_checksum(resp_buf, 4 + payload_len);
    resp_buf[4 + payload_len] = checksum;

    int total_len = 4 + payload_len + 1;
    int ret = rpmsg_lite_send(g_rpmsg_instance, g_rpmsg_ept, dst_addr,
                              resp_buf, total_len, RL_BLOCK);

    if (ret == RL_SUCCESS) {
        rt_kprintf("[RPMSG] Sent response: status=0x%02x, len=%d\n", status, total_len);
    } else {
        rt_kprintf("[RPMSG] ERROR: Failed to send response (ret=%d)\n", ret);
    }
}

/* ============================================================================
 * Command Handlers - Phase 3 (Real DMX)
 * ============================================================================ */

static void handle_cmd_enable(uint32_t dst_addr)
{
    rt_kprintf("[DMX] ENABLE command\n");
    dmx_enable();
    send_response(dst_addr, STATUS_OK, NULL, 0);
}

static void handle_cmd_disable(uint32_t dst_addr)
{
    rt_kprintf("[DMX] DISABLE command\n");
    dmx_disable();
    send_response(dst_addr, STATUS_OK, NULL, 0);
}

static void handle_cmd_set_channels(uint32_t dst_addr, const uint8_t *data, uint16_t len)
{
    if (len < 2) {
        rt_kprintf("[DMX] ERROR: SET_CHANNELS payload too short\n");
        send_response(dst_addr, STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    uint16_t start_channel = data[0] | (data[1] << 8);
    uint16_t count = len - 2;

    if (start_channel + count > DMX_MAX_CHANNELS) {
        rt_kprintf("[DMX] ERROR: Channel range out of bounds\n");
        send_response(dst_addr, STATUS_ERROR, NULL, 0);
        return;
    }

    rt_kprintf("[DMX] SET_CHANNELS: start=%d, count=%d\n", start_channel, count);

    /* Update DMX driver */
    if (dmx_set_channels(start_channel, &data[2], count) < 0) {
        rt_kprintf("[DMX] ERROR: dmx_set_channels failed\n");
        send_response(dst_addr, STATUS_ERROR, NULL, 0);
        return;
    }

    send_response(dst_addr, STATUS_OK, NULL, 0);
}

static void handle_cmd_get_status(uint32_t dst_addr)
{
    rt_kprintf("[DMX] GET_STATUS command\n");

    dmx_driver_status_t dmx_st;
    dmx_get_status(&dmx_st);

    dmx_status_payload_t status;
    status.enabled = dmx_st.enabled ? 1 : 0;
    status.frame_count = dmx_st.frame_count;
    status.fps = dmx_st.fps;

    send_response(dst_addr, STATUS_OK, (uint8_t *)&status, sizeof(status));
}

static void handle_cmd_blackout(uint32_t dst_addr)
{
    rt_kprintf("[DMX] BLACKOUT command\n");
    dmx_blackout();
    send_response(dst_addr, STATUS_OK, NULL, 0);
}

static void handle_cmd_set_timing(uint32_t dst_addr, uint8_t *payload, uint16_t len)
{
    if (len != sizeof(dmx_timing_t)) {
        rt_kprintf("[DMX] ERR: Invalid SET_TIMING payload length %d (expected %d)\n",
                   len, sizeof(dmx_timing_t));
        send_response(dst_addr, STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    dmx_timing_t *timing = (dmx_timing_t *)payload;
    rt_kprintf("[DMX] SET_TIMING: %dHz, BREAK=%dµs, MAB=%dµs (0=unchanged)\n",
               timing->refresh_hz, timing->break_us, timing->mab_us);

    int ret = dmx_set_timing(timing->refresh_hz, timing->break_us, timing->mab_us);
    if (ret != 0) {
        send_response(dst_addr, STATUS_ERROR, NULL, 0);
    } else {
        send_response(dst_addr, STATUS_OK, NULL, 0);
    }
}

static void handle_cmd_get_timing(uint32_t dst_addr)
{
    dmx_timing_payload_t timing;

    dmx_get_timing(&timing.refresh_hz, &timing.break_us, &timing.mab_us);

    rt_kprintf("[DMX] GET_TIMING: %dHz, BREAK=%dµs, MAB=%dµs\n",
               timing.refresh_hz, timing.break_us, timing.mab_us);

    send_response(dst_addr, STATUS_OK, (uint8_t *)&timing, sizeof(timing));
}

/* ============================================================================
 * Command Parser
 * ============================================================================ */

static void parse_command(uint8_t *rx_buf, uint32_t rx_len, uint32_t src_addr)
{
    /* Minimum packet: magic(1) + cmd(1) + len(2) + checksum(1) = 5 bytes */
    if (rx_len < 5) {
        rt_kprintf("[RPMSG] ERROR: Packet too short (%d bytes)\n", rx_len);
        send_response(src_addr, STATUS_INVALID_LENGTH, NULL, 0);
        return;
    }

    /* Verify magic byte */
    if (rx_buf[0] != DMX_MAGIC_CMD) {
        rt_kprintf("[RPMSG] ERROR: Invalid magic 0x%02x (expected 0x%02x)\n",
                   rx_buf[0], DMX_MAGIC_CMD);
        send_response(src_addr, STATUS_INVALID_MAGIC, NULL, 0);
        return;
    }

    /* Verify checksum */
    if (!dmx_verify_checksum(rx_buf, rx_len)) {
        rt_kprintf("[RPMSG] ERROR: Invalid checksum\n");
        send_response(src_addr, STATUS_INVALID_CHECKSUM, NULL, 0);
        return;
    }

    /* Extract command and payload */
    uint8_t cmd = rx_buf[1];
    uint16_t payload_len = rx_buf[2] | (rx_buf[3] << 8);
    uint8_t *payload = (payload_len > 0) ? &rx_buf[4] : NULL;

    rt_kprintf("[RPMSG] CMD: 0x%02x, payload_len=%d\n", cmd, payload_len);

    /* Dispatch command */
    switch (cmd) {
        case CMD_DMX_ENABLE:
            handle_cmd_enable(src_addr);
            break;

        case CMD_DMX_DISABLE:
            handle_cmd_disable(src_addr);
            break;

        case CMD_DMX_SET_CHANNELS:
            handle_cmd_set_channels(src_addr, payload, payload_len);
            break;

        case CMD_DMX_GET_STATUS:
            handle_cmd_get_status(src_addr);
            break;

        case CMD_DMX_BLACKOUT:
            handle_cmd_blackout(src_addr);
            break;

        case CMD_DMX_SET_TIMING:
            handle_cmd_set_timing(src_addr, payload, payload_len);
            break;

        case CMD_DMX_GET_TIMING:
            handle_cmd_get_timing(src_addr);
            break;

        default:
            rt_kprintf("[RPMSG] ERROR: Unknown command 0x%02x\n", cmd);
            send_response(src_addr, STATUS_INVALID_CMD, NULL, 0);
            break;
    }
}

/* ============================================================================
 * RPMSG Reception Thread
 * ============================================================================ */

static void rpmsg_recv_thread(void *parameter)
{
    char *rx_buf;
    uint32_t rx_len;
    uint32_t src_addr;
    int ret;

    rt_kprintf("[RPMSG] Reception thread started\n");

    rx_buf = (char *)rt_malloc(RL_BUFFER_PAYLOAD_SIZE);
    if (!rx_buf) {
        rt_kprintf("[RPMSG] ERROR: Failed to allocate RX buffer\n");
        return;
    }

    while (1) {
        ret = rpmsg_queue_recv(g_rpmsg_instance, g_rpmsg_queue,
                               &src_addr, rx_buf, RL_BUFFER_PAYLOAD_SIZE,
                               &rx_len, RL_BLOCK);

        if (ret == RL_SUCCESS) {
            rt_kprintf("[RPMSG] RX %d bytes from 0x%x\n", rx_len, src_addr);
            parse_command((uint8_t *)rx_buf, rx_len, src_addr);
        }
    }

    rt_free(rx_buf);
}

/* ============================================================================
 * RPMSG Name Service Callback
 * ============================================================================ */

static void rpmsg_ns_callback(uint32_t new_ept, const char *new_ept_name,
                              uint32_t flags, void *user_data)
{
    rt_kprintf("[RPMSG] Name service: new_ept=0x%x name=%s\n", new_ept, new_ept_name);
}

/* ============================================================================
 * RPMSG Init
 * ============================================================================ */

static int rpmsg_dmx_init(void)
{
    uint32_t master_id, remote_id, link_id;
    rt_thread_t recv_tid;
    void *ns_cb_data = NULL;

    rt_kprintf("\n");
    rt_kprintf("========================================\n");
    rt_kprintf("         RPMSG DMX512 Driver\n");
    rt_kprintf("========================================\n");
    rt_kprintf("\n");

    /* Check shared memory */
    if ((RPMSG_MEM_BASE + RPMSG_MEM_SIZE) > RPMSG_MEM_END) {
        rt_kprintf("[RPMSG] ERROR: Shared memory size error\n");
        return -RT_ERROR;
    }

    rt_kprintf("[RPMSG] Shared memory: 0x%x - 0x%x\n", RPMSG_MEM_BASE, RPMSG_MEM_END);

    /* Initialize DMX driver FIRST */
    rt_kprintf("\n");
    if (dmx_init() != RT_EOK) {
        rt_kprintf("[ERROR] DMX driver init failed\n");
        return -RT_ERROR;
    }

    /* Determine master/remote ID */
    master_id = MASTER_ID;
#ifdef HAL_AP_CORE
    remote_id = HAL_CPU_TOPOLOGY_GetCurrentCpuId();
    rt_kprintf("[RPMSG] Remote core CPU ID: %d (AP)\n", remote_id);
#else
    remote_id = 4;
    rt_kprintf("[RPMSG] Remote core ID: %d (MCU)\n", remote_id);
#endif

    link_id = RL_PLATFORM_SET_LINK_ID(master_id, remote_id);

    /* Init RPMSG lite as REMOTE */
    rt_kprintf("[RPMSG] Initializing as REMOTE (link=0x%x)...\n", link_id);

    g_rpmsg_instance = rpmsg_lite_remote_init((void *)RPMSG_MEM_BASE, link_id, RL_NO_FLAGS);
    if (!g_rpmsg_instance) {
        rt_kprintf("[RPMSG] ERROR: rpmsg_lite_remote_init failed\n");
        return -RT_ERROR;
    }

    /* Wait for link up */
    rt_kprintf("[RPMSG] Waiting for link up...\n");
    rpmsg_lite_wait_for_link_up(g_rpmsg_instance, RL_BLOCK);
    rt_kprintf("[RPMSG] Link UP!\n");

    /* Bind name service */
    rpmsg_ns_bind(g_rpmsg_instance, rpmsg_ns_callback, &ns_cb_data);

    /* Create queue */
    g_rpmsg_queue = rpmsg_queue_create(g_rpmsg_instance);
    if (!g_rpmsg_queue) {
        rt_kprintf("[RPMSG] ERROR: rpmsg_queue_create failed\n");
        return -RT_ERROR;
    }

    /* Create endpoint */
    g_rpmsg_ept = rpmsg_lite_create_ept(g_rpmsg_instance, RPMSG_EPT_ADDR,
                                        rpmsg_queue_rx_cb, g_rpmsg_queue);
    if (!g_rpmsg_ept) {
        rt_kprintf("[RPMSG] ERROR: rpmsg_lite_create_ept failed\n");
        return -RT_ERROR;
    }

    rt_kprintf("[RPMSG] Endpoint created (addr=0x%x)\n", RPMSG_EPT_ADDR);

    /* Announce channel */
    rt_kprintf("[RPMSG] Announcing channel '%s'...\n", RPMSG_CHANNEL_NAME);
    rpmsg_ns_announce(g_rpmsg_instance, g_rpmsg_ept, RPMSG_CHANNEL_NAME, RL_NS_CREATE);

    /* Create reception thread */
    recv_tid = rt_thread_create("rpmsg_rx", rpmsg_recv_thread, RT_NULL,
                                2048, RT_THREAD_PRIORITY_MAX / 2 - 1, 10);
    if (recv_tid != RT_NULL) {
        rt_thread_startup(recv_tid);
        rt_kprintf("[RPMSG] Reception thread created\n");
    } else {
        rt_kprintf("[RPMSG] ERROR: Failed to create thread\n");
        return -RT_ERROR;
    }

    rt_kprintf("\n");
    rt_kprintf("========================================\n");
    rt_kprintf("  DMX512 Driver Ready!\n");
    rt_kprintf("  UART3 TX: 250kbaud, 8N2, 44Hz\n");
    rt_kprintf("  Waiting for commands...\n");
    rt_kprintf("========================================\n");
    rt_kprintf("\n");

    return RT_EOK;
}

INIT_APP_EXPORT(rpmsg_dmx_init);

#else /* !RT_USING_LINUX_RPMSG */

static int rpmsg_dmx_init(void)
{
    rt_kprintf("[ERROR] RT_USING_LINUX_RPMSG not enabled!\n");
    return -RT_ERROR;
}
INIT_APP_EXPORT(rpmsg_dmx_init);

#endif /* RT_USING_LINUX_RPMSG */
