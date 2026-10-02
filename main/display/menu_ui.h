#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Menu surface IDs.
 *
 * These identify what the panel is showing; /api/status reports one as "page".
 * MENU_HOME is the root list; everything else is either a nested list or a
 * read-only screen selected from one. */
typedef enum {
    MENU_HOME = 0,     /* root list: the four groups                       */
    MENU_STATUS,       /* device state                                     */
    MENU_RX_MON,       /* live serial monitor                              */
    MENU_SWD,          /* SWD/JTAG pins + last IDCODE read                 */
    MENU_CONFIG,       /* kept for API compatibility                       */
    MENU_PWM,          /* PWM input + LEDC output                          */
    MENU_SPI,          /* SPI captured transactions                        */
    MENU_I2C,          /* I2C captured transactions                        */
    MENU_AI,           /* MCP readiness                                    */
    MENU_CAPTURE,      /* timestamped capture log summary                  */
    MENU_NET,          /* WiFi / IP / RSSI                                 */
    MENU_FIRMWARE,     /* firmware slot / OTA state / reset reason         */
    MENU_USB_STATE,    /* USB-C role and DAP counters                      */
    MENU_LIST_MONITOR, /* the "Monitor" list                               */
    MENU_LIST_PROBE,   /* the "Probe" list                                 */
    MENU_LIST_SYSTEM,  /* the "System" list                                */
    MENU_LIST_INFO,    /* the "Info" list                                  */
    MENU_COUNT
} menu_page_t;

/* Initialize the menu. Call after oled_init(). */
void menu_init(void);

/* Navigation input. ONE meaning everywhere:
 *   SW1 = up        SW3 = down        SW2 = select
 * SW2 held = jump to the root list from anywhere.
 *
 * Every nested list carries its own "Back" entry as item 0, so going back is a
 * visible, selectable item rather than an invisible mode - the pattern the
 * reference implementations use, and the reason there is no navigation stack
 * to get out of sync here. */
void menu_on_sw1_press(void);
void menu_on_sw3_press(void);
void menu_on_sw2_press(void);
void menu_on_sw2_long_press(void);

/* Feed serial RX/TX data to the RX monitor's line history. */
void menu_push_rx_data(const uint8_t *data, uint32_t len);
void menu_push_tx_data(const uint8_t *data, uint32_t len);

/* Simulate a button press from the HTTP API: b = 1 (SW1), 2 (SW2), 3 (SW3);
 * action = "press", or "long"/"hold" for the SW2 held gesture. */
void menu_simulate_button(int btn_id, const char *action);

/* Render the current surface and push the framebuffer to the panel.
 * Only the UI task may call this: the panel sits on a shared I2C bus. */
void menu_render(void);

/* Sleep up to `ms`, returning early when navigation happens (a key press
 * repaints immediately instead of waiting for the next tick). */
void menu_ui_wait(int ms);

/* ---- Introspection (used by the HTTP status API) ---- */
int  menu_get_current_page(void);
int  menu_get_selected(void);

/* Menu state as text, so the key model can be checked on a headless board:
 * e.g. "LIST System sel=2". The numeric page id alone cannot distinguish an
 * editor from the list it was opened from. */
void menu_get_state(char *buf, unsigned len);

void menu_clear_rx(void);
int  menu_get_rx_hist_n(void);
int  menu_get_rx_hist_max(void);
void menu_set_rx_hist_max(int m);

#ifdef __cplusplus
}
#endif
