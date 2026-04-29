/*
 * ESP32-S3 Clocks definition
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"

#define TYPE_ESP32S3_CLOCK "esp32s3.soc.clk"
#define ESP32S3_CLOCK(obj) OBJECT_CHECK(ESP32S3ClockState, (obj), TYPE_ESP32S3_CLOCK)
#define ESP32S3_CLOCK_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3ClockClass, obj, TYPE_ESP32S3_CLOCK)
#define ESP32S3_CLOCK_CLASS(klass) OBJECT_CLASS_CHECK(ESP32S3ClockClass, klass, TYPE_ESP32S3_CLOCK)


#define ESP32S3_SYSTEM_CPU_INTR_COUNT   4
#define ESP32S3_CLOCK_CPU1_STALL_GPIO  "cpu1-stall"
#define ESP32S3_CLOCK_UPDATE_GPIO      "sysclk-update"

/**
 * Value for SYSTEM_SOC_CLK_SEL
 */
#define ESP32S3_CLK_SEL_XTAL    0
#define ESP32S3_CLK_SEL_PLL     1
#define ESP32S3_CLK_SEL_RCFAST  2

/**
 * Values for SYSTEM_PLL_FREQ_SEL
 * Per ESP32-S3 TRM and ESP-IDF: 0 = 320 MHz, 1 = 480 MHz
 */
#define ESP32S3_FREQ_SEL_PLL_320    0
#define ESP32S3_FREQ_SEL_PLL_480    1

/**
 * Values for SYSTEM_CPUPERIOD_SEL
*/
#define ESP32S3_PERIOD_SEL_80       0
#define ESP32S3_PERIOD_SEL_160      1


typedef struct ESP32S3ClockState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    /* Registers for clocks configuration and frequency dividers */
    uint32_t cpuperconf;
    uint32_t sysclk;


    /* IRQs for crosscore interrupts */
    qemu_irq irqs[ESP32S3_SYSTEM_CPU_INTR_COUNT];

    /* Bitmap that keeps the level of the IRQs */
    uint32_t levels;
    
    uint32_t app_cpu_addr;
    uint32_t core1_ctrl0;

    /* GPIO output to control CPU 1 stall state */
    qemu_irq cpu1_stall;

    /* GPIO output to signal CPU clock frequency change */
    qemu_irq clk_update;

    uint32_t sys_ext_dev_enc_dec_ctrl;

    /* Generic register storage for peripheral clock/reset enables etc. */
#define ESP32S3_CLOCK_REG_COUNT (0xA4 / sizeof(uint32_t))
    uint32_t regs[ESP32S3_CLOCK_REG_COUNT];
} ESP32S3ClockState;

typedef struct ESP32S3ClockClass {
    SysBusDeviceClass parent_class;
    /* Virtual methods */
    uint32_t (*get_ext_dev_enc_dec_ctrl)(ESP32S3ClockState *s);
} ESP32S3ClockClass;

