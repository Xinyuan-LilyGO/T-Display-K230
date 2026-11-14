#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
CONF="${CONF:-k230_canmv_t_display_debian13_cli_defconfig}"
EXPORT_DIR="${EXPORT_DIR:-${REPO_DIR}/k230_bsp/images}"
SDK_IMAGES="${SDK_DIR}/output/${CONF}/images"
OUTPUT_IMAGE="${EXPORT_DIR}/t-display-k230-debian13-cli.img.gz"
BR_SOURCE="${SDK_DIR}/output/buildroot-2025.02.1"
DISTRIBUTION_SCRIPT="${BR_SOURCE}/board/canaan/k230-soc/distribution/distribution.sh"
ACTIVE_CONFIG="${SDK_DIR}/output/${CONF}/.config"

bash "${REPO_DIR}/scripts/apply_debian13_cli_to_sdk.sh"

# Always regenerate the active Buildroot configuration from the CLI defconfig.
# The SDK Makefile otherwise reuses an existing .config, even after the
# defconfig has gained newly required packages.
make -C "${SDK_DIR}" CONF="${CONF}" "${CONF}"

for symbol in \
    BR2_PACKAGE_BUSYBOX_SHOW_OTHERS \
    BR2_PACKAGE_I2C_TOOLS \
    BR2_PACKAGE_KBD; do
    if ! grep -q "^${symbol}=y$" "${ACTIVE_CONFIG}"; then
        echo "Missing required Buildroot option: ${symbol}=y" >&2
        echo "Active configuration: ${ACTIVE_CONFIG}" >&2
        exit 1
    fi
done

make -C "${SDK_DIR}" CONF="${CONF}" buildroot
make -C "${SDK_DIR}" CONF="${CONF}" sync
sudo env K230_CLI_MINIMAL=1 \
    bash "${DISTRIBUTION_SCRIPT}" debian "${SDK_DIR}/output/${CONF}"

if [ ! -f "${SDK_IMAGES}/debian.img.gz" ]; then
    echo "Missing Debian image: ${SDK_IMAGES}/debian.img.gz" >&2
    exit 1
fi

SDK_DIR="${SDK_DIR}" CONF="${CONF}" \
    bash "${REPO_DIR}/scripts/diagnose_debian13_cli_image.sh"

mkdir -p "${EXPORT_DIR}"
sudo cp -fL "${SDK_IMAGES}/debian.img.gz" "${OUTPUT_IMAGE}"
sudo chown "$(id -u):$(id -g)" "${OUTPUT_IMAGE}"
sha256sum "${OUTPUT_IMAGE}" > "${OUTPUT_IMAGE}.sha256"

echo "Debian 13 CLI image: ${OUTPUT_IMAGE}"
echo "Checksum: ${OUTPUT_IMAGE}.sha256"
