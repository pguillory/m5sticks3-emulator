/*
 * AXP2101 PMIC I2C emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * Emulates the AXP2101 power management IC's I2C register interface.
 * The firmware's power driver (M5Unified Power_Class, or direct AXP2101
 * drivers) reads chip ID, configures power rails, sets charging parameters,
 * and monitors battery status through I2C. This model accepts all writes
 * and returns sensible power-on defaults so the driver initializes
 * successfully.
 *
 * Key behaviors:
 *   - Chip ID register (0x03) returns 0x47 (AXP2101 identifier)
 *   - Status registers report USB power present, battery charging
 *   - ADC registers report ~3.8V battery (~80%), ~5V VBUS
 *   - Battery percentage register reads 80%
 *   - All power rail enables and voltage settings are writable
 *   - IRQ status registers are write-1-to-clear
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/axp2101.h"

#define AXP2101_DEBUG   0


/*
 * Set default register values to match a real AXP2101 with USB power
 * connected and battery at ~80%.
 */
static void axp2101_reset_regs(AXP2101State *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    /*
     * 0x00 PMU_STATUS_1:
     *   bit 5: VBUS good indication (1 = VBUS present and valid)
     *   bit 3: battery current direction (0 = charging)
     *   bit 2: battery present (1 = battery connected)
     *   bit 0: charging indication (1 = charging active)
     */
    s->regs[AXP2101_PMU_STATUS_1] = 0x25;

    /*
     * 0x01 PMU_STATUS_2:
     *   bits[7:5]: charging state (001 = constant current phase)
     *   bit 3: VBUS present (1 = VBUS available)
     */
    s->regs[AXP2101_PMU_STATUS_2] = 0x28;

    /*
     * 0x03 CHIP_ID: AXP2101 identification
     *   Returns 0x47 (71 decimal) — the standard AXP2101 chip ID
     */
    s->regs[AXP2101_CHIP_ID] = 0x47;

    /* Data buffers — user-accessible general-purpose storage (zeroed) */
    s->regs[AXP2101_DATA_BUFFER_0] = 0x00;
    s->regs[AXP2101_DATA_BUFFER_1] = 0x00;
    s->regs[AXP2101_DATA_BUFFER_2] = 0x00;
    s->regs[AXP2101_DATA_BUFFER_3] = 0x00;

    /*
     * PMU common configuration defaults
     */
    s->regs[AXP2101_COMMON_CFG]    = 0x00;  /* Default common config */
    s->regs[AXP2101_BATFET_CTRL]   = 0x00;  /* BATFET enabled (default) */
    s->regs[AXP2101_TEMP_CTRL]     = 0x00;  /* Default temp control */
    s->regs[AXP2101_MIN_SYS_VOL_CTRL] = 0x07;  /* 3.5V min system voltage */
    s->regs[AXP2101_INPUT_VOLT_LIMIT]  = 0x0E;  /* 4.36V input voltage limit */
    s->regs[AXP2101_INPUT_CURR_LIMIT]  = 0x08;  /* 500mA input current limit */
    s->regs[AXP2101_RESET_FUEL_GAUGE]  = 0x00;
    s->regs[AXP2101_CHARGE_GAUGE_WDT]  = 0x00;
    s->regs[AXP2101_WATCHDOG_CTRL]     = 0x00;
    s->regs[AXP2101_LOW_BATT_WARNING]  = 0x04;  /* 3.0V low battery warning */

    /*
     * Power-on/off status
     */
    s->regs[AXP2101_PWRON_STATUS]  = 0x20;  /* Power on via VBUS insertion */
    s->regs[AXP2101_PWROFF_STATUS] = 0x00;  /* No shutdown events */
    s->regs[AXP2101_PWROFF_EN]     = 0x07;  /* Default shutdown enables */
    s->regs[AXP2101_DCDC_PWROFF_CTRL] = 0x00;
    s->regs[AXP2101_PWROFF_VSYS_THRESH] = 0x02;  /* 2.6V VSYS shutdown */
    s->regs[AXP2101_PWROK_DELAY_SEQ]    = 0x00;
    s->regs[AXP2101_SLEEP_WAKEUP_CTRL]  = 0x00;
    s->regs[AXP2101_IRQ_PWRON_TIMING]   = 0x06;  /* PowerKey hold=1s, off=4s */

    /* Fast power-on config */
    s->regs[AXP2101_FAST_PWRON_0]  = 0x00;
    s->regs[AXP2101_FAST_PWRON_1]  = 0x00;
    s->regs[AXP2101_FAST_PWRON_2]  = 0x00;
    s->regs[AXP2101_FAST_PWRON_3]  = 0x00;

    /*
     * ADC: enable all measurements by default
     * bit 0: VBAT, bit 1: TS, bit 2: VBUS, bit 3: VSYS, bit 4: TDIE
     */
    s->regs[AXP2101_ADC_ENABLE] = 0x1F;

    /*
     * ADC results — battery at ~3.8V (80% charge)
     *
     * AXP2101 ADC is 14-bit. Encoding: H[7:0] = data[13:6], L[5:0] = data[5:0]
     *
     * VBAT: 3800mV. Scale: VBAT_mV = ADC_value * 1.0 (1mV per LSB)
     *   ADC_value = 3800 = 0xED8
     *   H = 0xED8 >> 6 = 0x3B,  L = 0xED8 & 0x3F = 0x18
     */
    s->regs[AXP2101_VBAT_H] = 0x3B;
    s->regs[AXP2101_VBAT_L] = 0x18;

    /*
     * TS (thermistor): ~25°C typical value
     */
    s->regs[AXP2101_TS_H] = 0x20;
    s->regs[AXP2101_TS_L] = 0x00;

    /*
     * VBUS: ~5000mV (USB power)
     *   ADC_value = 5000 = 0x1388
     *   H = 0x1388 >> 6 = 0x4E,  L = 0x1388 & 0x3F = 0x08
     */
    s->regs[AXP2101_VBUS_H] = 0x4E;
    s->regs[AXP2101_VBUS_L] = 0x08;

    /*
     * VSYS: ~3800mV (following battery)
     */
    s->regs[AXP2101_VSYS_H] = 0x3B;
    s->regs[AXP2101_VSYS_L] = 0x18;

    /*
     * TDIE: internal die temperature ~25°C
     * Encoding: 7274 + (T_die - 25) * 20 ≈ 7274 for 25°C
     *   7274 = 0x1C6A → H = 0x71, L = 0x2A
     */
    s->regs[AXP2101_TDIE_H] = 0x71;
    s->regs[AXP2101_TDIE_L] = 0x2A;

    /*
     * IRQ enable registers — all disabled by default
     */
    s->regs[AXP2101_IRQ_ENABLE_0] = 0x00;
    s->regs[AXP2101_IRQ_ENABLE_1] = 0x00;
    s->regs[AXP2101_IRQ_ENABLE_2] = 0x00;

    /* IRQ status — no pending interrupts */
    s->regs[AXP2101_IRQ_STATUS_0] = 0x00;
    s->regs[AXP2101_IRQ_STATUS_1] = 0x00;
    s->regs[AXP2101_IRQ_STATUS_2] = 0x00;

    /* Temperature sensor control */
    s->regs[AXP2101_TS_CTRL] = 0x03;  /* TS enabled, default mode */

    /*
     * Charging configuration — sensible defaults
     */
    s->regs[AXP2101_PRECHARGE_SETTING]   = 0x01;  /* 50mA precharge */
    s->regs[AXP2101_FASTCHARGE_SETTING]  = 0x08;  /* 300mA fast charge */
    s->regs[AXP2101_TERM_CURR_SETTING]   = 0x03;  /* 15% termination */
    s->regs[AXP2101_CHG_VOLTAGE_SETTING] = 0x03;  /* 4.2V charge target */
    s->regs[AXP2101_THERMAL_REGULATION]  = 0x02;  /* 100°C thermal limit */
    s->regs[AXP2101_CHG_TIMEOUT_SETTING] = 0x00;  /* Timeout disabled */
    s->regs[AXP2101_BATT_DETECT_CTRL]    = 0x01;  /* Battery detection on */
    s->regs[AXP2101_CHGLED_CTRL]         = 0x04;  /* LED controlled by charger */

    /*
     * DCDC control — DCDC1-4 enabled, with sensible voltage defaults
     *
     * 0x80 DCDC_EN_CTRL:
     *   bit 0: DCDC1 enable
     *   bit 1: DCDC2 enable
     *   bit 2: DCDC3 enable
     *   bit 3: DCDC4 enable
     */
    s->regs[AXP2101_DCDC_EN_CTRL]  = 0x0F;  /* DCDC1-4 enabled */
    s->regs[AXP2101_DCDC_PWM_CTRL] = 0x00;  /* Auto PWM/PFM mode */

    /*
     * DCDC voltage settings:
     *   DCDC1: 3.3V (typical system rail)
     *   DCDC2: 0.9V (CPU core — lower power default)
     *   DCDC3: 3.3V
     *   DCDC4: 1.5V
     *   DCDC5: 1.4V
     */
    s->regs[AXP2101_DCDC1_VOLTAGE] = 0x1C;  /* ~3.3V: (0x1C * 20 + 1500) mV */
    s->regs[AXP2101_DCDC2_VOLTAGE] = 0x28;  /* ~0.9V */
    s->regs[AXP2101_DCDC3_VOLTAGE] = 0x1C;  /* ~3.3V */
    s->regs[AXP2101_DCDC4_VOLTAGE] = 0x10;  /* ~1.5V */
    s->regs[AXP2101_DCDC5_VOLTAGE] = 0x00;  /* 1.4V base */

    /*
     * LDO control — enable common LDOs by default
     *
     * 0x90 LDO_EN_CTRL_0:
     *   bit 0: ALDO1, bit 1: ALDO2, bit 2: ALDO3, bit 3: ALDO4
     *   bit 4: BLDO1, bit 5: BLDO2, bit 7: DLDO1
     * 0x91 LDO_EN_CTRL_1:
     *   bit 0: DLDO2, bit 1: CPUSLDO
     */
    s->regs[AXP2101_LDO_EN_CTRL_0] = 0xBF;  /* Most LDOs enabled */
    s->regs[AXP2101_LDO_EN_CTRL_1] = 0x01;  /* DLDO2 enabled */

    /*
     * LDO voltage settings — all set to 3.3V by default
     * For ALDOx/BLDOx: voltage = 500mV + reg_val * 100mV
     *   3.3V → (3300 - 500) / 100 = 28 = 0x1C
     * For DLDOx: voltage = 500mV + reg_val * 50mV (different step)
     *   3.3V → (3300 - 500) / 50 = 56 = 0x38
     */
    s->regs[AXP2101_ALDO1_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_ALDO2_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_ALDO3_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_ALDO4_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_BLDO1_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_BLDO2_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_CPUSLDO_VOLTAGE] = 0x1C;  /* 3.3V */
    s->regs[AXP2101_DLDO1_VOLTAGE] = 0x38;  /* 3.3V (backlight rail) */
    s->regs[AXP2101_DLDO2_VOLTAGE] = 0x38;  /* 3.3V */

    /*
     * Fuel gauge — battery at 80%
     */
    s->regs[AXP2101_BATTERY_PARAM]      = 0x00;
    s->regs[AXP2101_FUEL_GAUGE_CTRL]    = 0x00;
    s->regs[AXP2101_BATTERY_PERCENTAGE] = 80;  /* 80% state of charge */
}


