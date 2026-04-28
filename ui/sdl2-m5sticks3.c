/*
 * M5StickS3 device shell rendering for QEMU SDL2 display
 *
 * Renders a visual representation of the M5StickS3 device around the
 * ST7789V2 framebuffer output, with clickable buttons that inject GPIO state.
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/sdl2.h"
#include "qom/object.h"
#include "hw/gpio/esp32_gpio.h"

/*
 * Layout constants (all in final window pixel coordinates).
 * The ST7789V2 framebuffer is 240x320. The M5StickS3 shows a 135x240
 * viewport within it (offset 52,40). We display that viewport scaled 2x.
 */
#define M5_SCALE            2

/* Visible portion of the framebuffer (ST7789 native coords) */
#define M5_FB_OFFSET_X      52
#define M5_FB_OFFSET_Y      40
#define M5_FB_VISIBLE_W     135
#define M5_FB_VISIBLE_H     240

/* Scaled display size */
#define M5_DISPLAY_W        (M5_FB_VISIBLE_W * M5_SCALE)  /* 270 */
#define M5_DISPLAY_H        (M5_FB_VISIBLE_H * M5_SCALE)  /* 480 */

/* Device body layout */
#define M5_DEVICE_X         40
#define M5_DEVICE_Y         20
#define M5_DEVICE_W         342
#define M5_DEVICE_H         680
#define M5_DEVICE_RADIUS    24

/* Screen (display) position within window */
#define M5_SCREEN_X         (M5_DEVICE_X + 36)   /* 76 */
#define M5_SCREEN_Y         (M5_DEVICE_Y + 40)   /* 60 */

/* Screen bezel (slightly larger than display) */
#define M5_BEZEL_PAD        6
#define M5_BEZEL_X          (M5_SCREEN_X - M5_BEZEL_PAD)
#define M5_BEZEL_Y          (M5_SCREEN_Y - M5_BEZEL_PAD)
#define M5_BEZEL_W          (M5_DISPLAY_W + M5_BEZEL_PAD * 2)
#define M5_BEZEL_H          (M5_DISPLAY_H + M5_BEZEL_PAD * 2)

/* Front button (KEY1) — blue pill shape below screen */
#define M5_BTN_FRONT_W      112
#define M5_BTN_FRONT_H      36
#define M5_BTN_FRONT_X      (M5_SCREEN_X + (M5_DISPLAY_W - M5_BTN_FRONT_W) / 2)
#define M5_BTN_FRONT_Y      (M5_SCREEN_Y + M5_DISPLAY_H + 40)

/* Side button (KEY2) — small gray button on left edge */
#define M5_BTN_SIDE_W       12
#define M5_BTN_SIDE_H       56
#define M5_BTN_SIDE_X       (M5_DEVICE_X - M5_BTN_SIDE_W + 2)
#define M5_BTN_SIDE_Y       (M5_DEVICE_Y + 160)

/* USB-C port hint at bottom */
#define M5_USB_W            32
#define M5_USB_H            10
#define M5_USB_X            (M5_DEVICE_X + (M5_DEVICE_W - M5_USB_W) / 2)
#define M5_USB_Y            (M5_DEVICE_Y + M5_DEVICE_H - 24)

/* Total window size */
#define M5_WIN_W            (M5_DEVICE_X + M5_DEVICE_W + 40)  /* 422 */
#define M5_WIN_H            (M5_DEVICE_Y + M5_DEVICE_H + 20)  /* 720 */

/* Colors (RGB) */
#define M5_COLOR_BG_R       0x1a
#define M5_COLOR_BG_G       0x1a
#define M5_COLOR_BG_B       0x2e

#define M5_COLOR_BODY_R     0x42
#define M5_COLOR_BODY_G     0x42
#define M5_COLOR_BODY_B     0x4a

#define M5_COLOR_BEZEL_R    0x22
#define M5_COLOR_BEZEL_G    0x22
#define M5_COLOR_BEZEL_B    0x22

#define M5_COLOR_BTN_FRONT_R    0x02
#define M5_COLOR_BTN_FRONT_G    0x88
#define M5_COLOR_BTN_FRONT_B    0xd1

