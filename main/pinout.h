#pragma once
#include "driver/gpio.h"

/* ============================================================
 *  Pin map - NexLink (ESP32-S3 CMSIS-DAP wireless debugger)
 *  Target: ESP32-S3-WROOM-1-N16R8
 *          (16MB quad SPI flash + 8MB octal SPI PSRAM, PCB antenna)
 *
 *  NOTE on pins that are NOT available:
 *    IO19/IO20 - native USB DMI/DP, owned by the USB peripheral (CMSIS-DAP)
 *    IO26..IO32 - SPI0/1 flash
 *    IO33..IO37 - Octal PSRAM (CONFIG_SPIRAM_MODE_OCT) - do not use
 *    IO43/IO44  - UART0 (console)
 * ============================================================ */

/* === UART0: console / log output ===
 * The console was moved off USB-Serial-JTAG because the native USB peripheral
 * is now owned by the CMSIS-DAP probe (CherryUSB). UART0 defaults to
 * GPIO43/44 on ESP32-S3. */
#define PIN_UART0_TX         43
#define PIN_UART0_RX         44

/* === UART1: DUT serial bridge (via TXB0106 level shifter) === */
#define PIN_UART1_TX         47   /* ESP32 -> TXB0106 -> DUT */
#define PIN_UART1_RX         21   /* DUT -> TXB0106 -> ESP32 */
#define UART1_PORT_NUM       UART_NUM_1

/* === SWD: dedicated fixed pins for the DAP probe (not permutable) ===
 * Hard-wired to the J3 debug header through the TXB0106. */
#define PIN_SWD_SWCLK        13
#define PIN_SWD_SWDIO        14
#define PIN_SWD_NRST         12

/* === OLED: 0.96" 128x64 SSD1306 over I2C (4-pin module: VCC/GND/SCL/SDA) === */
#define OLED_I2C_PORT        I2C_NUM_0
#define PIN_OLED_SCL         10
#define PIN_OLED_SDA         11
#define OLED_I2C_ADDR        0x3C

/* === Buttons (active low) === */
#define PIN_BTN_SW1          1
#define PIN_BTN_SW2          2
#define PIN_BTN_SW3          42

/* === Free IOs: general purpose, protocol assignment is permutable ===
 * These were the ST7735S LCD's SPI pins. The display now runs over I2C, so
 * all five are released for user-assigned protocols (SPI / I2C / PWM / GPIO). */
#define PIN_FREE_1           48
#define PIN_FREE_2           45   /* strapping pin (VDD_SPI) */
#define PIN_FREE_3           38
#define PIN_FREE_4           39
#define PIN_FREE_5           40

/* === USB (native OTG, fixed pins, driven by the USB peripheral) === */
/* IO19 = USB_DM, IO20 = USB_DP - never configure these as plain GPIO. */
