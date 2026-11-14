<div align="center">
  <img src=".github/LilyGo_logo.png" alt="LILYGO" width="96">
  <h1>T-Display K230 Debian 13 CLI</h1>
</div>

[English](README.MD) | [中文](README_CN.MD)

<p align="center">
  <img src="./images/debian.jpg" alt="LilyGo logo" width="320" />
</p>

本项目基于 Kendryte 官方 K230 Linux SDK，为 LILYGO T-Display K230 构建
Debian 13 命令行系统。

此分支不包含 LVGL Launcher 或 MicroPython OS。主要改动包括 RM69A10 屏幕的
framebuffer 控制台、TCA8418 实体键盘、控制台键位映射，以及 Debian 专用的构建和
镜像检查脚本。

## 目录

```text
debian13_cli/    Debian 补丁和 rootfs overlay
k230_bsp/        U-Boot、Linux、设备树和 Buildroot 板级支持
k230_linux_sdk/  官方 K230 Linux SDK 子模块
scripts/         环境准备、构建和镜像检查脚本
```

## 编译

SDK 工具链要求 x86_64 Linux 环境，推荐 Ubuntu 22.04 或 24.04。

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git
cd T-Display-K230
./scripts/setup_ubuntu.sh
./scripts/build_debian13_cli.sh
```

如果克隆时没有拉取子模块：

```sh
git submodule update --init --recursive
```

构建脚本会应用 BSP 和 Debian 补丁、编译 SDK、检查最终 rootfs，并生成：

```text
k230_bsp/images/t-display-k230-debian13-cli.img.gz
k230_bsp/images/t-display-k230-debian13-cli.img.gz.sha256
```

### Windows + WSL2

在管理员 PowerShell 中安装 Ubuntu：

```powershell
wsl --install -d Ubuntu-24.04
```

请在 WSL 的 Linux 文件系统内编译，例如 `~/work`。不要放在 `/mnt/c` 或 `/mnt/e`
下编译，因为 SDK 依赖 Linux 文件权限和软链接。

```sh
mkdir -p ~/work && cd ~/work
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git
cd T-Display-K230
./scripts/setup_ubuntu.sh
./scripts/build_debian13_cli.sh
```

从 Windows 资源管理器打开产物目录：

```sh
explorer.exe "$(wslpath -w k230_bsp/images)"
```

### macOS

不支持在 macOS 原生编译。请使用 x86_64 Ubuntu 22.04/24.04 虚拟机或远程
x86_64 Ubuntu 主机，编译完成后将 `.img.gz` 复制回 macOS。Apple Silicon 上的
ARM64 Ubuntu 虚拟机不能直接使用随 SDK 提供的 x86_64 Linux 工具链。

## 烧录 TF 卡

烧录会清空目标设备，写入前务必确认设备名称。

### Ubuntu

用 `lsblk` 找到 TF 卡，将下方 `/dev/sdX` 替换为整张卡，而不是 `/dev/sdX1`
这样的分区。

```sh
gzip -dc k230_bsp/images/t-display-k230-debian13-cli.img.gz | \
  sudo dd of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

### Windows

用 7-Zip 解压 `.img.gz`，打开 Rufus，选择 TF 卡和解压得到的 `.img` 文件，使用
DD 镜像模式写入。烧录完成后，如果 Windows 提示格式化 Linux 分区，请取消。

### macOS

用 `diskutil list` 找到 TF 卡，将 `diskN` 替换为整张卡对应的磁盘，并通过速度更快的
`rdiskN` 写入：

```sh
diskutil unmountDisk /dev/diskN
gzip -dc k230_bsp/images/t-display-k230-debian13-cli.img.gz | \
  sudo dd of=/dev/rdiskN bs=8m
sync
diskutil eject /dev/diskN
```

将 TF 卡插入 T-Display K230 后上电，Debian 会显示在 AMOLED 控制台上；串口控制台
仍可用于诊断。
