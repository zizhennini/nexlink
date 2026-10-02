/*
 * menu_ui.c - OLED menu for NexLink (SSD1306 128x64, I2C).
 *
 * WHY THIS WAS REWRITTEN
 * ----------------------
 * The previous menu was an eight-page ring in which the home screen was BOTH a
 * launcher and one of the ring stops. That made the same two buttons mean
 * different things in different places: on home SW1/SW3 moved a cursor, on any
 * other page they switched pages. The config screen had its own private
 * list/edit state machine on top, and crammed eleven items into one page under
 * abbreviations (SBuf, SHist, RHist, NetRst...).
 *
 * This version makes three concepts framework-level instead of per-page:
 *
 *   1. A GROUPED TREE. Home is a plain list of four groups; each group is a
 *      list of entries. An entry is one of four kinds: another list, a
 *      read-only screen, a one-shot action, or an adjustable value. Pages are
 *      pushed on a small return stack, so "SW2 = enter / back" is the entire
 *      input model and the tree can grow without new key logic.
 *
 *   2. ONE KEY MEANING EVERYWHERE (the point of the rewrite):
 *        SW1 = up      SW3 = down      SW2 = enter / back
 *        SW2 long      = jump home from anywhere, including edit mode
 *      On a read-only screen SW1/SW3 scroll its body when it has one (only the
 *      live RX monitor does) and are inert otherwise - they never silently
 *      mean something else.
 *
 *   3. A FRAMEWORK-LEVEL EDIT MODE. An adjustable entry is edited by the same
 *      code path wherever it lives: SW2 enters, SW1/SW3 change the value, SW2
 *      confirms. The five-second idle exit is kept - an editor the user walked
 *      away from must never swallow the escape gesture.
 *
 * PAINTING RULE (unchanged from the previous implementation)
 * ----------------------------------------------------------
 * Only the UI task ever touches the panel: button handlers mutate state and
 * kick the UI task through s_kick; menu_render() snapshots and flushes. The
 * panel sits on a shared I2C bus with the I2C monitor, so a second writer
 * would corrupt both.
 */
#include "menu_ui.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
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

static const char *TAG = "menu";

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/*                                                                     */
/*  The font cell is 16px tall, so the panel holds exactly four text    */
/*  rows from y=0 to y=48 (48+16 = 64 = the last pixel row). The layout */
/*  is therefore: title bar on row 0, up to THREE content rows, and the */
/*  key hint as the fourth row. Anything drawn below y=48 is clipped -  */
/*  an earlier revision put the hint at y=55 and lost its bottom third. */
/* ------------------------------------------------------------------ */

#define ROW_TITLE_Y   0
#define ROW1_Y        16
#define ROW2_Y        32
#define ROW3_Y        48

/* Content rows available below the title, leaving row 3 for the hint. */
#define BODY_ROWS     3

/* Text columns that actually fit (128px / 8px per cell). */
#define TEXT_COLS     16

/* ------------------------------------------------------------------ */
/*  RX line history (fed by the serial bridge callback)                */
/* ------------------------------------------------------------------ */

#define RX_LINE_MAX   60
#define RX_LINE_CAP   100
static char  s_rx_lines[RX_LINE_CAP][RX_LINE_MAX + 1];
static int   s_rx_line_len[RX_LINE_CAP];
static volatile int s_rx_line_w = 0;
static volatile int s_rx_line_n = 0;
static volatile int s_rx_hist_max = 50;
static int   s_rx_cur_col = 0;
static bool  s_rx_hex_mode = false;

/* Content width: 128px minus the 2px margin and the "> " prefix. */
#define RX_CONTENT_PX  108

/* ================================================================== */
/*  Menu tree types                                                    */
/* ================================================================== */

typedef struct menu_list menu_list_t;

typedef enum {
    ENTRY_LIST,     /* push another list                              */
    ENTRY_SCREEN,   /* open a read-only screen (page id)              */
    ENTRY_ACTION,   /* run fn() once                                  */
    ENTRY_VALUE,    /* edit: index into a value table, applied by fn() */
} entry_kind_t;

typedef struct {
    const char  *label;
    entry_kind_t kind;
    union {
        const menu_list_t *list;               /* ENTRY_LIST */
        int                page;               /* ENTRY_SCREEN */
        void             (*fn)(void);          /* ENTRY_ACTION */
        struct {                               /* ENTRY_VALUE */
            int8_t      *idx;
            uint8_t      count;
            const int   *table;
            void       (*apply)(int value);
            const char  *unit;
        } val;
    } u;
} menu_entry_t;

