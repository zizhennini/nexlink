/*
 * cmsis_dap.c - CMSIS-DAP v1 protocol handler for OpenOCD/pyOCD.
 *
 * Translates CMSIS-DAP commands into the existing GPIO bit-bang SWD calls.
 * Designed to be called from a TCP server (one command per packet).
 *
 * Response layouts follow the CMSIS-DAP spec (the vendored ARM DAP.c on the
 * USB path is the reference):
 *   DAP_Transfer       response = 0x05 | count | value | data...
 *                      (ONE value byte, then 4 bytes per successful read)
 *   DAP_TransferBlock  response = 0x06 | count(2) | value | data...
 *                      (value byte sits at index 3, BEFORE the data)
 *   DAP_Info           response = 0x00 | length | data
 */
#include "cmsis_dap.h"
#include "swd_bridge.h"
#include "pin_config.h"
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dap";

static bool s_connected = false;
static uint32_t s_clock_hz = 1000000;  /* placeholder, not used by bit-bang */

/* DAP_TransferConfigure parameters (defaults per spec). */
static uint8_t  s_idle_cycles = 0;
static uint16_t s_wait_retry  = 100;
static uint8_t  s_match_retry = 10;
static volatile bool s_abort_requested = false;

void cmsis_dap_init(void)
{
    s_connected = false;
    swd_init();
}

/*
 * Map an esp_err_t from a swd_bridge transfer onto the CMSIS-DAP response
 * value bit flags (OK=0x01, WAIT=0x02, FAULT=0x04, protocol error=0x08).
 */
static uint8_t transfer_status(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return DAP_TRANSFER_OK;
    case ESP_ERR_TIMEOUT:
        return (swd_get_last_ack() == 0x2) ? DAP_TRANSFER_WAIT : DAP_TRANSFER_FAULT;
    case ESP_ERR_INVALID_CRC:
        return DAP_TRANSFER_ERROR;   /* parity / protocol error */
    default:
        return (swd_get_last_ack() == 0x4) ? DAP_TRANSFER_FAULT : DAP_TRANSFER_ERROR;
    }
}

/* ---- DAP_INFO ---- */
static int handle_info(const uint8_t *req, int req_len,
                       uint8_t *resp, int resp_max)
{
    (void)req_len;
    uint8_t id = req[1];
    int rlen = 2;  /* position after header (cmd + length byte) */
    const char *str = NULL;

    switch (id) {
    case DAP_ID_VENDOR:          str = "ESP32"; break;
    case DAP_ID_PRODUCT:         str = "NexLink CMSIS-DAP (TCP)"; break;
    case DAP_ID_SER_NUM:         str = "ESP32S3"; break;
    case DAP_ID_FW_VER:          str = "1.1"; break;
    case DAP_ID_DEVICE_VENDOR:   str = "Espressif"; break;
    case DAP_ID_DEVICE_NAME:     str = "ESP32-S3"; break;
    case DAP_ID_CAPABILITIES:
        if (resp_max < 3) return 2;
        resp[rlen++] = 0x01;  /* bit0 = SWD supported */
        goto done;
    case DAP_ID_PACKET_COUNT:
        if (resp_max < 3) return 2;
        resp[rlen++] = 1;
        goto done;
    case DAP_ID_PACKET_SIZE:
        if (resp_max < 4) return 2;
        resp[rlen++] = 64;  /* little-endian uint16 */
        resp[rlen++] = 0;
        goto done;
    default:
        break;
    }

    if (str) {
        int slen = (int)strlen(str);
        if (slen > resp_max - 2) slen = resp_max - 2;
        memcpy(resp + rlen, str, slen);
        rlen += slen;
    }
    /* Unknown ID: length 0, no data (rlen stays 2). */

done:
    resp[0] = DAP_INFO;
    resp[1] = (uint8_t)(rlen - 2);  /* data length */
    return rlen;
}

/* ---- DAP_HOST_STATUS ---- */
static int handle_host_status(const uint8_t *req, int req_len,
                              uint8_t *resp, int resp_max)
{
    (void)req_len; (void)resp_max;
    /* req[1] = type (0=connect, 1=running), req[2] = status */
    resp[0] = DAP_HOST_STATUS;
    resp[1] = 0;  /* DAP_OK */
    return 2;
}

