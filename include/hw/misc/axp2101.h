/*
 * AXP2101 PMIC I2C emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * The AXP2101 is a single-cell Li-battery power management IC by X-Powers.
 * On the M5StickS3 it manages power rails (DCDCs, LDOs), battery charging,
 * and backlight control. Configuration is done via I2C at address 0x34.
 *
 * This model emulates the I2C register interface so the firmware's power
 * management driver initializes without errors and can read battery status.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/i2c/i2c.h"

#define TYPE_AXP2101 "axp2101"
#define AXP2101(obj) OBJECT_CHECK(AXP2101State, (obj), TYPE_AXP2101)

/* AXP2101 I2C address (7-bit): 0x34 */
#define AXP2101_I2C_ADDR       0x34

/* Register file size — covers all documented registers up to 0xA4+ */
#define AXP2101_REG_COUNT      0xB0


typedef struct AXP2101State {
    I2CSlave parent_obj;

    /* I2C transaction state */
    uint8_t reg_addr;        /* Current register pointer */
    bool addr_received;      /* Whether we've received the register address byte */

    /* Register file */
    uint8_t regs[AXP2101_REG_COUNT];

} AXP2101State;


/*
 * AXP2101 register addresses
 *
 * Reference: X-Powers AXP2101 Datasheet V1.0/V1.4
 */

/* Status registers */
#define AXP2101_PMU_STATUS_1           0x00
#define AXP2101_PMU_STATUS_2           0x01
#define AXP2101_CHIP_ID                0x03

/* Data buffer (general-purpose user storage) */
#define AXP2101_DATA_BUFFER_0          0x04
#define AXP2101_DATA_BUFFER_1          0x05
#define AXP2101_DATA_BUFFER_2          0x06
#define AXP2101_DATA_BUFFER_3          0x07

/* PMU common configuration */
#define AXP2101_COMMON_CFG             0x10
#define AXP2101_BATFET_CTRL            0x12
#define AXP2101_TEMP_CTRL              0x13
#define AXP2101_MIN_SYS_VOL_CTRL      0x14
#define AXP2101_INPUT_VOLT_LIMIT       0x15
#define AXP2101_INPUT_CURR_LIMIT       0x16
#define AXP2101_RESET_FUEL_GAUGE       0x17
#define AXP2101_CHARGE_GAUGE_WDT       0x18
#define AXP2101_WATCHDOG_CTRL          0x19
#define AXP2101_LOW_BATT_WARNING       0x1A

/* Power-on/off status */
#define AXP2101_PWRON_STATUS           0x20
#define AXP2101_PWROFF_STATUS          0x21
#define AXP2101_PWROFF_EN              0x22
#define AXP2101_DCDC_PWROFF_CTRL       0x23
#define AXP2101_PWROFF_VSYS_THRESH     0x24
#define AXP2101_PWROK_DELAY_SEQ        0x25
#define AXP2101_SLEEP_WAKEUP_CTRL      0x26
#define AXP2101_IRQ_PWRON_TIMING       0x27

/* Fast power-on config */
#define AXP2101_FAST_PWRON_0           0x28
#define AXP2101_FAST_PWRON_1           0x29
#define AXP2101_FAST_PWRON_2           0x2A
#define AXP2101_FAST_PWRON_3           0x2B

/* ADC control and results */
#define AXP2101_ADC_ENABLE             0x30
#define AXP2101_VBAT_H                 0x34
#define AXP2101_VBAT_L                 0x35
#define AXP2101_TS_H                   0x36
#define AXP2101_TS_L                   0x37
#define AXP2101_VBUS_H                 0x38
#define AXP2101_VBUS_L                 0x39
#define AXP2101_VSYS_H                 0x3A
#define AXP2101_VSYS_L                 0x3B
#define AXP2101_TDIE_H                 0x3C
#define AXP2101_TDIE_L                 0x3D

/* IRQ enable and status */
#define AXP2101_IRQ_ENABLE_0           0x40
#define AXP2101_IRQ_ENABLE_1           0x41
#define AXP2101_IRQ_ENABLE_2           0x42
#define AXP2101_IRQ_STATUS_0           0x48
#define AXP2101_IRQ_STATUS_1           0x49
#define AXP2101_IRQ_STATUS_2           0x4A

/* Temperature sensor */
#define AXP2101_TS_CTRL                0x50

/* Charging configuration */
#define AXP2101_PRECHARGE_SETTING      0x61
#define AXP2101_FASTCHARGE_SETTING     0x62
#define AXP2101_TERM_CURR_SETTING      0x63
#define AXP2101_CHG_VOLTAGE_SETTING    0x64
#define AXP2101_THERMAL_REGULATION     0x65
#define AXP2101_CHG_TIMEOUT_SETTING    0x67
#define AXP2101_BATT_DETECT_CTRL       0x68
#define AXP2101_CHGLED_CTRL            0x69

/* DCDC control */
#define AXP2101_DCDC_EN_CTRL           0x80
#define AXP2101_DCDC_PWM_CTRL          0x81
#define AXP2101_DCDC1_VOLTAGE          0x82
#define AXP2101_DCDC2_VOLTAGE          0x83
#define AXP2101_DCDC3_VOLTAGE          0x84
#define AXP2101_DCDC4_VOLTAGE          0x85
#define AXP2101_DCDC5_VOLTAGE          0x86

/* LDO control */
#define AXP2101_LDO_EN_CTRL_0         0x90
#define AXP2101_LDO_EN_CTRL_1         0x91
#define AXP2101_ALDO1_VOLTAGE          0x92
#define AXP2101_ALDO2_VOLTAGE          0x93
#define AXP2101_ALDO3_VOLTAGE          0x94
#define AXP2101_ALDO4_VOLTAGE          0x95
#define AXP2101_BLDO1_VOLTAGE          0x96
#define AXP2101_BLDO2_VOLTAGE          0x97
#define AXP2101_CPUSLDO_VOLTAGE        0x98
#define AXP2101_DLDO1_VOLTAGE          0x99
#define AXP2101_DLDO2_VOLTAGE          0x9A

/* Fuel gauge */
#define AXP2101_BATTERY_PARAM          0xA1
#define AXP2101_FUEL_GAUGE_CTRL        0xA2
#define AXP2101_BATTERY_PERCENTAGE     0xA4
