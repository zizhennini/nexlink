#include "debug_pins.h"

#include "pinout.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/gpio.h"
/* spi_mon.h / i2c_mon.h expose the stop APIs used below; the SPI and I2C
 * drivers themselves are not called directly any more (bus release is the
 * monitor's own job), so driver/spi_master.h is deliberately not included. */
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
 * Order matters: the owner has to close its driver first (an SPI or I2C driver
 * keeps routing the pin through the GPIO matrix), and only then can
 * gpio_reset_pin() detach the pad cleanly.
 *
 * Both SPI personalities have to be checked, and each has to be stopped
 * through its own API - they do not share a bus handle lifetime:
 *   capture  : spi_slave_initialize() ... spi_slave_free()   (s_running)
 *   user tool: spi_bus_initialize()   ... spi_bus_free()     (s_send_running)
 * Calling spi_bus_free() directly while the user-tool master owns the bus was
 * a bug: it releases the peripheral but leaves s_send_running true, so the
 * next spi_send_stop() would free the bus a second time. spi_send_stop()
 * clears the flag as well, so it is the only call made here.
 * (spi_mon_stop() already stops the user-tool master itself when it is up.) */
static void release_io(int io)
{
    const bool is_spi = (io == pin_config_spi_sck()  || io == pin_config_spi_mosi() ||
                         io == pin_config_spi_miso() || io == pin_config_spi_cs());
    const bool is_i2c = (io == pin_config_i2c_sda()  || io == pin_config_i2c_scl());

    if (is_spi) {
        bool was = false;
        if (spi_mon_running()) {
            ESP_LOGW(TAG, "expansion IO%d was the SPI monitor - stopping it for the debug probe", io);
            spi_mon_stop();
            was = true;
        }
        if (spi_send_running()) {
            ESP_LOGW(TAG, "expansion IO%d was the SPI user-tool master - stopping it for the debug probe", io);
            spi_send_stop();
            was = true;
        }
        if (!was && !spi_mon_running() && !spi_send_running())
            ESP_LOGD(TAG, "expansion IO%d was assigned to SPI but no SPI owner was running", io);
    } else if (is_i2c) {
        if (i2c_mon_running()) {
            ESP_LOGW(TAG, "expansion IO%d was the I2C monitor - stopping it for the debug probe", io);
            i2c_mon_stop();
        }
        /* The monitor's slave device and the user-tool master bus are separate
         * handles on I2C_NUM_1: i2c_mon_stop() released the slave above, and
         * i2c_send_stop() releases the master bus (a no-op when it never
         * started, which is why it is called unconditionally). */
        i2c_send_stop();
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
        /* Guard against a pinout.h edit that points a debug signal at a pin the
         * board cannot give up. A plain `io > 48` range check would happily
         * accept IO19/IO20 (native USB) or IO26..IO37 (flash / octal PSRAM),
         * i.e. exactly the pins that must never be taken - so the check is
         * expressed as "must be one of the expansion IOs" instead. */
        bool is_expansion = (io == PIN_FREE_1 || io == PIN_FREE_2 ||
                             io == PIN_FREE_3 || io == PIN_FREE_4 ||
                             io == PIN_FREE_5);
        if (!is_expansion) {
            ESP_LOGE(TAG, "debug IO%d is not an expansion IO (IO%d/%d/%d/%d/%d) - "
                          "debug probe disabled",
                     io, PIN_FREE_1, PIN_FREE_2, PIN_FREE_3, PIN_FREE_4, PIN_FREE_5);
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
