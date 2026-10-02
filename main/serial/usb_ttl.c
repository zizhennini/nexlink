/*
 * usb_ttl.c - USB-CDC "virtual COM" bridge to the DUT UART (TTL download mode).
 *
 * Enumerates the USB-C port as a standard CDC-ACM device (no MS OS tricks:
 * Win10+/Linux/macOS bind their in-box CDC driver and hand out a COM port),
 * and mirrors every byte between that COM port and UART1, the same pins the
 * TCP bridge uses. The three things that make it a real programmer cable
 * rather than a dumb echo box:
 *
 *   1. SET_LINE_CODING follows the host: the flash tool picks the baud and
 *      UART1 retunes (applied from task context, never from the USB IRQ).
 *   2. DTR/RTS drive BOOT/NRST with the polarity the classic auto-download
 *      circuit has, so esptool's reset sequence enters the ROM bootloader
 *      on ESP targets and a plain RTS pulse resets STM32 targets.
 *   3. DUT responses travel on their own 8 kB fan-out stream (serial_bridge)
 *      so web/TCP/MCP consumers can never starve the flash tool of ACKs.
 *   4. The CDC class-hook overrides further down are strong symbols that the
 *      whole firmware links against once -- which includes the CDC function
 *      that dap_usb.c registers in DAP mode.  They are therefore guarded with
 *      s_started: a host talking to the DAP build's COM port must never reach
 *      BOOT/NRST or the UART bridge (it would drive the SWD pins instead).
 *
 * Structure mirrors dap_usb.c on purpose (same CherryUSB port, same IRQ/task
 * hand-off idioms). One buffer per direction with NAK backpressure is enough
 * for the half-duplex block protocols the flash tools speak.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"

#include "usb_config.h"     /* ESP_USBD_BASE, CherryUSB's ESP32-S3 port config */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#include "pinout.h"
#include "pin_config.h"
#include "serial_bridge.h"
#include "usb_ttl.h"
#include "dap_usb.h"

static const char *TAG = "usb_ttl";

/* ------------------------------------------------------------------------- */
/* 1. Device identity and endpoint numbers                                    */
/* ------------------------------------------------------------------------- */

#define TTL_USBD_VID        0x303A   /* Espressif's vendor id */
#define TTL_USBD_PID        0x4001   /* the id IDF's CDC examples use */
#define TTL_USBD_MAX_POWER  500U     /* mA */
#define TTL_BUS_ID          0U

/* Same endpoint layout as CherryUSB's cdc_acm_template. */
#define TTL_IN_EP           0x81U    /* bulk  device -> host */
#define TTL_OUT_EP          0x02U    /* bulk  host -> device */
#define TTL_INT_EP          0x83U    /* interrupt, serial-state (unused)      */
#define TTL_MPS             64U      /* full-speed bulk max packet size       */
#define TTL_CHUNK           512U     /* transfer granularity, whole packets   */

/* ------------------------------------------------------------------------- */
/* 2. Descriptors                                                             */
/* ------------------------------------------------------------------------- */

#define TTL_CONFIG_SIZE  (9U + CDC_ACM_DESCRIPTOR_LEN)

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               TTL_USBD_VID, TTL_USBD_PID, 0x0100, 0x01),
};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(TTL_CONFIG_SIZE, 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, TTL_USBD_MAX_POWER),
    /* IAD + comm interface (notification EP) + data interface (bulk pair). */
    CDC_ACM_DESCRIPTOR_INIT(0x00, TTL_INT_EP, TTL_OUT_EP, TTL_IN_EP,
                            TTL_MPS, 0x02),
};

/* USB 2.0 full-speed device: the host may still ask for a qualifier. */
static const uint8_t s_device_quality_descriptor[] = {
    0x0A, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

_Static_assert(TTL_CHUNK % TTL_MPS == 0U,
               "chunk size must be a whole number of bulk packets");

static char s_serial_string[16];   /* filled in task context before start */

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
    case 0U: {
        static const char langid[2] = { 0x09, 0x04 };   /* en-US, UTF-16 pair */
        return langid;
    }
    case 1U:
        return "NexLink";
    case 2U:
        return "NexLink USB-TTL";
    case 3U:
        return (s_serial_string[0] != '\0') ? s_serial_string : NULL;
    default:
        return NULL;
    }
}

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_quality_descriptor;
}

static const struct usb_descriptor s_ttl_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/* ------------------------------------------------------------------------- */
/* 3. Bridge state and endpoint callbacks (USB interrupt context)             */
/* ------------------------------------------------------------------------- */

static bool s_started;
static volatile bool s_configured;
static uint32_t s_to_dut, s_to_host;

