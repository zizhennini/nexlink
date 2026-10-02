#pragma once
/*
 * capture.c/h - timestamped capture log for the DUT serial link.
 *
 * WHY
 * ---
 * The raw RX streams (web / MCP / USB-TTL / TCP) are byte pipes: they carry no
 * timing and no direction, so a protocol analyst cannot tell how far apart two
 * replies were, or whether a byte came from the target or from us. This module
 * keeps a bounded, chunked history of everything that crossed UART1 in BOTH
 * directions, each chunk stamped with the microsecond clock.
 *
 * DESIGN
 * ------
 * Chunks, not bytes. UART data arrives in bursts anyway (one callback per
 * event), so a timestamp per burst is both cheaper and more faithful than a
 * timestamp per byte: 8 bytes of overhead per up to CAPTURE_PAYLOAD_MAX bytes.
 * The ring is fixed-size and allocated once, so the log can never grow into
 * the rest of the firmware - it silently overwrites its oldest chunks.
 *
 * Direction is recorded because it is free here and impossible to reconstruct
 * later from the byte stream.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time bounds. The values chosen give a fixed ~35 kB ring. ---- */
#define CAPTURE_SLOTS_DEFAULT    256
#define CAPTURE_SLOTS_MAX        1024
#define CAPTURE_PAYLOAD_MAX      128      /* bytes stored per chunk */
#define CAPTURE_DEFAULT_MS       60000    /* keep >= 60 s of history */

typedef enum {
    CAPTURE_DIR_RX = 0,   /* target -> us  */
    CAPTURE_DIR_TX = 1,   /* us -> target  */
} capture_dir_t;

/* One entry as exposed to callers (payload copied out). */
typedef struct {
    uint32_t seq;                          /* monotonic, never reused */
    int64_t  timestamp_us;                 /* esp_timer clock */
    uint8_t  dir;                           /* capture_dir_t */
    uint16_t len;
    uint8_t  data[CAPTURE_PAYLOAD_MAX];
    bool     truncated;                     /* source run was longer than len */
} capture_chunk_t;

/* Allocate the ring. `slots` is clamped to [16, CAPTURE_SLOTS_MAX].
 * Safe to call once at boot; a second call is a no-op. */
void capture_init(size_t slots);

/* Append one run of bytes. Long runs are split across several chunks so no
 * data is lost. No-op when the ring was never allocated. */
void capture_record(capture_dir_t dir, const uint8_t *data, size_t len);

/* Single-character marker (used for line feeds / events worth pinpointing). */
void capture_mark(capture_dir_t dir, uint8_t byte);

/* ---- Queries ---- */

/* Copy up to `max` chunks with seq >= since, oldest first. Returns how many
 * were copied; *next_since receives the seq to pass next time. */
size_t capture_read(uint32_t since, capture_chunk_t *out, size_t max,
                    uint32_t *next_since);

/* Ring contents summary. */
uint32_t capture_oldest_seq(void);   /* seq of the oldest retained chunk   */
uint32_t capture_next_seq(void);     /* seq the next chunk will get        */
size_t   capture_count(void);        /* chunks currently retained          */
size_t   capture_capacity(void);     /* configured slot count              */
size_t   capture_bytes(void);        /* payload bytes currently retained   */
bool     capture_dropped(void);      /* true if overwrites lost data       */

void     capture_clear(void);        /* drop history, keep counters fresh  */
void     capture_reset_counters(void); /* also zero the overwrite flag     */

#ifdef __cplusplus
}
#endif