/* ---- DAP_CONNECT ---- */
static int handle_connect(const uint8_t *req, int req_len,
                          uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1] = requested port: 0=any, 1=SWD, 2=JTAG */
    uint8_t port = (req_len >= 2) ? req[1] : 0;

    resp[0] = DAP_CONNECT;
    if (port == DAP_MODE_JTAG) {
        resp[1] = 0;  /* no port connected -- JTAG is not supported */
        ESP_LOGW(TAG, "CONNECT(JTAG) unsupported");
        return 2;
    }
    /* SWD mode: line reset -> JTAG-to-SWD -> line reset */
    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();
    s_connected = true;
    resp[1] = DAP_MODE_SWD;
    ESP_LOGI(TAG, "CONNECT (SWD)");
    return 2;
}

/* ---- DAP_DISCONNECT ---- */
static int handle_disconnect(const uint8_t *req, int req_len,
                             uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    s_connected = false;
    resp[0] = DAP_DISCONNECT;
    resp[1] = 1;  /* success */
    ESP_LOGI(TAG, "DISCONNECT");
    return 2;
}

/* ---- DAP_TRANSFER_CONFIGURE ---- */
static int handle_transfer_configure(const uint8_t *req, int req_len,
                                     uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1]=idle_cycles, req[2..3]=wait_retry (LE16), req[4]=match_retry */
    if (req_len >= 5) {
        s_idle_cycles = req[1];
        s_wait_retry  = (uint16_t)(req[2] | (req[3] << 8));
        s_match_retry = req[4];
        if (s_wait_retry == 0) s_wait_retry = 100;
    }
    resp[0] = DAP_TRANSFER_CONFIGURE;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWJ_CLOCK ---- */
