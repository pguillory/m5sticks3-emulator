/*
 * ESP32-S3 BLE Bridge
 *
 * Provides BLE (Bluetooth Low Energy) support for the ESP32-S3 emulator by
 * intercepting NimBLE API calls at the ELF symbol level and bridging them
 * to the host's Bluetooth hardware (CoreBluetooth on macOS).
 *
 * Architecture:
 *   Firmware NimBLE calls → SIMCALL trampoline → BLE bridge → CoreBluetooth
 *   CoreBluetooth events → BLE bridge → MMIO event queue → firmware callbacks
 *
 * The bridge implements a Nordic UART Service (NUS) compatible BLE peripheral
 * that can communicate with iOS/Android apps.
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/queue.h"

#define TYPE_ESP32S3_BLE "misc.esp32s3.ble"
#define ESP32S3_BLE(obj) OBJECT_CHECK(Esp32s3BleState, (obj), TYPE_ESP32S3_BLE)

/* MMIO region at DR_REG_BT_BASE (0x60011000) */
#define ESP32S3_BLE_MMIO_SIZE   0x1000

/*
 * MMIO register offsets for the BLE bridge event mailbox.
 * The nimble_port_run replacement loop polls these registers.
 */
#define BLE_REG_EVENT_FLAG      0x000   /* R: non-zero if event pending */
#define BLE_REG_EVENT_TYPE      0x004   /* R: event type */
#define BLE_REG_EVENT_ACK       0x008   /* W: write 1 to acknowledge event */
#define BLE_REG_CALLBACK_ADDR   0x00C   /* R: callback function address */
#define BLE_REG_CONN_HANDLE     0x010   /* R: connection handle */
#define BLE_REG_ATTR_HANDLE     0x014   /* R: attribute handle */
#define BLE_REG_DATA_PTR        0x018   /* R: pointer to data in emulated RAM */
#define BLE_REG_DATA_LEN        0x01C   /* R: data length */
#define BLE_REG_CB_ARG          0x020   /* R: callback argument */
#define BLE_REG_MTU             0x024   /* R: negotiated MTU */
#define BLE_REG_VTASKDELAY_ADDR 0x040   /* R: address of vTaskDelay (from ELF) */
#define BLE_REG_STATUS          0x100   /* R: BLE status (see BLE_STATUS_*) */
#define BLE_REG_MAC_ADDR        0x104   /* R: BLE MAC address (low 4 bytes) */
#define BLE_REG_MAC_ADDR_HI     0x108   /* R: BLE MAC address (high 2 bytes) */

/* BLE status values */
#define BLE_STATUS_IDLE         0
#define BLE_STATUS_ADVERTISING  1
#define BLE_STATUS_CONNECTED    2

/* BLE event types */
#define BLE_EVENT_NONE          0
#define BLE_EVENT_CONNECT       1
#define BLE_EVENT_DISCONNECT    2
#define BLE_EVENT_MTU           3
#define BLE_EVENT_WRITE_RX      4   /* Data written to RX characteristic */

/* Maximum number of GATT services */
#define BLE_MAX_SERVICES        8
#define BLE_MAX_CHARS           16

/* Data buffer in emulated RAM for BLE data exchange */
#define BLE_DATA_BUF_SIZE       1024

/* NimBLE function IDs (for dispatch in the SIMCALL handler) */
enum {
    BLE_FUNC_NIMBLE_PORT_INIT = 0,
    BLE_FUNC_NIMBLE_PORT_FREERTOS_INIT,
    BLE_FUNC_NIMBLE_PORT_RUN,
    BLE_FUNC_BLE_HS_CFG_STORE,
    BLE_FUNC_BLE_SVC_GAP_INIT,
    BLE_FUNC_BLE_SVC_GATT_INIT,
    BLE_FUNC_BLE_SVC_GAP_DEVICE_NAME_SET,
    BLE_FUNC_BLE_ATT_SET_PREFERRED_MTU,
    BLE_FUNC_BLE_GATTS_COUNT_CFG,
    BLE_FUNC_BLE_GATTS_ADD_SVCS,
    BLE_FUNC_BLE_GAP_ADV_START,
    BLE_FUNC_BLE_GAP_ADV_STOP,
    BLE_FUNC_BLE_GATTS_NOTIFY_CUSTOM,
    BLE_FUNC_BLE_HS_START,
    BLE_FUNC_NimBLE_PORT_DEINIT,
    BLE_FUNC_BLE_GAP_EVENT_LISTENER_REGISTER,
    BLE_FUNC_BLE_STORE_CONFIG_SET,
    BLE_FUNC_ESP_NIMBLE_ENABLE,
    BLE_FUNC_ESP_NIMBLE_DISABLE,
    BLE_FUNC_ESP_NIMBLE_INIT,
    BLE_FUNC_ESP_NIMBLE_DEINIT,
    BLE_FUNC_COUNT
};

