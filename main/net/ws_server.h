#pragma once
/*
 * ws_server.h - WebSocket real-time stream for the NexLink web UI.
 *
 * The HTTP API is poll-based: a client repeatedly GETs /api/data and drains it.
 * That is wasteful (each poll carries a request round trip) and lossy for
 * bursts, so this server pushes instead:
 *
 *   - UART RX from the target is broadcast to every client as it arrives;
 *   - clients can send JSON commands back over the same socket (write to the
 *     target, change baud, clear buffers, dump the capture log).
 *
 * It shares port 80 with the REST API, so there is no extra port to open and
 * no second HTTP server instance.
 */
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_PATH  "/ws"

/* Register the WebSocket endpoint on an already-running HTTP server and start
 * the broadcast task. Safe to call once; a second call returns
 * ESP_ERR_INVALID_STATE. */
esp_err_t ws_server_start(void *httpd_handle);

/* True once the endpoint is registered and the broadcast task is running. */
bool ws_is_running(void);

/* Number of clients currently attached over WebSocket. */
int ws_client_count(void);

/* Broadcast helpers. All are non-blocking: they enqueue for the broadcast
 * task and return immediately, so they are safe to call from the UART event
 * task (serial RX callback) and from HTTP handlers.
 *
 * Data is only enqueued when at least one client is attached, so an idle
 * device pays nothing. Returns ESP_OK when queued (or when there is nothing to
 * do), ESP_ERR_NO_MEM when the queue is full/dropped. */
esp_err_t ws_broadcast_data(int dir, const uint8_t *data, size_t len);

/* Push a small JSON control message (status snapshots, hello, errors). */
esp_err_t ws_broadcast_json(const char *json);

#ifdef __cplusplus
}
#endif
