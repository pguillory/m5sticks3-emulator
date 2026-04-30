/*
 * ESP32-S3 GPSPI (SPI2/SPI3) controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * General-purpose SPI controller for the ESP32-S3. This is distinct from
 * SPI0/SPI1 (the flash/cache controllers). SPI2 and SPI3 drive external
 * peripherals such as displays, SD cards, and sensors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/registerfields.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"

#define TYPE_ESP32S3_GPSPI "ssi.esp32s3.gpspi"
#define ESP32S3_GPSPI(obj) OBJECT_CHECK(ESP32S3GpspiState, (obj), TYPE_ESP32S3_GPSPI)

#define ESP32S3_GPSPI_BUF_WORDS   16
#define ESP32S3_GPSPI_CS_COUNT    6
#define ESP32S3_GPSPI_IO_SIZE     (0xF0 + 4)


typedef struct ESP32S3GpspiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;
    qemu_irq cs_gpio[ESP32S3_GPSPI_CS_COUNT];
    qemu_irq irq;

    /* Configuration registers */
    uint32_t cmd;
    uint32_t addr;
    uint32_t ctrl;
    uint32_t clock;
    uint32_t user;
    uint32_t user1;
    uint32_t user2;
    uint32_t ms_dlen;
    uint32_t misc;
    uint32_t din_mode;
    uint32_t din_num;
    uint32_t dout_mode;
    uint32_t dma_conf;
    uint32_t dma_int_ena;
    uint32_t dma_int_raw;
    uint32_t dma_int_clr;
    uint32_t dma_int_set;
    uint32_t slave;
    uint32_t slave1;
    uint32_t clk_gate;
    uint32_t date;

    /* Data buffer */
    uint32_t data_reg[ESP32S3_GPSPI_BUF_WORDS];

    /* GDMA reference for DMA transfers */
    ESPGdmaState *gdma;
    GdmaPeripheral gdma_periph_id;

} ESP32S3GpspiState;


/* ---- Register offsets (GPSPI, NOT SPI_MEM) ---- */

REG32(GPSPI_CMD, 0x00)
    FIELD(GPSPI_CMD, CONF_BITLEN, 0, 23)
    FIELD(GPSPI_CMD, UPDATE, 23, 1)
    FIELD(GPSPI_CMD, USR,    24, 1)

REG32(GPSPI_ADDR, 0x04)
    FIELD(GPSPI_ADDR, USR_ADDR_VALUE, 0, 32)

REG32(GPSPI_CTRL, 0x08)
    FIELD(GPSPI_CTRL, WR_BIT_ORDER, 26, 1)
    FIELD(GPSPI_CTRL, RD_BIT_ORDER, 25, 1)
    FIELD(GPSPI_CTRL, FREAD_QIO,    24, 1)
    FIELD(GPSPI_CTRL, FREAD_DIO,    23, 1)
    FIELD(GPSPI_CTRL, FREAD_QUAD,   20, 1)
    FIELD(GPSPI_CTRL, FREAD_DUAL,   14, 1)
    FIELD(GPSPI_CTRL, Q_POL,        18, 1)
    FIELD(GPSPI_CTRL, D_POL,        19, 1)

REG32(GPSPI_CLOCK, 0x0C)
    FIELD(GPSPI_CLOCK, CLK_EQU_SYSCLK, 31, 1)
    FIELD(GPSPI_CLOCK, CLKCNT_N,       16, 6)
    FIELD(GPSPI_CLOCK, CLKCNT_H,       8, 6)
    FIELD(GPSPI_CLOCK, CLKCNT_L,       0, 6)

REG32(GPSPI_USER, 0x10)
    FIELD(GPSPI_USER, USR_COMMAND,   31, 1)
    FIELD(GPSPI_USER, USR_ADDR,      30, 1)
    FIELD(GPSPI_USER, USR_DUMMY,     29, 1)
    FIELD(GPSPI_USER, USR_MISO,      28, 1)
    FIELD(GPSPI_USER, USR_MOSI,      27, 1)
    FIELD(GPSPI_USER, USR_DUMMY_IDLE,26, 1)
    FIELD(GPSPI_USER, USR_MOSI_HIGHPART, 25, 1)
    FIELD(GPSPI_USER, USR_MISO_HIGHPART, 24, 1)
    FIELD(GPSPI_USER, SIO,           17, 1)
    FIELD(GPSPI_USER, CK_OUT_EDGE,   9, 1)
    FIELD(GPSPI_USER, CS_SETUP,      7, 1)
    FIELD(GPSPI_USER, CS_HOLD,       6, 1)
    FIELD(GPSPI_USER, DOUTDIN,       0, 1)

