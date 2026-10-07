<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 🌟</h1>


<p align="center">
  <img src="./images/top.png" alt="LilyGo logo" width="640" />
</p>

[English](./README.MD) | [中文](./README_CN.MD)

This repository packages the board support files, launcher application, and host
scripts for building a LILYGO T-Display K230 SD-card image from the
official Kendryte K230 Linux SDK.

## Feature Overview

See [CHANGELOG.MD](CHANGELOG.MD) for release history.

The public image includes a phone-style LVGL launcher, responsive
portrait/landscape layouts, camera and media apps, settings pages, optional
keyboard-base hardware support, nRF52840/nRF9151 management, LoRa/LoRaWAN
tools, and Meshtastic beta support.

Feature status:

- Stable daily-use apps: camera, gallery, music, network radio, recorder,
  video player, terminal, MTP, Wi-Fi, Ethernet, display, audio, notification,
  haptic, battery, charger, sensor, keyboard, and system settings.
- Beta apps: Meshtastic, LoRaWAN, Xiaozhi voice assistant, PicoClaw,
  nRF9151 cellular/GNSS data tools, and nRF52840 UART DFU.
- User-provided content: NES ROM files and offline map tiles are not bundled in
  the default image.

## Repository Layout

```text
k230_bsp/          Board support overlay for U-Boot, Linux, Buildroot, DTB, logo, and hardware services
k230_launcher/     LVGL launcher application and bundled runtime resources
k230_linux_sdk/    Official Kendryte K230 Linux SDK submodule
scripts/           Host setup, SDK overlay, image build, and SSH deployment helpers
```

## Documentation

- [Changelog](CHANGELOG.MD)
- [Launcher app guide](k230_launcher/docs/APPS.md)
- [Offline map tile guide](k230_launcher/docs/MAPS.md)
- [BSP GPIO and hardware map](k230_bsp/docs/HARDWARE_PINMAP.md)
- [BSP README](k230_bsp/README.MD)
- [Launcher README](k230_launcher/README.MD)
- [nRF52840 firmware and related documentation](https://github.com/Xinyuan-LilyGO/T-Display-K230-nRF52840)
- [nRF9151 & keyboard firmware and related documentation](https://github.com/Xinyuan-LilyGO/T-Display-K230-nRF9151)

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

For a fully clean rebuild, remove the SDK output directory, re-apply the board
overlay, then build through the helper script:

```sh
rm -rf k230_linux_sdk/output
./scripts/apply_to_sdk.sh
./scripts/build_sdcard_image.sh
```

## Build From Windows With WSL2

Building from native Windows shells such as CMD, PowerShell, Git Bash, MSYS2, or
Cygwin is not recommended. Use WSL2 with Ubuntu so the SDK sees a normal Linux
filesystem, permissions, symlinks, and shell environment.

Install Ubuntu 22.04 from an Administrator PowerShell window:

```powershell
wsl --install -d Ubuntu-22.04
```

Restart Windows if WSL asks for it, then open the Ubuntu terminal and install the
basic tools:

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

Clone and build inside the WSL Linux filesystem, for example under `~/work`.
Do not build under `/mnt/c/Users/...`; that path is slower and can break
Linux-style permissions or symlinks used by the SDK.

```sh
mkdir -p ~/work
cd ~/work
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
./scripts/setup_ubuntu.sh
./scripts/apply_to_sdk.sh
./scripts/build_sdcard_image.sh
```

The generated image is:

```text
k230_bsp/images/sysimage-sdcard.img
```

To open the image folder from Windows Explorer:

```sh
explorer.exe "$(wslpath -w k230_bsp/images)"
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

### Flash With Rufus On Windows

1. Insert the TF card into the Windows PC.
2. Download [Rufus](https://rufus.ie/en) then open Rufus.
3. In `Device`, select the TF card.
4. In `Boot selection`, choose `Disk or ISO image`, then click `SELECT`.
5. Select `sysimage-sdcard.img` from `k230_bsp/images/`.
6. Click `START`.
7. If Rufus asks for the write mode, select `Write in DD Image mode`.
8. Confirm the erase warning and wait until Rufus reports completion.
9. Eject the TF card safely and insert it into the T-Display K230.

After flashing, Windows may show a prompt asking to format an unknown partition.
Cancel that prompt. The card contains Linux partitions used by the K230 image.

## Fast SSH Deployment

When the board is already running Linux and SSH is reachable, rebuild and deploy
the launcher without reflashing the card:

```sh
./scripts/deploy_launcher.sh 192.168.1.100
```

Deploy the launcher and boot files:

```sh
./scripts/deploy_launcher.sh 192.168.1.100 --full --reboot
```

> [!IMPORTANT]
>
> Please replace the above IP addresses with the IP address of your device.
> 

## Included Hardware Support

- RM69A10 AMOLED MIPI DSI panel and U-Boot boot logo.
- GT9895 / Goodix Berlin touchscreen.
- GC2093 camera with optional DW9714 focus actuator.
- RTL8189FS SDIO Wi-Fi.
- USB Bluetooth adapter support and BlueZ runtime.
- nRF52840 UART preset.
- nRF9151 UART and enable-pin preset.
- SX1262/LR2021 LoRa SPI preset.
- MAX98357A external I2S amplifier and audio route switching.
- AHT20, BQ25896, BQ27220, XL9555, TCA8418, and optional DRV2605 I2C devices.
- Keyboard interrupt GPIO42 and keyboard backlight GPIO52/PWM4.
- PMU power key input and K230 thermal sensor support.

Generated image files under `k230_bsp/images/` are local build artifacts.

> [!IMPORTANT]
> LR2021 LILYGO 2.4 GHz HF profiles are capped at 4 dBm by default for module
> safety. The LR2021 868/915 MHz module version does not use an MXD8721 RF
> switch; it uses the DPX205850DT-4055A1 duplexer. LR2021 DIO8/DIO10 RF-switch
> control is only relevant to the 433 MHz module variant.

## Known Limitations

- Meshtastic is included as a beta feature. Core chat, node display, map view,
  and channel sharing are available. Voice/photo transfer is only available
  between K230 boards fitted with LR2021 modules; SX1262 boards support basic
  text mesh messages only.
- HDMI support is kept as a diagnostic feature. AMOLED remains the default
  display path.
- Optional hardware features are shown only when the related module is detected.
  This includes nRF9151, nRF52840, TCA8418, BQ27220, BQ25896, XL9555,
  DRV2605, and AHT20.
- Xiaozhi requires network access and a compatible Xiaozhi service. Wake-word
  listening is active only while the Xiaozhi app is open and owns the microphone.
- PicoClaw requires network access and an OpenAI-compatible model API key.
- Offline map tiles, NES ROMs, and user media files should be copied to the SD
  card by the user.
