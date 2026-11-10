/*
 * QTest for ESP32-S3 GPIO controller
 *
 * Tests the GPIO register model by writing direction, output, and interrupt
 * configuration registers, then reading back input and status registers.
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest-single.h"

/* ESP32-S3 GPIO base address */
#define GPIO_BASE           0x60004000

/* Register offsets (from esp32_gpio.h) */
#define GPIO_OUT            0x0004
#define GPIO_OUT_W1TS       0x0008
#define GPIO_OUT_W1TC       0x000C
#define GPIO_OUT1           0x0010
#define GPIO_OUT1_W1TS      0x0014
#define GPIO_OUT1_W1TC      0x0018
#define GPIO_ENABLE         0x0020
#define GPIO_ENABLE_W1TS    0x0024
#define GPIO_ENABLE_W1TC    0x0028
#define GPIO_ENABLE1        0x002C
#define GPIO_ENABLE1_W1TS   0x0030
#define GPIO_ENABLE1_W1TC   0x0034
#define GPIO_STRAP          0x0038
#define GPIO_IN_REG         0x003C
#define GPIO_IN1            0x0040
#define GPIO_STATUS         0x0044
#define GPIO_STATUS_W1TS    0x0048
#define GPIO_STATUS_W1TC    0x004C
#define GPIO_STATUS1        0x0050
#define GPIO_PIN_REG_BASE   0x0074

/* Per-pin config bits */
#define GPIO_PIN_INT_TYPE_SHIFT  7
#define GPIO_PIN_INT_ENA_SHIFT   13

#define INT_TYPE_HIGH_LEVEL      5


static uint32_t gpio_readl(QTestState *s, uint32_t offset)
{
    return qtest_readl(s, GPIO_BASE + offset);
}

static void gpio_writel(QTestState *s, uint32_t offset, uint32_t value)
{
    qtest_writel(s, GPIO_BASE + offset, value);
}


/*
 * Test: output-enabled pins reflect their output value through GPIO_IN.
 *
 * 1. Enable pins 0-7 as outputs
 * 2. Write a pattern to GPIO_OUT
 * 3. Read GPIO_IN and verify it matches the output for enabled pins
 */
static void test_gpio_output_readback(const void *data)
{
    QTestState *s = (QTestState *)data;

    /* Enable pins 0-7 as outputs */
    gpio_writel(s, GPIO_ENABLE, 0x000000FF);

    /* Write pattern to output register */
    gpio_writel(s, GPIO_OUT, 0x000000A5);

    /* GPIO_IN for output-enabled pins should reflect the output value.
     * Non-output pins (8-31) reflect their input state (default: all HIGH). */
    uint32_t in_val = gpio_readl(s, GPIO_IN_REG);

    /* Pins 0-7: reflect output (0xA5).
     * Pins 8-31: input, default HIGH (0xFFFFFF).
     * Combined: 0xFFFFFFA5 */
    g_assert_cmphex(in_val, ==, 0xFFFFFFA5);
}


/*
 * Test: W1TS (Write-1-to-Set) and W1TC (Write-1-to-Clear) semantics.
 *
 * These registers modify individual bits in GPIO_OUT without a
 * read-modify-write cycle.
 */
static void test_gpio_w1ts_w1tc(const void *data)
{
    QTestState *s = (QTestState *)data;

    /* Start with output cleared */
    gpio_writel(s, GPIO_OUT, 0x00000000);
    g_assert_cmphex(gpio_readl(s, GPIO_OUT), ==, 0x00000000);

    /* Set bits 0, 2, 4, 6 using W1TS */
    gpio_writel(s, GPIO_OUT_W1TS, 0x00000055);
    g_assert_cmphex(gpio_readl(s, GPIO_OUT), ==, 0x00000055);

    /* Set bits 1, 3, 5, 7 — previous bits should remain set */
    gpio_writel(s, GPIO_OUT_W1TS, 0x000000AA);
    g_assert_cmphex(gpio_readl(s, GPIO_OUT), ==, 0x000000FF);

    /* Clear bits 0-3 using W1TC */
    gpio_writel(s, GPIO_OUT_W1TC, 0x0000000F);
    g_assert_cmphex(gpio_readl(s, GPIO_OUT), ==, 0x000000F0);

    /* W1TS/W1TC registers should read as 0 (write-only) */
    g_assert_cmphex(gpio_readl(s, GPIO_OUT_W1TS), ==, 0);
    g_assert_cmphex(gpio_readl(s, GPIO_OUT_W1TC), ==, 0);
}


/*
 * Test: direction register controls GPIO_IN behavior.
 *
 * When a pin's output is disabled (input mode), GPIO_IN reflects the
 * external input level (default: HIGH due to simulated pull-ups).
 * When output is enabled, GPIO_IN reflects the output register value.
 */
