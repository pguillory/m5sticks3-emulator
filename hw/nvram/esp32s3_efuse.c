/*
 * ESP32-S3 eFuse emulation
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/nvram/esp32s3_efuse.h"


static void esp32s3_efuse_realize(DeviceState *dev, Error **errp)
{
    ESP32S3EfuseClass* esp32s3_class = ESP32S3_EFUSE_GET_CLASS(dev);
    ESPEfuseState *s = ESP_EFUSE(dev);

    /* Set PKG_VERSION = 1 (LGA56 / PICO package) in BLOCK1 word 3.
     * The M5StickS3 uses ESP32-S3-PICO-1-N8R8 which is pkg_ver 1.
     * M5Unified / M5GFX reads this to decide which board detection
     * path to follow; pkg_ver 0 (QFN56) leads to wrong detection. */
    esp32s3_class->parent_realize(dev, errp);

    /* Set PKG_VERSION after parent realize to avoid being overwritten.
     * Must also set in the mirror — esp_efuse_reset_hold() reloads from
     * the mirror, which would overwrite our blocks value with zeros. */
    uint32_t val = FIELD_DP32(s->efuses.blocks.rd_mac_spi_sys_3,
                              EFUSE_RD_MAC_SPI_SYS_3, PKG_VERSION, 1);
    s->efuses.blocks.rd_mac_spi_sys_3 = val;
    s->efuses_internal.blocks.rd_mac_spi_sys_3 = val;

    /* Set in the mirror/block-device so resets preserve the value */
    if (s->mirror) {
        ESPEfuseBlocks *mirror_blocks = (ESPEfuseBlocks *)s->mirror;
        mirror_blocks->rd_mac_spi_sys_3 =
            FIELD_DP32(mirror_blocks->rd_mac_spi_sys_3,
                       EFUSE_RD_MAC_SPI_SYS_3, PKG_VERSION, 1);
    }

    warn_report("[EFUSE] Set PKG_VERSION=1 in rd_mac_spi_sys_3=0x%08x",
                s->efuses.blocks.rd_mac_spi_sys_3);
}


static void esp32s3_efuse_init(Object *obj)
{
}

static void esp32s3_efuse_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ESP32S3EfuseClass* esp32s3_efuse = ESP32S3_EFUSE_CLASS(klass);

    device_class_set_parent_realize(dc, esp32s3_efuse_realize, &esp32s3_efuse->parent_realize);
}

static const TypeInfo esp32s3_efuse_info = {
    .name = TYPE_ESP32S3_EFUSE,
    .parent = TYPE_ESP_EFUSE,
    .instance_size = sizeof(ESP32S3EfuseState),
    .instance_init = esp32s3_efuse_init,
    .class_init = esp32s3_efuse_class_init,
    .class_size = sizeof(ESP32S3EfuseClass)
};

static void esp32s3_efuse_register_types(void)
{
    type_register_static(&esp32s3_efuse_info);
}

type_init(esp32s3_efuse_register_types)
