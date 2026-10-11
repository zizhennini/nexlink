/*
 * dash_ui.c - dashboard for the 128x64 SSD1306 OLED.
 *
 * Six screens. Button model, identical on every screen:
 *
 *   SW1 short  previous screen
 *   SW3 short  next screen
 *   SW2 short  that screen's action
 *   SW2 long   back to the first screen
 *
 * Layout (128x64):
 *   y 0..15  title row   - professional screen name (left), page n/6 (right)
 *   y16..31  data row A
 *   y32..47  data row B
 *   y48..63  ACTION BAR   - solid black button naming exactly what SW2 does,
 *                           so the current entry is always obvious (the key
 *                           name "SW2" is deliberately NOT printed).
 *
 * A one-time boot animation ("NexLink" 2x logo + progress bar) is played by
 * dash_boot_animation() from the UI task, before the first dashboard paint.
 *
 * Screens (titles use the professional names):
 *   1 STATUS  WiFi state + IP                 (action: reconnect WiFi)
 *   2 UART    two live serial lines           (action: toggle HEX/text)
 *   3 SWD     SWCLK/SWDIO pins + IDCODE       (action: read IDCODE)
 *   4 TARGET  NRST pin + IDCODE               (action: reset target)
 *   5 PWM     measured freq / duty            (action: output/stop 1 kHz)
 *   6 USB     current role DAP/TTL/OFF        (action: cycle role)
 *
 * Text mixes 8x16 ASCII with a small embedded 16x16 CJK bitmap font
 * (cjk_font.h, only the chars this UI uses, ~2 KB).
 */
#include "dash_ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled_ssd1306.h"
#include "font8x16.h"
#include "cjk_font.h"
#include "pinout.h"
#include "pin_config.h"
#include "serial_bridge.h"
#include "usb_ttl.h"
#include "wifi_manager.h"
#include "swd_bridge.h"
#include "pwm_mon.h"

static const char *TAG = "dash";

/* ---- geometry ---- */
#define ROW_TITLE   0
#define ROW_A       16
#define ROW_B       32
#define ROW_ACTION  48
#define COLS        16

typedef enum {
    SCR_OVERVIEW = 0,
    SCR_SERIAL,
    SCR_SWD,
    SCR_TARGET,
    SCR_PWM,
    SCR_USB,
    SCR_COUNT
} screen_t;

static screen_t s_screen = SCR_OVERVIEW;

/* ---- UART live ring ---- */
#define RX_LINES   2
#define RX_LINE_SZ 17
static char s_rx[RX_LINES][RX_LINE_SZ];
static int  s_rx_head, s_rx_used, s_rx_col;
static volatile bool s_rx_hex;
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- command ring ---- */
typedef enum { CMD_NONE=0, CMD_NEXT, CMD_PREV, CMD_HOME, CMD_ACTION } dash_cmd_t;
#define CMD_RING 8
static volatile dash_cmd_t s_cmd[CMD_RING];
static volatile int s_cmd_head, s_cmd_tail;
static portMUX_TYPE s_cmd_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- toast on last row (temporarily replaces the action bar) ---- */
static char     s_toast[20];
static int64_t  s_toast_until_us;

/* ---- cached SWD result ---- */
static uint32_t s_idcode;
static bool     s_idcode_ok;

static SemaphoreHandle_t s_kick;
static void kick(void) { if (s_kick) xSemaphoreGive(s_kick); }

/* ================================================================== */
/*  Mixed ASCII / CJK text                                            */
/* ================================================================== */
static const cjk_glyph_t *cjk_find(uint16_t cp)
{
    for (size_t i = 0; i < CJK_GLYPH_COUNT; i++)
        if (cjk_table[i].cp == cp) return &cjk_table[i];
    return NULL;
}

/* Decode one UTF-8 code point from *pp, advancing the pointer. */
static uint32_t utf8_next(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint32_t cp; int n;
    if (*p < 0x80) { cp = *p; n = 1; }
    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
    else { cp = 0xFFFD; n = 1; }
    bool bad = false;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) == 0x80) cp = (cp << 6) | (p[i] & 0x3F);
        else bad = true;
    }
    *pp = p + n;
    return bad ? 0xFFFD : cp;
}

/* Pixel width of a mixed string: ASCII cells 8px, CJK cells 16px. */
static int mix_width(const char *s)
{
    int x = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        x += (cp < 0x80) ? 8 : 16;
    }
    return x;
}

