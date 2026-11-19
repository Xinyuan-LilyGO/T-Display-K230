#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
CLI_DIR="${REPO_DIR}/debian13_cli"
DISTRIBUTION_SCRIPT="${SDK_DIR}/buildroot-overlay/board/canaan/k230-soc/distribution/distribution.sh"

if ! git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Missing SDK checkout: ${SDK_DIR}" >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

bash "${REPO_DIR}/k230_bsp/scripts/apply.sh" "${SDK_DIR}"
rsync -a "${CLI_DIR}/sdk-overlay/" "${SDK_DIR}/"

# Files copied through Windows/VMware may have CRLF endings and lose their
# executable bit. Normalize both sides before git apply compares contexts.
sed -i 's/\r$//' "${DISTRIBUTION_SCRIPT}"
chmod +x "${DISTRIBUTION_SCRIPT}"
find "${CLI_DIR}/patches" -maxdepth 1 -type f -name '*.patch' \
    -exec sed -i 's/\r$//' {} +

for patch in "${CLI_DIR}"/patches/*.patch; do
    patch_name="$(basename "${patch}")"
    marker=""
    case "${patch_name}" in
        0001-*) marker='K230_CLI_MINIMAL' ;;
        0002-*) marker='for tool in i2cdetect i2cdump i2cget i2cset i2ctransfer' ;;
        0003-*) marker='t-display-k230-keymap.service' ;;
        0004-*) marker='Verified minimal CLI rootfs payload' ;;
        0005-*) marker='DT_NEEDED entry for it' ;;
    esac

    if [ -n "${marker}" ] && grep -qF "${marker}" \
        "${DISTRIBUTION_SCRIPT}"; then
        echo "Already applied: ${patch_name}"
    elif git -C "${SDK_DIR}" apply --reverse --check "${patch}" >/dev/null 2>&1; then
        echo "Already applied: $(basename "${patch}")"
    elif git -C "${SDK_DIR}" apply --check "${patch}"; then
        git -C "${SDK_DIR}" apply "${patch}"
        echo "Applied: $(basename "${patch}")"
    else
        echo "Patch does not apply cleanly: ${patch}" >&2
        exit 1
    fi
done

chmod +x "${DISTRIBUTION_SCRIPT}"

echo "Debian 13 CLI board support applied to ${SDK_DIR}"
