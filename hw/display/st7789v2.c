/*
 * ST7789V2 SPI LCD controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Emulates the ST7789V2 as an SSI slave. The firmware sends commands and
 * pixel data over SPI exactly as it would to real hardware. This model
 * parses the ST7789 command protocol, maintains a framebuffer, and renders
 * it through QEMU's display console.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/ssi.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qemu/error-report.h"
#include "hw/display/st7789v2.h"

#define ST7789_DEBUG   0
#define ST7789_WARNING 1


/*
 * Convert RGB565 to the host display format (xRGB8888).
 * RGB565: RRRR_RGGG_GGGB_BBBB
 */
static inline uint32_t rgb565_to_xrgb8888(uint16_t rgb565)
{
    uint32_t r = (rgb565 >> 11) & 0x1F;
    uint32_t g = (rgb565 >> 5)  & 0x3F;
    uint32_t b = (rgb565 >> 0)  & 0x1F;

    /* Expand to 8 bits per channel */
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    return (r << 16) | (g << 8) | b;
}


static void st7789v2_mark_dirty(St7789v2State *s, uint16_t x, uint16_t y)
{
    if (!s->dirty) {
        s->dirty = true;
        s->dirty_x1 = s->dirty_x2 = x;
        s->dirty_y1 = s->dirty_y2 = y;
    } else {
        if (x < s->dirty_x1) s->dirty_x1 = x;
        if (x > s->dirty_x2) s->dirty_x2 = x;
        if (y < s->dirty_y1) s->dirty_y1 = y;
        if (y > s->dirty_y2) s->dirty_y2 = y;
    }
}


/*
 * Write a single pixel into the framebuffer at the current cursor position,
 * then advance the cursor within the CASET/RASET window.
 */
static void st7789v2_write_pixel(St7789v2State *s, uint16_t rgb565)
{
    uint16_t col = s->cur_col;
    uint16_t row = s->cur_row;

    /* Bounds check against native framebuffer */
    if (col < ST7789V2_NATIVE_WIDTH && row < ST7789V2_NATIVE_HEIGHT) {
        s->framebuffer[row * ST7789V2_NATIVE_WIDTH + col] = rgb565;
        st7789v2_mark_dirty(s, col, row);
    }

    /* Advance cursor: columns first, then rows, wrapping within the window */
    s->cur_col++;
    if (s->cur_col > s->col_end) {
        s->cur_col = s->col_start;
        s->cur_row++;
        if (s->cur_row > s->row_end) {
            s->cur_row = s->row_start;
        }
    }
}


/*
 * Handle a command byte (DC=0).
 */
static void st7789v2_command(St7789v2State *s, uint8_t cmd)
{
    s->current_cmd = cmd;
    s->param_index = 0;
    s->pixel_half = false;

    switch (cmd) {
    case ST7789_SWRESET:
#if ST7789_DEBUG
        info_report("[ST7789] SWRESET");
#endif
        s->sleep = true;
        s->display_on = false;
        s->inversion = false;
        s->madctl = 0;
        s->colmod = ST7789_COLMOD_16BIT;
        s->col_start = 0;
        s->col_end = ST7789V2_NATIVE_WIDTH - 1;
        s->row_start = 0;
        s->row_end = ST7789V2_NATIVE_HEIGHT - 1;
        s->cur_col = 0;
        s->cur_row = 0;
        break;

    case ST7789_SLPIN:
#if ST7789_DEBUG
        info_report("[ST7789] SLPIN");
#endif
        s->sleep = true;
        break;

    case ST7789_SLPOUT:
#if ST7789_DEBUG
        info_report("[ST7789] SLPOUT");
#endif
        s->sleep = false;
        break;

    case ST7789_NORON:
        break;

    case ST7789_INVOFF:
        s->inversion = false;
        break;

    case ST7789_INVON:
        s->inversion = true;
        break;

    case ST7789_DISPOFF:
#if ST7789_DEBUG
        info_report("[ST7789] DISPOFF");
#endif
        s->display_on = false;
        break;

    case ST7789_DISPON:
#if ST7789_DEBUG
        info_report("[ST7789] DISPON");
#endif
        s->display_on = true;
        break;

    case ST7789_CASET:
    case ST7789_RASET:
    case ST7789_MADCTL:
    case ST7789_COLMOD:
    case ST7789_RAMWR:
    case ST7789_RAMWRC:
        /* These commands have data parameters; handled in st7789v2_data() */
        break;

    case ST7789_NOP:
    case ST7789_PTLON:
    case ST7789_PTLAR:
    case ST7789_VSCRDEF:
    case ST7789_TEOFF:
    case ST7789_TEON:
    case ST7789_VSCRSADD:
        /* Accepted but not implemented for MVP */
        break;

    default:
#if ST7789_WARNING
        warn_report("[ST7789] Unknown command 0x%02x", cmd);
#endif
        break;
    }
}