struct menu_list {
    const char         *title;
    const menu_entry_t *items;
    uint8_t             count;
};

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */

static const menu_list_t list_root;
static const menu_list_t list_monitor;
static const menu_list_t list_probe;
static const menu_list_t list_system;
static const menu_list_t list_info;

static void act_clear_rx(void);
static void act_clear_capture(void);
static void act_reset_target(void);
static void act_swd_idcode(void);
static void act_usb_off(void);
static void act_usb_dap(void);
static void act_usb_ttl(void);
static void act_wifi_ap(void);

/* ================================================================== */
/*  Configurable values                                                */
/* ================================================================== */

static const int val_bauds[]  = {9600, 115200, 460800, 921600};
static const int val_bright[] = {10, 25, 50, 75, 100};
static const int val_bufs[]   = {1024, 2048, 4096, 8192};
static const int val_hist[]   = {10, 30, 50, 100};

#define N_BAUD   4
#define N_BRIGHT 5
#define N_BUF    4
#define N_HIST   4

static int8_t s_baud_idx;
static int8_t s_bright_idx;
static int8_t s_buf_idx;
static int8_t s_shist_idx;
static int8_t s_ihist_idx;
static int8_t s_rhist_idx;

static void apply_baud(int v)   { serial_bridge_set_baud((uint32_t)v); }
static void apply_bright(int v) { oled_set_contrast((uint8_t)((v * 255) / 100)); }
static void apply_buf(int v)    { serial_bridge_set_bufsize((size_t)v); }
static void apply_shist(int v)  { spi_mon_set_history_max(v); }
static void apply_ihist(int v)  { i2c_mon_set_history_max(v); }
static void apply_rhist(int v)  { menu_set_rx_hist_max(v); }

/* ================================================================== */
/*  List contents                                                      */
/* ================================================================== */

static const menu_entry_t items_monitor[] = {
    { "RX Monitor",    ENTRY_SCREEN, { .page = MENU_RX_MON } },
    { "I2C Bus",       ENTRY_SCREEN, { .page = MENU_I2C } },
    { "SPI Bus",       ENTRY_SCREEN, { .page = MENU_SPI } },
    { "PWM Measure",   ENTRY_SCREEN, { .page = MENU_PWM } },
    { "Capture Log",   ENTRY_SCREEN, { .page = MENU_CAPTURE } },
    { "Clear RX Hist", ENTRY_ACTION, { .fn = act_clear_rx } },
    { "Clear Capture", ENTRY_ACTION, { .fn = act_clear_capture } },
};
static const menu_list_t list_monitor = {
    "Monitor", items_monitor, sizeof(items_monitor) / sizeof(items_monitor[0])
};

static const menu_entry_t items_probe[] = {
    { "SWD / JTAG",   ENTRY_SCREEN, { .page = MENU_SWD } },
    { "Read IDCODE",  ENTRY_ACTION, { .fn = act_swd_idcode } },
    { "Reset Target", ENTRY_ACTION, { .fn = act_reset_target } },
    { "USB: off",     ENTRY_ACTION, { .fn = act_usb_off } },
    { "USB: probe",   ENTRY_ACTION, { .fn = act_usb_dap } },
    { "USB: serial",  ENTRY_ACTION, { .fn = act_usb_ttl } },
};
static const menu_list_t list_probe = {
    "Probe", items_probe, sizeof(items_probe) / sizeof(items_probe[0])
};

static const menu_entry_t items_system[] = {
    { "UART Baud",     ENTRY_VALUE, { .val = { &s_baud_idx,   N_BAUD,   val_bauds,  apply_baud,   "bps" } } },
    { "Brightness",    ENTRY_VALUE, { .val = { &s_bright_idx, N_BRIGHT, val_bright, apply_bright, "%"   } } },
    { "UART Buffer",   ENTRY_VALUE, { .val = { &s_buf_idx,    N_BUF,    val_bufs,   apply_buf,    "B"   } } },
    { "SPI Depth",     ENTRY_VALUE, { .val = { &s_shist_idx,  N_HIST,   val_hist,   apply_shist,  "rec" } } },
    { "I2C Depth",     ENTRY_VALUE, { .val = { &s_ihist_idx,  N_HIST,   val_hist,   apply_ihist,  "rec" } } },
    { "RX Lines",      ENTRY_VALUE, { .val = { &s_rhist_idx,  N_HIST,   val_hist,   apply_rhist,  "ln"  } } },
    { "WiFi: AP Mode", ENTRY_ACTION, { .fn = act_wifi_ap } },
};
static const menu_list_t list_system = {
    "System", items_system, sizeof(items_system) / sizeof(items_system[0])
};

