#include "ws_server.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "capture.h"
#include "serial_bridge.h"
#include "wifi_manager.h"

static const char *TAG = "ws";

/* ------------------------------------------------------------------ */
/*  Bounded broadcast queue                                            */
/*                                                                    */
/*  Producers are the UART event task (serial RX callback) and HTTP    */
/*  handlers; the single consumer is ws_task. A short queue with a     */
/*  drop-on-full policy keeps a slow or stalled browser from ever      */
/*  applying back-pressure to the UART path - losing a web frame is    */
/*  acceptable, stalling the DUT bridge is not.                        */
/* ------------------------------------------------------------------ */
#define WS_QUEUE_LEN     12
#define WS_PAYLOAD_MAX   1024

typedef struct {
    int      dir;                 /* dir < 0 => control/text message */
    int      len;
    uint8_t  buf[WS_PAYLOAD_MAX];
} ws_item_t;

static httpd_handle_t   s_server;
static QueueHandle_t    s_queue;
static TaskHandle_t     s_task;
static volatile int     s_clients;

static void ws_task(void *arg);

/* ------------------------------------------------------------------ */
/*  Client bookkeeping                                                 */
/* ------------------------------------------------------------------ */

static int ws_fds(int *fds, size_t max)
{
    size_t n = max;
    if (!s_server || httpd_get_client_list(s_server, &n, fds) != ESP_OK)
        return 0;
    return (int)n;
}

/* Count only sockets that actually completed the WebSocket handshake: plain
 * HTTP keep-alive connections sit in the same client list. */
static int ws_count_clients(void)
{
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    int n = ws_fds(fds, sizeof(fds) / sizeof(fds[0]));
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            c++;
    }
    return c;
}

int ws_client_count(void) { return s_clients; }

static void ws_send_all(const uint8_t *data, size_t len, httpd_ws_type_t type)
{
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    int n = ws_fds(fds, sizeof(fds) / sizeof(fds[0]));
    int ok = 0;

    httpd_ws_frame_t frame = {
        .final      = true,
        .fragmented = false,
        .type       = type,
        .payload    = (uint8_t *)data,
        .len        = len,
    };

    for (int i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        esp_err_t e = httpd_ws_send_frame_async(s_server, fds[i], &frame);
        if (e == ESP_OK) {
            ok++;
        } else {
            /* The socket is already gone; the httpd session cleanup will
             * reclaim it. Never close it from here. */
            ESP_LOGD(TAG, "send failed on fd=%d: %s", fds[i], esp_err_to_name(e));
        }
    }
    s_clients = ok;
}

/* ------------------------------------------------------------------ */
/*  Producers                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t ws_send(int dir, const uint8_t *data, size_t len)
{
    if (!s_queue || !len || len > WS_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;
    /* Nothing attached: skip the copy and the queue entirely. */
    if (s_clients <= 0) return ESP_OK;

    ws_item_t it;
    it.dir = dir;
    it.len = (int)len;
    memcpy(it.buf, data, len);
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        ESP_LOGD(TAG, "broadcast queue full, frame dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ws_broadcast_data(int dir, const uint8_t *data, size_t len)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    /* Oversized runs are split instead of dropped, so a long log line still
     * reaches the browser in order. */
    while (len) {
        size_t n = (len > WS_PAYLOAD_MAX) ? WS_PAYLOAD_MAX : len;
        if (ws_send(dir, data, n) != ESP_OK) return ESP_ERR_NO_MEM;
        data += n;
        len  -= n;
    }
    return ESP_OK;
}

esp_err_t ws_broadcast_json(const char *json)
{
    if (!s_task || !json) return ESP_ERR_INVALID_STATE;
    return ws_send(-1, (const uint8_t *)json, strlen(json));
}

/* ------------------------------------------------------------------ */
/*  Consumer                                                           */
/* ------------------------------------------------------------------ */

static void ws_task(void *arg)
{
    (void)arg;
    ws_item_t it;

    for (;;) {
        if (xQueueReceive(s_queue, &it, portMAX_DELAY) != pdTRUE)
            continue;
        if (it.dir < 0)
            ws_send_all(it.buf, (size_t)it.len, HTTPD_WS_TYPE_TEXT);
        else
            ws_send_all(it.buf, (size_t)it.len, HTTPD_WS_TYPE_BINARY);
    }
}

/* ------------------------------------------------------------------ */
/*  Command handling (client -> device)                                */
/* ------------------------------------------------------------------ */

static void ws_notify(int code, const char *msg)
{
    char j[192];
    snprintf(j, sizeof(j), "{\"t\":\"ack\",\"code\":%d,\"msg\":\"%s\"}", code, msg);
    ws_broadcast_json(j);
}

