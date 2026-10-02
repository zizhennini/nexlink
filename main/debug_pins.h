#pragma once
/*
 * debug_pins.h - ownership of the expansion IOs used by the debug probe.
 *
 * WHY THIS EXISTS
 * ---------------
 * NRST / SWCLK / SWDIO live on dedicated, non-permutable pins (IO12/13/14),
 * but the board has no spare dedicated pins left for the rest of the debug
 * probe. Everything else has to come out of the five expansion IOs
 * (IO48/45/38/39/40) - the same pins the protocol monitors want:
 *
 *      TDI     JTAG data in      (bit-banged by the DAP core)
 *      TDO     JTAG data out
 *      nTRST   JTAG test reset
 *      SWO     trace / ITM single wire (UART RX at the target's SWO rate)
 *
 * Those five IOs are only five, and SPI/I2C capture is the board's other main
 * job, so this module does not hard-reserve them for one purpose or the other.
 * It implements ownership instead:
 *
 *   - debug_pins_init() runs after the monitors have started. Any monitor
 *     holding one of the debug IOs is STOPPED and released, because the probe
 *     takes priority over capturing on the same physical pin.
 *   - The claim is reported through /api/status and the log, so "why did my
 *     SPI capture stop?" always has a visible answer.
 *   - main/CMakeLists.txt builds this file unconditionally, so JTAG and SWO
 *     work out of the box with no configuration.
 *
 * If the debug signals are later moved to different IOs, only DEBUG_PIN_* in
 * pinout.h has to change.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Take ownership of the debug IOs.
 *
 * Must be called AFTER the monitors/protocol drivers have started and BEFORE
 * anything drives these pins, i.e. right before dap_usb_start() in app_main.
 * Safe to call more than once (the second call is a no-op).
 *
 * Returns the number of IOs actually claimed (0 if JTAG/SWO were compiled out
 * or no free IO was left). */
int debug_pins_init(void);

/* True when the debug probe owns the expansion IOs. */
bool debug_pins_claimed(void);

/* Which GPIO carries one of the debug signals; -1 when none. The signals come
 * from pinout.h and are compile-time fixed, so these never move at runtime. */
int debug_pins_tdi(void);
int debug_pins_tdo(void);
int debug_pins_ntrst(void);
int debug_pins_swo(void);

/* True if `io` is one of the debug probe's IOs. pin_config uses this to tell
 * the user when a permutation assignment collides with the probe. */
bool debug_pins_is_debug_io(int io);

/* Comma separated list of the claimed IOs for the status JSON, e.g. "48,38,39".
 * Writes at most `len` bytes; always NUL terminated. */
void debug_pins_report(char *buf, unsigned len);

#ifdef __cplusplus
}
#endif
