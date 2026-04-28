#!/usr/bin/env bash
#
# M5StickS3 Emulator launcher
#
# Usage:
#   ./m5sticks3/run.sh <firmware.bin>
#   ./m5sticks3/run.sh --flash-image <merged.bin>
#
# The firmware.bin can be either:
#   1. A merged flash image (bootloader + partitions + app)
#   2. Just the app binary (requires esptool.py to merge)
#
# For PlatformIO projects, the build outputs are typically in:
#   .pio/build/m5sticks3/firmware.bin
#   .pio/build/m5sticks3/bootloader.bin
#   .pio/build/m5sticks3/partitions.bin
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${REPO_DIR}/build"
QEMU="${BUILD_DIR}/qemu-system-xtensa"

# Default flash size: 8MB (M5StickS3 has 8MB flash)
FLASH_SIZE=8388608  # 8 * 1024 * 1024

# Default PSRAM: 8MB
PSRAM_SIZE=8  # in MB

# Valid flash sizes in bytes (2MB, 4MB, 8MB, 16MB)
VALID_FLASH_SIZES=(2097152 4194304 8388608 16777216)

# Pad a flash image file to the nearest valid flash size
pad_flash_image() {
    local file="$1"
    local file_size
    file_size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)

    # Find the smallest valid size >= file_size
    local target_size=0
    for vs in "${VALID_FLASH_SIZES[@]}"; do
        if [ "$file_size" -le "$vs" ]; then
            target_size=$vs
            break
        fi
    done

    if [ "$target_size" -eq 0 ]; then
        echo "Error: Firmware image (${file_size} bytes) exceeds maximum flash size (16MB)."
        exit 1
    fi

    if [ "$file_size" -lt "$target_size" ]; then
        local pad_bytes=$((target_size - file_size))
        echo "Padding flash image from ${file_size} to ${target_size} bytes (+${pad_bytes} bytes)"
        dd if=/dev/zero bs=1 count="$pad_bytes" >> "$file" 2>/dev/null
    fi
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] <firmware>

Arguments:
  firmware              Path to firmware binary (.bin or .elf)

Options:
  --flash-image         Treat firmware as a pre-merged flash image
  --pio-dir DIR         PlatformIO build directory (auto-merges binaries)
  --gdb                 Start with GDB server on port 1234
  --no-display          Run without display window (serial only)
  -h, --help            Show this help

Examples:
  $(basename "$0") firmware.bin
  $(basename "$0") --pio-dir .pio/build/m5sticks3
  $(basename "$0") --flash-image merged_flash.bin
EOF
    exit 1
}

# Check that QEMU is built
check_qemu() {
    if [ ! -x "$QEMU" ]; then
        echo "Error: QEMU not built yet. Run 'm5sticks3/build.sh' first."
        echo "Expected: $QEMU"
        exit 1
    fi
}

# Create a merged flash image from PlatformIO build artifacts
merge_pio_image() {
    local pio_dir="$1"
    local output="$2"

    local bootloader="${pio_dir}/bootloader.bin"
    local partitions="${pio_dir}/partitions.bin"
    local firmware="${pio_dir}/firmware.bin"

    for f in "$bootloader" "$partitions" "$firmware"; do
        if [ ! -f "$f" ]; then
            echo "Error: Missing build artifact: $f"
            exit 1
        fi
    done

    # Create empty flash image
    dd if=/dev/zero of="$output" bs=1 count=$FLASH_SIZE 2>/dev/null

    # Write bootloader at 0x0000 (ESP32-S3 default)
    dd if="$bootloader" of="$output" bs=1 seek=0 conv=notrunc 2>/dev/null

    # Write partition table at 0x8000
    dd if="$partitions" of="$output" bs=1 seek=32768 conv=notrunc 2>/dev/null

    # Write app at 0x10000
    dd if="$firmware" of="$output" bs=1 seek=65536 conv=notrunc 2>/dev/null

    echo "Merged flash image: $output"
}

# Parse arguments
FLASH_IMAGE=""
PIO_DIR=""
GDB_FLAG=""
DISPLAY_FLAG=""
INPUT=""
IS_FLASH_IMAGE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash-image)
            IS_FLASH_IMAGE=true
            shift
            ;;
        --pio-dir)
            PIO_DIR="$2"
            shift 2
            ;;
        --gdb)
            GDB_FLAG="-s -S"
            shift
            ;;
        --no-display)
            DISPLAY_FLAG="-nographic"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            INPUT="$1"
            shift
            ;;
    esac
done

check_qemu

# Determine the flash image to use
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

if [ -n "$PIO_DIR" ]; then
    FLASH_IMAGE="${TMPDIR}/flash.bin"
    merge_pio_image "$PIO_DIR" "$FLASH_IMAGE"
elif [ -n "$INPUT" ]; then
    if $IS_FLASH_IMAGE; then
        # Copy to temp so we can pad without modifying the original
        FLASH_IMAGE="${TMPDIR}/flash.bin"
        cp "$INPUT" "$FLASH_IMAGE"
        pad_flash_image "$FLASH_IMAGE"
    elif [[ "$INPUT" == *.elf ]]; then
        # ELF files can be loaded directly
        exec "$QEMU" \
            -M esp32s3 \
            -m ${PSRAM_SIZE}M \
            -kernel "$INPUT" \
            -serial mon:stdio \
            $GDB_FLAG $DISPLAY_FLAG
    else
        # Assume raw app binary — create a flash image
        FLASH_IMAGE="${TMPDIR}/flash.bin"
        dd if=/dev/zero of="$FLASH_IMAGE" bs=1 count=$FLASH_SIZE 2>/dev/null
        dd if="$INPUT" of="$FLASH_IMAGE" bs=1 seek=65536 conv=notrunc 2>/dev/null
        echo "Note: Loaded app binary at offset 0x10000."
        echo "      For full boot, use --pio-dir or --flash-image with a merged image."
    fi
else
    usage
fi

# Run QEMU
exec "$QEMU" \
    -M esp32s3 \
    -m ${PSRAM_SIZE}M \
    -drive file="$FLASH_IMAGE",if=mtd,format=raw \
    -serial mon:stdio \
    $GDB_FLAG $DISPLAY_FLAG