static esp_err_t ws_handle_cmd(const char *cmd, const char *val, const char *raw)
{
    if (!strcmp(cmd, "send")) {
        /* Preferred form: any JSON string value (the browser may send binary
         * line endings, which a hand-rolled \n escape could not express). */
        if (val) {
            serial_bridge_write((const uint8_t *)val, strlen(val));
        } else {
            const char *p = strstr(raw, "\"data\"");
            if (p) p = strchr(p, ':');
            if (p) {
                p++;
                while (*p == ' ') p++;
                if (*p == '"') {
                    p++;
                    char buf[WS_PAYLOAD_MAX];
                    size_t o = 0;
                    while (*p && *p != '"' && o < sizeof(buf) - 1) {
                        if (*p == '\\' && p[1]) {
                            p++;
                            if      (*p == 'n') buf[o++] = '\n';
                            else if (*p == 'r') buf[o++] = '\r';
                            else if (*p == 't') buf[o++] = '\t';
                            else                buf[o++] = *p;
                            p++;
                        } else {
                            buf[o++] = *p++;
                        }
                    }
                    if (o) serial_bridge_write((const uint8_t *)buf, o);
                }
            }
        }
        return ESP_OK;
    }

    if (!strcmp(cmd, "baud")) {
        int b = val ? atoi(val) : 0;
        if (b <= 0) return ESP_ERR_INVALID_ARG;
        serial_bridge_set_baud((uint32_t)b);
        char j[96];
        snprintf(j, sizeof(j), "{\"t\":\"baud\",\"b\":%d}", b);
        ws_broadcast_json(j);
        return ESP_OK;
    }

    if (!strcmp(cmd, "clear")) {
        if (!val || !strcmp(val, "capture") || !strcmp(val, "all"))
            capture_reset_counters();
        if (!val || !strcmp(val, "serial") || !strcmp(val, "all"))
            serial_bridge_clear();
        ws_notify(0, "cleared");
        return ESP_OK;
    }

    if (!strcmp(cmd, "status")) {
        /* Push the full status page state as JSON so a fresh client has
         * something on screen before the first data byte arrives. */
        char ip[16] = {0};
        wifi_manager_get_ip_str(ip, sizeof(ip));
        uint32_t baud = 0;
        char j[256];
        snprintf(j, sizeof(j),
                 "{\"t\":\"status\",\"ip\":\"%s\",\"baud\":%lu,"
                 "\"rx\":%lu,\"tx\":%lu,\"cap\":%u,\"capcap\":%u,\"dropped\":%s}",
                 ip,
                 (unsigned long)baud,
                 (unsigned long)serial_bridge_get_rx_count(),
                 (unsigned long)serial_bridge_get_tx_count(),
                 (unsigned)capture_count(), (unsigned)capture_capacity(),
                 capture_dropped() ? "true" : "false");
        ws_broadcast_json(j);
        return ESP_OK;
    }

    if (!strcmp(cmd, "capture")) {
        /* Dump a slice of the timestamped log on demand. Kept small: each
         * entry becomes one text frame, and the client pages with `since`. */
        int max = val ? atoi(val) : 20;
        if (max <= 0 || max > 32) max = 32;

        static capture_chunk_t chunks[32];
        uint32_t next = 0;
        size_t n = capture_read(0, chunks, (size_t)max, &next);

        char hdr[128];
        snprintf(hdr, sizeof(hdr),
                 "{\"t\":\"capture\",\"returned\":%u,\"next\":%lu,\"dropped\":%s}",
                 (unsigned)n, (unsigned long)next,
                 capture_dropped() ? "true" : "false");
        ws_broadcast_json(hdr);

        for (size_t i = 0; i < n; i++) {
            char line[160];
            int p = snprintf(line, sizeof(line), "%lu %+lldms %s ",
                             (unsigned long)chunks[i].seq,
                             (long long)(chunks[i].timestamp_us / 1000),
                             chunks[i].dir ? "TX" : "RX");
            for (size_t k = 0; k < chunks[i].len && p < (int)sizeof(line) - 4; k++) {
                uint8_t b = chunks[i].data[k];
                line[p++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            line[p++] = '\n';
            line[p]   = '\0';
            ws_send(0, (const uint8_t *)line, (size_t)p);
        }
        return ESP_OK;
    }

    if (!strcmp(cmd, "raw")) {
        /* Escape hatch for binary payloads: {"cmd":"raw","hex":"AABBCC"} */
        const char *p = strstr(raw, "\"hex\"");
        if (!p) return ESP_ERR_INVALID_ARG;
        p = strchr(p, ':');
        if (!p) return ESP_ERR_INVALID_ARG;
        p++;
        while (*p == ' ' || *p == '"') p++;

        uint8_t out[WS_PAYLOAD_MAX / 2];
        size_t o = 0;
        int hi = -1;
        for (; *p && *p != '"' && o < sizeof(out); p++) {
            int v;
            if      (*p >= '0' && *p <= '9') v = *p - '0';
            else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
            else continue;
            if (hi < 0) hi = v;
            else { out[o++] = (uint8_t)((hi << 4) | v); hi = -1; }
        }
        if (o) serial_bridge_write(out, o);
        ws_notify(0, "raw sent");
        return ESP_OK;
    }

    ws_notify(-1, "unknown command");
    return ESP_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers (share port 80 with the REST API)                    */
/* ------------------------------------------------------------------ */

static esp_err_t ws_get_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake. This handler is also invoked for every subsequent data
         * frame, but those arrive as HTTP_POST (see httpd_uri_t below). */
        ESP_LOGI(TAG, "client connected");
        s_clients = ws_count_clients();
        return ESP_OK;
    }

    char buf[WS_PAYLOAD_MAX];

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT, .payload = NULL };
    esp_err_t e = httpd_ws_recv_frame(req, &frame, 0);
    if (e != ESP_OK) return e;

    /* Length was just queried; read it now. Only text commands are accepted,
     * and oversized ones are rejected before allocating anything. */
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) return ESP_OK;
    if (frame.len >= sizeof(buf)) {
        ESP_LOGW(TAG, "command too long (%u bytes), ignored", (unsigned)frame.len);
        ws_notify(-1, "command too long");
        return ESP_OK;
    }

    frame.payload = (uint8_t *)buf;
    e = httpd_ws_recv_frame(req, &frame, frame.len);
    if (e != ESP_OK) return e;
    buf[frame.len] = '\0';

    /* Command is the first "key":"value" pair with a scalar value; the raw
     * text stays available for the string/number recovery paths. */
    char key[24] = {0};
    const char *kp = strstr(buf, "\"cmd\"");
    if (kp) {
        kp = strchr(kp, ':');
        if (kp) {
            kp++;
            while (*kp == ' ' || *kp == '"') kp++;
            size_t i = 0;
            while (*kp && *kp != '"' && *kp != ',' && *kp != '}' && i < sizeof(key) - 1)
                key[i++] = *kp++;
            key[i] = '\0';
        }
    }
    if (!key[0]) { ws_notify(-1, "missing cmd"); return ESP_OK; }

    /* Scalar value of that command, when present (e.g. {"cmd":"baud","v":921600}
     * or {"cmd":"baud","value":"921600"}). */
    char val[256] = {0};
    const char *vp = strstr(buf, "\"v\"");
    if (!vp) vp = strstr(buf, "\"value\"");
    if (vp) {
        vp = strchr(vp, ':');
        if (vp) {
            vp++;
            while (*vp == ' ') vp++;
            if (*vp == '"') {
                vp++;
                size_t i = 0;
                while (*vp && *vp != '"' && i < sizeof(val) - 1) val[i++] = *vp++;
                val[i] = '\0';
            } else {
                size_t i = 0;
                while (*vp && *vp != ',' && *vp != '}' && i < sizeof(val) - 1)
                    val[i++] = *vp++;
                val[i] = '\0';
            }
        }
    }

    ws_handle_cmd(key, val[0] ? val : NULL, buf);
    return ESP_OK;
}

