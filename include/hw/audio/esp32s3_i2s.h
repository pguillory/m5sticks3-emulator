/*
 * ESP32-S3 I2S controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * I2S (Inter-IC Sound) peripheral for the ESP32-S3. Supports both TX (playback)
 * and RX (capture) with DMA via GDMA. Connects to audio codecs (e.g., ES8311)
 * on the M5StickS3.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/dma/esp_gdma.h"
#include "audio/audio.h"

#define TYPE_ESP32S3_I2S "audio.esp32s3.i2s"
#define ESP32S3_I2S(obj) OBJECT_CHECK(ESP32S3I2sState, (obj), TYPE_ESP32S3_I2S)

#define ESP32S3_I2S_IO_SIZE     0x100


typedef struct ESP32S3I2sState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* GDMA reference for DMA transfers */
    ESPGdmaState *gdma;
    GdmaPeripheral gdma_periph_id;

    /* QEMU audio interface */
    QEMUSoundCard card;
    SWVoiceOut *out_voice;
    SWVoiceIn *in_voice;

    /* Audio configuration derived from registers */
    int sample_rate;
    int channels;
    int bits_per_sample;

    /* Audio buffers for DMA callback processing */
    uint8_t out_buf[4096];
    int out_buf_pos;
    uint8_t in_buf[4096];
    int in_buf_pos;
    int in_buf_len;

    /* TX active state (set when DMA is feeding data) */
    bool tx_running;
    bool rx_running;

    /* Registers -- based on ESP32-S3 TRM I2S chapter */
    uint32_t int_raw;
    uint32_t int_st;
    uint32_t int_ena;
    uint32_t int_clr;

    uint32_t rx_conf;
    uint32_t tx_conf;
    uint32_t rx_conf1;
    uint32_t tx_conf1;

    uint32_t rx_clkm_conf;
    uint32_t tx_clkm_conf;
    uint32_t rx_clkm_div_conf;
    uint32_t tx_clkm_div_conf;

    uint32_t tx_pcm2pdm_conf;
    uint32_t tx_pcm2pdm_conf1;
    uint32_t rx_pdm2pcm_conf;

    uint32_t tx_tdm_ctrl;
    uint32_t tx_tdm_active_chan;
    uint32_t rx_tdm_ctrl;
    uint32_t rx_tdm_active_chan;

    uint32_t rx_timing;
    uint32_t tx_timing;

    uint32_t lc_hung_conf;
    uint32_t rxeof_num;

    uint32_t conf_sigle_data;  /* single data output register */

    uint32_t state;

    uint32_t date;

} ESP32S3I2sState;


/* ---- Register offsets (ESP32-S3 I2S) ---- */

REG32(I2S_INT_RAW, 0x0C)
    FIELD(I2S_INT_RAW, RX_DONE,           0, 1)
    FIELD(I2S_INT_RAW, TX_DONE,           1, 1)
    FIELD(I2S_INT_RAW, RX_HUNG,           2, 1)
    FIELD(I2S_INT_RAW, TX_HUNG,           3, 1)

REG32(I2S_INT_ST, 0x10)
    FIELD(I2S_INT_ST, RX_DONE,            0, 1)
    FIELD(I2S_INT_ST, TX_DONE,            1, 1)
    FIELD(I2S_INT_ST, RX_HUNG,            2, 1)
    FIELD(I2S_INT_ST, TX_HUNG,            3, 1)

REG32(I2S_INT_ENA, 0x14)
    FIELD(I2S_INT_ENA, RX_DONE,           0, 1)
    FIELD(I2S_INT_ENA, TX_DONE,           1, 1)
    FIELD(I2S_INT_ENA, RX_HUNG,           2, 1)
    FIELD(I2S_INT_ENA, TX_HUNG,           3, 1)

REG32(I2S_INT_CLR, 0x18)
    FIELD(I2S_INT_CLR, RX_DONE,           0, 1)
    FIELD(I2S_INT_CLR, TX_DONE,           1, 1)
    FIELD(I2S_INT_CLR, RX_HUNG,           2, 1)
    FIELD(I2S_INT_CLR, TX_HUNG,           3, 1)

