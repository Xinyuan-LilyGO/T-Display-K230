<div align="center">
  <img src=".github/LilyGo_logo.png" alt="LILYGO" width="96">
  <h1>T-Display K230 Debian 13 CLI</h1>
</div>

[English](README.MD) | [中文](README_CN.MD)

<p align="center">
  <img src="./images/debian.jpg" alt="LilyGo logo" width="320" />
</p>

Debian 13 CLI image support for the LILYGO T-Display K230, built on the
official Kendryte K230 Linux SDK.

This branch replaces the LVGL/MicroPython-oriented userspace with a Debian 13
console system. It adds the RM69A10 framebuffer console, TCA8418 keyboard
support, a console keymap, and Debian-specific build and validation scripts.

## Layout

```text
debian13_cli/    Debian patches and rootfs overlay
k230_bsp/        U-Boot, Linux, device-tree and Buildroot board support
k230_linux_sdk/  Official K230 Linux SDK submodule
scripts/         Setup, build and image validation scripts
```

## Build

The SDK toolchain requires an x86_64 Linux environment. Ubuntu 22.04 or 24.04
is recommended.

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git
cd T-Display-K230
./scripts/setup_ubuntu.sh
./scripts/build_debian13_cli.sh
```

If the repository was cloned without its submodule:

```sh
git submodule update --init --recursive
```

The build script applies the BSP and Debian patches, builds the SDK, validates
the final root filesystem, and writes:

```text
k230_bsp/images/t-display-k230-debian13-cli.img.gz
k230_bsp/images/t-display-k230-debian13-cli.img.gz.sha256
```

### Windows with WSL2

Install Ubuntu from an Administrator PowerShell window:

```powershell
wsl --install -d Ubuntu-24.04
```

Clone and build inside the WSL Linux filesystem, such as `~/work`. Avoid
building below `/mnt/c` or `/mnt/e` because the SDK depends on Linux file
permissions and symbolic links.

```sh
mkdir -p ~/work && cd ~/work
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git
cd T-Display-K230
./scripts/setup_ubuntu.sh
./scripts/build_debian13_cli.sh
```

Open the output directory in Windows Explorer with:

```sh
explorer.exe "$(wslpath -w k230_bsp/images)"
```

### macOS

Native macOS builds are not supported. Build in an x86_64 Ubuntu 22.04/24.04
virtual machine or on a remote x86_64 Ubuntu host, then copy the generated
`.img.gz` file back to macOS. On Apple Silicon, an ARM64 Ubuntu VM is not
sufficient because the supplied SDK host toolchain is built for x86_64 Linux.

## Flash to a TF Card

Flashing erases the selected device. Verify the device name before writing.

### Ubuntu

Find the TF card with `lsblk`, then replace `/dev/sdX` below with the whole
device, not a partition such as `/dev/sdX1`.

```sh
gzip -dc k230_bsp/images/t-display-k230-debian13-cli.img.gz | \
  sudo dd of=/dev/sdX bs=8M status=progress conv=fsync
sync
```

### Windows

Extract the `.img.gz` file with 7-Zip. Open Rufus, select the TF card and the
extracted `.img` file, then write it in DD Image mode. Cancel any Windows prompt
to format the Linux partitions after flashing.

### macOS

Find the card with `diskutil list`, replace `diskN` with the whole TF-card disk,
and use the raw `rdiskN` device for writing:

```sh
diskutil unmountDisk /dev/diskN
gzip -dc k230_bsp/images/t-display-k230-debian13-cli.img.gz | \
  sudo dd of=/dev/rdiskN bs=8m
sync
diskutil eject /dev/diskN
```

Insert the TF card into the T-Display K230 and power it on. Debian starts on the
AMOLED console; the serial console remains available for diagnostics.
