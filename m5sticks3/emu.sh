#!/usr/bin/env bash
#
# M5StickS3 Emulator — headless CLI control
#
# Usage:
#   emu.sh start <firmware.bin>   Launch QEMU headless with QMP control socket
#   emu.sh screenshot [out.ppm]   Capture display to file (default: .qemu-screen.ppm)
#   emu.sh press <btnA|btnB>      Inject a GPIO button press (requires custom QMP cmd)
#   emu.sh serial                 Print serial output log
#   emu.sh stop                   Kill QEMU and clean up
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
QEMU="${REPO_DIR}/build/qemu-system-xtensa"

SOCK=".qemu.sock"
PID_FILE=".qemu.pid"
SERIAL_LOG=".qemu-serial.log"
FLASH_SIZE=8388608  # 8MB

# --- helpers ---

die()  { echo "Error: $*" >&2; exit 1; }

require_running() {
    [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null \
        || die "emulator is not running (no live $PID_FILE)"
}

cleanup_stale() {
    if [ -f "$PID_FILE" ]; then
        local pid; pid=$(cat "$PID_FILE")
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Cleaning up stale PID file (process $pid is dead)"
            rm -f "$PID_FILE" "$SOCK"
        fi
    elif [ -e "$SOCK" ] && ! [ -f "$PID_FILE" ]; then
        echo "Cleaning up orphaned socket"
        rm -f "$SOCK"
    fi
}

# Send a QMP command. Performs the capabilities handshake each time.
# Usage: qmp_cmd '{"execute":"screendump","arguments":{"filename":"/tmp/x.ppm"}}'
qmp_cmd() {
    command -v socat >/dev/null 2>&1 || die "socat is required but not installed"
    require_running
    local cmd="$1"
    # greeting → capabilities → command → read response
    # Use -t2 to wait up to 2s for response after sending commands
    {
        printf '%s\n%s\n' '{"execute":"qmp_capabilities"}' "$cmd"
        sleep 2
    } | socat -t2 STDIO UNIX-CONNECT:"$SOCK" 2>/dev/null \
        | grep -v '"QMP"' | grep -v '"qmp_capabilities"' | tail -1
}

# Detect whether a binary is a merged flash image (has partition table at 0x8000).
is_merged_flash_image() {
    local src="$1"
    local file_size
    file_size=$(stat -f%z "$src" 2>/dev/null || stat -c%s "$src" 2>/dev/null)
    # Must be at least 0x8002 bytes to check partition table magic
    [ "$file_size" -ge 32770 ] || return 1
    # ESP partition table entries start with magic bytes 0xAA 0x50
    local magic; magic=$(xxd -s 0x8000 -l 2 -p "$src")
    [ "$magic" = "aa50" ]
}

# Prepare a padded flash image from a firmware binary.
# Auto-detects whether the input is a merged flash image or an app-only binary.
prepare_flash() {
    local src="$1" dst="$2"
    # Create 8MB zero-filled image
    dd if=/dev/zero of="$dst" bs=1048576 count=8 2>/dev/null
    if is_merged_flash_image "$src"; then
        # Merged image: copy as-is at offset 0
        dd if="$src" of="$dst" bs=4096 conv=notrunc 2>/dev/null
    else
        # App-only binary: place at 0x10000 (no bootloader/partition table)
        echo "Warning: firmware appears to be app-only (no partition table at 0x8000)."
        echo "         For full boot, provide a merged flash image."
        dd if="$src" of="$dst" bs=4096 seek=16 conv=notrunc 2>/dev/null
    fi
}

# --- subcommands ---

cmd_start() {
    local fw="${1:?Usage: emu.sh start <firmware.bin>}"
    [ -f "$fw" ] || die "firmware not found: $fw"
    [ -x "$QEMU" ] || die "QEMU not built — run m5sticks3/build.sh first"

    cleanup_stale
    [ -e "$SOCK" ] && die "emulator already running ($SOCK exists)"

    local tmpflash; tmpflash=$(mktemp "${TMPDIR:-/tmp}/emu-flash-XXXXXX")
    prepare_flash "$fw" "$tmpflash"

    "$QEMU" \
        -M esp32s3 \
        -m 8M \
        -drive file="$tmpflash",if=mtd,format=raw \
        -display none \
        -serial file:"$SERIAL_LOG" \
        -qmp unix:"$SOCK",server,wait=off \
        -daemonize \
        -pidfile "$PID_FILE"

    echo "Emulator started (PID $(cat "$PID_FILE"))"
}

cmd_screenshot() {
    local out="${1:-.qemu-screen.ppm}"
    # screendump needs an absolute path
    [[ "$out" != /* ]] && out="$PWD/$out"
    # Target the ST7789V2 display device specifically (not the default console)
    local resp; resp=$(qmp_cmd "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$out\",\"device\":\"st7789v2\"}}")
    if echo "$resp" | grep -q '"return"'; then
        echo "$out"
    else
        die "screendump failed: $resp"
    fi
}

cmd_press() {
    local btn="${1:?Usage: emu.sh press <btnA|btnB>}"
    # BtnA = GPIO 11 (KEY1, front), BtnB = GPIO 12 (KEY2, side), active-low
    local pin
    case "$btn" in
        btnA|btna|a|A) pin=11 ;;
        btnB|btnb|b|B) pin=12 ;;
        *) die "unknown button '$btn' (use btnA or btnB)" ;;
    esac

    # Press: drive pin LOW (active-low buttons)
    local resp
    resp=$(qmp_cmd "{\"execute\":\"x-gpio-set-pin\",\"arguments\":{\"pin\":$pin,\"level\":0}}")
    if ! echo "$resp" | grep -q '"return"'; then
        die "press failed: $resp"
    fi

    # Hold for 100ms
    sleep 0.1

    # Release: drive pin HIGH
    resp=$(qmp_cmd "{\"execute\":\"x-gpio-set-pin\",\"arguments\":{\"pin\":$pin,\"level\":1}}")
    if ! echo "$resp" | grep -q '"return"'; then
        die "release failed: $resp"
    fi

    echo "pressed $btn (GPIO $pin)"
}

cmd_serial() {
    if [ -f "$SERIAL_LOG" ]; then
        cat "$SERIAL_LOG"
    else
        die "no serial log found ($SERIAL_LOG)"
    fi
}

cmd_stop() {
    require_running
    local pid; pid=$(cat "$PID_FILE")
    kill "$pid" 2>/dev/null || true
    rm -f "$PID_FILE" "$SOCK" "$SERIAL_LOG"
    echo "Emulator stopped (was PID $pid)"
}

# --- dispatch ---

case "${1:-}" in
    start)       shift; cmd_start "$@" ;;
    screenshot)  shift; cmd_screenshot "$@" ;;
    press)       shift; cmd_press "$@" ;;
    serial)      cmd_serial ;;
    stop)        cmd_stop ;;
    *)
        echo "Usage: emu.sh <start|screenshot|press|serial|stop> [args...]"
        exit 1
        ;;
esac
