/*
 * BM8563 RTC I2C stub (M5StickS3 uses this at 0x51)
 *
 * Minimal stub that ACKs all transactions and returns zeros.
 * M5Unified's board detection probes this address after finding M5PM1.
 */

#pragma once

#include "hw/i2c/i2c.h"

#define TYPE_BM8563 "bm8563"
#define BM8563(obj) OBJECT_CHECK(BM8563State, (obj), TYPE_BM8563)

#define BM8563_I2C_ADDR 0x51
#define BM8563_NUM_REGS 16

typedef struct BM8563State {
    I2CSlave parent_obj;
    uint8_t regs[BM8563_NUM_REGS];
    uint8_t reg_addr;
    bool addr_received;
} BM8563State;
