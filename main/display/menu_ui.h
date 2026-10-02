#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Menu page IDs */
typedef enum {
    MENU_HOME = 0,    /* icon grid: RX / I2C / SPI / PWM / PIN / STA / CFG / AI */
    MENU_STATUS,      /* status page: WiFi/IP/baud/counters */
    MENU_RX_MON,      /* live RX data monitor */
    MENU_SWD,         /* SWD pinout + IDCODE read */
    MENU_CONFIG,      /* config page */
    MENU_PWM,         /* PWM monitor: frequency + duty cycle */
    MENU_SPI,         /* SPI slave monitor: captured bytes */
    MENU_I2C,         /* I2C bus monitor: captured transactions */
    MENU_AI,          /* AI assistant page */
    MENU_COUNT
} menu_page_t;

/* Initialize the menu UI system. Call after oled_init(). */
void menu_init(void);

/* Navigation input from buttons.
 * Model: SW1=left key = up/previous, SW3=right key = down/next, SW2=middle =
 * context action. On MENU_HOME the side keys move the list cursor and SW2
 * enters; on a sub-page the side keys cycle pages (animated vertical slide)
 * and SW2-held (menu_on_sw2_long_press) returns home. On MENU_CONFIG the side
 * keys are the item cursor / value adjust and SW2 executes or confirms. */
void menu_on_sw1_press(void);       /* up / previous */
void menu_on_sw3_press(void);       /* down / next */
void menu_on_sw2_press(void);       /* enter subpage / toggle mode / confirm */
void menu_on_sw2_long_press(void);  /* SW2 held: universal escape -> HOME */

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
int  menu_get_rx_hist_n(void);
int  menu_get_rx_hist_max(void);
void menu_set_rx_hist_max(int m);
void menu_clear_rx(void);

#ifdef __cplusplus
}
#endif
