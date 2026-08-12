#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 /path/to/k230_linux_sdk [output_dir] [defconfig]" >&2
    exit 1
fi

SDK_DIR="$(cd "$1" && pwd)"
BSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${2:-${BSP_DIR}/images}"
CONF="${3:-k230_canmv_t_display_rm69a10_defconfig}"
SDK_IMAGES="${SDK_DIR}/output/${CONF}/images"
UPSTREAM_COMMIT="$(git -C "${SDK_DIR}" rev-parse HEAD)"
BSP_COMMIT="not-a-git-repo"

if [ -d "${BSP_DIR}/.git" ]; then
    BSP_COMMIT="$(git -C "${BSP_DIR}" rev-parse HEAD)"
fi

if [ ! -d "${SDK_IMAGES}" ]; then
    echo "Missing SDK images directory: ${SDK_IMAGES}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

for name in \
    sysimage-sdcard.img \
    sysimage-sdcard.img.gz \
    Image \
    k230-canmv-rm69a10.dtb \
    k230-canmv-rm69a10-hdmi.dtb \
    k.dtb \
    logo.xrgb \
    logo.yuv; do
    if [ -f "${SDK_IMAGES}/${name}" ]; then
        cp -a "${SDK_IMAGES}/${name}" "${OUT_DIR}/"
    fi
done

cat > "${OUT_DIR}/BUILD_INFO.txt" <<EOF
T-Display K230 BSP image
Build UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)
SDK URL: $(sed -n '1p' "${BSP_DIR}/metadata/upstream_sdk_url.txt")
SDK commit: ${UPSTREAM_COMMIT}
Expected SDK commit: $(sed -n '1p' "${BSP_DIR}/metadata/upstream_sdk_commit.txt")
BSP commit: ${BSP_COMMIT}
Defconfig: ${CONF}
SDK images: ${SDK_IMAGES}
EOF

(cd "${OUT_DIR}" && find . -maxdepth 1 -type f ! -name SHA256SUMS.txt ! -name .gitkeep -printf '%P\0' | LC_ALL=C sort -z | xargs -0 sha256sum > SHA256SUMS.txt)

echo "Exported images to ${OUT_DIR}"
