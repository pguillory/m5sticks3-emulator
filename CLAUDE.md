# M5StickS3 Emulator

Instruction-accurate emulator for the M5StickS3 (ESP32-S3-PICO-1-N8R8). The goal is an experience like the iOS Simulator: compile firmware the normal way, hand the binary to the emulator instead of flashing it, and get a window showing the real display with clickable buttons. Same binary, same pixels, same behavior — just running on virtual hardware.

This is a generic, open-source tool for anyone building firmware for this device.

## Why This Exists

Developing firmware for the M5StickS3 today means flash-and-pray. You compile, flash the device, and look at serial output — but you can't see the screen or press the button programmatically. This emulator removes that bottleneck. Iterate on firmware without touching the device.

## Approach

This is **hardware emulation, not API mocking.** The firmware binary runs unmodified — the same `.bin` file you'd flash to the device. The emulator provides virtual hardware underneath: an emulated ESP32-S3 CPU executing real Xtensa machine code, with emulated peripherals (SPI, GPIO, I2S, etc.) that behave like the real chips.

The foundation is Espressif's QEMU fork (`github.com/espressif/qemu`, branch `esp-develop`), which already emulates:

- Dual-core Xtensa LX7 with ESP32-S3 TIE extensions (instruction-accurate)
- 8MB flash + 8MB PSRAM (OPI)
- SPI1 flash controller
- UART, timers, RTC, eFuses
- Crypto accelerators (SHA, AES, RSA, HMAC)
- Cache/MMU for flash-mapped memory
- A virtual RGB framebuffer display (not the ST7789, but proves the display plumbing works)
- OpenCores Ethernet (virtual NIC — not WiFi, but provides IP connectivity)

What's missing for the M5StickS3, and what this project builds:

1. **GPSPI (SPI2/SPI3) controller** — the general-purpose SPI peripheral that drives the display. The existing SPI1 model (flash controller) is a template.
2. **ST7789V2 display model** — a virtual SPI slave that interprets ST7789 commands (CASET, RASET, RAMWR, etc.), maintains a 135×240 framebuffer, and renders to QEMU's display console. This is what makes it pixel-perfect — the real font renderer, the real drawing code, pushing pixels through the real SPI driver to a virtual screen.
3. **GPIO with actual pin state** — the current GPIO model is a stub (strap register only). Needs full input/output register emulation so button presses can be injected.
4. **I2S peripheral** — for audio (mic capture + speaker playback). Not required for MVP but needed for full simulation.

### What Pixel-Perfect Means

The firmware compiles to a blob of Xtensa machine code. M5Unified's display library rasterizes bitmap fonts, computes text alignment, fills rectangles, and pushes the resulting pixels over SPI to the ST7789 display controller. In the emulator, that same machine code runs on an emulated Xtensa CPU, calls the same SPI registers, and the virtual ST7789 receives the same pixel data. The display output is identical by construction — not "close enough," identical, because it's the same code path.

## Target Device

M5StickS3 hardware:
- MCU: ESP32-S3-PICO-1-N8R8 (dual-core Xtensa LX7, 8MB flash, 8MB PSRAM)
- Display: 135×240 ST7789V2 LCD, connected via SPI
- Buttons: BtnA (GPIO), BtnB (GPIO)
- Audio: MEMS microphone, ES8311 codec + AW8737 amplifier (I2S)
- Connectivity: WiFi, BLE (NimBLE stack)
- Storage: NVS (flash-backed key-value store)

## MVP Scope

Get the display and buttons working. That means:
1. GPSPI peripheral model in QEMU
2. ST7789V2 SPI slave model in QEMU
3. GPIO peripheral model with injectable button state
4. A wrapper script or launcher that takes a firmware `.bin` and opens a window

WiFi: the existing OpenCores Ethernet virtual NIC may be sufficient if the ESP-IDF WiFi stack can be bypassed at the right layer, or we may need to stub at the lwIP level. This needs investigation.

BLE: stub or skip for MVP. BLE is only used during first-time provisioning on most M5StickS3 projects.

Audio: skip for MVP. Add I2S emulation later.

## Development

This is a C project (QEMU is C). The workflow is:
1. Build the modified QEMU fork
2. Build any M5StickS3 firmware project the normal way (e.g. `pio run`)
3. Run the firmware binary in the emulator

## Testing Firmware with the Emulator

The headless emulator CLI is at `m5sticks3/emu.sh`.

### Commands

```bash
./m5sticks3/emu.sh start <firmware.bin>   # start emulator with firmware
./m5sticks3/emu.sh screenshot [out.ppm]   # capture display (Read tool can view PPM images)
./m5sticks3/emu.sh press btnA|btnB        # press button (GPIO 11 = BtnA front, GPIO 12 = BtnB side)
./m5sticks3/emu.sh serial                 # read serial/debug output
./m5sticks3/emu.sh status                 # check if emulator is running
./m5sticks3/emu.sh stop                   # stop and clean up
./m5sticks3/emu.sh cleanup               # kill any orphaned QEMU processes
```

### Workflow

1. Build firmware (`pio run` or equivalent)
2. `emu.sh start <path-to-firmware.bin>`
3. Wait ~2 seconds for boot
4. `emu.sh screenshot` → use Read tool to view the PPM image
5. `emu.sh press btnA` / `emu.sh press btnB` to interact
6. `emu.sh serial` for debug logs
7. `emu.sh stop` when done

### Important

- **Always stop the emulator** — call `emu.sh stop` or `emu.sh cleanup` before ending your session. Don't leave QEMU processes running.
- Boot takes ~1-2 seconds; take screenshots after a short wait.
- Buttons are active-low. BtnA = GPIO 11 (front), BtnB = GPIO 12 (side).
- State files (`.qemu.sock`, `.qemu.pid`, `.qemu-serial.log`) live in the working directory.
- Only one emulator instance per directory.
