/*
 * ESP32-S3 GPSPI (SPI2/SPI3) controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * General-purpose SPI controller for the ESP32-S3. Handles both CPU-mode
 * transfers (via W0-W15 data registers) and DMA-mode transfers (via GDMA).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/esp32s3_gpspi.h"
#include "qemu/error-report.h"

#define GPSPI_DEBUG   0
#define GPSPI_WARNING 1


static void esp32s3_gpspi_update_irq(ESP32S3GpspiState *s)
{
    uint32_t int_st = s->dma_int_raw & s->dma_int_ena;
    qemu_set_irq(s->irq, int_st ? 1 : 0);
}


static void esp32s3_gpspi_cs_set(ESP32S3GpspiState *s, int value)
{
    for (int i = 0; i < ESP32S3_GPSPI_CS_COUNT; i++) {
        /* Each CSn_DIS bit at position (i+1) in MISC register disables that CS */
        uint32_t cs_dis_mask = 1 << (i + 1);
        if (!(s->misc & cs_dis_mask)) {
            qemu_set_irq(s->cs_gpio[i], value ? 1 : 0);
        } else {
            qemu_set_irq(s->cs_gpio[i], 1);  /* CS inactive (high) when disabled */
        }
    }
}


static void esp32s3_gpspi_txrx_buffer(ESP32S3GpspiState *s,
                                       const uint8_t *tx, int tx_bytes,
                                       uint8_t *rx, int rx_bytes)
{
    int bytes = MAX(tx_bytes, rx_bytes);
    for (int i = 0; i < bytes; i++) {
        uint8_t byte = 0;
        if (i < tx_bytes && tx != NULL) {
            byte = tx[i];
        }
        uint32_t res = ssi_transfer(s->spi, byte);
        if (i < rx_bytes && rx != NULL) {
            rx[i] = (uint8_t)res;
        }
    }
}


static void esp32s3_gpspi_do_dma_transfer(ESP32S3GpspiState *s, int data_bytes)
{
    if (s->gdma == NULL) {
#if GPSPI_WARNING
        warn_report("[GPSPI] DMA transfer requested but no GDMA connected");
#endif
        return;
    }

    bool dma_tx = FIELD_EX32(s->dma_conf, GPSPI_DMA_CONF, DMA_TX_ENA);
    bool dma_rx = FIELD_EX32(s->dma_conf, GPSPI_DMA_CONF, DMA_RX_ENA);

    if (dma_tx && data_bytes > 0) {
        /*
         * TX DMA: GDMA reads data from memory (OUT direction from GDMA's
         * perspective) and we send it over the SPI bus.
         *
         * Process in chunks to avoid allocating huge buffers.
         */
        uint32_t gdma_chan;
        if (!esp_gdma_get_channel_periph(s->gdma, s->gdma_periph_id,
                                         ESP_GDMA_OUT_IDX, &gdma_chan)) {
#if GPSPI_WARNING
            warn_report("[GPSPI] No GDMA OUT channel found for peripheral %d",
                        s->gdma_periph_id);
#endif
            return;
        }

        /* Read from GDMA in chunks and send over SPI */
        int remaining = data_bytes;
        uint8_t chunk[256];
        while (remaining > 0) {
            int chunk_size = MIN(remaining, (int)sizeof(chunk));
            if (!esp_gdma_read_channel(s->gdma, gdma_chan, chunk, chunk_size)) {
                break;
            }
            esp32s3_gpspi_txrx_buffer(s, chunk, chunk_size, NULL, 0);
            remaining -= chunk_size;
        }
    }

    if (dma_rx && data_bytes > 0) {
        uint32_t gdma_chan;
        if (!esp_gdma_get_channel_periph(s->gdma, s->gdma_periph_id,
                                         ESP_GDMA_IN_IDX, &gdma_chan)) {
            return;
        }

        int remaining = data_bytes;
        uint8_t chunk[256];
        while (remaining > 0) {
            int chunk_size = MIN(remaining, (int)sizeof(chunk));
            /* Receive data from SPI */
            esp32s3_gpspi_txrx_buffer(s, NULL, 0, chunk, chunk_size);
            /* Write to GDMA (which writes to memory) */
            esp_gdma_write_channel(s->gdma, gdma_chan, chunk, chunk_size);
            remaining -= chunk_size;
        }
    }
}


