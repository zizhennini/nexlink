#include "debug_pins.h"

#include "pinout.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "pin_config.h"

static const char *TAG = "dbgpin";

/* The debug probe's IOs, in the order they are reported. Everything here is
 * compile-time fixed (pinout.h); runtime remapping is deliberately not offered
 * because the JTAG pins are bit-banged with direct register writes. */
static const int DEBUG_IO[] = {
    PIN_DEBUG_TDI, PIN_DEBUG_TDO, PIN_DEBUG_NTRST, PIN_DEBUG_SWO,
};
#define DEBUG_IO_N (sizeof(DEBUG_IO) / sizeof(DEBUG_IO[0]))

static bool s_claimed;

bool debug_pins_is_debug_io(int io)
{
    for (size_t i = 0; i < DEBUG_IO_N; i++)
        if (DEBUG_IO[i] == io) return true;
    return false;
}

int debug_pins_tdi(void)   { return PIN_DEBUG_TDI; }
int debug_pins_tdo(void)   { return PIN_DEBUG_TDO; }
int debug_pins_ntrst(void) { return PIN_DEBUG_NTRST; }
int debug_pins_swo(void)   { return PIN_DEBUG_SWO; }

bool debug_pins_claimed(void) { return s_claimed; }

void debug_pins_report(char *buf, unsigned len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';
    unsigned used = 0;

    if (!s_claimed) {
        strlcpy(buf, "none", len);
        return;
    }

    for (size_t i = 0; i < DEBUG_IO_N; i++) {
        int n = snprintf(buf + used, len - used, "%s%d", i ? "," : "", DEBUG_IO[i]);
        if (n < 0 || (unsigned)n >= len - used) break;
        used += (unsigned)n;
    }
}

/* Release a GPIO from whatever peripheral/monitor currently owns it.
 *
 * Order matters: the monitor has to be told to close its driver first (an SPI
 * or I2C driver keeps routing the pin through the GPIO matrix), and only then
 * can gpio_reset_pin() detach the pad cleanly. */
static void release_io(int io)
{
    const bool is_spi = (io == pin_config_spi_sck()  || io == pin_config_spi_mosi() ||
                         io == pin_config_spi_miso() || io == pin_config_spi_cs());
    const bool is_i2c = (io == pin_config_i2c_sda()  || io == pin_config_i2c_scl());

    if (is_spi) {
        if (spi_mon_running()) {
            ESP_LOGW(TAG, "expansion IO%d was the SPI monitor - stopping it for the debug probe", io);
            spi_mon_stop();
        }
        /* spi_mon_stop() already frees SPI3_HOST; if the user-tool master
         * (/api/spi/send) happens to own it instead, free it here so the pad
         * is not still routed through the SPI matrix. A free on a bus that is
         * not initialised just returns an error, which is fine. */
        if (!spi_mon_running()) spi_bus_free(SPI3_HOST);
    } else if (is_i2c) {
        if (i2c_mon_running()) {
            ESP_LOGW(TAG, "expansion IO%d was the I2C monitor - stopping it for the debug probe", io);
            i2c_mon_stop();
        }
        /* The monitor's slave device and the user-tool master bus are two
         * separate handles (I2C_NUM_1); i2c_send_stop() releases the master
         * bus, and i2c_mon_stop() released the slave above. */
        if (!i2c_mon_running()) i2c_send_stop();
    }

    gpio_reset_pin((gpio_num_t)io);
    gpio_set_direction((gpio_num_t)io, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)io, GPIO_FLOATING);
}

int debug_pins_init(void)
{
    if (s_claimed) return (int)DEBUG_IO_N;

    for (size_t i = 0; i < DEBUG_IO_N; i++) {
        int io = DEBUG_IO[i];
        /* Config protection: these are strapping-adjacent pins on some boards
         * and are always expansion IOs. Guard against a pinout.h edit that
         * accidentally points at a dedicated pin (USB, flash, PSRAM, OLED...). */
        if (io < 0 || io > 48) {
            ESP_LOGE(TAG, "debug IO%d out of range - debug probe disabled", io);
            return 0;
        }
        release_io(io);
    }

    /* Idle state: inputs, no pull. The DAP core latches its own direction in
     * dap_pads_init()/PORT_*_SETUP(), and an unconfigured pad must not drive
     * the target before a session starts. */
    s_claimed = true;

    char list[48];
    debug_pins_report(list, sizeof(list));
    ESP_LOGI(TAG, "debug probe owns expansion IO %s (TDI=%d TDO=%d nTRST=%d SWO=%d)",
             list, PIN_DEBUG_TDI, PIN_DEBUG_TDO, PIN_DEBUG_NTRST, PIN_DEBUG_SWO);

    return (int)DEBUG_IO_N;
}