REG32(GPSPI_USER1, 0x14)
    FIELD(GPSPI_USER1, USR_ADDR_BITLEN,    27, 5)
    FIELD(GPSPI_USER1, USR_DUMMY_CYCLELEN, 0, 8)

REG32(GPSPI_USER2, 0x18)
    FIELD(GPSPI_USER2, USR_COMMAND_BITLEN, 28, 4)
    FIELD(GPSPI_USER2, USR_COMMAND_VALUE,  0, 16)

REG32(GPSPI_MS_DLEN, 0x1C)
    FIELD(GPSPI_MS_DLEN, MS_DATA_BITLEN, 0, 18)

REG32(GPSPI_MISC, 0x20)
    FIELD(GPSPI_MISC, CS_KEEP_ACTIVE, 10, 1)
    FIELD(GPSPI_MISC, CK_IDLE_EDGE,   9, 1)
    FIELD(GPSPI_MISC, CS5_DIS,        6, 1)
    FIELD(GPSPI_MISC, CS4_DIS,        5, 1)
    FIELD(GPSPI_MISC, CS3_DIS,        4, 1)
    FIELD(GPSPI_MISC, CS2_DIS,        3, 1)
    FIELD(GPSPI_MISC, CS1_DIS,        2, 1)
    FIELD(GPSPI_MISC, CS0_DIS,        1, 1)
    FIELD(GPSPI_MISC, CK_DIS,         0, 1)

REG32(GPSPI_DIN_MODE,  0x24)
REG32(GPSPI_DIN_NUM,   0x28)
REG32(GPSPI_DOUT_MODE, 0x2C)

REG32(GPSPI_DMA_CONF, 0x30)
    FIELD(GPSPI_DMA_CONF, DMA_TX_ENA,    31, 1)
    FIELD(GPSPI_DMA_CONF, DMA_RX_ENA,    30, 1)
    FIELD(GPSPI_DMA_CONF, RX_AFIFO_RST,  29, 1)
    FIELD(GPSPI_DMA_CONF, BUF_AFIFO_RST, 28, 1)
    FIELD(GPSPI_DMA_CONF, DMA_AFIFO_RST, 27, 1)

REG32(GPSPI_DMA_INT_ENA, 0x34)
    FIELD(GPSPI_DMA_INT_ENA, TRANS_DONE,     13, 1)
    FIELD(GPSPI_DMA_INT_ENA, MST_RX_AFIFO_WFULL_ERR, 12, 1)
    FIELD(GPSPI_DMA_INT_ENA, MST_TX_AFIFO_REMPTY_ERR, 11, 1)
    FIELD(GPSPI_DMA_INT_ENA, DMA_SEG_TRANS_DONE, 7, 1)
    FIELD(GPSPI_DMA_INT_ENA, SLV_CMD_ERR, 5, 1)

REG32(GPSPI_DMA_INT_CLR, 0x38)
REG32(GPSPI_DMA_INT_RAW, 0x3C)
    FIELD(GPSPI_DMA_INT_RAW, TRANS_DONE,     13, 1)

REG32(GPSPI_DMA_INT_ST,  0x40)
    FIELD(GPSPI_DMA_INT_ST, TRANS_DONE,      13, 1)

REG32(GPSPI_DMA_INT_SET, 0x44)

/* Data buffer registers: W0 at 0x98, W1 at 0x9C, ... W15 at 0xD4 */
#define A_GPSPI_W0   0x98
#define A_GPSPI_W15  0xD4

REG32(GPSPI_SLAVE, 0xE0)
    FIELD(GPSPI_SLAVE, SLAVE_MODE, 26, 1)
    FIELD(GPSPI_SLAVE, USR_CONF,   28, 1)

REG32(GPSPI_SLAVE1, 0xE4)

REG32(GPSPI_CLK_GATE, 0xE8)
    FIELD(GPSPI_CLK_GATE, MST_CLK_ACTIVE, 2, 1)
    FIELD(GPSPI_CLK_GATE, MST_CLK_SEL,    1, 1)
    FIELD(GPSPI_CLK_GATE, CLK_EN,         0, 1)

REG32(GPSPI_DATE, 0xF0)
    FIELD(GPSPI_DATE, DATE, 0, 28)
