#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
CONF="${CONF:-k230_canmv_t_display_rm69a10_defconfig}"
EXPORT_DIR="${EXPORT_DIR:-${REPO_DIR}/k230_bsp/images}"

"${REPO_DIR}/scripts/apply_to_sdk.sh"
"${REPO_DIR}/k230_bsp/scripts/build.sh" "${SDK_DIR}" "${CONF}"
"${REPO_DIR}/k230_bsp/scripts/export_images.sh" "${SDK_DIR}" "${EXPORT_DIR}" "${CONF}"
