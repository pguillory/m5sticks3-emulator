/*
 * ESP32-S3 WiFi Network Bridge
 *
 * Intercepts ESP-IDF WiFi API calls in the firmware via ELF symbol patching
 * and bridges them to a QEMU virtual NIC for real network connectivity.
 *
 * The firmware calls WiFi functions as normal (esp_wifi_init, esp_wifi_start,
 * esp_wifi_connect, etc.). At load time, those function entry points are
 * replaced with SIMCALL trampolines. When the firmware calls a WiFi function,
 * the SIMCALL handler dispatches to the appropriate stub here.
 *
 * Event delivery uses "extended trampolines" — after SIMCALL returns, the
 * trampoline code calls the firmware's registered event handler via CALLX8,
 * so events like WIFI_EVENT_STA_CONNECTED fire synchronously.
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
#include "hw/misc/esp32s3_wifi_net.h"
#include "hw/xtensa/esp32s3_elf_hooks.h"
#include "exec/cpu-common.h"
#include "net/net.h"

#define WIFI_NET_DEBUG 1

#if WIFI_NET_DEBUG
#define WIFI_DPRINTF(fmt, ...) \
    qemu_log("[ESP32S3-WiFi-Net] " fmt, ## __VA_ARGS__)
#else
#define WIFI_DPRINTF(fmt, ...) do {} while (0)
#endif

/* Global WiFi net state */
Esp32s3WifiNetState *esp32s3_wifi_net_global = NULL;

/*
 * Extended trampoline bytes for functions that fire events.
 *
 * Single-event trampoline (15 bytes):
 *   ENTRY a1, 48     (0x36, 0x61, 0x00)
 *   SIMCALL          (0x00, 0x51, 0x00)
 *   RETW             (0x90, 0x00, 0x00)  -- taken if no event
 *   CALLX8 a8       (0x00, 0xE8, 0x00)  -- fire event handler
 *   RETW             (0x90, 0x00, 0x00)  -- return after event
 *
 * Double-event trampoline (33 bytes) for esp_wifi_connect:
 *   ENTRY a1, 48     (0x36, 0x61, 0x00)
 *   SIMCALL          (0x00, 0x51, 0x00)
 *   RETW             (0x90, 0x00, 0x00)  -- taken if no event
 *   CALLX8 a8       (0x00, 0xE8, 0x00)  -- fire first event
 *   OR a8, a3, a3   (0x30, 0x83, 0x20)  -- mov a8, a3 (2nd handler)
 *   OR a10, a4, a4  (0x40, 0xA4, 0x20)  -- mov a10, a4
 *   OR a11, a5, a5  (0x50, 0xB5, 0x20)  -- mov a11, a5
 *   OR a12, a6, a6  (0x60, 0xC6, 0x20)  -- mov a12, a6
 *   OR a13, a7, a7  (0x70, 0xD7, 0x20)  -- mov a13, a7
 *   CALLX8 a8       (0x00, 0xE8, 0x00)  -- fire second event
 *   RETW             (0x90, 0x00, 0x00)  -- return after both events
 */

static const uint8_t single_event_trampoline[] = {
    0x36, 0x61, 0x00,  /* entry a1, 48 */
    0x00, 0x51, 0x00,  /* simcall */
    0x90, 0x00, 0x00,  /* retw (no-event path) */
    0x00, 0xE8, 0x00,  /* callx8 a8 */
    0x90, 0x00, 0x00,  /* retw */
};

static const uint8_t double_event_trampoline[] = {
    0x36, 0x61, 0x00,  /* entry a1, 48 */
    0x00, 0x51, 0x00,  /* simcall */
    0x90, 0x00, 0x00,  /* retw (no-event path) */
    0x00, 0xE8, 0x00,  /* callx8 a8 (first event) */
    0x30, 0x83, 0x20,  /* or a8, a3, a3 */
    0x40, 0xA4, 0x20,  /* or a10, a4, a4 */
    0x50, 0xB5, 0x20,  /* or a11, a5, a5 */
    0x60, 0xC6, 0x20,  /* or a12, a6, a6 */
    0x70, 0xD7, 0x20,  /* or a13, a7, a7 */
    0x00, 0xE8, 0x00,  /* callx8 a8 (second event) */
    0x90, 0x00, 0x00,  /* retw */
};


/* ========================================================================
 * Helper functions
 * ======================================================================== */

/*
 * Find a registered event handler matching the given event base and ID.
 * Returns the handler entry, or NULL if not found.
 */
