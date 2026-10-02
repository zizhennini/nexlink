/*
 * swo_uart.c - CMSIS-DAP SWO (Serial Wire Output) capture in UART mode.
 *
 * WHAT SWO GIVES YOU
 * ------------------
 * A Cortex-M target can emit ITM trace (printf via ITM_SendChar) on a single
 * wire instead of a UART. A debug probe decodes it and hands it to the IDE's
 * trace console. Without this file the board could debug a target but never
 * show its trace output.
 *
 * WHY THIS IS NOT ARM'S SWO.c
 * ---------------------------
 * ARM's implementation (reference: DAPLink SWO.c v2.0.1) drives a CMSIS
 * `Driver_USART` instance, which does not exist in ESP-IDF. The command layer,
 * the response encoding and the error/status semantics are reproduced exactly
 * here; only the byte source is different. The host cannot tell the difference.
 *
 * The SWO wire is an expansion-header IO (PIN_DEBUG_SWO), claimed at boot by
 * debug_pins.c, because the board has no spare dedicated pin for it.
 *
 * NO EVENT TASK, NO PRIVATE RING
 * ------------------------------
 * The bytes live in the UART driver's own RX ring, which its ISR fills and
 * which already drops the OLDEST data when full - exactly the policy SWO
 * wants. A previous revision added an event task plus a second ring on top,
 * which introduced two real hazards: freeing the driver while that task was
 * still blocked on the driver's queue (use-after-free), and a producer that
 * consumed from the ring while the consumer was reading, so the byte count
 * announced to the host could exceed the bytes actually copied.
 *
 * Both hazards disappear by removing the second stage: SWO_Data() reads the
 * driver ring directly. This is safe because every SWO command handler runs in
 * the single DAP request task - DAP_ExecuteCommand() is called only from
 * dap_usb.c's request task (the TCP CMSIS-DAP path in swd/cmsis_dap.c has its
 * own command set) - and the UART driver is thread-safe against its own ISR.
 * There is therefore no cross-task race left to synchronize, and no shutdown
 * handshake to get wrong.
 *
 * Overrun reporting still works without our own ring: installing an event
 * queue makes the driver's ISR report FIFO overflow and buffer-full through
 * UART_FIFO_OVF / UART_BUFFER_FULL, which are drained (non-blocking) into the
 * standard DAP_SWO_BUFFER_OVERRUN status bit.
 */
#include "DAP_config.h"

#if (SWO_UART != 0)

#include "DAP.h"
#include "pinout.h"
#include "debug_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "swo";

/* ------------------------------------------------------------------ */
/*  UART plumbing                                                      */
/* ------------------------------------------------------------------ */

/* The driver's RX ring is the trace buffer. SWO_BUFFER_SIZE is required to be a
 * power of two by ARM's arithmetic; it is used here only as the ring size, so
 * any sane value works, but keep it a power of two to match the documentation
 * and the DAP_ID_SWO_BUFFER_SIZE value reported to the host. */
_Static_assert((SWO_BUFFER_SIZE & (SWO_BUFFER_SIZE - 1U)) == 0U,
               "SWO_BUFFER_SIZE must be a power of two");

#define SWO_EVT_QUEUE   8

static bool          s_installed;
static uart_port_t   s_port = (uart_port_t)SWO_UART_DRIVER;
static uint32_t      s_baud;               /* 0 = not configured yet */
static QueueHandle_t s_evt_q;

/* Task-context only (see the file header): no locking is required. */
static uint16_t s_ovf;                     /* overruns since the last report */
static bool     s_stream_err;              /* wire-level fault seen (break/framing) */
static uint32_t s_rx_total;                /* ITM bytes handed to the host */

/* Drain whatever the driver's ISR has posted, without blocking. Called from the
 * command handlers, i.e. the DAP request task. */
