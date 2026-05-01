#!/usr/bin/env bash
#
# Build the M5StickS3 emulator (QEMU with GPSPI + ST7789V2 + GPIO)
#
# Usage:
#   ./m5sticks3/build.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${REPO_DIR}/build"

# Ensure CC/CXX point to real compilers, not shell aliases.
# (Some users alias 'cc' to other tools, which breaks configure.)
if [ -z "${CC:-}" ]; then
    if [ -x /usr/bin/cc ]; then
        export CC=/usr/bin/cc
    elif command -v gcc >/dev/null 2>&1; then
        export CC=gcc
    fi
fi
if [ -z "${CXX:-}" ]; then
    if [ -x /usr/bin/c++ ]; then
        export CXX=/usr/bin/c++
    elif command -v g++ >/dev/null 2>&1; then
        export CXX=g++
    fi
fi

cd "$REPO_DIR"

# Configure if not already done
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "==> Configuring QEMU build..."
    ./configure \
        --target-list=xtensa-softmmu \
        --prefix="${REPO_DIR}/install" \
        --enable-sdl \
        --enable-gcrypt \
        --disable-werror
fi

echo "==> Building QEMU (xtensa-softmmu)..."
cd "$BUILD_DIR"
ninja qemu-system-xtensa

echo ""
echo "Build complete: ${BUILD_DIR}/qemu-system-xtensa"
echo "Run with: ./m5sticks3/run.sh <firmware.bin>"
