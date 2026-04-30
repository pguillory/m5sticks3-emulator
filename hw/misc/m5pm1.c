/*
 * M5PM1 PMIC I2C emulation (PY32-based, used on M5StickS3)
 *
 * The M5StickS3 uses a custom PY32-based PMIC (M5PM1) at I2C address 0x6E
 * instead of a standard AXP2101. M5Unified's M5GFX auto-detection probes
 * this address to identify M5StickS3 boards (pkg_ver=1 path). The detection
 * only needs the device to ACK and return any value for register 0x00.
 *
 * GPIO2 on M5PM1 controls LCD power (L3B Enable).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/m5pm1.h"

#define M5PM1_DEBUG  1


static void m5pm1_reset_regs(M5PM1State *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    /* Device ID — M5Unified checks has_value(), some code checks for 0x50 */
    s->regs[M5PM1_REG_DEVICE_ID] = 0x50;

    /* GPIO defaults: all inputs, push-pull mode, GPIO function */
    s->regs[M5PM1_REG_GPIO_DIR]      = 0x00;
    s->regs[M5PM1_REG_GPIO_OUT]      = 0x00;
    s->regs[M5PM1_REG_GPIO_OUT_MODE] = 0x00;
    s->regs[M5PM1_REG_GPIO_FUNC_SEL] = 0x00;
}


static int m5pm1_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    M5PM1State *s = M5PM1(i2c);

    static const char *event_names[] = {
        [I2C_START_SEND] = "START_SEND",
        [I2C_START_SEND_ASYNC] = "START_SEND_ASYNC",
        [I2C_START_RECV] = "START_RECV",
        [I2C_FINISH] = "FINISH",
        [I2C_NACK] = "NACK",
    };
    warn_report("[M5PM1] event: %s", event_names[event]);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
        s->addr_received = false;
        break;
    case I2C_START_RECV:
    case I2C_FINISH:
    case I2C_NACK:
        break;
    }
    return 0;
}


static uint8_t m5pm1_i2c_recv(I2CSlave *i2c)
{
    M5PM1State *s = M5PM1(i2c);
    uint8_t val = s->regs[s->reg_addr];

    warn_report("[M5PM1] Read reg 0x%02x => 0x%02x", s->reg_addr, val);

    s->reg_addr++;
    return val;
}


static int m5pm1_i2c_send(I2CSlave *i2c, uint8_t data)
{
    M5PM1State *s = M5PM1(i2c);

    if (!s->addr_received) {
        s->reg_addr = data;
        s->addr_received = true;
        warn_report("[M5PM1] Set reg addr = 0x%02x", data);
    } else {
        warn_report("[M5PM1] Write reg 0x%02x <= 0x%02x", s->reg_addr, data);
        s->regs[s->reg_addr] = data;
        s->reg_addr++;
    }
    return 0;
}


static void m5pm1_reset_hold(Object *obj, ResetType type)
{
    M5PM1State *s = M5PM1(obj);
    m5pm1_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void m5pm1_realize(DeviceState *dev, Error **errp)
{
}


static void m5pm1_init(Object *obj)
{
    M5PM1State *s = M5PM1(obj);
    m5pm1_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void m5pm1_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = m5pm1_reset_hold;
    dc->realize = m5pm1_realize;
    sc->event = m5pm1_i2c_event;
    sc->recv = m5pm1_i2c_recv;
    sc->send = m5pm1_i2c_send;
}


static const TypeInfo m5pm1_info = {
    .name          = TYPE_M5PM1,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(M5PM1State),
    .instance_init = m5pm1_init,
    .class_init    = m5pm1_class_init,
};


static void m5pm1_register_types(void)
{
    type_register_static(&m5pm1_info);
}

type_init(m5pm1_register_types)
