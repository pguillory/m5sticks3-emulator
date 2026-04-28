/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Full GPIO register emulation: output, input, enable, per-pin config,
 * and output state observable by connected QEMU devices.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"

#define GPIO_DEBUG   0
#define GPIO_WARNING 0


/*
 * Fire output IRQs for any pins whose output value changed.
 * old_val/new_val are the 32-bit output register values.
 * base is the GPIO number offset (0 for OUT, 32 for OUT1).
 */
static void esp32_gpio_notify_outputs(Esp32GpioState *s,
                                      uint32_t old_val, uint32_t new_val,
                                      int base)
{
    uint32_t changed = old_val ^ new_val;
    while (changed) {
        int bit = __builtin_ctz(changed);
        int pin = base + bit;
        if (pin < ESP32_GPIO_MAX_COUNT) {
            qemu_set_irq(s->out_gpios[pin], (new_val >> bit) & 1);
        }
        changed &= ~(1u << bit);
    }
}


static uint64_t esp32_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint64_t r = 0;

    switch (addr) {
    case A_GPIO_OUT:
        r = s->gpio_out;
        break;
    case A_GPIO_OUT_W1TS:
    case A_GPIO_OUT_W1TC:
        r = 0;  /* Write-only registers read as 0 */
        break;
    case A_GPIO_OUT1:
        r = s->gpio_out1;
        break;
    case A_GPIO_OUT1_W1TS:
    case A_GPIO_OUT1_W1TC:
        r = 0;
        break;
    case A_GPIO_ENABLE:
        r = s->gpio_enable;
        break;
    case A_GPIO_ENABLE_W1TS:
    case A_GPIO_ENABLE_W1TC:
        r = 0;
        break;
    case A_GPIO_ENABLE1:
        r = s->gpio_enable1;
        break;
    case A_GPIO_ENABLE1_W1TS:
    case A_GPIO_ENABLE1_W1TC:
        r = 0;
        break;
    case A_GPIO_STRAP:
        r = s->strap_mode;
        break;
    case A_GPIO_IN:
        r = s->gpio_in;
        break;
    case A_GPIO_IN1:
        r = s->gpio_in1;
        break;
    case A_GPIO_STATUS:
        r = s->gpio_status;
        break;
    case A_GPIO_STATUS_W1TS:
    case A_GPIO_STATUS_W1TC:
        r = 0;
        break;
    case A_GPIO_STATUS1:
        r = s->gpio_status1;
        break;
    case A_GPIO_STATUS1_W1TS:
    case A_GPIO_STATUS1_W1TC:
        r = 0;
        break;
    case A_GPIO_PCPU_INT:
    case A_GPIO_PCPU_NMI_INT:
        r = s->gpio_status;  /* Simplified: same as status */
        break;
    case A_GPIO_PCPU_INT1:
    case A_GPIO_PCPU_NMI_INT1:
        r = s->gpio_status1;
        break;
    default:
        /* Per-pin config registers */
        if (addr >= GPIO_PIN_REG_BASE &&
            addr < GPIO_PIN_REG_BASE + ESP32_GPIO_MAX_COUNT * 4) {
            int pin = (addr - GPIO_PIN_REG_BASE) / 4;
            r = s->pin_reg[pin];
        }
        /* Function input selection */
        else if (addr >= GPIO_FUNC_IN_SEL_BASE &&
                 addr < GPIO_FUNC_IN_SEL_BASE + 256 * 4) {
            int idx = (addr - GPIO_FUNC_IN_SEL_BASE) / 4;
            r = s->func_in_sel[idx];
        }
        /* Function output selection */
        else if (addr >= GPIO_FUNC_OUT_SEL_BASE &&
                 addr < GPIO_FUNC_OUT_SEL_BASE + ESP32_GPIO_MAX_COUNT * 4) {
            int pin = (addr - GPIO_FUNC_OUT_SEL_BASE) / 4;
            r = s->func_out_sel[pin];
        }
#if GPIO_WARNING
        else {
            warn_report("[GPIO] Unsupported read from 0x%lx",
                        (unsigned long)addr);
        }
#endif
        break;
    }

#if GPIO_DEBUG
    info_report("[GPIO] Read 0x%03lx => 0x%08lx",
                (unsigned long)addr, (unsigned long)r);
#endif

    return r;
}


static void esp32_gpio_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned int size)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    uint32_t wval = (uint32_t)value;
    uint32_t old;

#if GPIO_DEBUG
    info_report("[GPIO] Write 0x%03lx <= 0x%08lx",
                (unsigned long)addr, (unsigned long)value);
