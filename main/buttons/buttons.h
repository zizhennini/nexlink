#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Button IDs ---- */
typedef enum {
    BTN_SW1 = 0,
    BTN_SW2,
    BTN_SW3,
    BTN_COUNT
} button_id_t;

/* ---- Button Events ---- */
typedef enum {
    BTN_EVENT_PRESS = 0,      /* short click, fired immediately on release */
    BTN_EVENT_LONG_PRESS,     /* DEPRECATED: no longer emitted (double-click
                               * removed to keep presses instant). Kept only so
                               * existing switch statements still compile. */
    BTN_EVENT_HOLD,           /* fired once while held past BTN_HOLD_MS (~600 ms) */
    BTN_EVENT_RELEASE,        /* rising edge after press */
} button_event_t;

/* ---- Callback ---- */
typedef void (*button_callback_t)(button_id_t btn, button_event_t event);

/* ---- API ---- */

/*
 * Initialise 3 active-low user buttons (SW1=IO1, SW2=IO2, SW3=IO42) as
 * GPIO inputs with internal pull-ups and falling-edge interrupts.
 * Creates an ISR->queue->debounce-task pipeline; events are delivered
 * to the supplied callback from the debounce task context (NOT ISR).
 *
 * Pass NULL to de-initialise a previously registered callback.
 */
esp_err_t buttons_init(button_callback_t callback);

/*
 * Return true if the given button is currently held down (active-low == 0).
 * Safe to call from any task.
 */
bool button_is_pressed(button_id_t btn);

#ifdef __cplusplus
}
#endif
