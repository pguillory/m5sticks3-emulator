/*
 * ESP32-S3 WiFi register stubs
 *
 * This peripheral provides stub implementations for WiFi-related register
 * regions. The ESP32-S3 emulator does not emulate the WiFi hardware (which
 * is driven by a proprietary binary blob in ESP-IDF). Instead, networking
 * is provided through the OpenCores Ethernet virtual NIC at 0x600CD000.
 *
 * Purpose:
 * - Prevent bus faults when firmware touches WiFi registers during init
 * - Return plausible values for key status registers
 * - The WDEV region (0x3ff75000) is outside the main I/O catch-all and
 *   MUST be explicitly mapped
 *
 * Networking approach:
 * The emulator provides an OpenCores 10/100 Ethernet MAC at DR_REG_EMAC_BASE
 * (0x600CD000). Firmware should use ESP-IDF's built-in OpenCores Ethernet
 * driver (esp_eth_mac_openeth) instead of WiFi when running in QEMU.
 * Firmware can detect QEMU by reading the RGB_QEMU_ORIGIN_REG.
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32s3_wifi.h"


#define WIFI_DEBUG 0

#if WIFI_DEBUG
#define WIFI_DPRINTF(fmt, ...) \
    qemu_log("[ESP32S3-WiFi] " fmt, ## __VA_ARGS__)
#else
#define WIFI_DPRINTF(fmt, ...) do {} while (0)
#endif


/*
 * WDEV register read handler.
 *
 * The WDEV block is located at 0x3ff75000, outside the main peripheral
 * I/O catch-all region. Key registers:
 *   - 0x144: Hardware RNG (returns random data)
 *   - Various control/status registers used by the WiFi blob
 *
 * We return 0 for most registers, and random data for the RNG register.
 */
static uint64_t esp32s3_wdev_read(void *opaque, hwaddr addr, unsigned int size)
{
    switch (addr) {
    case WDEV_RND_REG: {
        /* Hardware random number generator - return random data */
        uint32_t r = 0;
        qemu_guest_getrandom_nofail(&r, sizeof(r));
        WIFI_DPRINTF("WDEV RNG read: 0x%08x\n", r);
        return r;
    }
    default:
        WIFI_DPRINTF("WDEV read: offset=0x%03lx size=%d (stubbed=0)\n",
                     (unsigned long)addr, size);
        return 0;
    }
}

static void esp32s3_wdev_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    WIFI_DPRINTF("WDEV write: offset=0x%03lx value=0x%08lx size=%d (ignored)\n",
                 (unsigned long)addr, (unsigned long)value, size);
}

static const MemoryRegionOps esp32s3_wdev_ops = {
    .read = esp32s3_wdev_read,
    .write = esp32s3_wdev_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


static void esp32s3_wifi_init(Object *obj)
{
    Esp32s3WifiState *s = ESP32S3_WIFI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* WDEV register region - must be mapped at 0x3ff75000 */
    memory_region_init_io(&s->wdev_iomem, obj, &esp32s3_wdev_ops, s,
                          TYPE_ESP32S3_WIFI ".wdev", ESP32S3_WDEV_SIZE);
    sysbus_init_mmio(sbd, &s->wdev_iomem);
}

static void esp32s3_wifi_realize(DeviceState *dev, Error **errp)
{
    /* Nothing additional needed at realize time */
}

static void esp32s3_wifi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = esp32s3_wifi_realize;
}

static const TypeInfo esp32s3_wifi_info = {
    .name = TYPE_ESP32S3_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3WifiState),
    .instance_init = esp32s3_wifi_init,
    .class_init = esp32s3_wifi_class_init,
};

static void esp32s3_wifi_register_types(void)
{
    type_register_static(&esp32s3_wifi_info);
}

type_init(esp32s3_wifi_register_types)