/* Draw a UTF-8 string at x; ASCII cells 8px, CJK 16px. Returns ending x.
 * Glyphs past the right edge are clipped; a missing CJK glyph still keeps
 * its 16px cell so following text never shifts. inv = inverse video. */
static int draw_mix_inv(int x, int y, const char *s, bool inv)
{
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp < 0x80) {
            if (x + 8 <= OLED_WIDTH) oled_char(x, y, (char)cp, inv);
            x += 8;
        } else {
            const cjk_glyph_t *g = cjk_find((uint16_t)cp);
            if (g && x + 16 <= OLED_WIDTH)
                oled_bitmap(x, y, 16, 16, g->d, inv);
            x += 16;
        }
    }
    return x;
}

static int draw_mix(int x, int y, const char *s)
{
    return draw_mix_inv(x, y, s, false);
}

/* Title row: screen name on the left, "n/6" on the right, thin rule under. */
static void title_row(const char *name, int idx)
{
    oled_fill_rect(0, 0, OLED_WIDTH, 16, false);
    oled_text(0, 0, name, false);
    char p[8];
    snprintf(p, sizeof(p), "%d/%d", idx, (int)SCR_COUNT);
    oled_text(OLED_WIDTH - (int)strlen(p) * 8, 0, p, false);
    oled_hline(0, 15, OLED_WIDTH, true);
}

/* One full-width content row; always cleared first so nothing can smear. */
static void content_row(int y, const char *s)
{
    oled_fill_rect(0, y, OLED_WIDTH, 16, false);
    draw_mix(0, y, s);
}

/* The action bar: a solid highlighted "button" on the last row whose caption
 * says exactly what SW2 does on this screen. Inverse video = lit bar with
 * black glyphs, the standard selection highlight. */
static void action_bar(const char *text)
{
    int w = mix_width(text);
    int bw = w + 10;
    if (bw > OLED_WIDTH) bw = OLED_WIDTH;
    oled_fill_rect(0, ROW_ACTION, OLED_WIDTH, 16, false);  /* clear row    */
    oled_fill_rect(0, ROW_ACTION, bw, 16, true);           /* lit bar      */
    draw_mix_inv(5, ROW_ACTION, text, true);               /* black glyphs */
}

/* Signal-strength bars for the WiFi row. */
static void sig_bars(int x, int rssi)
{
    int n = rssi >= -55 ? 5 : rssi >= -62 ? 4 : rssi >= -70 ? 3 :
            rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 5; i++) {
        int h = 3 + i * 2;
        if (i < n) oled_fill_rect(x + i * 4, ROW_A + 14 - h, 2, h, true);
    }
}

/* Professional USB role name. */
static const char *usb_role_name(uint8_t m)
{
    if (m == USB_MODE_DAP) return "DAP";
    if (m == USB_MODE_TTL) return "TTL";
    return "OFF";
}

/* IDCODE row shared by the SWD and TARGET screens. */
static void idcode_row(int y)
{
    char line[20];
    if (s_idcode_ok) snprintf(line, sizeof(line), "ID 0x%08lX",
                              (unsigned long)s_idcode);
    else             snprintf(line, sizeof(line), "ID 无目标");
    content_row(y, line);
}

/* ================================================================== */
/*  SCREEN 1 - STATUS                                                 */
/* ================================================================== */
static void scr_overview(void)
{
    title_row("STATUS", 1);

    wifi_state_t st = wifi_manager_get_state();
    if (st == WIFI_STATE_CONNECTED_STA) {
        content_row(ROW_A, "WiFi 已连接");
        wifi_ap_record_t ar;
        if (esp_wifi_sta_get_ap_info(&ar) == ESP_OK) sig_bars(104, ar.rssi);
    } else if (st == WIFI_STATE_AP_MODE) {
        content_row(ROW_A, "WiFi 热点模式");
    } else if (st == WIFI_STATE_CONNECTING) {
        content_row(ROW_A, "WiFi 连接中");
    } else {
        content_row(ROW_A, "WiFi 未连接");
    }

    char ip[16] = "-";
    wifi_manager_get_ip_str(ip, sizeof(ip));
    char line[20];
    snprintf(line, sizeof(line), "IP %s", ip);
    content_row(ROW_B, line);

    action_bar("> 重连 WiFi");
}

