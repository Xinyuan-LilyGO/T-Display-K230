# T-Display K230 Debian 13 CLI

这个构建目标保留 LilyGo BSP 的 U-Boot、RM69A10 DRM/DSI 驱动和设备树，使用
Canaan SDK 的 Debian 13 rootfs 替换 Buildroot 用户空间。它不安装 LilyGo LVGL
Launcher，也不使用 LVGL 显示终端。

## 首个里程碑

- 从 TF 卡启动 Debian 13 riscv64。
- RM69A10 使用原生 568x1232 显示模式。
- Linux framebuffer console 绑定到 `tty1`。
- 使用内核 `TER16x32` 字体，约 35 列、38 行。
- 同时保留 `ttyS0,115200` 串口控制台用于排错。
- 使用 `multi-user.target`，不启动图形桌面。
- 不安装 LVGL Launcher。
- 禁用 GT9895/Goodix 触摸内核驱动。
- 跳过 Canaan Debian 打包流程中的相机、AI Python wheel、ADB/MTP 附加层。

## Ubuntu 24.04 构建

```sh
cd ~/T-Display-K230
git submodule update --init --recursive
bash ./scripts/setup_ubuntu.sh
bash ./scripts/build_debian13_cli.sh
```

生成文件：

```text
k230_bsp/images/t-display-k230-debian13-cli.img.gz
k230_bsp/images/t-display-k230-debian13-cli.img.gz.sha256
```

烧录前解压：

```sh
gzip -dk k230_bsp/images/t-display-k230-debian13-cli.img.gz
sudo dd if=k230_bsp/images/t-display-k230-debian13-cli.img \
    of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

请将 `/dev/sdX` 替换为 TF 卡的整盘设备。首次启动账户来自 Canaan Debian 13
rootfs，当前默认是 `root` / `root`，联网前必须修改密码。

## 控制台启动参数

CLI overlay 在 U-Boot 环境中设置：

```text
root=/dev/mmcblk1p2 rootwait rootfstype=ext4
console=tty1 console=ttyS0,115200
fbcon=font:TER16x32 earlycon=sbi
```

`console=tty1` 负责 AMOLED 上的内核控制台；串口控制台保留为最后一个 console，
方便屏幕驱动失败时继续获取完整日志。

## 首次上板检查

通过串口或 AMOLED 登录后执行：

```sh
cat /proc/cmdline
dmesg | grep -Ei 'rm69a10|drm|dsi|fbcon|framebuffer'
cat /sys/class/graphics/fb0/name
systemctl status getty@tty1.service --no-pager
```

预期能看到 RM69A10/DSI 初始化、DRM framebuffer 注册、`fbcon` 接管控制台，且
`getty@tty1.service` 为 active。若 AMOLED 没有进入 Linux 控制台，先保存完整串口
启动日志；屏幕时序、DRM framebuffer 注册和 tty 绑定可以由这份日志明确区分。
