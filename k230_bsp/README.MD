<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 BSP 🌟</h1>

This directory contains the board support overlay for the LILYGO T-Display K230
RM69A10 hardware. It is applied on top of the official Kendryte K230 Linux SDK.

Validated upstream SDK:

```text
https://github.com/kendryte/k230_linux_sdk.git
dev commit 22d02c6b6783a57a3aca7eb3160e313e772cb710
```

## What This BSP Includes

- RM69A10 AMOLED MIPI DSI panel and U-Boot boot logo.
- GT9895 / Goodix Berlin touchscreen.
- GC2093 camera support.
- RTL8189FS and RTL8723DS SDIO Wi-Fi support.
- USB Bluetooth adapter kernel support and BlueZ tools.
- nRF52840 UART1 pin preset.
- nRF9151 UART3 and GPIO2 enable pin preset.
- SX1262/LR2021 LoRa SPI pin preset.
- MAX98357A external I2S amplifier pins and output switch support.
- AHT20, BQ25896, BQ27220, XL9555, and TCA8418 keyboard-base I2C support.
- Keyboard interrupt GPIO42 and keyboard backlight GPIO52/PWM4.
- PMU power-key input and K230 thermal sensor support.

See [docs/HARDWARE_PINMAP.md](docs/HARDWARE_PINMAP.md) for the GPIO, bus, and
peripheral connection map.

## Build From a Fresh Ubuntu PC

The upstream SDK is verified mainly on Ubuntu 22.04 and 24.04.

Install Git and basic tools:

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

Create a workspace:

```sh
mkdir -p ~/work
cd ~/work
```

Clone the upstream SDK and lock it to the validated commit:

```sh
git clone https://github.com/kendryte/k230_linux_sdk.git
cd k230_linux_sdk
git checkout 22d02c6b6783a57a3aca7eb3160e313e772cb710
cd ..
```

Clone this repository:

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
```

Install the upstream SDK toolchain and dependencies:

```sh
cd ~/work/k230_linux_sdk
sudo make toolchain_and_depend
cd ..
```

Apply the BSP:

```sh
./t-display-k230/k230_bsp/scripts/apply.sh ./k230_linux_sdk
```

Build the SD-card image:

```sh
./t-display-k230/k230_bsp/scripts/build.sh ./k230_linux_sdk
```

The BSP defconfig sets `BR2_TAR_OPTIONS="--no-same-owner"` so Linux source extraction works in containers or rootless environments that cannot restore archive uid/gid ownership.

The BSP defconfig also locks LVGL to the SDK-compatible `59dc7e436ae97a25e32656739ea6a943f9f11b6a` package revision and disables upstream `python-k230`. The Python/LVGL binding package is not part of hardware enablement and is incompatible with the current LVGL 9 generated API in this upstream SDK snapshot.

U-Boot display detection uses DesignWare DM I2C. The BSP enables `CONFIG_DM_I2C`, `CONFIG_SYS_I2C_DW`, and `CONFIG_CMD_I2C` in the T-Display U-Boot defconfig and carries the RM69A10/LT9611 default I2C address fix used during clean-build validation.

The SD-card image uses an MBR partition table. This avoids stale backup-GPT data on larger TF cards causing Linux to ignore the boot/rootfs partitions after flashing a small image.

Export images into the BSP `images/` directory:

```sh
./t-display-k230/k230_bsp/scripts/export_images.sh ./k230_linux_sdk
```

The main image is:

```text
t-display-k230/k230_bsp/images/sysimage-sdcard.img
```

If the SDK generates only `sysimage-sdcard.img.gz`, decompress it before flashing.

## Flashing

On Linux, replace `/dev/sdX` with the actual TF-card block device:

```sh
sudo dd if=t-display-k230/k230_bsp/images/sysimage-sdcard.img of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

On macOS, replace `diskN` with the actual TF-card disk:

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=t-display-k230/k230_bsp/images/sysimage-sdcard.img of=/dev/rdiskN bs=8m status=progress
sync
```

If a card was previously flashed with a GPT image and the board cannot find `/dev/mmcblk1p2`, reflash with the current MBR image.

## Launcher App

To include the bundled LVGL launcher in the image, apply the launcher after the
BSP:

```sh
./t-display-k230/k230_bsp/scripts/apply.sh ./k230_linux_sdk
./t-display-k230/k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

Then rebuild the image or rebuild only the launcher package.

## Directory Layout

```text
overlay/                 Files copied into k230_linux_sdk
scripts/apply.sh         Apply BSP overlay to an SDK checkout
scripts/build.sh         Build k230_canmv_t_display_rm69a10_defconfig
scripts/export_images.sh Copy image artifacts into images/
metadata/                Upstream SDK URL/commit and overlay manifest
docs/HARDWARE_PINMAP.md  Board GPIO and hardware map
docs/HARDWARE_PINMAP_CN.md Chinese board GPIO and hardware map
images/                  Local image export directory
```

Generated images in `images/` are intentionally ignored by git. They are copied there by `scripts/export_images.sh` for local testing and release packaging.

## Notes

- If the SDK commit differs from the validated commit, `apply.sh` warns but still copies the overlay. Build failures in that case mean the BSP must be rebased.
- This BSP keeps HDMI experimental support as an optional DTB/output path. AMOLED remains the default display target.
- RTL8723DS Bluetooth requires the BT UART pins to be connected; SDIO covers Wi-Fi only.
