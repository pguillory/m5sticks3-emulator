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

cd "$REPO_DIR"

# Configure if not already done
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    echo "==> Configuring QEMU build..."
    ./configure \
        --target-list=xtensa-softmmu \
        --prefix="${REPO_DIR}/install" \
        --enable-sdl \
        --disable-werror
fi

echo "==> Building QEMU (xtensa-softmmu)..."
cd "$BUILD_DIR"
ninja qemu-system-xtensa

echo ""
echo "Build complete: ${BUILD_DIR}/qemu-system-xtensa"
echo "Run with: ./m5sticks3/run.sh <firmware.bin>"