static void test_gpio_direction(const void *data)
{
    QTestState *s = (QTestState *)data;

    /* All pins input mode (default after reset).
     * GPIO_IN should reflect external inputs (all HIGH by default). */
    gpio_writel(s, GPIO_ENABLE, 0x00000000);
    gpio_writel(s, GPIO_OUT, 0x00000000);
    uint32_t in_val = gpio_readl(s, GPIO_IN_REG);
    g_assert_cmphex(in_val, ==, 0xFFFFFFFF);

    /* Enable pin 0 as output with output=0.
     * Pin 0 in GPIO_IN should now read 0, others still HIGH. */
    gpio_writel(s, GPIO_ENABLE_W1TS, 0x00000001);
    in_val = gpio_readl(s, GPIO_IN_REG);
    g_assert_cmphex(in_val, ==, 0xFFFFFFFE);

    /* Switch pin 0 back to input mode via W1TC.
     * Pin 0 should reflect external input (HIGH) again. */
    gpio_writel(s, GPIO_ENABLE_W1TC, 0x00000001);
    in_val = gpio_readl(s, GPIO_IN_REG);
    g_assert_cmphex(in_val, ==, 0xFFFFFFFF);

    /* Verify enable register itself is correct */
    g_assert_cmphex(gpio_readl(s, GPIO_ENABLE), ==, 0x00000000);
}


/*
 * Test: interrupt status register set/clear.
 *
 * Configure a pin for high-level interrupt, verify the status bit is set
 * when the pin is already HIGH, then clear it with W1TC.
 */
static void test_gpio_interrupt_status(const void *data)
{
    QTestState *s = (QTestState *)data;

    /* Start clean */
    gpio_writel(s, GPIO_STATUS, 0x00000000);
    g_assert_cmphex(gpio_readl(s, GPIO_STATUS), ==, 0x00000000);

    /* Pin 5 is input (default), level is HIGH (default pull-up).
     * Configure pin 5 for high-level interrupt with INT_ENA=1. */
    uint32_t pin_cfg = (INT_TYPE_HIGH_LEVEL << GPIO_PIN_INT_TYPE_SHIFT) |
                       (1 << GPIO_PIN_INT_ENA_SHIFT);
    gpio_writel(s, GPIO_PIN_REG_BASE + 5 * 4, pin_cfg);

    /* The model re-evaluates level-triggered interrupts on config write.
     * Since pin 5 is HIGH and we set high-level trigger, status should be set. */
    uint32_t status = gpio_readl(s, GPIO_STATUS);
    g_assert_cmphex(status & (1u << 5), ==, (1u << 5));

    /* Clear the status bit with W1TC */
    gpio_writel(s, GPIO_STATUS_W1TC, (1u << 5));
    status = gpio_readl(s, GPIO_STATUS);
    g_assert_cmphex(status & (1u << 5), ==, 0);
}


/*
 * Test: high GPIO bank (GPIO 32-48) output and readback.
 *
 * Same semantics as the low bank but using GPIO_OUT1/GPIO_IN1 registers.
 */
static void test_gpio_high_bank(const void *data)
{
    QTestState *s = (QTestState *)data;

    /* Enable GPIO 32 and 33 as outputs (bits 0 and 1 in the high bank) */
    gpio_writel(s, GPIO_ENABLE1, 0x00000003);

    /* Set GPIO 32 output HIGH, GPIO 33 output LOW */
    gpio_writel(s, GPIO_OUT1, 0x00000001);

    /* Read GPIO_IN1:
     * Bit 0 (GPIO 32): output enabled, output=1 → reads 1
     * Bit 1 (GPIO 33): output enabled, output=0 → reads 0
     * Bits 2-16 (GPIO 34-48): input, default HIGH → all 1
     *
     * Expected: 0x0001FFFD
     *  bits 2-16 = 0x1FFFC (all high)
     *  bit 0 = 1 (output high)
     *  bit 1 = 0 (output low)
     *  → 0x1FFFC | 0x01 = 0x1FFFD */
    uint32_t in1 = gpio_readl(s, GPIO_IN1);
    g_assert_cmphex(in1, ==, 0x0001FFFD);

    /* Test W1TS/W1TC on high bank */
    gpio_writel(s, GPIO_OUT1_W1TS, 0x00000002);  /* Set bit 1 (GPIO 33) */
    g_assert_cmphex(gpio_readl(s, GPIO_OUT1), ==, 0x00000003);

    gpio_writel(s, GPIO_OUT1_W1TC, 0x00000001);  /* Clear bit 0 (GPIO 32) */
    g_assert_cmphex(gpio_readl(s, GPIO_OUT1), ==, 0x00000002);
}


int main(int argc, char **argv)
{
    QTestState *s;
    int r;

    g_test_init(&argc, &argv, NULL);

    s = qtest_init("-machine esp32s3 -display none");

    qtest_add_data_func("/esp32s3/gpio/output_readback", s,
                        test_gpio_output_readback);
    qtest_add_data_func("/esp32s3/gpio/w1ts_w1tc", s,
                        test_gpio_w1ts_w1tc);
    qtest_add_data_func("/esp32s3/gpio/direction", s,
                        test_gpio_direction);
    qtest_add_data_func("/esp32s3/gpio/interrupt_status", s,
                        test_gpio_interrupt_status);
    qtest_add_data_func("/esp32s3/gpio/high_bank", s,
                        test_gpio_high_bank);

    r = g_test_run();
    qtest_quit(s);

    return r;
}
