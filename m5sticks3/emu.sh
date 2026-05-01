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
    printf '%s\n%s\n' '{"execute":"qmp_capabilities"}' "$cmd" \
        | socat - UNIX-CONNECT:"$SOCK" 2>/dev/null \
        | tail -1
}

# Prepare a padded flash image from a raw firmware binary.
prepare_flash() {
    local src="$1" dst="$2"
    dd if=/dev/zero of="$dst" bs=1 count=$FLASH_SIZE 2>/dev/null
    dd if="$src" of="$dst" bs=1 seek=65536 conv=notrunc 2>/dev/null
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
    local resp; resp=$(qmp_cmd "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$out\"}}")
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

    # NOTE: Standard QMP has no GPIO-write command. This would require either:
    #   1. A custom QMP/HMP command added to the QEMU fork (e.g. "gpio-set-pin")
    #   2. Writing to the MMIO register (GPIO_BASE+0x3C = 0x6000403C) via GDB
    # For now we attempt the human-monitor-command path, which won't work until
    # a custom HMP command is added. Leaving the plumbing in place.
    echo "press $btn (GPIO $pin): not yet supported via QMP"
    echo "Requires a custom 'gpio-set-pin' QMP command in the QEMU fork."
    echo "See ui/sdl2-m5sticks3.c:m5sticks3_set_button() for the internal API."
    return 1
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