REG32(I2S_RX_CONF, 0x20)
    FIELD(I2S_RX_CONF, RX_RESET,          0, 1)
    FIELD(I2S_RX_CONF, RX_FIFO_RESET,     1, 1)
    FIELD(I2S_RX_CONF, RX_START,          2, 1)
    FIELD(I2S_RX_CONF, RX_SLAVE_MOD,      3, 1)
    FIELD(I2S_RX_CONF, RX_MONO,           5, 1)
    FIELD(I2S_RX_CONF, RX_BIG_ENDIAN,     7, 1)
    FIELD(I2S_RX_CONF, RX_UPDATE,         8, 1)
    FIELD(I2S_RX_CONF, RX_MONO_FST_VLD,   9, 1)
    FIELD(I2S_RX_CONF, RX_PCM_CONF,      10, 2)
    FIELD(I2S_RX_CONF, RX_PCM_BYPASS,    12, 1)
    FIELD(I2S_RX_CONF, RX_STOP_MODE,     13, 2)
    FIELD(I2S_RX_CONF, RX_LEFT_ALIGN,    15, 1)
    FIELD(I2S_RX_CONF, RX_24_FILL_EN,    16, 1)
    FIELD(I2S_RX_CONF, RX_WS_IDLE_POL,   17, 1)
    FIELD(I2S_RX_CONF, RX_BIT_ORDER,     18, 1)
    FIELD(I2S_RX_CONF, RX_TDM_EN,        19, 1)
    FIELD(I2S_RX_CONF, RX_PDM_EN,        20, 1)

REG32(I2S_TX_CONF, 0x24)
    FIELD(I2S_TX_CONF, TX_RESET,          0, 1)
    FIELD(I2S_TX_CONF, TX_FIFO_RESET,     1, 1)
    FIELD(I2S_TX_CONF, TX_START,          2, 1)
    FIELD(I2S_TX_CONF, TX_SLAVE_MOD,      3, 1)
    FIELD(I2S_TX_CONF, TX_MONO,           5, 1)
    FIELD(I2S_TX_CONF, TX_CHAN_EQUAL,     6, 1)
    FIELD(I2S_TX_CONF, TX_BIG_ENDIAN,     7, 1)
    FIELD(I2S_TX_CONF, TX_UPDATE,         8, 1)
    FIELD(I2S_TX_CONF, TX_MONO_FST_VLD,   9, 1)
    FIELD(I2S_TX_CONF, TX_PCM_CONF,      10, 2)
    FIELD(I2S_TX_CONF, TX_PCM_BYPASS,    12, 1)
    FIELD(I2S_TX_CONF, TX_STOP_MODE,     13, 2)
    FIELD(I2S_TX_CONF, TX_LEFT_ALIGN,    15, 1)
    FIELD(I2S_TX_CONF, TX_24_FILL_EN,    16, 1)
    FIELD(I2S_TX_CONF, TX_WS_IDLE_POL,   17, 1)
    FIELD(I2S_TX_CONF, TX_BIT_ORDER,     18, 1)
    FIELD(I2S_TX_CONF, TX_TDM_EN,        19, 1)
    FIELD(I2S_TX_CONF, TX_PDM_EN,        20, 1)
    FIELD(I2S_TX_CONF, TX_BCK_NO_DLY,    24, 1)

REG32(I2S_RX_CONF1, 0x28)
    FIELD(I2S_RX_CONF1, RX_TDM_WS_WIDTH,      0, 7)
    FIELD(I2S_RX_CONF1, RX_BCK_DIV_NUM,       7, 6)
    FIELD(I2S_RX_CONF1, RX_BITS_MOD,         13, 5)
    FIELD(I2S_RX_CONF1, RX_HALF_SAMPLE_BITS, 18, 6)
    FIELD(I2S_RX_CONF1, RX_TDM_CHAN_BITS,    24, 5)
    FIELD(I2S_RX_CONF1, RX_MSB_SHIFT,        29, 1)

REG32(I2S_TX_CONF1, 0x2C)
    FIELD(I2S_TX_CONF1, TX_TDM_WS_WIDTH,      0, 7)
    FIELD(I2S_TX_CONF1, TX_BCK_DIV_NUM,       7, 6)
    FIELD(I2S_TX_CONF1, TX_BITS_MOD,         13, 5)
    FIELD(I2S_TX_CONF1, TX_HALF_SAMPLE_BITS, 18, 6)
    FIELD(I2S_TX_CONF1, TX_TDM_CHAN_BITS,    24, 5)
    FIELD(I2S_TX_CONF1, TX_MSB_SHIFT,        29, 1)

REG32(I2S_RX_CLKM_CONF, 0x30)
    FIELD(I2S_RX_CLKM_CONF, RX_CLKM_DIV_NUM, 0, 8)
    FIELD(I2S_RX_CLKM_CONF, RX_CLK_ACTIVE,   26, 1)
    FIELD(I2S_RX_CLKM_CONF, RX_CLK_SEL,      27, 2)
    FIELD(I2S_RX_CLKM_CONF, MCLK_SEL,        29, 1)

REG32(I2S_TX_CLKM_CONF, 0x34)
    FIELD(I2S_TX_CLKM_CONF, TX_CLKM_DIV_NUM, 0, 8)
    FIELD(I2S_TX_CLKM_CONF, TX_CLK_ACTIVE,   26, 1)
    FIELD(I2S_TX_CLKM_CONF, TX_CLK_SEL,      27, 2)

