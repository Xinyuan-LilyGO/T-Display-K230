#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 /path/to/k230_linux_sdk" >&2
    exit 1
fi

SDK_DIR="$(cd "$1" && pwd)"
BSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Not a git checkout: ${SDK_DIR}" >&2
    exit 1
fi

EXPECTED_COMMIT="$(sed -n '1p' "${BSP_DIR}/metadata/upstream_sdk_commit.txt")"
CURRENT_COMMIT="$(git -C "${SDK_DIR}" rev-parse HEAD)"

echo "T-Display K230 BSP apply"
echo "SDK: ${SDK_DIR}"
echo "BSP: ${BSP_DIR}"
echo "Expected upstream commit: ${EXPECTED_COMMIT}"
echo "Current SDK commit     : ${CURRENT_COMMIT}"

if [ "${CURRENT_COMMIT}" != "${EXPECTED_COMMIT}" ]; then
    echo "Warning: SDK commit differs from the BSP validated upstream commit." >&2
    echo "The overlay will still be copied, but build failures may require rebasing." >&2
fi

PRUNE_PATHS=(
    "buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S00resizemmc"
    "buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/first_boot_flag"
)

echo "[1/4] Copy BSP overlay"
echo "Prune stale test-only patch overlay"
rm -f "${SDK_DIR}"/buildroot-overlay/linux/*ov5647*camera-profile*.patch
rm -f "${SDK_DIR}"/buildroot-overlay/linux/*imx219*camera-profile*.patch
rsync -a "${BSP_DIR}/overlay/" "${SDK_DIR}/"

echo "[2/4] Prune unsupported upstream first-boot resize files"
for rel in "${PRUNE_PATHS[@]}"; do
    rm -f "${SDK_DIR}/${rel}"
done

find "${SDK_DIR}/output" -path "*/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S00resizemmc" -delete 2>/dev/null || true
find "${SDK_DIR}/output" -path "*/board/canaan/k230-soc/rootfs_overlay/first_boot_flag" -delete 2>/dev/null || true
find "${SDK_DIR}/output" -path "*/target/etc/init.d/S00resizemmc" -delete 2>/dev/null || true
find "${SDK_DIR}/output" -path "*/target/first_boot_flag" -delete 2>/dev/null || true
find "${SDK_DIR}/output" -maxdepth 3 \( -type f -o -type l \) \
    \( -name "rootfs.ext2" -o -name "rootfs.ext4" -o -name "rootfs.tar" \) \
    -delete 2>/dev/null || true

echo "[3/4] Invalidate generated Buildroot overlay sync stamps"
while IFS= read -r stamp; do
    backup="${stamp}.stale.$(date -u +%Y%m%d%H%M%S)"
    mv "${stamp}" "${backup}"
    echo "Moved ${stamp} -> ${backup}"
done < <(find "${SDK_DIR}/output" -maxdepth 2 -name .overlay_sync -type f 2>/dev/null || true)

echo "[4/4] Resulting SDK status"
git -C "${SDK_DIR}" status --short | sed -n '1,160p'

echo "Done."
