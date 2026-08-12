# T-Display K230 Launcher

This directory contains the phone-style LVGL launcher application for the
LILYGO T-Display K230 RM69A10 board.

## Prerequisites

Install basic host tools first:

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

Install and apply the BSP first:

```sh
git clone --recurse-submodules <this-repository-url> t-display-k230
cd t-display-k230
./scripts/apply_to_sdk.sh
```

The BSP README explains the full SDK dependency setup and image build flow.

## Install Into The SDK

Clone or copy this launcher next to the SDK, then run:

```sh
./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

The script installs:

- `buildroot-overlay/package/k230_phone_ui`
- `BR2_PACKAGE_K230_PHONE_UI=y` in `k230_canmv_t_display_rm69a10_defconfig`
- `S99zz_k230_phone_ui` autostart service
- default media folders: `/root/music`, `/root/nes`, `/root/videos`, `/root/photos`, `/root/screenshots`, `/root/recordings`, `/root/lorawan`
- optional bundled media from `resources/videos` into `/root/videos`
- optional bundled media from `resources/music` into `/root/music`

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
- Press `FN + LILYGO` on the hardware keyboard.

Use the MTP app to expose the SD-card root directory to a host computer, then copy files from `/root/screenshots`.

## Notes

The launcher expects the BSP-provided hardware interfaces and runtime tools.

For iterative development, edit this repository, rerun `scripts/install_to_sdk.sh`, and rebuild the SDK or the Buildroot package as needed.
