<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 🌟</h1>

This repository packages the board support files, launcher application, and host
scripts for building a LILYGO T-Display K230 SD-card image from the
official Kendryte K230 Linux SDK.

Validated upstream SDK:

```text
https://github.com/kendryte/k230_linux_sdk.git
dev commit 22d02c6b6783a57a3aca7eb3160e313e772cb710
```

## Repository Layout

```text
k230_bsp/          Board support overlay for U-Boot, Linux, Buildroot, DTB, logo, and hardware services
k230_launcher/     LVGL launcher application and bundled runtime resources
k230_linux_sdk/    Official Kendryte K230 Linux SDK submodule
scripts/           Host setup, SDK overlay, image build, and SSH deployment helpers
```

## Build From A Fresh Ubuntu PC

Ubuntu 22.04 or 24.04 is recommended.

Install Git and basic build tools:

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

Clone this repository with the SDK submodule:

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
```

If the repository was cloned without submodules:

```sh
git submodule update --init --recursive
```

Install the SDK toolchain and host dependencies:

```sh
./scripts/setup_ubuntu.sh
```

Apply the board support files and launcher to the SDK:

```sh
./scripts/apply_to_sdk.sh
```

Build the SD-card image:

```sh
./scripts/build_sdcard_image.sh
```

The image is exported to:

```text
k230_bsp/images/sysimage-sdcard.img
```

## Flashing

On Linux, replace `/dev/sdX` with the actual TF-card block device:

```sh
sudo dd if=k230_bsp/images/sysimage-sdcard.img of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

On macOS, replace `diskN` with the actual TF-card disk:

```sh
diskutil unmountDisk /dev/diskN
sudo dd if=k230_bsp/images/sysimage-sdcard.img of=/dev/rdiskN bs=8m status=progress
sync
```

## Fast SSH Deployment

When the board is already running Linux and SSH is reachable, rebuild and deploy
the launcher without reflashing the card:

```sh
./scripts/deploy_launcher.sh 192.168.36.232
```

Deploy the launcher and boot files:

```sh
./scripts/deploy_launcher.sh 192.168.36.232 --full --reboot
```

## Included Hardware Support

- RM69A10 AMOLED MIPI DSI panel and U-Boot boot logo.
- GT9895 / Goodix Berlin touchscreen.
- GC2093 camera.
- RTL8189FS and RTL8723DS SDIO Wi-Fi.
- USB Bluetooth adapter support and BlueZ runtime.
- nRF52840 UART preset.
- nRF9151 UART and enable-pin preset.
- SX1262/LR2021 LoRa SPI preset.
- MAX98357A external I2S amplifier and audio route switching.
- AHT20, BQ25896, BQ27220, XL9555, and TCA8418 keyboard-base I2C devices.
- Keyboard interrupt GPIO42 and keyboard backlight GPIO52/PWM4.
- PMU power key input and K230 thermal sensor support.

Generated image files under `k230_bsp/images/` are local build artifacts.
