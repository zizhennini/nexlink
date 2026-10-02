#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SWD bridge API (GPIO bit-bang) ---- */

/* Configure SWCLK=IO13 (out), SWDIO=IO14 (open-drain bi-dir), NRST=IO12 (out). */
esp_err_t swd_init(void);

/* >50 SWCLK cycles with SWDIO high. */
void swd_line_reset(void);

/* Write the JTAG-to-SWD selection sequence (line 0x79E7, sent LSB-first as
 * 0xE79E, i.e. the wire pattern 0111 1001 1110 0111). */
void swd_jtag_to_swd(void);

/* Read DP IDCODE (DP reg 0). Returns ESP_OK on success. */
esp_err_t swd_read_idcode(uint32_t *idcode);

/* DP / AP register access. addr = 0..3 (banks handled via SELECT). */
esp_err_t swd_read_dp(uint32_t addr, uint32_t *data);
esp_err_t swd_write_dp(uint32_t addr, uint32_t data);
esp_err_t swd_read_ap(uint32_t addr, uint32_t *data);
esp_err_t swd_write_ap(uint32_t addr, uint32_t data);

/* Assert (1) or release (0) hardware NRST. */
esp_err_t swd_reset_target(bool assert);

/* Last SWD ACK value (diagnostic): 1=OK, 2=WAIT, 4=FAULT, 0/7=no-response. */
uint32_t swd_get_last_ack(void);

/*
 * Shared bus lock for the three SWD pads. The TCP DAP path, the USB DAP path
 * (DAP.c) and the menu/HTTP diagnostics all bit-bang the same pins; any
 * caller that performs a multi-step sequence must hold this lock for the
 * whole sequence. Re-entrant callers must not take it twice.
 */
void swd_bus_lock(void);
void swd_bus_unlock(void);

/*
 * JSON SWD command handler.
 *   cmd:       "reset", "read_id", "read_dp", "write_dp", "read_ap", "write_ap"
 *   addr_hex:  hex address (without 0x), used for dp/ap reads/writes
 *   data_hex:  hex data (without 0x), used for writes
 *   resp:      output buffer for the JSON response string
 *   resp_len:  size of resp buffer
 *
 * Returns ESP_OK on recognised command, ESP_ERR_INVALID_ARG otherwise.
 */
esp_err_t swd_bridge_handle(const char *cmd,
                            const char *addr_hex,
                            const char *data_hex,
                            char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif
