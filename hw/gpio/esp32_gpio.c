/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Full GPIO register emulation: output, input, enable, per-pin config,
 * interrupt generation, and injectable input pin state.
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


/*
 * Re-evaluate the aggregate GPIO interrupt line.
 * Raises the IRQ if any pin has both a pending status bit and its
 * interrupt enable bits set; lowers it otherwise.
 */
static void esp32_gpio_update_irq(Esp32GpioState *s)
{
    bool pending = false;

    for (int pin = 0; pin < ESP32_GPIO_MAX_COUNT; pin++) {
        uint32_t cfg = s->pin_reg[pin];
        int int_ena = (cfg & GPIO_PIN_INT_ENA_MASK) >> GPIO_PIN_INT_ENA_SHIFT;
        if (int_ena == 0) {
            continue;
        }

        uint32_t status_bit;
        if (pin < 32) {
            status_bit = s->gpio_status & (1u << pin);
        } else {
            status_bit = s->gpio_status1 & (1u << (pin - 32));
        }

        if (status_bit) {
            pending = true;
            break;
        }
    }

    if (pending) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}


/*
 * Evaluate interrupt condition for a single pin after its input level changed.
 * Sets the corresponding status bit and calls update_irq if triggered.
 */
static void esp32_gpio_eval_pin_interrupt(Esp32GpioState *s, int pin,
                                          bool old_level, bool new_level)
{
    uint32_t cfg = s->pin_reg[pin];
    int int_type = (cfg & GPIO_PIN_INT_TYPE_MASK) >> GPIO_PIN_INT_TYPE_SHIFT;
    int int_ena = (cfg & GPIO_PIN_INT_ENA_MASK) >> GPIO_PIN_INT_ENA_SHIFT;

    if (int_type == GPIO_INT_TYPE_DISABLE || int_ena == 0) {
        return;
    }

    bool triggered = false;

    switch (int_type) {
    case GPIO_INT_TYPE_RISING:
        triggered = !old_level && new_level;
        break;
    case GPIO_INT_TYPE_FALLING:
        triggered = old_level && !new_level;
        break;
    case GPIO_INT_TYPE_ANY_EDGE:
        triggered = (old_level != new_level);
        break;
    case GPIO_INT_TYPE_LOW_LEVEL:
        triggered = !new_level;
        break;
    case GPIO_INT_TYPE_HIGH_LEVEL:
        triggered = !!new_level;
        break;
    }

    if (triggered) {
        if (pin < 32) {
            s->gpio_status |= (1u << pin);
        } else {
            s->gpio_status1 |= (1u << (pin - 32));
        }
        esp32_gpio_update_irq(s);
    }
}


/*
 * Get the current input level of a pin (what GPIO_IN would report for it).
 * Output-enabled pins reflect the output register; input pins reflect
 * the externally driven level stored in gpio_in/gpio_in1.
 */
static inline bool esp32_gpio_get_pin_level(Esp32GpioState *s, int pin)
{
    if (pin < 32) {
        if (s->gpio_enable & (1u << pin)) {
            return (s->gpio_out >> pin) & 1;
        }
        return (s->gpio_in >> pin) & 1;
    } else {
        int bit = pin - 32;
        if (s->gpio_enable1 & (1u << bit)) {
            return (s->gpio_out1 >> bit) & 1;
        }
        return (s->gpio_in1 >> bit) & 1;
    }
}


/*
 * Handler for qdev_init_gpio_in_named("gpio-in").
 * Called when another QEMU device drives a GPIO input line.
 */
static void esp32_gpio_input_handler(void *opaque, int line, int level)
{
    Esp32GpioState *s = ESP32_GPIO(opaque);
    esp32_gpio_set_pin_level(s, line, level);
}


/*
 * Public API: set the level of a GPIO input pin.
 * Updates the internal input register and evaluates interrupt conditions.
 */