REG32(I2S_RX_CLKM_DIV_CONF, 0x38)
    FIELD(I2S_RX_CLKM_DIV_CONF, RX_CLKM_DIV_Z, 0, 9)
    FIELD(I2S_RX_CLKM_DIV_CONF, RX_CLKM_DIV_Y, 9, 9)
    FIELD(I2S_RX_CLKM_DIV_CONF, RX_CLKM_DIV_X, 18, 9)
    FIELD(I2S_RX_CLKM_DIV_CONF, RX_CLKM_DIV_YN1, 27, 1)

REG32(I2S_TX_CLKM_DIV_CONF, 0x3C)
    FIELD(I2S_TX_CLKM_DIV_CONF, TX_CLKM_DIV_Z, 0, 9)
    FIELD(I2S_TX_CLKM_DIV_CONF, TX_CLKM_DIV_Y, 9, 9)
    FIELD(I2S_TX_CLKM_DIV_CONF, TX_CLKM_DIV_X, 18, 9)
    FIELD(I2S_TX_CLKM_DIV_CONF, TX_CLKM_DIV_YN1, 27, 1)

REG32(I2S_TX_PCM2PDM_CONF, 0x40)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_HP_BYPASS, 25, 1)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_SINC_OSR2, 19, 4)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_PRESCALE,  13, 6)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_HP_IN_SHIFT, 11, 2)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_LP_IN_SHIFT, 9, 2)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_SINC_IN_SHIFT, 7, 2)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_SIGMADELTA_IN_SHIFT, 5, 2)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_DAC_2OUT_EN, 4, 1)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_DAC_MODE_EN, 3, 1)
    FIELD(I2S_TX_PCM2PDM_CONF, PCM2PDM_CONV_EN, 2, 1)
    FIELD(I2S_TX_PCM2PDM_CONF, TX_PDM_FP,       0, 2)

REG32(I2S_TX_PCM2PDM_CONF1, 0x44)
    FIELD(I2S_TX_PCM2PDM_CONF1, TX_PDM_FS,            0, 10)
    FIELD(I2S_TX_PCM2PDM_CONF1, TX_IIR_HP_MULT12_5,  10, 3)
    FIELD(I2S_TX_PCM2PDM_CONF1, TX_IIR_HP_MULT12_0,  13, 3)

REG32(I2S_RX_PDM2PCM_CONF, 0x48)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_PDM2PCM_EN,   19, 1)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_PDM_SINC_DSR_16_EN, 18, 1)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_PDM2PCM_AMPLIFY_NUM, 13, 5)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_PDM_HP_BYPASS, 12, 1)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_IIR_HP_MULT12_5, 7, 3)
    FIELD(I2S_RX_PDM2PCM_CONF, RX_IIR_HP_MULT12_0, 4, 3)

REG32(I2S_TX_TDM_CTRL, 0x50)
    FIELD(I2S_TX_TDM_CTRL, TX_TDM_TOT_CHAN_NUM,  0, 4)
    FIELD(I2S_TX_TDM_CTRL, TX_TDM_SKIP_MSK_EN, 16, 1)

REG32(I2S_TX_TDM_ACTIVE_CHAN, 0x54)

REG32(I2S_RX_TDM_CTRL, 0x58)
    FIELD(I2S_RX_TDM_CTRL, RX_TDM_TOT_CHAN_NUM,  0, 4)
    FIELD(I2S_RX_TDM_CTRL, RX_TDM_SKIP_MSK_EN, 16, 1)

REG32(I2S_RX_TDM_ACTIVE_CHAN, 0x5C)

REG32(I2S_RX_TIMING, 0x60)
REG32(I2S_TX_TIMING, 0x64)

REG32(I2S_LC_HUNG_CONF, 0x68)
    FIELD(I2S_LC_HUNG_CONF, LC_FIFO_TIMEOUT,      0, 8)
    FIELD(I2S_LC_HUNG_CONF, LC_FIFO_TIMEOUT_SHIFT, 8, 3)
    FIELD(I2S_LC_HUNG_CONF, LC_FIFO_TIMEOUT_ENA, 11, 1)

REG32(I2S_RXEOF_NUM, 0x6C)
    FIELD(I2S_RXEOF_NUM, RX_EOF_NUM, 0, 12)

REG32(I2S_CONF_SIGLE_DATA, 0x70)

REG32(I2S_STATE, 0x74)
    FIELD(I2S_STATE, TX_IDLE, 0, 1)

REG32(I2S_DATE, 0x80)
    FIELD(I2S_DATE, DATE, 0, 28)
