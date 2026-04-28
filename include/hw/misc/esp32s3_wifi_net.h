/*
 * ESP32-S3 WiFi Network Bridge
 *
 * Intercepts ESP-IDF WiFi API calls via ELF symbol patching and provides
 * network connectivity through a QEMU virtual NIC. The firmware calls
 * esp_wifi_connect() and gets real internet — the bridge handles all the
 * WiFi lifecycle management, event delivery, and packet forwarding.
 *
 * Architecture:
 *   Firmware WiFi calls → SIMCALL trampoline → WiFi bridge stubs
 *   Bridge stubs → fire ESP-IDF events via extended trampolines
 *   Bridge stubs → forward packets via QEMU NIC
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
#include "net/net.h"

#define TYPE_ESP32S3_WIFI_NET "misc.esp32s3.wifi_net"
#define ESP32S3_WIFI_NET(obj) \
    OBJECT_CHECK(Esp32s3WifiNetState, (obj), TYPE_ESP32S3_WIFI_NET)

/* WiFi modes (mirrors ESP-IDF wifi_mode_t) */
#define WIFI_MODE_NULL  0
#define WIFI_MODE_STA   1
#define WIFI_MODE_AP    2
#define WIFI_MODE_APSTA 3

/* WiFi states */
#define WIFI_STATE_DEINIT       0
#define WIFI_STATE_INIT         1
#define WIFI_STATE_STARTED      2
#define WIFI_STATE_CONNECTED    3

/* ESP-IDF event IDs */
#define WIFI_EVENT_STA_START            2
#define WIFI_EVENT_STA_STOP             3
#define WIFI_EVENT_STA_CONNECTED        4
#define WIFI_EVENT_STA_DISCONNECTED     5
#define WIFI_EVENT_SCAN_DONE            1

#define IP_EVENT_STA_GOT_IP            0

/* ESP-IDF WiFi interface types */
#define WIFI_IF_STA   0
#define WIFI_IF_AP    1

/* ESP_OK and error codes */
#define ESP_OK              0
#define ESP_ERR_WIFI_NOT_INIT   0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002

/* Mailbox in guest memory for event data and strings */
#define WIFI_MAILBOX_BASE       0x600FFF00
#define WIFI_MAILBOX_SIZE       0x100

/* Offsets within mailbox */
#define WIFI_MB_EVENT_STR       0x00  /* "WIFI_EVENT\0" */
#define WIFI_MB_IP_STR          0x10  /* "IP_EVENT\0" */
#define WIFI_MB_GOT_IP_DATA     0x20  /* ip_event_got_ip_t structure */
#define WIFI_MB_SCAN_LIST       0x40  /* Scan results */
#define WIFI_MB_MAC_ADDR        0x80  /* MAC address */

/* Default IP configuration (QEMU user-mode networking) */
#define WIFI_DEFAULT_IP         0x0F00020A  /* 10.0.2.15 in network order */
#define WIFI_DEFAULT_NETMASK    0x00FFFFFF  /* 255.255.255.0 */
#define WIFI_DEFAULT_GW         0x0200020A  /* 10.0.2.2 */

/* Maximum event handlers we track */
#define WIFI_MAX_EVENT_HANDLERS 8

/* Maximum pending RX packets */
#define WIFI_MAX_RX_QUEUE       16
#define WIFI_MAX_PKT_SIZE       1600

/* Event handler registration entry */
typedef struct WifiEventHandler {
    uint32_t event_base_ptr;    /* Guest pointer to event base string */
    int32_t event_id;           /* Event ID or -1 for any */
    uint32_t handler_addr;      /* Guest function address */
    uint32_t handler_arg;       /* Opaque arg passed to handler */
    bool active;
} WifiEventHandler;

/* Pending RX packet */
typedef struct WifiRxPacket {
    uint8_t data[WIFI_MAX_PKT_SIZE];
    uint16_t len;
    bool pending;
} WifiRxPacket;

/* WiFi configuration stored from set_config */
typedef struct WifiStaConfig {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t bssid[6];
    uint8_t channel;
} WifiStaConfig;

typedef struct Esp32s3WifiNetState {
    SysBusDevice parent_obj;

    /* WiFi state */
    int state;                  /* WIFI_STATE_* */
    int mode;                   /* WIFI_MODE_* */
    uint8_t mac[6];             /* STA MAC address */
    WifiStaConfig sta_config;   /* Station configuration */
    uint8_t channel;            /* Current channel */
    int storage;                /* WiFi config storage type */

    /* Event delivery */
    WifiEventHandler event_handlers[WIFI_MAX_EVENT_HANDLERS];
    int num_event_handlers;

    /* Pending events to deliver via vTaskDelay pump */
    struct {
        uint32_t handler_addr;
        uint32_t handler_arg;
        uint32_t event_base_ptr;
        int32_t event_id;
        uint32_t event_data_ptr;
    } pending_events[4];
    int num_pending_events;

    /* RX callback (registered by firmware via esp_wifi_internal_reg_rxcb) */
    uint32_t rx_cb_addr;        /* Firmware RX callback address */
    uint32_t rx_cb_eb;          /* eb parameter (opaque) */

    /* RX packet queue */
    WifiRxPacket rx_queue[WIFI_MAX_RX_QUEUE];
    int rx_queue_head;
    int rx_queue_tail;

    /* Guest memory addresses for event base strings */
    uint32_t wifi_event_str_addr;   /* Address of "WIFI_EVENT" in guest */
    uint32_t ip_event_str_addr;     /* Address of "IP_EVENT" in guest */
    uint32_t got_ip_data_addr;      /* Address of ip_event_got_ip_t */

    /* ELF symbol addresses (looked up during load) */
    uint64_t vtaskdelay_addr;       /* vTaskDelay function address */

    /* Function addresses for extended trampolines */
    uint64_t esp_wifi_start_addr;
    uint64_t esp_wifi_connect_addr;

    /* Mailbox initialized flag */
    bool mailbox_ready;

    /* NIC for packet I/O */
    NICState *nic;
    NICConf nic_conf;

    /* Reference to CPU */
    void *cpu;

} Esp32s3WifiNetState;

/* Global state */
extern Esp32s3WifiNetState *esp32s3_wifi_net_global;

/*
 * Register all WiFi function hooks with the ELF patching system.
 * Call after creating the WiFi net device and before loading the ELF.
 */
void esp32s3_wifi_net_register_hooks(Esp32s3WifiNetState *s, void *elf_hooks);

/*
 * Initialize the guest memory mailbox with event strings and data structures.
 * Called after ELF loading is complete.
 */
void esp32s3_wifi_net_init_mailbox(Esp32s3WifiNetState *s);

/*
 * Write extended trampolines (larger than 9 bytes) for functions that need
 * to fire events via CALLX8 after SIMCALL.
 * Called after esp32s3_elf_hooks_patch().
 */
void esp32s3_wifi_net_patch_extended(Esp32s3WifiNetState *s,
                                      void *hooks_opaque,
                                      void *cpu_opaque);
