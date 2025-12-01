/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2025 Pierre Jay
 *
 * RT-Thread entry point for DMX512 AMP demo.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "hal_base.h"

/* ============================================================================
 * Printf redirect to UART4 (debug RT-Thread)
 * ============================================================================ */

#if defined(HAL_DBG_USING_LIBC_PRINTF) || defined(HAL_DBG_USING_HAL_PRINTF)

static struct UART_REG *pUart = UART4;

#ifdef __GNUC__
__USED int _write(int fd, char *ptr, int len)
{
    int i = 0;

    /*
     * write "len" of char from "ptr" to file id "fd"
     * Return number of char written.
     *
     * Only work for STDOUT, STDIN, and STDERR
     */
    if (fd > 2) {
        return -1;
    }

    while (*ptr && (i < len)) {
        if (*ptr == '\n') {
            HAL_UART_SerialOutChar(pUart, '\r');
        }
        HAL_UART_SerialOutChar(pUart, *ptr);

        i++;
        ptr++;
    }

    return i;
}
#else
int fputc(int ch, FILE *f)
{
    if (ch == '\n') {
        HAL_UART_SerialOutChar(pUart, '\r');
    }

    HAL_UART_SerialOutChar(pUart, (char)ch);

    return 0;
}
#endif /* end of __GNUC__ */

#endif /* end of HAL_DBG_USING_LIBC_PRINTF */

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv)
{
    rt_kprintf("\n");
    rt_kprintf("========================================\n");
    rt_kprintf("  RT-Thread on RK3506G2 CPU2\n");
    rt_kprintf("  AMP Mode: Linux (CPU0+1) + RTOS (CPU2)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("\n");
    rt_kprintf("RT-Thread version: %d.%d.%d\n",
               RT_VERSION, RT_SUBVERSION, RT_REVISION);
    rt_kprintf("CPU: Cortex-A7 #2 (dedicated)\n");
    rt_kprintf("\n");
    rt_kprintf("Application: DMX512 Gateway\n");
    rt_kprintf("\n");

    /*
     * The rpmsg_dmx_init() app will be launched automatically
     * via INIT_APP_EXPORT() in rpmsg_uart_dmx.c
     */

#ifdef RT_USING_NEW_OTA
    /*
     * rk_ota_set_boot_success will set `successful_boot` flag to 1.
     * Move this function to a location where all of your service
     * has finished to make sure to set `successful_boot` after
     * all of your main service has succeeded.
     */
    rk_ota_set_boot_success();
#endif

    return 0;
}
