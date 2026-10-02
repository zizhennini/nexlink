/*
 * oled_ssd1306.c - SSD1306 128x64 monochrome OLED driver for ESP32-S3.
 *
 * Replaces the ST7735S RGB565 panel. Only two wires (SCL/SDA) are needed, and
 * the framebuffer drops from 25600 bytes of RGB565 to 1024 bytes of 1bpp, so it
 * lives comfortably in internal RAM instead of PSRAM.
 *
 * Text uses the two glyph sources already carried by the project:
 *   - font8x16.h  : 8x16 ASCII (VGA derived), same as before
 *   - HZK16       : 16x16 GB2312 CJK, looked up through cjk_font.c
 */
#include "oled_ssd1306.h"
#include "pinout.h"
#include "font8x16.h"
#include "cjk_font.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "oled";

/* SSD1306 control bytes: 0x00 = following bytes are commands,
 * 0x40 = following bytes are display RAM data. */
#define SSD1306_CTRL_CMD   0x00
#define SSD1306_CTRL_DATA  0x40

/* Chunk size for one I2C transaction. The panel has no length limit, but
 * keeping transfers small bounds the stack/buffer cost and plays nicely with
 * other tasks sharing the bus. */
#define OLED_CHUNK         128

static i2c_master_bus_handle_t s_bus  = NULL;
static i2c_master_dev_handle_t s_dev  = NULL;
static bool s_ready = false;

/* One page of pixels + the control byte in front of it. */
static uint8_t s_fb[OLED_FB_BYTES];
static uint8_t s_tx[1 + OLED_CHUNK];

/* ------------------------------------------------------------------ */
/* Low level I2C                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t oled_cmd(const uint8_t *cmds, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[1 + 32];
    if (n > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = SSD1306_CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

static esp_err_t oled_cmd1(uint8_t c)
{
    return oled_cmd(&c, 1);
}

/* ------------------------------------------------------------------ */
/* Panel setup                                                        */
/* ------------------------------------------------------------------ */

