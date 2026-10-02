/*
 * pin_config.c - NVS-backed protocol assignment for the five FREE IOs.
 *
 * Board layout (see pinout.h):
 *
 *   Fixed dedicated buses - never permuted:
 *     UART1  TX=IO47  RX=IO21        (DUT serial bridge)
 *     SWD    SWCLK=IO13 SWDIO=IO14 NRST=IO12   (CMSIS-DAP probe)
 *     I2C0   SCL=IO10 SDA=IO11       (SSD1306 OLED)
 *
 *   Five free IOs - protocol assignment is permutable and saved to NVS:
 *     slot0 -> IO48
 *     slot1 -> IO45
 *     slot2 -> IO38
 *     slot3 -> IO39
 *     slot4 -> IO40
 *
 * Because SWD now lives on its own dedicated pins, it is always available and
 * no longer has to fight the permutation set for a slot.
 */
#include "pin_config.h"
#include "pinout.h"
#include <string.h>
#include <strings.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "pinconf";

#define NVS_NS    "pinconf"
#define KEY_ORDER "order"

/* Fixed PCB routing: free-IO slot -> ESP GPIO. */
static const int POS_IO[PIN_NUM_SIGNAL_SLOTS] = {
    PIN_FREE_1, PIN_FREE_2, PIN_FREE_3, PIN_FREE_4, PIN_FREE_5,
};

/* Default function-per-slot: a 3-wire SPI + I2C combination, which is the most
 * common bench setup. PWM is available at the cost of one of them. */
static const pin_kind_t DEFAULT_ORDER[PIN_NUM_SIGNAL_SLOTS] = {
    PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_I2C_SDA, PIN_I2C_SCL,
};

static pin_kind_t s_order[PIN_NUM_SIGNAL_SLOTS];

bool pin_config_kind_is_permutable(pin_kind_t k)
{
    return k == PIN_PWM      || k == PIN_GPIO     ||
           k == PIN_SPI_SCK  || k == PIN_SPI_MOSI || k == PIN_SPI_MISO ||
           k == PIN_SPI_CS   || k == PIN_I2C_SDA  || k == PIN_I2C_SCL;
}

static bool order_is_valid(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS])
{
    bool seen[PIN_KIND_COUNT] = { false };
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        if (!pin_config_kind_is_permutable(order[i])) return false;
        if (seen[order[i]]) return false;
        seen[order[i]] = true;
    }
    return true;
}

void pin_config_init(void)
{
    memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved pin config; using defaults");
    } else {
        size_t len = sizeof(s_order);
        if (nvs_get_blob(h, KEY_ORDER, s_order, &len) != ESP_OK || len != sizeof(s_order)) {
            memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));
        }
        nvs_close(h);

        if (!order_is_valid(s_order)) {
            ESP_LOGW(TAG, "saved order invalid, reverting to defaults");
            memcpy(s_order, DEFAULT_ORDER, sizeof(s_order));
        }
    }

    /* Report both groups so the console log shows the full picture. */
    ESP_LOGI(TAG, "fixed bus: SWCLK=IO%d SWDIO=IO%d NRST=IO%d TX=IO%d RX=IO%d SCL=IO%d SDA=IO%d",
             PIN_SWD_SWCLK, PIN_SWD_SWDIO, PIN_SWD_NRST,
             PIN_UART1_TX, PIN_UART1_RX, PIN_OLED_SCL, PIN_OLED_SDA);
    ESP_LOGI(TAG, "free IO map: IO%d=%s IO%d=%s IO%d=%s IO%d=%s IO%d=%s",
             POS_IO[0], pin_config_kind_label(s_order[0]),
             POS_IO[1], pin_config_kind_label(s_order[1]),
             POS_IO[2], pin_config_kind_label(s_order[2]),
             POS_IO[3], pin_config_kind_label(s_order[3]),
             POS_IO[4], pin_config_kind_label(s_order[4]));
}

static esp_err_t save_order(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, KEY_ORDER, s_order, sizeof(s_order));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

pin_kind_t pin_config_pos_func(int pos)
{
    if (pos < 0 || pos >= PIN_NUM_SIGNAL_SLOTS) return PIN_KIND_COUNT;
    return s_order[pos];
}

int pin_config_pos_io(int pos)
{
    if (pos < 0 || pos >= PIN_NUM_SIGNAL_SLOTS) return -1;
    return POS_IO[pos];
}

int pin_config_func_io(pin_kind_t func)
{
    /* Fixed dedicated signals resolve straight to their hardware pins. */
    switch (func) {
    case PIN_SWCLK: return PIN_SWD_SWCLK;
    case PIN_SWDIO: return PIN_SWD_SWDIO;
    case PIN_NRST:  return PIN_SWD_NRST;
    case PIN_TX:    return PIN_UART1_TX;
    case PIN_RX:    return PIN_UART1_RX;
    default: break;
    }
    /* Permutable functions come from the free-IO assignment. */
    for (int p = 0; p < PIN_NUM_SIGNAL_SLOTS; p++) {
        if (s_order[p] == func) return POS_IO[p];
    }
    return -1;
}

int pin_config_swclk(void) { return PIN_SWD_SWCLK; }
int pin_config_swdio(void) { return PIN_SWD_SWDIO; }
int pin_config_nrst(void)  { return PIN_SWD_NRST; }
int pin_config_tx(void)    { return PIN_UART1_TX; }
int pin_config_rx(void)    { return PIN_UART1_RX; }

