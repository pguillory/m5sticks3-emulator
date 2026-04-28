/*
 * ES8311 audio codec I2C emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Emulates the ES8311 codec's I2C register interface. The ESP-IDF
 * ES8311 driver writes clock configuration, gain settings, and
 * power control through I2C. This model accepts all writes and returns
 * sensible defaults on reads so the driver initializes successfully.
 *
 * Actual audio data flows through the I2S peripheral, not through I2C.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/audio/es8311.h"

#define ES8311_DEBUG    0


/*
 * Set default register values to match a real ES8311 after power-on.
 */
static void es8311_reset_regs(ES8311State *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    /* Chip ID registers (read-only) */
    s->regs[ES8311_REG00_RESET] = 0x80;  /* CSM_ON=1 after init */

    /* Default clock settings */
    s->regs[ES8311_REG01_CLK_MANAGER1] = 0x30;  /* MCLK from pin, divider=1 */
    s->regs[ES8311_REG02_CLK_MANAGER2] = 0x00;
    s->regs[ES8311_REG06_CLK_MANAGER6] = 0x08;  /* BCLK divider */

    /* Default SDP (serial data port) settings - 16-bit I2S */
    s->regs[ES8311_REG09_SDPIN]  = 0x0C;  /* 16-bit, I2S format */
    s->regs[ES8311_REG0A_SDPOUT] = 0x0C;  /* 16-bit, I2S format */

    /* System defaults */
    s->regs[ES8311_REG0B_SYSTEM] = 0x00;
    s->regs[ES8311_REG0C_SYSTEM] = 0x00;
    s->regs[ES8311_REG0D_SYSTEM] = 0x01;  /* Power on defaults */
    s->regs[ES8311_REG0E_SYSTEM] = 0x02;

    /* ADC defaults */
    s->regs[ES8311_REG0F_ADC] = 0x00;
    s->regs[ES8311_REG10_ADCVOL] = 0xBF;  /* ADC volume: 0dB */
    s->regs[ES8311_REG14_ADC] = 0x1A;     /* ALC off, PGA gain default */

    /* DAC defaults */
    s->regs[ES8311_REG15_DAC] = 0x00;
    s->regs[ES8311_REG17_DACVOL] = 0xBF;  /* DAC volume: 0dB */

    /* GPIO defaults */
    s->regs[ES8311_REG32_GPIO] = 0x00;

    /* Status flag */
    s->regs[ES8311_REG44_FLAG] = 0x00;  /* No flags active */
}


static int es8311_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    ES8311State *s = ES8311(i2c);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
        s->addr_received = false;
        break;
    case I2C_START_RECV:
        break;
    case I2C_FINISH:
        break;
    case I2C_NACK:
        break;
    }
    return 0;
}


static uint8_t es8311_i2c_recv(I2CSlave *i2c)
{
    ES8311State *s = ES8311(i2c);
    uint8_t val = 0;

    if (s->reg_addr < ES8311_REG_COUNT) {
        val = s->regs[s->reg_addr];
    } else {
        /* Chip identification registers at high addresses */
        switch (s->reg_addr) {
        case 0xFA:
            val = 0x83;  /* ES8311 chip ID byte 1 */
            break;
        case 0xFB:
            val = 0x11;  /* ES8311 chip ID byte 2 */
            break;
        case 0xFC:
            val = 0x00;  /* ES8311 chip ID byte 3 */
            break;
        case 0xFD:
            val = 0x00;  /* Chip version */
            break;
        default:
            val = 0;
            break;
        }
    }

#if ES8311_DEBUG
    info_report("[ES8311] Read reg 0x%02x => 0x%02x", s->reg_addr, val);
#endif

    /* Auto-increment register address */
    s->reg_addr++;
    return val;
}


static int es8311_i2c_send(I2CSlave *i2c, uint8_t data)
{
    ES8311State *s = ES8311(i2c);

    if (!s->addr_received) {
        /* First byte after START is the register address */
        s->reg_addr = data;
        s->addr_received = true;
    } else {
        /* Subsequent bytes are data to write */
#if ES8311_DEBUG
        info_report("[ES8311] Write reg 0x%02x <= 0x%02x", s->reg_addr, data);
#endif
        if (s->reg_addr < ES8311_REG_COUNT) {
            /* Handle special register behavior */
            if (s->reg_addr == ES8311_REG00_RESET && (data & 0x80) == 0) {
                /* Writing 0 to CSM_ON performs a software reset */
                es8311_reset_regs(s);
            } else {
                s->regs[s->reg_addr] = data;
            }
        }
        s->reg_addr++;
    }
    return 0;
}


static void es8311_reset_hold(Object *obj, ResetType type)
{
    ES8311State *s = ES8311(obj);
    es8311_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void es8311_realize(DeviceState *dev, Error **errp)
{
}


static void es8311_init(Object *obj)
{
    ES8311State *s = ES8311(obj);
    es8311_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void es8311_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = es8311_reset_hold;
    dc->realize = es8311_realize;
    sc->event = es8311_i2c_event;
    sc->recv = es8311_i2c_recv;
    sc->send = es8311_i2c_send;
}


static const TypeInfo es8311_info = {
    .name          = TYPE_ES8311,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ES8311State),
    .instance_init = es8311_init,
    .class_init    = es8311_class_init,
};


static void es8311_register_types(void)
{
    type_register_static(&es8311_info);
}

type_init(es8311_register_types)