static void esp32s3_gpspi_begin_transaction(ESP32S3GpspiState *s)
{
    /* Determine transaction phases from USER register */
    bool has_cmd   = FIELD_EX32(s->user, GPSPI_USER, USR_COMMAND);
    bool has_addr  = FIELD_EX32(s->user, GPSPI_USER, USR_ADDR);
    bool has_dummy = FIELD_EX32(s->user, GPSPI_USER, USR_DUMMY);
    bool has_mosi  = FIELD_EX32(s->user, GPSPI_USER, USR_MOSI);
    bool has_miso  = FIELD_EX32(s->user, GPSPI_USER, USR_MISO);

    /* Data length from the combined MS_DLEN register */
    int data_bits = FIELD_EX32(s->ms_dlen, GPSPI_MS_DLEN, MS_DATA_BITLEN);
    int data_bytes = (data_bits + 1 + 7) / 8;  /* bitlen is bits-minus-1 */

    bool dma_tx = FIELD_EX32(s->dma_conf, GPSPI_DMA_CONF, DMA_TX_ENA);
    bool dma_rx = FIELD_EX32(s->dma_conf, GPSPI_DMA_CONF, DMA_RX_ENA);
    bool use_dma = dma_tx || dma_rx;

    /* Assert CS */
    esp32s3_gpspi_cs_set(s, 0);

    /* Command phase */
    if (has_cmd) {
        uint32_t cmd_val = FIELD_EX32(s->user2, GPSPI_USER2, USR_COMMAND_VALUE);
        int cmd_bits = FIELD_EX32(s->user2, GPSPI_USER2, USR_COMMAND_BITLEN);
        int cmd_bytes = (cmd_bits + 1 + 7) / 8;

        /* Send command MSB-first. For single-byte commands (typical), just
         * send the low byte of cmd_val. */
        uint8_t cmd_buf[4] = {0};
        for (int i = 0; i < cmd_bytes && i < 4; i++) {
            cmd_buf[i] = (cmd_val >> (8 * (cmd_bytes - 1 - i))) & 0xFF;
        }
        esp32s3_gpspi_txrx_buffer(s, cmd_buf, cmd_bytes, NULL, 0);
    }

    /* Address phase */
    if (has_addr) {
        int addr_bits = FIELD_EX32(s->user1, GPSPI_USER1, USR_ADDR_BITLEN);
        int addr_bytes = (addr_bits + 1 + 7) / 8;

        uint32_t addr_val = s->addr;
        uint8_t addr_buf[4] = {0};
        /* MSB first */
        for (int i = 0; i < addr_bytes && i < 4; i++) {
            addr_buf[i] = (addr_val >> (8 * (addr_bytes - 1 - i))) & 0xFF;
        }
        esp32s3_gpspi_txrx_buffer(s, addr_buf, addr_bytes, NULL, 0);
    }

    /* Dummy phase */
    if (has_dummy) {
        int dummy_cycles = FIELD_EX32(s->user1, GPSPI_USER1, USR_DUMMY_CYCLELEN);
        int dummy_bytes = (dummy_cycles + 1 + 7) / 8;
        for (int i = 0; i < dummy_bytes; i++) {
            ssi_transfer(s->spi, 0);
        }
    }

    /* Data phase */
    if ((has_mosi || has_miso) && data_bytes > 0) {
        if (use_dma) {
            /* DMA mode: transfer data via GDMA */
            esp32s3_gpspi_do_dma_transfer(s, data_bytes);
        } else {
            /* CPU mode: data in W0-W15 registers */
            int buf_bytes = MIN(data_bytes,
                                ESP32S3_GPSPI_BUF_WORDS * (int)sizeof(uint32_t));
            uint8_t *buf = (uint8_t *)s->data_reg;

            if (has_mosi && has_miso) {
                /* Full duplex */
                uint8_t rx_buf[ESP32S3_GPSPI_BUF_WORDS * 4];
                esp32s3_gpspi_txrx_buffer(s, buf, buf_bytes,
                                          rx_buf, buf_bytes);
                memcpy(buf, rx_buf, buf_bytes);
            } else if (has_mosi) {
                esp32s3_gpspi_txrx_buffer(s, buf, buf_bytes, NULL, 0);
            } else {
                esp32s3_gpspi_txrx_buffer(s, NULL, 0, buf, buf_bytes);
            }
        }
    }

    /* Deassert CS (unless CS_KEEP_ACTIVE is set) */
    if (!FIELD_EX32(s->misc, GPSPI_MISC, CS_KEEP_ACTIVE)) {
        esp32s3_gpspi_cs_set(s, 1);
    }

    /* Transaction complete — clear USR bit and fire interrupt */
    s->cmd &= ~R_GPSPI_CMD_USR_MASK;
    s->dma_int_raw |= R_GPSPI_DMA_INT_RAW_TRANS_DONE_MASK;
    esp32s3_gpspi_update_irq(s);

#if GPSPI_DEBUG
    info_report("[GPSPI] Transaction complete: cmd=%d addr=%d dummy=%d "
                "mosi=%d miso=%d data_bytes=%d dma=%d",
                has_cmd, has_addr, has_dummy, has_mosi, has_miso,
                data_bytes, use_dma);
#endif
}