static WifiEventHandler *find_event_handler(Esp32s3WifiNetState *s,
                                             uint32_t event_base_ptr,
                                             int32_t event_id)
{
    for (int i = 0; i < s->num_event_handlers; i++) {
        WifiEventHandler *h = &s->event_handlers[i];
        if (!h->active) continue;

        /* Match event base (pointer comparison) */
        if (h->event_base_ptr != event_base_ptr) continue;

        /* Match event ID: -1 (ESP_EVENT_ANY_ID) matches everything */
        if (h->event_id == -1 || h->event_id == event_id) {
            return h;
        }
    }
    return NULL;
}


/* ========================================================================
 * WiFi API stub handlers
 * ======================================================================== */

/*
 * esp_wifi_init_internal(wifi_init_config_t *config) -> esp_err_t
 * (May also be exported as esp_wifi_init)
 */
static bool handle_esp_wifi_init(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_init() called\n");

    s->state = WIFI_STATE_INIT;
    s->mode = WIFI_MODE_STA;  /* Default mode */

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_deinit_internal() -> esp_err_t
 */
static bool handle_esp_wifi_deinit(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_deinit() called\n");

    s->state = WIFI_STATE_DEINIT;

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_start() -> esp_err_t
 *
 * Uses single-event extended trampoline to fire WIFI_EVENT_STA_START.
 */
static bool handle_esp_wifi_start(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_start() called (state=%d)\n", s->state);

    s->state = WIFI_STATE_STARTED;
    regs->regs[2] = ESP_OK;

    /* Find handler for WIFI_EVENT / STA_START */
    WifiEventHandler *h = find_event_handler(s, s->wifi_event_str_addr,
                                              WIFI_EVENT_STA_START);
    if (h && s->mailbox_ready) {
        WIFI_DPRINTF("  Firing WIFI_EVENT_STA_START via extended trampoline "
                     "(handler=0x%08x)\n", h->handler_addr);
        /* Set up registers for CALLX8 */
        regs->regs[8] = h->handler_addr;   /* CALLX8 target */
        regs->regs[10] = h->handler_arg;   /* → new a2: handler_arg */
        regs->regs[11] = s->wifi_event_str_addr; /* → new a3: event_base */
        regs->regs[12] = WIFI_EVENT_STA_START;   /* → new a4: event_id */
        regs->regs[13] = 0;                /* → new a5: event_data (NULL) */

        /* Redirect to CALLX8 instruction (offset 9 in extended trampoline) */
        regs->new_pc = s->esp_wifi_start_addr + 9;
        return true;  /* cpu_loop_exit to redirect PC */
    }

    return false;  /* Normal RETW */
}

/*
 * esp_wifi_stop() -> esp_err_t
 */
static bool handle_esp_wifi_stop(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_stop() called\n");

    s->state = WIFI_STATE_INIT;

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_connect() -> esp_err_t
 *
 * Uses double-event extended trampoline to fire both:
 *   1. WIFI_EVENT_STA_CONNECTED
 *   2. IP_EVENT_STA_GOT_IP
 */
static bool handle_esp_wifi_connect(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_connect() called (ssid=\"%.32s\")\n",
                 s->sta_config.ssid);

    s->state = WIFI_STATE_CONNECTED;
    regs->regs[2] = ESP_OK;

    /* Find handlers for both events */
    WifiEventHandler *h_conn = find_event_handler(s, s->wifi_event_str_addr,
                                                   WIFI_EVENT_STA_CONNECTED);
    WifiEventHandler *h_ip = find_event_handler(s, s->ip_event_str_addr,
                                                 IP_EVENT_STA_GOT_IP);

    if (h_conn && s->mailbox_ready) {
        WIFI_DPRINTF("  Firing WIFI_EVENT_STA_CONNECTED + IP_EVENT_STA_GOT_IP\n");

        /* First event (CONNECTED) - goes in a8, a10-a13 */
        regs->regs[8] = h_conn->handler_addr;
        regs->regs[10] = h_conn->handler_arg;
        regs->regs[11] = s->wifi_event_str_addr;
        regs->regs[12] = WIFI_EVENT_STA_CONNECTED;
        regs->regs[13] = 0;  /* no event data */

        /* Second event (GOT_IP) - saved in a3-a7 for MOV after first CALLX8 */
        if (h_ip) {
            regs->regs[3] = h_ip->handler_addr;
            regs->regs[4] = h_ip->handler_arg;
            regs->regs[5] = s->ip_event_str_addr;
            regs->regs[6] = IP_EVENT_STA_GOT_IP;
            regs->regs[7] = s->got_ip_data_addr;
        } else {
            /* No IP handler - use same handler for second call (it'll ignore) */
            regs->regs[3] = h_conn->handler_addr;
            regs->regs[4] = h_conn->handler_arg;
            regs->regs[5] = s->ip_event_str_addr;
            regs->regs[6] = IP_EVENT_STA_GOT_IP;
            regs->regs[7] = s->got_ip_data_addr;
        }

        /* Redirect to first CALLX8 (offset 9 in double-event trampoline) */
        regs->new_pc = s->esp_wifi_connect_addr + 9;
        return true;
    } else if (h_conn) {
        WIFI_DPRINTF("  WARNING: mailbox not ready, events not fired\n");
    }

    return false;
}

/*
 * esp_wifi_disconnect() -> esp_err_t
 */
static bool handle_esp_wifi_disconnect(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    WIFI_DPRINTF("esp_wifi_disconnect() called\n");

    if (s->state == WIFI_STATE_CONNECTED) {
        s->state = WIFI_STATE_STARTED;
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_scan_start(wifi_scan_config_t *config, bool block) -> esp_err_t
 */
static bool handle_esp_wifi_scan_start(Esp32s3ElfHookRegs *regs, void *opaque)
{
    WIFI_DPRINTF("esp_wifi_scan_start() called\n");
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_scan_get_ap_num(uint16_t *number) -> esp_err_t
 */
static bool handle_esp_wifi_scan_get_ap_num(Esp32s3ElfHookRegs *regs,
                                             void *opaque)
{
    uint32_t num_ptr = regs->regs[2];
    CPUState *cs = (CPUState *)regs->cpu_state;

    WIFI_DPRINTF("esp_wifi_scan_get_ap_num(ptr=0x%08x)\n", num_ptr);

    /* Return 1 AP found */
    if (num_ptr) {
        uint16_t num = 1;
        cpu_memory_rw_debug(cs, num_ptr, (uint8_t *)&num, 2, true);
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
 *   -> esp_err_t
 */
static bool handle_esp_wifi_scan_get_ap_records(Esp32s3ElfHookRegs *regs,
                                                 void *opaque)
{
    uint32_t num_ptr = regs->regs[2];
    uint32_t records_ptr = regs->regs[3];
    CPUState *cs = (CPUState *)regs->cpu_state;

    WIFI_DPRINTF("esp_wifi_scan_get_ap_records(num=0x%08x, rec=0x%08x)\n",
                 num_ptr, records_ptr);

    /* Return 1 fake AP record */
    if (num_ptr && records_ptr) {
        uint16_t num = 1;
        cpu_memory_rw_debug(cs, num_ptr, (uint8_t *)&num, 2, true);

        /* wifi_ap_record_t is 80 bytes. Write a plausible entry. */
        uint8_t record[80] = {0};
        /* bssid (offset 0, 6 bytes) */
        record[0] = 0xAA; record[1] = 0xBB; record[2] = 0xCC;
        record[3] = 0xDD; record[4] = 0xEE; record[5] = 0xFF;
        /* ssid (offset 6, 33 bytes) */
        memcpy(record + 6, "QEMU-WiFi", 9);
        /* primary channel (offset 39, 1 byte) */
        record[39] = 6;
        /* rssi (offset 41, int8_t) */
        record[41] = (uint8_t)-50;  /* -50 dBm, good signal */
        /* authmode (offset 42, 1 byte) - WIFI_AUTH_WPA2_PSK = 3 */
        record[42] = 3;

        cpu_memory_rw_debug(cs, records_ptr, record, sizeof(record), true);
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_set_mode(wifi_mode_t mode) -> esp_err_t
 */
static bool handle_esp_wifi_set_mode(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    s->mode = (int)regs->regs[2];
    WIFI_DPRINTF("esp_wifi_set_mode(%d)\n", s->mode);

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_get_mode(wifi_mode_t *mode) -> esp_err_t
 */
static bool handle_esp_wifi_get_mode(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t mode_ptr = regs->regs[2];
    CPUState *cs = (CPUState *)regs->cpu_state;

    if (mode_ptr) {
        uint32_t mode = s->mode;
        cpu_memory_rw_debug(cs, mode_ptr, (uint8_t *)&mode, 4, true);
    }

    WIFI_DPRINTF("esp_wifi_get_mode() -> %d\n", s->mode);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
 *   -> esp_err_t
 */
static bool handle_esp_wifi_set_config(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t iface = regs->regs[2];
    uint32_t conf_ptr = regs->regs[3];
    CPUState *cs = (CPUState *)regs->cpu_state;

    WIFI_DPRINTF("esp_wifi_set_config(iface=%d, conf=0x%08x)\n",
                 iface, conf_ptr);

    if (conf_ptr && iface == WIFI_IF_STA) {
        /* Read SSID (first 32 bytes of wifi_config_t.sta) */
        cpu_memory_rw_debug(cs, conf_ptr, s->sta_config.ssid, 32, false);
        /* Read password (next 64 bytes) */
        cpu_memory_rw_debug(cs, conf_ptr + 32, s->sta_config.password, 64,
                            false);
        WIFI_DPRINTF("  STA config: ssid=\"%.32s\" pass=\"%.8s...\"\n",
                     s->sta_config.ssid, s->sta_config.password);
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf)
 *   -> esp_err_t
 */
static bool handle_esp_wifi_get_config(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t iface = regs->regs[2];
    uint32_t conf_ptr = regs->regs[3];
    CPUState *cs = (CPUState *)regs->cpu_state;

    WIFI_DPRINTF("esp_wifi_get_config(iface=%d)\n", iface);

    if (conf_ptr && iface == WIFI_IF_STA) {
        /* Write back stored config */
        cpu_memory_rw_debug(cs, conf_ptr, s->sta_config.ssid, 32, true);
        cpu_memory_rw_debug(cs, conf_ptr + 32, s->sta_config.password, 64,
                            true);
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_set_storage(wifi_storage_t storage) -> esp_err_t
 */
static bool handle_esp_wifi_set_storage(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    s->storage = (int)regs->regs[2];
    WIFI_DPRINTF("esp_wifi_set_storage(%d)\n", s->storage);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second)
 *   -> esp_err_t
 */
static bool handle_esp_wifi_set_channel(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    s->channel = (uint8_t)regs->regs[2];
    WIFI_DPRINTF("esp_wifi_set_channel(%d)\n", s->channel);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
 *   -> esp_err_t
 */
static bool handle_esp_wifi_get_channel(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t primary_ptr = regs->regs[2];
    CPUState *cs = (CPUState *)regs->cpu_state;

    if (primary_ptr) {
        uint8_t ch = s->channel ? s->channel : 6;
        cpu_memory_rw_debug(cs, primary_ptr, &ch, 1, true);
    }

    WIFI_DPRINTF("esp_wifi_get_channel() -> %d\n",
                 s->channel ? s->channel : 6);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6]) -> esp_err_t
 */
static bool handle_esp_wifi_get_mac(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t mac_ptr = regs->regs[3];
    CPUState *cs = (CPUState *)regs->cpu_state;

    if (mac_ptr) {
        cpu_memory_rw_debug(cs, mac_ptr, s->mac, 6, true);
    }

    WIFI_DPRINTF("esp_wifi_get_mac() -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                 s->mac[0], s->mac[1], s->mac[2],
                 s->mac[3], s->mac[4], s->mac[5]);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_set_mac(wifi_interface_t ifx, const uint8_t mac[6]) -> esp_err_t
 */
static bool handle_esp_wifi_set_mac(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t mac_ptr = regs->regs[3];
    CPUState *cs = (CPUState *)regs->cpu_state;

    if (mac_ptr) {
        cpu_memory_rw_debug(cs, mac_ptr, s->mac, 6, false);
    }

    WIFI_DPRINTF("esp_wifi_set_mac(%02X:%02X:%02X:%02X:%02X:%02X)\n",
                 s->mac[0], s->mac[1], s->mac[2],
                 s->mac[3], s->mac[4], s->mac[5]);
    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) -> esp_err_t
 */
static bool handle_esp_wifi_sta_get_ap_info(Esp32s3ElfHookRegs *regs,
                                             void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t info_ptr = regs->regs[2];
    CPUState *cs = (CPUState *)regs->cpu_state;

    WIFI_DPRINTF("esp_wifi_sta_get_ap_info()\n");

    if (info_ptr && s->state == WIFI_STATE_CONNECTED) {
        uint8_t record[80] = {0};
        record[0] = 0xAA; record[1] = 0xBB; record[2] = 0xCC;
        record[3] = 0xDD; record[4] = 0xEE; record[5] = 0xFF;
        memcpy(record + 6, s->sta_config.ssid, 32);
        record[39] = s->channel ? s->channel : 6;
        record[41] = (uint8_t)-45;
        record[42] = 3;  /* WPA2 */
        cpu_memory_rw_debug(cs, info_ptr, record, sizeof(record), true);
    }

    regs->regs[2] = (s->state == WIFI_STATE_CONNECTED) ? ESP_OK : 0x3007;
    return false;
}

/*
 * esp_wifi_get_state() -> wifi_state_t (non-standard, some firmwares use it)
 */
static bool handle_esp_wifi_get_state(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    WIFI_DPRINTF("esp_wifi_get_state() -> %d\n", s->state);
    regs->regs[2] = s->state;
    return false;
}


/* ========================================================================
 * Data path handlers (packet TX/RX)
 * ======================================================================== */

/*
 * esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len)
 *   -> esp_err_t
 *
 * Called by lwIP to send an Ethernet frame through WiFi.
 * We forward it through the QEMU NIC.
 */
static bool handle_esp_wifi_internal_tx(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t buffer_addr = regs->regs[3];
    uint16_t len = (uint16_t)regs->regs[4];
    CPUState *cs = (CPUState *)regs->cpu_state;

    if (buffer_addr && len > 0 && len <= WIFI_MAX_PKT_SIZE) {
        uint8_t pkt[WIFI_MAX_PKT_SIZE];
        cpu_memory_rw_debug(cs, buffer_addr, pkt, len, false);

        if (s->nic) {
            qemu_send_packet(qemu_get_queue(s->nic), pkt, len);
        }
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_internal_reg_rxcb(wifi_interface_t ifx,
 *                            wifi_rxcb_t fn) -> esp_err_t
 *
 * Registers the callback that receives incoming packets.
 */
static bool handle_esp_wifi_internal_reg_rxcb(Esp32s3ElfHookRegs *regs,
                                               void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t cb_addr = regs->regs[3];

    WIFI_DPRINTF("esp_wifi_internal_reg_rxcb(cb=0x%08x)\n", cb_addr);
    s->rx_cb_addr = cb_addr;

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_wifi_internal_free_rx_buffer(void *buffer) -> void
 *
 * Frees a buffer previously delivered via the RX callback.
 * Since we manage our own buffers, this is a no-op.
 */
static bool handle_esp_wifi_internal_free_rx_buffer(Esp32s3ElfHookRegs *regs,
                                                     void *opaque)
{
    /* No-op: our RX buffers are managed internally */
    return false;
}


/* ========================================================================
 * Event system hooks
 * ======================================================================== */

/*
 * esp_event_handler_instance_register(esp_event_base_t event_base,
 *                                      int32_t event_id,
 *                                      esp_event_handler_t event_handler,
 *                                      void *event_handler_arg,
 *                                      esp_event_handler_instance_t *instance)
 *   -> esp_err_t
 *
 * We capture event handler registrations so we can call them directly
 * when firing WiFi/IP events.
 */
static bool handle_esp_event_handler_instance_register(
    Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;
    uint32_t event_base_ptr = regs->regs[2];
    int32_t event_id = (int32_t)regs->regs[3];
    uint32_t handler_addr = regs->regs[4];
    uint32_t handler_arg = regs->regs[5];

    WIFI_DPRINTF("esp_event_handler_instance_register("
                 "base=0x%08x, id=%d, handler=0x%08x, arg=0x%08x)\n",
                 event_base_ptr, event_id, handler_addr, handler_arg);

    /* Store the event handler registration */
    if (s->num_event_handlers < WIFI_MAX_EVENT_HANDLERS) {
        WifiEventHandler *h = &s->event_handlers[s->num_event_handlers];
        h->event_base_ptr = event_base_ptr;
        h->event_id = event_id;
        h->handler_addr = handler_addr;
        h->handler_arg = handler_arg;
        h->active = true;
        s->num_event_handlers++;

        /* Check if this is a WIFI_EVENT or IP_EVENT handler by reading
         * the string that event_base_ptr points to */
        CPUState *cs = (CPUState *)regs->cpu_state;
        char base_name[16] = {0};
        cpu_memory_rw_debug(cs, event_base_ptr, (uint8_t *)base_name,
                            sizeof(base_name) - 1, false);

        if (strcmp(base_name, "WIFI_EVENT") == 0) {
            s->wifi_event_str_addr = event_base_ptr;
            WIFI_DPRINTF("  → Captured WIFI_EVENT handler\n");
        } else if (strcmp(base_name, "IP_EVENT") == 0) {
            s->ip_event_str_addr = event_base_ptr;
            WIFI_DPRINTF("  → Captured IP_EVENT handler\n");
        }
    }

    regs->regs[2] = ESP_OK;
    return false;
}

/*
 * esp_event_handler_register(esp_event_base_t event_base,
 *                             int32_t event_id,
 *                             esp_event_handler_t event_handler,
 *                             void *event_handler_arg)
 *   -> esp_err_t
 *
 * Older API without instance. Same capture logic.
 */
static bool handle_esp_event_handler_register(Esp32s3ElfHookRegs *regs,
                                               void *opaque)
{
    /* Same argument layout minus the instance parameter */
    return handle_esp_event_handler_instance_register(regs, opaque);
}


/* ========================================================================
 * vTaskDelay hook for event/packet pump
 * ======================================================================== */

/*
 * vTaskDelay(TickType_t xTicksToDelay) -> void
 *
 * Hooked to pump pending events and RX packets. When there are pending
 * items to deliver, we redirect to the extended trampoline's CALLX8.
 * When idle, we just return immediately (no actual delay in emulation).
 */
static bool handle_vTaskDelay(Esp32s3ElfHookRegs *regs, void *opaque)
{
    Esp32s3WifiNetState *s = (Esp32s3WifiNetState *)opaque;

    /* Check for pending events */
    if (s->num_pending_events > 0) {
        int idx = s->num_pending_events - 1;
        WIFI_DPRINTF("vTaskDelay: pumping pending event %d\n", idx);

        regs->regs[8] = s->pending_events[idx].handler_addr;
        regs->regs[10] = s->pending_events[idx].handler_arg;
        regs->regs[11] = s->pending_events[idx].event_base_ptr;
        regs->regs[12] = s->pending_events[idx].event_id;
        regs->regs[13] = s->pending_events[idx].event_data_ptr;
        s->num_pending_events--;

        /* Redirect to CALLX8 in extended trampoline (offset 9) */
        regs->new_pc = s->vtaskdelay_addr + 9;
        return true;
    }

    /* No pending work, just return immediately */
    return false;
}


/* ========================================================================
 * NIC callbacks (for packet reception from host network)
 * ======================================================================== */

static bool wifi_net_can_receive(NetClientState *nc)
{
    return true;  /* Always ready to receive */
}

static ssize_t wifi_net_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    Esp32s3WifiNetState *s = qemu_get_nic_opaque(nc);

    if (!s || !s->rx_cb_addr || size > WIFI_MAX_PKT_SIZE) {
        return size;  /* Drop packet */
    }

    /* Queue the packet for delivery via vTaskDelay pump */
    int next_tail = (s->rx_queue_tail + 1) % WIFI_MAX_RX_QUEUE;
    if (next_tail == s->rx_queue_head) {
        /* Queue full, drop */
        return size;
    }

    WifiRxPacket *pkt = &s->rx_queue[s->rx_queue_tail];
    memcpy(pkt->data, buf, size);
    pkt->len = size;
    pkt->pending = true;
    s->rx_queue_tail = next_tail;

    return size;
}

static void wifi_net_link_status_changed(NetClientState *nc)
{
    /* Nothing to do */
}

static NetClientInfo wifi_net_client_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = wifi_net_can_receive,
    .receive = wifi_net_receive,
    .link_status_changed = wifi_net_link_status_changed,
};


/* ========================================================================
 * Device initialization
 * ======================================================================== */

static void esp32s3_wifi_net_init(Object *obj)
{
    Esp32s3WifiNetState *s = ESP32S3_WIFI_NET(obj);

    /* Generate a consistent MAC address */
    s->mac[0] = 0x30;  /* Espressif OUI */
    s->mac[1] = 0xAE;
    s->mac[2] = 0xA4;
    uint32_t rnd = 0;
    qemu_guest_getrandom_nofail(&rnd, sizeof(rnd));
    s->mac[3] = (rnd >> 0) & 0xFF;
    s->mac[4] = (rnd >> 8) & 0xFF;
    s->mac[5] = (rnd >> 16) & 0xFF;

    s->state = WIFI_STATE_DEINIT;
    s->mode = WIFI_MODE_NULL;
    s->channel = 6;

    WIFI_DPRINTF("WiFi Net bridge initialized, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                 s->mac[0], s->mac[1], s->mac[2],
                 s->mac[3], s->mac[4], s->mac[5]);
}

static void esp32s3_wifi_net_realize(DeviceState *dev, Error **errp)
{
    Esp32s3WifiNetState *s = ESP32S3_WIFI_NET(dev);

    /* Set up MAC address in NIC config */
    s->nic_conf.macaddr.a[0] = s->mac[0];
    s->nic_conf.macaddr.a[1] = s->mac[1];
    s->nic_conf.macaddr.a[2] = s->mac[2];
    s->nic_conf.macaddr.a[3] = s->mac[3];
    s->nic_conf.macaddr.a[4] = s->mac[4];
    s->nic_conf.macaddr.a[5] = s->mac[5];

    /* Create NIC - connects to default netdev */
    s->nic = qemu_new_nic(&wifi_net_client_info, &s->nic_conf,
                           TYPE_ESP32S3_WIFI_NET, dev->id,
                           &dev->mem_reentrancy_guard, s);
    if (s->nic) {
        WIFI_DPRINTF("NIC created for packet I/O\n");
    }

    esp32s3_wifi_net_global = s;
}

static void esp32s3_wifi_net_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = esp32s3_wifi_net_realize;
}

static const TypeInfo esp32s3_wifi_net_info = {
    .name = TYPE_ESP32S3_WIFI_NET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3WifiNetState),
    .instance_init = esp32s3_wifi_net_init,
    .class_init = esp32s3_wifi_net_class_init,
};

static void esp32s3_wifi_net_register_types(void)
{
    type_register_static(&esp32s3_wifi_net_info);
}

type_init(esp32s3_wifi_net_register_types)


/* ========================================================================
 * Guest memory mailbox initialization
 * ======================================================================== */

void esp32s3_wifi_net_init_mailbox(Esp32s3WifiNetState *s)
{
    /*
     * Write event base strings and IP data structure to a known location
     * in guest memory (end of RTC fast memory area).
     */
    const char wifi_str[] = "WIFI_EVENT";
    const char ip_str[] = "IP_EVENT";

    cpu_physical_memory_write(WIFI_MAILBOX_BASE + WIFI_MB_EVENT_STR,
                              wifi_str, sizeof(wifi_str));
    cpu_physical_memory_write(WIFI_MAILBOX_BASE + WIFI_MB_IP_STR,
                              ip_str, sizeof(ip_str));

    /* Write ip_event_got_ip_t structure:
     *   offset 0: esp_netif_t *esp_netif (4 bytes, NULL ok)
     *   offset 4: esp_netif_ip_info_t ip_info:
     *     offset 4: ip (4 bytes)
     *     offset 8: netmask (4 bytes)
     *     offset 12: gw (4 bytes)
     *   offset 16: bool ip_changed (4 bytes)
     */
    uint8_t got_ip_data[20] = {0};
    uint32_t ip = WIFI_DEFAULT_IP;
    uint32_t mask = WIFI_DEFAULT_NETMASK;
    uint32_t gw = WIFI_DEFAULT_GW;
    uint32_t changed = 1;
    memcpy(got_ip_data + 0, &ip, 4);      /* Skip esp_netif ptr at offset 0 */
    /* Actually the structure starts with esp_netif pointer */
    memset(got_ip_data, 0, 4);             /* esp_netif = NULL */
    memcpy(got_ip_data + 4, &ip, 4);      /* ip_info.ip */
    memcpy(got_ip_data + 8, &mask, 4);    /* ip_info.netmask */
    memcpy(got_ip_data + 12, &gw, 4);     /* ip_info.gw */
    memcpy(got_ip_data + 16, &changed, 4); /* ip_changed */
    cpu_physical_memory_write(WIFI_MAILBOX_BASE + WIFI_MB_GOT_IP_DATA,
                              got_ip_data, sizeof(got_ip_data));

    /* Store addresses for use in event delivery */
    s->got_ip_data_addr = WIFI_MAILBOX_BASE + WIFI_MB_GOT_IP_DATA;

    /* Note: wifi_event_str_addr and ip_event_str_addr are set when
     * esp_event_handler_register is called (we use the firmware's own
     * string pointers). As a fallback, use our mailbox strings. */
    if (!s->wifi_event_str_addr) {
        s->wifi_event_str_addr = WIFI_MAILBOX_BASE + WIFI_MB_EVENT_STR;
    }
    if (!s->ip_event_str_addr) {
        s->ip_event_str_addr = WIFI_MAILBOX_BASE + WIFI_MB_IP_STR;
    }

    s->mailbox_ready = true;
    WIFI_DPRINTF("Mailbox initialized at 0x%08x\n", WIFI_MAILBOX_BASE);
}


/* ========================================================================
 * ELF hook registration and extended trampoline patching
 * ======================================================================== */

/* Table of functions using standard 9-byte trampoline */
static const struct {
    const char *name;
    esp32s3_elf_hook_fn handler;
} wifi_standard_hooks[] = {
    { "esp_wifi_init_internal",          handle_esp_wifi_init },
    { "esp_wifi_init",                   handle_esp_wifi_init },
    { "esp_wifi_deinit_internal",        handle_esp_wifi_deinit },
    { "esp_wifi_deinit",                 handle_esp_wifi_deinit },
    { "esp_wifi_stop",                   handle_esp_wifi_stop },
    { "esp_wifi_disconnect",             handle_esp_wifi_disconnect },
    { "esp_wifi_scan_start",             handle_esp_wifi_scan_start },
    { "esp_wifi_scan_get_ap_records",    handle_esp_wifi_scan_get_ap_records },
    { "esp_wifi_scan_get_ap_num",        handle_esp_wifi_scan_get_ap_num },
    { "esp_wifi_set_mode",               handle_esp_wifi_set_mode },
    { "esp_wifi_get_mode",               handle_esp_wifi_get_mode },
    { "esp_wifi_set_config",             handle_esp_wifi_set_config },
    { "esp_wifi_get_config",             handle_esp_wifi_get_config },
    { "esp_wifi_set_storage",            handle_esp_wifi_set_storage },
    { "esp_wifi_set_channel",            handle_esp_wifi_set_channel },
    { "esp_wifi_get_channel",            handle_esp_wifi_get_channel },
    { "esp_wifi_get_mac",                handle_esp_wifi_get_mac },
    { "esp_wifi_set_mac",                handle_esp_wifi_set_mac },
    { "esp_wifi_sta_get_ap_info",        handle_esp_wifi_sta_get_ap_info },
    { "esp_wifi_get_state",              handle_esp_wifi_get_state },
    { "esp_wifi_internal_tx",            handle_esp_wifi_internal_tx },
    { "esp_wifi_internal_reg_rxcb",      handle_esp_wifi_internal_reg_rxcb },
    { "esp_wifi_internal_free_rx_buffer", handle_esp_wifi_internal_free_rx_buffer },
    { "esp_event_handler_instance_register",
                                         handle_esp_event_handler_instance_register },
    { "esp_event_handler_register",      handle_esp_event_handler_register },
};

/* Functions using extended trampolines (patched after ELF load) */
static const struct {
    const char *name;
    esp32s3_elf_hook_fn handler;
    const uint8_t *trampoline;
    int trampoline_size;
} wifi_extended_hooks[] = {
    { "esp_wifi_start",   handle_esp_wifi_start,
      single_event_trampoline, sizeof(single_event_trampoline) },
    { "esp_wifi_connect", handle_esp_wifi_connect,
      double_event_trampoline, sizeof(double_event_trampoline) },
    { "vTaskDelay",       handle_vTaskDelay,
      single_event_trampoline, sizeof(single_event_trampoline) },
};

void esp32s3_wifi_net_register_hooks(Esp32s3WifiNetState *s, void *elf_hooks_opaque)
{
    Esp32s3ElfHooks *hooks = (Esp32s3ElfHooks *)elf_hooks_opaque;

    /* Register standard hooks */
    for (size_t i = 0; i < ARRAY_SIZE(wifi_standard_hooks); i++) {
        esp32s3_elf_hooks_register(hooks, wifi_standard_hooks[i].name,
                                   wifi_standard_hooks[i].handler, s);
    }

    /* Register extended hooks (these get larger trampolines) */
    for (size_t i = 0; i < ARRAY_SIZE(wifi_extended_hooks); i++) {
        esp32s3_elf_hooks_register(hooks, wifi_extended_hooks[i].name,
                                   wifi_extended_hooks[i].handler, s);
    }

    WIFI_DPRINTF("Registered %zu standard + %zu extended WiFi hooks\n",
                 ARRAY_SIZE(wifi_standard_hooks),
                 ARRAY_SIZE(wifi_extended_hooks));
}

/*
 * Called after esp32s3_elf_hooks_patch() to write extended trampolines.
 * The standard patch writes 9-byte trampolines; this overwrites them
 * with larger extended trampolines for functions that need event delivery.
 */
void esp32s3_wifi_net_patch_extended(Esp32s3WifiNetState *s,
                                      void *hooks_opaque,
                                      void *cpu_opaque)
{
    Esp32s3ElfHooks *hooks = (Esp32s3ElfHooks *)hooks_opaque;

    for (size_t i = 0; i < ARRAY_SIZE(wifi_extended_hooks); i++) {
        const char *name = wifi_extended_hooks[i].name;

        /* Find the hook entry to get the function address */
        for (int j = 0; j < hooks->count; j++) {
            if (strcmp(hooks->entries[j].name, name) == 0 &&
                hooks->entries[j].addr != 0) {
                uint64_t addr = hooks->entries[j].addr;

                /* Write extended trampoline */
                cpu_physical_memory_write(
                    addr,  /* Assuming identity mapping for IRAM */
                    wifi_extended_hooks[i].trampoline,
                    wifi_extended_hooks[i].trampoline_size);

                /* Store function addresses for handler use */
                if (strcmp(name, "esp_wifi_start") == 0) {
                    s->esp_wifi_start_addr = addr;
                } else if (strcmp(name, "esp_wifi_connect") == 0) {
                    s->esp_wifi_connect_addr = addr;
                } else if (strcmp(name, "vTaskDelay") == 0) {
                    s->vtaskdelay_addr = addr;
                }

                WIFI_DPRINTF("Extended trampoline: '%s' at 0x%08" PRIx64
                             " (%d bytes)\n",
                             name, addr,
                             wifi_extended_hooks[i].trampoline_size);
                break;
            }
        }
    }
}
