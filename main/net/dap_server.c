/*
 * dap_server.c - CMSIS-DAP TCP server (port 5555).
 *
 * Accepts one client at a time (OpenOCD/pyOCD style hosts over a raw TCP
 * bridge). Each TCP message is one CMSIS-DAP command; the response is sent
 * back immediately.
 *
 * CMSIS-DAP v1 framing: first byte = command ID, rest = payload.  We read
 * one byte to get the command, then read the expected payload based on the
 * command, then send the response.  Payload lengths below MUST match what
 * the vendored ARM DAP.c handlers consume (the "consumed" count returned as
 * request_bytes << 16), otherwise the byte stream desynchronises.
 */
#include "dap_server.h"
#include "cmsis_dap.h"
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dap_srv";

#define DAP_PORT       5555
#define DAP_BUF_SIZE   1024
#define DAP_TIMEOUT_S  120

static volatile int s_client_count = 0;
static volatile bool s_running = false;

/*
 * Read exactly `want` bytes. Returns true on success; false on timeout /
 * peer close / error. TCP recv() can return short reads, so a single recv
 * is never enough -- the old code desynchronised the framing that way.
 */
static bool recv_exact(int sock, uint8_t *p, int want)
{
    int got = 0;
    while (got < want) {
        int n = recv(sock, p + got, want - got, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            return false;
        }
        if (n == 0) return false;   /* peer closed */
        got += n;
    }
    return true;
}