#define M5_COLOR_BTN_FRONT_HI_R    0x4f
#define M5_COLOR_BTN_FRONT_HI_G    0xc3
#define M5_COLOR_BTN_FRONT_HI_B    0xf7

#define M5_COLOR_BTN_FRONT_HELD_R  0x01
#define M5_COLOR_BTN_FRONT_HELD_G  0x57
#define M5_COLOR_BTN_FRONT_HELD_B  0x9b

#define M5_COLOR_BTN_SIDE_R     0x55
#define M5_COLOR_BTN_SIDE_G     0x55
#define M5_COLOR_BTN_SIDE_B     0x55

#define M5_COLOR_BTN_SIDE_HELD_R    0x99
#define M5_COLOR_BTN_SIDE_HELD_G    0x99
#define M5_COLOR_BTN_SIDE_HELD_B    0x99

#define M5_COLOR_USB_R      0x2a
#define M5_COLOR_USB_G      0x2a
#define M5_COLOR_USB_B      0x30

#define M5_COLOR_LABEL_R    0x88
#define M5_COLOR_LABEL_G    0x88
#define M5_COLOR_LABEL_B    0x88

/* GPIO pin assignments (active-low) */
#define M5_KEY1_GPIO        11
#define M5_KEY2_GPIO        12


/* Cached GPIO device pointer (lazy-initialized) */
static Esp32GpioState *m5_gpio_dev = NULL;


/*
 * Resolve the GPIO device from the QOM tree (lazy init).
 */
static Esp32GpioState *m5_get_gpio(void)
{
    if (!m5_gpio_dev) {
        Object *obj = object_resolve_path("/machine/soc/gpio", NULL);
        if (obj) {
            m5_gpio_dev = (Esp32GpioState *)obj;
        }
    }
    return m5_gpio_dev;
}


/*
 * Check if this console should use M5StickS3 device shell mode.
 * We detect based on the surface being 240x320 (the ST7789V2 native size).
 */
bool m5sticks3_detect(struct sdl2_console *scon)
{
    if (!scon->surface) {
        return false;
    }
    int w = surface_width(scon->surface);
    int h = surface_height(scon->surface);
    return (w == 240 && h == 320);
}


/*
 * Get the window dimensions for M5StickS3 mode.
 */
void m5sticks3_get_window_size(int *w, int *h)
{
    *w = M5_WIN_W;
    *h = M5_WIN_H;
}


/*
 * Draw a filled rounded rectangle using SDL2 primitives.
 * Approximation: draws a rectangle with corner "notches" for rounding effect.
 */
static void draw_rounded_rect(SDL_Renderer *renderer,
                              int x, int y, int w, int h, int radius)
{
    /* Main body (excluding corners) */
    SDL_Rect rects[3];

    /* Horizontal strip (full width, minus top/bottom radius) */
    rects[0].x = x;
    rects[0].y = y + radius;
    rects[0].w = w;
    rects[0].h = h - 2 * radius;

    /* Top strip (narrower, within radius area) */
    rects[1].x = x + radius;
    rects[1].y = y;
    rects[1].w = w - 2 * radius;
    rects[1].h = radius;

    /* Bottom strip */
    rects[2].x = x + radius;
    rects[2].y = y + h - radius;
    rects[2].w = w - 2 * radius;
    rects[2].h = radius;

    SDL_RenderFillRects(renderer, rects, 3);

    /* Draw rounded corners as filled circles (approximate with small rects) */
    int cx, cy;
    for (int corner = 0; corner < 4; corner++) {
        switch (corner) {
        case 0: cx = x + radius; cy = y + radius; break;         /* top-left */
        case 1: cx = x + w - radius; cy = y + radius; break;     /* top-right */
        case 2: cx = x + radius; cy = y + h - radius; break;     /* bottom-left */
        case 3: cx = x + w - radius; cy = y + h - radius; break; /* bottom-right */
        }
        /* Fill quarter-circle */
        for (int dy = -radius; dy <= 0; dy++) {
            int dx = (int)sqrt((double)(radius * radius - dy * dy));
            SDL_Rect line;
            switch (corner) {
            case 0:
                line.x = cx - dx; line.y = cy + dy;
                line.w = dx; line.h = 1;
                break;
            case 1:
                line.x = cx; line.y = cy + dy;
                line.w = dx; line.h = 1;
                break;
            case 2:
                line.x = cx - dx; line.y = cy - dy;
                line.w = dx; line.h = 1;
                break;
            case 3:
                line.x = cx; line.y = cy - dy;
                line.w = dx; line.h = 1;
                break;
            }
            SDL_RenderFillRect(renderer, &line);
        }
    }
}


