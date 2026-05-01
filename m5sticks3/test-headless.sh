#!/usr/bin/env bash
#
# M5StickS3 Emulator — headless regression test
#
# Exercises the full headless workflow: build, boot, serial check,
# screenshot, button presses, display change verification, shutdown.
#
# Run from repo root: ./m5sticks3/test-headless.sh
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
EMU="$SCRIPT_DIR/emu.sh"
FIRMWARE="$SCRIPT_DIR/test-firmware/M5Nemo-v3.2.1-M5StickS3.bin"

PASS_COUNT=0
FAIL_COUNT=0
SCREENSHOT_DIR=""

# --- helpers ---

pass() {
    echo "  ✓ PASS: $1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    echo "  ✗ FAIL: $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

cleanup() {
    echo ""
    echo "Cleaning up..."
    "$EMU" stop 2>/dev/null || "$EMU" cleanup 2>/dev/null || true
    [ -n "$SCREENSHOT_DIR" ] && rm -rf "$SCREENSHOT_DIR"
}
trap cleanup EXIT

# --- step 1: build QEMU if needed ---

echo "Step 1: Build QEMU"
if [ -x "$REPO_DIR/build/qemu-system-xtensa" ]; then
    pass "QEMU binary already built"
else
    echo "  Building QEMU..."
    if ninja -C "$REPO_DIR/build" qemu-system-xtensa >/dev/null 2>&1; then
        pass "QEMU built successfully"
    else
        fail "QEMU build failed"
        echo ""
        echo "RESULT: $PASS_COUNT passed, $FAIL_COUNT failed (build required)"
        exit 1
    fi
fi

# --- step 2: start emulator ---

echo "Step 2: Start emulator"
# Ensure no leftover state
"$EMU" cleanup >/dev/null 2>&1 || true

SCREENSHOT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/emu-test-XXXXXX")

if "$EMU" start "$FIRMWARE" >/dev/null 2>&1; then
    pass "Emulator started"
else
    fail "Emulator failed to start"
    echo ""
    echo "RESULT: $PASS_COUNT passed, $FAIL_COUNT failed"
    exit 1
fi

# --- step 3: wait for boot ---

echo "Step 3: Wait for boot (polling serial for 'auto detect board')"
BOOT_TIMEOUT=30
BOOTED=false
for i in $(seq 1 $BOOT_TIMEOUT); do
    if [ -f "$REPO_DIR/.qemu-serial.log" ] && grep -q "auto detect board" "$REPO_DIR/.qemu-serial.log" 2>/dev/null; then
        BOOTED=true
        break
    fi
    sleep 1
done

if $BOOTED; then
    pass "Boot detected in ${i}s"
    # Wait for display to fully render after boot
    sleep 3
else
    fail "Boot not detected within ${BOOT_TIMEOUT}s"
    echo "  Serial log tail:"
    tail -5 "$REPO_DIR/.qemu-serial.log" 2>/dev/null | sed 's/^/    /'
    echo ""
    echo "RESULT: $PASS_COUNT passed, $FAIL_COUNT failed"
    exit 1
fi

# --- step 4: verify serial output ---

echo "Step 4: Verify serial output"
SERIAL=$("$EMU" serial 2>/dev/null || true)

if echo "$SERIAL" | grep -q "ESP-ROM"; then
    pass "Serial contains 'ESP-ROM'"
else
    fail "Serial missing 'ESP-ROM'"
fi

if echo "$SERIAL" | grep -q "board:26"; then
    pass "Serial contains 'board:26'"
else
    fail "Serial missing 'board:26'"
fi

# --- step 5: take screenshot 1 ---

echo "Step 5: Take screenshot"
SHOT1="$SCREENSHOT_DIR/screen1.ppm"
if "$EMU" screenshot "$SHOT1" >/dev/null 2>&1 && [ -f "$SHOT1" ] && [ -s "$SHOT1" ]; then
    pass "Screenshot 1 exists and is non-empty ($(wc -c < "$SHOT1") bytes)"
else
    fail "Screenshot 1 failed or is empty"
fi

# --- step 6: press btnA, take screenshot 2 ---

echo "Step 6: Press btnA and take screenshot"
if "$EMU" press btnA >/dev/null 2>&1; then
    pass "btnA press succeeded"
else
    fail "btnA press failed"
fi

sleep 2  # Allow display to update

SHOT2="$SCREENSHOT_DIR/screen2.ppm"
if "$EMU" screenshot "$SHOT2" >/dev/null 2>&1 && [ -f "$SHOT2" ] && [ -s "$SHOT2" ]; then
    pass "Screenshot 2 exists and is non-empty"
else
    fail "Screenshot 2 failed or is empty"
fi

# --- step 7: verify screenshots differ ---

echo "Step 7: Verify display changed after btnA"
if [ -f "$SHOT1" ] && [ -f "$SHOT2" ]; then
    if ! cmp -s "$SHOT1" "$SHOT2"; then
        pass "Screenshots 1 and 2 are different (display updated)"
    else
        fail "Screenshots 1 and 2 are identical (display did not change)"
    fi
else
    fail "Cannot compare — screenshots missing"
fi

# --- step 8: press btnB, take screenshot 3 ---

echo "Step 8: Press btnB and take screenshot"
if "$EMU" press btnB >/dev/null 2>&1; then
    pass "btnB press succeeded"
else
    fail "btnB press failed"
fi

sleep 1

SHOT3="$SCREENSHOT_DIR/screen3.ppm"
if "$EMU" screenshot "$SHOT3" >/dev/null 2>&1 && [ -f "$SHOT3" ] && [ -s "$SHOT3" ]; then
    pass "Screenshot 3 exists and is non-empty"
else
    fail "Screenshot 3 failed or is empty"
fi

# --- step 9: stop emulator ---

echo "Step 9: Stop emulator"
if "$EMU" stop >/dev/null 2>&1; then
    pass "Emulator stopped"
else
    fail "Emulator stop command failed"
fi

# --- step 10: verify clean shutdown ---

echo "Step 10: Verify clean shutdown"
ALL_CLEAN=true
for f in "$REPO_DIR/.qemu.sock" "$REPO_DIR/.qemu.pid" "$REPO_DIR/.qemu-serial.log"; do
    if [ -e "$f" ]; then
        fail "State file not cleaned up: $f"
        ALL_CLEAN=false
    fi
done
if $ALL_CLEAN; then
    pass "All state files removed"
fi

# --- summary ---

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "RESULT: $PASS_COUNT passed, $FAIL_COUNT failed"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

[ "$FAIL_COUNT" -eq 0 ] && exit 0 || exit 1