static const menu_entry_t items_info[] = {
    { "Device Status", ENTRY_SCREEN, { .page = MENU_STATUS } },
    { "Network",       ENTRY_SCREEN, { .page = MENU_NET } },
    { "Firmware",      ENTRY_SCREEN, { .page = MENU_FIRMWARE } },
    { "USB Role",      ENTRY_SCREEN, { .page = MENU_USB_STATE } },
    { "AI / MCP",      ENTRY_SCREEN, { .page = MENU_AI } },
};
static const menu_list_t list_info = {
    "Info", items_info, sizeof(items_info) / sizeof(items_info[0])
};

static const menu_entry_t items_root[] = {
    { "Monitor", ENTRY_LIST, { .list = &list_monitor } },
    { "Probe",   ENTRY_LIST, { .list = &list_probe   } },
    { "System",  ENTRY_LIST, { .list = &list_system  } },
    { "Info",    ENTRY_LIST, { .list = &list_info    } },
};
static const menu_list_t list_root = {
    "NexLink", items_root, sizeof(items_root) / sizeof(items_root[0])
};

/* ================================================================== */
/*  Navigation state                                                   */
/* ================================================================== */

#define STACK_MAX 6

typedef enum {
    VIEW_LIST,     /* a list from the tree             */
    VIEW_SCREEN,   /* a read-only screen               */
    VIEW_EDIT,     /* editing s_edit                   */
} view_t;

static view_t             s_view = VIEW_LIST;
static const menu_list_t *s_stack[STACK_MAX];
static int                s_cursor[STACK_MAX];
static int                s_depth = 1;          /* root is level 0 */
static int                s_screen_page = MENU_HOME;
static int                s_selected = 0;
static int                s_scroll = 0;         /* RX monitor scroll-back */

static const menu_entry_t *s_edit;
static uint32_t            s_edit_ts;

/* SWD result shown by the SWD/JTAG screen. */
static uint32_t s_swd_idcode;
static bool     s_swd_idcode_valid;
static int      s_swd_last_err;

/* Animation + cross-task wakeup.
 *
 * The slide animation of the old page-ring is gone: with a grouped list tree
 * the useful motion is the cursor, and a full-screen slide on every list push
 * made the (slow, shared-I2C) panel visibly lag. s_anim_* is kept only because
 * oled_slide_from() is still available if a future transition wants it. */
static uint8_t           s_anim_old[OLED_FB_BYTES] __attribute__((unused));
static volatile bool     s_anim_pending __attribute__((unused));
static volatile int      s_anim_dir     __attribute__((unused));
static SemaphoreHandle_t s_kick;
static SemaphoreHandle_t s_nav_lock;

/* Defined in main.c; declared here rather than pulling in a private header. */
extern const char *main_boot_reason(void);

/* ================================================================== */
/*  Small helpers                                                      */
/* ================================================================== */

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static void ui_kick(void)
{
    if (s_kick) xSemaphoreGive(s_kick);
}

#define EDIT_TIMEOUT_S 5

/* True while an entry is being edited. Auto-exits after a period of no input
 * (Marlin behaviour). Called from both tasks; the volatile 32-bit store cannot
 * tear into a wrong value, so the worst case is one stale frame. */
static bool edit_active(void)
{
    if (s_view != VIEW_EDIT) return false;
    if (now_s() - s_edit_ts >= EDIT_TIMEOUT_S) {
        s_view = VIEW_LIST;
        return false;
    }
    return true;
}

/* ================================================================== */
/*  Actions                                                            */
/* ================================================================== */

static void act_clear_rx(void)      { menu_clear_rx(); }
static void act_clear_capture(void) { capture_reset_counters(); }

static void act_reset_target(void)
{
    /* NRST is a plain GPIO, so this works even when the target is wedged and
     * no SWD link can be established. */
    swd_bus_lock();
    swd_reset_target(true);
    vTaskDelay(pdMS_TO_TICKS(25));
    swd_reset_target(false);
    swd_bus_unlock();
    ESP_LOGI(TAG, "target reset pulse sent");
}