/* ================================================================== */
/*  SCREEN 2 - UART                                                   */
/* ================================================================== */
static void scr_serial(void)
{
    title_row("UART", 2);

    portENTER_CRITICAL(&s_rx_mux);
    int n = s_rx_used < RX_LINES ? s_rx_used : RX_LINES;
    char snap[RX_LINES][RX_LINE_SZ];
    for (int i = 0; i < RX_LINES; i++) {
        int idx = (s_rx_head - n + 1 + i + RX_LINES * 2) % RX_LINES;
        if (n > i) memcpy(snap[i], s_rx[idx], RX_LINE_SZ);
        else       snap[i][0] = '\0';
    }
    portEXIT_CRITICAL(&s_rx_mux);

    static const int ys[RX_LINES] = { ROW_A, ROW_B };
    for (int i = 0; i < RX_LINES; i++) {
        if (n == 0 && i == 0) content_row(ys[i], "等待数据...");
        else                 content_row(ys[i], snap[i]);
    }

    action_bar(s_rx_hex ? "> 切换为文本" : "> 切换为 HEX");
}

/* ================================================================== */
/*  SCREEN 3 - SWD                                                    */
/* ================================================================== */
static void scr_swd(void)
{
    char line[22];
    title_row("SWD", 3);

    snprintf(line, sizeof(line), "SWD CLK%d DIO%d",
             pin_config_swclk(), pin_config_swdio());
    content_row(ROW_A, line);
    idcode_row(ROW_B);

    action_bar("> 读取 IDCODE");
}

/* ================================================================== */
/*  SCREEN 4 - TARGET                                                 */
/* ================================================================== */
static void scr_target(void)
{
    char line[20];
    title_row("TARGET", 4);

    snprintf(line, sizeof(line), "NRST IO%d", pin_config_nrst());
    content_row(ROW_A, line);
    idcode_row(ROW_B);

    action_bar("> 复位目标");
}

/* ================================================================== */
/*  SCREEN 5 - PWM                                                    */
/* ================================================================== */
static void scr_pwm(void)
{
    char line[20];
    title_row("PWM", 5);

    float f, d;
    pwm_mon_get(&f, &d);
    if (f <= 0) {
        content_row(ROW_A, "频率 --");
    } else if (f >= 1000) {
        snprintf(line, sizeof(line), "频率 %.2f kHz", f / 1000.0);
        content_row(ROW_A, line);
    } else {
        snprintf(line, sizeof(line), "频率 %.0f Hz", f);
        content_row(ROW_A, line);
    }

    snprintf(line, sizeof(line), "占空 %.1f%%", d);
    content_row(ROW_B, line);

    action_bar(pwm_out_running() ? "> 停止输出" : "> 输出 1kHz");
}

/* ================================================================== */
/*  SCREEN 6 - USB                                                    */
/* ================================================================== */
static void scr_usb(void)
{
    char line[20];
    title_row("USB", 6);

    uint8_t m = pin_config_usb_mode();
    uint8_t next = (uint8_t)((m + 1) % 3);

    snprintf(line, sizeof(line), "角色 %s", usb_role_name(m));
    content_row(ROW_A, line);
    content_row(ROW_B, "切换后重启");

    snprintf(line, sizeof(line), "> 切换为 %s", usb_role_name(next));
    action_bar(line);
}

/* ================================================================== */
/*  Boot animation                                                    */
/* ================================================================== */

/* One ASCII glyph scaled 2x (16 wide x 32 tall) straight from font_8x16. */
static void draw_big(int x, int y, char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = font_8x16[c - 0x20];
    for (int r = 0; r < 16; r++) {
        uint8_t row = g[r];
        for (int col = 0; col < 8; col++)
            if (row & (0x80 >> col))
                oled_fill_rect(x + col * 2, y + r * 2, 2, 2, true);
    }
}

/* "NexLink" types itself in 2x, then a progress bar fills. ~1.4 s total.
 * Runs once on the UI task; networking/USB keep booting in parallel. */
void dash_boot_animation(void)
{
    if (!oled_is_ready()) return;

    const char *logo = "NexLink";
    int n = (int)strlen(logo);
    const int gw = 16, gy = 12;
    int gx = (OLED_WIDTH - n * gw) / 2;

    oled_clear();
    oled_flush();

    for (int k = 1; k <= n; k++) {
        oled_clear();
        for (int i = 0; i < k; i++) draw_big(gx + i * gw, gy, logo[i]);
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(90));
    }

    const int px = 8, py = 50, pw = 112, ph = 6;
    oled_rect(px, py, pw, ph, true);
    oled_flush();
    for (int w = 0; w <= pw - 2; w += 2) {
        oled_fill_rect(px + 1, py + 1, w, ph - 2, true);
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    vTaskDelay(pdMS_TO_TICKS(350));

    oled_clear();
    oled_flush();
}

