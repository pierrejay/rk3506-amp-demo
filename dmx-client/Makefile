# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2025 Pierre Jay
#
# DMX Client - Cross-compilation Makefile
#
# For Luckfox Lyra (RK3506)
#
# Usage:
#   export SDK_DIR=/path/to/luckfox-lyra-sdk
#   make              # Cross-compile
#   make install      # Install to SDK overlay

# SDK_DIR must be set to Luckfox SDK root
ifndef SDK_DIR
$(error SDK_DIR is not set. Run: export SDK_DIR=/path/to/luckfox-lyra-sdk)
endif

# Toolchain from SDK
CROSS_COMPILE = $(SDK_DIR)/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

# Build flags
CFLAGS = -Wall -O2 -static
TARGET = dmx

# Sources
SRCS = dmx_client.c
HDRS = dmx_protocol.h

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

# Install to SDK overlay (for buildroot inclusion)
OVERLAY_DIR = $(SDK_DIR)/device/rockchip/common/overlays/rootfs/custom/root/usr/bin
install: $(TARGET)
	install -D -m 755 $(TARGET) $(OVERLAY_DIR)/$(TARGET)
