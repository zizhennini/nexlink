#pragma once
/*
 * usb_ttl.h - USB-CDC virtual COM bridge to the DUT UART ("TTL download" mode).
 *
 * With the USB mode set to TTL (Config page / pin_config_set_usb_mode), the
 * USB-C port enumerates as a standard CDC-ACM serial device, so host-side
 * flash tools (esptool, STM32 Flash Loader, any terminal) can talk to the
 * target MCU through the board's TX/RX level-shifted header pins - the board
 * becomes an ordinary USB-TTL dongle, plus:
 *
 *   - the host's baud rate (CDC SET_LINE_CODING) drives UART1 directly,
 *   - DTR/RTS are mapped to the target's BOOT/NRST lines following the
 *     classic two-transistor auto-download circuit:
 *         BOOT_n = !DTR   (the free IO currently assigned to the GPIO function)
 *         RST_n  = !RTS   (the fixed NRST pin)
 *     which is exactly the semantics esptool's ClassicReset drives, so an
 *     ESP target drops into its ROM bootloader without touching a button
 *     (BOOT must be wired to the GPIO slot; NRST already goes to J3).
 *
 * The USB PHY claim is permanent for the boot (same rule as dap_usb.c): the
 * mode is persisted and switching DAP <-> TTL takes effect after a reboot.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_ttl_start(void);
bool usb_ttl_is_started(void);

/* Byte counters for the status endpoint: host->DUT and DUT->host. */
uint32_t usb_ttl_get_to_dut(void);
uint32_t usb_ttl_get_to_host(void);

/* Pulse NRST for ms; enter_boot holds BOOT low across the release so an ESP
 * target enters its ROM download bootloader (needs started + wiring). */
void usb_ttl_reset_pulse(int ms, bool enter_boot);

#ifdef __cplusplus
}
#endif