/* ================================================================== */
/*  Render                                                            */
/* ================================================================== */
void dash_render(void)
{
    switch (s_screen) {
    case SCR_OVERVIEW: scr_overview(); break;
    case SCR_SERIAL:   scr_serial();   break;
    case SCR_SWD:      scr_swd();      break;
    case SCR_TARGET:   scr_target();   break;
    case SCR_PWM:      scr_pwm();      break;
    case SCR_USB:      scr_usb();      break;
    default: break;
    }

    /* A toast temporarily replaces the action-bar button. */
    if (s_toast_until_us && esp_timer_get_time() < s_toast_until_us) {
        oled_fill_rect(0, ROW_ACTION, OLED_WIDTH, 16, false);
        draw_mix(0, ROW_ACTION, s_toast);
    } else {
        s_toast_until_us = 0;
    }
    oled_flush();
}

/* ================================================================== */
/*  Input                                                             */
/* ================================================================== */
static void post(dash_cmd_t c);
void dash_next_screen(void) { post(CMD_NEXT); }
void dash_prev_screen(void) { post(CMD_PREV); }
void dash_home(void)        { post(CMD_HOME); }
void dash_action(void)      { post(CMD_ACTION); }

static void toast(const char *msg, int hold_ms)
{
    snprintf(s_toast, sizeof(s_toast), "%s", msg);
    s_toast_until_us = esp_timer_get_time() + (int64_t)hold_ms * 1000;
}

static void post(dash_cmd_t c)
{
    portENTER_CRITICAL(&s_cmd_mux);
    int next = (s_cmd_tail + 1) % CMD_RING;
    if (next != s_cmd_head) { s_cmd[s_cmd_tail] = c; s_cmd_tail = next; }
    portEXIT_CRITICAL(&s_cmd_mux);
    kick();
}

