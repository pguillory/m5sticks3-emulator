# Implementation Plan

## Goal
Add three peripheral models to Espressif's QEMU fork so unmodified M5StickS3
firmware can run with a working display and button input.

## M5StickS3 Pin Assignments (from M5GFX board config)
- Display SPI bus: **SPI3_HOST** (base 0x60025000)
- MOSI: GPIO 39
- SCLK: GPIO 40
- CS:   GPIO 41
- DC:   GPIO 45
- RST:  GPIO 21
- Backlight: GPIO 38
- Panel: ST7789, 135x240, color inverted, offset (52, 40)
- Buttons: via PMIC over I2C (not direct GPIO)

## Phases

### Phase 1: GPSPI Controller (`hw/ssi/esp32s3_gpspi.c`)
General-purpose SPI2/SPI3 peripheral. The display depends on this.

New files:
- `include/hw/ssi/esp32s3_gpspi.h` — register definitions, state struct
- `hw/ssi/esp32s3_gpspi.c` — implementation

Register layout (offsets differ from SPI1's SPI_MEM_* registers):
- 0x00 SPI_CMD     — bit 18 = SPI_USR trigger
- 0x04 SPI_ADDR
- 0x08 SPI_CTRL
- 0x0C SPI_CLOCK
- 0x10 SPI_USER    — bits 31-27: enable CMD/ADDR/DUMMY/MISO/MOSI phases
- 0x14 SPI_USER1   — address bit length, dummy cycle length
- 0x18 SPI_USER2   — command value and bit length
- 0x1C SPI_MS_DLEN — single combined data length register (bits minus 1)
- 0x20 SPI_MISC    — CS disable, clock polarity
- 0x30 SPI_DMA_CONF — DMA enable/configuration
- 0x34-0x44 SPI_DMA_INT_* — DMA interrupt registers
- 0x98-0xD4 SPI_W0-W15 — 16x32-bit data buffers
- 0xE0 SPI_SLAVE
- 0xE8 SPI_CLK_GATE
- 0xF0 SPI_DATE

Behavior:
- Writing SPI_USR bit triggers a transaction
- Transaction reads CMD/ADDR/DUMMY config, then transfers data
- CPU mode: data from W0-W15 registers
- DMA mode: data from GDMA via esp_gdma_read_channel()
- After completion: clears SPI_USR, fires interrupt

### Phase 2: ST7789V2 Display (`hw/display/st7789v2.c`)
SPI slave that receives pixel data and renders to QEMU display console.

New files:
- `include/hw/display/st7789v2.h` — state struct, constants
- `hw/display/st7789v2.c` — implementation

SSI slave device with GPIO input for DC pin.

Key ST7789 commands:
- 0x01 SWRESET — software reset
- 0x11 SLPOUT  — sleep out
- 0x21 INVON   — color inversion on
- 0x29 DISPON  — display on
- 0x2A CASET   — column address set (4 data bytes)
- 0x2B RASET   — row address set (4 data bytes)
- 0x2C RAMWR   — memory write (pixel stream follows)
- 0x36 MADCTL  — rotation/mirror/color order
- 0x3A COLMOD  — pixel format (0x55 = RGB565)

State machine:
1. DC low  → byte is a command, parse it
2. DC high → byte is data, interpret per current command
3. RAMWR: stream RGB565 pixels into framebuffer at cursor position
4. Cursor auto-advances within CASET/RASET window

Display rendering:
- 240x320 internal framebuffer (ST7789 native resolution)
- M5StickS3 uses 135x240 viewport at offset (52, 40)
- graphic_console_init() creates QEMU display window
- gfx_update callback pushes dirty regions via dpy_gfx_update()

### Phase 3: GPIO Expansion (`hw/gpio/esp32_gpio.c`)
Replace the stub with real register emulation.

Files to modify:
- `include/hw/gpio/esp32_gpio.h` — expand state struct
- `hw/gpio/esp32_gpio.c` — implement register read/write
- `include/hw/gpio/esp32s3_gpio.h` — adjust for 49 GPIOs
- `hw/gpio/esp32s3_gpio.c` — S3-specific overrides

Key registers (base 0x60004000):
- 0x04 GPIO_OUT        — output value, GPIOs 0-31
- 0x08 GPIO_OUT_W1TS   — set bits (write-only)
- 0x0C GPIO_OUT_W1TC   — clear bits (write-only)
- 0x10 GPIO_OUT1        — GPIOs 32-48
- 0x14 GPIO_OUT1_W1TS
- 0x18 GPIO_OUT1_W1TC
- 0x20 GPIO_ENABLE      — output enable, GPIOs 0-31
- 0x2C GPIO_ENABLE1     — GPIOs 32-48
- 0x38 GPIO_STRAP       — bootstrap (already implemented)
- 0x3C GPIO_IN          — input value, GPIOs 0-31
- 0x40 GPIO_IN1         — GPIOs 32-48
- 0x44 GPIO_STATUS      — interrupt status
- 0x74+ GPIO_PINn       — per-pin config (49 pins)

Behavior:
- W1TS/W1TC are write-only, modify OUT registers
- Output changes fire qemu_irq to connected devices
- IN register reflects injected input state (for buttons)
- Output GPIOs observable by ST7789V2 for DC pin

### Phase 4: Board Wiring (`hw/xtensa/esp32s3.c`)
Connect all new peripherals into the machine definition.

Changes:
- Add ESP32S3GpspiState gpspi[2] and St7789v2State st7789 to SocState
- In esp32s3_machine_init():
  - Initialize GPSPI2 at DR_REG_SPI2_BASE, GPSPI3 at DR_REG_SPI3_BASE
  - Attach ST7789V2 as SSI slave on GPSPI3's bus
  - Wire GPSPI CS to ST7789V2 CS
  - Wire GPIO output pin 45 to ST7789V2 DC input
  - Connect SPI2/SPI3 interrupts to interrupt matrix
  - Give GPSPI reference to GDMA for DMA transfers

### Phase 5: Build System
- `hw/ssi/meson.build` — add esp32s3_gpspi.c
- `hw/display/meson.build` — add st7789v2.c
- `hw/display/Kconfig` — add config ST7789V2
- `hw/xtensa/Kconfig` — select ST7789V2

### Phase 6: Launcher Script
- `scripts/run.sh` — takes firmware.bin, runs QEMU with correct flags
- Handles flash image assembly (merge bootloader + partitions + app)
