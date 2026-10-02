/*
 * menu_ui.c - 128x64 monochrome OLED menu UI (replaces the ST7735S build)
 *
 * Layout: one 16px title row at the top, a separator, then three 16px content
 * rows. ASCII is 8px wide (16 columns), CJK 16px wide (8 columns).
 *
 * Navigation (panel: SW1=left, SW2=middle, SW3=right):
 *   HOME      SW1/SW3 move the list cursor, SW2 enters the page.
 *   subpage   SW1 = previous page, SW3 = next page (the eight pages form a
 *             ring; every swap is animated as a vertical slide), SW2 = the
 *             page's context action, SW2 held ~600 ms = back to HOME.
 *   CFG       SW1/SW3 are the item cursor / value editor here; SW2 executes
 *             an action item or confirms an edit. The title reads "Edit x"
 *             while editing and the editor auto-closes after 5 s idle.
 * SW2 long is the ONE universal escape: from every page and every mode it
 * returns HOME - it never gets swallowed by a sub-mode.
 * Clicks fire the instant the button is released - no double-click window,
 * so navigation never feels delayed. Position is never guesswork either:
 * every sub-page header carries its ring index "n/8".
 */
#include "menu_ui.h"
#include "oled_ssd1306.h"
#include "serial_bridge.h"
#include "wifi_manager.h"
#include "swd_bridge.h"
#include "pinout.h"
#include "pin_config.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "dap_usb.h"
#include "usb_ttl.h"
#include "esp_system.h"

#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

static const char *TAG = "menu";

/* ---- Layout geometry ---- */
#define ROW_TITLE_Y   0
#define ROW1_Y        16
#define ROW2_Y        32
#define ROW3_Y        48

/* ---- RX line history ---- */
#define RX_LINE_MAX   60
#define RX_LINE_CAP   100
static char  s_rx_lines[RX_LINE_CAP][RX_LINE_MAX + 1];
static int   s_rx_line_len[RX_LINE_CAP];
static volatile int s_rx_line_w = 0;
static volatile int s_rx_line_n = 0;
static volatile int s_rx_hist_max = 50;
static int   s_rx_cur_col = 0;
static int   s_rx_view_offset = 0;
static bool  s_rx_hex_mode = false;

/* Content width: 128px minus the 2px margin and the 16px "> " prefix. */
#define RX_CONTENT_PX  108

/* The eight sub-pages form a vertical ring. The HOME view is a list of the
 * same ring; s_selected is the shared cursor, so the carousel and the list
 * always agree and "back to home" highlights the page you came from. */
#define NAV_N 8
static const menu_page_t s_nav_ring[NAV_N] = {
    MENU_RX_MON, MENU_I2C, MENU_SPI, MENU_PWM,
    MENU_SWD, MENU_STATUS, MENU_CONFIG, MENU_AI,
};
static const char *const s_nav_labels[NAV_N] = {
    "RX Monitor", "I2C Bus", "SPI Bus", "PWM",
    "SWD / DAP", "Status", "Config", "AI / MCP",
};

static menu_page_t s_current_page = MENU_HOME;
static int s_selected = 0;                 /* ring cursor, 0..NAV_N-1 */

/* Page-swap animation. The button task only records the previous screen and
 * the intent here; the UI task performs every oled flush (the panel sits on a
 * shared I2C bus, so only ONE task may ever talk to it). */
#define ANIM_FRAMES 5                      /* ~5 x 26ms = 130ms slide */
static uint8_t           s_anim_old[OLED_FB_BYTES];
static volatile bool     s_anim_pending;
static volatile int      s_anim_dir;
static SemaphoreHandle_t s_kick;           /* wakes the UI task instantly */
static SemaphoreHandle_t s_nav_lock;       /* guards page state during swaps */

/* ---- SWD page state ---- */
static uint32_t s_swd_idcode = 0;
static bool     s_swd_idcode_valid = false;
static int      s_swd_last_err = 0;

void menu_init(void)
{
    /* Boot on the page list: the first thing you see tells you exactly where
     * you are and what the three buttons do. */
    s_current_page = MENU_HOME;
    s_selected = 0;
    s_rx_line_w = 0;
    s_rx_line_n = 0;
    s_rx_cur_col = 0;
    s_rx_view_offset = 0;
    s_rx_hex_mode = false;
    s_anim_pending = false;
    memset(s_rx_lines, 0, sizeof(s_rx_lines));
    memset(s_rx_line_len, 0, sizeof(s_rx_line_len));
    if (!s_kick) s_kick = xSemaphoreCreateBinary();
    if (!s_nav_lock) s_nav_lock = xSemaphoreCreateMutex();
}

static void rx_new_line(void)
{
    s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
    if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
    s_rx_lines[s_rx_line_w][0] = '\0';
    s_rx_line_len[s_rx_line_w] = 0;
    s_rx_cur_col = 0;
}