/* Data frames are delivered as HTTP_POST to the same URI, so the endpoint has
 * to be registered twice with identical handlers. */
static esp_err_t ws_post_handler(httpd_req_t *req) { return ws_get_handler(req); }

/* ------------------------------------------------------------------ */
/*  Startup                                                            */
/* ------------------------------------------------------------------ */

esp_err_t ws_server_start(void *httpd_handle)
{
    if (s_task) {
        ESP_LOGW(TAG, "already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!httpd_handle) return ESP_ERR_INVALID_ARG;
    s_server = (httpd_handle_t)httpd_handle;

    s_queue = xQueueCreate(WS_QUEUE_LEN, sizeof(ws_item_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ws_task, "ws_tx", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    static const httpd_uri_t uri_ws_get = {
        .uri = WS_PATH, .method = HTTP_GET, .handler = ws_get_handler,
        .is_websocket = true,
    };
    static const httpd_uri_t uri_ws_post = {
        .uri = WS_PATH, .method = HTTP_POST, .handler = ws_post_handler,
        .is_websocket = true,
    };

    esp_err_t e = httpd_register_uri_handler(s_server, &uri_ws_get);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "register GET %s failed: %s", WS_PATH, esp_err_to_name(e));
        return e;
    }
    e = httpd_register_uri_handler(s_server, &uri_ws_post);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "register POST %s failed: %s", WS_PATH, esp_err_to_name(e));
        return e;
    }

    ESP_LOGI(TAG, "WebSocket endpoint ready at %s (port 80)", WS_PATH);
    return ESP_OK;
}

bool ws_is_running(void) { return s_task != NULL; }
