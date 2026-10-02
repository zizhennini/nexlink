/*
 * NexLink - Main Entry Point
 * ESP32-S3-WROOM-1 (N16R8: 16MB quad flash + 8MB octal PSRAM)
 *
 * SSD1306 128x64 OLED menu UI + USB CMSIS-DAP probe + TCP serial bridge (3333)
 * + HTTP status / MCP API (80) + raw CMSIS-DAP over TCP (5555).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pinout.h"
#include "oled_ssd1306.h"
#include "menu_ui.h"
#include "wifi_manager.h"
#include "serial_bridge.h"
#include "capture.h"
#include "swd_bridge.h"
#include "pin_config.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "buttons.h"
#include "tcp_server.h"
#include "ws_server.h"
#include "dap_server.h"
#include "dap_usb.h"
/* debug_pins.h is intentionally not included here: the expansion-IO claim now
 * happens inside dap_usb_start(), so every path that enables the probe
 * arbitrates identically. */
#include "usb_ttl.h"
#include "http_status.h"

static const char *TAG = "main";

/* Reset reason of the PREVIOUS boot, captured first thing in app_main.
 * Surfaced via /api/status ("rst") and used by the USB safe-mode guard. */
static volatile esp_reset_reason_t s_boot_reason = ESP_RST_UNKNOWN;

const char *main_boot_reason(void)
{
    switch (s_boot_reason) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "ext-reset";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT-WDT";
    case ESP_RST_TASK_WDT: return "TASK-WDT";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    default:               return "other";
    }
}

static void on_serial_rx(const uint8_t *data, size_t len)
{
    tcp_server_broadcast(data, len);
    /* Push to WebSocket clients as it arrives: the browser no longer has to
     * poll /api/data, and short bursts are no longer coalesced or lost. */
    ws_broadcast_data(0, data, len);
    menu_push_rx_data(data, len);
}

static void on_button_event(button_id_t btn, button_event_t event)
{
    /* Navigation only - page functions (AP, baud, buffers) live in Config.
     * SW1=left=up/previous, SW3=right=down/next, SW2=middle=context. */
    if (event == BTN_EVENT_PRESS) {
        switch (btn) {
        case BTN_SW1: menu_on_sw1_press(); break;   /* up / previous page */
        case BTN_SW2: menu_on_sw2_press(); break;   /* enter / context    */
        case BTN_SW3: menu_on_sw3_press(); break;   /* down / next page   */
        default: break;
        }
    } else if (event == BTN_EVENT_HOLD) {
        /* SW2 held ~600 ms: cancel a config edit or slide back to home.
         * SW1/SW3 have no hold action (their release still fires a click). */
        if (btn == BTN_SW2) menu_on_sw2_long_press();
    }
    /* BTN_EVENT_RELEASE and the retired BTN_EVENT_LONG_PRESS are ignored. */
}

/* UI render task. Normally ticks at ~5Hz, but navigation kicks it awake
 * instantly so a page swap / slide starts the moment a button is released. */
static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        menu_render();
        menu_ui_wait(200);
    }
}

