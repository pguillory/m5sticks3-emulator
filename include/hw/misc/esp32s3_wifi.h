/*
 * ESP32-S3 WiFi register stubs
 *
 * Provides stub register regions for WiFi-related peripherals so that
 * firmware accessing WiFi hardware doesn't crash. The emulator uses the
 * OpenCores Ethernet NIC for actual network connectivity instead of
 * real WiFi hardware.
 *
 * Register regions covered:
 *   - WDEV  (0x3ff75000) - WiFi device control (outside main I/O catch-all)
 *   - BB    (0x6001D000) - Baseband
 *   - NRX   (0x6001CC00) - WiFi RX
 *   - FE    (0x60006000) - RF front-end
 *   - FE2   (0x60005000) - RF front-end 2
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ESP32S3_WIFI "misc.esp32s3.wifi"
#define ESP32S3_WIFI(obj) OBJECT_CHECK(Esp32s3WifiState, (obj), TYPE_ESP32S3_WIFI)

/* WDEV register region - WiFi device control block.
 * Located at 0x3ff75000, outside the main 0x60000000 I/O catch-all region.
 * Must be explicitly mapped or accesses will cause a bus fault. */
#define ESP32S3_WDEV_BASE       0x3ff75000
#define ESP32S3_WDEV_SIZE       0x1000

/* Key WDEV register offsets */
#define WDEV_RND_REG            0x144   /* Hardware random number (same as RNG) */

typedef struct Esp32s3WifiState {
    SysBusDevice parent_obj;

    MemoryRegion wdev_iomem;    /* WDEV register region (0x3ff75000) */
} Esp32s3WifiState;