/* host -> DUT: one OUT buffer; re-armed by the task after the bytes land in
 * the UART driver, which NAK-backpressures the host while a write is in
 * flight. Perfectly adequate for block-oriented boot protocols. */
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_out_buf[TTL_CHUNK];
static volatile uint32_t s_out_len;
static volatile bool s_out_ready;

/* DUT -> host: one IN buffer plus the classic busy flag. */
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_in_buf[TTL_CHUNK];
static volatile bool s_in_busy;

/* CDC control state as reported by the host. */
static volatile bool s_dtr, s_rts;
static int s_nrst_io = -1;
static int s_boot_io = -1;

/* Line coding: stored for GET, applied by the task (uart_* is not ISR-safe). */
static struct cdc_line_coding s_line_coding = {
    .dwDTERate = 115200, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8
};
/* volatile: written by the USB IRQ, consumed by the bridge task. */
static volatile struct cdc_line_coding s_line_pending;
static volatile bool s_line_dirty;

static SemaphoreHandle_t s_wake;

static void wake_task(void)
{
    if (s_wake) {
        BaseType_t woke = pdFALSE;
        xSemaphoreGiveFromISR(s_wake, &woke);
        portYIELD_FROM_ISR(woke);
    }
}

/* Drive BOOT/NRST with the classic-circuit polarity:
 *   BOOT_n = !DTR, RST_n = !RTS   (idle / lines deasserted -> both released)
 * Both are push-pull outputs configured high at start. */
static void apply_line_state(void)
{
    if (s_nrst_io >= 0) gpio_set_level(s_nrst_io, s_rts ? 0 : 1);
    if (s_boot_io >= 0) gpio_set_level(s_boot_io, s_dtr ? 0 : 1);
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        s_configured = true;
        s_in_busy = false;
        usbd_ep_start_read(busid, TTL_OUT_EP, s_out_buf, TTL_CHUNK);
        break;
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        s_configured = false;
        break;
    default:
        break;
    }
}

static void ttl_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    if (!s_out_ready) {                    /* should not happen: armed until consumed */
        s_out_len = nbytes;
        s_out_ready = true;
        wake_task();
    }
}

static void ttl_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (nbytes > 0 && (nbytes % TTL_MPS) == 0) {
        /* Exact packet count: follow with a ZLP so short-read hosts end. */
        usbd_ep_start_write(busid, TTL_IN_EP, s_in_buf, 0);
        return;
    }
    s_in_busy = false;
    wake_task();
}

static struct usbd_endpoint s_out_endpoint = {
    .ep_addr = TTL_OUT_EP,
    .ep_cb = ttl_out_callback,
};

static struct usbd_endpoint s_in_endpoint = {
    .ep_addr = TTL_IN_EP,
    .ep_cb = ttl_in_callback,
};

static struct usbd_interface s_comm_intf;
static struct usbd_interface s_data_intf;

/* ------------------------------------------------------------------------- */
/* 4. CherryUSB class hooks (strong replacements of the __WEAK defaults)      */
/*    Called from the USB interrupt: record, wake the task, let IT apply.     */
/* ------------------------------------------------------------------------- */

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;
    if (!s_started) {
        /* DAP mode owns the CDC class now (composite descriptor); its serial
         * side is a placeholder.  Record nothing, wake nothing. */
        return;
    }
    s_line_pending = *line_coding;
    s_line_dirty = true;
    wake_task();
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf,
                                  struct cdc_line_coding *line_coding)
{
    (void)busid;
    (void)intf;
    *line_coding = s_line_coding;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    if (!s_started) {
        return;     /* DAP mode: the serial side is inert, do not touch NRST/BOOT */
    }
    s_dtr = dtr;
    apply_line_state();     /* gpio_set_level is ISR-safe */
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    (void)busid;
    (void)intf;
    if (!s_started) {
        return;
    }
    s_rts = rts;
    apply_line_state();
}

/* ------------------------------------------------------------------------- */
/* 5. Bridge task                                                             */
/* ------------------------------------------------------------------------- */

