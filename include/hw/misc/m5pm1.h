/*
 * M5PM1 PMIC I2C emulation (PY32-based, used on M5StickS3)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#ifndef M5PM1_H
#define M5PM1_H

#include "hw/i2c/i2c.h"

#define TYPE_M5PM1 "m5pm1"
#define M5PM1(obj) OBJECT_CHECK(M5PM1State, (obj), TYPE_M5PM1)

/* I2C address: 0x6E */
#define M5PM1_I2C_ADDR 0x6E

/* Register map (based on M5Unified driver code) */
#define M5PM1_REG_DEVICE_ID       0x00  /* Device ID / detection register */
#define M5PM1_REG_I2C_CONFIG      0x09  /* I2C configuration (0 = disable idle sleep) */
#define M5PM1_REG_GPIO_DIR        0x10  /* GPIO direction (bit set = output) */
#define M5PM1_REG_GPIO_OUT        0x11  /* GPIO output level (bit set = high) */
#define M5PM1_REG_GPIO_OUT_MODE   0x13  /* GPIO output mode (bit clear = push-pull) */
#define M5PM1_REG_GPIO_FUNC_SEL   0x16  /* GPIO function select (bit clear = GPIO) */

#define M5PM1_REG_COUNT           256

typedef struct M5PM1State {
    I2CSlave parent_obj;
    uint8_t regs[M5PM1_REG_COUNT];
    uint8_t reg_addr;
    bool addr_received;
} M5PM1State;

#endif /* M5PM1_H */