static int axp2101_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    AXP2101State *s = AXP2101(i2c);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_SEND_ASYNC:
        s->addr_received = false;
        break;
    case I2C_START_RECV:
        break;
    case I2C_FINISH:
        break;
    case I2C_NACK:
        break;
    }
    return 0;
}


static uint8_t axp2101_i2c_recv(I2CSlave *i2c)
{
    AXP2101State *s = AXP2101(i2c);
    uint8_t val = 0;

    if (s->reg_addr < AXP2101_REG_COUNT) {
        val = s->regs[s->reg_addr];
    } else {
        /* Unmapped registers read as 0 */
        val = 0;
    }

#if AXP2101_DEBUG
    info_report("[AXP2101] Read reg 0x%02x => 0x%02x", s->reg_addr, val);
#endif

    /* Auto-increment register address */
    s->reg_addr++;
    return val;
}


static int axp2101_i2c_send(I2CSlave *i2c, uint8_t data)
{
    AXP2101State *s = AXP2101(i2c);

    if (!s->addr_received) {
        /* First byte after START is the register address */
        s->reg_addr = data;
        s->addr_received = true;
    } else {
        /* Subsequent bytes are data to write */
#if AXP2101_DEBUG
        info_report("[AXP2101] Write reg 0x%02x <= 0x%02x", s->reg_addr, data);
#endif
        if (s->reg_addr < AXP2101_REG_COUNT) {
            switch (s->reg_addr) {
            case AXP2101_CHIP_ID:
                /* Read-only: chip ID cannot be changed */
                break;

            case AXP2101_PMU_STATUS_1:
            case AXP2101_PMU_STATUS_2:
                /* Read-only status registers — ignore writes */
                break;

            case AXP2101_VBAT_H ... AXP2101_TDIE_L:
                /* ADC result registers are read-only */
                break;

            case AXP2101_BATTERY_PERCENTAGE:
                /* Fuel gauge percentage is read-only */
                break;

            case AXP2101_IRQ_STATUS_0:
            case AXP2101_IRQ_STATUS_1:
            case AXP2101_IRQ_STATUS_2:
                /*
                 * IRQ status registers are write-1-to-clear:
                 * writing a 1 bit clears that interrupt flag.
                 */
                s->regs[s->reg_addr] &= ~data;
                break;

            case AXP2101_RESET_FUEL_GAUGE:
                /*
                 * Writing specific bits triggers resets of the fuel gauge
                 * subsystems. Accept the write but don't change behavior.
                 */
                s->regs[s->reg_addr] = data;
                break;

            default:
                /* All other registers are read-write configuration */
                s->regs[s->reg_addr] = data;
                break;
            }
        }
        s->reg_addr++;
    }
    return 0;
}


static void axp2101_reset_hold(Object *obj, ResetType type)
{
    AXP2101State *s = AXP2101(obj);
    axp2101_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void axp2101_realize(DeviceState *dev, Error **errp)
{
}


static void axp2101_init(Object *obj)
{
    AXP2101State *s = AXP2101(obj);
    axp2101_reset_regs(s);
    s->reg_addr = 0;
    s->addr_received = false;
}


static void axp2101_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = axp2101_reset_hold;
    dc->realize = axp2101_realize;
    sc->event = axp2101_i2c_event;
    sc->recv = axp2101_i2c_recv;
    sc->send = axp2101_i2c_send;
}


static const TypeInfo axp2101_info = {
    .name          = TYPE_AXP2101,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AXP2101State),
    .instance_init = axp2101_init,
    .class_init    = axp2101_class_init,
};


static void axp2101_register_types(void)
{
    type_register_static(&axp2101_info);
}

type_init(axp2101_register_types)