static void act_swd_idcode(void)
{
    /* The SWD pads are shared with the USB/TCP DAP transports, so hold the bus
     * across the sequence. */
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
    /* Show the result instead of only flashing it: switch to the SWD screen. */
    s_screen_page = MENU_SWD;
    s_scroll = 0;
    s_view = VIEW_SCREEN;
    ui_kick();
}

/* A USB role change reboots the board (a live USB stack is never torn down),
 * so tell the user before restarting instead of appearing to hang. */
static void usb_role_confirm(uint8_t mode, const char *label)
{
    pin_config_set_usb_mode(mode);
    oled_clear();
    oled_text(2, ROW1_Y, "USB role ->", false);
    oled_text(2, ROW2_Y, label, false);
    oled_text(2, ROW3_Y, "rebooting...", false);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static void act_usb_off(void) { usb_role_confirm(USB_MODE_OFF, "off"); }
static void act_usb_dap(void) { usb_role_confirm(USB_MODE_DAP, "probe"); }
static void act_usb_ttl(void) { usb_role_confirm(USB_MODE_TTL, "serial"); }

static void act_wifi_ap(void)
{
    wifi_manager_start_ap();
    oled_clear();
    oled_text(2, ROW1_Y, "WiFi AP mode", false);
    oled_text(2, ROW2_Y, "started", false);
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(1200));
}

/* ================================================================== */
/*  RX line history                                                    */
/* ================================================================== */

static void rx_new_line(void)
{
    s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
    if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
    s_rx_lines[s_rx_line_w][0] = '\0';
    s_rx_line_len[s_rx_line_w] = 0;
    s_rx_cur_col = 0;
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
    /* Clear together: otherwise s_rx_line_w could point outside the new window
     * and the ring would read entries that are no longer part of it. */
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
        if (c == '\r') continue;
        if (c == '\n') { rx_new_line(); continue; }

        char hexbuf[5];
        const char *ins;
        char one[2] = { c, '\0' };
        if (s_rx_hex_mode) {
            snprintf(hexbuf, sizeof(hexbuf), "%02X ", (uint8_t)c);
            ins = hexbuf;
        } else {
            ins = one;
        }

        int w = 0;
        for (const char *p = ins; *p; p++) w += char_pixel_width((unsigned char)*p);
        if (w == 0) continue;

        if (display_w + w > RX_CONTENT_PX) {
            rx_new_line();
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
/*  Text helpers                                                       */
/* ================================================================== */

static void page_header(const char *title)
{
    char line[TEXT_COLS + 1];
    /* Truncate explicitly: oled_text() draws whatever it is given, straight
     * past the right edge of the panel. */
    snprintf(line, sizeof(line), "%-*.*s", TEXT_COLS, TEXT_COLS, title);
    oled_text(0, ROW_TITLE_Y, line, false);
    oled_hline(0, ROW_TITLE_Y + 15, OLED_WIDTH, true);
}

/* One "key value" line. The key gets 6 columns, the value the remaining 9. */
static void row_kv(int y, const char *key, const char *value)
{
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "%-6.6s%.10s", key, value);
    line[TEXT_COLS] = '\0';
    oled_text(0, y, line, false);
}

/* Key hint on the bottom row (y=48). Text is padded to the full width so a
 * shorter hint does not leave tail characters from the previous screen. */
static void footer(const char *hint)
{
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "%-*.*s", TEXT_COLS, TEXT_COLS, hint);
    oled_text(0, ROW3_Y, line, false);
}

/* Centred line, for hint/status text that is shorter than the panel. */
static void centered(int y, const char *text)
{
    int n = (int)strlen(text);
    if (n > TEXT_COLS) n = TEXT_COLS;
    int x = (OLED_WIDTH - n * OLED_CHAR_W) / 2;
    if (x < 0) x = 0;
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof(line), "%.*s", TEXT_COLS, text);
    oled_text(x, y, line, false);
}

/* ================================================================== */
/*  Read-only screens                                                  */
/*                                                                    */
/*  Layout contract for every screen:                                  */
/*    y=16, y=32   two rows of key/value detail                        */
/*    y=48         hint row (footer) - never long body text            */
/*  Three content rows plus a hint would need a fifth row, which the   */
/*  panel does not have. A screen that genuinely needs a third value   */
/*  line uses compact() to fold it into row 2.                         */
/* ================================================================== */

