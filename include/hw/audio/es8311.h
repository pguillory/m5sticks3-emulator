/*
 * ES8311 audio codec I2C emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * The ES8311 is a low-power mono audio codec by Everest Semiconductor.
 * On the M5StickS3 it handles DAC (speaker output via AW8737 amp) and
 * ADC (microphone input). Configuration is done via I2C; audio data
 * flows through the I2S bus.
 *
 * This model emulates the I2C register interface so ESP-IDF's ES8311
 * driver initializes without errors. Actual audio processing happens
 * in the I2S peripheral model.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/i2c/i2c.h"

#define TYPE_ES8311 "es8311"
#define ES8311(obj) OBJECT_CHECK(ES8311State, (obj), TYPE_ES8311)

/* ES8311 I2C address (7-bit): 0x18 or 0x19 depending on AD0 pin */
#define ES8311_I2C_ADDR     0x18

/* Number of ES8311 registers */
#define ES8311_REG_COUNT    0x50


typedef struct ES8311State {
    I2CSlave parent_obj;

    /* I2C transaction state */
    uint8_t reg_addr;        /* Current register pointer */
    bool addr_received;      /* Whether we've received the register address byte */

    /* Register file */
    uint8_t regs[ES8311_REG_COUNT];

} ES8311State;


/* Key ES8311 register addresses */
#define ES8311_REG00_RESET          0x00
#define ES8311_REG01_CLK_MANAGER1   0x01
#define ES8311_REG02_CLK_MANAGER2   0x02
#define ES8311_REG03_CLK_MANAGER3   0x03
#define ES8311_REG04_CLK_MANAGER4   0x04
#define ES8311_REG05_CLK_MANAGER5   0x05
#define ES8311_REG06_CLK_MANAGER6   0x06
#define ES8311_REG07_CLK_MANAGER7   0x07
#define ES8311_REG08_CLK_MANAGER8   0x08
#define ES8311_REG09_SDPIN          0x09
#define ES8311_REG0A_SDPOUT         0x0A
#define ES8311_REG0B_SYSTEM         0x0B
#define ES8311_REG0C_SYSTEM         0x0C
#define ES8311_REG0D_SYSTEM         0x0D
#define ES8311_REG0E_SYSTEM         0x0E
#define ES8311_REG0F_ADC            0x0F
#define ES8311_REG10_ADCVOL         0x10
#define ES8311_REG11_ADC            0x11
#define ES8311_REG12_ADC            0x12
#define ES8311_REG13_ADC_EQ         0x13
#define ES8311_REG14_ADC            0x14
#define ES8311_REG15_DAC            0x15
#define ES8311_REG16_DAC            0x16
#define ES8311_REG17_DACVOL         0x17
#define ES8311_REG18_DAC            0x18
#define ES8311_REG19_DAC            0x19
#define ES8311_REG1A_DAC            0x1A
#define ES8311_REG1B_DAC            0x1B
#define ES8311_REG1C_DAC            0x1C
#define ES8311_REG32_GPIO           0x32
#define ES8311_REG33_GPIO           0x33
#define ES8311_REG44_FLAG           0x44
#define ES8311_REGFA_ID1            0xFA
#define ES8311_REGFB_ID2            0xFB
#define ES8311_REGFC_ID3            0xFC
#define ES8311_REGFD_CHIPVER        0xFD