static dash_cmd_t take_cmd(void)
{
    dash_cmd_t c = CMD_NONE;
    portENTER_CRITICAL(&s_cmd_mux);
    if (s_cmd_head != s_cmd_tail) {
        c = s_cmd[s_cmd_head]; s_cmd_head = (s_cmd_head + 1) % CMD_RING;
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    return c;
}

static void read_idcode(void)
{
    char t[20];
    swd_bus_lock();
    esp_err_t e = swd_read_idcode(&s_idcode);
    s_idcode_ok = (e == ESP_OK);
    swd_bus_unlock();
    if (s_idcode_ok) snprintf(t, sizeof(t), "ID 0x%08lX", (unsigned long)s_idcode);
    else             snprintf(t, sizeof(t), "无目标");
    toast(t, 1200);
}

static void do_action(void)
{
    switch (s_screen) {
    case SCR_OVERVIEW:
        wifi_manager_connect_sta();
        toast("无线重连中", 800);
        break;

    case SCR_SERIAL:
        s_rx_hex = !s_rx_hex;
        dash_clear_rx();
        toast(s_rx_hex ? "HEX模式" : "文本模式", 700);
        break;

    case SCR_SWD:
        read_idcode();
        break;

    case SCR_TARGET:
        if (usb_ttl_is_started()) {
            usb_ttl_reset_pulse(25, false);
        } else {
            swd_reset_target(true);
            vTaskDelay(pdMS_TO_TICKS(25));
            swd_reset_target(false);
        }
        toast("目标已复位", 900);
        break;

    case SCR_PWM:
        if (pwm_out_running()) {
            pwm_out_stop();
            toast("已停止", 800);
        } else {
            int gpio = pin_config_pwm();
            if (gpio < 0) { toast("无PWM", 800); break; }
            pwm_out_start(gpio, 1000, 50);
            toast("开始输出", 800);
        }
        break;

    case SCR_USB: {
        uint8_t want = (pin_config_usb_mode() + 1) % 3;
        bool busy = false;
        extern bool dap_usb_is_started(void);
        if (dap_usb_is_started() || usb_ttl_is_started()) busy = true;
        pin_config_set_usb_mode(want);
        if (busy) {
            toast("切换中", 600);
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();
        } else {
            if (want == USB_MODE_DAP) {
                extern esp_err_t dap_usb_start(void);
                dap_usb_start();
            } else if (want == USB_MODE_TTL) {
                usb_ttl_start();
            }
            toast("已切换", 800);
        }
        break;
    }

    default: break;
    }
}

static void handle(void)
{
    for (;;) {
        dash_cmd_t c = take_cmd();
        switch (c) {
        case CMD_NEXT: s_screen = (screen_t)((s_screen + 1) % SCR_COUNT); break;
        case CMD_PREV: s_screen = (screen_t)((s_screen + SCR_COUNT - 1) % SCR_COUNT); break;
        case CMD_HOME: s_screen = SCR_OVERVIEW; break;
        case CMD_ACTION: do_action(); break;
        default: return;
        }
    }
}

void dash_simulate_button(int btn_id, const char *action)
{
    bool is_long = action && (strcmp(action, "long") == 0 || strcmp(action, "hold") == 0);
    switch (btn_id) {
    case 1: post(CMD_PREV); break;
    case 3: post(CMD_NEXT); break;
    case 2: post(is_long ? CMD_HOME : CMD_ACTION); break;
    default: break;
    }
}

/* ================================================================== */
/*  UART live ring                                                    */
/* ================================================================== */
static void rx_newline(void)
{
    s_rx_head = (s_rx_head + 1) % RX_LINES;
    s_rx[s_rx_head][0] = '\0';
    s_rx_col = 0;
    if (s_rx_used < RX_LINES) s_rx_used++;
}

static void rx_append_byte(uint8_t b)
{
    if (s_rx_hex) {
        static const char h[] = "0123456789ABCDEF";
        char trip[3] = { h[b >> 4], h[b & 0xF], ' ' };
        for (int j = 0; j < 3; j++) {
            if (s_rx_col >= COLS) rx_newline();
            s_rx[s_rx_head][s_rx_col++] = trip[j];
            s_rx[s_rx_head][s_rx_col] = '\0';
        }
    } else {
        char c = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        if (s_rx_col >= COLS) rx_newline();
        s_rx[s_rx_head][s_rx_col++] = c;
        s_rx[s_rx_head][s_rx_col] = '\0';
    }
}

void dash_push_rx(const uint8_t *data, uint32_t len)
{
    if (!data || !len) return;
    portENTER_CRITICAL(&s_rx_mux);
    for (uint32_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n') { rx_newline(); continue; }
        if (c == '\r') continue;
        rx_append_byte((uint8_t)c);
    }
    if (s_rx_used == 0) s_rx_used = 1;
    portEXIT_CRITICAL(&s_rx_mux);
    kick();
}

void dash_push_tx(const uint8_t *data, uint32_t len)
{
    dash_push_rx(data, len);
}

void dash_clear_rx(void)
{
    portENTER_CRITICAL(&s_rx_mux);
    s_rx_head = 0; s_rx_used = 0; s_rx_col = 0;
    memset(s_rx, 0, sizeof(s_rx));
    portEXIT_CRITICAL(&s_rx_mux);
    kick();
}

/* ================================================================== */
/*  Init / plumbing                                                   */
/* ================================================================== */
void dash_init(void)
{
    s_screen = SCR_OVERVIEW;
    s_rx_hex = false;
    s_idcode_ok = false; s_idcode = 0;
    s_toast[0] = '\0'; s_toast_until_us = 0;
    s_cmd_head = 0; s_cmd_tail = 0;
    dash_clear_rx();
    if (!s_kick) s_kick = xSemaphoreCreateBinary();
    ESP_LOGI(TAG, "dashboard ready: %d screens", (int)SCR_COUNT);
}

void dash_wait(int ms)
{
    if (s_kick) xSemaphoreTake(s_kick, 0);
    handle();
    if (!s_kick) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    xSemaphoreTake(s_kick, pdMS_TO_TICKS(ms));
    handle();
}

int dash_get_screen(void) { return (int)s_screen; }
int dash_get_screen_count(void) { return (int)SCR_COUNT; }

void dash_get_state(char *buf, unsigned len)
{
    static const char *names[SCR_COUNT] = {
        "OVERVIEW","UART","SWD","TARGET","PWM","USB"
    };
    if (!buf || !len) return;
    int s = (int)s_screen;
    snprintf(buf, len, "%s %d/%d",
             (s >= 0 && s < SCR_COUNT) ? names[s] : "?",
             s + 1, (int)SCR_COUNT);
}