void esp32_gpio_set_pin_level(Esp32GpioState *s, int pin, int level)
{
    if (pin < 0 || pin >= ESP32_GPIO_MAX_COUNT) {
        warn_report("[GPIO] set_pin_level: pin %d out of range", pin);
        return;
    }

    bool old_level = esp32_gpio_get_pin_level(s, pin);

    /* Update the external input register */
    if (pin < 32) {
        if (level) {
            s->gpio_in |= (1u << pin);
        } else {
            s->gpio_in &= ~(1u << pin);
        }
    } else {
        int bit = pin - 32;
        if (level) {
            s->gpio_in1 |= (1u << bit);
        } else {
            s->gpio_in1 &= ~(1u << bit);
        }
    }

    bool new_level = esp32_gpio_get_pin_level(s, pin);

#if GPIO_DEBUG
    info_report("[GPIO] Pin %d: %d -> %d (input set to %d)",
                pin, old_level, new_level, level);
#endif

    /* Evaluate interrupt if the effective level changed */
    if (old_level != new_level) {
        esp32_gpio_eval_pin_interrupt(s, pin, old_level, new_level);
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
        /*
         * GPIO_IN reflects the actual pad level:
         *   - Output-enabled pins: reflects the output register value
         *   - Input pins: reflects the externally driven level (gpio_in)
         */
        r = (s->gpio_out & s->gpio_enable) |
            (s->gpio_in & ~s->gpio_enable);
        break;
    case A_GPIO_IN1:
        r = (s->gpio_out1 & s->gpio_enable1) |
            (s->gpio_in1 & ~s->gpio_enable1);
        /*
         * I2C bit-bang ACK: if any pins have a pending ACK, force
         * those bits LOW in this read, then clear the pending flag
         * and restore gpio_in1 to HIGH for future reads.
         */
        if (s->i2c_ack_pending1) {
            r &= ~s->i2c_ack_pending1;
            s->gpio_in1 |= s->i2c_ack_pending1;  /* restore to HIGH */
            s->i2c_ack_pending1 = 0;
        }
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
        {
            /* Detect output→input transitions on I2C SDA pins */
            uint32_t clearing = s->gpio_enable1 & ~wval;
            if (clearing & s->i2c_sda_ack_mask1) {
                uint32_t ack_pins = clearing & s->i2c_sda_ack_mask1;
                s->gpio_in1 &= ~ack_pins;       /* Pull SDA LOW (ACK) */
                s->i2c_ack_pending1 |= ack_pins; /* Mark for one-shot restore */
            }
            s->gpio_enable1 = wval;
        }
        break;
    case A_GPIO_ENABLE1_W1TS:
        s->gpio_enable1 |= wval;
        break;
    case A_GPIO_ENABLE1_W1TC:
        {
            /* Detect output→input transitions on I2C SDA pins */
            uint32_t clearing = wval & s->gpio_enable1;
            if (clearing & s->i2c_sda_ack_mask1) {
                uint32_t ack_pins = clearing & s->i2c_sda_ack_mask1;
                s->gpio_in1 &= ~ack_pins;       /* Pull SDA LOW (ACK) */
                s->i2c_ack_pending1 |= ack_pins; /* Mark for one-shot restore */
            }
            s->gpio_enable1 &= ~wval;
        }
        break;

    case A_GPIO_STATUS:
        s->gpio_status = wval;
        esp32_gpio_update_irq(s);
        break;
    case A_GPIO_STATUS_W1TS:
        s->gpio_status |= wval;
        esp32_gpio_update_irq(s);
        break;
    case A_GPIO_STATUS_W1TC:
        s->gpio_status &= ~wval;
        esp32_gpio_update_irq(s);
        break;

    case A_GPIO_STATUS1:
        s->gpio_status1 = wval;
        esp32_gpio_update_irq(s);
        break;
    case A_GPIO_STATUS1_W1TS:
        s->gpio_status1 |= wval;
        esp32_gpio_update_irq(s);
        break;
    case A_GPIO_STATUS1_W1TC:
        s->gpio_status1 &= ~wval;
        esp32_gpio_update_irq(s);
        break;

    default:
        /* Per-pin config registers */
        if (addr >= GPIO_PIN_REG_BASE &&
            addr < GPIO_PIN_REG_BASE + ESP32_GPIO_MAX_COUNT * 4) {
            int pin = (addr - GPIO_PIN_REG_BASE) / 4;
            s->pin_reg[pin] = wval;
            /*
             * Re-evaluate interrupt for this pin: firmware may have just
             * enabled an interrupt on a pin already at the triggering level.
             */
            bool level = esp32_gpio_get_pin_level(s, pin);
            int int_type = (wval & GPIO_PIN_INT_TYPE_MASK) >>
                           GPIO_PIN_INT_TYPE_SHIFT;
            int int_ena = (wval & GPIO_PIN_INT_ENA_MASK) >>
                          GPIO_PIN_INT_ENA_SHIFT;
            if (int_ena && int_type >= GPIO_INT_TYPE_LOW_LEVEL) {
                /* Level-triggered: check if already at triggering level */
                bool trig = (int_type == GPIO_INT_TYPE_LOW_LEVEL) ? !level
                                                                  : !!level;
                if (trig) {
                    if (pin < 32) {
                        s->gpio_status |= (1u << pin);
                    } else {
                        s->gpio_status1 |= (1u << (pin - 32));
                    }
                    esp32_gpio_update_irq(s);
                }
            }
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
    /*
     * Default input level: all HIGH.
     * This simulates pull-up resistors on all pins, which matches the
     * M5StickS3 button convention (active-low: HIGH = released, LOW = pressed).
     * Firmware that configures pull-downs will override via IO_MUX (not yet
     * emulated), but for buttons this is the correct default.
     */
    s->gpio_in = 0xFFFFFFFF;
    s->gpio_in1 = 0x0001FFFF;  /* Only bits 0-16 valid (GPIO 32-48) */
    s->gpio_status = 0;
    s->gpio_status1 = 0;

    s->i2c_ack_pending1 = 0;
    /* i2c_sda_ack_mask1 is set by machine config, don't clear it here */

    memset(s->pin_reg, 0, sizeof(s->pin_reg));
    memset(s->func_in_sel, 0, sizeof(s->func_in_sel));
    memset(s->func_out_sel, 0, sizeof(s->func_out_sel));

    /* Lower the interrupt line */
    qemu_irq_lower(s->irq);
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

    /* Initialize input GPIOs so other devices can drive pin state */
    qdev_init_gpio_in_named(DEVICE(s), esp32_gpio_input_handler, "gpio-in",
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