static uint64_t esp32s3_gpspi_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3GpspiState *s = ESP32S3_GPSPI(opaque);
    uint64_t r = 0;

    switch (addr) {
    case A_GPSPI_CMD:
        r = s->cmd;
        break;
    case A_GPSPI_ADDR:
        r = s->addr;
        break;
    case A_GPSPI_CTRL:
        r = s->ctrl;
        break;
    case A_GPSPI_CLOCK:
        r = s->clock;
        break;
    case A_GPSPI_USER:
        r = s->user;
        break;
    case A_GPSPI_USER1:
        r = s->user1;
        break;
    case A_GPSPI_USER2:
        r = s->user2;
        break;
    case A_GPSPI_MS_DLEN:
        r = s->ms_dlen;
        break;
    case A_GPSPI_MISC:
        r = s->misc;
        break;
    case A_GPSPI_DIN_MODE:
        r = s->din_mode;
        break;
    case A_GPSPI_DIN_NUM:
        r = s->din_num;
        break;
    case A_GPSPI_DOUT_MODE:
        r = s->dout_mode;
        break;
    case A_GPSPI_DMA_CONF:
        r = s->dma_conf;
        break;
    case A_GPSPI_DMA_INT_ENA:
        r = s->dma_int_ena;
        break;
    case A_GPSPI_DMA_INT_CLR:
        /* Write-only register per TRM, but firmware reads it.
         * Return 0 (no pending clears). */
        r = 0;
        break;
    case A_GPSPI_DMA_INT_RAW:
        r = s->dma_int_raw;
        break;
    case A_GPSPI_DMA_INT_ST:
        /* INT_ST = INT_RAW & INT_ENA */
        r = s->dma_int_raw & s->dma_int_ena;
        break;
    case A_GPSPI_DMA_INT_SET:
        r = 0;
        break;
    case A_GPSPI_W0 ... A_GPSPI_W15:
        r = s->data_reg[(addr - A_GPSPI_W0) / sizeof(uint32_t)];
        break;
    case A_GPSPI_SLAVE:
        r = s->slave;
        break;
    case A_GPSPI_SLAVE1:
        r = s->slave1;
        break;
    case A_GPSPI_CLK_GATE:
        r = s->clk_gate;
        break;
    case A_GPSPI_DATE:
        r = s->date;
        break;
    default:
#if GPSPI_WARNING
        warn_report("[GPSPI] Unsupported read from 0x%lx", (unsigned long)addr);
#endif
        break;
    }

#if GPSPI_DEBUG
    info_report("[GPSPI] Read 0x%02lx => 0x%08lx",
                (unsigned long)addr, (unsigned long)r);
#endif

    return r;
}


static void esp32s3_gpspi_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    ESP32S3GpspiState *s = ESP32S3_GPSPI(opaque);
    uint32_t wval = (uint32_t)value;

#if GPSPI_DEBUG
    info_report("[GPSPI] Write 0x%02lx <= 0x%08lx",
                (unsigned long)addr, (unsigned long)value);
