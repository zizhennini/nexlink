/*
 * usbtrace.h - USB enumeration trace for diagnostics without a console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Outcomes recorded for one SETUP packet. */
#define USBTRACE_OUTCOME_UNKNOWN 0U
#define USBTRACE_OUTCOME_OK      1U
#define USBTRACE_OUTCOME_STALL   2U
#define USBTRACE_OUTCOME_PENDING 3U

/**
 * @brief Record one control-transfer SETUP packet.  ISR context.
 *
 * @param setup8  the 8 raw SETUP bytes
 * @param outcome one of USBTRACE_OUTCOME_*
 */
void usbtrace_setup(const uint8_t *setup8, uint8_t outcome);

/** @brief Update the outcome of the most recent SETUP.  ISR context. */
void usbtrace_setup_result(uint8_t outcome);

/** @brief Record a failed endpoint open (ep address + port return code). */
void usbtrace_ep_fail(uint8_t ep_addr, int rc);

/* Event codes for usbtrace_event(). */
#define USBTRACE_EV_EP0_IN   1U   /* a=nbytes sent, b=residue left, c=wLength */
#define USBTRACE_EV_EP0_SETUP 2U  /* a=wLength, b=bRequest, c=wValue */

/** @brief Record a misc event (EP0 IN completions etc.).  ISR context. */
void usbtrace_event(uint8_t code, uint16_t a, uint16_t b, uint16_t c);

/** @brief Render the trace as plain text into out (NUL-terminated). */
void usbtrace_dump(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
