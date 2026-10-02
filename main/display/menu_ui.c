/*
 * menu_ui.c - OLED menu for NexLink (SSD1306 128x64, I2C).
 *
 * PATTERN (copied from how the mature firmware menus are actually written)
 * -----------------------------------------------------------------------
 * This file was rewritten after reading the real source of Meshtastic's menu
 * layer (src/graphics/draw/MenuHandler.{h,cpp} and SharedUIDisplay.cpp). The
 * pattern those use, and which this now follows, is:
 *
 *   1. A menu IS A TABLE. A static array of option labels plus a dispatch
 *      callback that switches on the selected index. Nothing else.
 *   2. "Back" IS AN ORDINARY ITEM, item 0 of every nested list. Going back is a
 *      visible choice, not an invisible mode.
 *   3. NO NAVIGATION STATE MACHINE. There is exactly one cursor and one
 *      "current menu" pointer in this whole file. The previous revision kept a
 *      stack, a depth, a view enum, a selected index and a scroll offset that
 *      all had to agree with each other, and every bug it had came from two of
 *      them disagreeing. A table + a callback cannot get out of sync.
 *   4. CLEAR BEFORE DRAWING. The selection is an inverted band: the band is
 *      filled black first and the glyphs are drawn inverted on top. The
 *      reference code does `setColor(BLACK); fillRect(...)` before every
 *      highlight for the same reason - without it, a longer previous line
 *      leaves its tail pixels behind and two strings appear on top of each
 *      other.
 *
 * KEY MODEL (one meaning everywhere, no per-page exceptions)
 *   SW1 = up      SW3 = down      SW2 = select
 *   SW2 held      = back to the root list
 *
 * LAYOUT (the panel is 4 rows of 16px: y = 0, 16, 32, 48)
 *   y=0   title, with a separator line under it
 *   y=16  menu items, one per row, up to 3 visible, window follows the cursor
 *   y=48  status/hint row (also the value editor for adjustable entries)
 *
 * PAINTING RULE: menu_render() is called from the UI task only. Key handlers
 * mutate a cursor and kick that task; they never touch the panel. The OLED
 * shares I2C with the I2C monitor, so a second writer would corrupt both.
 */
#include "menu_ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#include "oled_ssd1306.h"
#include "pinout.h"
#include "pin_config.h"
#include "serial_bridge.h"
#include "tcp_server.h"
#include "usb_ttl.h"
#include "wifi_manager.h"
#include "swd_bridge.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "capture.h"
#include "debug_pins.h"
#include "ui_assets.h"        /* generated: 16x16 icons + 5x7 body font */

static const char *TAG = "menu";

/* Defined in main.c. Declared here rather than exporting a private header. */
extern const char *main_boot_reason(void);
extern bool        dap_usb_is_started(void);
extern uint32_t    dap_usb_configured_count(void);
extern uint32_t    dap_usb_get_rx_packets(void);
extern uint32_t    dap_usb_get_tx_packets(void);

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The panel is 64px tall and the full-size font cell is 16px, so the  */
/*  budget is exactly four 16px rows: y = 0, 16, 32, 48.               */
/*                                                                     */
/*  This was got wrong once by shrinking the row pitch to 14px to fit   */
/*  a fourth entry: 16 + 3*14 + status = 66px, i.e. 2px past the bottom */
/*  of the display, while the row's clear band was only 11px against a   */
/*  16px icon - so icons bled into the following row. Overlap here is   */
/*  always an arithmetic slip, never a styling choice. The layout below  */
/*  adds up to 64 exactly and every band is as tall as what it holds.    */
/* ------------------------------------------------------------------ */

#define ROW_TITLE_Y   4            /* compact header: small face, centred   */
#define TITLE_H       14
#define ROW_ITEM_Y    16           /* first item row                       */
#define ITEM_PITCH    16           /* one full row per entry               */
#define ITEM_ROWS     3            /* 3 x 16 = 48; 48 + 16 = 64 exactly    */
#define ROW_STATUS_Y  48           /* only used by the value editor        */
#define TEXT_COLS     16           /* 128 / 8 */

/* ------------------------------------------------------------------ */
/*  Menu table types                                                   */
/* ------------------------------------------------------------------ */

typedef struct menu_def menu_def_t;

/* Dispatch: called with the index of the item SW2 was pressed on.
 * Navigate by assigning to s_menu / s_screen. */
typedef void (*menu_dispatch_t)(int selected);

/* Optional value handler: when set, SW1/SW3 adjust the value instead of moving
 * the cursor. One editor for every adjustable setting, no per-page edit state. */
typedef void (*menu_adjust_t)(int dir);

struct menu_def {
    const char          *title;
    const char *const   *items;
    const uint8_t *const *icons;   /* parallel to items; NULL = text only */
    uint8_t              count;
    menu_dispatch_t      dispatch;
    menu_adjust_t        adjust;   /* NULL for a plain list */
};

/* What the panel is currently showing. */
typedef enum { SCREEN_LIST, SCREEN_INFO } screen_t;