void app_main(void)
{
    s_boot_reason = esp_reset_reason();
    ESP_LOGI(TAG, "NexLink booting... (reset reason: %s)",
             main_boot_reason());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Display first so the user sees progress. A missing panel is not fatal:
     * the OLED is a convenience, every feature is also reachable over the
     * network interfaces. */
    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "OLED unavailable - continuing headless");
    }
    menu_init();

    wifi_manager_init();
    pin_config_init();
    /* Allocate the capture ring before the serial bridge starts producing
     * data; capture_record() is a no-op until this runs. */
    capture_init(CAPTURE_SLOTS_DEFAULT);
    serial_bridge_init(SERIAL_BAUD_DEFAULT);
    serial_bridge_set_rx_callback(on_serial_rx);
    swd_init();
    if (pin_config_pwm() >= 0) {
        pwm_mon_start(pin_config_pwm());
    }
    {
        int sck = pin_config_spi_sck(), mosi = pin_config_spi_mosi();
        int miso = pin_config_spi_miso(), cs = pin_config_spi_cs();
        if (sck >= 0 && mosi >= 0 && miso >= 0 && cs >= 0) {
            spi_mon_start(sck, mosi, miso, cs, 0);
        }
    }
    {
        int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
        if (sda >= 0 && scl >= 0) {
            i2c_mon_start(sda, scl, 0);
        }
    }
    buttons_init(on_button_event);
    tcp_server_start();
    http_status_start();
    dap_server_start();

    /* USB-C role - one PHY, three personalities (persisted).
     * OFF: the native USB peripheral and IO19/IO20 stay free, so the USB-C
     *      port works as USB-Serial-JTAG for flashing this board, and the PHY
     *      draws no current.
     * DAP: CMSIS-DAP v2 probe for Cortex-M targets.
     * TTL: CDC-ACM virtual COM bridged to UART1 - flash another MCU with
     *      esptool / STM32 Flash Loader straight off the header pins.
     * Pick it from the OLED Config page ("USB mode"), GET /api/usb_mode, or
     * the web control. Switching from OFF takes effect immediately; switching
     * between two active roles needs a reboot (a live USB stack is never torn
     * down). When DAP is on it is still started LAST so everything else
     * finishes touching SWCLK/SWDIO/nRESET first.
     *
     * Safe-mode guard: an active role that crashed the previous boot (panic
     * or watchdog) would just crash this one too, bricking remote access.
     * On an abnormal reset we force USB off for this boot and persist it -
     * the device stays reachable and the operator can retry deliberately. */
    uint8_t usb_mode = pin_config_usb_mode();
    if ((s_boot_reason == ESP_RST_PANIC || s_boot_reason == ESP_RST_INT_WDT ||
         s_boot_reason == ESP_RST_TASK_WDT) &&
        (usb_mode == USB_MODE_DAP || usb_mode == USB_MODE_TTL)) {
        ESP_LOGE(TAG, "abnormal reset (%s) while USB role was active - SAFE MODE: USB kept off this boot",
                 main_boot_reason());
        pin_config_set_usb_mode(USB_MODE_OFF);
        usb_mode = USB_MODE_OFF;
    }
    if (usb_mode == USB_MODE_DAP) {
        /* dap_usb_start() claims the expansion IOs the probe needs for
         * TDI/TDO/nTRST/SWO through debug_pins_init(), so every path that
         * enables the probe - boot, /api/usb_dap, /api/usb_mode, the OLED
         * Config page - arbitrates the pins the same way. A monitor holding
         * one of those IOs is stopped there and the reason shows up in the log
         * and in /api/status. */
        if (dap_usb_start() != ESP_OK) {
            ESP_LOGW(TAG, "USB CMSIS-DAP probe unavailable - SWD over TCP only");
        }
    } else if (usb_mode == USB_MODE_TTL) {
        if (usb_ttl_start() != ESP_OK) {
            ESP_LOGW(TAG, "USB-TTL bridge unavailable");
        }
    } else {
        ESP_LOGI(TAG, "USB off: IO19/IO20 free (Config page or /api/usb_mode to pick DAP/TTL)");
    }

    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "All subsystems initialized");
    ESP_LOGI(TAG, "HTTP: http://<ip>:80  TCP: tcp://<ip>:3333");

    /* OTA rollback check-in. Reaching this line means the image booted and
     * every subsystem came up, so confirm it as healthy. An image that came
     * through /api/ota starts PENDING_VERIFY and would be auto-reverted by
     * the bootloader if it crashed before getting here. */
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (running &&
            esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "first boot of OTA image %s: %s", running->label,
                     e == ESP_OK ? "confirmed, rollback cancelled"
                                 : esp_err_to_name(e));
        }
    }
#endif
}