/*
 * Handle a data byte (DC=1), interpreted according to current_cmd.
 */
static void st7789v2_data(St7789v2State *s, uint8_t data)
{
    switch (s->current_cmd) {
    case ST7789_CASET:
        /* 4 bytes: XS_H, XS_L, XE_H, XE_L */
        s->param_buf[s->param_index] = data;
        s->param_index++;
        if (s->param_index >= 4) {
            s->col_start = (s->param_buf[0] << 8) | s->param_buf[1];
            s->col_end   = (s->param_buf[2] << 8) | s->param_buf[3];
            s->cur_col = s->col_start;
#if ST7789_DEBUG
            info_report("[ST7789] CASET col=%d..%d", s->col_start, s->col_end);
#endif
            s->param_index = 0;
        }
        break;

    case ST7789_RASET:
        /* 4 bytes: YS_H, YS_L, YE_H, YE_L */
        s->param_buf[s->param_index] = data;
        s->param_index++;
        if (s->param_index >= 4) {
            s->row_start = (s->param_buf[0] << 8) | s->param_buf[1];
            s->row_end   = (s->param_buf[2] << 8) | s->param_buf[3];
            s->cur_row = s->row_start;
#if ST7789_DEBUG
            info_report("[ST7789] RASET row=%d..%d", s->row_start, s->row_end);
#endif
            s->param_index = 0;
        }
        break;

    case ST7789_MADCTL:
        s->madctl = data;
#if ST7789_DEBUG
        info_report("[ST7789] MADCTL 0x%02x", data);
#endif
        break;

    case ST7789_COLMOD:
        s->colmod = data & 0x07;
#if ST7789_DEBUG
        info_report("[ST7789] COLMOD 0x%02x", data);
#endif
        break;

    case ST7789_RAMWR:
    case ST7789_RAMWRC:
        /*
         * Pixel data stream. In RGB565 mode (the common case), each pixel
         * is 2 bytes: high byte first, then low byte.
         */
        if (s->colmod == ST7789_COLMOD_16BIT) {
            if (!s->pixel_half) {
                s->pixel_byte = data;
                s->pixel_half = true;
            } else {
                uint16_t rgb565 = (s->pixel_byte << 8) | data;
                st7789v2_write_pixel(s, rgb565);
                s->pixel_half = false;
            }
        } else {
            /*
             * For 18-bit mode (3 bytes/pixel), treat as RGB666 and
             * convert to RGB565. This is rare for the M5StickS3.
             */
            s->param_buf[s->param_index] = data;
            s->param_index++;
            if (s->param_index >= 3) {
                uint8_t r = s->param_buf[0] >> 3;
                uint8_t g = s->param_buf[1] >> 2;
                uint8_t b = s->param_buf[2] >> 3;
                uint16_t rgb565 = (r << 11) | (g << 5) | b;
                st7789v2_write_pixel(s, rgb565);
                s->param_index = 0;
            }
        }
        break;

    default:
        /* Data for unhandled commands — ignore */
        break;
    }
}


/*
 * SSI slave transfer callback. Called for each byte sent over SPI.
 * Returns 0 (displays don't send data back on MOSI).
 */
static uint32_t st7789v2_transfer(SSIPeripheral *dev, uint32_t val)
{
    St7789v2State *s = ST7789V2(dev);
    uint8_t byte = (uint8_t)val;

    if (s->dc == 0) {
        st7789v2_command(s, byte);
    } else {
        st7789v2_data(s, byte);
    }

    return 0;
}


/*
 * DC (Data/Command) pin handler. Connected from GPIO.
 */
static void st7789v2_dc_handler(void *opaque, int n, int level)
{
    St7789v2State *s = ST7789V2(opaque);
    s->dc = level;
}


/*
 * QEMU display console gfx_update callback.
 * Converts the RGB565 framebuffer to the console surface format and
 * pushes dirty regions.
 */