/* ------------------------------------------------------------------ */
/*  State: ONE cursor, ONE menu pointer, ONE screen id. That is all.    */
/* ------------------------------------------------------------------ */

static const menu_def_t *s_menu;      /* current list                       */
static int               s_cursor;    /* index into s_menu->items           */
static screen_t          s_screen = SCREEN_LIST;
static int               s_page   = MENU_HOME;  /* reported to /api/status   */

/* ------------------------------------------------------------------ */
/*  RX line history                                                    */
/* ------------------------------------------------------------------ */

#define RX_LINE_MAX   60
#define RX_LINE_CAP   100
static char s_rx_lines[RX_LINE_CAP][RX_LINE_MAX + 1];
static int  s_rx_line_len[RX_LINE_CAP];
static volatile int s_rx_line_w = 0;
static volatile int s_rx_line_n = 0;
static volatile int s_rx_hist_max = 50;
static int  s_rx_cur_col = 0;
static bool s_rx_hex_mode = false;
static int  s_rx_scroll = 0;

#define RX_CONTENT_PX  108

/* SWD result, shown by the SWD screen. */
static uint32_t s_swd_idcode;
static bool     s_swd_idcode_valid;
static int      s_swd_last_err;

/* Value editor state. Every adjustable setting shares this one display slot:
 * the editor function writes the value it just applied, so the status row can
 * show "115200 bps" rather than the table index. */
static int s_ed_idx;
static int s_ed_step;
static int s_ed_show;
static const char *s_ed_unit;

static SemaphoreHandle_t s_kick;

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void ui_kick(void) { if (s_kick) xSemaphoreGive(s_kick); }

/* Text helpers. Every one of these clears the row it is about to draw into,
 * which is what stops a shorter new string from leaving the tail of the old
 * one on screen. */
static void row_clear(int y) { oled_fill_rect(0, y, OLED_WIDTH, 16, false); }

static void row_text(int y, const char *s, bool inverted)
{
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "%-*.*s", TEXT_COLS, TEXT_COLS, s);
    row_clear(y);
    if (inverted) {
        /* Selection band: fill the row, then draw the glyphs inverted so they
         * punch through it. */
        oled_fill_rect(0, y, OLED_WIDTH, 16, true);
        oled_text(0, y, line, true);
    } else {
        oled_text(0, y, line, false);
    }
}

static void row_kv(int y, const char *k, const char *v)
{
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "%-6.6s%.10s", k, v);
    row_text(y, line, false);
}

/* ---- Small (5x7) body text and icon rows ------------------------------
 *
 * This is the visual hierarchy the reference UIs rely on: the header uses the
 * full 8x16 face while the list body uses a small one. It also buys density -
 * a 5x7 glyph is 6px advance and 8px tall, so a row is ~11px instead of 16px,
 * which is how those menus fit 4-5 entries where a single font fits 3.
 */
static void small_str(int x, int y, const char *s, bool inverted)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < UI_FONT_FIRST || ch > UI_FONT_LAST) ch = '?';
        const uint8_t *g = ui_font5x7[ch - UI_FONT_FIRST];
        for (int r = 0; r < UI_FONT_H; r++) {
            for (int c = 0; c < UI_FONT_W; c++) {
                bool lit = (g[r] >> (7 - c)) & 1;
                oled_pixel(x + c, y + r, inverted ? !lit : lit);
            }
        }
        x += UI_FONT_W + 1;                 /* one column of letter spacing */
        if (x + UI_FONT_W > OLED_WIDTH) break;
    }
}

/* Small face at an integer scale, used for the compact header wordmark. */
static void small_str_scaled(int x, int y, const char *s, int scale)
{
    const int adv = (UI_FONT_W + 1) * scale;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < UI_FONT_FIRST || ch > UI_FONT_LAST) ch = '?';
        const uint8_t *g = ui_font5x7[ch - UI_FONT_FIRST];
        for (int r = 0; r < UI_FONT_H; r++) {
            for (int c = 0; c < UI_FONT_W; c++) {
                if (!((g[r] >> (7 - c)) & 1)) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        oled_pixel(x + c * scale + sx, y + r * scale + sy, true);
            }
        }
        x += adv;
    }
}

#define SMALL_LINE_H 16                     /* the row band, not the glyph */

/* One list row: selection marker, icon, then small text. There is
 * deliberately NO highlight band - the marker alone says which row is
 * selected, which keeps the list calm to look at and leaves the text at full
 * contrast whether or not it is the current row. */
#define MARK_X        1
#define LIST_ICON_X   7
#define LIST_TEXT_X   (LIST_ICON_X + 16 + 2)

static void list_row(int y, const uint8_t *icon, const char *label, bool selected)
{
    oled_fill_rect(0, y, OLED_WIDTH, SMALL_LINE_H, false);
    if (selected) {
        /* A small right-pointing triangle: the conventional "you are here"
         * marker, drawn instead of an inverted band. */
        oled_hline(MARK_X, MARK_X + 2, y + 8, true);
        oled_hline(MARK_X, MARK_X + 1, y + 7, true);
        oled_hline(MARK_X, MARK_X + 1, y + 9, true);
        oled_pixel(MARK_X, y + 8, true);
    }
    if (icon) oled_bitmap(LIST_ICON_X, y, 16, 16, icon, false);
    small_str(LIST_TEXT_X, y + 5, label, false);
}