#endif

    switch (addr) {
    case A_GPSPI_CMD:
        s->cmd = wval;

        /* UPDATE (bit 24) and CONF_BITLEN (bit 23) are auto-clear bits.
         * Hardware applies the configuration immediately and clears them.
         * Clear them so firmware polling loops don't spin forever. */
        s->cmd &= ~(R_GPSPI_CMD_UPDATE_MASK | R_GPSPI_CMD_CONF_BITLEN_MASK);

        if (wval & R_GPSPI_CMD_USR_MASK) {
            esp32s3_gpspi_begin_transaction(s);
        }
        break;
    case A_GPSPI_ADDR:
        s->addr = wval;
        break;
    case A_GPSPI_CTRL:
        s->ctrl = wval;
        break;
    case A_GPSPI_CLOCK:
        s->clock = wval;
        break;
    case A_GPSPI_USER:
        s->user = wval;
        break;
    case A_GPSPI_USER1:
        s->user1 = wval;
        break;
    case A_GPSPI_USER2:
        s->user2 = wval;
        break;
    case A_GPSPI_MS_DLEN:
        s->ms_dlen = wval;
        break;
    case A_GPSPI_MISC:
        s->misc = wval;
        break;
    case A_GPSPI_DIN_MODE:
        s->din_mode = wval;
        break;
    case A_GPSPI_DIN_NUM:
        s->din_num = wval;
        break;
    case A_GPSPI_DOUT_MODE:
        s->dout_mode = wval;
        break;
    case A_GPSPI_DMA_CONF:
        s->dma_conf = wval;
        break;
    case A_GPSPI_DMA_INT_ENA:
        s->dma_int_ena = wval;
        esp32s3_gpspi_update_irq(s);
        break;
    case A_GPSPI_DMA_INT_CLR:
        s->dma_int_raw &= ~wval;
        esp32s3_gpspi_update_irq(s);
        break;
    case A_GPSPI_DMA_INT_RAW:
        /* RAW register is typically not written directly, but allow it */
        s->dma_int_raw = wval;
        esp32s3_gpspi_update_irq(s);
        break;
    case A_GPSPI_DMA_INT_SET:
        s->dma_int_raw |= wval;
        esp32s3_gpspi_update_irq(s);
        break;
    case A_GPSPI_W0 ... A_GPSPI_W15:
        s->data_reg[(addr - A_GPSPI_W0) / sizeof(uint32_t)] = wval;
        break;
    case A_GPSPI_SLAVE:
        s->slave = wval;
        break;
    case A_GPSPI_SLAVE1:
        s->slave1 = wval;
        break;
    case A_GPSPI_CLK_GATE:
        s->clk_gate = wval;
        break;
    case A_GPSPI_DATE:
        s->date = wval;
        break;
    default:
#if GPSPI_WARNING
        warn_report("[GPSPI] Unsupported write to 0x%lx (0x%08lx)",
                    (unsigned long)addr, (unsigned long)value);
#endif
        break;
    }
}


static const MemoryRegionOps esp32s3_gpspi_ops = {
    .read  = esp32s3_gpspi_read,
    .write = esp32s3_gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void esp32s3_gpspi_reset_hold(Object *obj, ResetType type)
{
    ESP32S3GpspiState *s = ESP32S3_GPSPI(obj);

    s->cmd = 0;
    s->addr = 0;
    s->ctrl = 0;
    s->clock = 0;
    s->user = 0;
    s->user1 = 0;
    s->user2 = FIELD_DP32(0, GPSPI_USER2, USR_COMMAND_BITLEN, 7);
    s->ms_dlen = 0;
    s->misc = 0x3E;  /* All CS lines disabled by default except CS0 */
    s->din_mode = 0;
    s->din_num = 0;
    s->dout_mode = 0;
    s->dma_conf = 0;
    s->dma_int_ena = 0;
    s->dma_int_raw = 0;
    s->dma_int_clr = 0;
    s->dma_int_set = 0;
    s->slave = 0;
    s->slave1 = 0;
    s->clk_gate = 0;
    s->date = 0x00101900;

    memset(s->data_reg, 0, sizeof(s->data_reg));
}


static void esp32s3_gpspi_realize(DeviceState *dev, Error **errp)
{
}


static void esp32s3_gpspi_init(Object *obj)
{
    ESP32S3GpspiState *s = ESP32S3_GPSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_gpspi_ops, s,
                          TYPE_ESP32S3_GPSPI, ESP32S3_GPSPI_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->spi = ssi_create_bus(DEVICE(s), "spi");
    qdev_init_gpio_out_named(DEVICE(s), &s->cs_gpio[0], SSI_GPIO_CS,
                             ESP32S3_GPSPI_CS_COUNT);

    s->gdma = NULL;
    s->gdma_periph_id = GDMA_SPI2;

    esp32s3_gpspi_reset_hold(obj, RESET_TYPE_COLD);
}


static Property esp32s3_gpspi_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};


static void esp32s3_gpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_gpspi_reset_hold;
    dc->realize = esp32s3_gpspi_realize;
    device_class_set_props(dc, esp32s3_gpspi_properties);
}


static const TypeInfo esp32s3_gpspi_info = {
    .name          = TYPE_ESP32S3_GPSPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3GpspiState),
    .instance_init = esp32s3_gpspi_init,
    .class_init    = esp32s3_gpspi_class_init,
};


static void esp32s3_gpspi_register_types(void)
{
    type_register_static(&esp32s3_gpspi_info);
}

type_init(esp32s3_gpspi_register_types)
