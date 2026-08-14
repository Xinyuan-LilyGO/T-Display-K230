<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 BSP 🌟</h1>

[English](./README.MD) | [中文](./README_CN.MD)

这个目录是 LILYGO T-Display K230 RM69A10 硬件的板级支持 overlay，
用于叠加到 Kendryte 官方 K230 Linux SDK。

已验证的上游 SDK：

```text
https://github.com/kendryte/k230_linux_sdk.git
dev commit 22d02c6b6783a57a3aca7eb3160e313e772cb710
```

## 本 BSP 包含什么

- RM69A10 AMOLED MIPI DSI 屏幕和 U-Boot 启动 logo。
- GT9895 / Goodix Berlin 触摸驱动。
- GC2093 摄像头支持。
- RTL8189FS 和 RTL8723DS SDIO Wi-Fi 支持。
- USB 蓝牙适配器内核支持和 BlueZ 工具。
- nRF52840 UART1 引脚预设。
- nRF9151 UART3 和 GPIO2 使能脚预设。
- SX1262/LR2021 LoRa SPI 引脚预设。
- MAX98357A 外部 I2S 功放引脚和音频输出切换支持。
- AHT20、BQ25896、BQ27220、XL9555、TCA8418 键盘底板 I2C 支持。
- 键盘中断 GPIO42 和键盘背光 GPIO52/PWM4。
- PMU 电源键 input 驱动和 K230 温度传感器支持。

GPIO、总线和外设连接关系请看
[docs/HARDWARE_PINMAP_CN.md](docs/HARDWARE_PINMAP_CN.md)。

## 从全新 Ubuntu 电脑开始构建

上游 SDK 主要验证环境是 Ubuntu 22.04 和 Ubuntu 24.04。

安装 Git 和基础工具：

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

创建工作目录：

```sh
mkdir -p ~/work
cd ~/work
```

克隆上游 SDK，并切换到已验证 commit：

```sh
git clone https://github.com/kendryte/k230_linux_sdk.git
cd k230_linux_sdk
git checkout 22d02c6b6783a57a3aca7eb3160e313e772cb710
cd ..
```

克隆本仓库：

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
```

安装上游 SDK 工具链和依赖：

```sh
cd ~/work/k230_linux_sdk
sudo make toolchain_and_depend
cd ..
```

应用 BSP：

```sh
./t-display-k230/k230_bsp/scripts/apply.sh ./k230_linux_sdk
```

构建 SD 卡镜像：

```sh
./t-display-k230/k230_bsp/scripts/build.sh ./k230_linux_sdk
```

BSP defconfig 默认设置 `BR2_TAR_OPTIONS="--no-same-owner"`，这样在容器或 rootless 环境中解压 Linux 源码包时不会因为无法恢复压缩包内 uid/gid 而失败。

BSP defconfig 也将 LVGL 固定到 SDK 当前可构建的 `59dc7e436ae97a25e32656739ea6a943f9f11b6a` 版本，并禁用上游 `python-k230`。`python-k230` 是 Python/LVGL 绑定示例，不属于硬件 BSP；在当前上游 SDK 快照里它和 LVGL 9 生成 API 不兼容。

U-Boot 显示探测使用 DesignWare DM I2C。BSP 在 T-Display U-Boot defconfig 中启用 `CONFIG_DM_I2C`、`CONFIG_SYS_I2C_DW` 和 `CONFIG_CMD_I2C`，并包含本次干净构建验证时使用的 RM69A10/LT9611 默认 I2C 地址修正。

SD 卡镜像使用 MBR 分区表。这样可以避免小镜像写入大 TF 卡后，旧的 backup GPT 残留导致 Linux 忽略 boot/rootfs 分区。

导出镜像到 BSP 的 `images/` 目录：

```sh
./t-display-k230/k230_bsp/scripts/export_images.sh ./k230_linux_sdk
```

主要镜像文件是：

```text
t-display-k230/k230_bsp/images/sysimage-sdcard.img
```

如果 SDK 只生成 `sysimage-sdcard.img.gz`，烧录前需要先解压。

## 烧录

Linux 下将 `/dev/sdX` 替换为真实 TF 卡设备：

```sh
sudo dd if=t-display-k230/k230_bsp/images/sysimage-sdcard.img of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

macOS 下将 `diskN` 替换为真实 TF 卡设备：

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=t-display-k230/k230_bsp/images/sysimage-sdcard.img of=/dev/rdiskN bs=8m status=progress
sync
```

如果某张卡之前烧录过 GPT 镜像，并且启动时找不到 `/dev/mmcblk1p2`，请使用当前 MBR 镜像重新烧录。

## Launcher 应用

需要将随仓库提供的 LVGL launcher 打包进镜像时，在 BSP 之后安装 launcher：

```sh
./t-display-k230/k230_bsp/scripts/apply.sh ./k230_linux_sdk
./t-display-k230/k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

之后根据需要重新构建完整镜像，或者只重建 launcher package。

## 目录结构

```text
overlay/                 复制到 k230_linux_sdk 的 BSP 文件
scripts/apply.sh         将 BSP overlay 应用到 SDK
scripts/build.sh         构建 k230_canmv_t_display_rm69a10_defconfig
scripts/export_images.sh 将镜像产物复制到 images/
metadata/                上游 SDK URL/commit 和 overlay 文件清单
docs/HARDWARE_PINMAP.md  板级 GPIO 和硬件连接说明
docs/HARDWARE_PINMAP_CN.md 中文板级 GPIO 和硬件连接说明
images/                  本地镜像导出目录
```

`images/` 下的镜像是生成产物，默认不提交到 git。它们由 `scripts/export_images.sh` 复制到该目录，方便本机测试和发布打包。

## 注意事项

- 如果 SDK commit 和已验证 commit 不一致，`apply.sh` 会警告但仍会复制 overlay。如果因此构建失败，需要重新 rebase BSP。
- HDMI 实验支持保留为可选 DTB/输出路径，默认显示目标仍然是 AMOLED。
- RTL8723DS 蓝牙需要硬件实际接出 BT UART；SDIO 只负责 Wi-Fi。