#endif

    switch (addr) {
    case A_GPIO_OUT:
        old = s->gpio_out;
        s->gpio_out = wval;
        esp32_gpio_notify_outputs(s, old, wval, 0);
        break;
    case A_GPIO_OUT_W1TS:
        old = s->gpio_out;
        s->gpio_out |= wval;
        esp32_gpio_notify_outputs(s, old, s->gpio_out, 0);
        break;
    case A_GPIO_OUT_W1TC:
        old = s->gpio_out;
        s->gpio_out &= ~wval;
        esp32_gpio_notify_outputs(s, old, s->gpio_out, 0);
        break;

    case A_GPIO_OUT1:
        old = s->gpio_out1;
        s->gpio_out1 = wval;
        esp32_gpio_notify_outputs(s, old, wval, 32);
        break;
    case A_GPIO_OUT1_W1TS:
        old = s->gpio_out1;
        s->gpio_out1 |= wval;
        esp32_gpio_notify_outputs(s, old, s->gpio_out1, 32);
        break;
    case A_GPIO_OUT1_W1TC:
        old = s->gpio_out1;
        s->gpio_out1 &= ~wval;
        esp32_gpio_notify_outputs(s, old, s->gpio_out1, 32);
        break;

    case A_GPIO_ENABLE:
        s->gpio_enable = wval;
        break;
    case A_GPIO_ENABLE_W1TS:
        s->gpio_enable |= wval;
        break;
    case A_GPIO_ENABLE_W1TC:
        s->gpio_enable &= ~wval;
        break;

    case A_GPIO_ENABLE1:
        s->gpio_enable1 = wval;
        break;
    case A_GPIO_ENABLE1_W1TS:
        s->gpio_enable1 |= wval;
        break;
    case A_GPIO_ENABLE1_W1TC:
        s->gpio_enable1 &= ~wval;
        break;

    case A_GPIO_STATUS:
        s->gpio_status = wval;
        break;
    case A_GPIO_STATUS_W1TS:
        s->gpio_status |= wval;
        break;
    case A_GPIO_STATUS_W1TC:
        s->gpio_status &= ~wval;
        break;

    case A_GPIO_STATUS1:
        s->gpio_status1 = wval;
        break;
    case A_GPIO_STATUS1_W1TS:
        s->gpio_status1 |= wval;
        break;
    case A_GPIO_STATUS1_W1TC:
        s->gpio_status1 &= ~wval;
        break;

    default:
        /* Per-pin config registers */
        if (addr >= GPIO_PIN_REG_BASE &&
            addr < GPIO_PIN_REG_BASE + ESP32_GPIO_MAX_COUNT * 4) {
            int pin = (addr - GPIO_PIN_REG_BASE) / 4;
            s->pin_reg[pin] = wval;
        }
        /* Function input selection */
        else if (addr >= GPIO_FUNC_IN_SEL_BASE &&
                 addr < GPIO_FUNC_IN_SEL_BASE + 256 * 4) {
            int idx = (addr - GPIO_FUNC_IN_SEL_BASE) / 4;
            s->func_in_sel[idx] = wval;
        }
        /* Function output selection */
        else if (addr >= GPIO_FUNC_OUT_SEL_BASE &&
                 addr < GPIO_FUNC_OUT_SEL_BASE + ESP32_GPIO_MAX_COUNT * 4) {
            int pin = (addr - GPIO_FUNC_OUT_SEL_BASE) / 4;
            s->func_out_sel[pin] = wval;
        }
#if GPIO_WARNING
        else {
            warn_report("[GPIO] Unsupported write to 0x%lx (0x%08lx)",
                        (unsigned long)addr, (unsigned long)value);
        }
#endif
        break;
    }
}


static const MemoryRegionOps esp32_gpio_ops = {
    .read  = esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void esp32_gpio_reset_hold(Object *obj, ResetType type)
{
    Esp32GpioState *s = ESP32_GPIO(obj);

    s->gpio_out = 0;
    s->gpio_out1 = 0;
    s->gpio_enable = 0;
    s->gpio_enable1 = 0;
    s->gpio_in = 0;
    s->gpio_in1 = 0;
    s->gpio_status = 0;
    s->gpio_status1 = 0;

    memset(s->pin_reg, 0, sizeof(s->pin_reg));
    memset(s->func_in_sel, 0, sizeof(s->func_in_sel));
    memset(s->func_out_sel, 0, sizeof(s->func_out_sel));
}


static void esp32_gpio_realize(DeviceState *dev, Error **errp)
{
}


static void esp32_gpio_init(Object *obj)
{
    Esp32GpioState *s = ESP32_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Set the default value for the strap_mode property */
    object_property_set_int(obj, "strap_mode", ESP32_STRAP_MODE_FLASH_BOOT,
                            &error_fatal);

    memory_region_init_io(&s->iomem, obj, &esp32_gpio_ops, s,
                          TYPE_ESP32_GPIO, ESP32_GPIO_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    /* Initialize output GPIOs so other devices can observe pin state */
    qdev_init_gpio_out_named(DEVICE(s), s->out_gpios, "gpio-out",
                             ESP32_GPIO_MAX_COUNT);
}


static Property esp32_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};


static void esp32_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32_gpio_reset_hold;
    dc->realize = esp32_gpio_realize;
    device_class_set_props(dc, esp32_gpio_properties);
}


static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void)
{
    type_register_static(&esp32_gpio_info);
}

type_init(esp32_gpio_register_types)
