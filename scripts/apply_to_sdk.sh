#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
CONF="${CONF:-k230_canmv_t_display_rm69a10_defconfig}"

"${REPO_DIR}/k230_bsp/scripts/apply.sh" "${SDK_DIR}"
"${REPO_DIR}/k230_launcher/scripts/install_to_sdk.sh" "${SDK_DIR}" "${CONF}"
