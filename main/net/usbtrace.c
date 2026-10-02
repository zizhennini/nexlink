/*
 * usbtrace.c - a tiny USB enumeration trace that survives without a console.
 *
 * The board's UART0 console is not always reachable (cable, wiring, or the
 * header being used for something else), but GET /api/usbtrace is: it returns
 * the last N control-transfer SETUP packets the device saw, the endpoint opens
 * that failed, and one entry per EP0 IN packet that the host actually
 * acknowledged.  That is enough to tell exactly how far a host got during
 * enumeration, which request was stalled, and whether a multi-packet control
 * transfer (e.g. the 172-byte MS OS descriptor set: 64+64+44) really went out.
 *
 * Companion endpoint: GET /api/usbdesc (dap_usb.c) dumps the descriptors as
 * the device truly sends them, with every declared length field parsed out.
 *
 * The recorders are invoked from three hooks compiled into the vendored
 * CherryUSB core (managed_components/cherry-embedded__cherryusb/core/usbd_-
 * core.c: the SETUP entry of __usbd_event_ep0_setup_complete_handler, the
 * failure branch of usbd_set_endpoint, and usbd_event_ep0_in_complete_-
 * handler).  When the component is ever updated, keep those hooks or delete
 * them together with this module -- they are diagnostics, not behavior.
 *
 * Recorders run from the USB interrupt, so they only memcpy into a static ring
 * and never log, allocate or take a lock on the non-ISR path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "usbtrace.h"

#define USBTRACE_ENTRIES 48U

typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint8_t  outcome;      /* see USBTRACE_* */
} usbtrace_entry_t;

typedef struct {
    uint8_t ep_addr;
    int     rc;
    uint32_t count;
} usbtrace_epfail_t;

static volatile usbtrace_entry_t s_ring[USBTRACE_ENTRIES];
static volatile uint32_t s_write;      /* next slot to fill */
static volatile uint32_t s_total;      /* total SETUPs seen */

static volatile usbtrace_epfail_t s_epfails[8];
static volatile uint32_t s_epfail_count;

void usbtrace_setup(const uint8_t *setup8, uint8_t outcome)
{
    uint32_t slot = s_write;

    if (slot >= USBTRACE_ENTRIES) {
        slot = 0U;                     /* wrap defensively */
    }

    s_ring[slot].bmRequestType = setup8[0];
    s_ring[slot].bRequest      = setup8[1];
    s_ring[slot].wValue        = (uint16_t)setup8[2] | ((uint16_t)setup8[3] << 8);
    s_ring[slot].wIndex        = (uint16_t)setup8[4] | ((uint16_t)setup8[5] << 8);
    s_ring[slot].wLength       = (uint16_t)setup8[6] | ((uint16_t)setup8[7] << 8);
    s_ring[slot].outcome       = outcome;

    slot++;
    if (slot >= USBTRACE_ENTRIES) {
        slot = 0U;
    }
    s_write = slot;
    s_total++;
}

void usbtrace_setup_result(uint8_t outcome)
{
    uint32_t slot = s_write;

    if (slot == 0U) {
        slot = USBTRACE_ENTRIES;
    }
    s_ring[slot - 1U].outcome = outcome;
}

void usbtrace_ep_fail(uint8_t ep_addr, int rc)
{
    uint32_t n = s_epfail_count;

    for (uint32_t i = 0U; i < 8U; i++) {
        if (s_epfails[i].count != 0U && s_epfails[i].ep_addr == ep_addr) {
            s_epfails[i].rc = rc;
            s_epfails[i].count++;
            return;
        }
    }
    if (n >= 8U) {
        n = 7U;
    }
    s_epfails[n].ep_addr = ep_addr;
    s_epfails[n].rc = rc;
    s_epfails[n].count++;
    s_epfail_count = n + 1U;
}

/* --- event log (EP0 IN completions etc.) ---------------------------------- */

#define USBTRACE_EVENTS 32U

typedef struct {
    uint8_t  code;
    uint16_t a;
    uint16_t b;
    uint16_t c;
} usbtrace_event_t;

static volatile usbtrace_event_t s_events[USBTRACE_EVENTS];
static volatile uint32_t s_event_write;
static volatile uint32_t s_event_total;

void usbtrace_event(uint8_t code, uint16_t a, uint16_t b, uint16_t c)
{
    uint32_t slot = s_event_write;

    if (slot >= USBTRACE_EVENTS) {
        slot = 0U;
    }
    s_events[slot].code = code;
    s_events[slot].a = a;
    s_events[slot].b = b;
    s_events[slot].c = c;
    slot++;
    if (slot >= USBTRACE_EVENTS) {
        slot = 0U;
    }
    s_event_write = slot;
    s_event_total++;
}

void usbtrace_dump(char *out, size_t out_len)
{
    static const char *outcome_s[] = { "?", "ok", "STALL", "pending" };
    size_t used = 0;
    uint32_t total = s_total;
    uint32_t count = (total < USBTRACE_ENTRIES) ? total : USBTRACE_ENTRIES;
    uint32_t start = (total >= USBTRACE_ENTRIES) ? s_write : 0U;

    used += (size_t)snprintf(out + used, out_len - used,
                             "setups=%lu shown=%lu\n"
                             "idx bmRequestType bRequest wValue wIndex wLength outcome\n",
                             (unsigned long)total, (unsigned long)count);

    for (uint32_t i = 0U; i < count && used + 96U < out_len; i++) {
        uint32_t idx = (start + i) % USBTRACE_ENTRIES;
        uint8_t oc = s_ring[idx].outcome;
        const char *ocs = (oc < 4U) ? outcome_s[oc] : "?";

        used += (size_t)snprintf(out + used, out_len - used,
                                 "%02lx  %02x %02x %04x %04x %04x %s\n",
                                 (unsigned long)i,
                                 s_ring[idx].bmRequestType, s_ring[idx].bRequest,
                                 s_ring[idx].wValue, s_ring[idx].wIndex,
                                 s_ring[idx].wLength, ocs);
    }

    used += (size_t)snprintf(out + used, out_len - used, "ep_open_failures=%lu\n",
                             (unsigned long)s_epfail_count);
    for (uint32_t i = 0U; i < s_epfail_count && used + 64U < out_len; i++) {
        used += (size_t)snprintf(out + used, out_len - used,
                                 "  ep 0x%02x rc=%d count=%lu\n",
                                 s_epfails[i].ep_addr, s_epfails[i].rc,
                                 (unsigned long)s_epfails[i].count);
    }

    /* EP0 IN completions: code 1 = data packet sent (a=nbytes, b=residue left,
     * c=wLength of the request), code 2 = setup/status stage. */
    {
        uint32_t etotal = s_event_total;
        uint32_t ecount = (etotal < USBTRACE_EVENTS) ? etotal : USBTRACE_EVENTS;
        uint32_t estart = (etotal >= USBTRACE_EVENTS) ? s_event_write : 0U;

        used += (size_t)snprintf(out + used, out_len - used,
                                 "ep0_events=%lu\n", (unsigned long)etotal);
        for (uint32_t i = 0U; i < ecount && used + 64U < out_len; i++) {
            uint32_t idx = (estart + i) % USBTRACE_EVENTS;
            used += (size_t)snprintf(out + used, out_len - used,
                                     "  code=%u a=%u b=%u c=%u\n",
                                     s_events[idx].code, s_events[idx].a,
                                     s_events[idx].b, s_events[idx].c);
        }
    }
}
