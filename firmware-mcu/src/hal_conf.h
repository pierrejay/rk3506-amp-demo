/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2022 Rockchip Electronics Co., Ltd.
 * Copyright (c) 2025 Pierre Jay
 *
 * Modified for MCU DMX gateway:
 *   - Minimal HAL modules for size optimization
 *   - Added HAL_MBOX_MODULE_ENABLED for RPMSG
 */

#ifndef _HAL_CONF_H_
#define _HAL_CONF_H_

/* CPU config */
#define SOC_RK3506
#define HAL_MCU_CORE

#define HAL_BUS_MCU_CORE

#define SYS_TIMER TIMER5 /* System timer designation (RK TIMER) */

/* HAL Driver Config */
/* #define HAL_CRU_MODULE_ENABLED */  /* DISABLED - saves ~5KB, Linux manages clocks */
#define HAL_INTMUX_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_MBOX_MODULE_ENABLED
#define HAL_NVIC_MODULE_ENABLED
#define HAL_PINCTRL_MODULE_ENABLED
#define HAL_SYSTICK_MODULE_ENABLED
#define HAL_TIMER_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* HAL_DBG SUB CONFIG */

#define HAL_DBG_ON
#ifdef HAL_DBG_ON
#define HAL_DBG_USING_HAL_PRINTF
#define HAL_DBG_ON
#define HAL_DBG_INFO_ON
#define HAL_DBG_WRN_ON
#define HAL_DBG_ERR_ON
#endif

#endif
