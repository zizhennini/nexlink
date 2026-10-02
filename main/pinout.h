#pragma once
#include "driver/gpio.h"

/* ============================================================
 *  Pin map - NexLink (ESP32-S3 CMSIS-DAP wireless debugger)
 *  Target: ESP32-S3-WROOM-1-N16R8
 *          (16MB quad SPI flash + 8MB octal SPI PSRAM, PCB antenna)
 *
 *  Hardware revision note (current PCB):
 *    The TXB0106 level shifter of the first revision is GONE. Every signal on
 *    the 2x8 pin header (and the two on-board buttons) connects straight to the
 *    ESP32-S3, so the whole board is native 3.3V: an LDO feeds the S3 from
 *    USB-C 5V, 3V3 is broken out for the target, and only the BOOT and EN
 *    buttons carry a 3V3 pull-up. A 5V-logic target therefore needs its own
 *    level shifting - the header cannot do it.
 *
 *  NOTE on pins that are NOT available:
 *    IO19/IO20 - native USB DMI/DP, owned by the USB peripheral (CMSIS-DAP)
 *    IO26..IO32 - SPI0/1 flash
 *    IO33..IO37 - Octal PSRAM (CONFIG_SPIRAM_MODE_OCT) - do not use
 * ============================================================ */

/* === UART0: console / log output - also the first header UART ===
 * The console was moved off USB-Serial-JTAG because the native USB peripheral
 * is now owned by the CMSIS-DAP probe (CherryUSB). UART0 defaults to
 * GPIO43/44 on ESP32-S3 and is broken out as TXD0 / RXD0. */
#define PIN_UART0_TX         43
#define PIN_UART0_RX         44

/* === UART1: DUT serial bridge - the second header UART === */
#define PIN_UART1_TX         47   /* ESP32 TXD1 -> target RX */
#define PIN_UART1_RX         21   /* target TX  -> ESP32 RXD1 */
#define UART1_PORT_NUM       UART_NUM_1

/* === SWD: dedicated fixed pins for the DAP probe (not permutable) ===
 * Broken out as NRST / SWCLK / SWDIO on the pin header, wired straight to the
 * ESP32-S3 (no level shifter). */
#define PIN_SWD_SWCLK        13
#define PIN_SWD_SWDIO        14
#define PIN_SWD_NRST         12

/* === OLED: 0.96" 128x64 SSD1306 over I2C (4-pin module: VCC/GND/SCL/SDA) ===
 * SCL/SDA are also broken out on the pin header, so the same bus can be used
 * to sniff an external I2C link (the panel shares it - address 0x3C). */
#define OLED_I2C_PORT        I2C_NUM_0
#define PIN_OLED_SCL         10
#define PIN_OLED_SDA         11
#define OLED_I2C_ADDR        0x3C

/* === Buttons (active low, 3V3 pull-up on the PCB) === */
#define PIN_BTN_SW1          1
#define PIN_BTN_SW2          2
#define PIN_BTN_SW3          42

/* === Free IOs: general purpose, protocol assignment is permutable ===
 * All five are broken out on the pin header as expansion IO. They were the
 * ST7735S LCD's SPI pins in the first revision; the display now runs over I2C,
 * so they are free for user-assigned protocols (SPI / I2C / PWM / GPIO).
 * PIN_FREE_2 (IO45) is also a strapping pin (VDD_SPI) - avoid driving it at
 * reset if the target is expected to boot. */
#define PIN_FREE_1           48
#define PIN_FREE_2           45   /* strapping pin (VDD_SPI) */
#define PIN_FREE_3           38
#define PIN_FREE_4           39
#define PIN_FREE_5           40

/* === USB (native OTG, fixed pins, driven by the USB peripheral) === */
/* IO19 = USB_DM, IO20 = USB_DP - never configure these as plain GPIO. */
