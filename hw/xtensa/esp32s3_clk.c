/*
 * ESP32-S3 Clocks definition
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/xtensa/esp32s3_clk.h"
#include "hw/xtensa/esp32s3_clk_defs.h"

#define CLOCK_DEBUG      0
#define CLOCK_WARNING    0

static uint32_t esp32s3_read_cpu_intr(ESP32S3ClockState *s, uint32_t index)
{
    return (s->levels >> index) & 1;
}


static void esp32s3_write_cpu_intr(ESP32S3ClockState *s, uint32_t index, uint32_t value)
{
    const uint32_t field = FIELD_EX32(value, SYSTEM_CPU_INTR_FROM_CPU_0, CPU_INTR_FROM_CPU_0);
    if (field) {
        s->levels |= BIT(index);
        qemu_set_irq(s->irqs[index], 1);
    } else {
        s->levels &= ~BIT(index);
        qemu_set_irq(s->irqs[index], 0);
    }
}

static uint32_t esp32s3_clock_get_ext_dev_enc_dec_ctrl(ESP32S3ClockState *s)
{
    return s->sys_ext_dev_enc_dec_ctrl;
}

static uint64_t esp32s3_clock_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(opaque);
    uint64_t r = 0;

    switch(addr) {
        case A_SYSTEM_CORE_1_CONTROL_0_REG:
            r = s->core1_ctrl0;
            break;
        case A_SYSTEM_CORE_1_CONTROL_1_REG:
                r = s->app_cpu_addr;
            break;
        case A_SYSTEM_CPU_PER_CONF:
            r = s->cpuperconf;
#if CLOCK_DEBUG
            {
                static int cpc_rd = 0;
                if (++cpc_rd <= 50) {
                    warn_report("[CLOCK] CPU_PER_CONF read #%d => 0x%08x (CPUPERIOD_SEL=%d PLL_FREQ_SEL=%d)",
                                cpc_rd, (uint32_t)r,
                                FIELD_EX32(r, SYSTEM_CPU_PER_CONF, CPUPERIOD_SEL),
                                FIELD_EX32(r, SYSTEM_CPU_PER_CONF, PLL_FREQ_SEL));
                }
            }
#endif
            break;
        case A_SYSTEM_SYSCLK_CONF:
            r = s->sysclk;
            /* Ensure CLK_XTAL_FREQ always reports the true crystal frequency
             * (40 MHz). The ROM may overwrite this field with 0 if its crystal
             * frequency detection fails in emulation. */
            r = FIELD_DP32(r, SYSTEM_SYSCLK_CONF, CLK_XTAL_FREQ, 40);
#if CLOCK_DEBUG
            {
                static int sc_rd = 0;
                if (++sc_rd <= 50) {
                    warn_report("[CLOCK] SYSCLK_CONF read #%d => 0x%08x (SOC_CLK_SEL=%d PRE_DIV_CNT=%d)",
                                sc_rd, (uint32_t)r,
                                FIELD_EX32(r, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL),
                                FIELD_EX32(r, SYSTEM_SYSCLK_CONF, PRE_DIV_CNT));
                }
            }
#endif
            break;
        case A_SYSTEM_CPU_INTR_FROM_CPU_0:
        case A_SYSTEM_CPU_INTR_FROM_CPU_1:
        case A_SYSTEM_CPU_INTR_FROM_CPU_2:
        case A_SYSTEM_CPU_INTR_FROM_CPU_3:
            r = esp32s3_read_cpu_intr(s, (addr - A_SYSTEM_CPU_INTR_FROM_CPU_0) / sizeof(uint32_t));
            break;
        case A_SYSTEM_EXTERNAL_DEVICE_ENCRYPT_DECRYPT_CONTROL:
            r = s->sys_ext_dev_enc_dec_ctrl;
            break;
        default:
        {
            uint32_t index = addr / sizeof(uint32_t);
            if (index < ESP32S3_CLOCK_REG_COUNT) {
                r = s->regs[index];
            }
#if CLOCK_WARNING
            warn_report("[CLOCK] Unsupported read from %08lx\n", addr);
#endif
            break;
        }
    }
    return r;
}

