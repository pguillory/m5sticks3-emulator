/*
 * ESP32-S3 BLE Bridge
 *
 * Intercepts NimBLE API calls in the firmware via ELF symbol patching and
 * bridges them to the host's real Bluetooth hardware (CoreBluetooth on macOS).
 *
 * The firmware calls NimBLE functions as normal. At load time, those function
 * entry points are replaced with SIMCALL trampolines. When the firmware calls
 * a NimBLE function, the SIMCALL handler dispatches to the appropriate stub
 * here, which translates the operation into CoreBluetooth calls.
 *
 * Copyright (c) 2024
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
#include "hw/misc/esp32s3_ble.h"
#include "hw/xtensa/esp32s3_elf_hooks.h"
#include "exec/cpu-common.h"

#define BLE_DEBUG 0

#if BLE_DEBUG
#define BLE_DPRINTF(fmt, ...) \
    qemu_log("[ESP32S3-BLE] " fmt, ## __VA_ARGS__)
#else
#define BLE_DPRINTF(fmt, ...) do {} while (0)
#endif

/* Global BLE state */
Esp32s3BleState *esp32s3_ble_global = NULL;


/* ========================================================================
 * NimBLE API stub handlers
 *
 * Each handler is called when the firmware invokes the corresponding
 * NimBLE function. The handler receives the CPUXtensaState and can
 * read arguments from a2-a7 and write the return value to a2.
 * ======================================================================== */

/*
 * nimble_port_init() -> esp_err_t
 * Initialize the NimBLE host stack. We just return 0 (ESP_OK).
 */
static bool handle_nimble_port_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("nimble_port_init() called\n");
    regs->regs[2] = 0;  /* ESP_OK */
    return false;
}

/*
 * esp_nimble_init() -> esp_err_t
 */