int pin_config_pwm(void)      { return pin_config_func_io(PIN_PWM); }
int pin_config_spi_sck(void)  { return pin_config_func_io(PIN_SPI_SCK); }
int pin_config_spi_mosi(void) { return pin_config_func_io(PIN_SPI_MOSI); }
int pin_config_spi_miso(void) { return pin_config_func_io(PIN_SPI_MISO); }
int pin_config_spi_cs(void)   { return pin_config_func_io(PIN_SPI_CS); }
int pin_config_i2c_sda(void)  { return pin_config_func_io(PIN_I2C_SDA); }
int pin_config_i2c_scl(void)  { return pin_config_func_io(PIN_I2C_SCL); }

esp_err_t pin_config_set_signal_order(const pin_kind_t order[PIN_NUM_SIGNAL_SLOTS])
{
    if (!order_is_valid(order)) {
        ESP_LOGW(TAG, "invalid signal order");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_order, order, sizeof(s_order));
    return save_order();
}

/* Map a protocol name (as used by the HTTP API / MCP tool) to a kind.
 * Accepts the short labels too so both "SPI_SCK" and "SCK" work. */
static pin_kind_t kind_from_name(const char *name)
{
    if (!name || !*name) return PIN_KIND_COUNT;
    if (!strcasecmp(name, "PWM"))    return PIN_PWM;
    if (!strcasecmp(name, "GPIO"))   return PIN_GPIO;
    if (!strcasecmp(name, "SCK"))    return PIN_SPI_SCK;
    if (!strcasecmp(name, "SPI_SCK"))return PIN_SPI_SCK;
    if (!strcasecmp(name, "MOSI"))   return PIN_SPI_MOSI;
    if (!strcasecmp(name, "SPI_MOSI")) return PIN_SPI_MOSI;
    if (!strcasecmp(name, "MISO"))   return PIN_SPI_MISO;
    if (!strcasecmp(name, "SPI_MISO")) return PIN_SPI_MISO;
    if (!strcasecmp(name, "CS"))     return PIN_SPI_CS;
    if (!strcasecmp(name, "SPI_CS")) return PIN_SPI_CS;
    if (!strcasecmp(name, "SDA"))    return PIN_I2C_SDA;
    if (!strcasecmp(name, "I2C_SDA"))return PIN_I2C_SDA;
    if (!strcasecmp(name, "SCL"))    return PIN_I2C_SCL;
    if (!strcasecmp(name, "I2C_SCL"))return PIN_I2C_SCL;
    /* Fixed signals may be *displayed* but cannot be assigned to a free IO. */
    return PIN_KIND_COUNT;
}

esp_err_t pin_config_parse_order(const char *text, pin_kind_t out[PIN_NUM_SIGNAL_SLOTS])
{
    if (!text || !out) return ESP_ERR_INVALID_ARG;

    char buf[128];
    size_t n = strlen(text);
    if (n >= sizeof(buf)) return ESP_ERR_INVALID_ARG;
    memcpy(buf, text, n + 1);

    int slot = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && slot < PIN_NUM_SIGNAL_SLOTS;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';

        pin_kind_t k = kind_from_name(tok);
        if (k == PIN_KIND_COUNT) return ESP_ERR_INVALID_ARG;
        out[slot++] = k;
    }

    if (slot != PIN_NUM_SIGNAL_SLOTS) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

const char *pin_config_kind_label(pin_kind_t k)
{
    switch (k) {
    case PIN_5V:    return "5V";
    case PIN_GND:   return "GND";
    case PIN_DUT:   return "DUT";
    case PIN_SWCLK: return "SWCLK";
    case PIN_SWDIO: return "SWDIO";
    case PIN_NRST:  return "NRST";
    case PIN_TX:    return "TX";
    case PIN_RX:    return "RX";
    case PIN_PWM:   return "PWM";
    case PIN_SPI_SCK:  return "SCK";
    case PIN_SPI_MOSI: return "MOSI";
    case PIN_SPI_MISO: return "MISO";
    case PIN_SPI_CS:   return "CS";
    case PIN_I2C_SDA:  return "SDA";
    case PIN_I2C_SCL:  return "SCL";
    case PIN_GPIO:     return "GPIO";
    default:        return "?";
    }
}

/* ---- USB role for the USB-C port (persisted; default OFF) ----
 * The native USB peripheral and IO19/IO20 are a single PHY, so exactly one
 * personality can own them at a time:
 *   0 OFF : release them -> USB-Serial-JTAG (flashing the board itself)
 *   1 DAP : CMSIS-DAP v2 probe for Cortex-M targets (dap_usb.c)
 *   2 TTL : CDC-ACM virtual COM bridged to UART1 for serial flashing
 *           (usb_ttl.c)
 * The stored value is the same u8 key it always was; old "1 = DAP" values
 * carry over unchanged, so no migration is needed. */
#define KEY_USBROLE "usbdap"

uint8_t pin_config_usb_mode(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return USB_MODE_OFF;
    uint8_t v = USB_MODE_OFF;
    esp_err_t e = nvs_get_u8(h, KEY_USBROLE, &v);
    nvs_close(h);
    if (e != ESP_OK || v > USB_MODE_TTL) return USB_MODE_OFF;
    return v;
}

esp_err_t pin_config_set_usb_mode(uint8_t mode)
{
    if (mode > USB_MODE_TTL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, KEY_USBROLE, mode);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool pin_config_usb_dap(void)
{
    return pin_config_usb_mode() == USB_MODE_DAP;
}

esp_err_t pin_config_set_usb_dap(bool on)
{
    /* Legacy bool wrapper used by the off<->dap toggle: enabling selects DAP,
     * disabling clears whatever the port was doing. */
    return pin_config_set_usb_mode(on ? USB_MODE_DAP : USB_MODE_OFF);
}
