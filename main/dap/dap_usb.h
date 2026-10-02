/*
 * dap_usb.h - CMSIS-DAP v2 (WinUSB) transport.
 *
 * Presents the board as a USB debug probe: one vendor-specific interface with
 * a bulk OUT and a bulk IN endpoint, plus the Microsoft OS 2.0 descriptor set
 * that makes Windows bind WinUSB to it without an .inf file.  Requests are
 * handed to the vendored CMSIS-DAP core (dap/DAP.c), so the same code backs
 * both this transport and the TCP one in net/dap_server.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the USB device stack and start the DAP request task.
 *
 * Idempotent: if the probe is already live this returns ESP_OK immediately.
 * The probe is disabled only by clearing the persisted flag and rebooting
 * (a live USB device stack is never torn down mid-transfer).  Requires the
 * native USB peripheral to be free: the console must not be on
 * USB-Serial-JTAG (see sdkconfig.defaults).
 *
 * @return ESP_OK on success.
 */
esp_err_t dap_usb_start(void);

/**
 * @brief True while the USB device stack and DAP task are live (as opposed to
 * dap_usb_is_configured(), which needs a host to have enumerated us).
 */
bool dap_usb_is_started(void);

/**
 * @brief True once the host has enumerated the probe and set the configuration.
 *
 * This is what a "debugger connected" indicator should follow.
 */
bool dap_usb_is_configured(void);

/** @brief Number of command packets received from the host (diagnostics). */
uint32_t dap_usb_get_rx_packets(void);

/** @brief Number of response packets sent to the host (diagnostics). */
uint32_t dap_usb_get_tx_packets(void);

/** @brief Number of times the host successfully configured the probe. */
uint32_t dap_usb_configured_count(void);

/**
 * @brief Hex-dump the descriptors as the device really sends them.
 *
 * Diagnostic only (served as GET /api/usbdesc): makes it possible to verify
 * wTotalLength / subset lengths / BOS fields without a bus analyser.
 */
void dap_usb_dump_descriptors(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
