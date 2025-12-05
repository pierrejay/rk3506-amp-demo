# Custom SDK Configurations

Headless configs for Luckfox Lyra RK3506G. No display, no audio = more RAM, faster boot.

## Configs

| Config | Linux CPUs | Real-time | RAM | RPMSG |
|--------|-----------|-----------|-----|-------|
| **base-noamp-lite** | 3 | None | ~120MB | - |
| **mcu-only-lite** | 3 | MCU | ~117MB | `ttyRPMSG0` |
| **ap-only-lite** | 2 | CPU2 (RT-Thread) | ~112MB | `ttyRPMSG0` |
| **ap-mcu-lite** | 2 | CPU2 + MCU | ~110MB | `ttyRPMSG0`, `ttyRPMSG1` |

## Deploy

```bash
CONFIG="base-noamp-lite"  # or mcu-only-lite, ap-only-lite, ap-mcu-lite

# Copy to SDK
cp CUSTOM/custom-configs/$CONFIG/*.dtsi kernel-6.1/arch/arm/boot/dts/
cp CUSTOM/custom-configs/$CONFIG/*.dts kernel-6.1/arch/arm/boot/dts/
cp CUSTOM/custom-configs/$CONFIG/*.its device/rockchip/.chips/rk3506/
cp CUSTOM/custom-configs/$CONFIG/*_defconfig device/rockchip/.chips/rk3506/

# Build
./build.sh <defconfig_name>
./build.sh

# Flash
sudo ./rkflash.sh update
```

## Defconfig names

- base-noamp-lite: `luckfox_lyra_plus_buildroot_spinand_lite_defconfig`
- mcu-only-lite: `rk3506g_buildroot_spinand_mcu_only_lite_defconfig`
- ap-only-lite: `rk3506g_buildroot_spinand_ap_only_lite_defconfig`
- ap-mcu-lite: `rk3506g_buildroot_spinand_ap_mcu_lite_defconfig`

## Reserved pins

### base-noamp-lite

No reserved pins - all RM_IO available for Linux.

### mcu-only-lite

| Pin | Function | Usage |
|-----|----------|-------|
| RM_IO6 | UART2 TX | MCU app |
| RM_IO7 | UART2 RX | MCU app |
| GPIO1_D2 | UART5 TX | MCU debug |
| GPIO1_D3 | UART5 RX | MCU debug |

### ap-only-lite

| Pin | Function | Usage |
|-----|----------|-------|
| RM_IO2 | UART4 TX | CPU2 debug |
| RM_IO3 | UART4 RX | CPU2 debug |
| RM_IO4 | UART3 TX | CPU2 app |
| RM_IO5 | UART3 RX | CPU2 app |
| RM_IO12 | I2C0 SDA | CPU2 |
| RM_IO13 | I2C0 SCL | CPU2 |

### ap-mcu-lite

| Pin | Function | Usage |
|-----|----------|-------|
| RM_IO2 | UART4 TX | CPU2 debug |
| RM_IO3 | UART4 RX | CPU2 debug |
| RM_IO4 | UART3 TX | CPU2 app |
| RM_IO5 | UART3 RX | CPU2 app |
| RM_IO6 | UART2 TX | MCU app |
| RM_IO7 | UART2 RX | MCU app |
| RM_IO12 | I2C0 SDA | CPU2 |
| RM_IO13 | I2C0 SCL | CPU2 |
| GPIO1_D2 | UART5 TX | MCU debug |
| GPIO1_D3 | UART5 RX | MCU debug |

## Files per config

```
<config>/
├── rk3506-*.dtsi           # AMP config (AMP configs only)
├── rk3506g-*-spinand.dts   # Board DTS
├── amp_linux*.its          # FIT image (AMP configs only)
└── *_defconfig             # SDK config
```
