#include "boot_splash.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled_ssd1306.h"

/* The panel cell is 8x16, so a 16px-tall band is one text row. */
#define SPLASH_ROWS      4
#define BAR_Y            46
#define BAR_H            8
#define BAR_MARGIN       8
#define BAR_W            (OLED_WIDTH - 2 * BAR_MARGIN)

static void text_centered(int y, const char *s, bool inv)
{
    int n = (int)strlen(s);
    int w = n * OLED_CHAR_W;
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) x = 0;
    oled_text(x, y, s, inv);
}

static void text_scaled(int y, const char *s)
{
    /* A "big" wordmark without a second font: draw each character twice, one
     * pixel column to the right, which doubles the horizontal weight enough to
     * read as a title at 128px wide. */
    int n = (int)strlen(s);
    int w = n * OLED_CHAR_W + 1;
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) x = 0;
    for (int i = 0; i < n; i++) {
        oled_char(x + i * OLED_CHAR_W,         y, s[i], false);
        oled_char(x + i * OLED_CHAR_W + 1,     y, s[i], false);
    }
}

static void draw_bar(int filled_px)
{
    oled_rect(BAR_MARGIN, BAR_Y, BAR_W, BAR_H, true);
    if (filled_px > 0) {
        if (filled_px > BAR_W - 2) filled_px = BAR_W - 2;
        oled_fill_rect(BAR_MARGIN + 1, BAR_Y + 1, filled_px, BAR_H - 2, true);
    }
}

void boot_splash_ready(const char *ip)
{
    oled_clear();
    text_scaled(4, "NEXLINK");

    if (ip && ip[0]) {
        text_centered(24, "ready at", false);
        text_centered(40, ip, false);
    } else {
        text_centered(24, "wireless", false);
        text_centered(40, "debugger", false);
    }

    oled_hline(0, 58, OLED_WIDTH, true);
    oled_flush();
}

void boot_splash_run(const char *ip, int ms)
{
    if (ms < 100) ms = 100;

    /* Steps chosen so the whole sweep is ~ms long; each step is one flush. */
    const int steps = 24;
    const int delay_ms = ms / steps > 0 ? ms / steps : 4;

    for (int s = 0; s <= steps; s++) {
        oled_clear();

        text_scaled(4, "NEXLINK");
        text_centered(24, "wireless debugger", false);

        /* A one-line status that also proves the font row is aligned. */
        if (s < steps / 2) text_centered(40, "booting...", false);
        else               text_centered(40, ip && ip[0] ? ip : "starting", false);

        draw_bar((BAR_W - 2) * s / steps);
        oled_flush();

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    /* Hold the completed bar briefly so the transition is not a flicker. */
    vTaskDelay(pdMS_TO_TICKS(150));
}