static void dap_client_task(void *arg)
{
    int sock = (int)(intptr_t)arg;

    uint8_t *rxbuf = malloc(DAP_BUF_SIZE);
    uint8_t *txbuf = malloc(DAP_BUF_SIZE);
    if (!rxbuf || !txbuf) {
        ESP_LOGE(TAG, "alloc failed");
        close(sock);
        free(rxbuf); free(txbuf);
        s_client_count = 0;
        vTaskDelete(NULL);
        return;
    }

    /* Receive timeout so a half-open connection cannot wedge us forever,
     * plus TCP keepalive to actually notice dead peers while idle. */
    struct timeval tv = { .tv_sec = DAP_TIMEOUT_S };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    ESP_LOGI(TAG, "client connected (fd=%d)", sock);

    while (s_running) {
        /* Read command byte; block out timeouts are just idle. */
        uint8_t cmd;
        int n = recv(sock, &cmd, 1, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) break;

        rxbuf[0] = cmd;

        /* Read the payload that the DAP command consumes. */
        int payload_len = 0;
        bool self_read = false;
        switch (cmd) {
        case DAP_INFO:                payload_len = 1; break;  /* info_id */
        case DAP_HOST_STATUS:         payload_len = 2; break;  /* type + status */
        case DAP_CONNECT:             payload_len = 1; break;  /* port request */
        case DAP_DISCONNECT:          payload_len = 0; break;
        case DAP_TRANSFER_CONFIGURE:  payload_len = 5; break;  /* idle + wait(2) + match */
        case DAP_TRANSFER:
            /* index(1) + count(1) + entries; each entry is 1 request byte,
             * plus 4 data bytes for writes AND for match-value reads. */
            if (!recv_exact(sock, rxbuf + 1, 2)) goto disconnect;
            payload_len = 2;
            {
                int count = rxbuf[2];
                if (count > (DAP_BUF_SIZE - 3) / 5) goto disconnect; /* malformed */
                for (int i = 0; i < count; i++) {
                    int off = 3 + i * 5;
                    if (!recv_exact(sock, rxbuf + off, 1)) goto disconnect;
                    payload_len++;
                    uint8_t r = rxbuf[off];
                    bool is_write = !(r & DAP_TRANSFER_RnW);
                    bool is_match = (r & DAP_TRANSFER_MATCH) != 0;
                    /* Writes and match-value reads both carry 4 data bytes. */
                    if (is_write || is_match) {
                        if (!recv_exact(sock, rxbuf + off + 1, 4)) goto disconnect;
                        payload_len += 4;
                    }
                }
            }
            self_read = true;
            break;
        case DAP_TRANSFER_BLOCK:      /* index(1) + count(2) + request(1) + data */
            if (!recv_exact(sock, rxbuf + 1, 4)) goto disconnect;
            payload_len = 4;
            {
                int count = rxbuf[2] | (rxbuf[3] << 8);
                bool is_read = (rxbuf[4] & DAP_TRANSFER_RnW) != 0;
                if (!is_read) {
                    /* Writes carry count*4 data bytes. Refuse to read a
                     * request that cannot fit -- anything else desyncs. */
                    if (count > (DAP_BUF_SIZE - 5) / 4) goto disconnect;
                    int data_bytes = count * 4;
                    if (!recv_exact(sock, rxbuf + 5, data_bytes)) goto disconnect;
                    payload_len += data_bytes;
                }
            }
            self_read = true;
            break;
        case DAP_WRITE_ABORT:         payload_len = 5; break;  /* index(1) + data(4) */
        case DAP_DELAY:               payload_len = 2; break;
        case DAP_RESET_TARGET:        payload_len = 0; break;
        case DAP_SWJ_PINS:            payload_len = 6; break;  /* out + mask + 4 wait */
        case DAP_SWJ_CLOCK:           payload_len = 4; break;
        case DAP_SWJ_SEQUENCE:        /* bit_count(1) + data; 0 means 256 bits */
            if (!recv_exact(sock, rxbuf + 1, 1)) goto disconnect;
            payload_len = 1;
            {
                int bits = rxbuf[1];
                if (bits == 0) bits = 256;
                int byte_count = (bits + 7) / 8;
                if (byte_count > DAP_BUF_SIZE - 2) goto disconnect;
                if (!recv_exact(sock, rxbuf + 2, byte_count)) goto disconnect;
                payload_len += byte_count;
            }
            self_read = true;
            break;
        case DAP_SWD_CONFIGURE:       payload_len = 1; break;
        case DAP_TRANSFER_ABORT:      payload_len = 0; break;  /* no response */
        default:
            /* Unknown or unframmable command (0x1D SWD_Sequence, 0x14-0x16
             * JTAG, 0x7E/0x7F Queue/ExecuteCommands ...): we cannot know how
             * many payload bytes follow, so the only safe action is to drop
             * the connection rather than desynchronise the stream. */
            ESP_LOGW(TAG, "Unsupported/unframmable cmd 0x%02X -- closing", cmd);
            goto disconnect;
        }

        /* Read any remaining payload (only for fixed-length commands) */
        if (!self_read && payload_len > 0) {
            if (!recv_exact(sock, rxbuf + 1, payload_len)) goto disconnect;
        }

        /* Process the command */
        int resp_len = cmsis_dap_process(rxbuf, 1 + payload_len,
                                         txbuf, DAP_BUF_SIZE);
        if (resp_len > 0) {
            int sent = 0;
            while (sent < resp_len) {
                n = send(sock, txbuf + sent, resp_len - sent, 0);
                if (n <= 0) goto disconnect;
                sent += n;
            }
        }
    }

disconnect:
    ESP_LOGI(TAG, "client disconnected (fd=%d)", sock);
    close(sock);
    free(rxbuf);
    free(txbuf);
    s_client_count = 0;
    vTaskDelete(NULL);
}

static void dap_server_task(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "socket create failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DAP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }

    listen(server_fd, 1);
    ESP_LOGI(TAG, "CMSIS-DAP server listening on :%d", DAP_PORT);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        if (s_client_count > 0) {
            /* Only one client at a time */
            const char *msg = "BUSY\n";
            send(client_fd, msg, strlen(msg), 0);
            close(client_fd);
            continue;
        }

        /* Claim the slot here (single-threaded check-then-set), NOT in the
         * client task, so a second accept cannot slip past the busy check. */
        s_client_count = 1;

        if (xTaskCreate(dap_client_task, "dap_client", 8192,
                        (void *)(intptr_t)client_fd, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "client task create failed");
            close(client_fd);
            s_client_count = 0;
        }
    }

    close(server_fd);
    vTaskDelete(NULL);
}

esp_err_t dap_server_start(void)
{
    if (s_running) return ESP_OK;
    s_running = true;
    cmsis_dap_init();
    xTaskCreate(dap_server_task, "dap_server", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void dap_server_stop(void)
{
    s_running = false;
}

int dap_server_client_count(void) { return s_client_count; }
