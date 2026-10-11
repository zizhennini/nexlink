#pragma once
/*
 * dash_ui.h - the board's OLED interface: a single-screen dashboard.
 *
 * One screen per FUNCTION, not per data value. Whatever the firmware can do is
 * reachable on the panel; whatever the panel shows is something the firmware
 * actually measures. No menu tree, no cursor, no sub-lists, no scrolling - the
 * panel is exactly four 16px rows, so a screen is four rows and nothing can
 * overlap by construction.
 *
 * Key map, identical on every screen:
 *   SW1        previous screen
 *   SW3        next screen
 *   SW2        that screen's action (never a no-op)
 *   SW2 held   back to the first screen
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the interface up. Call after oled_init(). */
void dash_init(void);

/* One-time "NexLink" boot animation, ~1.4 s. Call once from the UI task
 * before its first dash_wait()/dash_render(); safe no-op with no panel. */
void dash_boot_animation(void);

/* Keep the RX screen's line history fed from the serial bridge. */
void dash_push_rx(const uint8_t *data, uint32_t len);
void dash_push_tx(const uint8_t *data, uint32_t len);

/* Drop the retained RX lines (POST /api/clear?what=rx). */
void dash_clear_rx(void);

/* Buttons. */
void dash_next_screen(void);       /* SW3 */
void dash_prev_screen(void);       /* SW1 */
void dash_action(void);            /* SW2 */
void dash_home(void);              /* SW2 held */

/* Simulated button press from the HTTP API: b = 1 (SW1), 2 (SW2), 3 (SW3);
 * action = "press", or "long"/"hold" for the held-SW2 gesture. */
void dash_simulate_button(int btn_id, const char *action);

/* Render the current screen and push it to the panel. UI task only: the panel
 * sits on an I2C bus shared with the I2C monitor, so there is exactly one
 * writer. */
void dash_render(void);

/* Sleep up to `ms`, waking early when a button changes something. */
void dash_wait(int ms);

/* ---- Introspection (used by /api/status) ---- */
int  dash_get_screen(void);
int  dash_get_screen_count(void);
void dash_get_state(char *buf, unsigned len);

#ifdef __cplusplus
}
#endif
