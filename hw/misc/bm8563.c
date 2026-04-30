/*
 * BM8563 RTC I2C stub (M5StickS3 uses this at 0x51)
 *
 * Minimal stub that ACKs all transactions and returns zeros.
 * M5Unified's board detection probes this address after finding M5PM1.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/bm8563.h"


static int bm8563_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    BM8563State *s = BM8563(i2c);

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


static uint8_t bm8563_i2c_recv(I2CSlave *i2c)
{
    BM8563State *s = BM8563(i2c);
    uint8_t val = s->regs[s->reg_addr % BM8563_NUM_REGS];
    s->reg_addr++;
    return val;
}


static int bm8563_i2c_send(I2CSlave *i2c, uint8_t data)
{
    BM8563State *s = BM8563(i2c);

    if (!s->addr_received) {
        s->reg_addr = data;
        s->addr_received = true;
    } else {
        s->regs[s->reg_addr % BM8563_NUM_REGS] = data;
        s->reg_addr++;
    }
    return 0;
}


static void bm8563_reset_hold(Object *obj, ResetType type)
{
    BM8563State *s = BM8563(obj);
    memset(s->regs, 0, sizeof(s->regs));
    s->reg_addr = 0;
    s->addr_received = false;
}


static void bm8563_init(Object *obj)
{
    BM8563State *s = BM8563(obj);
    memset(s->regs, 0, sizeof(s->regs));
}


static void bm8563_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bm8563_reset_hold;
    sc->event = bm8563_i2c_event;
    sc->recv = bm8563_i2c_recv;
    sc->send = bm8563_i2c_send;
}


static const TypeInfo bm8563_info = {
    .name          = TYPE_BM8563,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(BM8563State),
    .instance_init = bm8563_init,
    .class_init    = bm8563_class_init,
};


static void bm8563_register_types(void)
{
    type_register_static(&bm8563_info);
}

type_init(bm8563_register_types)