/* GATT characteristic info (captured from service registration) */
typedef struct BleCharacteristic {
    uint16_t uuid16;        /* 16-bit UUID (if applicable) */
    uint8_t uuid128[16];    /* 128-bit UUID */
    bool is_uuid128;
    uint16_t flags;
    uint64_t access_cb;     /* Firmware callback address */
    uint64_t arg;           /* Callback argument */
    uint16_t val_handle;    /* Attribute handle assigned by us */
} BleCharacteristic;

/* GATT service info */
typedef struct BleService {
    uint16_t uuid16;
    uint8_t uuid128[16];
    bool is_uuid128;
    BleCharacteristic chars[BLE_MAX_CHARS];
    int num_chars;
} BleService;

/* Pending BLE event */
typedef struct BleEvent {
    int type;
    uint16_t conn_handle;
    uint16_t attr_handle;
    uint8_t data[BLE_DATA_BUF_SIZE];
    uint32_t data_len;
    uint64_t callback_addr;
    uint64_t cb_arg;
    QTAILQ_ENTRY(BleEvent) next;
} BleEvent;

typedef struct Esp32s3BleState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;         /* MMIO region at DR_REG_BT_BASE */

    /* BLE state */
    int status;                 /* BLE_STATUS_* */
    uint16_t conn_handle;       /* Current connection handle */
    uint16_t mtu;               /* Negotiated MTU */
    uint8_t mac[6];             /* BLE MAC address */
    char device_name[64];       /* Advertising name */

    /* GATT services */
    BleService services[BLE_MAX_SERVICES];
    int num_services;
    uint16_t next_attr_handle;

    /* NUS-specific handles */
    uint16_t nus_rx_handle;     /* RX characteristic value handle */
    uint16_t nus_tx_handle;     /* TX characteristic value handle */
    uint64_t nus_rx_cb;         /* RX write callback address */
    uint64_t nus_rx_cb_arg;     /* RX write callback argument */

    /* Event queue for firmware callbacks */
    QTAILQ_HEAD(, BleEvent) event_queue;
    QemuMutex event_lock;

    /* GAP event callback (from firmware) */
    uint64_t gap_event_cb;
    uint64_t gap_event_cb_arg;

    /* ble_hs_cfg.sync_cb address */
    uint64_t sync_cb;

    /* Data buffer in emulated RAM for passing data to callbacks */
    uint32_t data_buf_addr;     /* Guest address of data buffer */

    /* Reference to CPU for register access */
    void *cpu;

    /* CoreBluetooth bridge handle (opaque, platform-specific) */
    void *cb_bridge;

    /* Address of vTaskDelay for yielding */
    uint64_t vtaskdelay_addr;

    /* Flag: has nimble_port_run been entered? */
    bool nimble_running;

} Esp32s3BleState;

/* Global BLE state (one per machine) */
extern Esp32s3BleState *esp32s3_ble_global;

/*
 * Register all NimBLE function hooks with the ELF patching system.
 * Call after creating the BLE device and before loading the ELF.
 */
void esp32s3_ble_register_hooks(Esp32s3BleState *s, void *elf_hooks);

/*
 * Queue a BLE event for delivery to the firmware.
 * Can be called from any thread (CoreBluetooth callbacks run on main thread).
 */
void esp32s3_ble_queue_event(Esp32s3BleState *s, BleEvent *evt);

/*
 * Send a notification on the TX characteristic.
 * Called from NimBLE stub when firmware calls ble_gatts_notify_custom().
 */
void esp32s3_ble_send_notification(Esp32s3BleState *s,
                                    const uint8_t *data, uint32_t len);

/*
 * Called by CoreBluetooth bridge when data is received on RX characteristic.
 */
void esp32s3_ble_on_rx_data(Esp32s3BleState *s,
                             const uint8_t *data, uint32_t len);

/*
 * Called by CoreBluetooth bridge on connection/disconnection events.
 */
void esp32s3_ble_on_connect(Esp32s3BleState *s, uint16_t conn_handle);
void esp32s3_ble_on_disconnect(Esp32s3BleState *s, uint16_t conn_handle);
void esp32s3_ble_on_mtu_change(Esp32s3BleState *s, uint16_t mtu);

/* ---- CoreBluetooth bridge API (implemented in esp32s3_ble_darwin.m) ---- */

/*
 * Initialize the CoreBluetooth bridge.
 * Returns an opaque handle to the bridge.
 */
void *esp32s3_ble_cb_init(Esp32s3BleState *ble_state);

/*
 * Start advertising as a BLE peripheral with NUS service.
 */
void esp32s3_ble_cb_start_advertising(void *bridge, const char *name);

/*
 * Stop advertising.
 */
void esp32s3_ble_cb_stop_advertising(void *bridge);

/*
 * Send a notification on the TX characteristic.
 */
void esp32s3_ble_cb_send_notification(void *bridge,
                                       const uint8_t *data, uint32_t len);

/*
 * Cleanup the CoreBluetooth bridge.
 */
void esp32s3_ble_cb_cleanup(void *bridge);
