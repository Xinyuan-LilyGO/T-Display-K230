# T-Display K230 Debian 13 CLI

This build target keeps the U-Boot, RM69A10 DRM/DSI drivers, and device tree from the LilyGo BSP, 
but replaces the Buildroot user space with the Debian 13 rootfs from the Canaan SDK. 
It doesn’t install the LilyGo LVGL Launcher, nor does it use the LVGL display terminal.

## First milestone

- Boot Debian 13 riscv64 from a TF card.  
- RM69A10 uses its native 568x1232 display mode.  
- Linux framebuffer console is bound to `tty1`.  
- Using the kernel font `TER16x32`, about 35 columns by 38 rows.  
- Keep the `ttyS0,115200` serial console for debugging.  
- Use `multi-user.target`, no graphical desktop.  
- Don’t install LVGL Launcher.  
- Disable GT9895/Goodix touch kernel driver.  
- Skip the camera, AI Python wheel, and ADB/MTP add-ons in the Canaan Debian packaging process.

## Ubuntu 24.04 build

```sh
cd ~/T-Display-K230
git submodule update --init --recursive
bash ./scripts/setup_ubuntu.sh
bash ./scripts/build_debian13_cli.sh
```

Generate file:

```text
k230_bsp/images/t-display-k230-debian13-cli.img.gz
k230_bsp/images/t-display-k230-debian13-cli.img.gz.sha256
```

Zip before burning the OS:

```sh
gzip -dk k230_bsp/images/t-display-k230-debian13-cli.img.gz
sudo dd if=k230_bsp/images/t-display-k230-debian13-cli.img \
    of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

Please replace `/dev/sdX` with the entire device of the TF card. The first boot account comes from Canaan Debian 13 rootfs, 
and the current default is `root` / `root`. You must change the password before connecting to the network.

## Console Startup Parameters

Setting up the CLI overlay in the U-Boot environment:

```text
root=/dev/mmcblk1p2 rootwait rootfstype=ext4
console=tty1 console=ttyS0,115200
fbcon=font:TER16x32 earlycon=sbi
```

`console=tty1` is responsible for the kernel console on the AMOLED; 
the serial console is kept as the last console so you can still get the full logs if the screen driver fails.

## First board inspection

After logging in via serial port or AMOLED, execute:

```sh
cat /proc/cmdline
dmesg | grep -Ei 'rm69a10|drm|dsi|fbcon|framebuffer'
cat /sys/class/graphics/fb0/name
systemctl status getty@tty1.service --no-pager
```

You should be able to see RM69A10/DSI initialization, DRM framebuffer registration, `fbcon` taking over the console, and `getty@tty1.service` being active. 
If AMOLED doesn't enter the Linux console, first save the complete serial boot log; 
the screen timing, DRM framebuffer registration, and tty binding can all be clearly distinguished from this log.