esp_err_t oled_init(void)
{
    /* --- I2C master bus on the OLED pins --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Probe before adding the device so a missing panel is a clean, logged
     * failure instead of a stream of NACK errors. */
    err = i2c_master_probe(s_bus, OLED_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SSD1306 at 0x%02X (SDA=IO%d SCL=IO%d) - display disabled",
                 OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    /* --- SSD1306 power-on sequence for a 128x64 panel --- */
    static const uint8_t init_seq[] = {
        0xAE,               /* display off while we configure            */
        0xD5, 0x80,         /* clock divide ratio / oscillator frequency  */
        0xA8, 0x3F,         /* multiplex ratio = 64                       */
        0xD3, 0x00,         /* display offset = 0                         */
        0x40,               /* display start line = 0                     */
        0x8D, 0x14,         /* charge pump: enable internal VCC           */
        0x20, 0x00,         /* memory addressing mode = horizontal        */
        /* Panel is mounted upside down: SEG normal + COM normal is the exact
         * 180-degree rotation (up-down flip AND left-right mirror) of the
         * usual 0xA1 + 0xC8 orientation. All drawing keeps working in logical
         * framebuffer coordinates; only the scan mapping changes. */
        0xA0,               /* segment remap: column 0 -> SEG0            */
        0xC0,               /* COM output scan direction: normal          */
        0xDA, 0x12,         /* COM pins: alternative, no left/right remap */
        0x81, 0xCF,         /* contrast                                   */
        0xD9, 0xF1,         /* pre-charge period                          */
        0xDB, 0x40,         /* VCOMH deselect level                       */
        0xA4,               /* resume from display RAM (not force-on)     */
        0xA6,               /* normal (non-inverted) display              */
        0xAF,               /* display on                                 */
    };
    err = oled_cmd(init_seq, sizeof(init_seq));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    memset(s_fb, 0, sizeof(s_fb));
    err = oled_flush();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "SSD1306 %dx%d ready (I2C addr 0x%02X, SDA=IO%d, SCL=IO%d)",
             OLED_WIDTH, OLED_HEIGHT, OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
    return ESP_OK;
}

bool oled_is_ready(void)
{
    return s_ready;
}

esp_err_t oled_flush(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    /* Window the whole panel, then stream the 8 pages. */
    ESP_RETURN_ON_ERROR(oled_cmd1(0x21), TAG, "set column range");
    ESP_RETURN_ON_ERROR(oled_cmd1(0x00), TAG, "col start");
    ESP_RETURN_ON_ERROR(oled_cmd1(OLED_WIDTH - 1), TAG, "col end");
    ESP_RETURN_ON_ERROR(oled_cmd1(0x22), TAG, "set page range");
    ESP_RETURN_ON_ERROR(oled_cmd1(0x00), TAG, "page start");
    ESP_RETURN_ON_ERROR(oled_cmd1(OLED_PAGES - 1), TAG, "page end");

    s_tx[0] = SSD1306_CTRL_DATA;
    for (int off = 0; off < OLED_FB_BYTES; off += OLED_CHUNK) {
        int n = OLED_FB_BYTES - off;
        if (n > OLED_CHUNK) n = OLED_CHUNK;
        memcpy(&s_tx[1], &s_fb[off], n);
        esp_err_t err = i2c_master_transmit(s_dev, s_tx, n + 1, 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t oled_set_power(bool on)
{
    return oled_cmd1(on ? 0xAF : 0xAE);
}

/* ---- Slide-transition support (used by menu_ui page changes) ---- */

void oled_fb_snapshot(uint8_t *dst)
{
    memcpy(dst, s_fb, OLED_FB_BYTES);
}

/*
 * Frame-stepped vertical slide from the previous screen (the 'old' snapshot)
 * into the current framebuffer contents. Every intermediate frame is flushed
 * here, so this MUST run from the single UI render task - never concurrently
 * with another flush (the panel sits on a shared I2C bus).
 *   dir > 0: content moves up; old exits through the top, the new screen
 *            slides in from the bottom ("next page").
 *   dir < 0: content moves down; old exits through the bottom, the new screen
 *            slides in from the top ("previous page" / "back").
 * frames is clamped to 1..OLED_PAGES; each frame advances a whole page (8 px)
 * so a frame is pure memcpy - no bit shuffling across page boundaries.
 */
void oled_slide_from(const uint8_t *old, int dir, int frames)
{
    if (!s_ready) return;
    if (frames < 1) frames = 1;
    if (frames > OLED_PAGES) frames = OLED_PAGES;

    uint8_t newfb[OLED_FB_BYTES];
    memcpy(newfb, s_fb, OLED_FB_BYTES);

    for (int f = 1; f <= frames; f++) {
        int moved = (OLED_PAGES * f) / frames;
        if (moved > OLED_PAGES) moved = OLED_PAGES;

        if (dir > 0) {
            int vis = OLED_PAGES - moved;   /* pages of the old screen still seen */
            for (int p = 0; p < vis; p++)
                memcpy(&s_fb[p * OLED_WIDTH], &old[(p + moved) * OLED_WIDTH], OLED_WIDTH);
            for (int p = vis; p < OLED_PAGES; p++)
                memcpy(&s_fb[p * OLED_WIDTH], &newfb[(p - vis) * OLED_WIDTH], OLED_WIDTH);
        } else {
            for (int p = 0; p < moved; p++)
                memcpy(&s_fb[p * OLED_WIDTH], &newfb[(OLED_PAGES - moved + p) * OLED_WIDTH], OLED_WIDTH);
            for (int p = moved; p < OLED_PAGES; p++)
                memcpy(&s_fb[p * OLED_WIDTH], &old[(p - moved) * OLED_WIDTH], OLED_WIDTH);
        }
        oled_flush();
    }

    /* Leave the framebuffer exactly in the new page's state. */
    memcpy(s_fb, newfb, OLED_FB_BYTES);
}

esp_err_t oled_set_contrast(uint8_t contrast_0_255)
{
    uint8_t c[2] = { 0x81, contrast_0_255 };
    return oled_cmd(c, 2);
}

esp_err_t oled_set_invert(bool invert)
{
    return oled_cmd1(invert ? 0xA7 : 0xA6);
}

/* ------------------------------------------------------------------ */
/* Framebuffer primitives                                             */
/* ------------------------------------------------------------------ */

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_fill(bool on)
{
    memset(s_fb, on ? 0xFF : 0x00, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t *p = &s_fb[(y >> 3) * OLED_WIDTH + x];
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) *p |= mask;
    else    *p &= (uint8_t)~mask;
}

void oled_hline(int x, int y, int w, bool on)
{
    if (y < 0 || y >= OLED_HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > OLED_WIDTH) w = OLED_WIDTH - x;
    if (w <= 0) return;

    uint8_t *base = &s_fb[(y >> 3) * OLED_WIDTH];
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) {
        for (int i = 0; i < w; i++) base[x + i] |= mask;
    } else {
        mask = (uint8_t)~mask;
        for (int i = 0; i < w; i++) base[x + i] &= mask;
    }
}

void oled_vline(int x, int y, int h, bool on)
{
    if (x < 0 || x >= OLED_WIDTH) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > OLED_HEIGHT) h = OLED_HEIGHT - y;
    if (h <= 0) return;
    for (int i = 0; i < h; i++) oled_pixel(x, y + i, on);
}

void oled_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    oled_hline(x, y, w, on);
    oled_hline(x, y + h - 1, w, on);
    oled_vline(x, y, h, on);
    oled_vline(x + w - 1, y, h, on);
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) oled_hline(x, y + i, w, on);
}