static void screen_status(void)
{
    char v[24];
    page_header("Device Status");

    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(v, sizeof(v), "%luh%02lum  baud %lu", (unsigned long)(s / 3600),
             (unsigned long)((s / 60) % 60), (unsigned long)serial_bridge_get_baud());
    row_kv(ROW1_Y, "Up", v);

    snprintf(v, sizeof(v), "%lu/%lu tcp%u", (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count(),
             (unsigned)tcp_server_client_count());
    row_kv(ROW2_Y, "RX/TX", v);

    footer("SW2=back");
}

static void screen_net(void)
{
    char ip[16] = "-", v[24], ssid[24] = "-";
    int rssi = 0;
    wifi_state_t st = wifi_manager_get_state();
    wifi_manager_get_ip_str(ip, sizeof(ip));

    page_header("Network");
    row_kv(ROW1_Y, "State", st == WIFI_STATE_CONNECTED_STA ? "STA" :
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
    row_kv(ROW2_Y, "IP", v);

    footer(ssid);
}

static void screen_firmware(void)
{
    char v[24];
    page_header("Firmware");

    const esp_partition_t *run = esp_ota_get_running_partition();
    row_kv(ROW1_Y, "Slot", run ? run->label : "?");

    const char *s = "ok";
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) s = "pending";
    snprintf(v, sizeof(v), "%s %s", s, main_boot_reason());
    row_kv(ROW2_Y, "State", v);

    footer("SW2=back");
}

static void screen_usb(void)
{
    extern bool     dap_usb_is_started(void);
    extern uint32_t dap_usb_configured_count(void);
    extern uint32_t dap_usb_get_rx_packets(void);
    extern uint32_t dap_usb_get_tx_packets(void);

    static const char *mode_s[3] = { "off", "probe", "serial" };
    char v[24], dbg[24];
    page_header("USB Role");

    uint8_t m = pin_config_usb_mode();
    snprintf(v, sizeof(v), "%s cfg%u", mode_s[m <= USB_MODE_TTL ? m : 0],
             (unsigned)dap_usb_configured_count());
    row_kv(ROW1_Y, "Role", v);

    snprintf(v, sizeof(v), "%lu/%lu", (unsigned long)dap_usb_get_rx_packets(),
             (unsigned long)dap_usb_get_tx_packets());
    row_kv(ROW2_Y, "DAPio", v);

    debug_pins_report(dbg, sizeof(dbg));
    footer(debug_pins_claimed() ? dbg : "no debug IO");
}

static void screen_swd(void)
{
    char v[24];
    page_header("SWD / JTAG");

    row_kv(ROW1_Y, "SWD", "12/13/14");
    snprintf(v, sizeof(v), "48/38/39 swo40");
    row_kv(ROW2_Y, "JTAG", v);

    if (s_swd_idcode_valid) snprintf(v, sizeof(v), "ID 0x%08lX", (unsigned long)s_swd_idcode);
    else                    snprintf(v, sizeof(v), "ID fail ack%d", s_swd_last_err);
    footer(v);
}

static void screen_pwm(void)
{
    char v[24];
    float f = 0.0f, d = 0.0f;
    page_header("PWM");

    pwm_mon_get(&f, &d);
    snprintf(v, sizeof(v), "%.1fHz %.1f%%", (double)f, (double)d);
    row_kv(ROW1_Y, "Input", v);

    if (pwm_out_running()) {
        pwm_out_get(&f, &d);
        snprintf(v, sizeof(v), "%.0fHz %.0f%%", (double)f, (double)d);
    } else {
        snprintf(v, sizeof(v), "stopped");
    }
    row_kv(ROW2_Y, "Output", v);

    footer("SW2=back");
}

static void screen_spi(void)
{
    char v[24], line[TEXT_COLS + 1];
    page_header("SPI Bus");

    snprintf(v, sizeof(v), "%lu  to%lu", (unsigned long)spi_mon_get_count(),
             (unsigned long)spi_mon_get_timeouts());
    row_kv(ROW1_Y, "Count", v);
    row_kv(ROW2_Y, "State", spi_mon_running() ? "capturing" : "stopped");

    /* Newest captured transaction, hex, truncated to the panel. */
    static spi_txn_t h[1];
    if (spi_mon_get_history(h, 1) == 1 && h[0].len > 0) {
        int p = 0;
        for (int i = 0; i < h[0].len && p < 12; i++)
            p += snprintf(line + p, sizeof(line) - p, "%02X", h[0].mosi[i]);
        footer(line);
    } else {
        footer("no transactions");
    }
}

static void screen_i2c(void)
{
    char v[24], line[TEXT_COLS + 1];
    page_header("I2C Bus");

    snprintf(v, sizeof(v), "%lu  isr%lu", (unsigned long)i2c_mon_get_count(),
             (unsigned long)i2c_mon_get_isr_count());
    row_kv(ROW1_Y, "Count", v);
    row_kv(ROW2_Y, "Mode", i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive");

    i2c_txn_t h[1];
    if (i2c_mon_get_history(h, 1) == 1) {
        snprintf(line, sizeof(line), "a%02X %s len%d", h[0].addr,
                 h[0].read ? "R" : "W", h[0].len);
        footer(line);
    } else {
        footer("no transactions");
    }
}

static void screen_capture(void)
{
    char v[24];
    page_header("Capture Log");

    snprintf(v, sizeof(v), "%u/%u", (unsigned)capture_count(),
             (unsigned)capture_capacity());
    row_kv(ROW1_Y, "Chunks", v);

    snprintf(v, sizeof(v), "%lu..%lu", (unsigned long)capture_oldest_seq(),
             (unsigned long)capture_next_seq());
    row_kv(ROW2_Y, "Seq", v);

    snprintf(v, sizeof(v), "%s %uB", capture_dropped() ? "WRAPPED" : "ok",
             (unsigned)capture_bytes());
    footer(v);
}

static void screen_ai(void)
{
    char ip[16] = "-";
    page_header("AI / MCP");

    wifi_state_t st = wifi_manager_get_state();
    bool net = (st == WIFI_STATE_CONNECTED_STA || st == WIFI_STATE_AP_MODE);
    wifi_manager_get_ip_str(ip, sizeof(ip));

    row_kv(ROW1_Y, "MCP", net ? "ready" : "no net");
    row_kv(ROW2_Y, "Tools", "24 over HTTP");
    footer(ip);
}

/* Live serial monitor: the only screen with a scrollable body. Uses all three
 * content rows, so its hint replaces the third row when scrolling. */
static void screen_rx(void)
{
    page_header("RX Monitor");

    int n = s_rx_line_n;
    if (n > s_rx_hist_max) n = s_rx_hist_max;

    /* s_scroll counts lines back from the newest (0 = newest visible). */
    int start = n - BODY_ROWS - s_scroll;
    if (start < 0) start = 0;

    for (int row = 0; row < BODY_ROWS; row++) {
        int idx = start + row;
        if (idx >= n - s_scroll) break;
        int ring = ((s_rx_line_w - (n - 1) + idx) % s_rx_hist_max + s_rx_hist_max) % s_rx_hist_max;
        char line[TEXT_COLS + 1];
        snprintf(line, sizeof(line), "%-*.*s", TEXT_COLS, TEXT_COLS, s_rx_lines[ring]);
        oled_text(0, ROW1_Y + row * 16, line, false);
    }

    char hint[TEXT_COLS + 1];
    if (s_scroll) snprintf(hint, sizeof(hint), "SCR%d  SW2=back", s_scroll);
    else          snprintf(hint, sizeof(hint), "%s  SW2=back",
                          s_rx_hex_mode ? "HEX" : "TXT");
    footer(hint);
}

/* ================================================================== */
/*  Rendering                                                          */
/* ================================================================== */

static void render_list(void)
{
    const menu_list_t *l = s_stack[s_depth - 1];
    int cur = s_cursor[s_depth - 1];

    page_header(l->title);

    /* Keep the cursor visible. */
    int first = 0;
    if (cur >= BODY_ROWS) first = cur - BODY_ROWS + 1;

    for (int row = 0; row < BODY_ROWS; row++) {
        int idx = first + row;
        if (idx >= l->count) break;
        char line[20];
        snprintf(line, sizeof(line), "%c%-*.*s", idx == cur ? '>' : ' ',
                 TEXT_COLS - 1, TEXT_COLS - 1, l->items[idx].label);
        oled_text(0, ROW1_Y + row * 16, line, false);
    }

    /* Buffer wider than the panel on purpose: -Werror=format-truncation needs
     * to prove the formatted text fits, and "%d/%d  SW2=ok" can be 14 columns
     * for a 9-item list, which leaves no slack in a 17-byte buffer. footer()
     * clips to 16 columns for display. */
    char hint[32];
    snprintf(hint, sizeof(hint), "%d/%d  SW2=ok", cur + 1, l->count);
    footer(hint);
}

static void render_edit(void)
{
    const menu_entry_t *e = s_edit;
    char line[TEXT_COLS + 1];
    char v[24];

    page_header(e->label);

    /* The ">" marker matches how a list shows its cursor, so "this is the item
     * you are changing" reads the same everywhere. (An inverted band was tried
     * first and is not usable here: oled_invert_rect() XORs the framebuffer, so
     * filling black and then inverting leaves the band exactly as it was.) */
    int idx = *e->u.val.idx;
    snprintf(v, sizeof(v), "%d %s", e->u.val.table[idx], e->u.val.unit);
    snprintf(line, sizeof(line), ">%-*.*s", TEXT_COLS - 1, TEXT_COLS - 1, v);
    oled_text(0, ROW1_Y, line, false);

    centered(ROW2_Y, "UP/DOWN changes it");
    footer("SW2=confirm");
}

static void render_screen(void)
{
    switch (s_screen_page) {
    case MENU_RX_MON:    screen_rx();       break;
    case MENU_STATUS:    screen_status();   break;
    case MENU_NET:       screen_net();      break;
    case MENU_FIRMWARE:  screen_firmware(); break;
    case MENU_USB_STATE: screen_usb();      break;
    case MENU_SWD:       screen_swd();      break;
    case MENU_PWM:       screen_pwm();      break;
    case MENU_SPI:       screen_spi();      break;
    case MENU_I2C:       screen_i2c();      break;
    case MENU_CAPTURE:   screen_capture();  break;
    case MENU_AI:        screen_ai();       break;
    default:
        page_header("?");
        footer("SW2=back");
        break;
    }
}

/* ================================================================== */
/*  Input                                                              */
/* ================================================================== */

static void nav_vertical(int dir)
{
    /* Editing: change the value. */
    if (edit_active()) {
        const menu_entry_t *e = s_edit;
        s_edit_ts = now_s();
        int n = e->u.val.count;
        int idx = ((int)*e->u.val.idx + dir + n) % n;
        *e->u.val.idx = (int8_t)idx;
        if (e->u.val.apply) e->u.val.apply(e->u.val.table[idx]);
        ui_kick();
        return;
    }

    /* On a screen only the RX monitor has anything to scroll. */
    if (s_view == VIEW_SCREEN) {
        if (s_screen_page == MENU_RX_MON) {
            int max = s_rx_line_n - BODY_ROWS;
            if (max < 0) max = 0;
            s_scroll += dir;
            if (s_scroll > max) s_scroll = max;
            if (s_scroll < 0) s_scroll = 0;
            ui_kick();
        }
        return;
    }

    /* In a list: move the cursor. */
    const menu_list_t *l = s_stack[s_depth - 1];
    int cur = ((s_cursor[s_depth - 1] + dir) % l->count + l->count) % l->count;
    s_cursor[s_depth - 1] = cur;
    s_selected = cur;
    ui_kick();
}

void menu_on_sw1_press(void) { nav_vertical(-1); }
void menu_on_sw3_press(void) { nav_vertical(+1); }

void menu_on_sw2_press(void)
{
    /* 1. Editing: confirm. A value entry lives in a list, so returning to
     * VIEW_LIST redraws that list with the new value in place. */
    if (edit_active()) {
        s_view = VIEW_LIST;
        s_edit = NULL;
        ui_kick();
        return;
    }

    /* 2. A read-only screen: back to the list it was opened from. */
    if (s_view == VIEW_SCREEN) {
        s_view = VIEW_LIST;
        s_scroll = 0;
        ui_kick();
        return;
    }

    /* 3. In a list.
     *
     * At the ROOT, SW2 activates the highlighted entry. Inside a GROUP, SW2
     * goes back up one level - without that the tree would be enter-only and
     * the long-press escape would be the only way home.
     *
     * These two must not be swapped: an earlier revision checked
     * "s_depth > 1" first, which made every second-level press "back", so no
     * entry inside a group could ever be activated. */
    if (s_depth > 1) {
        s_depth--;
        s_selected = s_cursor[s_depth - 1];
        ui_kick();
        return;
    }

    const menu_list_t *l = s_stack[s_depth - 1];
    const menu_entry_t *e = &l->items[s_cursor[s_depth - 1]];

    switch (e->kind) {
    case ENTRY_LIST:
        if (s_depth < STACK_MAX) {
            s_stack[s_depth]  = e->u.list;
            s_cursor[s_depth] = 0;
            s_depth++;
            s_selected = 0;
        }
        break;

    case ENTRY_SCREEN:
        s_screen_page = e->u.page;
        s_scroll = 0;
        s_view = VIEW_SCREEN;
        /* The cursor belongs to a list; a screen has none. Leaving the list's
         * index in s_selected made /api/status report a meaningless "sel"
         * while a screen was up. */
        s_selected = 0;
        break;

    case ENTRY_ACTION:
        if (e->u.fn) e->u.fn();
        break;

    case ENTRY_VALUE:
        s_edit = e;
        s_edit_ts = now_s();
        s_view = VIEW_EDIT;
        break;
    }

    if (s_view == VIEW_LIST) s_selected = s_cursor[s_depth - 1];
    else if (s_view == VIEW_SCREEN) s_selected = 0;
    ui_kick();
}

void menu_on_sw2_long_press(void)
{
    /* Universal escape: drop the whole stack, cancel any edit or scroll. This
     * is the one gesture that can never be ambiguous. */
    s_depth = 1;
    s_cursor[0] = 0;
    s_selected = 0;
    s_view = VIEW_LIST;
    s_scroll = 0;
    s_edit = NULL;
    ui_kick();
}

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

/* ================================================================== */
/*  Task plumbing                                                      */
/* ================================================================== */

void menu_init(void)
{
    s_depth = 1;
    s_stack[0] = &list_root;
    s_cursor[0] = 0;
    s_selected = 0;
    s_view = VIEW_LIST;
    s_scroll = 0;
    s_screen_page = MENU_HOME;
    s_edit = NULL;
    s_anim_pending = false;

    /* Seed the editors from live state, so the first visit shows the truth
     * instead of a hard-coded default. */
    uint32_t b = 0;
    uart_get_baudrate(UART1_PORT_NUM, &b);
    s_baud_idx = 1;
    for (int i = 0; i < N_BAUD; i++)
        if ((uint32_t)val_bauds[i] == b) s_baud_idx = (int8_t)i;
    s_bright_idx = 4;
    s_buf_idx    = 1;
    s_shist_idx  = 2;
    s_ihist_idx  = 2;
    s_rhist_idx  = 2;

    menu_clear_rx();
    if (!s_kick)     s_kick     = xSemaphoreCreateBinary();
    if (!s_nav_lock) s_nav_lock = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "menu ready: 4 groups, SW1/SW3=up/down, SW2=enter/back");
}

void menu_ui_wait(int ms)
{
    if (!s_kick) { vTaskDelay(pdMS_TO_TICKS(ms)); return; }
    xSemaphoreTake(s_kick, pdMS_TO_TICKS(ms));
}

void menu_render(void)
{
    /* Only the UI task reaches here, so the panel has a single writer. */
    switch (s_view) {
    case VIEW_EDIT:   render_edit();   break;
    case VIEW_SCREEN: render_screen(); break;
    case VIEW_LIST:
    default:          render_list();   break;
    }
    oled_flush();
}

int menu_get_current_page(void)
{
    if (s_view == VIEW_SCREEN) return s_screen_page;
    if (s_depth <= 1) return MENU_HOME;
    if (s_stack[1] == &list_probe)  return MENU_LIST_PROBE;
    if (s_stack[1] == &list_system) return MENU_LIST_SYSTEM;
    if (s_stack[1] == &list_info)   return MENU_LIST_INFO;
    return MENU_LIST_MONITOR;
}

int menu_get_selected(void) { return s_selected; }

void menu_get_state(char *buf, unsigned len)
{
    if (!buf || !len) return;

    const char *view = (s_view == VIEW_EDIT)   ? "EDIT"   :
                       (s_view == VIEW_SCREEN) ? "SCREEN" : "LIST";
    const char *where;
    if (s_view == VIEW_SCREEN)      where = "screen";
    else if (s_view == VIEW_EDIT)   where = s_edit ? s_edit->label : "?";
    else                            where = s_stack[s_depth - 1]->title;

    snprintf(buf, len, "%s %s d%d sel=%d", view, where, s_depth, s_selected);
}