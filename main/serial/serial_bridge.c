#include "serial_bridge.h"

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "pinout.h"
#include "pin_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "serial";

/* ---- State ---- */
static StreamBufferHandle_t s_rx_stream  = NULL;
static StreamBufferHandle_t s_mcp_stream = NULL;   /* independent copy for MCP */
static StreamBufferHandle_t s_usb_stream = NULL;   /* independent copy for USB-TTL */
static size_t               s_buf_size   = SERIAL_BUF_SIZE;
static QueueHandle_t         s_uart_queue = NULL;
static TaskHandle_t           s_task       = NULL;
static serial_rx_cb_t         s_rx_cb      = NULL;
static uint32_t               s_rx_count   = 0;
static uint32_t               s_tx_count   = 0;
static bool                   s_init_done  = false;

/* The USB-TTL stream keeps its own fixed, generous size: during a serial
 * flash the DUT's ACK stream must not be squeezed by web/MCP consumers, and
 * 8 kB covers esptool's block sizes with wide margin at 921600. */
#define SERIAL_USB_STREAM_BYTES 8192

/* ------------------------------------------------------------------ */
/*  Atomic-ish counters (single-core read/modify on ESP32-S3 is fine   */
/*  for this use-case; these are informational only).                  */
/* ------------------------------------------------------------------ */
static inline void inc_rx(uint32_t n) { s_rx_count += n; }
static inline void inc_tx(uint32_t n) { s_tx_count += n; }

/* ------------------------------------------------------------------ */
/*  UART event task                                                    */
/* ------------------------------------------------------------------ */

/* Append to an independent fan-out stream, dropping oldest bytes when the
 * stream is full (same policy as the main RX stream). */
static void fanout_append(StreamBufferHandle_t sb, const uint8_t *data, size_t len)
{
    if (!sb) return;
    size_t space = xStreamBufferSpacesAvailable(sb);
    if (len > space) {
        uint8_t d[64];
        size_t need = len - space;
        while (need > 0) {
            size_t r = xStreamBufferReceive(sb, d,
                                            need > sizeof(d) ? sizeof(d) : need, 0);
            if (r == 0) break;
            need -= r;
        }
    }
    xStreamBufferSend(sb, data, len, 0);
}

