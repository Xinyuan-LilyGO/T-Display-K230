<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 🌟</h1>

本仓库用于从 Kendryte 官方 K230 Linux SDK 构建 LILYGO T-Display K230
的 SD 卡镜像，包含板级支持文件、LVGL 启动器应用和主机侧辅助脚本

已验证的上游 SDK：

```text
https://github.com/kendryte/k230_linux_sdk.git
dev commit 22d02c6b6783a57a3aca7eb3160e313e772cb710
```

## 目录结构

```text
k230_bsp/          U-Boot、Linux、Buildroot、DTB、启动 logo 和硬件服务 overlay
k230_launcher/     LVGL 启动器应用和随镜像打包的运行资源
k230_linux_sdk/    Kendryte 官方 K230 Linux SDK 子模块
scripts/           主机依赖安装、overlay 应用、镜像构建和 SSH 部署脚本
```

## 文档入口

- [Launcher 应用使用说明](k230_launcher/docs/APPS_CN.md)
- [BSP GPIO 和硬件连接表](k230_bsp/docs/HARDWARE_PINMAP_CN.md)
- [BSP README](k230_bsp/README_CN.MD)
- [Launcher README](k230_launcher/README_CN.MD)

## 从全新 Ubuntu 电脑开始构建

推荐 Ubuntu 22.04 或 24.04

安装 Git 和基础构建工具：

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

克隆本仓库并拉取 SDK 子模块：

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
```

如果克隆时没有拉取子模块：

```sh
git submodule update --init --recursive
```

安装 SDK 工具链和主机依赖：

```sh
./scripts/setup_ubuntu.sh
```

将 BSP 和 Launcher 应用到 SDK：

```sh
./scripts/apply_to_sdk.sh
```

构建 SD 卡镜像：

```sh
./scripts/build_sdcard_image.sh
```

镜像会导出到：

```text
k230_bsp/images/sysimage-sdcard.img
```

## 烧录

Linux 下将 `/dev/sdX` 替换为真实 TF 卡设备：

```sh
sudo dd if=k230_bsp/images/sysimage-sdcard.img of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

macOS 下将 `diskN` 替换为真实 TF 卡设备：

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=k230_bsp/images/sysimage-sdcard.img of=/dev/rdiskN bs=8m status=progress
sync
```

## SSH 快速部署

设备已经进入 Linux 且可以 SSH 连接时，可以不重新烧录 SD 卡，直接重编并部署
Launcher：

```sh
./scripts/deploy_launcher.sh 192.168.36.232
```

同时部署 Launcher 和启动文件：

```sh
./scripts/deploy_launcher.sh 192.168.36.232 --full --reboot
```

## 已包含的硬件支持

- RM69A10 AMOLED MIPI DSI 屏幕和 U-Boot 启动 logo
- GT9895 / Goodix Berlin 触摸
- GC2093 摄像头
- RTL8189FS SDIO Wi-Fi
- USB 蓝牙适配器支持和 BlueZ 运行环境
- nRF52840 UART 预设
- nRF9151 UART 和使能引脚预设
- SX1262/LR2021 LoRa SPI 预设
- MAX98357A 外部 I2S 功放和音频输出切换
- AHT20、BQ25896、BQ27220、XL9555、TCA8418 键盘底板 I2C 设备
- 键盘中断 GPIO42 和键盘背光 GPIO52/PWM4
- PMU 电源键 input 和 K230 温度传感器支持

`k230_bsp/images/` 下的镜像文件是本机构建产物