static void esp32s3_clock_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(opaque);

    switch(addr) {
        case A_SYSTEM_CORE_1_CONTROL_0_REG:
            s->core1_ctrl0 = (uint32_t)value;
            /* Bit 0: CLKGATE_EN, Bit 1: RUNSTALL, Bit 2: RESETING
             * When CLKGATE_EN=1 and RUNSTALL=0, CPU 1 is released */
            {
                bool stall = !(value & 0x1) || (value & 0x2);
                qemu_set_irq(s->cpu1_stall, stall ? 1 : 0);
            }
            break;
        case A_SYSTEM_CORE_1_CONTROL_1_REG:
                s->app_cpu_addr = (uint32_t)value;
                /* On real ESP32-S3, writing a non-zero entry point triggers
                 * CPU1 startup: the hardware automatically enables the clock
                 * gate and clears RUNSTALL so CPU1 begins executing from its
                 * reset-vector polling loop (ROM code reads the entry point
                 * from this register and jumps to it).
                 * The firmware never explicitly clears RUNSTALL after setting
                 * the entry point — it relies on this hardware behavior. */
                if (value != 0) {
                    s->core1_ctrl0 = (s->core1_ctrl0 | 0x1) & ~0x2u;
                    qemu_set_irq(s->cpu1_stall, 0);
                }
            break;
        case A_SYSTEM_CPU_INTR_FROM_CPU_0:
        case A_SYSTEM_CPU_INTR_FROM_CPU_1:
        case A_SYSTEM_CPU_INTR_FROM_CPU_2:
        case A_SYSTEM_CPU_INTR_FROM_CPU_3:
            esp32s3_write_cpu_intr(s, (addr - A_SYSTEM_CPU_INTR_FROM_CPU_0) / sizeof(uint32_t), value);
            break;
        case A_SYSTEM_CPU_PER_CONF:
#if CLOCK_DEBUG
            {
                static int cpc_wr = 0;
                if (++cpc_wr <= 50) {
                    warn_report("[CLOCK] CPU_PER_CONF write #%d: 0x%08x => 0x%08x (CPUPERIOD_SEL=%d PLL_FREQ_SEL=%d)",
                                cpc_wr, (uint32_t)s->cpuperconf, (uint32_t)value,
                                FIELD_EX32(value, SYSTEM_CPU_PER_CONF, CPUPERIOD_SEL),
                                FIELD_EX32(value, SYSTEM_CPU_PER_CONF, PLL_FREQ_SEL));
                }
            }
#endif
            s->cpuperconf = (uint32_t)value;
            qemu_set_irq(s->clk_update, 1);
            break;
        case A_SYSTEM_SYSCLK_CONF:
#if CLOCK_DEBUG
            {
                static int sc_wr = 0;
                if (++sc_wr <= 50) {
                    warn_report("[CLOCK] SYSCLK_CONF write #%d: 0x%08x => 0x%08x (SOC_CLK_SEL=%d PRE_DIV_CNT=%d)",
                                sc_wr, (uint32_t)s->sysclk, (uint32_t)value,
                                FIELD_EX32(value, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL),
                                FIELD_EX32(value, SYSTEM_SYSCLK_CONF, PRE_DIV_CNT));
                }
            }
#endif
            s->sysclk = (uint32_t)value;
            qemu_set_irq(s->clk_update, 1);
            break;
        case A_SYSTEM_EXTERNAL_DEVICE_ENCRYPT_DECRYPT_CONTROL:
            s->sys_ext_dev_enc_dec_ctrl = value;
            break;
        default:
        {
            uint32_t index = addr / sizeof(uint32_t);
            if (index < ESP32S3_CLOCK_REG_COUNT) {
                s->regs[index] = (uint32_t)value;
            }
#if CLOCK_WARNING
            warn_report("[CLOCK] Unsupported write to %08lx (%08lx)\n", addr, value);
#endif
            break;
        }
    }
}

static const MemoryRegionOps esp32s3_clock_ops = {
    .read =  esp32s3_clock_read,
    .write = esp32s3_clock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_clock_reset_hold(Object *obj, ResetType type)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(obj);
    /* On board reset, set the proper clocks and dividers */
    s->sysclk = ( 1 << R_SYSTEM_SYSCLK_CONF_PRE_DIV_CNT_SHIFT) |
                (ESP32S3_CLK_SEL_PLL << R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_SHIFT) |
                (40 << R_SYSTEM_SYSCLK_CONF_CLK_XTAL_FREQ_SHIFT) |
                ( 1 << R_SYSTEM_SYSCLK_CONF_CLK_DIV_EN_SHIFT);

    /* Divider for PLL clock and APB  frequency */
    s->cpuperconf = (ESP32S3_PERIOD_SEL_80 << R_SYSTEM_CPU_PER_CONF_CPUPERIOD_SEL_SHIFT) |
                    (ESP32S3_FREQ_SEL_PLL_480 << R_SYSTEM_CPU_PER_CONF_PLL_FREQ_SEL_SHIFT);

    /* Initialize the IRQs */
    s->levels = 0;
    for (int i = 0 ; i < ESP32S3_SYSTEM_CPU_INTR_COUNT; i++) {
        qemu_irq_lower(s->irqs[i]);
    }
}

static void esp32s3_clock_realize(DeviceState *dev, Error **errp)
{
    /* Initialize the registers */
    esp32s3_clock_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
}

static void esp32s3_clock_init(Object *obj)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_clock_ops, s,
                          TYPE_ESP32S3_CLOCK, A_SYSTEM_COMB_PVT_ERR_HVT_SITE3 + sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->iomem);

    /* Initialize the output IRQ lines used to manually trigger interrupts */
    for (uint64_t i = 0; i < ESP32S3_SYSTEM_CPU_INTR_COUNT; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    /* GPIO output for CPU 1 stall control */
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu1_stall,
                             ESP32S3_CLOCK_CPU1_STALL_GPIO, 1);

    /* GPIO output to signal clock frequency changes */
    qdev_init_gpio_out_named(DEVICE(sbd), &s->clk_update,
                             ESP32S3_CLOCK_UPDATE_GPIO, 1);
}

static void esp32s3_clock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ESP32S3ClockClass* esp32s3_clock = ESP32S3_CLOCK_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_clock_reset_hold;
    dc->realize = esp32s3_clock_realize;

    esp32s3_clock->get_ext_dev_enc_dec_ctrl = esp32s3_clock_get_ext_dev_enc_dec_ctrl;
}

static const TypeInfo esp32s3_cache_info = {
    .name = TYPE_ESP32S3_CLOCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3ClockState),
    .instance_init = esp32s3_clock_init,
    .class_init = esp32s3_clock_class_init,
    .class_size = sizeof(ESP32S3ClockClass)
};

static void esp32s3_cache_register_types(void)
{
    type_register_static(&esp32s3_cache_info);
}

type_init(esp32s3_cache_register_types)
