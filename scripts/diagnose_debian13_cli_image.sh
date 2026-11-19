#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
CONF="${CONF:-k230_canmv_t_display_debian13_cli_defconfig}"
BR_SOURCE="${SDK_DIR}/output/buildroot-2025.02.1"
BUILD_DIR="${SDK_DIR}/output/${CONF}"
SDK_DIST="${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/distribution/distribution.sh"
SYNC_DIST="${BR_SOURCE}/board/canaan/k230-soc/distribution/distribution.sh"
TARGET_DIR="${BUILD_DIR}/target"
IMAGE="${IMAGE:-${BUILD_DIR}/images/debian.img.bak}"
SOURCE_DEFCONFIG="${REPO_DIR}/k230_bsp/overlay/buildroot-overlay/configs/${CONF}"
SDK_DEFCONFIG="${SDK_DIR}/buildroot-overlay/configs/${CONF}"
SYNC_DEFCONFIG="${BR_SOURCE}/configs/${CONF}"
MAP_REL="etc/console-setup/t-display-k230.map"
SERVICE_REL="etc/systemd/system/t-display-k230-keymap.service"
WANTS_REL="etc/systemd/system/multi-user.target.wants/t-display-k230-keymap.service"
failures=0
loop_dev=""
mount_dir=""
tmp_dir=""

pass() { printf '[OK]   %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; failures=$((failures + 1)); }
warn() { printf '[WARN] %s\n' "$*" >&2; }

check_file() {
    if [ -e "$1" ]; then
        pass "$1"
    else
        fail "$1"
    fi
}

check_marker() {
    local file="$1"
    local marker="$2"
    if [ -f "${file}" ] && grep -qF "${marker}" "${file}"; then
        pass "marker in ${file}: ${marker}"
    else
        fail "marker missing in ${file}: ${marker}"
    fi
}

cleanup() {
    if [ -n "${mount_dir}" ] && mountpoint -q "${mount_dir}"; then
        sudo umount "${mount_dir}" || true
    fi
    if [ -n "${loop_dev}" ]; then
        sudo losetup -d "${loop_dev}" || true
    fi
    if [ -n "${tmp_dir}" ] && [ -d "${tmp_dir}" ]; then
        rm -rf -- "${tmp_dir}"
    fi
}
trap cleanup EXIT

printf '%s\n' '=== Patched distribution sources ==='
for file in "${SDK_DIST}" "${SYNC_DIST}"; do
    check_marker "${file}" 'for tool in i2cdetect i2cdump i2cget i2cset i2ctransfer'
    check_marker "${file}" 't-display-k230-keymap.service'
    check_marker "${file}" 'Verified minimal CLI rootfs payload'
done

printf '%s\n' '=== Buildroot configuration and target ==='
for config in "${SOURCE_DEFCONFIG}" "${SDK_DEFCONFIG}" "${SYNC_DEFCONFIG}"; do
    for symbol in \
        BR2_PACKAGE_BUSYBOX_SHOW_OTHERS \
        BR2_PACKAGE_I2C_TOOLS \
        BR2_PACKAGE_KBD; do
        if grep -q "^${symbol}=y$" "${config}" 2>/dev/null; then
            pass "${symbol}=y in ${config}"
        else
            fail "${symbol}=y missing from ${config}"
        fi
    done
done
for symbol in \
    BR2_PACKAGE_BUSYBOX_SHOW_OTHERS \
    BR2_PACKAGE_I2C_TOOLS \
    BR2_PACKAGE_KBD; do
    if grep -q "^${symbol}=y$" "${BUILD_DIR}/.config" 2>/dev/null; then
        pass "${symbol}=y"
    else
        fail "${symbol} is missing from Buildroot .config"
    fi
done
check_file "${TARGET_DIR}/usr/sbin/i2cdetect"
check_file "${TARGET_DIR}/usr/bin/loadkeys"
check_file "${TARGET_DIR}/usr/bin/showkey"

printf '%s\n' '=== CLI overlay ==='
OVERLAY_DIR="${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/distribution/debian13_cli_overlay"
check_file "${OVERLAY_DIR}/${MAP_REL}"
check_file "${OVERLAY_DIR}/${SERVICE_REL}"
if command -v loadkeys >/dev/null 2>&1; then
    if loadkeys -b "${OVERLAY_DIR}/${MAP_REL}" >/dev/null 2>&1; then
        pass 'console keymap parses successfully'
    else
        fail 'console keymap parse failed'
    fi
else
    warn 'host loadkeys is unavailable; skipped keymap parser check'
fi

printf '%s\n' '=== Final uncompressed image ==='
check_file "${IMAGE}"
if [ -f "${IMAGE}" ]; then
    stat -c 'image: %n | size=%s | modified=%y' "${IMAGE}"
    sudo -v
    tmp_dir="$(mktemp -d)"
    mount_dir="${tmp_dir}/rootfs"
    mkdir -p "${mount_dir}"
    loop_dev="$(sudo losetup --find --show --partscan "${IMAGE}")"
    command -v udevadm >/dev/null 2>&1 && sudo udevadm settle || true
    root_part="${loop_dev}p2"
    if [ ! -b "${root_part}" ]; then
        root_part="$(lsblk -lnpo NAME,TYPE "${loop_dev}" | awk '$2 == "part" { last = $1 } END { print last }')"
    fi
    if [ -z "${root_part}" ] || [ ! -b "${root_part}" ]; then
        fail "rootfs partition not found in ${IMAGE}"
    else
        sudo mount -o ro "${root_part}" "${mount_dir}"
        check_file "${mount_dir}/usr/sbin/i2cdetect"
        check_file "${mount_dir}/usr/bin/loadkeys"
        check_file "${mount_dir}/${MAP_REL}"
        check_file "${mount_dir}/${SERVICE_REL}"
        if [ -L "${mount_dir}/${WANTS_REL}" ]; then
            pass "enabled service link: $(readlink "${mount_dir}/${WANTS_REL}")"
        else
            fail "missing service enable link: ${WANTS_REL}"
        fi
        if grep -qF 'shift keycode  16 = apostrophe' \
            "${mount_dir}/${MAP_REL}" 2>/dev/null; then
            pass "final image contains Shift+Q apostrophe mapping"
        else
            fail "final image does not contain Shift+Q apostrophe mapping"
        fi
    fi
fi

if [ "${failures}" -ne 0 ]; then
    printf '[FAIL] diagnostics found %d problem(s)\n' "${failures}" >&2
    exit 1
fi
printf '%s\n' '[OK] all Debian 13 CLI layers are present in the final image'