/*
 * Draw a filled pill/capsule shape (fully rounded ends).
 */
static void draw_pill(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    int radius = h / 2;
    draw_rounded_rect(renderer, x, y, w, h, radius);
}


/*
 * Render the full M5StickS3 device shell and framebuffer.
 */
void m5sticks3_render(struct sdl2_console *scon)
{
    SDL_Renderer *renderer = scon->real_renderer;
    if (!renderer || !scon->texture) {
        return;
    }

    /* Clear with background color */
    SDL_SetRenderDrawColor(renderer, M5_COLOR_BG_R, M5_COLOR_BG_G,
                           M5_COLOR_BG_B, 255);
    SDL_RenderClear(renderer);

    /* Draw device body */
    SDL_SetRenderDrawColor(renderer, M5_COLOR_BODY_R, M5_COLOR_BODY_G,
                           M5_COLOR_BODY_B, 255);
    draw_rounded_rect(renderer, M5_DEVICE_X, M5_DEVICE_Y,
                      M5_DEVICE_W, M5_DEVICE_H, M5_DEVICE_RADIUS);

    /* Draw screen bezel (dark inset around the display) */
    SDL_SetRenderDrawColor(renderer, M5_COLOR_BEZEL_R, M5_COLOR_BEZEL_G,
                           M5_COLOR_BEZEL_B, 255);
    SDL_Rect bezel = { M5_BEZEL_X, M5_BEZEL_Y, M5_BEZEL_W, M5_BEZEL_H };
    SDL_RenderFillRect(renderer, &bezel);

    /* Render the framebuffer (135x240 viewport from the 240x320 surface) */
    SDL_Rect src_rect = {
        M5_FB_OFFSET_X, M5_FB_OFFSET_Y,
        M5_FB_VISIBLE_W, M5_FB_VISIBLE_H
    };
    SDL_Rect dst_rect = {
        M5_SCREEN_X, M5_SCREEN_Y,
        M5_DISPLAY_W, M5_DISPLAY_H
    };
    SDL_RenderCopy(renderer, scon->texture, &src_rect, &dst_rect);

    /* Draw front button (KEY1) — blue pill shape */
    if (scon->m5_key1_held) {
        SDL_SetRenderDrawColor(renderer, M5_COLOR_BTN_FRONT_HELD_R,
                               M5_COLOR_BTN_FRONT_HELD_G,
                               M5_COLOR_BTN_FRONT_HELD_B, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, M5_COLOR_BTN_FRONT_R,
                               M5_COLOR_BTN_FRONT_G,
                               M5_COLOR_BTN_FRONT_B, 255);
    }
    draw_pill(renderer, M5_BTN_FRONT_X, M5_BTN_FRONT_Y,
              M5_BTN_FRONT_W, M5_BTN_FRONT_H);

    /* Draw a highlight on the front button when not held */
    if (!scon->m5_key1_held) {
        SDL_SetRenderDrawColor(renderer, M5_COLOR_BTN_FRONT_HI_R,
                               M5_COLOR_BTN_FRONT_HI_G,
                               M5_COLOR_BTN_FRONT_HI_B, 128);
        SDL_Rect highlight = {
            M5_BTN_FRONT_X + 4, M5_BTN_FRONT_Y + 2,
            M5_BTN_FRONT_W - 8, M5_BTN_FRONT_H / 3
        };
        SDL_RenderFillRect(renderer, &highlight);
    }

    /* Draw side button (KEY2) — small gray rectangle on left edge */
    if (scon->m5_key2_held) {
        SDL_SetRenderDrawColor(renderer, M5_COLOR_BTN_SIDE_HELD_R,
                               M5_COLOR_BTN_SIDE_HELD_G,
                               M5_COLOR_BTN_SIDE_HELD_B, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, M5_COLOR_BTN_SIDE_R,
                               M5_COLOR_BTN_SIDE_G,
                               M5_COLOR_BTN_SIDE_B, 255);
    }
    SDL_Rect side_btn = { M5_BTN_SIDE_X, M5_BTN_SIDE_Y,
                          M5_BTN_SIDE_W, M5_BTN_SIDE_H };
    SDL_RenderFillRect(renderer, &side_btn);

    /* Draw USB-C port hint at bottom */
    SDL_SetRenderDrawColor(renderer, M5_COLOR_USB_R, M5_COLOR_USB_G,
                           M5_COLOR_USB_B, 255);
    SDL_Rect usb = { M5_USB_X, M5_USB_Y, M5_USB_W, M5_USB_H };
    SDL_RenderFillRect(renderer, &usb);
    /* USB-C inner (metal contact area) */
    SDL_SetRenderDrawColor(renderer, 0x55, 0x55, 0x55, 255);
    SDL_Rect usb_inner = { M5_USB_X + 2, M5_USB_Y + 2,
                           M5_USB_W - 4, M5_USB_H - 4 };
    SDL_RenderFillRect(renderer, &usb_inner);

    /* Draw "KEY1" label below front button */
    /* (SDL2 doesn't have text rendering without SDL_ttf, skip for now) */

    SDL_RenderPresent(renderer);
}


