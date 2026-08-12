#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 /path/to/k230_linux_sdk [defconfig]" >&2
    exit 1
fi

SDK_DIR="$(cd "$1" && pwd)"
CONF="${2:-k230_canmv_t_display_rm69a10_defconfig}"

if ! git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Not a git checkout: ${SDK_DIR}" >&2
    exit 1
fi

echo "Build SDK: ${SDK_DIR}"
echo "Defconfig: ${CONF}"

make -C "${SDK_DIR}" CONF="${CONF}" "${CONF}"
make -C "${SDK_DIR}" CONF="${CONF}" all