static void swo_drain_events(void)
{
    if (!s_evt_q) return;

    uart_event_t ev;
    while (xQueueReceive(s_evt_q, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            /* Bytes were lost: the ISR already reported this, and the driver
             * dropped the oldest data. Surface it as the standard SWO overrun
             * flag rather than a private status field, because that is what
             * host tools already check. */
            s_ovf++;
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
        case UART_BREAK:
            /* A wire-level fault, not a buffer problem: DAP_SWO_STREAM_ERROR
             * is the matching CMSIS-DAP flag (ARM raises it from the USART
             * error callback). */
            s_stream_err = true;
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  CMSIS-DAP SWO command layer                                        */
/*                                                                    */
/*  The state machine mirrors ARM's SWO.c: TraceTransport / TraceMode / */
/*  TraceStatus plus the banked error flags. Hosts depend on the exact */
/*  status bit semantics, so this is kept verbatim rather than          */
/*  re-derived.                                                        */
/* ------------------------------------------------------------------ */

static uint8_t TraceTransport = 0U;      /* 0=off, 1=UART (DAP_SWO_Data) */
static uint8_t TraceMode      = 0U;
static uint8_t TraceStatus    = 0U;
static uint8_t TraceError[2]  = { 0U, 0U };
static uint8_t TraceError_n   = 0U;

static void ClearTrace(void)
{
    TraceError[0] = 0U;
    TraceError[1] = 0U;
    TraceError_n  = 0U;
    s_ovf     = 0;
    s_stream_err = false;
    if (s_installed) uart_flush_input(s_port);
}

/* Bytes available, straight from the UART driver's ring.
 *
 * ARM returns TraceIndexI - TraceIndexO even when capture is inactive, so a
 * host can still drain what arrived before SWO_Control(0); the equivalent here
 * is simply asking the driver. */
static uint32_t GetTraceCount(void)
{
    if (!s_installed) return 0U;
    size_t pending = 0;
    if (uart_get_buffered_data_len(s_port, &pending) != ESP_OK) return 0U;
    return (uint32_t)pending;
}

static uint8_t GetTraceStatus(void)
{
    uint8_t status;
    uint8_t n = TraceError_n;
    TraceError_n ^= 1U;
    status = (uint8_t)(TraceStatus | TraceError[n]);
    TraceError[n] = 0U;
    return status;
}

/* Fold the accumulated hardware error report into the status byte the host
 * reads. Called from every status-reporting command so an overrun is visible
 * through SWO_Status / SWO_ExtendedStatus too, not only SWO_Data. */
static uint8_t sw_status_with_errors(void)
{
    uint8_t status = GetTraceStatus();
    if (s_ovf)        status |= DAP_SWO_BUFFER_OVERRUN;
    if (s_stream_err) status |= DAP_SWO_STREAM_ERROR;
    return status;
}

/* --- SWO_Mode_UART: bring the UART up / down ---------------------------- */

/* Not static: DAP.h declares these as the public UART back-end of the SWO
 * command layer (ARM's SWO.c marks them __WEAK for the same reason). */
uint32_t SWO_Mode_UART(uint32_t enable)
{
    if (enable) {
        if (s_installed) return 1U;

        uart_config_t cfg = {
            .baud_rate  = (int)(s_baud ? s_baud : 1000000U),
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        if (uart_driver_install(s_port, SWO_BUFFER_SIZE, 0, SWO_EVT_QUEUE,
                                &s_evt_q, 0) != ESP_OK) {
            ESP_LOGE(TAG, "uart_driver_install failed");
            return 0U;
        }
        if (uart_param_config(s_port, &cfg) != ESP_OK ||
            uart_set_pin(s_port, UART_PIN_NO_CHANGE, debug_pins_swo(),
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            ESP_LOGE(TAG, "SWO UART setup failed on IO%d", debug_pins_swo());
            uart_driver_delete(s_port);
            s_evt_q = NULL;
            return 0U;
        }
        uart_flush_input(s_port);
        s_installed  = true;
        s_ovf        = 0;
        s_stream_err = false;
        s_baud       = (uint32_t)cfg.baud_rate;
        ESP_LOGI(TAG, "SWO UART ready on IO%d @ %lu bps (%u B RX ring)",
                 debug_pins_swo(), (unsigned long)s_baud, (unsigned)SWO_BUFFER_SIZE);
        return 1U;
    }

    /* Disable. Safe to free the driver here: every SWO command runs in the DAP
     * request task, and nothing else touches the peripheral, so there is no
     * task left blocked on the driver's queue. uart_driver_delete() also frees
     * s_evt_q. */
    if (s_installed) {
        uart_driver_delete(s_port);
        s_installed  = false;
        s_evt_q      = NULL;
    }
    return 1U;
}

uint32_t SWO_Baudrate_UART(uint32_t baudrate)
{
    if (!s_installed) return 0U;
    if (baudrate > SWO_UART_MAX_BAUDRATE) baudrate = SWO_UART_MAX_BAUDRATE;
    if (baudrate == 0) return 0U;

    /* Read out whatever is in flight before the rate changes, so the tail of
     * the previous stream is not decoded at the wrong baud. */
    uart_wait_tx_done(s_port, 0);
    uart_flush_input(s_port);

    if (uart_set_baudrate(s_port, (int)baudrate) != ESP_OK) {
        ESP_LOGW(TAG, "SWO baudrate %lu rejected", (unsigned long)baudrate);
        return 0U;
    }
    s_baud = baudrate;
    return baudrate;
}

uint32_t SWO_Control_UART(uint32_t active)
{
    if (!s_installed) return 0U;
    /* The data path is the driver's ring, so "start" only has to discard stale
     * bytes and "stop" has nothing to tear down (ClearTrace runs on the next
     * enable and flushes the ring). */
    if (active) uart_flush_input(s_port);
    return 1U;
}

/* --- CMSIS-DAP command handlers ---------------------------------------- */

uint32_t SWO_Transport(const uint8_t *request, uint8_t *response)
{
    uint8_t transport = *request;
    uint32_t result;

    if (TraceStatus & DAP_SWO_CAPTURE_ACTIVE) {
        result = 0U;
    } else if (transport == 0U || transport == 1U) {
        /* 2 (streaming) is compiled out via SWO_STREAM 0. */
        TraceTransport = transport;
        result = 1U;
    } else {
        result = 0U;
    }

    *response = result ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Mode(const uint8_t *request, uint8_t *response)
{
    uint8_t mode = *request;
    uint32_t result;

    if (TraceMode == DAP_SWO_UART) SWO_Mode_UART(0U);

    if (mode == DAP_SWO_OFF) {
        result = 1U;
    } else if (mode == DAP_SWO_UART) {
        result = SWO_Mode_UART(1U);
    } else {
        result = 0U;               /* Manchester is not offered */
    }

    TraceMode   = result ? mode : DAP_SWO_OFF;
    TraceStatus = 0U;

    *response = result ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Baudrate(const uint8_t *request, uint8_t *response)
{
    uint32_t baudrate = (uint32_t)request[0]        |
                        ((uint32_t)request[1] << 8)  |
                        ((uint32_t)request[2] << 16) |
                        ((uint32_t)request[3] << 24);

    if (TraceMode == DAP_SWO_UART) baudrate = SWO_Baudrate_UART(baudrate);
    else                           baudrate = 0U;

    if (baudrate == 0U) TraceStatus = 0U;

    response[0] = (uint8_t)(baudrate >> 0);
    response[1] = (uint8_t)(baudrate >> 8);
    response[2] = (uint8_t)(baudrate >> 16);
    response[3] = (uint8_t)(baudrate >> 24);
    return (4U << 16) | 4U;
}

uint32_t SWO_Control(const uint8_t *request, uint8_t *response)
{
    uint8_t  active = *request & DAP_SWO_CAPTURE_ACTIVE;
    uint32_t result;

    if (active == (TraceStatus & DAP_SWO_CAPTURE_ACTIVE)) {
        result = 1U;
    } else {
        if (active) ClearTrace();
        result = (TraceMode == DAP_SWO_UART) ? SWO_Control_UART(active) : 0U;
        if (result) TraceStatus = active;
    }

    *response = result ? DAP_OK : DAP_ERROR;
    return (1U << 16) | 1U;
}

uint32_t SWO_Status(uint8_t *response)
{
    swo_drain_events();
    uint8_t  status = sw_status_with_errors();
    uint32_t count  = GetTraceCount();

    response[0] = status;
    response[1] = (uint8_t)(count >> 0);
    response[2] = (uint8_t)(count >> 8);
    response[3] = (uint8_t)(count >> 16);
    response[4] = (uint8_t)(count >> 24);
    return 5U;
}

uint32_t SWO_ExtendedStatus(const uint8_t *request, uint8_t *response)
{
    swo_drain_events();
    uint8_t  cmd = *request;
    uint32_t num = 0U;

    if (cmd & 0x01U) {
        *response++ = sw_status_with_errors();
        num += 1U;
    }
    if (cmd & 0x02U) {
        uint32_t count = GetTraceCount();
        *response++ = (uint8_t)(count >> 0);
        *response++ = (uint8_t)(count >> 8);
        *response++ = (uint8_t)(count >> 16);
        *response++ = (uint8_t)(count >> 24);
        num += 4U;
    }
    if (cmd & 0x04U) {
        /* Trace index and tick. The index is the running byte count (driver
         * ring position plus whatever has been read out), and the tick is the
         * current microsecond clock - a query-time stamp, not the arrival time
         * of the last byte, which the host only uses for rough correlation. */
        uint32_t index = s_rx_total;
        uint32_t tick  = (uint32_t)esp_timer_get_time();

        *response++ = (uint8_t)(index >> 0);
        *response++ = (uint8_t)(index >> 8);
        *response++ = (uint8_t)(index >> 16);
        *response++ = (uint8_t)(index >> 24);
        *response++ = (uint8_t)(tick >> 0);
        *response++ = (uint8_t)(tick >> 8);
        *response++ = (uint8_t)(tick >> 16);
        *response++ = (uint8_t)(tick >> 24);
        num += 8U;
    }

    return (1U << 16) | num;
}

uint32_t SWO_Data(const uint8_t *request, uint8_t *response)
{
    swo_drain_events();

    uint8_t  status = sw_status_with_errors();
    uint32_t want;

    /* Transport 1 (UART) is the only supported one; transport 2 (streaming to
     * a dedicated bulk endpoint) is compiled out, so the host always requests
     * a length here. */
    if (TraceTransport == 1U) {
        want = (uint32_t)request[0] | ((uint32_t)request[1] << 8);
        if (want > (DAP_PACKET_SIZE - 4U)) want = DAP_PACKET_SIZE - 4U;
    } else {
        want = 0U;
    }

    /* Read exactly what is there, and report exactly what was read: the count
     * in the header is derived from the return value of the read, so the host
     * can never be told to expect bytes that were not copied. */
    int got = 0;
    if (want) {
        got = uart_read_bytes(s_port, &response[3], want, 0);
        if (got < 0) got = 0;
        s_rx_total += (uint32_t)got;
    }

    response[0] = status;
    response[1] = (uint8_t)((uint32_t)got >> 0);
    response[2] = (uint8_t)((uint32_t)got >> 8);

    return (2U << 16) | (uint32_t)(3 + got);
}

/* ------------------------------------------------------------------ */
/*  Streaming-trace hooks                                              */
/*                                                                    */
/*  SWO_STREAM is 0, so DAP.c never calls these and the CMSIS-DAP       */
/*  vendor command set does not use them. They exist only because      */
/*  DAP.h declares them and a future streaming transport would need    */
/*  them; keeping them explicit beats a link error if that changes.    */
/* ------------------------------------------------------------------ */

void SWO_QueueTransfer(uint8_t *buf, uint32_t num) { (void)buf; (void)num; }
void SWO_AbortTransfer(void) { }
void SWO_TransferComplete(void) { }

#endif  /* SWO_UART != 0 */