static void uart_event_task(void *pv)
{
    uart_event_t event;
    uint8_t *tmp = heap_caps_malloc(SERIAL_RX_BUF_BYTES, MALLOC_CAP_DEFAULT);
    if (!tmp) {
        ESP_LOGE(TAG, "Failed to allocate RX scratch buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        switch (event.type) {
        case UART_DATA: {
            size_t queued = 0;
            if (uart_get_buffered_data_len(UART1_PORT_NUM, &queued) != ESP_OK)
                queued = 0;
            size_t avail = queued ? queued : event.size;
            if (avail > SERIAL_RX_BUF_BYTES)
                avail = SERIAL_RX_BUF_BYTES;

            int n = uart_read_bytes(UART1_PORT_NUM, tmp, avail, pdMS_TO_TICKS(100));
            if (n > 0) {
                size_t space = xStreamBufferSpacesAvailable(s_rx_stream);
                size_t to_store = (size_t)n;
                if (to_store > space) {
                    /* Drop oldest data by flushing what we can */
                    uint8_t drop[64];
                    size_t need_drop = to_store - space;
                    while (need_drop > 0) {
                        size_t rd = xStreamBufferReceive(s_rx_stream, drop,
                                                         need_drop > sizeof(drop) ? sizeof(drop) : need_drop,
                                                         0);
                        if (rd == 0) break;
                        need_drop -= rd;
                    }
                }
                xStreamBufferSend(s_rx_stream, tmp, n, 0);
                /* independent copies: MCP (web/API consumers) and the USB-TTL
                 * CDC bridge. Neither competes with the web/TCP reader. */
                fanout_append(s_mcp_stream, tmp, (size_t)n);
                fanout_append(s_usb_stream, tmp, (size_t)n);
                inc_rx((uint32_t)n);

                if (s_rx_cb) {
                    s_rx_cb(tmp, (size_t)n);
                }
            }
            break;
        }
        case UART_FIFO_OVF:
            ESP_LOGW(TAG, "UART FIFO overflow");
            uart_flush_input(UART1_PORT_NUM);
            break;
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART buffer full");
            uart_flush_input(UART1_PORT_NUM);
            xQueueReset(s_uart_queue);
            break;
        case UART_BREAK:
            ESP_LOGD(TAG, "UART break detected");
            break;
        case UART_PARITY_ERR:
            ESP_LOGW(TAG, "UART parity error");
            break;
        case UART_FRAME_ERR:
            ESP_LOGW(TAG, "UART frame error");
            break;
        case UART_PATTERN_DET:
            break;
        default:
            ESP_LOGW(TAG, "Unhandled UART event type: %d", event.type);
            break;
        }
    }

    free(tmp);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t serial_bridge_init(uint32_t baudrate)
{
    if (s_init_done) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_ERR_INVALID_STATE;
    }

    if (baudrate == 0)
        baudrate = SERIAL_BAUD_DEFAULT;

    /* Create the internal stream buffer */
    s_rx_stream = xStreamBufferCreate(SERIAL_BUF_SIZE, 1);
    s_mcp_stream = xStreamBufferCreate(SERIAL_BUF_SIZE, 1);
    s_usb_stream = xStreamBufferCreate(SERIAL_USB_STREAM_BYTES, 1);
    s_buf_size = SERIAL_BUF_SIZE;
    if (!s_rx_stream) {
        ESP_LOGE(TAG, "Failed to create RX stream buffer");
        if (s_mcp_stream) {
            vStreamBufferDelete(s_mcp_stream);
            s_mcp_stream = NULL;
        }
        if (s_usb_stream) {
            vStreamBufferDelete(s_usb_stream);
            s_usb_stream = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    /* UART config */
    uart_config_t cfg = {
        .baud_rate  = (int)baudrate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(UART1_PORT_NUM,
                                        SERIAL_RX_BUF_BYTES,
                                        SERIAL_TX_BUF_BYTES,
                                        20,             /* queue depth */
                                        &s_uart_queue,
                                        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        vStreamBufferDelete(s_rx_stream);
        s_rx_stream = NULL;
        return err;
    }

    err = uart_param_config(UART1_PORT_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        goto fail;
    }

    int tx_pin = pin_config_tx();
    int rx_pin = pin_config_rx();
    /* RX=-1 means "don't configure RX pin" (UART_PIN_NO_CHANGE).
     * The UART TX still works; RX is simply not connected. */
    err = uart_set_pin(UART1_PORT_NUM,
                       tx_pin,            /* TX (from signal mapping) */
                       rx_pin,            /* RX (-1 = leave unconfigured) */
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Start event task */
    if (xTaskCreate(uart_event_task, "serial_evt", SERIAL_STACK_SIZE,
                    NULL, SERIAL_TASK_PRIO, &s_task) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create event task");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_init_done = true;
    ESP_LOGI(TAG, "Serial bridge ready: TX=IO%d RX=IO%d @ %lu bps",
             PIN_UART1_TX, PIN_UART1_RX, (unsigned long)baudrate);
    return ESP_OK;

fail:
    /* NOTE: do NOT vQueueDelete(s_uart_queue) here -- the queue was created
     * by uart_driver_install() and is owned by the driver; uart_driver_delete()
     * frees it. Deleting it manually first is a double-free. */
    if (s_rx_stream) {
        vStreamBufferDelete(s_rx_stream);
        s_rx_stream = NULL;
    }
    if (s_mcp_stream) {
        vStreamBufferDelete(s_mcp_stream);
        s_mcp_stream = NULL;
    }
    if (s_usb_stream) {
        vStreamBufferDelete(s_usb_stream);
        s_usb_stream = NULL;
    }
    uart_driver_delete(UART1_PORT_NUM);
    s_uart_queue = NULL;
    return err;
}

size_t serial_bridge_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!s_rx_stream || !buf || max_len == 0)
        return 0;
    return xStreamBufferReceive(s_rx_stream, buf, max_len,
                                pdMS_TO_TICKS(timeout_ms));
}

/* Read from the independent MCP stream (doesn't compete with web/TCP). */
size_t serial_bridge_read_mcp(uint8_t *buf, size_t maxlen, uint32_t timeout_ms)
{
    if (!s_mcp_stream) return 0;
    return xStreamBufferReceive(s_mcp_stream, buf, maxlen,
                                pdMS_TO_TICKS(timeout_ms));
}

size_t serial_bridge_read_usb(uint8_t *buf, size_t maxlen, uint32_t timeout_ms)
{
    if (!s_usb_stream) return 0;
    return xStreamBufferReceive(s_usb_stream, buf, maxlen,
                                pdMS_TO_TICKS(timeout_ms));
}

size_t serial_bridge_write(const uint8_t *buf, size_t len)
{
    if (!s_init_done || !buf || len == 0)
        return 0;
    int n = uart_write_bytes(UART1_PORT_NUM, buf, len);
    if (n > 0) {
        inc_tx((uint32_t)n);
    }
    return (n < 0) ? 0 : (size_t)n;
}

esp_err_t serial_bridge_set_baud(uint32_t baudrate)
{
    if (!s_init_done)
        return ESP_ERR_INVALID_STATE;
    if (baudrate == 0)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = uart_set_baudrate(UART1_PORT_NUM, (int)baudrate);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Baud rate set to %lu", (unsigned long)baudrate);
    } else {
        ESP_LOGE(TAG, "uart_set_baudrate failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t serial_bridge_set_bufsize(size_t bytes)
{
    if (!s_init_done || bytes < 64)
        return ESP_ERR_INVALID_ARG;

    /* Create the new buffers FIRST: deleting before creating left s_rx_stream
     * NULL while the UART event task was still running (use-after-free /
     * NULL-stream crash), and a failed create destroyed the old buffer. */
    StreamBufferHandle_t new_rx = xStreamBufferCreate(bytes, 1);
    if (!new_rx) {
        ESP_LOGE(TAG, "Failed to create new RX stream buffer");
        return ESP_ERR_NO_MEM;
    }
    StreamBufferHandle_t new_mcp = xStreamBufferCreate(bytes, 1);
    if (!new_mcp) {
        vStreamBufferDelete(new_rx);
        ESP_LOGE(TAG, "Failed to create new MCP stream buffer");
        return ESP_ERR_NO_MEM;
    }
    StreamBufferHandle_t new_usb = xStreamBufferCreate(SERIAL_USB_STREAM_BYTES, 1);
    if (!new_usb) {
        vStreamBufferDelete(new_rx);
        vStreamBufferDelete(new_mcp);
        ESP_LOGE(TAG, "Failed to create new USB-TTL stream buffer");
        return ESP_ERR_NO_MEM;
    }

    StreamBufferHandle_t old_rx = s_rx_stream;
    StreamBufferHandle_t old_mcp = s_mcp_stream;
    StreamBufferHandle_t old_usb = s_usb_stream;
    s_rx_stream = new_rx;
    s_mcp_stream = new_mcp;
    s_usb_stream = new_usb;
    s_buf_size = bytes;

    /* Let any in-flight use of the old handles by the UART event task (other
     * core) complete before freeing them; its stream operations are short
     * memcpy segments, so 10 ms is comfortably sufficient. */
    vTaskDelay(pdMS_TO_TICKS(10));

    if (old_rx) { vStreamBufferDelete(old_rx); }
    if (old_mcp) { vStreamBufferDelete(old_mcp); }
    if (old_usb) { vStreamBufferDelete(old_usb); }

    ESP_LOGI(TAG, "RX buffer resized to %u bytes", (unsigned)bytes);
    return ESP_OK;
}

void serial_bridge_clear(void)
{
    if (!s_rx_stream) return;
    uint8_t tmp[256];
    while (xStreamBufferReceive(s_rx_stream, tmp, sizeof(tmp), 0) > 0) { }
}

size_t serial_bridge_get_buffered(void)
{
    return s_rx_stream ? xStreamBufferBytesAvailable(s_rx_stream) : 0;
}

size_t serial_bridge_get_bufsize_current(void) { return s_buf_size; }

esp_err_t serial_bridge_repin(void)
{
    if (!s_init_done)
        return ESP_ERR_INVALID_STATE;
    int tx = pin_config_tx();
    int rx = pin_config_rx();
    esp_err_t err = uart_set_pin(UART1_PORT_NUM, tx, rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART re-pinned: TX=IO%d RX=IO%d", tx, rx);
    } else {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint32_t serial_bridge_get_rx_count(void)
{
    return s_rx_count;
}

uint32_t serial_bridge_get_tx_count(void)
{
    return s_tx_count;
}

void serial_bridge_set_rx_callback(serial_rx_cb_t cb)
{
    s_rx_cb = cb;
}
