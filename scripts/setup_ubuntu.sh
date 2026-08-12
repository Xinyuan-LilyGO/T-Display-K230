#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"

if ! git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Missing SDK submodule: ${SDK_DIR}" >&2
    echo "Run: git submodule update --init --recursive" >&2
    exit 1
fi

if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y git ca-certificates build-essential make rsync python3
fi

make -C "${SDK_DIR}" toolchain_and_depend
