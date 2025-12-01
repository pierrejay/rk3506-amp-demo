/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * RK3506 MCU Platform for rpmsg-lite - Configuration
 *
 * INSTALLATION:
 *   Copy to: $SDK/hal/middleware/rpmsg-lite/lib/include/platform/RK3506-MCU/
 *
 * NOTE: MCU uses MBOX1/MBOX3, CPU2 uses MBOX0/MBOX2 - no conflict.
 */

#ifndef RPMSG_CONFIG_H_
#define RPMSG_CONFIG_H_

#include "hal_conf.h"

/* ============================================================================
 * Buffer Configuration
 * ============================================================================ */

#define RL_BUFFER_PAYLOAD_SIZE (496U)
#define RL_BUFFER_COUNT (64U)
/* endpoint size is formed by payload and struct rpmsg_std_hdr */
#define RL_EPT_SIZE (RL_BUFFER_PAYLOAD_SIZE + 16UL)

#define RL_MAX_INSTANCE_NUM (12U)
#define RL_PLATFORM_HIGHEST_LINK_ID (0xFFU)

/* ============================================================================
 * MAILBOX Configuration - MCU Specific
 * ============================================================================ */

#define RL_PLATFORM_USING_MBOX

#ifdef RL_PLATFORM_USING_MBOX

/* Magic number for RPMSG messages */
#define RL_RPMSG_MAGIC (0x524D5347U)

/*
 * MCU MAILBOX IRQ - DIFFERENT from CPU2!
 *
 * RK3506 has 4 separate MBOX instances, each with 1 channel.
 * MCU receives ALL mailbox IRQs via a single multiplexed IRQ:
 *   MAILBOX_8MUX1_IRQn = 22 (direct NVIC, not GIC)
 *
 * The ISR must check which MBOX triggered the interrupt.
 */
#define RL_MCU_MBOX_IRQn  MAILBOX_8MUX1_IRQn

/*
 * CORRECTED: Match Linux DTS mbox-names order!
 *
 * Linux DTS says: mbox-names = "rpmsg-rx", "rpmsg-tx";
 *                 mboxes = <&mailbox1 0 &mailbox3 0>;
 * So Linux RX = MBOX1 (receives from MCU), Linux TX = MBOX3 (sends to MCU)
 *
 * Therefore MCU must:
 *   - RX on MBOX3 (where Linux TX sends)
 *   - TX on MBOX1 (where Linux RX listens)
 */
#define RL_MCU_MBOX_RX    MBOX3
#define RL_MCU_MBOX_TX    MBOX1

#endif /* RL_PLATFORM_USING_MBOX */

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/*
 * MCU address translation offset.
 * RK3506 MCU has no cache decode offset (unlike RK3562).
 */
#ifdef HAL_MCU_CORE
#define RL_PHY_MCU_OFFSET (0U)
#endif

/* ============================================================================
 * ISR Configuration
 * ============================================================================ */

/*
 * ISR count for bare-metal environment.
 * 4bit for master and 4bit for remote. each link_id has 2 virtqueue.
 */
#define ISR_COUNT (0x1FEU)
#define RL_ALLOW_CONSUMED_BUFFERS_NOTIFICATION (1)

#endif /* RPMSG_CONFIG_H_ */
