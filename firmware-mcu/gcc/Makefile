# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2022 Rockchip Electronics Co., Ltd.
# Copyright (c) 2025 Pierre Jay
#
# MODIFIED for MCU RPMSG Echo Firmware:
#   - Added nano.specs for minimal libc
#   - Added RPMSG-lite with RK3506-MCU platform
#   - Removed benchmark/simple_console (not needed)

PROJECT := RK3506
SOC		:= RK3506
CPU		:= -mcpu=cortex-m0

# BUS M0
# Size optimization flags:
#   -Os: optimize for size
#   -ffunction-sections -fdata-sections: enable --gc-sections to remove unused code
#   -fno-unwind-tables -fno-asynchronous-unwind-tables: no exception handling (~2KB)
CFLAGS := -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables

# nano.specs uses newlib-nano with minimal printf (no float support)
LDFLAGS := --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
LINKER_SCRIPT := gcc_bus_m0.ld

# RPMSG Platform: Use our RK3506-MCU platform
RPMSG_PLATFORM := RK3506-MCU
RPMSG_LITE_PATH := ../../../middleware/rpmsg-lite/lib

INCLUDES += \
-I"$(RPMSG_LITE_PATH)/include" \
-I"$(RPMSG_LITE_PATH)/include/environment/bm" \
-I"$(RPMSG_LITE_PATH)/include/platform/$(RPMSG_PLATFORM)" \

# RPMSG sources - explicit list to exclude rpmsg_queue.c (needs OS)
SRC_DIRS += \
    $(RPMSG_LITE_PATH)/common \
    $(RPMSG_LITE_PATH)/rpmsg_lite/porting/platform/$(RPMSG_PLATFORM) \
    $(RPMSG_LITE_PATH)/virtio \

# Add individual RPMSG files (NOT rpmsg_queue.c - requires env_*_queue functions)
SRCS += \
    $(RPMSG_LITE_PATH)/rpmsg_lite/rpmsg_lite.c \
    $(RPMSG_LITE_PATH)/rpmsg_lite/rpmsg_ns.c \
    $(RPMSG_LITE_PATH)/rpmsg_lite/porting/environment/rpmsg_env_bm.c

# NO benchmark.mk, NO simple_console.mk
include ../../common/GCC/Cortex-M.mk