static bool s_spi_hex_mode = true;
static bool s_i2c_hex_mode = true;

/* ---- CFG settings list ---- */
/* Items: 0=Baud 1=Bright 2=SBuf 3=SHist 4=IHist 5=RHist 6=NetRst 7=Clear */
typedef enum { CFG_LIST, CFG_EDIT } cfg_state_t;
static cfg_state_t s_cfg_state = CFG_LIST;
static int  s_cfg_sel = 0;
static int  s_cfg_baud_idx   = 1;
static int  s_cfg_bright_idx = 4;
static int  s_cfg_buf_idx    = 1;
static int  s_cfg_shist_idx  = 2;
static int  s_cfg_ihist_idx  = 2;
static int  s_cfg_rhist_idx  = 2;
static const uint32_t cfg_bauds[]  = {9600, 115200, 460800, 921600};
static const int cfg_bright[]      = {10, 25, 50, 75, 100};
static const int cfg_bufs[]        = {1024, 2048, 4096, 8192};
static const int cfg_hist[]        = {10, 30, 50, 100};

/* Edit-mode idle exit (Marlin-style timeout): the editor returns to the
 * list by itself after this many seconds without a key, so a mode the user
 * forgot about can never swallow the escape gesture. */
#define CFG_EDIT_TIMEOUT_S 5
static volatile uint32_t s_cfg_edit_ts;   /* uptime seconds of last edit activity */

static uint32_t cfg_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/* True when Config is in EDIT mode. Auto-closes an editor that has been
 * idle past the timeout (Marlin behaviour). Called from both tasks; the
 * volatile 32-bit stores here are single-copy, so a torn read cannot turn
 * into a wrong state - worst case one frame shows the stale mode. */
static bool cfg_edit_active(void)
{
    if (s_current_page != MENU_CONFIG || s_cfg_state != CFG_EDIT)
        return false;
    if (cfg_now_s() - s_cfg_edit_ts >= CFG_EDIT_TIMEOUT_S) {
        s_cfg_state = CFG_LIST;
        return false;
    }
    return true;
}
#define CFG_N_BAUD   ((int)(sizeof(cfg_bauds)/sizeof(cfg_bauds[0])))
#define CFG_N_BRIGHT ((int)(sizeof(cfg_bright)/sizeof(cfg_bright[0])))
#define CFG_N_BUF    ((int)(sizeof(cfg_bufs)/sizeof(cfg_bufs[0])))
#define CFG_N_HIST   ((int)(sizeof(cfg_hist)/sizeof(cfg_hist[0])))
#define CFG_N_ITEMS  11

static const char *s_cfg_names[CFG_N_ITEMS] = {
    "Baud", "Bright", "SBuf", "SHist", "IHist", "RHist", "NetRst", "Clear",
    "WiFiAP", "I2Cmo", "USB mod",
};

static int cfg_baud_index(void)
{
    uint32_t b = 0;
    uart_get_baudrate(UART1_PORT_NUM, &b);
    for (int i = 0; i < CFG_N_BAUD; i++) if (cfg_bauds[i] == b) return i;
    return 1;
}

