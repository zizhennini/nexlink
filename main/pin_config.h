#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Two pin groups on this board:
 *
 *  1) FIXED dedicated buses (not user-permutable, see pinout.h):
 *       - UART1 : PIN_UART1_TX / PIN_UART1_RX   (DUT serial bridge)
 *       - SWD   : PIN_SWD_SWCLK / SWDIO / NRST  (CMSIS-DAP probe)
 *       - I2C0  : PIN_OLED_SCL / PIN_OLED_SDA   (SSD1306 display)
 *
 *  2) FIVE FREE IOs (PIN_FREE_1..5) whose protocol function is permutable and
 *     persisted in NVS. This is what "pin configuration" means on this board.
 * ============================================================ */

/* Kinds shown on the expansion / J3 diagram. 5V/GND/DUT are power and
 * reference (fixed); SWCLK/SWDIO/NRST/TX/RX are fixed dedicated signals; the
 * remaining kinds can be assigned to one of the free IOs. */
typedef enum {
    PIN_5V = 0,
    PIN_GND,
    PIN_DUT,
    PIN_SWCLK,
    PIN_SWDIO,
    PIN_NRST,
    PIN_TX,
    PIN_RX,
    PIN_PWM,
    PIN_SPI_SCK,
    PIN_SPI_MOSI,
    PIN_SPI_MISO,
    PIN_SPI_CS,
    PIN_I2C_SDA,
    PIN_I2C_SCL,
    PIN_GPIO,
    PIN_KIND_COUNT
} pin_kind_t;

#define PIN_NUM_SIGNAL_SLOTS 5

/* Load persisted assignment from NVS (defaults if none).
 * Call once after nvs_flash_init(), before serial_bridge_init(). */
void pin_config_init(void);

/* Free-IO slot -> function assigned to it / the fixed GPIO of that slot. */
pin_kind_t pin_config_pos_func(int pos);
int        pin_config_pos_io(int pos);

/* The GPIO currently carrying `func`. For the fixed dedicated signals
 * (SWCLK/SWDIO/NRST/TX/RX) this always returns the fixed pinout.h pin.
 * For free-IO functions it returns -1 when the function is not assigned. */
int pin_config_func_io(pin_kind_t func);

/* --- Fixed dedicated signals (always valid, read from pinout.h) --- */
int pin_config_swclk(void);
int pin_config_swdio(void);
int pin_config_nrst(void);
int pin_config_tx(void);
int pin_config_rx(void);

/* --- Free-IO functions (valid only when assigned to one of the five slots) --- */
int pin_config_pwm(void);
int pin_config_spi_sck(void);
int pin_config_spi_mosi(void);
int pin_config_spi_miso(void);
int pin_config_spi_cs(void);
int pin_config_i2c_sda(void);
int pin_config_i2c_scl(void);

/* Set the function-per-slot order. Must be a permutation of the permutable
 * kinds. Saves to NVS. */
esp_err_t pin_config_set_signal_order(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS]);

/* Parse a comma separated list of kind names, e.g. "PWM,SPI_SCK,SPI_MOSI".
 * Fills `out` on success. */
esp_err_t pin_config_parse_order(const char *text, pin_kind_t out[PIN_NUM_SIGNAL_SLOTS]);

/* Human readable label for a kind, e.g. PIN_SPI_MOSI -> "MOSI". */
const char *pin_config_kind_label(pin_kind_t k);

/* True if `k` may be assigned to one of the five free IOs. */
bool pin_config_kind_is_permutable(pin_kind_t k);

/* ---- USB role for the USB-C port (NVS-backed, default OFF) ----
 * One PHY, three mutually-exclusive personalities:
 *   USB_MODE_OFF : native USB-Serial-JTAG free for flashing this board.
 *   USB_MODE_DAP : CMSIS-DAP v2 probe (dap_usb.c).
 *   USB_MODE_TTL : CDC-ACM virtual COM bridged to UART1 (usb_ttl.c).
 * Only the OFF -> active transition can take effect immediately; switching
 * between DAP and TTL (or back to OFF) takes effect at the next reboot,
 * because a live USB stack is never torn down. */
#define USB_MODE_OFF  0U
#define USB_MODE_DAP  1U
#define USB_MODE_TTL  2U

uint8_t pin_config_usb_mode(void);
esp_err_t pin_config_set_usb_mode(uint8_t mode);

/* Legacy boolean view: true == USB_MODE_DAP. */
bool pin_config_usb_dap(void);
esp_err_t pin_config_set_usb_dap(bool on);

#ifdef __cplusplus
}
#endif
