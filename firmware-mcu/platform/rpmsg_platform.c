/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * RK3506 MCU Platform for rpmsg-lite
 *
 * Platform implementation for Cortex-M0+ MCU on RK3506.
 * Based on RK3562-MCU implementation from Rockchip SDK.
 *
 * INSTALLATION:
 *   Copy to: $SDK/hal/middleware/rpmsg-lite/lib/rpmsg_lite/porting/platform/RK3506-MCU/
 *
 * ARCHITECTURE:
 *   - MCU uses MBOX3 (RX from Linux) and MBOX1 (TX to Linux)
 *   - IRQ routing: MAILBOX_BB_3 (source 117) -> INTMUX3 -> NVIC IRQ 31
 *   - CRITICAL: PCLK_INTMUX and PCLK_MAILBOX must be in DTS rockchip_amp clocks
 *
 * COEXISTENCE with CPU2:
 *   - CPU2 (RT-Thread) uses MBOX0/MBOX2 with GIC IRQs
 *   - MCU (bare-metal) uses MBOX1/MBOX3 with NVIC IRQs
 */

#include <stdio.h>
#include <string.h>

#include "rpmsg_platform.h"
#include "rpmsg_env.h"
#include "rpmsg_config.h"

#include "hal_base.h"

#if defined(RL_USE_ENVIRONMENT_CONTEXT) && (RL_USE_ENVIRONMENT_CONTEXT == 1)
#error "This RPMsg-Lite port requires RL_USE_ENVIRONMENT_CONTEXT set to 0"
#endif

/* ============================================================================
 * Static Variables
 * ============================================================================ */

static int32_t isr_counter = 0;
static int32_t disable_counter = 0;
static int32_t first_notify = 0;
static void *platform_lock;

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
static LOCK_STATIC_CONTEXT platform_lock_static_ctxt;
#endif

/* ============================================================================
 * MAILBOX Configuration
 * ============================================================================ */

#ifdef RL_PLATFORM_USING_MBOX

#define RL_MBOX_SIDE_B 0  /* MCU is side B (receiver from Linux) */

/*
 * MCU mailbox assignment:
 *   - MBOX3 for RX (Linux TX -> MCU RX)
 *   - MBOX1 for TX (MCU TX -> Linux RX)
 */
static struct MBOX_REG *rl_pMBox_RX = RL_MCU_MBOX_RX;
static struct MBOX_REG *rl_pMBox_TX = RL_MCU_MBOX_TX;

static int32_t register_count = 0;

static void rpmsg_remote_cb(struct MBOX_CMD_DAT *msg, void *args);

/* ============================================================================
 * Interrupt Handler
 * ============================================================================ */

static void rpmsg_mbox_isr(void)
{
    struct MBOX_CMD_DAT msg;
    uint32_t status;

    /* Check MBOX3 A2B (Linux -> MCU) */
    status = MBOX3->A2B_STATUS;
    if (status & 0x1) {
        msg.CMD = MBOX3->A2B_CMD;
        msg.DATA = MBOX3->A2B_DATA;
        MBOX3->A2B_STATUS = 0x1;  /* W1C clear */

        rpmsg_remote_cb(&msg, NULL);
    }

    /* Check MBOX1 B2A for TX acknowledgment */
    if (MBOX1->B2A_STATUS & 0x1) {
        MBOX1->B2A_STATUS = 0x1;
    }
}

/* ============================================================================
 * RPMSG Callbacks
 * ============================================================================ */

static void rpmsg_remote_cb(struct MBOX_CMD_DAT *msg, void *args)
{
    uint32_t link_id;

    (void)args;

    /* Verify RPMSG magic */
    if (msg->DATA != RL_RPMSG_MAGIC) {
        return;
    }

    link_id = msg->CMD & 0xFFU;

    /*
     * First notification is for VQ0 (RX vring ready),
     * subsequent notifications are for VQ1 (TX vring processed).
     */
    if (first_notify == 0) {
        env_isr(RL_GET_VQ_ID(link_id, 0));
        first_notify++;
    } else {
        env_isr(RL_GET_VQ_ID(link_id, 1));
    }
}

static struct MBOX_CLIENT mbox_client_rx = {
    .name = "mcu-rpmsg-rx",
    .irq = RL_MCU_MBOX_IRQn,
    .RXCallback = rpmsg_remote_cb,
    .callbackData = NULL
};

#endif /* RL_PLATFORM_USING_MBOX */

/* ============================================================================
 * Platform Functions
 * ============================================================================ */

static void platform_global_isr_disable(void)
{
    __disable_irq();
}

static void platform_global_isr_enable(void)
{
    __enable_irq();
}

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
#ifdef RL_PLATFORM_USING_MBOX
    int ret = 0;
