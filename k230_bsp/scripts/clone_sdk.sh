#!/usr/bin/env bash
set -euo pipefail

DEST="${1:-k230_linux_sdk}"
BSP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_URL="$(sed -n '1p' "${BSP_DIR}/metadata/upstream_sdk_url.txt")"
SDK_COMMIT="$(sed -n '1p' "${BSP_DIR}/metadata/upstream_sdk_commit.txt")"

git clone "${SDK_URL}" "${DEST}"
git -C "${DEST}" checkout "${SDK_COMMIT}"

echo "Cloned ${SDK_URL} at ${SDK_COMMIT} into ${DEST}"
