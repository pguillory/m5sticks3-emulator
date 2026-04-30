/*
 * ESP32-S3 eFuse emulation
 *
 * Copyright (c) 2023-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "esp_efuse.h"

#define TYPE_ESP32S3_EFUSE "nvram.esp32s3.efuse"
#define ESP32S3_EFUSE(obj) OBJECT_CHECK(ESP32S3EfuseState, (obj), TYPE_ESP32S3_EFUSE)
#define ESP32S3_EFUSE_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3EfuseClass, obj, TYPE_ESP32S3_EFUSE)
#define ESP32S3_EFUSE_CLASS(klass) OBJECT_CLASS_CHECK(ESP32S3EfuseClass, klass, TYPE_ESP32S3_EFUSE)

/* ESP32-S3 specific eFuse field in BLOCK1 word 3 (rd_mac_spi_sys_3).
 * Note: EFUSE_RD_MAC_SPI_SYS_3 register offset (0x50) is already defined
 * in the C3 eFuse header which shares the same layout. We only need the
 * S3-specific field definition here. */
FIELD(EFUSE_RD_MAC_SPI_SYS_3, PKG_VERSION, 21, 4)


typedef struct ESP32S3EfuseState {
    ESPEfuseState parent;
} ESP32S3EfuseState;


typedef struct ESP32S3EfuseClass {
    ESPEfuseClass parent_class;
    DeviceRealize parent_realize;
} ESP32S3EfuseClass;

