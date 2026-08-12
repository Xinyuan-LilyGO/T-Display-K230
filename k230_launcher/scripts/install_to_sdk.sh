#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 /path/to/k230_linux_sdk [defconfig]" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK_DIR="$(cd "$1" && pwd)"
CONF="${2:-k230_canmv_t_display_rm69a10_defconfig}"
RESOURCE_DIR="${K230_RESOURCE_DIR:-$LAUNCHER_DIR/resources}"

PACKAGE_DIR="$SDK_DIR/buildroot-overlay/package/k230_phone_ui"
ROOTFS_DIR="$SDK_DIR/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay"
CONFIG_IN="$SDK_DIR/buildroot-overlay/package/Config_canaan.in"
DEFCONFIG="$SDK_DIR/buildroot-overlay/configs/$CONF"
STAMP="$SDK_DIR/.overlay_sync"
OUTPUT_BUILDROOT_PACKAGE_DIR="$SDK_DIR/output/buildroot-2025.02.1/package/k230_phone_ui"
OUTPUT_CONF_DIR="$SDK_DIR/output/$CONF"
OUTPUT_PACKAGE_BUILD_DIR="$OUTPUT_CONF_DIR/build/k230_phone_ui"
OUTPUT_TARGET_APP_DIR="$OUTPUT_CONF_DIR/target/root/app/k230_phone_ui"

if [ ! -d "$SDK_DIR/.git" ]; then
    echo "ERROR: $SDK_DIR is not a git checkout" >&2
    exit 1
fi

if [ ! -f "$CONFIG_IN" ]; then
    echo "ERROR: missing $CONFIG_IN; apply k230_bsp first" >&2
    exit 1
fi

if [ ! -f "$DEFCONFIG" ]; then
    echo "ERROR: missing $DEFCONFIG; apply k230_bsp first" >&2
    exit 1
fi

mkdir -p "$PACKAGE_DIR"
rsync -a --delete "$LAUNCHER_DIR/k230_phone_ui/" "$PACKAGE_DIR/"

# The SDK's overlay sync and Buildroot package rsync do not delete files that
# disappeared after switching launcher branches. Remove generated copies here so
# dev-only files cannot leak into a main image, or the other way around.
rm -rf "$OUTPUT_BUILDROOT_PACKAGE_DIR" "$OUTPUT_PACKAGE_BUILD_DIR" "$OUTPUT_TARGET_APP_DIR"

if ! grep -q 'source "package/k230_phone_ui/Config.in"' "$CONFIG_IN"; then
    {
        echo
        echo 'source "package/k230_phone_ui/Config.in"'
    } >> "$CONFIG_IN"
fi

if grep -q '^# BR2_PACKAGE_K230_PHONE_UI is not set' "$DEFCONFIG"; then
    sed -i 's/^# BR2_PACKAGE_K230_PHONE_UI is not set/BR2_PACKAGE_K230_PHONE_UI=y/' "$DEFCONFIG"
elif ! grep -q '^BR2_PACKAGE_K230_PHONE_UI=y' "$DEFCONFIG"; then
    echo 'BR2_PACKAGE_K230_PHONE_UI=y' >> "$DEFCONFIG"
fi

install -D -m 0755 "$LAUNCHER_DIR/rootfs_overlay/etc/init.d/S99zz_k230_phone_ui" \
    "$ROOTFS_DIR/etc/init.d/S99zz_k230_phone_ui"

mkdir -p "$ROOTFS_DIR/root/music" "$ROOTFS_DIR/root/nes" "$ROOTFS_DIR/root/videos" \
    "$ROOTFS_DIR/root/photos" "$ROOTFS_DIR/root/screenshots" \
    "$ROOTFS_DIR/root/recordings" "$ROOTFS_DIR/root/lorawan"

sync_media_dir() {
    local src="$1"
    local dst="$2"
    local label="$3"

    if [ -d "$src" ]; then
        rsync -a --delete "$src"/ "$dst"/
        echo "Installed $label media from $src"
    else
        echo "No $label media source found at $src; keeping $dst as-is"
    fi
}

sync_media_dir "$RESOURCE_DIR/videos" "$ROOTFS_DIR/root/videos" "video"
sync_media_dir "$RESOURCE_DIR/music" "$ROOTFS_DIR/root/music" "music"

if [ -e "$STAMP" ]; then
    mv "$STAMP" "$STAMP.stale.$(date +%Y%m%d_%H%M%S)"
fi

echo "Installed k230_launcher into $SDK_DIR"
echo "Defconfig: $CONF"
echo "Next build:"
echo "  make -C \"$SDK_DIR\" CONF=\"$CONF\" \"$CONF\""
echo "  make -C \"$SDK_DIR\" CONF=\"$CONF\" all"