static void title_row(const char *title)
{
    /* Compact centred wordmark in the SMALL face. The header is deliberately
     * smaller than the list rows rather than larger: an inverted or scaled-up
     * header is itself a form of highlighting, and the request was for a calm,
     * unhighlighted screen. A thin rule under it is enough to separate the two
     * regions without shouting. */
    oled_fill_rect(0, 0, OLED_WIDTH, TITLE_H, false);

    int w = (int)strlen(title) * (UI_FONT_W + 1) - 1;
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) x = 0;
    small_str_scaled(x, ROW_TITLE_Y, title, 1);

    oled_hline(0, ROW_TITLE_Y + UI_FONT_H + 2, OLED_WIDTH, true);
}

/* One list row: selection marker, icon, then small text. */
static void open_menu(const menu_def_t *m, int page)
{
    s_menu = m;
    s_cursor = 0;
    s_screen = SCREEN_LIST;
    s_page = page;
}

static void open_info(int page)
{
    s_screen = SCREEN_INFO;
    s_page = page;
    s_rx_scroll = 0;
}

void menu_clear_rx(void)
{
    s_rx_line_w = 0;
    s_rx_line_n = 0;
    s_rx_cur_col = 0;
    memset(s_rx_lines, 0, sizeof(s_rx_lines));
    memset(s_rx_line_len, 0, sizeof(s_rx_line_len));
}

void menu_set_rx_hist_max(int m)
{
    if (m < 2) m = 2;
    if (m > RX_LINE_CAP) m = RX_LINE_CAP;
    s_rx_hist_max = m;
    /* Clear with the resize, or s_rx_line_w can point outside the new window. */
    menu_clear_rx();
}

int menu_get_rx_hist_n(void)   { return s_rx_line_n; }
int menu_get_rx_hist_max(void) { return s_rx_hist_max; }

static int char_pixel_width(unsigned char c)
{
    return (c < 0x80) ? OLED_CHAR_W : OLED_CJK_W;
}

static void push_serial_data(char prefix, const uint8_t *data, uint32_t len)
{
    if (s_rx_cur_col > 0) {
        s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
        if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
        s_rx_cur_col = 0;
    }
    if (s_rx_line_n == 0) s_rx_line_n = 1;

    s_rx_lines[s_rx_line_w][0] = prefix;
    s_rx_lines[s_rx_line_w][1] = ' ';
    s_rx_cur_col = 2;
    s_rx_line_len[s_rx_line_w] = 2;
    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
    int display_w = 0;

    for (uint32_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
            if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
            s_rx_lines[s_rx_line_w][0] = '\0';
            s_rx_line_len[s_rx_line_w] = 0;
            s_rx_cur_col = 0;
            continue;
        }

        char hexbuf[5];
        char one[2] = { c, '\0' };
        const char *ins = one;
        if (s_rx_hex_mode) {
            snprintf(hexbuf, sizeof(hexbuf), "%02X ", (uint8_t)c);
            ins = hexbuf;
        }

        int w = 0;
        for (const char *p = ins; *p; p++) w += char_pixel_width((unsigned char)*p);
        if (w == 0) continue;

        if (display_w + w > RX_CONTENT_PX) {
            s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
            if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
            s_rx_lines[s_rx_line_w][0] = prefix;
            s_rx_lines[s_rx_line_w][1] = ' ';
            s_rx_cur_col = 2;
            s_rx_line_len[s_rx_line_w] = 2;
            display_w = 0;
        }
        int cap = RX_LINE_MAX - s_rx_cur_col;
        int used = 0;
        for (const char *p = ins; *p && used < cap; p++, used++)
            s_rx_lines[s_rx_line_w][s_rx_cur_col++] = *p;
        s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
        s_rx_line_len[s_rx_line_w] = s_rx_cur_col;
        display_w += w;
    }
    ui_kick();
}

void menu_push_rx_data(const uint8_t *data, uint32_t len) { push_serial_data('>', data, len); }
void menu_push_tx_data(const uint8_t *data, uint32_t len) { push_serial_data('<', data, len); }

/* ================================================================== */
/*  Actions                                                            */
/* ================================================================== */

static void act_reset_target(void)
{
    swd_bus_lock();
    swd_reset_target(true);
    vTaskDelay(pdMS_TO_TICKS(25));
    swd_reset_target(false);
    swd_bus_unlock();
    ESP_LOGI(TAG, "target reset pulse sent");
}

static void act_swd_idcode(void)
{
    swd_bus_lock();
    uint32_t id = 0;
    esp_err_t e = swd_read_idcode(&id);
    swd_bus_unlock();

    if (e == ESP_OK) {
        s_swd_idcode = id;
        s_swd_idcode_valid = true;
        s_swd_last_err = 0;
        ESP_LOGI(TAG, "SWD IDCODE = 0x%08lX", (unsigned long)id);
    } else {
        s_swd_idcode_valid = false;
        s_swd_last_err = (int)swd_get_last_ack();
        ESP_LOGW(TAG, "SWD IDCODE read failed (ack=%d)", s_swd_last_err);
    }
    open_info(MENU_SWD);          /* show the result instead of only flashing it */
    ui_kick();
}

