# Changelog

[English](./CHANGELOG.MD) | [中文](./CHANGELOG_CN.MD)

## v0.2.8 - 2026-10-20

- Updated the K230 Linux SDK submodule to upstream `dev` commit
  `fc6cc2151d54cb307be47f81815023f7ffb0eae6` and adapted the BSP image layout
  while preserving the T-Display boot partition offsets.
- Aligned the RM69A10 U-Boot DSI D-PHY timing with the Linux 594 Mbps lane
  configuration so the boot logo and Linux display use consistent timing.
- Added K230 VPU hardware decoding and scaling for H.264 and HEVC video to
  improve playback smoothness and reduce CPU load.
- Added automatic software-decoder fallback for video streams unsupported by
  the hardware decoder. Set `video.hw_decode=0` in `settings.conf` to force
  software decoding.

## v0.2.7 - 2026-10-08

- Fixed the RM69A10 display intermittently shifting the whole image a few
  pixels sideways after a warm boot. The DSI D-PHY frequency range is now
  derived from the 594 Mbps lane rate instead of a fixed value meant for
  about 450 Mbps.

## v0.2.6 - 2026-10-07

- Added NES mapper 163 support and improved unsupported/invalid ROM handling
  before launch.
- Updated LR2021 LILYGO RF switch handling for DIO6/DIO7/DIO8/DIO10 routing.
- Limited LR2021 LILYGO 2.4 GHz HF profile power to 4 dBm and documented
  LR2021 module RF-switch differences.

## v0.2.5 - 2026-09-29

- Added optional DRV2605 haptics support and related settings entries.
- Added camera focus support for DW9714-based modules, including tap focus and
  autofocus stability improvements.
- Added QR code scanning through the camera app flow.
- Improved camera startup reliability and focus settling behavior.
- Improved music and NES list navigation, long-title handling, and portrait
  layout bounds.
- Added invalid NES ROM protection to avoid crashes when a ROM file is empty or
  incomplete.
- Improved MTP storage-space reporting and write-error handling so failed file
  copies are easier to diagnose.

## v0.2.4 - 2026-09-03

- Added PicoClaw chat app with model profiles, API-key storage, SSH profile
  provisioning, and Weixin QR login.
- Added on-device Meshtastic map tile download with built-in tile sources,
  custom source persistence, network checks, and download confirmation.
- Added storage expansion settings for SD cards larger than the flashed image.
- Improved I2C Scan with asynchronous scanning across board and extension buses.
- Improved LoRa tools with radio model persistence and cleaner RF test profile
  controls.
- Updated default hardware F-key actions: F3/F4 keyboard backlight down/up,
  F7 screenshot.
- Refined portrait and landscape layouts for MTP, Motion, LoRa, PicoClaw, and
  related settings pages.

## v0.2.3

- Refined MTP landscape layout bounds.
- Improved Motion app layout in landscape mode.
- Set the default low-battery shutdown threshold.

## v0.2.2

- Added Xiaozhi voice assistant with wake-word entry, push-to-talk, robot face
  mode, and automatic session recovery.
- Improved nRF52840 ownership coordination between Meshtastic BLE and UART DFU.
- Improved first-run charger, battery, and low-voltage shutdown defaults.
- Improved repeatable SD-card image generation.