static bool handle_esp_nimble_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("esp_nimble_init() called\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * esp_nimble_enable() -> esp_err_t
 * This normally starts the BLE controller. We skip it.
 */
static bool handle_esp_nimble_enable(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("esp_nimble_enable() called (stubbed)\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * esp_nimble_disable() -> esp_err_t
 */
static bool handle_esp_nimble_disable(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("esp_nimble_disable() called (stubbed)\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * esp_nimble_deinit() -> esp_err_t
 */
static bool handle_esp_nimble_deinit(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("esp_nimble_deinit() called (stubbed)\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * nimble_port_freertos_init(nimble_port_freertos_fn fn) -> void
 * Creates a FreeRTOS task to run the NimBLE host event loop.
 * We save the task function address but don't create a real task.
 * The BLE sync callback will be fired when advertising starts.
 */
static bool handle_nimble_port_freertos_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;
    uint32_t task_fn = regs->regs[2];

    BLE_DPRINTF("nimble_port_freertos_init(task_fn=0x%08x) called\n", task_fn);

    /*
     * In real NimBLE, this creates a task that runs nimble_port_run().
     * We don't need the task because we handle events via MMIO polling.
     * Instead, fire the sync callback to signal that the host is ready.
     */
    if (s->sync_cb) {
        BLE_DPRINTF("  Firing ble_hs_cfg.sync_cb at 0x%08" PRIx64 "\n",
                     s->sync_cb);
        /*
         * To fire the sync callback, we modify the CPU state to call it.
         * The SIMCALL will return, RETW will execute, returning to the
         * firmware. We then redirect execution to the sync callback.
         *
         * Actually, for simplicity, we set up a BLE event that will be
         * delivered via the MMIO mechanism. The firmware's main loop
         * should check for BLE events.
         *
         * For now, we just log it. The sync_cb will be called when
         * ble_gap_adv_start is invoked (the firmware calls it from sync_cb).
         */
    }

    /* Return void (no return value needed) */
    return false;
}

/*
 * nimble_port_run() -> void
 * This normally blocks forever processing the NimBLE event queue.
 * Since we handle events through MMIO polling, this just returns.
 * The caller (nimble_host_task) will fall through, which is fine
 * because we don't need a dedicated NimBLE task.
 */
static bool handle_nimble_port_run(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("nimble_port_run() called — returning immediately\n");
    /* Just return. The NimBLE task will exit. */
    return false;
}

/*
 * ble_hs_start() -> int
 */
static bool handle_ble_hs_start(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("ble_hs_start() called\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * ble_svc_gap_init() -> void
 */
static bool handle_ble_svc_gap_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("ble_svc_gap_init() called\n");
    return false;
}

/*
 * ble_svc_gatt_init() -> void
 */
static bool handle_ble_svc_gatt_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("ble_svc_gatt_init() called\n");
    return false;
}

/*
 * ble_svc_gap_device_name_set(const char *name) -> int
 */
static bool handle_ble_svc_gap_device_name_set(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;
    uint32_t name_addr = regs->regs[2];
    CPUState *cs = (CPUState *)regs->cpu_state;

    /* Read the device name from guest memory */
    char name[64] = {0};
    cpu_memory_rw_debug(cs, name_addr, name, sizeof(name) - 1, false);
    name[sizeof(name) - 1] = '\0';

    strncpy(s->device_name, name, sizeof(s->device_name) - 1);
    BLE_DPRINTF("ble_svc_gap_device_name_set(\"%s\")\n", s->device_name);

    regs->regs[2] = 0;
    return false;
}

/*
 * ble_att_set_preferred_mtu(uint16_t mtu) -> int
 */
static bool handle_ble_att_set_preferred_mtu(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;

    s->mtu = (uint16_t)regs->regs[2];
    BLE_DPRINTF("ble_att_set_preferred_mtu(%d)\n", s->mtu);

    regs->regs[2] = 0;
    return false;
}

/*
 * ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs) -> int
 * Counts GATT attribute handles needed. We just return 0 (success).
 */
static bool handle_ble_gatts_count_cfg(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("ble_gatts_count_cfg() called\n");
    regs->regs[2] = 0;
    return false;
}

/*
 * ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs) -> int
 *
 * This is where the firmware registers its GATT services (including NUS).
 * We parse the service definitions from guest memory to capture:
 *   - Service UUID (to identify NUS)
 *   - Characteristic UUIDs, flags, and access callbacks
 *   - The access callback for the RX characteristic
 *
 * NimBLE's ble_gatt_svc_def structure (from ESP-IDF):
 *   struct ble_gatt_svc_def {
 *       uint8_t type;           // 0=end, 1=primary, 2=secondary
 *       const ble_uuid_t *uuid;
 *       const struct ble_gatt_svc_def **includes;
 *       const struct ble_gatt_chr_def *characteristics;
 *   };
 *
 * These are 32-bit pointers on Xtensa (4 bytes each).
 */
static bool handle_ble_gatts_add_svcs(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;
    uint32_t defs_addr = regs->regs[2];

    BLE_DPRINTF("ble_gatts_add_svcs(defs=0x%08x)\n", defs_addr);

    /*
     * Parse the service definitions from guest memory.
     * We need to read the struct layout carefully.
     *
     * For the NUS service, we're looking for:
     *   UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
     *
     * For MVP, we'll just assign attribute handles and capture callbacks
     * without fully parsing the complex nested structures. The firmware
     * typically registers one or two services.
     */

    CPUState *cs = (CPUState *)regs->cpu_state;
    s->next_attr_handle = 1;  /* Start from handle 1 */

    /* Read service definitions (array terminated by type=0) */
    for (int svc_idx = 0; svc_idx < BLE_MAX_SERVICES; svc_idx++) {
        uint32_t svc_addr = defs_addr + svc_idx * 16; /* sizeof(ble_gatt_svc_def) = 16 on 32-bit */
        uint8_t svc_type;
        cpu_memory_rw_debug(cs, svc_addr, &svc_type, 1, false);

        if (svc_type == 0) {
            BLE_DPRINTF("  End of service definitions (svc_idx=%d)\n", svc_idx);
            break;
        }

        BLE_DPRINTF("  Service %d: type=%d\n", svc_idx, svc_type);

        /* Read UUID pointer (offset 4 in struct) */
        uint32_t uuid_ptr;
        cpu_memory_rw_debug(cs, svc_addr + 4, &uuid_ptr, 4, false);
        uuid_ptr = le32_to_cpu(uuid_ptr);

        /* Read UUID type and value */
        if (uuid_ptr) {
            uint8_t uuid_type;
            cpu_memory_rw_debug(cs, uuid_ptr, &uuid_type, 1, false);

            if (uuid_type == 128) {
                /* 128-bit UUID: type(1) + padding(1) + value(16) */
                uint8_t uuid128[16];
                cpu_memory_rw_debug(cs, uuid_ptr + 2, uuid128, 16, false);
                BLE_DPRINTF("  UUID128: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
                            "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                            uuid128[15], uuid128[14], uuid128[13], uuid128[12],
                            uuid128[11], uuid128[10], uuid128[9], uuid128[8],
                            uuid128[7], uuid128[6], uuid128[5], uuid128[4],
                            uuid128[3], uuid128[2], uuid128[1], uuid128[0]);

                if (s->num_services < BLE_MAX_SERVICES) {
                    memcpy(s->services[s->num_services].uuid128, uuid128, 16);
                    s->services[s->num_services].is_uuid128 = true;
                }
            }
        }

        /* Read characteristics pointer (offset 12 in struct) */
        uint32_t chars_ptr;
        cpu_memory_rw_debug(cs, svc_addr + 12, &chars_ptr, 4, false);
        chars_ptr = le32_to_cpu(chars_ptr);

        if (chars_ptr) {
            /*
             * Parse characteristics.
             * struct ble_gatt_chr_def {
             *   const ble_uuid_t *uuid;              // offset 0
             *   ble_gatt_access_fn *access_cb;       // offset 4
             *   void *arg;                            // offset 8
             *   struct ble_gatt_dsc_def *descriptors; // offset 12
             *   ble_gatt_chr_flags flags;             // offset 16 (uint16_t)
             *   uint8_t min_key_size;                 // offset 18
             *   uint16_t *val_handle;                 // offset 20
             * };
             * sizeof = 24 on 32-bit
             */
            for (int chr_idx = 0; chr_idx < BLE_MAX_CHARS; chr_idx++) {
                uint32_t chr_addr = chars_ptr + chr_idx * 24;
                uint32_t chr_uuid_ptr;
                cpu_memory_rw_debug(cs, chr_addr, &chr_uuid_ptr, 4, false);
                chr_uuid_ptr = le32_to_cpu(chr_uuid_ptr);

                if (chr_uuid_ptr == 0) {
                    BLE_DPRINTF("    End of characteristics (chr_idx=%d)\n", chr_idx);
                    break;
                }

                /* Read access callback */
                uint32_t access_cb;
                cpu_memory_rw_debug(cs, chr_addr + 4, &access_cb, 4, false);
                access_cb = le32_to_cpu(access_cb);

                /* Read callback arg */
                uint32_t cb_arg;
                cpu_memory_rw_debug(cs, chr_addr + 8, &cb_arg, 4, false);
                cb_arg = le32_to_cpu(cb_arg);

                /* Read flags */
                uint16_t flags;
                cpu_memory_rw_debug(cs, chr_addr + 16, &flags, 2, false);
                flags = le16_to_cpu(flags);

                /* Read val_handle pointer and write our assigned handle */
                uint32_t val_handle_ptr;
                cpu_memory_rw_debug(cs, chr_addr + 20, &val_handle_ptr, 4, false);
                val_handle_ptr = le32_to_cpu(val_handle_ptr);

                uint16_t handle = s->next_attr_handle++;

                if (val_handle_ptr) {
                    uint16_t h = cpu_to_le16(handle);
                    cpu_memory_rw_debug(cs, val_handle_ptr, &h, 2, true);
                }

                /* Read characteristic UUID to identify NUS RX/TX */
                uint8_t chr_uuid_type;
                cpu_memory_rw_debug(cs, chr_uuid_ptr, &chr_uuid_type, 1, false);

                if (chr_uuid_type == 128) {
                    uint8_t uuid128[16];
                    cpu_memory_rw_debug(cs, chr_uuid_ptr + 2, uuid128, 16, false);

                    BLE_DPRINTF("    Char %d: handle=%d flags=0x%04x "
                                "access_cb=0x%08x uuid=%02x%02x...%02x%02x\n",
                                chr_idx, handle, flags, access_cb,
                                uuid128[15], uuid128[14],
                                uuid128[1], uuid128[0]);

                    /*
                     * NUS RX characteristic UUID:
                     * 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
                     * In little-endian byte order: 9E CA DC 24 0E E5 A9 E0
                     *                              93 F3 A3 B5 02 00 40 6E
                     */
                    static const uint8_t nus_rx_uuid[16] = {
                        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                        0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
                    };
                    static const uint8_t nus_tx_uuid[16] = {
                        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                        0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
                    };

                    if (memcmp(uuid128, nus_rx_uuid, 16) == 0) {
                        s->nus_rx_handle = handle;
                        s->nus_rx_cb = access_cb;
                        s->nus_rx_cb_arg = cb_arg;
                        BLE_DPRINTF("    → NUS RX characteristic "
                                    "(handle=%d, cb=0x%08x)\n",
                                    handle, access_cb);
                    } else if (memcmp(uuid128, nus_tx_uuid, 16) == 0) {
                        s->nus_tx_handle = handle;
                        BLE_DPRINTF("    → NUS TX characteristic "
                                    "(handle=%d)\n", handle);
                    }
                }
            }
        }

        s->num_services++;
    }

    BLE_DPRINTF("  Registered %d services, next_handle=%d\n",
                s->num_services, s->next_attr_handle);

    regs->regs[2] = 0;
    return false;
}

/*
 * ble_gap_adv_start(own_addr_type, direct_addr, duration_ms,
 *                   adv_params, gap_event_cb, cb_arg) -> int
 *
 * Start BLE advertising. This is where we start the CoreBluetooth
 * peripheral and begin advertising the NUS service.
 */
static bool handle_ble_gap_adv_start(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;

    /* Arguments in windowed ABI (after ENTRY):
     * a2 = own_addr_type
     * a3 = direct_addr (pointer, usually NULL)
     * a4 = duration_ms (BLE_HS_FOREVER = INT32_MAX)
     * a5 = adv_params (pointer to struct ble_gap_adv_params)
     * a6 = gap_event_cb (function pointer)
     * a7 = cb_arg
     */
    uint32_t own_addr_type = regs->regs[2];
    uint32_t gap_event_cb = regs->regs[6];
    uint32_t cb_arg = regs->regs[7];

    BLE_DPRINTF("ble_gap_adv_start(addr_type=%d, event_cb=0x%08x, arg=0x%08x)\n",
                own_addr_type, gap_event_cb, cb_arg);

    s->gap_event_cb = gap_event_cb;
    s->gap_event_cb_arg = cb_arg;
    s->status = BLE_STATUS_ADVERTISING;

    /* Start CoreBluetooth advertising */
    if (s->cb_bridge) {
        const char *name = s->device_name[0] ? s->device_name : "ESP32-S3 BLE";
        esp32s3_ble_cb_start_advertising(s->cb_bridge, name);
    } else {
        BLE_DPRINTF("  CoreBluetooth bridge not available, advertising in "
                    "emulation-only mode\n");
    }

    regs->regs[2] = 0;  /* BLE_ERR_SUCCESS */
    return false;
}

/*
 * ble_gap_adv_stop() -> int
 */
static bool handle_ble_gap_adv_stop(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;

    BLE_DPRINTF("ble_gap_adv_stop()\n");

    s->status = BLE_STATUS_IDLE;

    if (s->cb_bridge) {
        esp32s3_ble_cb_stop_advertising(s->cb_bridge);
    }

    regs->regs[2] = 0;
    return false;
}

/*
 * ble_gatts_notify_custom(conn_handle, attr_handle, om) -> int
 *
 * Send a notification on a GATT characteristic (TX direction).
 * The firmware uses this to send data to the connected iOS device.
 *
 * om is an os_mbuf pointer. We need to read the data from it.
 * struct os_mbuf {
 *   struct os_mbuf *om_next;       // offset 0
 *   uint8_t *om_data;              // offset 4
 *   uint16_t om_len;               // offset 8
 *   uint16_t om_flags;             // offset 10
 *   ... (more fields)
 * };
 */
static bool handle_ble_gatts_notify_custom(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3BleState *s = (Esp32s3BleState *)opaque;

    uint16_t conn_handle = (uint16_t)regs->regs[2];
    uint16_t attr_handle = (uint16_t)regs->regs[3];
    uint32_t om_addr = regs->regs[4];

    BLE_DPRINTF("ble_gatts_notify_custom(conn=%d, attr=%d, om=0x%08x)\n",
                conn_handle, attr_handle, om_addr);

    if (om_addr == 0) {
        regs->regs[2] = 0;
        return false;
    }

    /* Read om_data pointer and om_len from the os_mbuf */
    CPUState *cs = (CPUState *)regs->cpu_state;
    uint32_t om_data_ptr;
    uint16_t om_len;

    cpu_memory_rw_debug(cs, om_addr + 4, &om_data_ptr, 4, false);
    om_data_ptr = le32_to_cpu(om_data_ptr);

    cpu_memory_rw_debug(cs, om_addr + 8, &om_len, 2, false);
    om_len = le16_to_cpu(om_len);

    if (om_data_ptr && om_len > 0 && om_len <= BLE_DATA_BUF_SIZE) {
        uint8_t data[BLE_DATA_BUF_SIZE];
        /* Read data from guest memory */
        cpu_memory_rw_debug(cs, om_data_ptr, data, om_len, false);

        BLE_DPRINTF("  TX notification: %d bytes: \"", om_len);
        for (uint16_t i = 0; i < om_len && i < 64; i++) {
            if (data[i] >= 0x20 && data[i] < 0x7f) {
                qemu_log("%c", data[i]);
            } else {
                qemu_log("\\x%02x", data[i]);
            }
        }
        qemu_log("\"\n");

        /* Send through CoreBluetooth */
        esp32s3_ble_send_notification(s, data, om_len);
    }

    regs->regs[2] = 0;
    return false;
}

/*
 * ble_gap_event_listener_register(listener, fn, arg) -> int
 */
static bool handle_ble_gap_event_listener_register(Esp32s3ElfHookRegs *regs,
                                                    void *opaque)
{
    BLE_DPRINTF("ble_gap_event_listener_register(listener=0x%08x, "
                "fn=0x%08x, arg=0x%08x)\n",
                regs->regs[2], regs->regs[3], regs->regs[4]);
    regs->regs[2] = 0;
    return false;
}

/*
 * ble_store_config_set(cfg) -> void
 */
static bool handle_ble_store_config_set(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("ble_store_config_set() called\n");
    return false;
}

/*
 * nimble_port_deinit() -> esp_err_t
 */
static bool handle_nimble_port_deinit(Esp32s3ElfHookRegs *regs, void *opaque)
{
    BLE_DPRINTF("nimble_port_deinit() called\n");
    regs->regs[2] = 0;
    return false;
}


/* ========================================================================
 * MMIO register handlers (for BLE event mailbox)
 * ======================================================================== */

static uint64_t esp32s3_ble_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3BleState *s = ESP32S3_BLE(opaque);

    switch (addr) {
    case BLE_REG_EVENT_FLAG: {
        qemu_mutex_lock(&s->event_lock);
        bool has_event = !QTAILQ_EMPTY(&s->event_queue);
        qemu_mutex_unlock(&s->event_lock);
        return has_event ? 1 : 0;
    }

    case BLE_REG_STATUS:
        return s->status;

    case BLE_REG_MTU:
        return s->mtu;

    case BLE_REG_MAC_ADDR:
        return (s->mac[3] << 24) | (s->mac[2] << 16) |
               (s->mac[1] << 8) | s->mac[0];

    case BLE_REG_MAC_ADDR_HI:
        return (s->mac[5] << 8) | s->mac[4];

    case BLE_REG_VTASKDELAY_ADDR:
        return (uint32_t)s->vtaskdelay_addr;

    default: {
        /* Read-as-written register file for BLE controller hardware registers.
         * The BLE controller init does read-modify-write on config registers;
         * returning 0 causes the LLD to assert. */
        uint32_t idx = addr / sizeof(uint32_t);
        if (idx < ARRAY_SIZE(s->hw_regs)) {
            return s->hw_regs[idx];
        }
        return 0;
    }
    }
}

static void esp32s3_ble_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    Esp32s3BleState *s = ESP32S3_BLE(opaque);

    switch (addr) {
    case BLE_REG_EVENT_ACK:
        /* Acknowledge and dequeue the current event */
        qemu_mutex_lock(&s->event_lock);
        if (!QTAILQ_EMPTY(&s->event_queue)) {
            BleEvent *evt = QTAILQ_FIRST(&s->event_queue);
            QTAILQ_REMOVE(&s->event_queue, evt, next);
            g_free(evt);
        }
        qemu_mutex_unlock(&s->event_lock);
        break;

    default: {
        /* Store to read-as-written register file */
        uint32_t idx = addr / sizeof(uint32_t);
        if (idx < ARRAY_SIZE(s->hw_regs)) {
            s->hw_regs[idx] = (uint32_t)value;
        }
        break;
    }
    }
}

static const MemoryRegionOps esp32s3_ble_ops = {
    .read = esp32s3_ble_read,
    .write = esp32s3_ble_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


/* ========================================================================
 * Event queue management
 * ======================================================================== */

void esp32s3_ble_queue_event(Esp32s3BleState *s, BleEvent *evt)
{
    qemu_mutex_lock(&s->event_lock);
    QTAILQ_INSERT_TAIL(&s->event_queue, evt, next);
    qemu_mutex_unlock(&s->event_lock);
}

void esp32s3_ble_send_notification(Esp32s3BleState *s,
                                    const uint8_t *data, uint32_t len)
{
    if (s->cb_bridge) {
        esp32s3_ble_cb_send_notification(s->cb_bridge, data, len);
    }
}

void esp32s3_ble_on_rx_data(Esp32s3BleState *s,
                             const uint8_t *data, uint32_t len)
{
    BLE_DPRINTF("RX data received: %d bytes\n", len);

    BleEvent *evt = g_new0(BleEvent, 1);
    evt->type = BLE_EVENT_WRITE_RX;
    evt->conn_handle = s->conn_handle;
    evt->attr_handle = s->nus_rx_handle;
    evt->callback_addr = s->nus_rx_cb;
    evt->cb_arg = s->nus_rx_cb_arg;
    if (len > BLE_DATA_BUF_SIZE) len = BLE_DATA_BUF_SIZE;
    memcpy(evt->data, data, len);
    evt->data_len = len;

    esp32s3_ble_queue_event(s, evt);
}

void esp32s3_ble_on_connect(Esp32s3BleState *s, uint16_t conn_handle)
{
    BLE_DPRINTF("BLE connected: conn_handle=%d\n", conn_handle);
    s->conn_handle = conn_handle;
    s->status = BLE_STATUS_CONNECTED;

    BleEvent *evt = g_new0(BleEvent, 1);
    evt->type = BLE_EVENT_CONNECT;
    evt->conn_handle = conn_handle;
    esp32s3_ble_queue_event(s, evt);
}

void esp32s3_ble_on_disconnect(Esp32s3BleState *s, uint16_t conn_handle)
{
    BLE_DPRINTF("BLE disconnected: conn_handle=%d\n", conn_handle);
    s->status = BLE_STATUS_IDLE;
    s->conn_handle = 0;

    BleEvent *evt = g_new0(BleEvent, 1);
    evt->type = BLE_EVENT_DISCONNECT;
    evt->conn_handle = conn_handle;
    esp32s3_ble_queue_event(s, evt);
}

void esp32s3_ble_on_mtu_change(Esp32s3BleState *s, uint16_t mtu)
{
    BLE_DPRINTF("MTU changed: %d\n", mtu);
    s->mtu = mtu;
}


/* ========================================================================
 * CoreBluetooth stub implementations (non-macOS)
 *
 * On macOS, the real implementations are in esp32s3_ble_darwin.m.
 * On other platforms, these stubs are used instead.
 * ======================================================================== */

#ifndef CONFIG_DARWIN
void *esp32s3_ble_cb_init(Esp32s3BleState *ble_state)
{
    return NULL;
}

void esp32s3_ble_cb_start_advertising(void *bridge, const char *name)
{
    (void)bridge;
    (void)name;
}

void esp32s3_ble_cb_stop_advertising(void *bridge)
{
    (void)bridge;
}

void esp32s3_ble_cb_send_notification(void *bridge,
                                       const uint8_t *data, uint32_t len)
{
    (void)bridge;
    (void)data;
    (void)len;
}

void esp32s3_ble_cb_cleanup(void *bridge)
{
    (void)bridge;
}
#endif /* !CONFIG_DARWIN */


/* ========================================================================
 * Device initialization
 * ======================================================================== */

static void esp32s3_ble_init(Object *obj)
{
    Esp32s3BleState *s = ESP32S3_BLE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_ble_ops, s,
                          TYPE_ESP32S3_BLE, ESP32S3_BLE_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    QTAILQ_INIT(&s->event_queue);
    qemu_mutex_init(&s->event_lock);

    /* Generate a consistent MAC address for the emulated device */
    s->mac[0] = 0x30;  /* Espressif OUI prefix */
    s->mac[1] = 0xAE;
    s->mac[2] = 0xA4;
    /* Last 3 bytes from random, but consistent per session */
    uint32_t rnd = 0;
    qemu_guest_getrandom_nofail(&rnd, sizeof(rnd));
    s->mac[3] = (rnd >> 0) & 0xFF;
    s->mac[4] = (rnd >> 8) & 0xFF;
    s->mac[5] = (rnd >> 16) & 0xFF;

    s->mtu = 512;  /* Default preferred MTU */
    s->status = BLE_STATUS_IDLE;
    s->next_attr_handle = 1;
    memset(s->hw_regs, 0, sizeof(s->hw_regs));

    BLE_DPRINTF("BLE device initialized, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                s->mac[0], s->mac[1], s->mac[2],
                s->mac[3], s->mac[4], s->mac[5]);
}

static void esp32s3_ble_realize(DeviceState *dev, Error **errp)
{
    Esp32s3BleState *s = ESP32S3_BLE(dev);

    /* Initialize CoreBluetooth bridge (macOS only) */
#ifdef CONFIG_DARWIN
    s->cb_bridge = esp32s3_ble_cb_init(s);
    if (s->cb_bridge) {
        BLE_DPRINTF("CoreBluetooth bridge initialized\n");
    } else {
        BLE_DPRINTF("CoreBluetooth bridge not available\n");
    }
#else
    s->cb_bridge = NULL;
    BLE_DPRINTF("CoreBluetooth not available (non-macOS platform)\n");
#endif

    esp32s3_ble_global = s;
}

static void esp32s3_ble_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = esp32s3_ble_realize;
}

static const TypeInfo esp32s3_ble_info = {
    .name = TYPE_ESP32S3_BLE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3BleState),
    .instance_init = esp32s3_ble_init,
    .class_init = esp32s3_ble_class_init,
};

static void esp32s3_ble_register_types(void)
{
    type_register_static(&esp32s3_ble_info);
}

type_init(esp32s3_ble_register_types)


/* ========================================================================
 * ELF hook registration
 * ======================================================================== */

/* Table mapping function names to handlers */
static const struct {
    const char *name;
    esp32s3_elf_hook_fn handler;
} ble_hook_table[] = {
    { "nimble_port_init",               handle_nimble_port_init },
    { "nimble_port_freertos_init",      handle_nimble_port_freertos_init },
    { "nimble_port_run",                handle_nimble_port_run },
    { "nimble_port_deinit",             handle_nimble_port_deinit },
    { "ble_svc_gap_init",              handle_ble_svc_gap_init },
    { "ble_svc_gatt_init",            handle_ble_svc_gatt_init },
    { "ble_svc_gap_device_name_set",   handle_ble_svc_gap_device_name_set },
    { "ble_att_set_preferred_mtu",     handle_ble_att_set_preferred_mtu },
    { "ble_gatts_count_cfg",           handle_ble_gatts_count_cfg },
    { "ble_gatts_add_svcs",            handle_ble_gatts_add_svcs },
    { "ble_gap_adv_start",            handle_ble_gap_adv_start },
    { "ble_gap_adv_stop",             handle_ble_gap_adv_stop },
    { "ble_gatts_notify_custom",       handle_ble_gatts_notify_custom },
    { "ble_hs_start",                 handle_ble_hs_start },
    { "ble_gap_event_listener_register", handle_ble_gap_event_listener_register },
    { "ble_store_config_set",          handle_ble_store_config_set },
    { "esp_nimble_enable",             handle_esp_nimble_enable },
    { "esp_nimble_disable",            handle_esp_nimble_disable },
    { "esp_nimble_init",               handle_esp_nimble_init },
    { "esp_nimble_deinit",             handle_esp_nimble_deinit },
};

void esp32s3_ble_register_hooks(Esp32s3BleState *s, void *elf_hooks_opaque)
{
    Esp32s3ElfHooks *hooks = (Esp32s3ElfHooks *)elf_hooks_opaque;

    for (size_t i = 0; i < ARRAY_SIZE(ble_hook_table); i++) {
        esp32s3_elf_hooks_register(hooks, ble_hook_table[i].name,
                                   ble_hook_table[i].handler, s);
    }

    BLE_DPRINTF("Registered %zu NimBLE function hooks\n",
                ARRAY_SIZE(ble_hook_table));
}
