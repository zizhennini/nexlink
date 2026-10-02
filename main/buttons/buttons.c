#include "buttons.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "pinout.h"

static const char *TAG = "btn";

/* ---- Timing constants ---- */
#define BTN_DEBOUNCE_MS     20      /* debounce window */
#define BTN_HOLD_MS         600     /* a press held this long fires BTN_EVENT_HOLD
                                     * (SW2: back to home). There is deliberately
                                     * NO double-click window any more: a short
                                     * press is delivered the instant the button
                                     * is released, so navigation feels immediate. */
#define BTN_QUEUE_LEN       16
/* 2048 was too tight: the callback runs menu actions in this task, and pages
 * like SWD (bit-bang IDCODE read) or the I2C mode toggle need real headroom. */
#define BTN_TASK_STACK      4096
#define BTN_TASK_PRIO       10

/* ISR -> debounce task message */
typedef struct {
    button_id_t btn;
    uint32_t   tick_isr;   /* xTaskGetTickCountFromISR() at interrupt time */
} btn_isr_msg_t;

/* Per-button runtime state (debounce task only) */
typedef struct {
    bool      pressed;             /* current debounced level (true=held) */
    uint32_t  press_tick;          /* tick when press confirmed */
    int       gpio;                /* GPIO number */
    bool      hold_fired;          /* HOLD event already emitted for this press */
} btn_state_t;

static btn_state_t            s_btn[BTN_COUNT];
static button_callback_t      s_cb        = NULL;
static QueueHandle_t          s_queue     = NULL;
static TaskHandle_t           s_task      = NULL;
static bool                   s_inited    = false;

/* GPIO number per button (from pinout.h) */
static const int s_gpio_map[BTN_COUNT] = {
    [BTN_SW1] = PIN_BTN_SW1,
    [BTN_SW2] = PIN_BTN_SW2,
    [BTN_SW3] = PIN_BTN_SW3,
};

/* ---- ISR: forward edge events to the queue ---- */
static void IRAM_ATTR btn_isr_handler(void *arg)
{
    button_id_t btn = (button_id_t)(intptr_t)arg;
    btn_isr_msg_t msg = {
        .btn      = btn,
        .tick_isr = xTaskGetTickCountFromISR(),
    };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_queue, &msg, &hp);
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---- Debounce / event-detection task ----
 * Emits:
 *   BTN_EVENT_PRESS       short click, fired immediately on release
 *   BTN_EVENT_HOLD        fired once while still held after BTN_HOLD_MS
 *   BTN_EVENT_RELEASE     on every release
 * There is no double-click path any more, so a click never waits. */
static void btn_task(void *arg)
{
    (void)arg;
    btn_isr_msg_t msg;

    while (1) {
        /* Block until an edge event arrives. While a button is held the loop
         * re-arms itself with a poll message, so no timer is needed. */
        BaseType_t got = xQueueReceive(s_queue, &msg, portMAX_DELAY);

        if (got == pdPASS) {
            button_id_t btn = msg.btn;
            if (btn >= 0 && btn < BTN_COUNT) {
                btn_state_t *st = &s_btn[btn];

                /* ---- Debounce: re-read the pin after the window ---- */
                vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));

                int level = gpio_get_level(st->gpio);
                bool now_pressed = (level == 0);   /* active low */
                uint32_t t = xTaskGetTickCount();

                if (now_pressed && !st->pressed) {
                    /* ---- falling edge confirmed: press begins ---- */
                    st->pressed    = true;
                    st->press_tick = t;
                } else if (!now_pressed && st->pressed) {
                    /* ---- rising edge confirmed: the click lands NOW ---- */
                    st->pressed = false;
                    if (st->hold_fired) {
                        /* A HOLD already fired while the button was down.
                         * SW2's HOLD *is* its action (back to home), so a long
                         * hold must not also fire a click on release. SW1/SW3
                         * have no HOLD action: their release still counts as a
                         * normal click so they never feel dead. */
                        st->hold_fired = false;
                        if (btn != BTN_SW2 && s_cb)
                            s_cb(btn, BTN_EVENT_PRESS);
                    } else if (s_cb) {
                        s_cb(btn, BTN_EVENT_PRESS);
                    }
                    if (s_cb) s_cb(btn, BTN_EVENT_RELEASE);
                }
                /* else: no state change (bounce, or still held) */

                /* ---- Hold detection + keep polling while held ---- */
                if (st->pressed) {
                    if (!st->hold_fired &&
                        (t - st->press_tick) * portTICK_PERIOD_MS >= BTN_HOLD_MS) {
                        st->hold_fired = true;
                        if (s_cb) s_cb(btn, BTN_EVENT_HOLD);
                    }
                    btn_isr_msg_t poll = { .btn = btn, .tick_isr = t };
                    xQueueSend(s_queue, &poll, 0);
                }
            }
        }
    }
}

/* ---- Public API ---- */

esp_err_t buttons_init(button_callback_t callback)
{
    if (s_inited) {
        ESP_LOGW(TAG, "already initialised");
        s_cb = callback;   /* allow callback swap */
        return ESP_OK;
    }

    memset(s_btn, 0, sizeof(s_btn));
    for (int i = 0; i < BTN_COUNT; i++) {
        s_btn[i].gpio = s_gpio_map[i];
    }
    s_cb = callback;

    /* ---- Create the ISR->task queue ---- */
    s_queue = xQueueCreate(BTN_QUEUE_LEN, sizeof(btn_isr_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    /* ---- Configure each button GPIO ---- */
    for (int i = 0; i < BTN_COUNT; i++) {
        int io = s_gpio_map[i];
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << io),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,   /* active low: falling edge */
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for IO%d: %s", io, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "SW%d on IO%d configured (active-low, pull-up, falling-edge)",
                 i + 1, io);
    }

    /* ---- Install GPIO ISR service & hook handlers ---- */
    /* Use no-flags install so we don't clobber other components' ISRs */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
        return isr_err;
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        esp_err_t err = gpio_isr_handler_add(s_gpio_map[i],
                                            btn_isr_handler,
                                            (void *)(intptr_t)i);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_isr_handler_add IO%d: %s",
                     s_gpio_map[i], esp_err_to_name(err));
            return err;
        }
    }

    /* ---- Launch the debounce task ---- */
    BaseType_t t = xTaskCreate(btn_task, "btn_debounce",
                               BTN_TASK_STACK, NULL, BTN_TASK_PRIO, &s_task);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate btn_debounce failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "buttons driver ready (debounce=%dms, hold=%dms, instant click)",
             BTN_DEBOUNCE_MS, BTN_HOLD_MS);
    return ESP_OK;
}

bool button_is_pressed(button_id_t btn)
{
    if (btn < 0 || btn >= BTN_COUNT || !s_inited) {
        return false;
    }
    /* Active low: pressed => level 0 */
    return gpio_get_level(s_btn[btn].gpio) == 0;
}
