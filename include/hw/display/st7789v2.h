/*
 * ST7789V2 SPI LCD controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Emulates the ST7789V2 display controller as an SSI (SPI) slave device.
 * Receives commands and pixel data over SPI, maintains an internal
 * framebuffer, and renders to a QEMU display console.
 *
 * The DC (Data/Command) pin determines whether incoming SPI bytes are
 * commands (DC=0) or data (DC=1). This pin must be wired from the GPIO
 * model to this device's "dc" GPIO input.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/ssi/ssi.h"
#include "ui/console.h"

#define TYPE_ST7789V2 "display.st7789v2"
#define ST7789V2(obj) OBJECT_CHECK(St7789v2State, (obj), TYPE_ST7789V2)

/*
 * ST7789V2 native resolution is 240x320. The M5StickS3 uses a 135x240
 * window within this, offset by (52, 40). We allocate the full native
 * framebuffer so MADCTL rotation works correctly.
 */
#define ST7789V2_NATIVE_WIDTH   240
#define ST7789V2_NATIVE_HEIGHT  320

/* ST7789 command set (subset relevant for display operation) */
#define ST7789_NOP      0x00
#define ST7789_SWRESET  0x01
#define ST7789_RDDID    0x04
#define ST7789_RDDST    0x09
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11
#define ST7789_PTLON    0x12
#define ST7789_NORON    0x13
#define ST7789_INVOFF   0x20
#define ST7789_INVON    0x21
#define ST7789_DISPOFF  0x28
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A
#define ST7789_RASET    0x2B
#define ST7789_RAMWR    0x2C
#define ST7789_RAMRD    0x2E
#define ST7789_PTLAR    0x30
#define ST7789_VSCRDEF  0x33
#define ST7789_TEOFF    0x34
#define ST7789_TEON     0x35
#define ST7789_MADCTL   0x36
#define ST7789_VSCRSADD 0x37
#define ST7789_COLMOD   0x3A
#define ST7789_RAMWRC   0x3C
#define ST7789_RAMRDC   0x3E


/* MADCTL bits */
#define MADCTL_MY   0x80  /* Row address order */
#define MADCTL_MX   0x40  /* Column address order */
#define MADCTL_MV   0x20  /* Row/Column exchange */
#define MADCTL_ML   0x10  /* Vertical refresh order */
#define MADCTL_BGR  0x08  /* BGR color order (vs RGB) */
#define MADCTL_MH   0x04  /* Horizontal refresh order */


typedef enum {
    ST7789_COLMOD_12BIT = 0x03,
    ST7789_COLMOD_16BIT = 0x05,  /* RGB565 — the common case */
    ST7789_COLMOD_18BIT = 0x06,
} St7789Colmod;


typedef struct St7789v2State {
    SSIPeripheral parent_obj;

    QemuConsole *con;

    /* DC pin state: 0 = command, 1 = data */
    int dc;

    /* Display state */
    bool sleep;
    bool display_on;
    bool inversion;
    uint8_t madctl;
    St7789Colmod colmod;

    /* Column/row address window */
    uint16_t col_start;
    uint16_t col_end;
    uint16_t row_start;
    uint16_t row_end;

    /* Current write cursor within the window */
    uint16_t cur_col;
    uint16_t cur_row;

    /* Command parser state */
    uint8_t current_cmd;
    int     param_index;     /* Byte index within current command's parameters */
    uint8_t param_buf[4];    /* Buffer for multi-byte parameters (CASET/RASET) */

    /* Pixel accumulator for RGB565 (2 bytes per pixel) */
    uint8_t pixel_byte;      /* First byte of a pixel pair */
    bool    pixel_half;      /* true = waiting for second byte */

    /*
     * Internal framebuffer: 240x320 pixels in RGB565.
     * Stored as native ST7789 coordinates; MADCTL transforms are applied
     * when writing pixels, not when reading back the framebuffer.
     */
    uint16_t framebuffer[ST7789V2_NATIVE_WIDTH * ST7789V2_NATIVE_HEIGHT];

    /* Dirty tracking for efficient display updates */
    bool dirty;
    uint16_t dirty_x1, dirty_y1, dirty_x2, dirty_y2;

} St7789v2State;