/*
 * Handle a mouse click event. Returns true if the click was consumed
 * (i.e., hit a button area).
 */
bool m5sticks3_handle_mouse_click(struct sdl2_console *scon, int x, int y)
{
    /* Check front button (KEY1) hit */
    if (x >= M5_BTN_FRONT_X && x < M5_BTN_FRONT_X + M5_BTN_FRONT_W &&
        y >= M5_BTN_FRONT_Y && y < M5_BTN_FRONT_Y + M5_BTN_FRONT_H) {
        /* Toggle KEY1 state */
        scon->m5_key1_held = !scon->m5_key1_held;
        m5sticks3_set_button(M5_KEY1_GPIO, scon->m5_key1_held);
        return true;
    }

    /* Check side button (KEY2) hit */
    if (x >= M5_BTN_SIDE_X && x < M5_BTN_SIDE_X + M5_BTN_SIDE_W &&
        y >= M5_BTN_SIDE_Y && y < M5_BTN_SIDE_Y + M5_BTN_SIDE_H) {
        /* Toggle KEY2 state */
        scon->m5_key2_held = !scon->m5_key2_held;
        m5sticks3_set_button(M5_KEY2_GPIO, scon->m5_key2_held);
        return true;
    }

    return false;
}


/*
 * Handle keyboard shortcuts for buttons.
 * 'A' key = KEY1, 'B' key = KEY2.
 * Returns true if the key was consumed.
 */
bool m5sticks3_handle_key(struct sdl2_console *scon, SDL_Scancode scancode,
                          bool down)
{
    switch (scancode) {
    case SDL_SCANCODE_A:
        if (down) {
            scon->m5_key1_held = !scon->m5_key1_held;
            m5sticks3_set_button(M5_KEY1_GPIO, scon->m5_key1_held);
        }
        return true;

    case SDL_SCANCODE_B:
        if (down) {
            scon->m5_key2_held = !scon->m5_key2_held;
            m5sticks3_set_button(M5_KEY2_GPIO, scon->m5_key2_held);
        }
        return true;

    default:
        return false;
    }
}


/*
 * Set a button's GPIO state. Buttons are active-low:
 * held = true  → GPIO LOW (0)
 * held = false → GPIO HIGH (1)
 */
void m5sticks3_set_button(int gpio_pin, bool held)
{
    Esp32GpioState *gpio = m5_get_gpio();
    if (gpio) {
        esp32_gpio_set_pin_level(gpio, gpio_pin, held ? 0 : 1);
    }
}