static int handle_swj_clock(const uint8_t *req, int req_len,
                            uint8_t *resp, int resp_max)
{
    (void)req_len; (void)resp_max;
    if (req_len >= 5) {
        s_clock_hz = req[1] | (req[2] << 8) | (req[3] << 16) | ((uint32_t)req[4] << 24);
        ESP_LOGI(TAG, "SWJ_CLOCK %lu Hz", (unsigned long)s_clock_hz);
    }
    resp[0] = DAP_SWJ_CLOCK;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWD_CONFIGURE ---- */
static int handle_swd_configure(const uint8_t *req, int req_len,
                                uint8_t *resp, int resp_max)
{
    (void)req_len; (void)resp_max;
    /* req[1]: turnaround bits / data phase config -- the bit-banger uses a
     * fixed 1-cycle turnaround, so the value is accepted but not applied. */
    resp[0] = DAP_SWD_CONFIGURE;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_SWJ_SEQUENCE ---- */
static int handle_swj_sequence(const uint8_t *req, int req_len,
                               uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1] = bit count (0 means 256), req[2..] = data bytes */
    resp[0] = DAP_SWJ_SEQUENCE;
    if (req_len < 2) { resp[1] = 0; return 2; }
    int bit_count = req[1];
    if (bit_count == 0) bit_count = 256;
    int byte_count = (bit_count + 7) / 8;
    if (req_len < 2 + byte_count) { resp[1] = 0; return 2; }

    const uint8_t *data = req + 2;

    /* Line reset: >=50 bits of all ones (only the first bit_count bits are
     * significant; the padding bits in the last byte are ignored). */
    if (bit_count >= 50) {
        bool all_ones = true;
        for (int i = 0; i < byte_count && all_ones; i++) {
            int valid = bit_count - i * 8;
            uint8_t mask = (valid >= 8) ? 0xFF : (uint8_t)((1U << valid) - 1U);
            if ((data[i] & mask) != mask) all_ones = false;
        }
        if (all_ones) {
            swd_line_reset();
            resp[1] = 1;
            return 2;
        }
    }

    /* JTAG-to-SWD selection: 16 bits of the pattern 0xE79E sent LSB-first
     * (wire pattern 0111 1001 1110 0111). */
    if (bit_count == 16 && req_len >= 4 && data[0] == 0x9E && data[1] == 0xE7) {
        swd_jtag_to_swd();
        resp[1] = 1;
        return 2;
    }

    /* Anything else: the bit-banger cannot replay an arbitrary bit sequence
     * faithfully, so answer without side effects rather than mis-guessing
     * (a spurious line reset here would drop the debug connection). */
    resp[1] = 1;
    return 2;
}

/* ---- DAP_SWJ_PINS ---- */
static int handle_swj_pins(const uint8_t *req, int req_len,
                           uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* v1 layout: req[1]=pin output, req[2]=pin select, req[3..6]=wait (LE32) */
    resp[0] = DAP_SWJ_PINS;
    if (req_len < 3) { resp[1] = 0; return 2; }

    uint8_t output = req[1];
    uint8_t select = req[2];

    /* Pin bits per DAP.h (same layout the vendored USB DAP.c uses):
     * bit0 SWCLK/TCK, bit1 SWDIO/TMS, bit5 nTRST, bit7 nRESET (active low). */
    if (select & 0x80) {
        swd_reset_target(!(output & 0x80));
    }
    if (select & 0x01) {
        gpio_set_level(pin_config_swclk(), (output & 0x01) ? 1 : 0);
    }
    if (select & 0x02) {
        gpio_set_level(pin_config_swdio(), (output & 0x02) ? 1 : 0);
    }
    if (select & 0x20) {
        int nrst = pin_config_nrst();
        if (nrst >= 0) gpio_set_level(nrst, 1);  /* keep nTRST released */
    }

    /* Response = current PIN INPUT state (spec), not an echo of the output.
     * Bit 0 = SWCLK/TCK, bit 1 = SWDIO/TMS, bit 7 = nRESET. */
    uint8_t in = 0;
    if (gpio_get_level(pin_config_swclk())) in |= 0x01;
    if (gpio_get_level(pin_config_swdio())) in |= 0x02;
    {
        int nrst = pin_config_nrst();
        if (nrst >= 0 && gpio_get_level(nrst)) in |= 0x80;
    }
    resp[1] = in;
    return 2;
}

/* ---- DAP_RESET_TARGET ---- */
static int handle_reset_target(const uint8_t *req, int req_len,
                               uint8_t *resp, int resp_max)
{
    (void)req; (void)req_len; (void)resp_max;
    swd_reset_target(true);                    /* assert */
    vTaskDelay(pdMS_TO_TICKS(20));             /* hold >= 20 ms */
    swd_reset_target(false);                   /* release */
    vTaskDelay(pdMS_TO_TICKS(1));
    resp[0] = DAP_RESET_TARGET;
    resp[1] = 1;  /* success */
    return 2;
}

/* ---- DAP_TRANSFER ---- */
static int handle_transfer(const uint8_t *req, int req_len,
                           uint8_t *resp, int resp_max)
{
    /* req[0]=DAP_TRANSFER, req[1]=DAP index(ignored), req[2]=count, req[3..]=transfers */
    resp[0] = DAP_TRANSFER;

    if (req_len < 3 || resp_max < 3) {
        resp[1] = 0;
        resp[2] = DAP_TRANSFER_ERROR;
        return 3;
    }

    uint8_t count = req[2];
    int pos = 3;
    int dpos = 3;          /* data write position (after cmd/count/value) */
    uint8_t done = 0;
    uint8_t value = DAP_TRANSFER_OK;

    for (int i = 0; i < count; i++) {
        if (pos >= req_len) { value = DAP_TRANSFER_ERROR; break; }
        uint8_t request = req[pos++];
        bool is_read = (request & DAP_TRANSFER_RnW) != 0;
        bool is_ap   = (request & DAP_TRANSFER_APnDP) != 0;
        uint8_t addr  = (request & 0x0C);
        esp_err_t err = ESP_FAIL;
        uint32_t data = 0;

        if (is_read) {
            if (request & DAP_TRANSFER_MATCH) {
                /* A match read carries 4 extra request bytes (match value +
                 * mask); the bit-banger cannot poll for a value, so report a
                 * protocol error after consuming the bytes. */
                if (pos + 4 > req_len) { value = DAP_TRANSFER_ERROR; break; }
                pos += 4;
                value = DAP_TRANSFER_ERROR;
                break;
            }
            for (uint16_t retry = 0; retry < s_wait_retry; retry++) {
                if (s_abort_requested) { value = DAP_TRANSFER_ERROR; break; }
                err = is_ap ? swd_read_ap(addr, &data) : swd_read_dp(addr, &data);
                value = transfer_status(err);
                if (value != DAP_TRANSFER_WAIT) break;
            }
            if (err != ESP_OK) break;
            if (dpos + 4 > resp_max) { value = DAP_TRANSFER_ERROR; break; }
            resp[dpos++] = (uint8_t)(data);
            resp[dpos++] = (uint8_t)(data >> 8);
            resp[dpos++] = (uint8_t)(data >> 16);
            resp[dpos++] = (uint8_t)(data >> 24);
        } else {
            if (pos + 4 > req_len) { value = DAP_TRANSFER_ERROR; break; }
            data = req[pos] | (req[pos+1] << 8) | (req[pos+2] << 16) |
                   ((uint32_t)req[pos+3] << 24);
            pos += 4;
            err = is_ap ? swd_write_ap(addr, data) : swd_write_dp(addr, data);
            value = transfer_status(err);
            if (err != ESP_OK) break;
        }
        done++;
    }

    /* Spec: ONE response value for the whole sequence, no per-transfer
     * status bytes; data words follow for each successful read. On a FAULT
     * the host itself issues DAP_WriteAbort -- do not touch the target. */
    resp[1] = done;
    resp[2] = value;
    return dpos;
}

/* ---- DAP_TRANSFER_BLOCK ---- */
static int handle_transfer_block(const uint8_t *req, int req_len,
                                 uint8_t *resp, int resp_max)
{
    /* req: 0x06 | index | count(2 LE) | request | data... */
    resp[0] = DAP_TRANSFER_BLOCK;

    if (req_len < 5 || resp_max < 4) {
        resp[1] = 0; resp[2] = 0;
        resp[3] = DAP_TRANSFER_ERROR;
        return 4;
    }

    int count = req[2] | (req[3] << 8);
    uint8_t request = req[4];
    bool is_read = (request & DAP_TRANSFER_RnW) != 0;
    bool is_ap   = (request & DAP_TRANSFER_APnDP) != 0;
    uint8_t addr  = (request & 0x0C);

    int processed = 0;
    uint8_t value = DAP_TRANSFER_OK;

    if (is_read) {
        int dpos = 4;
        for (int i = 0; i < count; i++) {
            if (dpos + 4 > resp_max) { value = DAP_TRANSFER_ERROR; break; }
            uint32_t data = 0;
            esp_err_t err = ESP_FAIL;
            for (uint16_t retry = 0; retry < s_wait_retry; retry++) {
                if (s_abort_requested) { value = DAP_TRANSFER_ERROR; break; }
                err = is_ap ? swd_read_ap(addr, &data) : swd_read_dp(addr, &data);
                value = transfer_status(err);
                if (value != DAP_TRANSFER_WAIT) break;
            }
            if (err != ESP_OK) break;
            resp[dpos++] = (uint8_t)(data);
            resp[dpos++] = (uint8_t)(data >> 8);
            resp[dpos++] = (uint8_t)(data >> 16);
            resp[dpos++] = (uint8_t)(data >> 24);
            processed++;
        }
        resp[1] = (uint8_t)(processed & 0xFF);
        resp[2] = (uint8_t)(processed >> 8);
        resp[3] = value;
        return dpos;
    } else {
        int dpos = 5;    /* request data starts right after the request byte */
        for (int i = 0; i < count; i++) {
            if (dpos + 4 > req_len) { value = DAP_TRANSFER_ERROR; break; }
            uint32_t data = req[dpos] | (req[dpos+1] << 8) |
                            (req[dpos+2] << 16) | ((uint32_t)req[dpos+3] << 24);
            dpos += 4;
            esp_err_t err = is_ap ? swd_write_ap(addr, data)
                                  : swd_write_dp(addr, data);
            value = transfer_status(err);
            if (err != ESP_OK) break;
            processed++;
        }
        resp[1] = (uint8_t)(processed & 0xFF);
        resp[2] = (uint8_t)(processed >> 8);
        resp[3] = value;
        return 4;
    }
}

/* ---- DAP_WRITE_ABORT ---- */
static int handle_write_abort(const uint8_t *req, int req_len,
                              uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req: 0x08 | DAP index | data(4 LE) */
    resp[0] = DAP_WRITE_ABORT;
    if (req_len >= 6) {
        uint32_t data = req[2] | (req[3] << 8) | (req[4] << 16) |
                        ((uint32_t)req[5] << 24);
        swd_write_dp(0x0, data);  /* DP ABORT register */
        resp[1] = 1;
    } else {
        resp[1] = 0;
    }
    return 2;
}

/* ---- DAP_DELAY ---- */
static int handle_delay(const uint8_t *req, int req_len,
                        uint8_t *resp, int resp_max)
{
    (void)resp_max;
    /* req[1..2] = delay in microseconds (LE16) */
    resp[0] = DAP_DELAY;
    resp[1] = 1;
    if (req_len >= 3) {
        uint32_t us = req[1] | (req[2] << 8);
        if (us > 20000) {
            vTaskDelay(pdMS_TO_TICKS(us / 1000U + 1U));
        } else if (us > 0) {
            esp_rom_delay_us(us);
        }
    }
    return 2;
}

/* ---- Main dispatch ---- */
int cmsis_dap_process(const uint8_t *req, int req_len,
                      uint8_t *resp, int resp_max)
{
    if (req_len < 1) return 0;

    uint8_t cmd = req[0];

    /* The three SWD pads are shared with the USB DAP path and the
     * menu/HTTP diagnostics: hold the bus for the whole command. */
    swd_bus_lock();
    s_abort_requested = false;

    int r;
    switch (cmd) {
    case DAP_INFO:                r = handle_info(req, req_len, resp, resp_max); break;
    case DAP_HOST_STATUS:         r = handle_host_status(req, req_len, resp, resp_max); break;
    case DAP_CONNECT:             r = handle_connect(req, req_len, resp, resp_max); break;
    case DAP_DISCONNECT:          r = handle_disconnect(req, req_len, resp, resp_max); break;
    case DAP_TRANSFER_CONFIGURE:  r = handle_transfer_configure(req, req_len, resp, resp_max); break;
    case DAP_TRANSFER:            r = handle_transfer(req, req_len, resp, resp_max); break;
    case DAP_TRANSFER_BLOCK:      r = handle_transfer_block(req, req_len, resp, resp_max); break;
    case DAP_TRANSFER_ABORT:
        /* Spec: no response at all; the next command's reply carries the
         * cancelled count. There is nothing mid-transfer to cancel here
         * (each command runs to completion synchronously). */
        s_abort_requested = true;
        swd_bus_unlock();
        return 0;
    case DAP_WRITE_ABORT:         r = handle_write_abort(req, req_len, resp, resp_max); break;
    case DAP_DELAY:               r = handle_delay(req, req_len, resp, resp_max); break;
    case DAP_RESET_TARGET:        r = handle_reset_target(req, req_len, resp, resp_max); break;
    case DAP_SWJ_PINS:            r = handle_swj_pins(req, req_len, resp, resp_max); break;
    case DAP_SWJ_CLOCK:           r = handle_swj_clock(req, req_len, resp, resp_max); break;
    case DAP_SWJ_SEQUENCE:        r = handle_swj_sequence(req, req_len, resp, resp_max); break;
    case DAP_SWD_CONFIGURE:       r = handle_swd_configure(req, req_len, resp, resp_max); break;
    default:
        ESP_LOGW(TAG, "Unknown cmd 0x%02X", cmd);
        resp[0] = cmd;
        resp[1] = 0;  /* unsupported */
        swd_bus_unlock();
        return 2;
    }
    swd_bus_unlock();
    return r;
}
