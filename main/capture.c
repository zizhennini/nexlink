#include "capture.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "capture";

/* ------------------------------------------------------------------ */
/*  Ring storage                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t  timestamp_us;
    uint32_t seq;
    uint16_t len;
    uint8_t  dir;
    uint8_t  truncated;
    uint8_t  data[CAPTURE_PAYLOAD_MAX];
} cap_slot_t;

static cap_slot_t *s_ring;
static size_t      s_slots;        /* configured capacity */
static size_t      s_head;         /* index the NEXT record goes into */
static size_t      s_count;        /* records currently retained */
static uint32_t    s_seq;          /* seq assigned to the next record */
static size_t      s_bytes;        /* payload bytes currently retained */
static bool        s_dropped;      /* set once an overwrite lost data */

/* capture_record() runs in the UART event task while the HTTP handler reads
 * the ring from the httpd task, so the structure is guarded by a spinlock:
 * every critical section is a bounded memcpy (<= CAPTURE_PAYLOAD_MAX bytes),
 * which keeps the disables-IRQ window short and avoids priority inversion
 * between the two tasks. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/*  Internal helpers (must be called with the lock held)               */
/* ------------------------------------------------------------------ */

static void slot_store(const uint8_t *data, size_t len, uint8_t dir,
                       bool truncated)
{
    cap_slot_t *slot = &s_ring[s_head];

    slot->timestamp_us = esp_timer_get_time();
    slot->seq          = s_seq++;
    slot->dir          = dir;
    slot->len          = (uint16_t)len;
    slot->truncated    = truncated ? 1U : 0U;
    if (len) memcpy(slot->data, data, len);

    /* Bytes evicted by advancing onto an occupied slot. */
    if (s_count == s_slots) {
        s_bytes -= s_ring[s_head].len;
        s_dropped = true;
    } else {
        s_count++;
    }
    s_bytes += len;

    s_head = (s_head + 1) % s_slots;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void capture_init(size_t slots)
{
    if (s_ring) return;                       /* already allocated */

    if (slots < 16) slots = 16;
    if (slots > CAPTURE_SLOTS_MAX) slots = CAPTURE_SLOTS_MAX;

    /* Internal RAM: the ring is written from a task that may run while the
     * cache is busy, and 35 kB is well within budget on this part. */
    s_ring = heap_caps_calloc(slots, sizeof(cap_slot_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring allocation failed (%u slots x %u B)",
                 (unsigned)slots, (unsigned)sizeof(cap_slot_t));
        return;
    }

    s_slots   = slots;
    s_head    = 0;
    s_count   = 0;
    s_seq     = 0;
    s_bytes   = 0;
    s_dropped = false;

    ESP_LOGI(TAG, "capture ring ready: %u slots x %u B payload (%u kB)",
             (unsigned)s_slots, (unsigned)CAPTURE_PAYLOAD_MAX,
             (unsigned)((s_slots * sizeof(cap_slot_t)) / 1024));
}

void capture_record(capture_dir_t dir, const uint8_t *data, size_t len)
{
    if (!s_ring || !data || !len) return;

    portENTER_CRITICAL(&s_lock);
    while (len) {
        size_t n = (len > CAPTURE_PAYLOAD_MAX) ? CAPTURE_PAYLOAD_MAX : len;
        slot_store(data, n, (uint8_t)dir, len > n);
        data += n;
        len  -= n;
    }
    portEXIT_CRITICAL(&s_lock);
}

void capture_mark(capture_dir_t dir, uint8_t byte)
{
    capture_record(dir, &byte, 1);
}

size_t capture_read(uint32_t since, capture_chunk_t *out, size_t max,
                    uint32_t *next_since)
{
    size_t n = 0;

    if (next_since) *next_since = since;
    if (!s_ring || !out || !max) return 0;

    portENTER_CRITICAL(&s_lock);

    /* Oldest retained seq; anything older has been overwritten. */
    uint32_t oldest = (s_seq > (uint32_t)s_count) ? (s_seq - (uint32_t)s_count) : 0U;
    if (since < oldest) since = oldest;

    if (since < s_seq) {
        size_t start = (s_head + s_slots - s_count) % s_slots;
        size_t skip  = (size_t)(since - oldest);

        for (size_t i = skip; i < s_count && n < max; i++) {
            const cap_slot_t *slot = &s_ring[(start + i) % s_slots];
            out[n].seq          = slot->seq;
            out[n].timestamp_us = slot->timestamp_us;
            out[n].dir          = slot->dir;
            out[n].len          = slot->len;
            out[n].truncated    = slot->truncated != 0;
            if (slot->len) memcpy(out[n].data, slot->data, slot->len);
            n++;
        }
    }

    portEXIT_CRITICAL(&s_lock);

    if (next_since) {
        *next_since = (n && s_ring) ? (out[n - 1].seq + 1U) : since;
    }
    return n;
}

uint32_t capture_oldest_seq(void)
{
    if (!s_ring) return 0;
    portENTER_CRITICAL(&s_lock);
    uint32_t oldest = (s_seq > (uint32_t)s_count) ? (s_seq - (uint32_t)s_count) : 0U;
    portEXIT_CRITICAL(&s_lock);
    return oldest;
}

uint32_t capture_next_seq(void)
{
    if (!s_ring) return 0;
    portENTER_CRITICAL(&s_lock);
    uint32_t next = s_seq;
    portEXIT_CRITICAL(&s_lock);
    return next;
}

size_t capture_count(void)
{
    if (!s_ring) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_count;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

size_t capture_capacity(void) { return s_slots; }

size_t capture_bytes(void)
{
    if (!s_ring) return 0;
    portENTER_CRITICAL(&s_lock);
    size_t n = s_bytes;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool capture_dropped(void)
{
    if (!s_ring) return false;
    portENTER_CRITICAL(&s_lock);
    bool d = s_dropped;
    portEXIT_CRITICAL(&s_lock);
    return d;
}

void capture_clear(void)
{
    if (!s_ring) return;
    portENTER_CRITICAL(&s_lock);
    s_head  = 0;
    s_count = 0;
    s_bytes = 0;
    /* s_seq keeps counting: clients that cached a seq must not be served a
     * different chunk under the same number after a clear. */
    portEXIT_CRITICAL(&s_lock);
}

void capture_reset_counters(void)
{
    capture_clear();
    if (!s_ring) return;
    portENTER_CRITICAL(&s_lock);
    s_dropped = false;
    portEXIT_CRITICAL(&s_lock);
}
