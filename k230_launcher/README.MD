<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 Launcher 🌟</h1>

[English](./README.MD) | [中文](./README_CN.MD)

This directory contains the phone-style LVGL launcher application for the
LILYGO T-Display K230.

## Release Scope

This launcher is the user interface layer used by the public image. It is
designed for the BSP-provided RM69A10 display, Goodix touch input, GC2093
camera, audio routes, optional keyboard base, LoRa module, nRF52840, and
nRF9151.

Main app groups:

- Camera, gallery, screenshot, video, music, network radio, recorder, mic FFT,
  I2S test, terminal, NES, MTP, and display test.
- Network and system settings for Wi-Fi, Ethernet, Bluetooth, display, audio,
  notification sounds, language, time, sensors, charger, battery, keyboard,
  F-key hotkeys, and system information.
- LoRa profile testing, LoRaWAN profile management, nRF52840 DFU, nRF9151
  cellular/GNSS/HTTP/MQTT tools, Meshtastic beta, and Xiaozhi voice assistant.

Apps that require optional hardware check for the related module before use. If
the module is not present, the launcher should show a user-facing notice instead
of entering a broken page.

Release history is tracked in the top-level [CHANGELOG.MD](../CHANGELOG.MD).

## Prerequisites

Install basic host tools first:

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

Install and apply the BSP first:

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
./scripts/apply_to_sdk.sh
```

The BSP README explains the full SDK dependency setup and image build flow.

## App Guide

See [docs/APPS.md](docs/APPS.md) for the usage guide of each launcher app.
See [docs/MAPS.md](docs/MAPS.md) for offline map tile download and install
instructions.
See [docs/APP_DEVELOPMENT_RULES.md](docs/APP_DEVELOPMENT_RULES.md) for
launcher app development rules and optional-peripheral handling.

Meshtastic is included as a beta app. Its voice/photo transfer requires LR2021
modules on both K230 boards; SX1262 boards support basic text mesh messages
only. Xiaozhi is also beta and requires network access plus a compatible
Xiaozhi service.

PicoClaw model profiles can be provisioned quickly over SSH, which avoids
typing long API keys with the on-screen keyboard:

```sh
/root/app/k230_phone_ui/k230_picoclaw_ctl.sh profile-add \
  deepseek-chat \
  deepseek/deepseek-chat \
  https://api.deepseek.com/v1 \
  'sk-xxxx' \
  'DeepSeek Chat'
```

The command adds or updates the matching profile and makes it active. Check the
current configuration with:

```sh
/root/app/k230_phone_ui/k230_picoclaw_ctl.sh profile-list
/root/app/k230_phone_ui/k230_picoclaw_ctl.sh status
```

`profile-list` shows the active profile. In `status`, `model_key=ready` means
the active profile has an API key. Use `profile-use INDEX` to switch profiles.

## Install Into The SDK

Clone or copy this launcher next to the SDK, then run:

```sh
./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

The script installs:

- `buildroot-overlay/package/k230_phone_ui`
- `BR2_PACKAGE_K230_PHONE_UI=y` in `k230_canmv_t_display_rm69a10_defconfig`
- `S99zz_k230_phone_ui` autostart service
- default media folders: `/root/music`, `/root/nes`, `/root/videos`, `/root/photos`, `/root/screenshots`, `/root/recordings`, `/root/lorawan`, `/root/meshtastic`, `/root/notification`, `/root/nrf52840/firmware`
- optional bundled media from `resources/videos` into `/root/videos`
- optional bundled media from `resources/music` into `/root/music`
- optional bundled notification sounds from `resources/notification` into `/root/notification`
- optional bundled PicoClaw files from `resources/picoclaw`
- a runtime map cache policy for `/root/maps`; map tiles are not bundled into the default image

To use a different local resource directory:

```sh
K230_RESOURCE_DIR=/path/to/resource ./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

Then build:

```sh
make -C ./k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig k230_canmv_t_display_rm69a10_defconfig
make -C ./k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig all
```

The SD-card image is generated under:

```text
k230_linux_sdk/output/k230_canmv_t_display_rm69a10_defconfig/images/sysimage-sdcard.img
```

## Screenshot Export

The launcher can save a PNG screenshot to `/root/screenshots`.

Supported triggers:

- Tap the `Screenshot` icon on the home screen.
- Press the default `F7` hotkey.
- Press `FN + LILYGO` on the hardware keyboard.

Use the MTP app to expose the SD-card root directory to a host computer, then copy files from `/root/screenshots`.

## User Data Folders

The launcher expects these folders on the device:

```text
/root/music                Local music files
/root/videos               Video files
/root/photos               Camera photos and thumbnails
/root/screenshots          PNG screenshots
/root/recordings           Recorder output
/root/nes                  User-provided NES ROM files
/root/lorawan              LoRaWAN profile JSON files
/root/meshtastic           Meshtastic channel/profile/cache files
/root/notification         Notification sound files
/root/nrf52840/firmware    nRF52840 DFU package, normally firmware.zip
/root/maps                 Optional offline map tiles copied by the user
```

NES ROMs and offline map tiles are not bundled by default. See
[docs/MAPS.md](docs/MAPS.md) for the expected map tile directory layout and
download options.

## Notes

The launcher expects the BSP-provided hardware interfaces and runtime tools.

For iterative development, edit this repository, rerun `scripts/install_to_sdk.sh`, and rebuild the SDK or the Buildroot package as needed.
