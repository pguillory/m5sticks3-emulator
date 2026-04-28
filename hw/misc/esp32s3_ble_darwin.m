/*
 * ESP32-S3 BLE Bridge — CoreBluetooth Backend (macOS)
 *
 * Uses CBPeripheralManager to advertise as a BLE peripheral with the
 * Nordic UART Service (NUS). When an iOS device connects and writes
 * to the RX characteristic, data is passed into the emulated firmware.
 * When the firmware sends a notification on the TX characteristic,
 * it's pushed out through CoreBluetooth to the connected device.
 *
 * NUS UUIDs:
 *   Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (write): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX (notify): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"

/* This file is only compiled on macOS (see hw/misc/meson.build) */

#import <Foundation/Foundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include "qemu/log.h"
#include "hw/misc/esp32s3_ble.h"

#define CB_DPRINTF(fmt, ...) \
    qemu_log("[CoreBluetooth] " fmt, ## __VA_ARGS__)

/* NUS Service and Characteristic UUIDs */
static NSString *const kNUSServiceUUID = @"6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const kNUSRxCharUUID  = @"6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const kNUSTxCharUUID  = @"6E400003-B5A3-F393-E0A9-E50E24DCCA9E";


/* ========================================================================
 * CoreBluetooth Peripheral Manager Delegate
 * ======================================================================== */

@interface QEMUBLEPeripheral : NSObject <CBPeripheralManagerDelegate>
@property (nonatomic, strong) CBPeripheralManager *peripheralManager;
@property (nonatomic, strong) CBMutableService *nusService;
@property (nonatomic, strong) CBMutableCharacteristic *rxCharacteristic;
@property (nonatomic, strong) CBMutableCharacteristic *txCharacteristic;
@property (nonatomic, strong) CBCentral *connectedCentral;
@property (nonatomic, copy) NSString *advertisingName;
@property (nonatomic, assign) Esp32s3BleState *bleState;
@property (nonatomic, assign) BOOL isAdvertising;
@property (nonatomic, assign) BOOL isPoweredOn;
@end

@implementation QEMUBLEPeripheral

- (instancetype)initWithBleState:(Esp32s3BleState *)bleState {
    self = [super init];
    if (self) {
        _bleState = bleState;
        _isAdvertising = NO;
        _isPoweredOn = NO;

        /* Initialize CBPeripheralManager on the main queue */
        dispatch_async(dispatch_get_main_queue(), ^{
            self.peripheralManager = [[CBPeripheralManager alloc]
                initWithDelegate:self
                queue:dispatch_get_main_queue()
                options:@{
                    CBPeripheralManagerOptionShowPowerAlertKey: @YES
                }];
        });
    }
    return self;
}

- (void)setupNUSService {
    /* Create TX characteristic (notify) */
    self.txCharacteristic = [[CBMutableCharacteristic alloc]
        initWithType:[CBUUID UUIDWithString:kNUSTxCharUUID]
        properties:CBCharacteristicPropertyNotify
        value:nil
        permissions:CBAttributePermissionsReadable];

    /* Create RX characteristic (write, write without response) */
    self.rxCharacteristic = [[CBMutableCharacteristic alloc]
        initWithType:[CBUUID UUIDWithString:kNUSRxCharUUID]
        properties:(CBCharacteristicPropertyWrite |
                    CBCharacteristicPropertyWriteWithoutResponse)
        value:nil
        permissions:CBAttributePermissionsWriteable];

    /* Create NUS service */
    self.nusService = [[CBMutableService alloc]
        initWithType:[CBUUID UUIDWithString:kNUSServiceUUID]
        primary:YES];
    self.nusService.characteristics = @[
        self.rxCharacteristic,
        self.txCharacteristic
    ];

    /* Add service to peripheral manager */
    [self.peripheralManager addService:self.nusService];

    CB_DPRINTF("NUS service configured\n");
}

- (void)startAdvertisingWithName:(NSString *)name {
    self.advertisingName = name;

    if (self.isPoweredOn) {
        [self doStartAdvertising];
    } else {
        CB_DPRINTF("Bluetooth not powered on yet, will advertise when ready\n");
    }
}

- (void)doStartAdvertising {
    if (self.isAdvertising) {
        [self.peripheralManager stopAdvertising];
    }

    NSDictionary *advertisingData = @{
        CBAdvertisementDataLocalNameKey: self.advertisingName ?: @"ESP32-S3",
        CBAdvertisementDataServiceUUIDsKey: @[
            [CBUUID UUIDWithString:kNUSServiceUUID]
        ]
    };

    [self.peripheralManager startAdvertising:advertisingData];
    self.isAdvertising = YES;

    CB_DPRINTF("Started advertising as \"%s\"\n",
               [self.advertisingName UTF8String]);
}

- (void)stopAdvertising {
    if (self.isAdvertising) {
        [self.peripheralManager stopAdvertising];
        self.isAdvertising = NO;
        CB_DPRINTF("Stopped advertising\n");
    }
}

- (void)sendNotification:(NSData *)data {
    if (!self.connectedCentral || !self.txCharacteristic) {
        CB_DPRINTF("Cannot send notification: no connected central\n");
        return;
    }

    BOOL success = [self.peripheralManager
        updateValue:data
        forCharacteristic:self.txCharacteristic
        onSubscribedCentrals:@[self.connectedCentral]];

    if (success) {
        CB_DPRINTF("Sent TX notification: %lu bytes\n",
                   (unsigned long)data.length);
    } else {
        CB_DPRINTF("TX notification queued (will retry)\n");
    }
}

/* ---- CBPeripheralManagerDelegate methods ---- */

- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral {
    switch (peripheral.state) {
        case CBManagerStatePoweredOn:
            CB_DPRINTF("Bluetooth powered ON\n");
            self.isPoweredOn = YES;
            [self setupNUSService];
            if (self.advertisingName) {
                [self doStartAdvertising];
            }
            break;
        case CBManagerStatePoweredOff:
            CB_DPRINTF("Bluetooth powered OFF\n");
            self.isPoweredOn = NO;
            self.isAdvertising = NO;
            break;
        case CBManagerStateUnauthorized:
            CB_DPRINTF("Bluetooth unauthorized — check System Settings > "
                       "Privacy > Bluetooth\n");
            break;
        case CBManagerStateUnsupported:
            CB_DPRINTF("Bluetooth not supported on this hardware\n");
            break;
        default:
            CB_DPRINTF("Bluetooth state: %ld\n", (long)peripheral.state);
            break;
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
            didAddService:(CBService *)service
                    error:(NSError *)error {
    if (error) {
        CB_DPRINTF("Failed to add service: %s\n",
                   [[error localizedDescription] UTF8String]);
    } else {
        CB_DPRINTF("Service added: %s\n",
                   [[service.UUID UUIDString] UTF8String]);
    }
}

- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)peripheral
                                       error:(NSError *)error {
    if (error) {
        CB_DPRINTF("Failed to start advertising: %s\n",
                   [[error localizedDescription] UTF8String]);
    } else {
        CB_DPRINTF("Advertising started successfully\n");
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                  central:(CBCentral *)central
didSubscribeToCharacteristic:(CBCharacteristic *)characteristic {
    CB_DPRINTF("Central subscribed to %s (MTU=%lu)\n",
               [[characteristic.UUID UUIDString] UTF8String],
               (unsigned long)central.maximumUpdateValueLength);

    self.connectedCentral = central;

    /* Notify the BLE state machine of the connection */
    if (self.bleState) {
        esp32s3_ble_on_connect(self.bleState, 1);  /* conn_handle = 1 */
        esp32s3_ble_on_mtu_change(self.bleState,
            (uint16_t)central.maximumUpdateValueLength);
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
                  central:(CBCentral *)central
didUnsubscribeFromCharacteristic:(CBCharacteristic *)characteristic {
    CB_DPRINTF("Central unsubscribed from %s\n",
               [[characteristic.UUID UUIDString] UTF8String]);

    if (self.connectedCentral == central) {
        self.connectedCentral = nil;

        if (self.bleState) {
            esp32s3_ble_on_disconnect(self.bleState, 1);
        }
    }
}

- (void)peripheralManager:(CBPeripheralManager *)peripheral
    didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests {
    for (CBATTRequest *request in requests) {
        CB_DPRINTF("Write request on %s: %lu bytes\n",
                   [[request.characteristic.UUID UUIDString] UTF8String],
                   (unsigned long)request.value.length);

        if ([request.characteristic.UUID isEqual:
             [CBUUID UUIDWithString:kNUSRxCharUUID]]) {
            /* Data written to RX characteristic — pass to firmware */
            const uint8_t *bytes = (const uint8_t *)request.value.bytes;
            uint32_t len = (uint32_t)request.value.length;

            /* Log the received data */
            CB_DPRINTF("  RX data: \"");
            for (uint32_t i = 0; i < len && i < 128; i++) {
                if (bytes[i] >= 0x20 && bytes[i] < 0x7f) {
                    qemu_log("%c", bytes[i]);
                } else {
                    qemu_log("\\x%02x", bytes[i]);
                }
            }
            qemu_log("\"\n");

            if (self.bleState) {
                esp32s3_ble_on_rx_data(self.bleState, bytes, len);
            }
        }

        /* Respond to the write request */
        [peripheral respondToRequest:request
                          withResult:CBATTErrorSuccess];
    }
}

- (void)peripheralManagerIsReadyToUpdateSubscribers:
    (CBPeripheralManager *)peripheral {
    CB_DPRINTF("Ready to send more notifications\n");
}

@end


/* ========================================================================
 * C API for QEMU integration
 * ======================================================================== */

void *esp32s3_ble_cb_init(Esp32s3BleState *ble_state)
{
    @autoreleasepool {
        QEMUBLEPeripheral *peripheral = [[QEMUBLEPeripheral alloc]
            initWithBleState:ble_state];
        /* Transfer ownership — caller must cleanup */
        return (__bridge_retained void *)peripheral;
    }
}

void esp32s3_ble_cb_start_advertising(void *bridge, const char *name)
{
    if (!bridge) return;

    QEMUBLEPeripheral *peripheral = (__bridge QEMUBLEPeripheral *)bridge;
    NSString *nsName = name ? [NSString stringWithUTF8String:name] : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        [peripheral startAdvertisingWithName:nsName];
    });
}

void esp32s3_ble_cb_stop_advertising(void *bridge)
{
    if (!bridge) return;

    QEMUBLEPeripheral *peripheral = (__bridge QEMUBLEPeripheral *)bridge;
    dispatch_async(dispatch_get_main_queue(), ^{
        [peripheral stopAdvertising];
    });
}

void esp32s3_ble_cb_send_notification(void *bridge,
                                       const uint8_t *data, uint32_t len)
{
    if (!bridge || !data || len == 0) return;

    QEMUBLEPeripheral *peripheral = (__bridge QEMUBLEPeripheral *)bridge;
    NSData *nsData = [NSData dataWithBytes:data length:len];

    dispatch_async(dispatch_get_main_queue(), ^{
        [peripheral sendNotification:nsData];
    });
}

void esp32s3_ble_cb_cleanup(void *bridge)
{
    if (!bridge) return;

    QEMUBLEPeripheral *peripheral = (__bridge_transfer QEMUBLEPeripheral *)bridge;
    dispatch_async(dispatch_get_main_queue(), ^{
        [peripheral stopAdvertising];
        peripheral.peripheralManager = nil;
    });
}
