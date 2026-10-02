#include "boot_splash.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled_ssd1306.h"

/* The panel cell is 8x16, so one text row occupies 16px of the 64px height. */
#define WORDMARK     "NexLink"
#define WORDMARK_LEN 7

/* A "big" wordmark without a second font: draw each glyph twice, one pixel
 * column apart. That doubles the stroke weight enough to read as a title at
 * 128px wide, and costs nothing but a second blit. */
#define WORDMARK_W   (WORDMARK_LEN * OLED_CHAR_W + 1)

#define WORDMARK_Y   24            /* vertically centred-ish: 24..40 */

static void draw_wordmark(int x)
{
    for (int i = 0; i < WORDMARK_LEN; i++) {
        oled_char(x + i * OLED_CHAR_W,     WORDMARK_Y, WORDMARK[i], false);
        oled_char(x + i * OLED_CHAR_W + 1, WORDMARK_Y, WORDMARK[i], false);
    }
}

/* Clear the strip the wordmark moves through, so a previous frame's pixels can
 * never survive as "ghosting" behind the next one. The doubled glyph is one
 * pixel wider than its cell, which is exactly the kind of overlap that shows
 * up as two words on top of each other. */
static void clear_wordmark_band(void)
{
    oled_fill_rect(0, WORDMARK_Y - 1, OLED_WIDTH, 18, false);
}

void boot_splash_run(int ms)
{
    if (ms < 120) ms = 120;

    const int steps = 18;
    const int frame_ms = ms / steps > 0 ? ms / steps : 6;
    const int travel = OLED_WIDTH - WORDMARK_W;   /* slide distance */

    for (int s = 0; s <= steps; s++) {
        /* Ease-out: fast at first, settling at the end (quadratic, fixed point
         * so there is no floating point in the boot path). */
        int t = s * 100 / steps;                   /* 0..100 */
        int eased = 100 - ((100 - t) * (100 - t)) / 100;
        int x = travel - (travel * eased) / 100;

        oled_clear();
        clear_wordmark_band();
        draw_wordmark(x);
        oled_flush();

        vTaskDelay(pdMS_TO_TICKS(frame_ms));
    }

    /* A short hold on the settled wordmark, so the transition into the menu
     * reads as intentional rather than as a flicker. */
    vTaskDelay(pdMS_TO_TICKS(220));
}

void boot_splash_ready(const char *ip)
{
    oled_clear();
    clear_wordmark_band();
    draw_wordmark((OLED_WIDTH - WORDMARK_W) / 2);

    if (ip && ip[0]) {
        int w = (int)strlen(ip) * OLED_CHAR_W;
        int x = (OLED_WIDTH - w) / 2;
        if (x < 0) x = 0;
        oled_text(x, 44, ip, false);
    }
    oled_flush();
}