static void st7789v2_gfx_update(void *opaque)
{
    St7789v2State *s = (St7789v2State *)opaque;

    if (!s->con || !s->dirty || !s->display_on) {
        return;
    }

    DisplaySurface *surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    uint32_t *dest = (uint32_t *)surface_data(surface);
    int dest_stride = surface_stride(surface) / sizeof(uint32_t);

    /* Update dirty region */
    uint16_t x1 = s->dirty_x1;
    uint16_t y1 = s->dirty_y1;
    uint16_t x2 = MIN(s->dirty_x2, ST7789V2_NATIVE_WIDTH - 1);
    uint16_t y2 = MIN(s->dirty_y2, ST7789V2_NATIVE_HEIGHT - 1);

    for (uint16_t y = y1; y <= y2; y++) {
        for (uint16_t x = x1; x <= x2; x++) {
            uint16_t rgb565 = s->framebuffer[y * ST7789V2_NATIVE_WIDTH + x];
            if (s->inversion) {
                rgb565 ^= 0xFFFF;
            }
            dest[y * dest_stride + x] = rgb565_to_xrgb8888(rgb565);
        }
    }

    dpy_gfx_update(s->con, x1, y1, x2 - x1 + 1, y2 - y1 + 1);

    s->dirty = false;
}


static void st7789v2_invalidate(void *opaque)
{
    St7789v2State *s = (St7789v2State *)opaque;

    /* Mark the entire display as dirty to force a full redraw */
    s->dirty = true;
    s->dirty_x1 = 0;
    s->dirty_y1 = 0;
    s->dirty_x2 = ST7789V2_NATIVE_WIDTH - 1;
    s->dirty_y2 = ST7789V2_NATIVE_HEIGHT - 1;
}


static const GraphicHwOps st7789v2_gfx_ops = {
    .invalidate  = st7789v2_invalidate,
    .gfx_update  = st7789v2_gfx_update,
};


static void st7789v2_realize(SSIPeripheral *dev, Error **errp)
{
    St7789v2State *s = ST7789V2(dev);

    /* Create the QEMU display console */
    s->con = graphic_console_init(DEVICE(dev), 0, &st7789v2_gfx_ops, s);

    /* Create the display surface at native resolution */
    DisplaySurface *surface = qemu_create_displaysurface_from(
        ST7789V2_NATIVE_WIDTH, ST7789V2_NATIVE_HEIGHT,
        PIXMAN_x8r8g8b8,
        ST7789V2_NATIVE_WIDTH * 4, NULL);
    surface->flags = QEMU_ALLOCATED_FLAG;
    dpy_gfx_replace_surface(s->con, surface);

    /* Initialize to black */
    void *data = surface_data(qemu_console_surface(s->con));
    memset(data, 0, ST7789V2_NATIVE_WIDTH * ST7789V2_NATIVE_HEIGHT * 4);

    /* DC pin input */
    qdev_init_gpio_in_named(DEVICE(dev), st7789v2_dc_handler, "dc", 1);

    info_report("[ST7789V2] Display initialized (%dx%d)",
                ST7789V2_NATIVE_WIDTH, ST7789V2_NATIVE_HEIGHT);
}


static void st7789v2_reset_hold(Object *obj, ResetType type)
{
    St7789v2State *s = ST7789V2(obj);

    s->dc = 0;
    s->sleep = true;
    s->display_on = false;
    s->inversion = false;
    s->madctl = 0;
    s->colmod = ST7789_COLMOD_16BIT;

    s->col_start = 0;
    s->col_end = ST7789V2_NATIVE_WIDTH - 1;
    s->row_start = 0;
    s->row_end = ST7789V2_NATIVE_HEIGHT - 1;
    s->cur_col = 0;
    s->cur_row = 0;

    s->current_cmd = ST7789_NOP;
    s->param_index = 0;
    s->pixel_half = false;
    s->dirty = false;

    memset(s->framebuffer, 0, sizeof(s->framebuffer));
}


static void st7789v2_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *ssc = SSI_PERIPHERAL_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    ssc->realize = st7789v2_realize;
    ssc->transfer = st7789v2_transfer;
    /*
     * Use SSI_CS_NONE so the display always receives SPI traffic.
     * The real firmware manages CS via GPIO 41 (not through the SPI
     * controller's built-in CS pins), so the SSI framework's CS
     * gating would block all transfers.  With only one device on the
     * bus, CS gating isn't needed for correctness.
     */
    ssc->cs_polarity = SSI_CS_NONE;
    rc->phases.hold = st7789v2_reset_hold;
}


static const TypeInfo st7789v2_info = {
    .name          = TYPE_ST7789V2,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(St7789v2State),
    .class_init    = st7789v2_class_init,
};


static void st7789v2_register_types(void)
{
    type_register_static(&st7789v2_info);
}

type_init(st7789v2_register_types)
