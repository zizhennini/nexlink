#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Menu surface IDs.
 *
 * These identify what the panel is showing; they are also what /api/status
 * reports as "page". The menu itself is a tree (see menu_ui.c): MENU_HOME is
 * the root list, the MENU_LIST_* ids are list levels, and the remaining ids
 * are read-only screens that a list entry can open. */
typedef enum {
    MENU_HOME = 0,     /* root list: the four groups                       */

    /* Read-only / self-contained screens. Every id here must be unique: the
     * HTTP layer reports it verbatim as /api/status "page", so two surfaces
     * sharing a value would make the field ambiguous. */
    MENU_STATUS,       /* device state (uptime, baud, counters)            */
    MENU_RX_MON,       /* live serial monitor                              */
    MENU_SWD,          /* SWD/JTAG pins + last IDCODE read                 */
    MENU_CONFIG,       /* kept for API compatibility (see README)          */
    MENU_PWM,          /* PWM input + LEDC output                          */
    MENU_SPI,          /* SPI captured transactions                        */
    MENU_I2C,          /* I2C captured transactions                        */
    MENU_AI,           /* MCP readiness                                    */
    MENU_CAPTURE,      /* timestamped capture log summary                  */
    MENU_NET,          /* WiFi / IP / RSSI                                 */
    MENU_FIRMWARE,     /* firmware slot / OTA state / reset reason         */
    MENU_USB_STATE,    /* USB-C role and DAP counters                      */

    /* List levels of the tree. */
    MENU_LIST_MONITOR,
    MENU_LIST_PROBE,
    MENU_LIST_SYSTEM,
    MENU_LIST_INFO,

    MENU_COUNT
} menu_page_t;

/* Initialize the menu UI system. Call after oled_init(). */
void menu_init(void);

/* Navigation input from buttons. ONE meaning everywhere:
 *   SW1 = up      SW3 = down      SW2 = enter / back
 *   SW2 held      = jump home from anywhere (including edit mode)
 * On a read-only screen SW1/SW3 scroll the body when it has one. */
void menu_on_sw1_press(void);       /* up / scroll up */
void menu_on_sw3_press(void);       /* down / scroll down */
void menu_on_sw2_press(void);       /* enter / back / confirm edit */
void menu_on_sw2_long_press(void);  /* universal escape -> HOME */

/* Feed serial RX data to the RX monitor page */
void menu_push_rx_data(const uint8_t *data, uint32_t len);

/* Echo serial TX data (e.g. sent from the web UI) into the RX monitor page,
 * prefixed with '<' so it is distinguishable from received ('>') data. */
void menu_push_tx_data(const uint8_t *data, uint32_t len);

/* Simulate button press from web API: 1=SW1, 2=SW2, 3=SW3.
 * action: "press", or "long"/"hold" (SW2 long gesture only) */
void menu_simulate_button(int btn_id, const char *action);

/* Render the current page and push the framebuffer to the panel.
 * Call periodically (e.g. 5Hz from the UI task). Also plays any pending
 * page-swap slide animation. */
void menu_render(void);

/* Sleep up to `ms` milliseconds, returning early when navigation happens
 * (page swaps and context actions repaint instantly instead of on the next
 * poll tick). Call from the UI task between menu_render() invocations. */
void menu_ui_wait(int ms);

/* ---- Introspection (used by the HTTP status API) ---- */
int  menu_get_current_page(void);
int  menu_get_selected(void);

/* Menu state as text, for /api/status and for debugging the key model on a
 * headless board: "view=LIST TITLE sel=2". The page id alone cannot
 * distinguish an editor from the list it was opened from. */
void menu_get_state(char *buf, unsigned len);
int  menu_get_rx_hist_n(void);
int  menu_get_rx_hist_max(void);
void menu_set_rx_hist_max(int m);
void menu_clear_rx(void);

#ifdef __cplusplus
}
#endif