void oled_invert_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > OLED_WIDTH) w = OLED_WIDTH - x;
    if (y < 0) { h += y; y = 0; }
    if (y + h > OLED_HEIGHT) h = OLED_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = y; yy < y + h; yy++) {
        uint8_t *base = &s_fb[(yy >> 3) * OLED_WIDTH];
        uint8_t mask = (uint8_t)(1u << (yy & 7));
        for (int xx = x; xx < x + w; xx++) base[xx] ^= mask;
    }
}

void oled_bitmap(int x, int y, int w, int h, const uint8_t *bitmap, bool inv)
{
    if (!bitmap) return;
    int stride = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        const uint8_t *src = bitmap + row * stride;
        for (int col = 0; col < w; col++) {
            bool lit = (src[col >> 3] >> (7 - (col & 7))) & 1;
            oled_pixel(x + col, y + row, inv ? !lit : lit);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                               */
/* ------------------------------------------------------------------ */

void oled_char(int x, int y, char ch, bool inv)
{
    const uint8_t *glyph = font_8x16[font_8x16_index(ch)];
    for (int row = 0; row < OLED_CHAR_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < OLED_CHAR_W; col++) {
            bool lit = (bits >> (7 - col)) & 1;
            oled_pixel(x + col, y + row, inv ? !lit : lit);
        }
    }
}

void oled_text(int x, int y, const char *text, bool inv)
{
    if (!text) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        if (cx + OLED_CHAR_W > OLED_WIDTH) break;   /* clip at right edge */
        if (cx >= 0) oled_char(cx, y, *p, inv);
        cx += OLED_CHAR_W;
    }
}

void oled_cjk(int x, int y, const uint8_t *bitmap, bool inv)
{
    if (!bitmap) return;
    /* HZK16 layout: 16 rows, 2 bytes each, big endian, MSB is the left pixel. */
    for (int row = 0; row < 16; row++) {
        uint16_t bits = (uint16_t)((bitmap[row * 2] << 8) | bitmap[row * 2 + 1]);
        for (int col = 0; col < 16; col++) {
            bool lit = (bits >> (15 - col)) & 1;
            oled_pixel(x + col, y + row, inv ? !lit : lit);
        }
    }
}

static void cjk_placeholder(int x, int y, bool inv)
{
    oled_fill_rect(x, y, 16, 16, inv);
    oled_rect(x, y, 16, 16, !inv);
}

int oled_utf8(int x, int y, const char *text, bool inv)
{
    if (!text) return x;
    int cx = x;
    const uint8_t *p = (const uint8_t *)text;

    while (*p) {
        if (*p < 0x80) {
            /* ASCII */
            if (cx + OLED_CHAR_W > OLED_WIDTH) break;
            if (cx >= 0) oled_char(cx, y, (char)*p, inv);
            cx += OLED_CHAR_W;
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            /* 2-byte UTF-8: not mapped by HZK16, show a placeholder */
            if (cx + OLED_CJK_W > OLED_WIDTH) break;
            if (cx >= 0) cjk_placeholder(cx, y, inv);
            cx += OLED_CJK_W;
            p += (*p && p[1]) ? 2 : 1;
        } else if ((*p & 0xF0) == 0xE0) {
            /* 3-byte UTF-8: CJK */
            if (cx + OLED_CJK_W > OLED_WIDTH) break;
            if (cx >= 0) {
                const uint8_t *bmp = cjk_get_bitmap_utf8(p, 3);
                if (bmp) oled_cjk(cx, y, bmp, inv);
                else     cjk_placeholder(cx, y, inv);
            }
            cx += OLED_CJK_W;
            p += 3;
        } else {
            /* 4-byte UTF-8 or stray continuation byte: skip one byte */
            if (cx + OLED_CJK_W > OLED_WIDTH) break;
            if (cx >= 0) cjk_placeholder(cx, y, inv);
            cx += OLED_CJK_W;
            p++;
        }
    }
    return cx;
}

int oled_utf8_width(const char *text, int max_px)
{
    if (!text) return 0;
    int w = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        int adv;
        if (*p < 0x80) { adv = OLED_CHAR_W; p++; }
        else if ((*p & 0xE0) == 0xC0) { adv = OLED_CJK_W; p += (p[1] ? 2 : 1); }
        else if ((*p & 0xF0) == 0xE0) { adv = OLED_CJK_W; p += 3; }
        else { adv = OLED_CJK_W; p++; }
        if (w + adv > max_px) break;
        w += adv;
    }
    return w;
}

const uint8_t *oled_get_fb(void)
{
    return s_ready ? s_fb : NULL;
}
