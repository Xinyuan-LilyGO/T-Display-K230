#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"

normalize_linux_tree()
{
    local tree="$1"
    local rel
    local file

    while IFS= read -r -d '' rel; do
        file="${tree}/${rel}"
        [ -f "${file}" ] || continue
        sed -i 's/\r$//' "${file}"
    done < <(git -C "${tree}" grep -Ilz $'\r$' -- 2>/dev/null || true)

    while IFS= read -r -d '' rel; do
        file="${tree}/${rel}"
        [ -f "${file}" ] || continue
        chmod a+x "${file}"
    done < <(git -C "${tree}" grep -Ilz '^#!' -- 2>/dev/null || true)
}

if ! git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Missing SDK submodule: ${SDK_DIR}" >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

echo "Normalizing files copied from Windows..."
normalize_linux_tree "${REPO_DIR}"
normalize_linux_tree "${SDK_DIR}"

if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y git ca-certificates build-essential make rsync python3
fi

make -C "${SDK_DIR}" toolchain_and_depend
