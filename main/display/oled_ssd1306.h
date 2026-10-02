#pragma once
#ifndef OLED_SSD1306_H
#define OLED_SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  SSD1306 128x64 monochrome OLED over I2C
 *
 *  Wiring (4-pin module):
 *    VCC -> 3V3
 *    GND -> GND
 *    SCL -> IO10
 *    SDA -> IO11
 *
 *  The driver keeps a 1024-byte framebuffer in RAM (8 pages x 128 columns,
 *  LSB of each byte = topmost row of that page) and pushes it with a single
 *  chunked I2C transfer in oled_flush().
 *
 *  All drawing uses the "on" flag: true lights a pixel, false clears it.
 *  Text helpers are opaque - they always paint the whole glyph cell (both the
 *  lit and unlit pixels) so redraws do not smear. Pass inv=true for
 *  inverse-video text, which is what menu selection highlighting uses.
 * ============================================================ */

#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_PAGES        (OLED_HEIGHT / 8)          /* 8  */
#define OLED_FB_BYTES     (OLED_WIDTH * OLED_PAGES)  /* 1024 */

/* Cell metrics. ASCII uses the 8x16 VGA font, CJK uses HZK16 16x16 - both are
 * 16px tall, so a line height of 16 gives 4 text rows on a 64px panel. */
#define OLED_CHAR_W       8
#define OLED_CHAR_H       16
#define OLED_COLS         (OLED_WIDTH / OLED_CHAR_W)   /* 16 ASCII columns  */
#define OLED_ROWS         (OLED_HEIGHT / OLED_CHAR_H)  /* 4 text rows       */
#define OLED_CJK_W        16
#define OLED_CJK_H        16
#define OLED_CJK_COLS     (OLED_WIDTH / OLED_CJK_W)    /* 8 CJK columns     */

/* ---- Lifecycle ---- */

/* Bring up I2C0 on SCL/SDA and initialise the panel.
 * Returns ESP_ERR_NOT_FOUND if no SSD1306 answers at OLED_I2C_ADDR; the rest
 * of the firmware is expected to keep running in that case. */
esp_err_t oled_init(void);

/* True once oled_init() has successfully talked to the panel. Every draw call
 * is a safe no-op while this is false. */
bool oled_is_ready(void);

/* Push the whole framebuffer to the panel. */
esp_err_t oled_flush(void);

/* ---- Slide-transition support ---- */

/* Copy the current framebuffer (OLED_FB_BYTES) into `dst`. */
void oled_fb_snapshot(uint8_t *dst);

/* Animate a vertical slide from the `old` snapshot into whatever the
 * framebuffer holds now, flushing every intermediate frame. Call this ONLY
 * from the UI render task (it owns the panel). dir>0: new screen enters from
 * the bottom (next); dir<0: new screen enters from the top (previous/back).
 * frames: 1..8, one display page (8px) per frame. */
void oled_slide_from(const uint8_t *old, int dir, int frames);

/* ---- Direct panel control ---- */
esp_err_t oled_set_power(bool on);
esp_err_t oled_set_contrast(uint8_t contrast_0_255);
esp_err_t oled_set_invert(bool invert);

/* ---- Framebuffer primitives ---- */
void oled_clear(void);
void oled_fill(bool on);
void oled_pixel(int x, int y, bool on);
void oled_hline(int x, int y, int w, bool on);
void oled_vline(int x, int y, int h, bool on);
void oled_rect(int x, int y, int w, int h, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_invert_rect(int x, int y, int w, int h);

/* Blit a raw row-major bitmap (1 bit per pixel, MSB leftmost, bytes padded to
 * a whole byte per row). Used for icons. */
void oled_bitmap(int x, int y, int w, int h, const uint8_t *bitmap, bool inv);

/* ---- Text ---- */

/* One 8x16 ASCII glyph. Bytes outside 0x20..0x7F render as a blank cell. */
void oled_char(int x, int y, char ch, bool inv);

/* ASCII string, one glyph per 8px. Characters past the right edge are clipped. */
void oled_text(int x, int y, const char *text, bool inv);

/* 16x16 CJK glyph from an HZK16 bitmap (32 bytes, 2 bytes per row, MSB left). */
void oled_cjk(int x, int y, const uint8_t *bitmap, bool inv);

/* Mixed UTF-8 string: ASCII advances 8px, 3-byte CJK advances 16px. Unmapped
 * non-ASCII codepoints draw a hollow 16x16 box so the gap is visible.
 * Returns the x coordinate just past the last glyph. */
int  oled_utf8(int x, int y, const char *text, bool inv);

/* Length in pixels that oled_utf8() would consume, capped at max_px. */
int  oled_utf8_width(const char *text, int max_px);

/* ---- Remote mirroring (HTTP / WS screen preview) ---- */
const uint8_t *oled_get_fb(void);

#ifdef __cplusplus
}
#endif

#endif /* OLED_SSD1306_H */