static void usb_role_confirm(uint8_t mode, const char *label)
{
    pin_config_set_usb_mode(mode);
    oled_clear();
    row_text(ROW_ITEM_Y, "USB role ->", false);
    row_text(ROW_ITEM_Y + 16, label, false);
    row_text(ROW_STATUS_Y, "rebooting...", false);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/* ================================================================== */
/*  The menus. Each one is a table + a callback, nothing more.         */
/* ================================================================== */

void menu_on_sw1_press(void);
void menu_on_sw3_press(void);

/* ---- Monitor ---- */
static const char *const mon_items[] = {
    "Back", "RX Monitor", "I2C Bus", "SPI Bus", "PWM", "Capture Log", "Clear RX Hist",
};
static void mon_dispatch(int sel)
{
    switch (sel) {
    case 1: open_info(MENU_RX_MON);   break;
    case 2: open_info(MENU_I2C);      break;
    case 3: open_info(MENU_SPI);      break;
    case 4: open_info(MENU_PWM);      break;
    case 5: open_info(MENU_CAPTURE);  break;
    case 6: menu_clear_rx();          break;
    default: break;   /* 0 = Back, handled centrally in menu_on_sw2_press */
    }
}

/* ---- Probe ---- */
static const char *const probe_items[] = {
    "Back", "SWD / JTAG", "Read IDCODE", "Reset Target", "USB: off", "USB: probe", "USB: serial",
};
static void probe_dispatch(int sel)
{
    switch (sel) {
    case 1: open_info(MENU_SWD);                    break;
    case 2: act_swd_idcode();                       break;
    case 3: act_reset_target();                     break;
    case 4: usb_role_confirm(USB_MODE_OFF, "off");  break;
    case 5: usb_role_confirm(USB_MODE_DAP, "probe");break;
    case 6: usb_role_confirm(USB_MODE_TTL, "serial");break;
    default: break;
    }
}

/* ---- System: adjustable values live on the status row ---- */
static void ed_baud(int dir)
{
    static const int tbl[] = {9600, 115200, 460800, 921600};
    if (dir == 0) {   /* opening: seed from the live rate */
        uint32_t b = 0;
        uart_get_baudrate(UART1_PORT_NUM, &b);
        s_ed_idx = 1;
        for (int i = 0; i < 4; i++) if ((uint32_t)tbl[i] == b) s_ed_idx = i;
        s_ed_show = tbl[s_ed_idx];
        s_ed_step = 1; s_ed_unit = "bps";
        return;
    }
    s_ed_idx = (s_ed_idx + dir + 4) % 4;
    serial_bridge_set_baud((uint32_t)tbl[s_ed_idx]); s_ed_show = tbl[s_ed_idx];
}

static void ed_bright(int dir)
{
    static const int tbl[] = {10, 25, 50, 75, 100};
    if (dir == 0) { s_ed_idx = 4; s_ed_step = 1; s_ed_unit = "%"; s_ed_show = tbl[s_ed_idx]; return; }
    s_ed_idx = (s_ed_idx + dir + 5) % 5;
    oled_set_contrast((uint8_t)((tbl[s_ed_idx] * 255) / 100)); s_ed_show = tbl[s_ed_idx];
}

static void ed_buf(int dir)
{
    static const int tbl[] = {1024, 2048, 4096, 8192};
    if (dir == 0) { s_ed_idx = 1; s_ed_step = 1; s_ed_unit = "B"; s_ed_show = tbl[s_ed_idx]; return; }
    s_ed_idx = (s_ed_idx + dir + 4) % 4;
    serial_bridge_set_bufsize((size_t)tbl[s_ed_idx]); s_ed_show = tbl[s_ed_idx];
}

static void ed_shist(int dir)
{
    static const int tbl[] = {10, 30, 50, 100};
    if (dir == 0) { s_ed_idx = 2; s_ed_step = 1; s_ed_unit = "rec"; s_ed_show = tbl[s_ed_idx]; return; }
    s_ed_idx = (s_ed_idx + dir + 4) % 4;
    spi_mon_set_history_max(tbl[s_ed_idx]); s_ed_show = tbl[s_ed_idx];
}

static void ed_ihist(int dir)
{
    static const int tbl[] = {10, 30, 50, 100};
    if (dir == 0) { s_ed_idx = 2; s_ed_step = 1; s_ed_unit = "rec"; s_ed_show = tbl[s_ed_idx]; return; }
    s_ed_idx = (s_ed_idx + dir + 4) % 4;
    i2c_mon_set_history_max(tbl[s_ed_idx]); s_ed_show = tbl[s_ed_idx];
}

static void ed_rhist(int dir)
{
    static const int tbl[] = {10, 30, 50, 100};
    if (dir == 0) { s_ed_idx = 2; s_ed_step = 1; s_ed_unit = "ln"; s_ed_show = tbl[s_ed_idx]; return; }
    s_ed_idx = (s_ed_idx + dir + 4) % 4;
    menu_set_rx_hist_max(tbl[s_ed_idx]); s_ed_show = tbl[s_ed_idx];
}

static const char *const sys_items[] = {
    "Back", "UART Baud", "Brightness", "UART Buffer", "SPI Depth", "I2C Depth", "RX Lines", "WiFi: AP Mode",
};
static void sys_dispatch(int sel);

/* The System menu is a list whose first six entries are value editors. */
static void sys_edit(int dir)
{
    switch (s_cursor) {
    case 1: ed_baud(dir);   break;
    case 2: ed_bright(dir); break;
    case 3: ed_buf(dir);    break;
    case 4: ed_shist(dir);  break;
    case 5: ed_ihist(dir);  break;
    case 6: ed_rhist(dir);  break;
    default: break;
    }
}

static void sys_dispatch(int sel)
{
    if (sel == 7) {
        wifi_manager_start_ap();
        oled_clear();
        row_text(ROW_ITEM_Y, "WiFi AP mode", false);
        row_text(ROW_ITEM_Y + 16, "started", false);
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}

/* ---- Info ---- */
static const char *const info_items[] = {
    "Back", "Device Status", "Network", "Firmware", "USB Role", "AI / MCP",
};
static void info_dispatch(int sel)
{
    switch (sel) {
    case 1: open_info(MENU_STATUS);    break;
    case 2: open_info(MENU_NET);       break;
    case 3: open_info(MENU_FIRMWARE);  break;
    case 4: open_info(MENU_USB_STATE); break;
    case 5: open_info(MENU_AI);        break;
    default: break;
    }
}

/* ---- Root ---- */
static const char *const root_items[] = { "Monitor", "Probe", "System", "Info" };
static void root_dispatch(int sel);

/* Icon per entry; the editor and action rows get no icon so the value is the
 * only thing on the line. Order matches each items[] array exactly. */
static const uint8_t *const icons_mon[] = {
    ICON_BACK, ICON_SERIAL, ICON_BUS, ICON_BUS, ICON_WAVE, ICON_LIST, NULL,
};
static const uint8_t *const icons_prb[] = {
    ICON_BACK, ICON_PROBE, ICON_CHIP, ICON_LIGHTNING, ICON_USB, ICON_USB, ICON_USB,
};
static const uint8_t *const icons_sys[] = {
    ICON_BACK, NULL, NULL, NULL, NULL, NULL, NULL, ICON_NETWORK,
};
static const uint8_t *const icons_inf[] = {
    ICON_BACK, ICON_CHIP, ICON_NETWORK, ICON_CPU, ICON_USB, ICON_CPU,
};
static const uint8_t *const icons_root[] = {
    ICON_SERIAL, ICON_PROBE, ICON_FOLDER, ICON_LIST,
};

static const menu_def_t menu_root = { "NexLink", root_items, icons_root, 4, root_dispatch, NULL };
static const menu_def_t menu_mon  = { "Monitor", mon_items,  icons_mon,  7, mon_dispatch,  NULL };
static const menu_def_t menu_prb  = { "Probe",   probe_items,icons_prb,  7, probe_dispatch,NULL };
static const menu_def_t menu_sys  = { "System",  sys_items,  icons_sys,  8, sys_dispatch,  sys_edit };
static const menu_def_t menu_inf  = { "Info",    info_items, icons_inf,  6, info_dispatch, NULL };

/* The root entry that opened the current group. Selecting "Back" returns the
 * cursor there instead of resetting to the top, so leaving a group does not
 * lose the user's place in the list above it. */
static int s_parent_entry;

static void go_back(void)
{
    s_menu = &menu_root;
    s_cursor = s_parent_entry;
    s_screen = SCREEN_LIST;
    s_page = MENU_HOME;
}

static void root_dispatch(int sel)
{
    s_parent_entry = sel;              /* remember for the "Back" return */
    switch (sel) {
    case 0: open_menu(&menu_mon, MENU_LIST_MONITOR); break;
    case 1: open_menu(&menu_prb, MENU_LIST_PROBE);   break;
    case 2: open_menu(&menu_sys, MENU_LIST_SYSTEM);  break;
    case 3: open_menu(&menu_inf, MENU_LIST_INFO);    break;
    default: break;
    }
}

/* The monitor/probe/info "Back" entries are handled centrally before dispatch,
 * so nothing extra is needed here. */

/* ================================================================== */
/*  Info screens. Two detail rows + a status row, always.              */
/* ================================================================== */

static void scr_status(void)
{
    char v[24];
    title_row("Device Status");

    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v, sizeof(v), "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
    row_kv(ROW_ITEM_Y, "Up", v);

    snprintf(v, sizeof(v), "%lu/%lu", (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count());
    row_kv(ROW_ITEM_Y + 16, "RX/TX", v);

    snprintf(v, sizeof(v), "%lu bps tcp%u", (unsigned long)serial_bridge_get_baud(),
             (unsigned)tcp_server_client_count());
    row_text(ROW_STATUS_Y, v, false);
}

static void scr_net(void)
{
    char ip[16] = "-", v[24], ssid[24] = "-";
    int rssi = 0;
    wifi_state_t st = wifi_manager_get_state();
    wifi_manager_get_ip_str(ip, sizeof(ip));

    title_row("Network");
    row_kv(ROW_ITEM_Y, "State", st == WIFI_STATE_CONNECTED_STA ? "STA" :
                           st == WIFI_STATE_AP_MODE        ? "AP"  : "down");

    if (st == WIFI_STATE_CONNECTED_STA) {
        wifi_ap_record_t ar;
        if (esp_wifi_sta_get_ap_info(&ar) == ESP_OK) {
            ar.ssid[sizeof(ar.ssid) - 1] = '\0';
            snprintf(ssid, sizeof(ssid), "%.9s", (char *)ar.ssid);
            rssi = ar.rssi;
        }
    }
    snprintf(v, sizeof(v), "%s %ddBm", ip, rssi);
    row_kv(ROW_ITEM_Y + 16, "IP", v);
    row_text(ROW_STATUS_Y, ssid, false);
}

static void scr_firmware(void)
{
    char v[24];
    title_row("Firmware");

    const esp_partition_t *run = esp_ota_get_running_partition();
    row_kv(ROW_ITEM_Y, "Slot", run ? run->label : "?");

    const char *s = "ok";
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) s = "pending";
    snprintf(v, sizeof(v), "%s", s);
    row_kv(ROW_ITEM_Y + 16, "State", v);
    row_text(ROW_STATUS_Y, main_boot_reason(), false);
}

static void scr_usb(void)
{
    static const char *mode_s[3] = { "off", "probe", "serial" };
    char v[24], dbg[24];
    title_row("USB Role");

    uint8_t m = pin_config_usb_mode();
    snprintf(v, sizeof(v), "%s cfg%u", mode_s[m <= USB_MODE_TTL ? m : 0],
             (unsigned)dap_usb_configured_count());
    row_kv(ROW_ITEM_Y, "Role", v);

    snprintf(v, sizeof(v), "%lu/%lu", (unsigned long)dap_usb_get_rx_packets(),
             (unsigned long)dap_usb_get_tx_packets());
    row_kv(ROW_ITEM_Y + 16, "DAPio", v);

    debug_pins_report(dbg, sizeof(dbg));
    row_text(ROW_STATUS_Y, debug_pins_claimed() ? dbg : "no debug IO", false);
}

static void scr_swd(void)
{
    char v[24];
    title_row("SWD / JTAG");
    row_kv(ROW_ITEM_Y, "SWD", "12/13/14");
    row_kv(ROW_ITEM_Y + 16, "JTAG", "48/38/39");

    if (s_swd_idcode_valid) snprintf(v, sizeof(v), "ID 0x%08lX", (unsigned long)s_swd_idcode);
    else                    snprintf(v, sizeof(v), "ID fail ack%d", s_swd_last_err);
    row_text(ROW_STATUS_Y, v, false);
}

static void scr_pwm(void)
{
    char v[24];
    float f = 0.0f, d = 0.0f;
    title_row("PWM");

    pwm_mon_get(&f, &d);
    snprintf(v, sizeof(v), "%.1fHz", (double)f);
    row_kv(ROW_ITEM_Y, "In F", v);
    snprintf(v, sizeof(v), "%.1f%%", (double)d);
    row_kv(ROW_ITEM_Y + 16, "In D", v);

    if (pwm_out_running()) {
        pwm_out_get(&f, &d);
        snprintf(v, sizeof(v), "out %.0fHz %.0f%%", (double)f, (double)d);
    } else {
        snprintf(v, sizeof(v), "out stopped");
    }
    row_text(ROW_STATUS_Y, v, false);
}

static void scr_spi(void)
{
    char v[24], line[TEXT_COLS + 1];
    title_row("SPI Bus");

    snprintf(v, sizeof(v), "%lu", (unsigned long)spi_mon_get_count());
    row_kv(ROW_ITEM_Y, "Count", v);
    row_kv(ROW_ITEM_Y + 16, "State", spi_mon_running() ? "capturing" : "stopped");

    static spi_txn_t h[1];
    if (spi_mon_get_history(h, 1) == 1 && h[0].len > 0) {
        int p = 0;
        for (int i = 0; i < h[0].len && p < 12; i++)
            p += snprintf(line + p, sizeof(line) - p, "%02X", h[0].mosi[i]);
    } else {
        snprintf(line, sizeof(line), "no transactions");
    }
    row_text(ROW_STATUS_Y, line, false);
}

static void scr_i2c(void)
{
    char v[24], line[TEXT_COLS + 1];
    title_row("I2C Bus");

    snprintf(v, sizeof(v), "%lu", (unsigned long)i2c_mon_get_count());
    row_kv(ROW_ITEM_Y, "Count", v);
    row_kv(ROW_ITEM_Y + 16, "Mode", i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive");

    i2c_txn_t h[1];
    if (i2c_mon_get_history(h, 1) == 1) {
        snprintf(line, sizeof(line), "a%02X %s len%d", h[0].addr, h[0].read ? "R" : "W", h[0].len);
    } else {
        snprintf(line, sizeof(line), "no transactions");
    }
    row_text(ROW_STATUS_Y, line, false);
}

static void scr_capture(void)
{
    char v[24];
    title_row("Capture Log");

    snprintf(v, sizeof(v), "%u/%u", (unsigned)capture_count(), (unsigned)capture_capacity());
    row_kv(ROW_ITEM_Y, "Chunks", v);

    snprintf(v, sizeof(v), "%lu..%lu", (unsigned long)capture_oldest_seq(),
             (unsigned long)capture_next_seq());
    row_kv(ROW_ITEM_Y + 16, "Seq", v);

    snprintf(v, sizeof(v), "%s %uB", capture_dropped() ? "WRAPPED" : "ok",
             (unsigned)capture_bytes());
    row_text(ROW_STATUS_Y, v, false);
}

static void scr_ai(void)
{
    char ip[16] = "-";
    title_row("AI / MCP");

    wifi_state_t st = wifi_manager_get_state();
    bool net = (st == WIFI_STATE_CONNECTED_STA || st == WIFI_STATE_AP_MODE);
    wifi_manager_get_ip_str(ip, sizeof(ip));

    row_kv(ROW_ITEM_Y, "MCP", net ? "ready" : "no net");
    row_kv(ROW_ITEM_Y + 16, "Tools", "24");
    row_text(ROW_STATUS_Y, ip, false);
}

/* Live serial monitor: three scrolling rows map to the item rows; the status
 * row shows the mode and the scroll offset. */
static void scr_rx(void)
{
    title_row("RX Monitor");

    int n = s_rx_line_n;
    if (n > s_rx_hist_max) n = s_rx_hist_max;

    int start = n - ITEM_ROWS - s_rx_scroll;
    if (start < 0) start = 0;

    for (int row = 0; row < ITEM_ROWS; row++) {
        int idx = start + row;
        if (idx >= n - s_rx_scroll) { row_clear(ROW_ITEM_Y + row * ITEM_PITCH); continue; }
        int ring = ((s_rx_line_w - (n - 1) + idx) % s_rx_hist_max + s_rx_hist_max) % s_rx_hist_max;
        row_text(ROW_ITEM_Y + row * ITEM_PITCH, s_rx_lines[ring], false);
    }

    /* Status row: the mode, and how far back the user has scrolled. No key
     * names: the row is for information about the DATA, not about the buttons. */
    char hint[32];
    if (s_rx_scroll) snprintf(hint, sizeof(hint), "%s  -%d", s_rx_hex_mode ? "HEX" : "TXT", s_rx_scroll);
    else             snprintf(hint, sizeof(hint), "%s  %d ln", s_rx_hex_mode ? "HEX" : "TXT", n);
    row_text(ROW_STATUS_Y, hint, false);
}

/* ================================================================== */
/*  Rendering                                                          */
/* ================================================================== */

static void render_list(void)
{
    title_row(s_menu->title);

    /* Window that keeps the cursor visible. */
    int first = 0;
    if (s_cursor >= ITEM_ROWS) first = s_cursor - ITEM_ROWS + 1;

    for (int row = 0; row < ITEM_ROWS; row++) {
        int idx = first + row;
        int y = ROW_ITEM_Y + row * ITEM_PITCH;
        if (idx >= s_menu->count) { row_clear(y); continue; }
        /* Icon + small-font label. list_row() clears the band first, which is
         * what stops a longer previous label leaving its tail on screen. */
        const uint8_t *ic = s_menu->icons ? s_menu->icons[idx] : NULL;
        list_row(y, ic, s_menu->items[idx], idx == s_cursor);
    }

    /* No scroll indicator and no page counter: the request was to drop them and
     * let the list simply scroll. Instead the window keeps one row of context
     * above and below the cursor where the list allows it, so moving feels like
     * scrolling through a continuous list rather than paging between blocks. */

    /* Status row: ONLY the value editor. A plain list shows nothing here, which
     * is what removes the "1 / 7" style counter from every screen. */
    if (s_menu->adjust && s_cursor >= 1) {
        char hint[32];
        s_menu->adjust(0);              /* seed the editor from live state */
        snprintf(hint, sizeof(hint), "%d %s", s_ed_show, s_ed_unit);
        row_text(ROW_STATUS_Y, hint, false);
    } else {
        row_clear(ROW_STATUS_Y);
    }
}

static void render_info(void)
{
    switch (s_page) {
    case MENU_RX_MON:    scr_rx();       break;
    case MENU_STATUS:    scr_status();   break;
    case MENU_NET:       scr_net();      break;
    case MENU_FIRMWARE:  scr_firmware(); break;
    case MENU_USB_STATE: scr_usb();      break;
    case MENU_SWD:       scr_swd();      break;
    case MENU_PWM:       scr_pwm();      break;
    case MENU_SPI:       scr_spi();      break;
    case MENU_I2C:       scr_i2c();      break;
    case MENU_CAPTURE:   scr_capture();  break;
    case MENU_AI:        scr_ai();       break;
    default:             title_row("?"); row_clear(ROW_ITEM_Y); row_clear(ROW_ITEM_Y + 16); row_clear(ROW_STATUS_Y); break;
    }
}

void menu_render(void)
{
    /* Only the UI task reaches here: the panel has a single writer. */
    if (s_screen == SCREEN_INFO) render_info();
    else                         render_list();
    oled_flush();
}

/* ================================================================== */
/*  Input                                                              */
/* ================================================================== */

/* One body for both directions; the two key handlers are thin wrappers. */
static void move(int dir);

void menu_on_sw1_press(void) { move(-1); }
void menu_on_sw3_press(void) { move(+1); }

static void move(int dir)
{
    if (s_screen == SCREEN_INFO) {
        /* Only the RX monitor scrolls; elsewhere SW1/SW3 are inert rather than
         * silently meaning something else. */
        if (s_page == MENU_RX_MON) {
            int max = s_rx_line_n - ITEM_ROWS;
            if (max < 0) max = 0;
            s_rx_scroll += dir;
            if (s_rx_scroll > max) s_rx_scroll = max;
            if (s_rx_scroll < 0) s_rx_scroll = 0;
        }
        ui_kick();
        return;
    }

    /* Adjustable entry highlighted: SW1/SW3 change the value. */
    if (s_menu->adjust && s_cursor >= 1) {
        s_menu->adjust(dir);
        ui_kick();
        return;
    }

    s_cursor = ((s_cursor + dir) % s_menu->count + s_menu->count) % s_menu->count;
    ui_kick();
}

void menu_on_sw2_press(void)
{
    if (s_screen == SCREEN_INFO) {
        /* Back to the list it was opened from. s_page must follow, or
         * /api/status keeps reporting the screen we just left (the menu_*
         * table is the authority on which list is up, not s_page). */
        s_screen = SCREEN_LIST;
        if      (s_menu == &menu_mon) s_page = MENU_LIST_MONITOR;
        else if (s_menu == &menu_prb) s_page = MENU_LIST_PROBE;
        else if (s_menu == &menu_sys) s_page = MENU_LIST_SYSTEM;
        else if (s_menu == &menu_inf) s_page = MENU_LIST_INFO;
        else                          s_page = MENU_HOME;
        ui_kick();
        return;
    }

    const int sel = s_cursor;

    /* "Back" is item 0 of every nested list - an ordinary selection, which is
     * why no navigation stack is needed. The root list has no Back entry. */
    if (s_menu != &menu_root && sel == 0) {
        go_back();
        ui_kick();
        return;
    }

    if (s_menu->dispatch) s_menu->dispatch(sel);
    ui_kick();
}

void menu_on_sw2_long_press(void)
{
    go_back();                        /* universal escape, from anywhere */
    ui_kick();
}

void menu_simulate_button(int btn_id, const char *action)
{
    bool is_long = action && (strcmp(action, "long") == 0 ||
                              strcmp(action, "hold") == 0);
    switch (btn_id) {
    case 1: menu_on_sw1_press(); break;      /* SW1 = up     */
    case 3: menu_on_sw3_press(); break;      /* SW3 = down   */
    case 2:                                  /* SW2 = select */
        if (is_long) menu_on_sw2_long_press();
        else         menu_on_sw2_press();
        break;
    default: break;
    }
}

/* ================================================================== */
/*  Introspection                                                      */
/* ================================================================== */

int menu_get_current_page(void)
{
    /* A single source of truth: the list table that is up, or the screen id
     * while an info screen is displayed. s_page is kept in step with both, and
     * this fallback keeps the reported id honest if they ever drift. */
    if (s_screen == SCREEN_INFO) return s_page;
    if (s_menu == &menu_mon) return MENU_LIST_MONITOR;
    if (s_menu == &menu_prb) return MENU_LIST_PROBE;
    if (s_menu == &menu_sys) return MENU_LIST_SYSTEM;
    if (s_menu == &menu_inf) return MENU_LIST_INFO;
    return MENU_HOME;
}

int menu_get_selected(void) { return s_cursor; }

void menu_get_state(char *buf, unsigned len)
{
    if (!buf || !len) return;
    const char *view = (s_screen == SCREEN_INFO) ? "INFO" : "LIST";
    snprintf(buf, len, "%s %s sel=%d", view, s_menu ? s_menu->title : "?", s_cursor);
}

/* ================================================================== */
/*  Init and task plumbing                                             */
/* ================================================================== */

void menu_init(void)
{
    s_menu = &menu_root;
    s_cursor = 0;
    s_screen = SCREEN_LIST;
    s_page = MENU_HOME;
    s_rx_scroll = 0;

    menu_clear_rx();
    if (!s_kick) s_kick = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "menu ready: table-driven, SW1/SW3=up/down, SW2=select");
}

void menu_ui_wait(int ms)
{
    if (!s_kick) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    xSemaphoreTake(s_kick, pdMS_TO_TICKS(ms));
}
