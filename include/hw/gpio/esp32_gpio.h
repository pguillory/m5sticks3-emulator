#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"

#define TYPE_ESP32_GPIO "esp32.gpio"
#define ESP32_GPIO(obj)             OBJECT_CHECK(Esp32GpioState, (obj), TYPE_ESP32_GPIO)
#define ESP32_GPIO_GET_CLASS(obj)   OBJECT_GET_CLASS(Esp32GpioClass, obj, TYPE_ESP32_GPIO)
#define ESP32_GPIO_CLASS(klass)     OBJECT_CLASS_CHECK(Esp32GpioClass, klass, TYPE_ESP32_GPIO)


/* Maximum GPIO count across all ESP32 variants (ESP32-S3 has 49) */
#define ESP32_GPIO_MAX_COUNT    49

/* GPIO register offsets */
REG32(GPIO_BT_SELECT,    0x0000)
REG32(GPIO_OUT,          0x0004)
REG32(GPIO_OUT_W1TS,     0x0008)
REG32(GPIO_OUT_W1TC,     0x000C)
REG32(GPIO_OUT1,         0x0010)
REG32(GPIO_OUT1_W1TS,    0x0014)
REG32(GPIO_OUT1_W1TC,    0x0018)
REG32(GPIO_SDIO_SELECT,  0x001C)
REG32(GPIO_ENABLE,       0x0020)
REG32(GPIO_ENABLE_W1TS,  0x0024)
REG32(GPIO_ENABLE_W1TC,  0x0028)
REG32(GPIO_ENABLE1,      0x002C)
REG32(GPIO_ENABLE1_W1TS, 0x0030)
REG32(GPIO_ENABLE1_W1TC, 0x0034)
REG32(GPIO_STRAP,        0x0038)
REG32(GPIO_IN,           0x003C)
REG32(GPIO_IN1,          0x0040)
REG32(GPIO_STATUS,       0x0044)
REG32(GPIO_STATUS_W1TS,  0x0048)
REG32(GPIO_STATUS_W1TC,  0x004C)
REG32(GPIO_STATUS1,      0x0050)
REG32(GPIO_STATUS1_W1TS, 0x0054)
REG32(GPIO_STATUS1_W1TC, 0x0058)
REG32(GPIO_PCPU_INT,     0x005C)
REG32(GPIO_PCPU_NMI_INT, 0x0060)
REG32(GPIO_PCPU_INT1,    0x0068)
REG32(GPIO_PCPU_NMI_INT1,0x006C)

/* Per-pin config registers start at 0x74, spaced 4 bytes apart */
#define GPIO_PIN_REG_BASE       0x0074
#define GPIO_PIN_REG(n)         (GPIO_PIN_REG_BASE + (n) * 4)

/* Function input/output selection registers */
#define GPIO_FUNC_IN_SEL_BASE   0x0154
#define GPIO_FUNC_OUT_SEL_BASE  0x0554

#define ESP32_STRAP_MODE_FLASH_BOOT 0x12
#define ESP32_STRAP_MODE_UART_BOOT  0x0f

/* GPIO I/O region size: covers up to FUNC_OUT_SEL for 49 pins */
#define ESP32_GPIO_IO_SIZE      0x0700

/* Per-pin register bit definitions (GPIO_PINn_REG) */
#define GPIO_PIN_PAD_DRIVER_BIT     2
#define GPIO_PIN_INT_TYPE_SHIFT     7
#define GPIO_PIN_INT_TYPE_MASK      (0x7 << GPIO_PIN_INT_TYPE_SHIFT)
#define GPIO_PIN_WAKEUP_ENABLE_BIT  10
#define GPIO_PIN_INT_ENA_SHIFT      13
#define GPIO_PIN_INT_ENA_MASK       (0x1F << GPIO_PIN_INT_ENA_SHIFT)

/* Interrupt type values (GPIO_PINn_INT_TYPE field) */
#define GPIO_INT_TYPE_DISABLE       0
#define GPIO_INT_TYPE_RISING        1
#define GPIO_INT_TYPE_FALLING       2
#define GPIO_INT_TYPE_ANY_EDGE      3
#define GPIO_INT_TYPE_LOW_LEVEL     4
#define GPIO_INT_TYPE_HIGH_LEVEL    5


typedef struct Esp32GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t strap_mode;

    /* Output GPIOs — observable by connected devices (e.g. display DC pin) */
    qemu_irq out_gpios[ESP32_GPIO_MAX_COUNT];

    /* GPIO registers */
    uint32_t gpio_out;      /* GPIO 0-31 output value */
    uint32_t gpio_out1;     /* GPIO 32-48 output value */
    uint32_t gpio_enable;   /* GPIO 0-31 output enable */
    uint32_t gpio_enable1;  /* GPIO 32-48 output enable */
    uint32_t gpio_in;       /* GPIO 0-31 input value (external signals) */
    uint32_t gpio_in1;      /* GPIO 32-48 input value (external signals) */
    uint32_t gpio_status;   /* GPIO 0-31 interrupt status */
    uint32_t gpio_status1;  /* GPIO 32-48 interrupt status */

    /* Per-pin configuration */
    uint32_t pin_reg[ESP32_GPIO_MAX_COUNT];

    /* Function selection (not fully emulated, but stored for firmware compat) */
    uint32_t func_in_sel[256];
    uint32_t func_out_sel[ESP32_GPIO_MAX_COUNT];

    /*
     * I2C bit-bang ACK simulation.
     *
     * On the M5StickS3, the I2C bus has external pull-up resistors and
     * slave devices (M5PM1 PMIC at 0x6E).  M5Unified's board detection
     * bit-bangs I2C by toggling GPIO outputs and reading the SDA pin.
     * The I2C slave models in the emulator are connected to the I2C
     * controller MMIO, not to raw GPIO, so the bit-bang never reaches them.
     *
     * To bridge this gap: when a pin marked in i2c_sda_ack_mask transitions
     * from output-enabled to input (output enable cleared), we temporarily
     * pull its gpio_in value LOW for one GPIO_IN read.  This simulates the
     * ACK pulse that a real I2C slave would generate.
     *
     * i2c_sda_ack_mask1: bitmask for GPIO 32-48 pins that have I2C slaves
     * i2c_ack_pending1:  bits pending a one-shot LOW read
     */
    uint32_t i2c_sda_ack_mask1;
    uint32_t i2c_ack_pending1;

} Esp32GpioState;

typedef struct Esp32GpioClass {
    SysBusDeviceClass parent_class;
} Esp32GpioClass;

/**
 * Set the level of a GPIO input pin programmatically.
 * This is the primary injection mechanism for buttons, sensors, etc.
 * Evaluates interrupt conditions and fires the GPIO IRQ if triggered.
 *
 * @param s     GPIO device state
 * @param pin   GPIO pin number (0-48)
 * @param level Pin level: 1 = HIGH, 0 = LOW
 */
void esp32_gpio_set_pin_level(Esp32GpioState *s, int pin, int level);
