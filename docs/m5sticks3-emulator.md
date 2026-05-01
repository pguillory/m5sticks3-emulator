# M5StickS3 Emulator — Architecture & Reference

An instruction-accurate emulator for the M5StickS3 (ESP32-S3-PICO-1-N8R8), built on Espressif's QEMU fork. The same firmware binary you'd flash to the device runs here on virtual hardware — same pixels, same behavior, no device required.

This document covers the chip architecture, the M5StickS3 board design, how each peripheral is emulated, and hard-won debugging knowledge from bringing it all up.

---

## Table of Contents

1. [ESP32-S3 SoC Architecture](#esp32-s3-soc-architecture)
2. [Memory Map](#memory-map)
3. [M5StickS3 Board Design](#m5sticks3-board-design)
4. [Display Pipeline](#display-pipeline)
5. [SPI Subsystem](#spi-subsystem)
6. [I2C Bus Topology](#i2c-bus-topology)
7. [GPIO System](#gpio-system)
8. [Clock & PLL Configuration](#clock--pll-configuration)
9. [eFuse & Chip Identity](#efuse--chip-identity)
10. [Cache & MMU (EXTMEM)](#cache--mmu-extmem)
11. [Boot Sequence](#boot-sequence)
12. [M5Unified Board Detection](#m5unified-board-detection)
13. [IO Catch-All Region](#io-catch-all-region)
14. [Peripheral Reference Table](#peripheral-reference-table)
15. [Debugging Cookbook](#debugging-cookbook)
16. [Running the Emulator](#running-the-emulator)
17. [Headless CLI Reference](#headless-cli-reference)
18. [Headless Development Workflow](#headless-development-workflow)
19. [Known Limitations](#known-limitations)

---

## ESP32-S3 SoC Architecture

The ESP32-S3 is a dual-core Xtensa LX7 SoC designed for IoT and edge AI. The two cores share most memory and peripherals but have independent instruction ROMs and reset/stall control.

### Core Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     ESP32-S3 SoC                        │
│                                                         │
│  ┌──────────┐  ┌──────────┐                             │
│  │  CPU 0   │  │  CPU 1   │   Xtensa LX7 cores         │
│  │ (PRO)    │  │ (APP)    │   PRID: 0xCDCD / 0xABAB    │
│  │ 240 MHz  │  │ 240 MHz  │   with ESP32-S3 TIE        │
│  └────┬─────┘  └────┬─────┘                             │
│       │              │                                   │
│  ┌────┴──────────────┴────┐                             │
│  │    Interrupt Matrix    │  80+ sources → CPU IRQ      │
│  │    (0x600C2000)        │  lines. Per-CPU routing.    │
│  └────────────┬───────────┘                             │
│               │                                          │
│  ┌────────────┴───────────────────────────────────────┐ │
│  │              Peripheral Bus (APB)                   │ │
│  │  UART×3  SPI×4  I2C×2  I2S×2  GPIO  Timers  ...   │ │
│  └────────────────────────────────────────────────────┘ │
│                                                         │
│  ┌─────────────────────────────────────────────────────┐│
│  │  DMA (GDMA)  │  5 channels, connects to SPI/I2S/   ││
│  │              │  crypto. IN (RX) + OUT (TX) per ch.  ││
│  └─────────────────────────────────────────────────────┘│
│                                                         │
│  ┌─────────────────────────────────────────────────────┐│
│  │  Crypto      │  SHA, AES, RSA, HMAC, DS, XTS-AES   ││
│  └─────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────┘
```

**CPU identifiers:** CPU 0 (Protocol CPU, PRID=0xCDCD) runs the main firmware. CPU 1 (Application CPU, PRID=0xABAB) starts stalled at boot and is released by firmware writing to `SYSTEM_CORE_1_CONTROL_0_REG`.

**Interrupt routing:** All peripherals emit interrupt signals into a central interrupt matrix at `0x600C2000`. The matrix maps 80+ interrupt sources to each CPU's local interrupt lines. This is how a single GPIO interrupt reaches the right CPU — the firmware configures the mapping.

**External Register (ER) space:** Each CPU has a private ER address space accessed via RER/WER Xtensa instructions. This contains the WORLD controller (memory protection), PMS (permission management), and debug registers. The emulator provides a catch-all handler returning 0 for unmapped ER addresses, which is safe — it means "no protection, default config."

---

## Memory Map

### Internal Memory

| Region | Address | Size | Description |
|--------|---------|------|-------------|
| DROM | `0x3FF00000` | 128 KB | Data ROM (alias into IROM with 0x40000 offset) |
| IROM | `0x40000000` | 384 KB | Instruction ROM (per-CPU, contains bootloader) |
| DRAM | `0x3FC80000` | 1.5 MB | Main data RAM (shared between cores) |
| IRAM | `0x40370000` | 512 KB | Instruction RAM (for code that can't run from flash) |
| RTC Slow | `0x50000000` | 8 KB | RTC domain, survives deep sleep |
| RTC Fast | `0x600FE000` | 8 KB | Fast RTC memory, core-local |

### External Memory (via Cache/MMU)

| Region | Address | Size | Description |
|--------|---------|------|-------------|
| D-Cache | `0x3C000000` | 512 KB | Data cache window into flash/PSRAM |
| I-Cache | `0x42000000` | 512 KB | Instruction cache window (execute from flash) |

The cache windows are virtual address ranges that the EXTMEM controller maps to physical flash or PSRAM pages. The firmware configures page-level mappings through MMU table entries. This is how firmware runs code directly from flash without copying it all to RAM.

### Peripheral IO Space

All peripherals live in the `0x60000000–0x600D1000` range. See [Peripheral Reference Table](#peripheral-reference-table) for the complete list.

### Special Regions

| Region | Address | Size | Description |
|--------|---------|------|-------------|
| Framebuffer (virtual) | `0x20000000` | 2 MB | RGB display VRAM (for direct-draw path) |
| WDEV (WiFi) | `0x3FF75000` | — | WiFi device registers (outside main IO range) |
| RNG | `0x3FF75144` | — | Random number generator (in WDEV space) |

---

## M5StickS3 Board Design

The M5StickS3 is built around the ESP32-S3-PICO-1-N8R8 module — a system-in-package with 8 MB flash and 8 MB PSRAM in an LGA56 form factor (package version 1).

### Block Diagram

```
                    M5StickS3
┌──────────────────────────────────────────────┐
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │     ESP32-S3-PICO-1-N8R8              │  │
│  │     (LGA56 / pkg_ver = 1)             │  │
│  │                                        │  │
│  │  8 MB Flash (SPI)  8 MB PSRAM (OPI)   │  │
│  │  Dual Xtensa LX7 @ 240 MHz           │  │
│  └────────────────────────────────────────┘  │
│       │          │         │        │        │
│       │SPI3      │I2C1     │I2S0    │GPIO    │
│       │          │         │        │        │
│  ┌────┴───┐  ┌───┴────┐  ┌┴─────┐  │        │
│  │ST7789  │  │ M5PM1  │  │ES8311│  │        │
│  │ V2 LCD │  │ PMIC   │  │Codec │  │        │
│  │135×240 │  │(0x6E)  │  │(0x18)│  │        │
│  └────────┘  └────────┘  └──────┘  │        │
│                  │                  │        │
│              ┌───┘            ┌────┘        │
│              │GPIO2           │              │
│              │(LCD pwr)       │              │
│              ▼           ┌────┴────┐        │
│         L3B Enable       │ Buttons │        │
│                          │ A: GP11 │        │
│                          │ B: GP12 │        │
│                          └─────────┘        │
└──────────────────────────────────────────────┘
```

### Key Components

| Component | Chip | Interface | Address/Pins | Purpose |
|-----------|------|-----------|-------------|---------|
| MCU | ESP32-S3-PICO-1-N8R8 | — | — | Main processor, 8MB flash + 8MB PSRAM |
| Display | ST7789V2 | SPI3 | CS=GPIO41, DC=GPIO45 | 135×240 LCD, RGB565 |
| PMIC | M5PM1 (PY32-based) | I2C1 | 0x6E | Power management, LCD power via GPIO2 |
| Audio codec | ES8311 | I2C1 + I2S0 | 0x18 | Microphone + speaker |
| Amplifier | AW8737 | I2S0 | — | Speaker amplifier |
| Button A | — | GPIO | GPIO 11 (active low) | Front blue button |
| Button B | — | GPIO | GPIO 12 (active low) | Side button |

### Display Pin Assignments

| Signal | GPIO | Description |
|--------|------|-------------|
| MOSI | 39 | SPI data out to display |
| SCLK | 40 | SPI clock |
| CS | 41 | Chip select (active low) |
| DC | 45 | Data/Command select (0=cmd, 1=data) |
| RST | 21 | Hardware reset |
| Backlight | 38 | Backlight control |

The display viewport is 135×240 pixels within the ST7789's native 240×320 framebuffer, offset by (52, 40). M5GFX handles this offset automatically — the firmware writes pixel data to columns 52–186 and rows 40–279.

---

## Display Pipeline

The display pipeline is the most important data path in the emulator. Firmware pixel data travels:

```
Firmware (M5GFX)
    │
    │ Writes to SPI3 registers at 0x60025000
    │ (configures CMD/ADDR/DATA phases, triggers SPI_USR)
    │
    ▼
GPSPI3 Controller (esp32s3_gpspi.c)
    │
    │ Executes SPI transaction: asserts CS, sends bytes
    │ CPU mode: reads from W0-W15 data registers
    │ DMA mode: reads from GDMA → system memory
    │
    ▼
SSI Bus (QEMU SSI framework)
    │
    │ Byte-by-byte transfer to the slave device
    │
    ▼
ST7789V2 Model (st7789v2.c)
    │
    │ DC pin from GPIO 45 determines command vs data:
    │   DC=0: parse as ST7789 command (CASET, RASET, RAMWR, etc.)
    │   DC=1: interpret as data (pixel bytes, parameters)
    │
    │ For RAMWR: accumulates 2 bytes → 1 RGB565 pixel
    │ Writes into 240×320 internal framebuffer
    │ Tracks dirty rectangle for efficient updates
    │
    ▼
QEMU Display Console (ui/console.h)
    │
    │ gfx_update callback converts dirty region:
    │   RGB565 → xRGB8888 (bit expansion per channel)
    │   Pushes to QEMU's display surface via dpy_gfx_update()
    │
    ▼
Host display window (SDL2 / Cocoa / GTK)
```

### ST7789 Command Protocol

The ST7789V2 is a typical SPI display controller. The DC (Data/Command) pin determines how each byte is interpreted:

**Commands (DC=0):**

| Byte | Name | Description |
|------|------|-------------|
| `0x01` | SWRESET | Software reset. Clears framebuffer, enters sleep. |
| `0x11` | SLPOUT | Exit sleep mode. Required before display output. |
| `0x21` | INVON | Enable color inversion (M5StickS3 uses this). |
| `0x29` | DISPON | Turn display on. No pixels appear until this. |
| `0x2A` | CASET | Set column window. Followed by 4 data bytes: XS_H, XS_L, XE_H, XE_L. |
| `0x2B` | RASET | Set row window. Followed by 4 data bytes: YS_H, YS_L, YE_H, YE_L. |
| `0x2C` | RAMWR | Begin pixel write. All subsequent data bytes are pixel data. |
| `0x36` | MADCTL | Set rotation/mirror/color order. |
| `0x3A` | COLMOD | Set color format. `0x55` = RGB565 (16 bits/pixel). |
| `0x3C` | RAMWRC | Continue pixel write (no window reset). |

**Pixel data (DC=1, after RAMWR):**

In RGB565 mode (the standard), each pixel is 2 bytes, MSB first:

```
Byte 0: RRRRRGGG  (5 bits red, 3 bits green high)
Byte 1: GGGBBBBB  (3 bits green low, 5 bits blue)
```

The cursor auto-advances within the CASET/RASET window: columns first, then rows, wrapping to the start when the window boundary is reached. This allows streaming an entire screen's pixels after a single RAMWR command.

### RGB565 → xRGB8888 Conversion

The display surface uses 32-bit xRGB8888. Conversion expands each channel by replicating the MSBs into the LSBs for full dynamic range:

```c
R8 = (R5 << 3) | (R5 >> 2)   // 5→8 bits
G8 = (G6 << 2) | (G6 >> 4)   // 6→8 bits
B8 = (B5 << 3) | (B5 >> 2)   // 5→8 bits
```

---

## SPI Subsystem

The ESP32-S3 has four SPI controllers, but they are *not* interchangeable:

| Controller | Base Address | Purpose | Type |
|-----------|-------------|---------|------|
| SPI0 | `0x60003000` | Cache/flash read (hardware-managed) | SPI_MEM |
| SPI1 | `0x60002000` | Flash programming + PSRAM | SPI_MEM |
| SPI2 | `0x60024000` | General-purpose (GPSPI) | GPSPI |
| SPI3 | `0x60025000` | General-purpose (GPSPI) — **display** | GPSPI |

SPI0 and SPI1 are "SPI_MEM" controllers — they have a completely different register layout from SPI2/SPI3. The `SPI_MEM_*` registers (flash commands, timing, auto-cmd) do not exist on GPSPI, and vice versa. This is a common source of confusion.

### GPSPI Register Map

The GPSPI controllers drive external peripherals. Key registers (offsets from base):

| Offset | Register | Key Fields |
|--------|----------|-----------|
| `0x00` | SPI_CMD | `USR` (bit 18): trigger transaction. `UPDATE` (bit 24): apply config. |
| `0x04` | SPI_ADDR | Address value for address phase |
| `0x08` | SPI_CTRL | Bit order, quad/dual mode |
| `0x0C` | SPI_CLOCK | Divider settings (CLKCNT_N, CLKCNT_H, CLKCNT_L) |
| `0x10` | SPI_USER | Phase enables: CMD (31), ADDR (30), DUMMY (29), MISO (28), MOSI (27) |
| `0x14` | SPI_USER1 | Address bit length, dummy cycle count |
| `0x18` | SPI_USER2 | Command value and bit length |
| `0x1C` | SPI_MS_DLEN | Data length in bits (bits-minus-1 encoding) |
| `0x20` | SPI_MISC | CS disable bits, `CS_KEEP_ACTIVE` (bit 10) |
| `0x30` | SPI_DMA_CONF | `DMA_TX_ENA` (31), `DMA_RX_ENA` (30) |
| `0x34` | DMA_INT_ENA | Interrupt enable (bit 13 = TRANS_DONE) |
| `0x38` | DMA_INT_CLR | Write to clear interrupt flags |
| `0x3C` | DMA_INT_RAW | Raw interrupt status |
| `0x40` | DMA_INT_ST | Masked interrupt status (RAW & ENA) |
| `0x98–0xD4` | W0–W15 | 16×32-bit data buffer (CPU mode) |
| `0xE0` | SPI_SLAVE | Slave mode config |
| `0xE8` | SPI_CLK_GATE | Clock gating |

### SPI Transaction Flow

1. Firmware configures phases in `SPI_USER` (which phases are active)
2. Writes command value to `SPI_USER2`, address to `SPI_ADDR`
3. For CPU mode: loads pixel data into `W0–W15` (up to 64 bytes per transaction)
4. For DMA mode: sets up GDMA descriptors in memory, enables `DMA_TX_ENA`
5. Writes `SPI_USR` bit in `SPI_CMD` — transaction executes immediately
6. Controller asserts CS → sends CMD → ADDR → DUMMY → DATA → deasserts CS
7. Clears `SPI_USR`, sets `TRANS_DONE` interrupt

**Important:** The `UPDATE` bit (24) and `CONF_BITLEN` bit (23) in `SPI_CMD` are auto-clear — hardware applies the config instantly and clears these bits. The emulator must do the same or firmware will spin waiting for them to clear.

### DMA Integration

For large transfers (like full-screen display updates), the GPSPI uses GDMA:

```
System Memory → GDMA OUT channel → GPSPI TX → SPI bus → ST7789
```

The GDMA controller has 5 channels. Each channel has IN (RX/read from peripheral) and OUT (TX/write to peripheral) directions. SPI2 uses `GDMA_SPI2` peripheral ID, SPI3 uses `GDMA_SPI3`. The GPSPI reads from GDMA in 256-byte chunks to avoid huge buffer allocations.

---

## I2C Bus Topology

The ESP32-S3 has two I2C controllers. On the M5StickS3, the internal devices are on I2C1:

```
I2C0 (0x60013000)                I2C1 (0x60027000)
    │                                │
    │ (external, unused)             ├── ES8311 @ 0x18 (audio codec)
    │                                │
                                     └── M5PM1  @ 0x6E (PMIC)
```

### M5PM1 PMIC

The M5StickS3 uses a custom PY32-based PMIC instead of the AXP2101 found on many M5Stack devices. This is important because M5GFX's board auto-detection specifically probes I2C address `0x6E` to identify the M5StickS3.

**Register map:**

| Address | Name | Default | Description |
|---------|------|---------|-------------|
| `0x00` | DEVICE_ID | `0x50` | Device identification. M5Unified checks `has_value()` here. |
| `0x09` | I2C_CONFIG | `0x00` | I2C configuration (0 = disable idle sleep) |
| `0x10` | GPIO_DIR | `0x00` | GPIO direction. Bit set = output. |
| `0x11` | GPIO_OUT | `0x00` | GPIO output level. Bit set = high. |
| `0x13` | GPIO_OUT_MODE | `0x00` | Output mode. Bit clear = push-pull. |
| `0x16` | GPIO_FUNC_SEL | `0x00` | Function select. Bit clear = GPIO mode. |

**GPIO2 on M5PM1** controls the LCD power rail (L3B Enable). The firmware sets GPIO2 as output-high to turn on the display backlight power.

### ES8311 Audio Codec

The ES8311 at `0x18` handles microphone input and speaker output. Control registers configure gain, sample rate, and codec paths. Audio sample data flows through I2S0, not I2C.

### I2C Protocol in the Emulator

The I2C controller emulation (`hw/i2c/esp32_i2c.c`) supports the ESP32 I2C command engine — a FIFO-based transaction processor. Key details:

- **Opcode 6 (RSTART):** The ESP32-S3 adds opcode 6 for repeated start, which the ESP32 didn't have. Without this, I2C read transactions fail (write address, repeated start, read data).
- **Transaction model:** Firmware queues up to 16 commands (RSTART, WRITE, READ, STOP, END) with byte counts. The controller executes them in sequence against the I2C bus.

---

## GPIO System

The ESP32-S3 has 49 GPIO pins (0–48). The GPIO controller at `0x60004000` provides:

### Key Registers

| Offset | Register | Description |
|--------|----------|-------------|
| `0x04` | GPIO_OUT | Output level for GPIOs 0–31 |
| `0x08` | GPIO_OUT_W1TS | Set output bits (write 1 to set, write-only) |
| `0x0C` | GPIO_OUT_W1TC | Clear output bits (write 1 to clear, write-only) |
| `0x10` | GPIO_OUT1 | Output level for GPIOs 32–48 |
| `0x14` | GPIO_OUT1_W1TS | Set output bits for 32–48 |
| `0x18` | GPIO_OUT1_W1TC | Clear output bits for 32–48 |
| `0x20` | GPIO_ENABLE | Output enable for GPIOs 0–31 |
| `0x2C` | GPIO_ENABLE1 | Output enable for GPIOs 32–48 |
| `0x38` | GPIO_STRAP | Bootstrap pin values (read-only) |
| `0x3C` | GPIO_IN | Input state for GPIOs 0–31 |
| `0x40` | GPIO_IN1 | Input state for GPIOs 32–48 |
| `0x44` | GPIO_STATUS | Interrupt status |
| `0x74+` | GPIO_PINn | Per-pin configuration (49 pins × 4 bytes) |

### W1TS/W1TC Pattern

The "write-1-to-set" and "write-1-to-clear" registers are a common pattern in ESP32 peripherals. Writing `0x00000020` to `GPIO_OUT_W1TS` sets bit 5 of `GPIO_OUT` without affecting other bits. This avoids read-modify-write races in concurrent code.

### GPIO → Display DC Wiring

The critical GPIO wiring for the display is pin 45 (DC signal). In the emulator, this is a QEMU GPIO connection:

```c
qdev_connect_gpio_out_named(DEVICE(&ss->gpio), "gpio-out", 45,
                            qdev_get_gpio_in_named(st7789, "dc", 0));
```

When firmware writes to `GPIO_OUT1` bit 13 (GPIO 45 = 32 + 13), the GPIO model fires an IRQ to the ST7789V2's DC input handler. The display model then interprets subsequent SPI bytes as commands (DC=0) or data (DC=1).

---

## Clock & PLL Configuration

The ESP32-S3 clock tree is complex. The emulator models the key paths that affect CPU frequency.

### Clock Sources

| Source | Frequency | Register Field |
|--------|-----------|---------------|
| XTAL | 40 MHz | `SOC_CLK_SEL = 0` |
| PLL | 480 or 320 MHz | `SOC_CLK_SEL = 1` |
| RC_FAST | 17.5 MHz | `SOC_CLK_SEL = 2` |

### CPU Frequency Derivation

```
PLL (480 MHz or 320 MHz)
    │
    ▼
CPUPERIOD_SEL divider:
    0 → 80 MHz
    1 → 160 MHz
    2 → 240 MHz (480 MHz PLL only)
```

The firmware selects PLL frequency via `SYSTEM_CPU_PER_CONF.PLL_FREQ_SEL` and the divider via `CPUPERIOD_SEL`. The emulator updates the QEMU clock model when these change, snapshotting `CCOUNT` to avoid timer discontinuities.

### REGI2C / I2C_ANA_MST (Analog Master)

The PLL is configured through an internal I2C bus called I2C_ANA_MST at `0x6000E000`. This is *not* the same as the external I2C0/I2C1 controllers — it's a hardware-level interface to analog blocks like the BBPLL.

**Command register (offset 0x000):**

```
Bits [7:0]   = slave/host identifier
Bits [15:8]  = register address within slave
Bits [23:16] = data (write value or read result)
Bit  24      = write flag (1=write, 0=read)
Bit  25      = busy flag
Bit  26      = start/enable
```

**Key BBPLL registers (slave addr 0x66):**

| Register | Name | Values |
|----------|------|--------|
| 2 | OC_REF_DIV | `8` → 480 MHz PLL, `2` → 320 MHz PLL |

The emulator maintains shadow registers for the BBPLL slave. Without this, `clk_ll_bbpll_get_freq_mhz()` reads back zero for the reference divider and divides by zero.

**Status register (offset 0x040):** Returns `0x01000000` (bit 24 = BBPLL_CAL_DONE). The ROM polls this after configuring the PLL. All other reads in the 0xE000 range return 0 to avoid stale-value polling loops.

---

## eFuse & Chip Identity

eFuses are one-time-programmable bits that store chip identity, security configuration, and calibration data. The emulator pre-programs critical fields.

### eFuse Blocks

| Block | Size | Contents |
|-------|------|----------|
| BLOCK0 | 192 bits (6 words) | Write protection, read protection, security config |
| BLOCK1 | 192 bits (6 words) | MAC address, SPI pad config, **PKG_VERSION** |
| BLOCK2 | 256 bits (8 words) | System key |
| BLOCK3 | 256 bits (8 words) | User key |

### PKG_VERSION — The Critical Field

**Location:** BLOCK1 word 3 (`EFUSE_RD_MAC_SPI_SYS_3`), bits [24:21]

| Value | Package | Description |
|-------|---------|-------------|
| 0 | QFN56 | Standard ESP32-S3 module |
| 1 | LGA56 / PICO | ESP32-S3-PICO-1 (M5StickS3 uses this) |

**Why this matters:** M5GFX reads `PKG_VERSION` from eFuses to choose its board detection path. With `pkg_ver=0` (the default), it follows the QFN56 path, which probes for different I2C devices and never finds the M5StickS3's M5PM1 PMIC. With `pkg_ver=1`, it follows the PICO/LGA56 path, probes I2C address `0x6E`, and correctly identifies the M5StickS3.

The emulator sets this in `esp32s3_efuse_realize()`:

```c
s->efuses.blocks.rd_mac_spi_sys_3 =
    FIELD_DP32(s->efuses.blocks.rd_mac_spi_sys_3,
               EFUSE_RD_MAC_SPI_SYS_3, PKG_VERSION, 1);
```

This must happen *after* the parent class realize (which initializes eFuse memory) to avoid being overwritten.

### eFuse Read Protocol

The eFuse controller at `0x60007000` uses a command-based interface:
- Write opcode `0x5AA5` to the read command register
- Wait 100 µs for the read to complete
- Read results from block data registers

---

## Cache & MMU (EXTMEM)

The EXTMEM controller at `0x600C4000` (aliased at `0x60040000` for ROM access) manages the cache and MMU that map flash/PSRAM into the CPU's address space.

### How Flash Execution Works

Firmware code stored in flash at physical address X can be accessed by the CPU at virtual address `0x42000000 + offset` through the instruction cache. The MMU maintains a page table that maps virtual pages to flash pages.

```
Virtual:  0x42000000  ──MMU──►  Flash physical addr  ──SPI1──►  Flash chip
```

Similarly, data reads from flash go through the data cache at `0x3C000000`.

### Boundary Registers (Critical Bug)

The EXTMEM controller has boundary registers that define the valid virtual address ranges for flash and PSRAM:

```
IBUS_TO_FLASH_START_VADDR   — where flash mapping begins in I-cache space
IBUS_TO_FLASH_END_VADDR     — where flash mapping ends
DBUS_TO_FLASH_START_VADDR   — same for D-cache
DBUS_TO_FLASH_END_VADDR
```

**The bug we fixed:** The cache controller's read handler had a default case that returned 0 for any register not explicitly handled. This meant that after the ROM bootloader wrote boundary values to configure the MMU, the firmware's `esp_mmu_map()` function read them back as 0 — concluding that no valid address range existed — and returned error `456: no such vaddr range`.

The fix: return the stored register value for any valid register index:

```c
default:
    if (index < ESP32S3_CACHE_REG_COUNT) {
        r = s->regs[index];  // was: r = 0;
    }
```

### PSRAM Size and -m Flag

The `-m` QEMU flag controls PSRAM size. The M5StickS3 has 8 MB PSRAM, so use `-m 8M`. Using `-m 512M` causes `esp_mmu_map` to fail because the cache controller can't handle mapping windows that large.

---

## Boot Sequence

### ROM Boot (flash image path)

```
1. CPU 0 starts at reset vector (0x40000400)
2. ROM bootloader initializes:
   - Clock: XTAL → PLL (via REGI2C/I2C_ANA_MST)
   - Cache/MMU: configures flash mapping
   - Reads flash header at offset 0x0000
   - Loads 2nd-stage bootloader from flash
3. 2nd-stage bootloader (ESP-IDF):
   - Reads partition table at flash offset 0x8000
   - Finds active OTA partition (app0, typically at 0x10000)
   - Configures MMU to map app code into I-cache and D-cache
   - Jumps to app entry point
4. App startup:
   - FreeRTOS scheduler starts
   - M5Unified init: board detection, display init, I2C probing
```

### ELF Loading Path (development)

When using `-kernel firmware.elf`, the ELF loader:
1. Maps ELF segments directly into memory
2. Patches function entry points with SIMCALL trampolines (for BLE/WiFi API interception)
3. Writes a jump stub at the reset vector to reach the ELF entry point
4. CPU 1 starts stalled

---

## M5Unified Board Detection

M5Unified's `_check_boardtype()` runs at startup to identify which M5Stack board is present. Understanding this flow is critical for getting the emulator to be detected correctly.

### Detection Flow

The detection is a two-stage process. M5GFX detects the display first (using I2C hardware), then M5Unified's `_check_boardtype()` refines or overrides the result (using bit-bang I2C on GPIO).

```
M5GFX::autodetect()               M5Unified::begin()
    │                                   │
    ├── Reads eFuse PKG_VERSION         ▼
    │   pkg_ver == 1 → LGA56 path  auto board = _check_boardtype(Display.getBoard())
    │                                   │
    ▼ (LGA56 path)                      ├── If board != unknown → skip detection
    Reads GPIO 47/48 (pull-up check)    │   (M5GFX already identified it)
    │                                   │
    ├── Both HIGH → I2C bus present     └── If board == unknown → own I2C probing
    │                                       (bit-bang GPIO, fallback detection)
    ▼
    I2C1 read: M5PM1 at 0x6E, reg 0x00
    │
    ├── has_value() → board_M5StickS3 (26)
    │   + configures LCD power via M5PM1 GPIO2
    │   + initializes ST7789 display
    │
    └── no value → continues checking other boards
```

### Board IDs (relevant subset)

| ID | Board |
|----|-------|
| 26 | M5StickS3 (display board) |
| 137 | M5AtomS3Lite (ESP32-S3 fallback) |
| 146 | M5PowerHub (QFN56 non-display board) |

**Current status:** Board detection works correctly. M5GFX's `autodetect()` reads eFuse PKG_VERSION=1, enters the LGA56/PICO path, reads M5PM1 device ID via I2C1 hardware, and returns `board_M5StickS3` (26). M5Unified receives this from `Display.getBoard()` and preserves it.

**History:** Before the I2C opcode fix (commit 725634402e), the I2C READ/STOP opcodes used ESP32 values instead of ESP32-S3 values. This caused I2C transactions with M5PM1 to fail, M5GFX returned `board_unknown`, and `_check_boardtype()` fell through to incorrect fallback detection.

### How M5GFX Detection Works

M5GFX's `autodetect()` for display configuration (pkg_ver == 1 path):
1. Reads `PKG_VERSION` from eFuses → must be 1 (LGA56/PICO)
2. Reads GPIO 47 and 48 with internal pull-down → both must read HIGH (external I2C pull-ups present)
3. Initializes I2C1 hardware peripheral (SDA=47, SCL=48)
4. Reads M5PM1 register 0x00 at address 0x6E → must return a value (device ID 0x50)
5. If M5PM1 found: sets `board_M5StickS3`, configures LCD power (M5PM1 GPIO2 HIGH), initializes ST7789

### What the Emulator Provides

Each step is handled by a specific emulator component:
- **PKG_VERSION=1**: Set in `esp32s3_efuse.c` during device realize
- **GPIO 47/48 HIGH**: GPIO model defaults all inputs to HIGH (`gpio_in1 = 0x0001FFFF`)
- **I2C1 hardware**: `esp32_i2c.c` with correct ESP32-S3 opcodes (READ=3, STOP=2, RSTART=6)
- **M5PM1 at 0x6E**: `m5pm1.c` attached to I2C1 bus, returns device ID 0x50 for register 0x00

---

## IO Catch-All Region

The emulator's most important architectural pattern is the IO catch-all region. This is a priority -1 memory region covering all of `0x60000000–0x600D1000` that handles reads/writes to any address not claimed by a specific device model.

### How It Works

```
┌─────────────────────────────────────────┐
│        System Memory (priority 0)       │
│                                         │
│  ┌──────────┐  ┌──────────┐            │
│  │ UART     │  │ GPIO     │  ← priority 0 (specific devices)
│  │ 0x60000  │  │ 0x60004  │            │
│  └──────────┘  └──────────┘            │
│                                         │
│  ┌─────────────────────────────────────┐│
│  │     IO Catch-All (priority -1)      ││
│  │     0x60000000 – 0x600D1000         ││
│  │     Backed by uint32_t io_regs[]    ││
│  └─────────────────────────────────────┘│
└─────────────────────────────────────────┘
```

Specific device models (UART, GPIO, SPI, etc.) register at priority 0, so they "win" for their address ranges. Everything else falls through to the catch-all, which provides write-then-readback semantics — firmware can write a value and read it back. This is critical because many ESP32 peripherals have registers that aren't modeled but firmware still reads/writes them.

### Special Cases in the Catch-All

Several address ranges within the catch-all have special behavior:

**I2C_ANA_MST (0x6000E000–0x6000EFFF):**
- Offset 0x000–0x03C: REGI2C command registers with shadow read/write. Reads auto-clear bit 25 (busy flag) to prevent polling stalls.
- Offset 0x040: Always returns `0x01000000` (bit 24 = BBPLL_CAL_DONE).
- Offset 0x044–0x0FF: Status/config registers. Returns `0xFFFFFFFF` to instantly satisfy any "wait for calibration done" polling loops. The firmware writes config then polls for hardware-set completion bits; all-ones signals all operations complete.
- Offset 0x100+: Additional analog master registers. Returns 0 to satisfy "wait for busy clear" polls.

**Radio Front-End (FE at 0x60006000, FE2 at 0x60005000):**
- Used during WiFi/BLE radio calibration. The proprietary calibration firmware polls these registers with complex multi-register sequences.
- Emulation strategy: toggle return value between 0 and `0xFFFFFFFF` on successive reads. This ensures any polling condition (whether checking for bits set or bits cleared) is eventually satisfied within a few iterations.
- Without this, the WiFi `phy_init()` calibration loop runs forever, blocking the application from reaching display init.

**SENS/SAR ADC (0x60008800–0x600088FF):**
- SAR_MEAS1_CTRL2 (offset 0x0C): Auto-sets bit 16 (measurement done)
- SAR_MEAS2_CTRL2 (offset 0x30): Auto-sets bit 16 (measurement done)
- SAR_TSENS_CTRL (offset 0x50): Auto-sets bit 8 (TSENS_READY) and returns dummy temperature value (128 ≈ 25°C) in bits 7:0.

**Reserved region (0x6002F000–0x60037FFF):**
- Auto-clears bit 31 on read (simulates hardware command completion)
- Without this, firmware loops forever polling for bit 31 to clear at 0x6003101C

**SPI0 FSM (0x60003054):**
- Initialized to `0x200` (LOCK_DELAY_TIME=4) at startup
- Represents the SPI0 flash controller's default state

---

## Peripheral Reference Table

Complete list of emulated peripherals with base addresses and QEMU type names:

### Core System

| Peripheral | Base Address | QEMU Type | IRQ Source |
|-----------|-------------|-----------|-----------|
| UART0 | `0x60000000` | `esp32s3.uart` | `ETS_UART0_INTR_SOURCE` |
| UART1 | `0x60010000` | `esp32s3.uart` | `ETS_UART1_INTR_SOURCE` |
| UART2 | `0x6002E000` | `esp32s3.uart` | `ETS_UART2_INTR_SOURCE` |
| GPIO | `0x60004000` | `esp32s3.gpio` | `ETS_GPIO_INTR_SOURCE` |
| Interrupt Matrix | `0x600C2000` | `esp32s3.intmatrix` | — |
| System Clock | `0x600C0000` | `esp32s3.clock` | `ETS_FROM_CPU_INTR0–3` |
| RTC Control | `0x60008000` | `esp32s3.rtc_cntl` | — |

### Memory & Storage

| Peripheral | Base Address | QEMU Type | Notes |
|-----------|-------------|-----------|-------|
| SPI1 (Flash) | `0x60002000` | `esp32s3.spi` | Flash + PSRAM controller |
| EXTMEM (Cache) | `0x600C4000` | `esp32s3.cache` | Also aliased at `0x60040000` |
| eFuse | `0x60007000` | `esp32s3.efuse` | PKG_VERSION in BLOCK1 |
| SDMMC | `0x60028000` | `dwc-sdmmc` | SD card controller |

### Communication

| Peripheral | Base Address | QEMU Type | Notes |
|-----------|-------------|-----------|-------|
| SPI2 (GPSPI) | `0x60024000` | `ssi.esp32s3.gpspi` | General-purpose SPI |
| SPI3 (GPSPI) | `0x60025000` | `ssi.esp32s3.gpspi` | Display SPI bus |
| I2C0 | `0x60013000` | `esp32.i2c` | External I2C |
| I2C1 | `0x60027000` | `esp32.i2c` | Internal (PMIC, codec) |
| I2S0 | `0x6000F000` | `esp32s3.i2s` | Audio interface |
| I2S1 | `0x6002D000` | `esp32s3.i2s` | Audio interface |

### Security

| Peripheral | Base Address | QEMU Type |
|-----------|-------------|-----------|
| SHA | `0x6003B000` | `esp32s3.sha` |
| AES | `0x6003A000` | `esp32s3.aes` |
| RSA | `0x6003C000` | `esp32s3.rsa` |
| HMAC | `0x6003E000` | `esp32s3.hmac` |
| Digital Signature | `0x6003D000` | `esp32s3.ds` |
| XTS-AES | `0x600CC000` | `esp32s3.xts_aes` |
| PMS | `0x600C1000` | `esp32s3.pms` |

### Timers

| Peripheral | Base Address | QEMU Type |
|-----------|-------------|-----------|
| Timer Group 0 | `0x6001F000` | `esp32s3.timg` |
| Timer Group 1 | `0x60020000` | `esp32s3.timg` |
| System Timer | `0x60023000` | `esp32s3.systimer` |

### Connectivity & Display

| Peripheral | Base Address | QEMU Type | Notes |
|-----------|-------------|-----------|-------|
| GDMA | `0x6003F000` | `esp32s3.gdma` | 5 channels |
| OpenCores Ethernet | `0x600CD000` | `open_eth` | Virtual NIC |
| RGB Display | `0x21000000` | `esp32.rgb` | Direct framebuffer |
| ST7789V2 | (on SPI3 bus) | `display.st7789v2` | SPI LCD slave |
| USB Serial JTAG | `0x60038000` | `esp32c3.jtag` | Debug interface |
| TWAI (CAN) | `0x6002B000` | `esp32s3.twai` | CAN bus |
| WiFi (WDEV) | `0x3FF75000` | `esp32s3.wifi` | Register stubs |
| BLE | `0x60011000` | `esp32s3.ble` | API interception |

### I2C Slave Devices

| Device | I2C Bus | Address | QEMU Type |
|--------|---------|---------|-----------|
| ES8311 | I2C1 | `0x18` | `es8311` |
| M5PM1 | I2C1 | `0x6E` | `m5pm1` |

---

## Debugging Cookbook

### Firmware Hangs at Boot

**Symptom:** No output after "2nd stage bootloader" or hangs silently.

**Most likely causes:**

1. **Wrong `-m` value.** Use `-m 8M` for M5StickS3. Larger values (512M) cause `esp_mmu_map` to fail with "no such vaddr range."

2. **Polling loop on unmodeled register.** Enable `ESP32S3_IO_STALL_DETECT` in `esp32s3.c` to log repeated reads to the same address. Common culprits:
   - `0x6000E000–0x6000E0FF`: I2C_ANA_MST registers. Fix: return `0xFFFFFFFF` for offsets 0x044–0x0FF (calibration done), return value with bit 25 cleared for offsets 0x000–0x03C (not busy), return `0x01000000` for offset 0x040 (BBPLL_CAL_DONE).
   - `0x60006000–0x60006FFF`: Radio front-end (FE) calibration. Fix: toggle between 0 and `0xFFFFFFFF` on successive reads. The proprietary WiFi calibration blob polls these with complex multi-register sequences.
   - `0x60008850`: Temperature sensor (TSENS_CTRL). Fix: set bit 8 (TSENS_READY) and provide dummy temperature in bits 7:0.
   - `0x6003101C`: Reserved region command bit. Fix: auto-clear bit 31 on read.

3. **Cache boundary registers returning 0.** Check `esp32s3_cache.c` — the default read case must return `s->regs[index]`, not 0.

### No Display Output

**Symptom:** Emulator runs, serial output appears, but display window is blank.

**Check in order:**

1. Are you using `-display none`? Remove it to get a window.
2. Is `[ST7789V2] Display initialized` in the log? If not, the device isn't being created.
3. Enable `ST7789_DEBUG` in `st7789v2.c` to see if commands arrive (SLPOUT, DISPON, CASET, RASET, RAMWR).
4. Enable `GPSPI_DEBUG` in `esp32s3_gpspi.c` to see if SPI transactions fire.
5. Check that GPIO 45 DC wiring is connected (in `esp32s3_machine_init`).

### Board Detection Wrong

**Symptom:** M5Unified reports wrong board type (e.g., "auto detect board:146").

**Verify eFuse PKG_VERSION:**
- Look for log line: `[EFUSE] Set PKG_VERSION=1 in rd_mac_spi_sys_3=0x00200000`
- If missing, check `esp32s3_efuse_realize()` is being called.

**Verify M5PM1 PMIC responds:**
- Enable `M5PM1_DEBUG` to see I2C reads/writes.
- Detection requires: I2C1 bus, address 0x6E, register 0x00 returns non-zero.

### Adding a New Peripheral

The pattern for adding a peripheral to the ESP32-S3 emulator:

1. **Header:** `include/hw/<category>/<name>.h` — state struct, register definitions, type macros
2. **Source:** `hw/<category>/<name>.c` — implementation with QEMU object model
3. **Build:** Add to `hw/<category>/meson.build` under `CONFIG_XTENSA_ESP32S3`
4. **Wire up:** In `hw/xtensa/esp32s3.c`:
   - Add state to `Esp32s3SocState`
   - `object_initialize_child()` in `esp32s3_soc_init()` or `esp32s3_machine_init()`
   - `sysbus_realize()` + `memory_region_add_subregion_overlap()` at the right address
   - Connect interrupts to the interrupt matrix
   - Connect GPIO signals if needed

### Useful Debug Flags

| File | Flag | Purpose |
|------|------|---------|
| `esp32s3.c` | `ESP32S3_IO_WARNING` | Log all catch-all IO reads/writes |
| `esp32s3.c` | `ESP32S3_IO_STALL_DETECT` | Detect polling loops (100+ reads to same addr) |
| `esp32s3.c` | `ESP32S3_HEARTBEAT` | 5-second wall-clock heartbeat with PC + vtime |
| `esp32s3_gpspi.c` | `GPSPI_DEBUG` | Log every SPI transaction |
| `esp32s3_gpspi.c` | `GPSPI_WARNING` | Log unsupported SPI register access |
| `st7789v2.c` | `ST7789_DEBUG` | Log display commands and parameters |
| `esp32s3_cache.c` | `CACHE_WARNING` | Log unsupported cache register access |
| `m5pm1.c` | `M5PM1_DEBUG` | Log I2C register reads/writes |

---

## Running the Emulator

### Build

```bash
cd /path/to/m5sticks3-emulator
mkdir -p build && cd build
../configure --target-list=xtensa-softmmu
make -j$(nproc)
```

### Run with Flash Image

```bash
./build/qemu-system-xtensa \
    -M esp32s3 \
    -m 8M \
    -drive file=flash_8mb.bin,if=mtd,format=raw \
    -serial mon:stdio \
    -no-reboot
```

### Run with ELF (Development)

```bash
./build/qemu-system-xtensa \
    -M esp32s3 \
    -m 8M \
    -drive file=flash_8mb.bin,if=mtd,format=raw \
    -kernel firmware.elf \
    -serial mon:stdio \
    -no-reboot
```

### Key QEMU Flags

| Flag | Description |
|------|-------------|
| `-M esp32s3` | Select ESP32-S3 machine type |
| `-m 8M` | 8 MB PSRAM (matches M5StickS3 hardware) |
| `-drive file=X,if=mtd,format=raw` | Flash image (2/4/8/16 MB) |
| `-serial mon:stdio` | UART0 output to terminal |
| `-display none` | Headless mode (no display window) |
| `-no-reboot` | Stop on crash instead of rebooting |
| `-kernel X.elf` | Load ELF directly (skips ROM boot) |
| `-s -S` | GDB server on :1234, start paused |

### Flash Image Format

The flash image is a raw binary matching the physical flash layout:

| Offset | Content |
|--------|---------|
| `0x0000` | Bootloader (2nd stage) |
| `0x8000` | Partition table |
| `0x9000` | NVS (non-volatile storage) |
| `0xE000` | OTA data |
| `0x10000` | Application firmware (app0) |
| Variable | SPIFFS/LittleFS, coredump, etc. |

Build the flash image with ESP-IDF or PlatformIO as normal, then pass it to QEMU.

---

## Headless CLI Reference

The `m5sticks3/emu.sh` script provides headless control of the emulator via QMP (QEMU Machine Protocol). It launches QEMU as a daemon with no display window, communicating through a Unix socket for screenshots, button injection, and lifecycle management.

### Subcommands

| Command | Description |
|---------|-------------|
| `start <firmware.bin>` | Launch QEMU headless with QMP control socket |
| `screenshot [output.ppm]` | Capture the ST7789V2 display to a PPM file |
| `press <btnA\|btnB>` | Inject a button press (active-low pulse on GPIO) |
| `serial` | Print accumulated serial output |
| `stop` | Kill QEMU and clean up state files |
| `status` | Show whether the emulator is running, report orphans |
| `cleanup` | Kill everything — tracked instance and any orphans |

### `start <firmware.bin>`

Launches the emulator in the background with no display window. Accepts either a merged flash image (with bootloader + partition table) or an app-only binary:

- **Merged flash image:** Detected by checking for partition table magic (`0xAA50`) at offset `0x8000`. Copied as-is into an 8 MB flash file. The OTA data partition at `0xE000` is zeroed to prevent a bootloader hang.
- **App-only binary:** Placed at offset `0x10000` (the standard app partition start). No bootloader or partition table is present, so ROM boot falls back to trying all flash offsets.

State files created:

| File | Purpose |
|------|---------|
| `.qemu.sock` | QMP Unix socket for control commands |
| `.qemu.pid` | PID of the daemonized QEMU process |
| `.qemu-serial.log` | UART0 serial output (file-backed) |

QEMU is launched with `-icount shift=4,align=off,sleep=on` for deterministic instruction counting. This makes execution reproducible but significantly slower than real-time — see [Known Limitations](#known-limitations).

If a previous instance left stale state (dead PID file, orphaned socket), `start` cleans it up automatically before launching.

### `screenshot [output.ppm]`

Captures the current ST7789V2 display contents via QMP `screendump`. Defaults to `.qemu-screen.ppm` if no output path is given.

```bash
m5sticks3/emu.sh screenshot              # → .qemu-screen.ppm
m5sticks3/emu.sh screenshot /tmp/ui.ppm  # → /tmp/ui.ppm
```

The screendump targets the `st7789v2` device specifically (not the default QEMU console). The output is a raw PPM image at 240×320 resolution — the full ST7789 framebuffer including the offset region. The M5StickS3's visible area is the 135×240 pixel region starting at (52, 40).

**Requires `socat`** for Unix socket communication.

### `press <btnA|btnB>`

Injects a button press by driving the corresponding GPIO pin low (press), waiting 100 ms, then driving it high (release).

| Button | GPIO Pin | Physical Location |
|--------|----------|-------------------|
| btnA | 11 | Front blue button |
| btnB | 12 | Side button |

Accepts case-insensitive variants: `btnA`, `btna`, `a`, `A` (and likewise for B).

```bash
m5sticks3/emu.sh press btnA   # Front button
m5sticks3/emu.sh press b      # Side button
```

**How button injection works:** The M5StickS3's buttons are wired directly to ESP32-S3 GPIO pins 11 and 12 — they are *not* routed through the PMIC or any I2C expander. The buttons are active-low: the GPIO reads HIGH (1) when released and LOW (0) when pressed, with internal pull-ups.

The QMP command `x-gpio-set-pin` is a custom extension that:
1. Clears the `gpio_enable` bit for the target pin (forces input mode, overriding any firmware output configuration)
2. Sets or clears the corresponding bit in `gpio_in` (the input register the firmware reads)

This is necessary because the emulator's GPIO model defaults all pins as outputs. Without clearing `gpio_enable` first, the injected value in `gpio_in` would be masked by the output driver.

### `serial`

Dumps the contents of `.qemu-serial.log` — everything UART0 has printed since boot. This includes ESP-IDF bootloader messages, application `printf`/`ESP_LOG` output, and crash backtraces.

```bash
m5sticks3/emu.sh serial | grep "display"
m5sticks3/emu.sh serial | tail -20
```

### `stop`

Sends SIGTERM to the tracked QEMU process, waits up to 5 seconds for graceful exit, escalates to SIGKILL if needed. Also kills any orphaned QEMU processes from this repo. Removes all state files (`.qemu.sock`, `.qemu.pid`, `.qemu-serial.log`).

### `status`

Reports whether the emulator is running (live PID) or not (no PID file, or stale PID). Also scans for orphaned `qemu-system-xtensa` processes launched from this build directory that lost their PID file.

```
$ m5sticks3/emu.sh status
Emulator is running (PID 12345)

$ m5sticks3/emu.sh status
Emulator is NOT running (no PID file)
  Orphaned QEMU process: PID 67890
Found 1 orphaned QEMU process(es) — run 'emu.sh cleanup' to kill them
```

### `cleanup`

Nuclear option. Stops the tracked instance (if any), kills all orphaned QEMU processes from this repo, and removes all state files. Use this when things get confused — e.g., after a session crash that left QEMU running without a PID file.

---

## Headless Development Workflow

The headless CLI enables a full firmware development loop without a display server or physical device:

```
Compile → Start → Wait → Screenshot → Interact → Inspect → Stop
```

### Step by Step

**1. Compile firmware**

Build with PlatformIO (or ESP-IDF directly). The merged flash image is the preferred input:

```bash
cd /path/to/your-firmware-project
pio run
# Output: .pio/build/<env>/firmware.bin (merged flash image)
```

**2. Start the emulator**

```bash
m5sticks3/emu.sh start /path/to/firmware.bin
# → "Emulator started (PID 12345)"
```

**3. Wait for boot**

Under `icount shift=4`, boot takes significantly longer than real hardware. Typical timings:

| Firmware complexity | Boot time (approximate) |
|--------------------|------------------------|
| Minimal (no WiFi, no crypto) | 15–20 seconds |
| Standard M5Unified app | 25–35 seconds |
| WiFi-enabled (with phy_init) | 45–60+ seconds |

Monitor progress via serial output:

```bash
# Watch for boot completion markers
m5sticks3/emu.sh serial | tail -5
```

Look for indicators like `M5Unified::begin()` completing, display init messages, or your application's startup log.

**4. Take a screenshot**

```bash
m5sticks3/emu.sh screenshot /tmp/display.ppm
# → "/tmp/display.ppm"
open /tmp/display.ppm   # macOS: opens in Preview
```

Take multiple screenshots over time to observe display state changes.

**5. Interact with buttons**

```bash
m5sticks3/emu.sh press btnA
sleep 2                          # Let firmware process the event
m5sticks3/emu.sh screenshot /tmp/after-press.ppm
```

**6. Read serial output**

```bash
m5sticks3/emu.sh serial         # Full output
m5sticks3/emu.sh serial | grep -i error   # Filter for errors
```

**7. Stop when done**

```bash
m5sticks3/emu.sh stop
# → "Emulator stopped (was PID 12345)"
```

### Scripted Testing

The headless interface is designed for automation. Example test script:

```bash
#!/bin/bash
set -e

m5sticks3/emu.sh start .pio/build/m5sticks3/firmware.bin
sleep 30   # Wait for boot

# Verify display shows expected content
m5sticks3/emu.sh screenshot /tmp/boot-screen.ppm

# Simulate user interaction
m5sticks3/emu.sh press btnA
sleep 2
m5sticks3/emu.sh screenshot /tmp/after-btn.ppm

# Check serial for errors
if m5sticks3/emu.sh serial | grep -qi "guru meditation\|abort\|panic"; then
    echo "FAIL: firmware crashed"
    m5sticks3/emu.sh stop
    exit 1
fi

m5sticks3/emu.sh stop
echo "PASS"
```

---

## Known Limitations

### Boot Speed Under icount

The `-icount shift=4` flag enables deterministic instruction counting, which is required for reproducible behavior but makes execution ~10–50× slower than real hardware. Crypto-heavy boot phases (secure boot verification, WiFi PHY calibration) are particularly affected because they perform millions of hash/AES operations that on real hardware complete in hardware accelerator cycles.

There is no workaround short of disabling icount (which sacrifices determinism). For iterative development, consider firmware that skips WiFi init at boot.

### WiFi Requires ELF Loading

WiFi functionality only works when loading firmware via `-kernel firmware.elf`. The ELF loader patches WiFi API entry points with SIMCALL trampolines that intercept calls to the proprietary WiFi blob and route them through the emulated OpenCores Ethernet NIC.

With raw flash images, the proprietary WiFi blob runs natively on the emulated CPU and tries to talk to radio hardware (FE/FE2 registers) that isn't fully emulated. The catch-all's toggle strategy gets the calibration to complete, but actual packet TX/RX doesn't work.

### BLE is macOS-Only

BLE emulation uses the host's Bluetooth stack via macOS CoreBluetooth APIs. It's not available on Linux. Like WiFi, it requires ELF loading for the API interception trampolines.

### QMP Responsiveness During Boot

QMP commands (`screenshot`, `press`) can be slow or unresponsive during CPU-intensive boot phases. The QMP socket is serviced by QEMU's main loop, which yields to guest CPU execution. During long-running computations (e.g., secure boot hash verification), the main loop may not run for several seconds.

**Workaround:** If a QMP command times out, wait a few seconds and retry. The `socat` timeout in `emu.sh` is set to 2 seconds — if QEMU is deep in a computation, this may not be enough. The command isn't lost; it will execute when the main loop next runs.

### Screenshot Timing

The `screendump` command captures whatever is in the ST7789V2's framebuffer at the moment the command executes. If firmware is in the middle of a multi-transaction screen update (e.g., drawing line by line), you may capture a partially-rendered frame. There is no vsync or "frame complete" signal — the display updates incrementally as SPI transactions arrive.

### Flash Image vs. ELF Trade-offs

| Feature | Flash Image | ELF |
|---------|-------------|-----|
| Boot path | Full ROM → bootloader → app | Direct to entry point |
| WiFi | Calibration runs, but no packets | Works (API interception) |
| BLE | Not functional | Works (macOS only) |
| Boot time | Longer (ROM + 2nd stage) | Shorter |
| Fidelity | Higher (real boot sequence) | Lower (skips ROM) |
| NVS | Works (flash-backed) | Not available |
