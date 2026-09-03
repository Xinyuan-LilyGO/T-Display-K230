# Changelog

[English](./CHANGELOG.MD) | [中文](./CHANGELOG_CN.MD)

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