static void usb_ttl_task(void *pv)
{
    (void)pv;
    uint8_t local[TTL_CHUNK];

    for (;;) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(20));

        /* (a) host changed the line coding -> retune UART1 here, not in IT. */
        if (s_line_dirty) {
            s_line_coding = s_line_pending;
            s_line_dirty = false;
            uint32_t bps = s_line_coding.dwDTERate;
            if (bps >= 300U) {
                esp_err_t e = serial_bridge_set_baud(bps);
                ESP_LOGI(TAG, "host set %lu bps (parity %u stop %u bits %u) %s",
                         (unsigned long)bps, s_line_coding.bParityType,
                         s_line_coding.bCharFormat, s_line_coding.bDataBits,
                         e == ESP_OK ? "" : esp_err_to_name(e));
            }
        }

        /* (b) host -> DUT */
        if (s_out_ready) {
            uint32_t n = s_out_len;
            serial_bridge_write(s_out_buf, n);
            s_to_dut += n;
            s_out_ready = false;
            usbd_ep_start_read(TTL_BUS_ID, TTL_OUT_EP, s_out_buf, TTL_CHUNK);
        }

        /* (c) DUT -> host */
        if (s_configured && !s_in_busy) {
            size_t n = serial_bridge_read_usb(local, TTL_CHUNK, 0);
            if (n > 0) {
                memcpy(s_in_buf, local, n);
                s_in_busy = true;
                usbd_ep_start_write(TTL_BUS_ID, TTL_IN_EP, s_in_buf, n);
                s_to_host += n;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 6. Bring-up                                                                */
/* ------------------------------------------------------------------------- */

bool usb_ttl_is_started(void)
{
    return s_started;
}

uint32_t usb_ttl_get_to_dut(void)  { return s_to_dut;   }
uint32_t usb_ttl_get_to_host(void) { return s_to_host;  }

/* Manual reset pulse from the web "复位目标" button / /api/dut/reset.
 * Runs in the httpd task, so vTaskDelay is fine here (not the USB ISR).
 * enter_boot replicates esptool's ClassicReset: hold BOOT low across the
 * NRST release so an ESP target samples IO0=0 and stays in its ROM loader.
 * When done, the lines fall back to whatever the host last programmed via
 * DTR/RTS so we never fight a running flash tool. */
void usb_ttl_reset_pulse(int ms, bool enter_boot)
{
    if (s_nrst_io < 0) return;

    if (enter_boot && s_boot_io >= 0) gpio_set_level(s_boot_io, 0);  /* IO0=LOW */
    gpio_set_level(s_nrst_io, 0);                                    /* EN=LOW   */
    vTaskDelay(pdMS_TO_TICKS(ms < 1 ? 1 : ms));
    gpio_set_level(s_nrst_io, s_rts ? 0 : 1);                        /* EN back  */

    if (enter_boot && s_boot_io >= 0) {
        vTaskDelay(pdMS_TO_TICKS(50));          /* reset_delay: hold IO0 low   */
        gpio_set_level(s_boot_io, s_dtr ? 0 : 1);
    }
}

esp_err_t usb_ttl_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (dap_usb_is_started()) {
        /* The PHY is owned by the CMSIS-DAP stack this boot. Never tear it
         * down; switch the mode and reboot instead. */
        ESP_LOGE(TAG, "USB PHY is held by the DAP stack - reboot to switch");
        return ESP_ERR_INVALID_STATE;
    }

    /* Control lines: NRST is already an output (idle high) from swd_bridge;
     * the BOOT pin is whatever slot currently carries the GPIO function.
     * Wire NRST->target reset and this GPIO->target BOOT/IO0 for full
     * esptool-style auto-download. */
    s_nrst_io = pin_config_nrst();
    s_boot_io = pin_config_func_io(PIN_GPIO);
    if (s_boot_io >= 0) {
        gpio_set_direction(s_boot_io, GPIO_MODE_OUTPUT);
        gpio_set_level(s_boot_io, 1);      /* BOOT released (high) */
    }
    if (s_nrst_io >= 0) {
        gpio_set_level(s_nrst_io, 1);
    }

    /* Serial number from the WiFi MAC: the string callback runs in IT, so
     * build it here while we still are in task context. */
    {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_serial_string, sizeof(s_serial_string),
                 "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    s_wake = xSemaphoreCreateBinary();
    if (!s_wake) {
        return ESP_ERR_NO_MEM;
    }

    usbd_desc_register(TTL_BUS_ID, &s_ttl_descriptor);
    usbd_add_interface(TTL_BUS_ID, usbd_cdc_acm_init_intf(TTL_BUS_ID, &s_comm_intf));
    usbd_add_interface(TTL_BUS_ID, usbd_cdc_acm_init_intf(TTL_BUS_ID, &s_data_intf));
    usbd_add_endpoint(TTL_BUS_ID, &s_out_endpoint);
    usbd_add_endpoint(TTL_BUS_ID, &s_in_endpoint);

    if (usbd_initialize(TTL_BUS_ID, ESP_USBD_BASE, usbd_event_handler) != 0) {
        ESP_LOGE(TAG, "USB device stack failed to start");
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return ESP_FAIL;
    }

    if (xTaskCreate(usb_ttl_task, "usb_ttl", 6144, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create the bridge task");
        usbd_deinitialize(TTL_BUS_ID);
        vSemaphoreDelete(s_wake);
        s_wake = NULL;
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "USB-TTL bridge up: CDC COM <-> UART1 (RTS->NRST io%d, DTR->BOOT %s)",
             s_nrst_io,
             s_boot_io >= 0 ? "on the GPIO slot" : "no GPIO slot assigned");
    return ESP_OK;
}