static void cfg_adjust(int dir)
{
    s_cfg_edit_ts = cfg_now_s();          /* any tweak restarts the idle clock */
    switch (s_cfg_sel) {
    case 0:
        s_cfg_baud_idx = (s_cfg_baud_idx + dir + CFG_N_BAUD) % CFG_N_BAUD;
        serial_bridge_set_baud(cfg_bauds[s_cfg_baud_idx]);
        break;
    case 1:
        s_cfg_bright_idx = (s_cfg_bright_idx + dir + CFG_N_BRIGHT) % CFG_N_BRIGHT;
        oled_set_contrast((uint8_t)((cfg_bright[s_cfg_bright_idx] * 255) / 100));
        break;
    case 2:
        s_cfg_buf_idx = (s_cfg_buf_idx + dir + CFG_N_BUF) % CFG_N_BUF;
        serial_bridge_set_bufsize((size_t)cfg_bufs[s_cfg_buf_idx]);
        break;
    case 3:
        s_cfg_shist_idx = (s_cfg_shist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        spi_mon_set_history_max(cfg_hist[s_cfg_shist_idx]);
        break;
    case 4:
        s_cfg_ihist_idx = (s_cfg_ihist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        i2c_mon_set_history_max(cfg_hist[s_cfg_ihist_idx]);
        break;
    case 5:
        s_cfg_rhist_idx = (s_cfg_rhist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        /* Route through the public setter: it resets the ring head/current
         * line together with the max, keeping s_rx_line_w < s_rx_hist_max. */
        menu_set_rx_hist_max(cfg_hist[s_cfg_rhist_idx]);
        break;
    default:
        break;   /* NetRst / Clear are actions, handled on confirm */
    }
}

void menu_clear_rx(void);

/* ------------------------------------------------------------------ */
/* Page switching + slide animation plumbing                          */
/* ------------------------------------------------------------------ */

static void ui_kick(void)
{
    if (s_kick) xSemaphoreGive(s_kick);
}

/* Ring position of a page id, or -1 (MENU_HOME is not part of the ring). */
static int ring_pos_of(menu_page_t p)
{
    for (int i = 0; i < NAV_N; i++) if (s_nav_ring[i] == p) return i;
    return -1;
}

/* Change page. dir != 0 requests the vertical slide: the current (old) screen
 * is snapshotted here, and the UI task later renders the new page and plays
 * the frames. dir > 0: new page enters from the bottom (next); dir < 0: from
 * the top (previous / back). All panel I/O stays on the UI task. */
static void set_page(menu_page_t p, int dir)
{
    if (p == s_current_page) dir = 0;   /* re-entering the same page: never
                                         * play a self-slide */
    if (s_nav_lock) xSemaphoreTake(s_nav_lock, portMAX_DELAY);
    oled_fb_snapshot(s_anim_old);
    s_current_page = p;
    int pos = ring_pos_of(p);
    if (pos >= 0) s_selected = pos;
    if (p == MENU_CONFIG) s_cfg_state = CFG_LIST;
    if (p == MENU_RX_MON) s_rx_view_offset = 0;
    if (dir != 0) {
        s_anim_dir = dir;
        s_anim_pending = true;
    }
    if (s_nav_lock) xSemaphoreGive(s_nav_lock);
    ui_kick();
}

/* Sleep until the next periodic redraw or until navigation wakes us. */
void menu_ui_wait(int ms)
{
    if (ms < 1) ms = 1;
    if (s_kick) xSemaphoreTake(s_kick, pdMS_TO_TICKS(ms));
    else vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ------------------------------------------------------------------ */
/* CFG helpers                                                        */
/* ------------------------------------------------------------------ */

/* Items 6.. are one-shot actions: SW2 executes them straight from the list;
 * items 0..5 are value selectors (list -> edit -> adjust -> confirm). */
static bool cfg_is_action(int i) { return i >= 6; }

static void cfg_toggle_i2c_mode(void)
{
    int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
    i2c_mon_stop();
    if (i2c_mon_mode() == I2C_MON_SLAVE) {
        if (sda >= 0 && scl >= 0) i2c_mon_start_passive(sda, scl);
    } else {
        if (sda >= 0 && scl >= 0) i2c_mon_start(sda, scl, 0);
    }
}

/* USB-C role cycle off -> DAP -> TTL -> off (Config item 10). */
static void usb_reboot_cb(void *arg) { esp_restart(); }

static void cfg_cycle_usb_mode(void)
{
    static esp_timer_handle_t s_reboot_timer;
    uint8_t conf = pin_config_usb_mode();
    uint8_t next = (uint8_t)((conf + 1U) % 3U);
    static const char *mode_s[3] = { "off", "dap", "ttl" };
    bool phy_busy = dap_usb_is_started() || usb_ttl_is_started();

    pin_config_set_usb_mode(next);

    if (!phy_busy) {
        /* Nothing owns the PHY yet: bring the new role up live. */
        if (next == USB_MODE_DAP && dap_usb_start() != ESP_OK) {
            ESP_LOGW(TAG, "USB DAP failed to start");
        } else if (next == USB_MODE_TTL && usb_ttl_start() != ESP_OK) {
            ESP_LOGW(TAG, "USB-TTL bridge failed to start");
        }
        ESP_LOGI(TAG, "USB mode -> %s (active now)", mode_s[next]);
    } else if (next != conf) {
        /* The running stack keeps the PHY; reboot to switch roles. Give the
         * display one redraw with the new value first. */
        if (!s_reboot_timer) {
            const esp_timer_create_args_t a = {
                .callback = usb_reboot_cb, .name = "usbmode"
            };
            esp_timer_create(&a, &s_reboot_timer);
        }
        if (s_reboot_timer) {
            esp_timer_start_once(s_reboot_timer, 1500000);
            ESP_LOGI(TAG, "USB mode -> %s: rebooting in 1.5 s to switch",
                     mode_s[next]);
        }
    }
}

static void cfg_execute_action(int i)
{
    switch (i) {
    case 6:  /* NetRst: forget saved WiFi credentials */
        wifi_manager_clear_credentials();
        break;
    case 7:  /* Clear: drop every captured buffer */
        serial_bridge_clear();
        spi_mon_clear();
        i2c_mon_clear();
        menu_clear_rx();
        break;
    case 8:  /* WiFiAP: bring up the hotspot */
        wifi_manager_start_ap();
        break;
    case 9:  /* I2Cmo: slave monitor <-> passive sniffer */
        cfg_toggle_i2c_mode();
        break;
    case 10: /* USB mode: cycle the USB-C role off -> DAP -> TTL -> off.
              * OFF -> active starts immediately. Any switch involving an
              * already-claimed PHY reboots automatically after ~1.5 s -
              * a live USB device stack is never torn down mid-transfer. */
        cfg_cycle_usb_mode();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Button handling                                                    */
/* ------------------------------------------------------------------ */

/* SW1 = left button = up / previous page. */
void menu_on_sw1_press(void)
{
    switch (s_current_page) {
    case MENU_HOME:
        s_selected = (s_selected + NAV_N - 1) % NAV_N;
        ui_kick();
        break;
    case MENU_CONFIG:
        if (cfg_edit_active())
            cfg_adjust(-1);
        else
            s_cfg_sel = (s_cfg_sel + CFG_N_ITEMS - 1) % CFG_N_ITEMS;
        ui_kick();
        break;
    default: {
        int pos = ring_pos_of(s_current_page);
        if (pos < 0) pos = s_selected;
        set_page(s_nav_ring[(pos + NAV_N - 1) % NAV_N], -1);
        break;
    }
    }
}

/* SW3 = right button = down / next page. */
void menu_on_sw3_press(void)
{
    switch (s_current_page) {
    case MENU_HOME:
        s_selected = (s_selected + 1) % NAV_N;
        ui_kick();
        break;
    case MENU_CONFIG:
        if (cfg_edit_active())
            cfg_adjust(+1);
        else
            s_cfg_sel = (s_cfg_sel + 1) % CFG_N_ITEMS;
        ui_kick();
        break;
    default: {
        int pos = ring_pos_of(s_current_page);
        if (pos < 0) pos = s_selected;
        set_page(s_nav_ring[(pos + 1) % NAV_N], +1);
        break;
    }
    }
}

void menu_on_sw2_press(void)
{
    switch (s_current_page) {
    case MENU_HOME:
        set_page(s_nav_ring[s_selected], +1);
        break;

    case MENU_RX_MON:
        s_rx_hex_mode = !s_rx_hex_mode;
        break;

    case MENU_SPI:
        s_spi_hex_mode = !s_spi_hex_mode;
        break;

    case MENU_I2C:
        s_i2c_hex_mode = !s_i2c_hex_mode;
        break;

    case MENU_SWD:
        /* Read the target's DP IDCODE on demand. The pads are shared with
         * the USB/TCP DAP transports, so hold the bus for the sequence. */
        swd_bus_lock();
        s_swd_last_err = (int)swd_read_idcode(&s_swd_idcode);
        swd_bus_unlock();
        s_swd_idcode_valid = (s_swd_last_err == 0);
        break;

    case MENU_CONFIG:
        if (cfg_edit_active()) {
            s_cfg_state = CFG_LIST;                  /* done editing */
        } else if (cfg_is_action(s_cfg_sel)) {
            cfg_execute_action(s_cfg_sel);           /* one-shot action */
        } else {
            s_cfg_baud_idx = cfg_baud_index();
            int sv = spi_mon_get_history_max();
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == sv) s_cfg_shist_idx = i;
            int iv = i2c_mon_get_history_max();
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == iv) s_cfg_ihist_idx = i;
            int rv = s_rx_hist_max;
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == rv) s_cfg_rhist_idx = i;
            s_cfg_state   = CFG_EDIT;
            s_cfg_edit_ts = cfg_now_s();
        }
        break;

    default:
        break;
    }
    ui_kick();   /* context actions repaint immediately, not on the next tick */
}

/* SW2 long (held ~600 ms): ALWAYS returns to HOME - the one universal
 * escape. The previous behaviour ("first long cancels a config edit") was
 * the trapped-in-Config bug: the gesture got swallowed with only a subtle
 * visual change, and releasing too early fires a click that re-enters EDIT
 * mode, so the page seems impossible to leave. There is nothing to cancel:
 * adjustments apply live, and entering Config always starts from the list. */
void menu_on_sw2_long_press(void)
{
    if (s_current_page != MENU_HOME) set_page(MENU_HOME, -1);
}

/* Simulated buttons (HTTP /api/btn). "long" and "hold" both mean the SW2 long
 * gesture now; SW1/SW3 ignore them. */
void menu_simulate_button(int btn_id, const char *action)
{
    bool is_long = action && (strcmp(action, "long") == 0 ||
                              strcmp(action, "hold") == 0);
    if (btn_id == 2) {
        if (is_long) menu_on_sw2_long_press();
        else         menu_on_sw2_press();
    } else if (btn_id == 1) {
        menu_on_sw1_press();
    } else if (btn_id == 3) {
        menu_on_sw3_press();
    }
}

/* ------------------------------------------------------------------ */
/* RX capture                                                         */
/* ------------------------------------------------------------------ */

static int char_pixel_width(unsigned char c)
{
    return (c < 0x80) ? OLED_CHAR_W : OLED_CJK_W;
}

static void push_serial_data(char prefix, const uint8_t *data, uint32_t len)
{
    if (s_rx_cur_col > 0) rx_new_line();
    if (s_rx_line_n == 0) s_rx_line_n = 1;

    s_rx_lines[s_rx_line_w][0] = prefix;
    s_rx_lines[s_rx_line_w][1] = ' ';
    s_rx_cur_col = 2;
    s_rx_line_len[s_rx_line_w] = 2;
    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
    int display_w = 0;

    for (uint32_t i = 0; i < len; i++) {
        char c = (char)data[i];

        const char *esc = NULL;
        int esc_w = 0;
        switch (c) {
        case '\n':   esc = "\\n"; esc_w = 2; break;
        case '\r':   esc = "\\r"; esc_w = 2; break;
        case '\t':   esc = "\\t"; esc_w = 2; break;
        case '\0':   esc = "\\0"; esc_w = 2; break;
        case '\x1b': esc = "\\e"; esc_w = 2; break;
        default:
            if ((unsigned char)c < 0x20) {
                static char hexbuf[5];
                snprintf(hexbuf, sizeof(hexbuf), "\\x%02X", (unsigned char)c);
                esc = hexbuf;
                esc_w = 4;
            }
            break;
        }

        if (esc) {
            for (int e = 0; e < esc_w; e++) {
                if (display_w + OLED_CHAR_W > RX_CONTENT_PX) {
                    rx_new_line();
                    s_rx_lines[s_rx_line_w][0] = ' ';
                    s_rx_lines[s_rx_line_w][1] = ' ';
                    s_rx_cur_col = 2;
                    s_rx_line_len[s_rx_line_w] = 2;
                    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                    display_w = 0;
                }
                if (s_rx_line_len[s_rx_line_w] < RX_LINE_MAX) {
                    s_rx_lines[s_rx_line_w][s_rx_cur_col++] = esc[e];
                    s_rx_line_len[s_rx_line_w]++;
                    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                    display_w += OLED_CHAR_W;
                }
            }
        } else {
            int cw = char_pixel_width((unsigned char)c);
            if (display_w + cw > RX_CONTENT_PX) {
                rx_new_line();
                s_rx_lines[s_rx_line_w][0] = ' ';
                s_rx_lines[s_rx_line_w][1] = ' ';
                s_rx_cur_col = 2;
                s_rx_line_len[s_rx_line_w] = 2;
                s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                display_w = 0;
            }
            if (s_rx_line_len[s_rx_line_w] < RX_LINE_MAX) {
                s_rx_lines[s_rx_line_w][s_rx_cur_col++] = c;
                s_rx_line_len[s_rx_line_w]++;
                s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                display_w += cw;
            }
        }
    }
}

void menu_push_rx_data(const uint8_t *data, uint32_t len) { push_serial_data('>', data, len); }
void menu_push_tx_data(const uint8_t *data, uint32_t len) { push_serial_data('<', data, len); }

/* ------------------------------------------------------------------ */
/* Shared drawing helpers                                             */
/* ------------------------------------------------------------------ */

/* Draw the standard page header and clear the content area below it. The
 * ring position "n/8" is stamped right-aligned so you always know which of
 * the eight pages you are on and how far the list slid. */
static void page_header(const char *title)
{
    oled_clear();
    oled_text(0, ROW_TITLE_Y, title, false);
    int pos = ring_pos_of(s_current_page);
    if (pos >= 0) {
        char idx[16];
        snprintf(idx, sizeof idx, "%d/%d", pos + 1, NAV_N);
        oled_text(OLED_WIDTH - (int)strlen(idx) * OLED_CHAR_W, ROW_TITLE_Y, idx, false);
    }
    oled_hline(0, ROW1_Y - 1, OLED_WIDTH, true);
}

/* Left-justified label plus a right-aligned value on the same text row. */
static void row_kv(int y, const char *key, const char *value)
{
    oled_text(0, y, key, false);
    if (value && *value) {
        int w = oled_utf8_width(value, OLED_WIDTH);
        int x = OLED_WIDTH - w;
        if (x < 0) x = 0;
        oled_utf8(x, y, value, false);
    }
}

/* Append `n` bytes as hex to `out`, never overflowing `cap` (keeps the NUL). */
static void append_hex(char *out, size_t cap, const uint8_t *d, int n)
{
    size_t used = strlen(out);
    for (int i = 0; i < n && used + 2 < cap; i++) {
        used += (size_t)snprintf(&out[used], cap - used, "%02X", d[i]);
    }
}

/* Append `n` bytes as printable ASCII ('.' for unprintable) to `out`. */
static void append_ascii(char *out, size_t cap, const uint8_t *d, int n)
{
    size_t used = strlen(out);
    for (int i = 0; i < n && used + 1 < cap; i++) {
        unsigned char c = d[i];
        out[used++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[used] = '\0';
}

/* ------------------------------------------------------------------ */
/* Pages                                                              */
/* ------------------------------------------------------------------ */

/* HOME is a vertically scrolling page list. Three rows are visible; the
 * window follows the cursor so the highlighted page is always centre-ish, and
 * the highlight is a full-width inverted bar that is impossible to lose. */
static void render_home(void)
{
    oled_clear();
    oled_text(0, ROW_TITLE_Y, "Menu", false);
    char b[16];
    snprintf(b, sizeof b, "%d/%d", s_selected + 1, NAV_N);
    oled_text(OLED_WIDTH - (int)strlen(b) * OLED_CHAR_W, ROW_TITLE_Y, b, false);
    oled_hline(0, ROW1_Y - 1, OLED_WIDTH, true);

    const int rows = 3;
    int top = s_selected - 1;          /* keep the cursor away from the edges */
    if (top < 0) top = 0;
    if (top > NAV_N - rows) top = NAV_N - rows;

    for (int r = 0; r < rows; r++) {
        int i = top + r;
        if (i >= NAV_N) break;
        int y  = ROW1_Y + r * OLED_CHAR_H;
        bool sel = (i == s_selected);
        if (sel) oled_fill_rect(0, y, OLED_WIDTH, OLED_CHAR_H, true);
        oled_text(0, y, s_nav_labels[i], sel);
        if (sel) oled_text(OLED_WIDTH - OLED_CHAR_W, y, ">", true);
    }
}

static void render_status(void)
{
    page_header("Status");

    wifi_state_t st = wifi_manager_get_state();
    const char *ws = (st == WIFI_STATE_AP_MODE)       ? "AP"  :
                     (st == WIFI_STATE_CONNECTED_STA) ? "STA" :
                     (st == WIFI_STATE_CONNECTING)    ? "..." : "OFF";
    char ip[20] = "-";
    wifi_manager_get_ip_str(ip, sizeof(ip));
    row_kv(ROW1_Y, ws, ip);

    uint32_t baud = 0;
    uart_get_baudrate(UART1_PORT_NUM, &baud);
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)baud);
    row_kv(ROW2_Y, "Baud", buf);

    snprintf(buf, sizeof(buf), "R%lu T%lu",
             (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count());
    row_kv(ROW3_Y, "UART1", buf);
}

static void render_rx_mon(void)
{
    char title[24];
    /* Keep it short: the "n/8" badge owns the right end of the title row. */
    snprintf(title, sizeof(title), "RX %s", s_rx_hex_mode ? "HEX" : "TXT");
    page_header(title);

    if (s_rx_line_n <= 0) {
        oled_utf8(0, ROW1_Y, "等待数据...", false);
        return;
    }

    int y = ROW1_Y;
    for (int row = 0; row < 3; row++) {
        int back = s_rx_view_offset + row;
        if (back >= s_rx_line_n) break;
        int idx = (s_rx_line_w - back + s_rx_hist_max * 2) % s_rx_hist_max;
        const char *line = s_rx_lines[idx];
        if (!line || !*line) continue;

        if (s_rx_hex_mode) {
            /* Render every byte as two hex digits, prefixed with the marker. */
            char hex[RX_LINE_MAX * 3 + 2];
            int n = 0;
            if (line[0] == '>' || line[0] == '<') hex[n++] = line[0];
            for (int i = 2; line[i] && n < (int)sizeof(hex) - 3; i++) {
                n += snprintf(&hex[n], sizeof(hex) - n, "%02X", (unsigned char)line[i]);
            }
            hex[n] = '\0';
            oled_utf8(0, y, hex, false);
        } else {
            oled_utf8(0, y, line, false);
        }
        y += OLED_CHAR_H;
    }
}

static void render_swd(void)
{
    page_header("SWD / DAP");
    char buf[24];

    /* Fixed dedicated SWD pins. */
    snprintf(buf, sizeof(buf), "C%d D%d R%d", PIN_SWD_SWCLK, PIN_SWD_SWDIO, PIN_SWD_NRST);
    row_kv(ROW1_Y, "Pin", buf);

    if (s_swd_idcode_valid) {
        snprintf(buf, sizeof(buf), "%08lX", (unsigned long)s_swd_idcode);
        row_kv(ROW2_Y, "ID", buf);
    } else {
        row_kv(ROW2_Y, "ID", s_swd_last_err ? "ERR" : "-");
    }

    row_kv(ROW3_Y, "SW2", "read ID");
}

static void render_config(void)
{
    char title[24];
    /* The mode must be OBVIOUS: the editor is a different screen, not the
     * same list with a 1-px arrow - that is how "I cannot exit Config"
     * happened. Title doubles as the mode label. */
    if (s_cfg_state == CFG_EDIT)
        snprintf(title, sizeof(title), "Edit %s", s_cfg_names[s_cfg_sel]);
    else
        snprintf(title, sizeof(title), "Config");
    page_header(title);

    char val[16];
    /* Show a sliding window of three items around the selection. */
    int first = s_cfg_sel - 1;
    if (first < 0) first = 0;
    if (first > CFG_N_ITEMS - 3) first = CFG_N_ITEMS - 3;

    for (int row = 0; row < 3; row++) {
        int i = first + row;
        if (i >= CFG_N_ITEMS) break;
        int y = ROW1_Y + row * OLED_CHAR_H;

        switch (i) {
        case 0: snprintf(val, sizeof(val), "%lu", (unsigned long)cfg_bauds[s_cfg_baud_idx]); break;
        case 1: snprintf(val, sizeof(val), "%d%%", cfg_bright[s_cfg_bright_idx]); break;
        case 2: snprintf(val, sizeof(val), "%d", cfg_bufs[s_cfg_buf_idx]); break;
        case 3: snprintf(val, sizeof(val), "%d", cfg_hist[s_cfg_shist_idx]); break;
        case 4: snprintf(val, sizeof(val), "%d", cfg_hist[s_cfg_ihist_idx]); break;
        case 5: snprintf(val, sizeof(val), "%d", cfg_hist[s_cfg_rhist_idx]); break;
        case 6: snprintf(val, sizeof(val), "do"); break;   /* NetRst action */
        case 7: snprintf(val, sizeof(val), "do"); break;   /* Clear action  */
        case 8: {                                          /* WiFiAP state  */
            wifi_state_t w = wifi_manager_get_state();
            snprintf(val, sizeof(val), "%s",
                     w == WIFI_STATE_AP_MODE ? "AP" :
                     w == WIFI_STATE_CONNECTED_STA ? "STA" : "off");
            break; }
        case 9: snprintf(val, sizeof(val), "%s",
                         i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive"); break;
        case 10: {                                   /* USB-C role        */
            static const char *umode_s[3] = { "off", "dap", "ttl" };
            uint8_t conf = pin_config_usb_mode();
            bool pending = (dap_usb_is_started() || usb_ttl_is_started())
                           && ((conf == USB_MODE_DAP) != dap_usb_is_started()
                               || (conf == USB_MODE_TTL) != usb_ttl_is_started());
            snprintf(val, sizeof(val), "%s%s", umode_s[conf], pending ? "*" : "");
            break; }
        default: val[0] = '\0'; break;
        }

        bool sel = (i == s_cfg_sel);
        if (sel) oled_fill_rect(0, y, OLED_WIDTH, OLED_CHAR_H, true);

        oled_text(0, y, s_cfg_names[i], sel);
        if (val[0]) {
            int w = (int)strlen(val) * OLED_CHAR_W;
            oled_text(OLED_WIDTH - w, y, val, sel);
        }
        if (sel && s_cfg_state == CFG_EDIT) {
            oled_text(OLED_WIDTH - 8 * (int)strlen(val) - 8, y, "<", sel);
        }
    }
}

static void render_pwm(void)
{
    page_header("PWM");

    float freq = 0.0f, duty = 0.0f;
    pwm_mon_get(&freq, &duty);

    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f Hz", (double)freq);
    row_kv(ROW1_Y, "Freq", buf);
    snprintf(buf, sizeof(buf), "%.1f %%", (double)duty);
    row_kv(ROW2_Y, "Duty", buf);

    /* Two periods of the measured square wave across the bottom row. */
    const int y_hi = ROW3_Y + 3, y_lo = ROW3_Y + 14;
    int hi_px = (int)((duty / 100.0f) * 64.0f);
    if (hi_px < 0) hi_px = 0;
    if (hi_px > 64) hi_px = 64;

    for (int rep = 0; rep < 2; rep++) {
        int x0 = rep * 64;
        if (hi_px > 0)  oled_hline(x0, y_hi, hi_px, true);
        if (hi_px < 64) oled_hline(x0 + hi_px, y_lo, 64 - hi_px, true);
        /* Edge transitions. */
        if (hi_px > 0 && hi_px < 64) {
            oled_vline(x0 + hi_px, y_hi, y_lo - y_hi + 1, true);
        }
        oled_vline(x0, y_hi, y_lo - y_hi + 1, true);
    }
}

static void render_spi(void)
{
    char title[24];
    snprintf(title, sizeof(title), "SPI %s %s",
             s_spi_hex_mode ? "HEX" : "TXT",
             spi_mon_running() ? "run" : "stop");
    page_header(title);

    char buf[40];
    snprintf(buf, sizeof(buf), "%lu txn",
             (unsigned long)spi_mon_get_count());
    row_kv(ROW1_Y, "Count", buf);

    /* Walk the two most recent transactions (newest first). */
    uint32_t count = spi_mon_get_count();
    spi_txn_t t;
    int shown = 0;
    for (uint32_t seq = count; seq > 0 && shown < 2; seq--) {
        if (spi_mon_read_history(seq - 1, &t) != 0) break;
        snprintf(buf, sizeof(buf), "%lu:", (unsigned long)t.seq);
        if (s_spi_hex_mode) append_hex(buf, sizeof(buf), t.mosi, t.len > 6 ? 6 : t.len);
        else                append_ascii(buf, sizeof(buf), t.mosi, t.len > 10 ? 10 : t.len);
        oled_utf8(0, shown == 0 ? ROW2_Y : ROW3_Y, buf, false);
        shown++;
    }
    if (shown == 0) oled_text(0, ROW2_Y, "no data", false);
}

static void render_i2c(void)
{
    char title[28];
    snprintf(title, sizeof(title), "I2C %s %s",
             s_i2c_hex_mode ? "HEX" : "TXT",
             i2c_mon_running() ? "run" : "stop");
    page_header(title);

    char buf[40];
    snprintf(buf, sizeof(buf), "%lu txn",
             (unsigned long)i2c_mon_get_count());
    row_kv(ROW1_Y, "Count", buf);

    uint32_t count = i2c_mon_get_count();
    i2c_txn_t t;
    int shown = 0;
    for (uint32_t seq = count; seq > 0 && shown < 2; seq--) {
        if (i2c_mon_read_history(seq - 1, &t) != 0) break;
        snprintf(buf, sizeof(buf), "%02X%c ", (unsigned)t.addr, t.read ? 'R' : 'W');
        if (s_i2c_hex_mode) append_hex(buf, sizeof(buf), t.data, t.len > 6 ? 6 : t.len);
        else                append_ascii(buf, sizeof(buf), t.data, t.len > 10 ? 10 : t.len);
        oled_utf8(0, shown == 0 ? ROW2_Y : ROW3_Y, buf, false);
        shown++;
    }
    if (shown == 0) {
        /* No traffic yet: show the monitor mode instead so the page is useful. */
        oled_text(0, ROW2_Y, i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive", false);
        oled_text(0, ROW3_Y, "CFG item I2Cmo", false);
    }
}

static void render_ai(void)
{
    page_header("AI / MCP");

    wifi_state_t st = wifi_manager_get_state();
    oled_text(0, ROW1_Y, (st == WIFI_STATE_CONNECTED_STA ||
                          st == WIFI_STATE_AP_MODE) ? "MCP ready" : "MCP no net", false);

    char ip[20] = "-";
    wifi_manager_get_ip_str(ip, sizeof(ip));
    row_kv(ROW2_Y, "IP", ip);

    oled_text(0, ROW3_Y,
              dap_usb_is_started() ? "DAP usb+tcp:5555" : "DAP tcp:5555",
              false);
}

void menu_render(void)
{
    if (!oled_is_ready()) { s_anim_pending = false; return; }

    /* Consume any pending page-swap intent set by the button task. The new
     * page is rendered once, then replayed as a vertical slide from the old
     * screen. All panel writes happen here, on the single UI task.
     * The lock serializes against set_page(): a press that arrives while the
     * slide plays snapshots cleanly afterwards instead of grabbing a
     * half-drawn frame or getting dropped. */
    if (s_nav_lock) xSemaphoreTake(s_nav_lock, portMAX_DELAY);

    /* Even with nobody touching the keys the idle editor must close. */
    cfg_edit_active();

    bool anim = s_anim_pending;
    int  dir  = s_anim_dir;
    s_anim_pending = false;

    switch (s_current_page) {
    case MENU_HOME:   render_home();   break;
    case MENU_STATUS: render_status(); break;
    case MENU_RX_MON: render_rx_mon(); break;
    case MENU_SWD:    render_swd();    break;
    case MENU_CONFIG: render_config(); break;
    case MENU_PWM:    render_pwm();    break;
    case MENU_SPI:    render_spi();    break;
    case MENU_I2C:    render_i2c();    break;
    case MENU_AI:     render_ai();     break;
    default:          render_home();   break;
    }

    if (anim) oled_slide_from(s_anim_old, dir, ANIM_FRAMES);
    else      oled_flush();

    if (s_nav_lock) xSemaphoreGive(s_nav_lock);
}

int menu_get_current_page(void) { return (int)s_current_page; }
int menu_get_selected(void) { return s_selected; }

int menu_get_rx_hist_n(void)   { return s_rx_line_n; }
int menu_get_rx_hist_max(void) { return s_rx_hist_max; }
void menu_set_rx_hist_max(int m)
{
    if (m < 2) m = 2;
    if (m > RX_LINE_CAP) m = RX_LINE_CAP;
    s_rx_hist_max = m;
    s_rx_line_n = 0;
    s_rx_line_w = 0;
}
void menu_clear_rx(void)
{
    s_rx_line_n = 0;
    s_rx_line_w = 0;
    s_rx_cur_col = 0;
}
