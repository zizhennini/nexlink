#include "boot_splash.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled_ssd1306.h"
#include "ui_assets.h"

/*
 * Power-on animation. The requested effect was LVGL's lv_anim with a scale plus
 * an opacity fade; this project has no LVGL (the SSD1306 driver is our own, the
 * menu is our own), so the same two effects are produced from the framebuffer
 * primitives:
 *
 *   scale   - the wordmark is drawn from the small bitmap face with an integer
 *             scale factor that starts large and settles, which reads exactly
 *             like a "pop" (snap to full size, then contract to rest).
 *   opacity - a 1-bit panel has no alpha, so the fade is a gradient of pixel
 *             DENSITY: each step reveals a sparser-to-denser stipple mask, so
 *             the text appears to materialise out of the background. This is
 *             the standard trick for fades on monochrome OLEDs.
 *
 * No images are imported: the wordmark is drawn from the generated 5x7 glyph
 * table (vector-like stroke scaling of the bitmap face), so there is nothing to
 * flash beyond the code.
 */

#define WORD      "NexLink"
#define WORD_LEN  7

/* Scale range: 4 fills the width (7 * 5 * 4 = 140 > 128, so the first frames
 * clip slightly, which is what makes it read as an overshoot), settling at 3
 * (7 * 5 * 3 = 105, comfortably inside with margins). */
#define SCALE_MAX 4
#define SCALE_MIN 3

/* Draw one scaled glyph run, centred on `cy`, with a stipple mask giving the
 * fade. mask_bit is advanced for every drawn pixel so the dither pattern is
 * stable across frames instead of crawling. */
static void draw_word_scaled(int scale, int level, int cy)
{
    const int adv = (UI_FONT_W + 1) * scale;
    const int w = WORD_LEN * adv - scale;
    int x0 = (OLED_WIDTH - w) / 2;
    if (x0 < 0) x0 = 0;

    int gh = UI_FONT_H * scale;
    int y0 = cy - gh / 2;
    if (y0 < 0) y0 = 0;

    /* Density levels: 0 = almost nothing, 4 = all pixels. The threshold is a
     * 2-bit ordered dither so the pattern is even, not random-looking. */
    static const int reveal[5] = { 36, 27, 18, 9, 0 };   /* threshold out of 36 */
    int thresh = reveal[level > 4 ? 4 : level];

    int x = x0;
    for (const char *p = WORD; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < UI_FONT_FIRST || ch > UI_FONT_LAST) ch = '?';
        const uint8_t *g = ui_font5x7[ch - UI_FONT_FIRST];

        for (int r = 0; r < UI_FONT_H; r++) {
            for (int c = 0; c < UI_FONT_W; c++) {
                if (!((g[r] >> (7 - c)) & 1)) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y0 + r * scale + sy;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + c * scale + sx;
                        if (px >= OLED_WIDTH) continue;
                        /* ordered 6x6 dither (values 0..35) */
                        int d = ((px % 6) * 6 + (py % 6)) % 36;
                        if (d >= thresh) oled_pixel(px, py, true);
                    }
                }
            }
        }
        x += adv;
    }
}

void boot_splash_run(int ms)
{
    if (ms < 200) ms = 200;

    /* 1.2s total by default, in 24 steps. The scale snaps to full in the first
     * fifth and then settles; opacity ramps across the whole animation. */
    const int steps = 24;
    const int frame_ms = ms / steps > 0 ? ms / steps : 8;
    const int cy = OLED_HEIGHT / 2;

    for (int s = 0; s <= steps; s++) {
        /* level 0..4 across the animation */
        int level = s * 4 / steps;

        /* scale: overshoot immediately, then contract (ease-out on the tail) */
        int scale;
        if (s == 0)            scale = SCALE_MAX;
        else if (s < steps / 4) scale = SCALE_MAX;
        else                    scale = SCALE_MIN;

        oled_clear();
        draw_word_scaled(scale, level, cy);
        oled_flush();

        vTaskDelay(pdMS_TO_TICKS(frame_ms));
    }

    /* Settle frame: full density, resting size. */
    oled_clear();
    draw_word_scaled(SCALE_MIN, 4, cy);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(180));
}

void boot_splash_ready(const char *ip)
{
    oled_clear();
    draw_word_scaled(SCALE_MIN, 4, ip && ip[0] ? 22 : OLED_HEIGHT / 2);

    if (ip && ip[0]) {
        int w = (int)strlen(ip) * (UI_FONT_W + 1);
        int x = (OLED_WIDTH - w) / 2;
        if (x < 0) x = 0;
        /* Address in the small face under the wordmark. */
        for (const char *p = ip; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch < UI_FONT_FIRST || ch > UI_FONT_LAST) ch = '?';
            const uint8_t *g = ui_font5x7[ch - UI_FONT_FIRST];
            for (int r = 0; r < UI_FONT_H; r++)
                for (int c = 0; c < UI_FONT_W; c++)
                    if ((g[r] >> (7 - c)) & 1) oled_pixel(x + c, 44 + r, true);
            x += UI_FONT_W + 1;
        }
    }
    oled_flush();
}
