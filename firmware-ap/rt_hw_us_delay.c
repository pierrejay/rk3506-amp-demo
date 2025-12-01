/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * rt_hw_us_delay() using Rockchip HAL TIMER5 (24MHz).
 */

#include <rtthread.h>
#include "hal_base.h"

void rt_hw_us_delay(rt_uint32_t us)
{
    HAL_DelayUs(us);
}