#endif

    env_register_isr(vector_id, isr_data);

    env_lock_mutex(platform_lock);

    RL_ASSERT(0 <= isr_counter);
    if (isr_counter < 2 * RL_MAX_INSTANCE_NUM)
    {
#ifdef RL_PLATFORM_USING_MBOX
        if (register_count % 2 == 0)
        {
            /* Initialize mailboxes */
            HAL_MBOX_Init(rl_pMBox_RX, RL_MBOX_SIDE_B);
            ret = HAL_MBOX_RegisterClient(rl_pMBox_RX, MBOX_CH_0, &mbox_client_rx);
            if (ret) {
                printf("[MCU] MBOX client register failed: %d\n", ret);
            }

            /* Enable A2B interrupt with level trigger */
            {
                uint32_t inten = rl_pMBox_RX->A2B_INTEN;
                inten |= (1u << 0);   /* a2b_inten */
                inten |= (1u << 8);   /* a2b_trig_mode */
                rl_pMBox_RX->A2B_INTEN = inten;
            }

            HAL_MBOX_Init(rl_pMBox_TX, RL_MBOX_SIDE_B);

            /*
             * Register ISR on NVIC IRQ 31 (INTMUX_OUT3)
             * This is the correct path for MAILBOX_BB_3 (source 117)
             */
            HAL_NVIC_SetIRQHandler(INTMUX_OUT3_IRQn, rpmsg_mbox_isr);
            HAL_NVIC_EnableIRQ(INTMUX_OUT3_IRQn);

            /* Also register on IRQ 22 for legacy path compatibility */
            HAL_NVIC_SetIRQHandler(RL_MCU_MBOX_IRQn, rpmsg_mbox_isr);
            HAL_NVIC_EnableIRQ(RL_MCU_MBOX_IRQn);

            /* Enable MAILBOX sources in INTMUX */
            HAL_INTMUX_EnableIRQ(MAILBOX_BB_3_IRQn);  /* RX: Linux -> MCU */
            HAL_INTMUX_EnableIRQ(MAILBOX_BB_1_IRQn);  /* TX: MCU -> Linux ack */

            /* Check for pending message (catch-up) */
            if (rl_pMBox_RX->A2B_STATUS & 0x1) {
                struct MBOX_CMD_DAT catch_msg;
                catch_msg.CMD = rl_pMBox_RX->A2B_CMD;
                catch_msg.DATA = rl_pMBox_RX->A2B_DATA;
                rl_pMBox_RX->A2B_STATUS = 0x1;
                rpmsg_remote_cb(&catch_msg, NULL);
            }
        }
        register_count++;
#endif
    }
    isr_counter++;

    env_unlock_mutex(platform_lock);

    return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    env_lock_mutex(platform_lock);

    RL_ASSERT(0 < isr_counter);
    isr_counter--;

    env_unregister_isr(vector_id);

    env_unlock_mutex(platform_lock);

    return 0;
}

void platform_notify(uint32_t vector_id)
{
    uint32_t link_id;
    struct MBOX_CMD_DAT tx_msg;

    link_id = RL_GET_LINK_ID(vector_id);
    tx_msg.CMD = link_id & 0xFFU;
    tx_msg.DATA = RL_RPMSG_MAGIC;

    env_lock_mutex(platform_lock);

#ifdef RL_PLATFORM_USING_MBOX
    HAL_MBOX_SendMsg(rl_pMBox_TX, MBOX_CH_0, &tx_msg);
#endif

    env_unlock_mutex(platform_lock);
}

void platform_time_delay(uint32_t num_msec)
{
    HAL_DelayMs(num_msec);
}

int32_t platform_in_isr(void)
{
    return ((__get_IPSR() & 0x1FU) != 0U) ? 1 : 0;
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    RL_ASSERT(0 < disable_counter);

    platform_global_isr_disable();
    disable_counter--;
    if (disable_counter < 2 * RL_MAX_INSTANCE_NUM)
    {
#ifdef RL_PLATFORM_USING_MBOX
        HAL_NVIC_EnableIRQ(RL_MCU_MBOX_IRQn);
        HAL_NVIC_EnableIRQ(INTMUX_OUT3_IRQn);
#endif
    }
    platform_global_isr_enable();

    return ((int32_t)vector_id);
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    RL_ASSERT(0 <= disable_counter);

    platform_global_isr_disable();
    if (disable_counter < 2 * RL_MAX_INSTANCE_NUM)
    {
#ifdef RL_PLATFORM_USING_MBOX
        HAL_NVIC_DisableIRQ(RL_MCU_MBOX_IRQn);
        HAL_NVIC_DisableIRQ(INTMUX_OUT3_IRQn);
#endif
    }
    disable_counter++;
    platform_global_isr_enable();

    return ((int32_t)vector_id);
}

void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags)
{
    /* No MMU on Cortex-M0+ */
}

void platform_cache_all_flush_invalidate(void)
{
    /* No cache on Cortex-M0+ */
}

void platform_cache_disable(void)
{
    /* No cache on Cortex-M0+ */
}

uint32_t platform_vatopa(void *addr)
{
    return ((uint32_t)(char *)addr);
}

void *platform_patova(uint32_t addr)
{
#ifdef RL_PHY_MCU_OFFSET
    addr -= RL_PHY_MCU_OFFSET;
#endif
    return ((void *)(char *)addr);
}

int32_t platform_init(void)
{
#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
    if (0 != env_create_mutex(&platform_lock, 1, &platform_lock_static_ctxt))
#else
    if (0 != env_create_mutex(&platform_lock, 1))
#endif
    {
        return -1;
    }

    return 0;
}

int32_t platform_deinit(void)
{
    env_delete_mutex(platform_lock);
    platform_lock = ((void *)0);
    return 0;
}
