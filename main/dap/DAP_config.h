/*
 * CMSIS-DAP configuration for the AI Wireless Debugger (ESP32-S3-WROOM-1-N16R8)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the CMSIS-DAP DAP_config.h template (ARM Limited, Apache-2.0)
 * and from CherryDAP's ESP32-S3 port (cherry-embedded/CherryDAP, Apache-2.0).
 *
 * Differences from the CherryDAP version, on purpose:
 *   - GNU/Linux-hosted ESP-IDF instead of CherryUSB's own port layer: the GPIO
 *     accessors are ESP-IDF based, and the one-time pad configuration goes
 *     through the ESP-IDF gpio driver rather than a board-specific pins_init().
 *   - The pin map comes from ../pinout.h so there is exactly one place in the
 *     firmware that knows which pad is SWCLK / SWDIO / nRESET.
 *   - nRESET is real here (IO12).  CherryDAP compiles it out; we want it so the
 *     host can actually reset the target.
 */

#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include <stdint.h>
#include <string.h>

#include "pinout.h"          /* PIN_SWD_SWCLK / PIN_SWD_SWDIO / PIN_SWD_NRST */
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"         /* REG_WRITE / REG_READ */
#include "esp_timer.h"       /* TIMESTAMP_GET */
#include "esp_mac.h"         /* DAP_GetSerNumString */

//**************************************************************************************************
// CMSIS compiler abstraction
//
// DAP.h and SW_DP.c are written against CMSIS Core's cmsis_compiler.h, which
// does not exist for Xtensa.  Only these three are actually needed.
//**************************************************************************************************

#ifndef __STATIC_INLINE
#define __STATIC_INLINE                     static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE                __attribute__((always_inline)) static inline
#endif
#ifndef __WEAK
#define __WEAK                              __attribute__((weak))
#endif
#ifndef __NOP
#define __NOP()                             __asm__ __volatile__("nop")
#endif

//**************************************************************************************************
/**
\defgroup DAP_Config_Debug_gr CMSIS-DAP Debug Unit Information
@{
*/

/// Processor Clock of the MCU used in the Debug Unit.
/// Only used to convert a requested SWJ clock into a PIN_DELAY_SLOW() loop
/// count, and to convert a requested delay in us/ms into the same loop count.
/// ESP32-S3 runs at 240 MHz out of reset; if CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ is
/// changed, adjust this too or the SWD clock will be off by that factor.
#define CPU_CLOCK               240000000U

/// Number of processor cycles a single I/O port write costs.
/// On Xtensa a store to GPIO_OUT_W1TS/W1TC plus the loop overhead lands around
/// 4 cycles.  It is subtracted from the computed delay so the two halves of an
/// SWD clock cycle come out roughly equal.
#define IO_PORT_WRITE_CYCLES    4U

/// Serial Wire Debug is the whole point of this build.
#define DAP_SWD                 1

/// JTAG is supported in addition to SWD. TCK/TMS are the same pads as
/// SWCLK/SWDIO (as on any SWJ-DP target); TDI/TDO/nTRST come from the
/// expansion header, since the board has no spare dedicated pins for them.
/// The host chooses the port at runtime with DAP_Connect (DAP_PORT_SWD or
/// DAP_PORT_JTAG), so enabling this costs nothing for an SWD-only session.
///
/// DAP_JTAG_DEV_CNT is the length of the multi-device scan-chain descriptor:
/// 1..8 entries for DAP_JTAG_Configure. Keep it small - each entry is 6 bytes
/// of DAP_Info state that most hosts never use.
#define DAP_JTAG                1
#define DAP_JTAG_DEV_CNT        4U

/// Port selected by DAP_Connect when the host asks for "default".
#define DAP_DEFAULT_PORT        DAP_PORT_SWD

/// Initial SWJ clock in Hz; DAP_SWJ_Clock can change it at runtime.
/// Kept moderate: the SWD pins are bit-banged by the CPU, so the ceiling is
/// the GPIO toggle loop plus lead/jumper capacitance, not the target.
#define DAP_DEFAULT_SWJ_CLOCK   1000000U

/// Maximum packet size.  ESP32-S3's USB-OTG peripheral is full speed, so the
/// endpoint packet size is 64 bytes; the WinUSB CMSIS-DAP v2 protocol is built
/// on top of that.
#define DAP_PACKET_SIZE         64U

/// One packet is enough: the transport queues packets on the host side.
#define DAP_PACKET_COUNT        1U

/// SWO / trace: UART (asynchronous) mode, implemented on an ESP32-S3 UART.
///
/// The target's SWO pin is a single wire carrying UART-framed ITM data, so the
/// host asks for DAP_SWO_UART and this side just needs a UART RX. Manchester
/// mode is not offered - it would need the RMT peripheral and ASYNC mode is
/// what every current debug probe uses anyway.
///
/// SWO_UART_DRIVER selects the CMSIS USART instance in ARM's SWO.c; here it
/// selects the ESP32 UART port number (see main/swo_uart.c). UART2 is free:
/// UART0 is the console and UART1 is the DUT bridge.
#define SWO_UART                1
#define SWO_UART_DRIVER         2
/// The SWO wire is on the expansion header (PIN_DEBUG_SWO in pinout.h) because
/// the dedicated pins are fully used; debug_pins.c arbitrates ownership.
#define SWO_UART_MAX_BAUDRATE   4000000U
#define SWO_MANCHESTER          0
/// Streaming trace (bulk endpoint) is off: the host polls ID_DAP_SWO_Data,
/// which is what pyOCD/OpenOCD do for SWO.
#define SWO_STREAM              0
/// Must stay a power of two (the index arithmetic relies on it).
#define SWO_BUFFER_SIZE         4096U

/// Timestamp domain.  esp_timer_get_time() already returns microseconds, which
/// is exactly what the host expects for the DAP_SWJ_Pins wait and for
/// timestamped transfers.
#define TIMESTAMP_CLOCK         1000000U

/// The CMSIS-DAP UART-bridge commands are not implemented; serial pass-through
/// on this board goes out over TCP / HTTP instead (see net/tcp_server.c).
#define DAP_UART                0
#define DAP_UART_RX_BUFFER_SIZE 0U
#define DAP_UART_TX_BUFFER_SIZE 0U
#define DAP_UART_USB_COM_PORT   0

/// This is a generic probe, not a fixed on-board debugger.
#define TARGET_FIXED            0

//**************************************************************************************************
// Identification strings
//
// The debugger is what the IDE shows in its probe list, so give it something
// recognisable.  A serial number is synthesised from the eFuse MAC -- without
// one, tools that see several probes cannot tell them apart.
//**************************************************************************************************

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str)
{
    static const char vendor[] = "NexLink";
    memcpy(str, vendor, sizeof(vendor) - 1U);
    return (uint8_t)(sizeof(vendor) - 1U);
}

__STATIC_INLINE uint8_t DAP_GetProductString(char *str)
{
    static const char product[] = "NexLink CMSIS-DAP";
    memcpy(str, product, sizeof(product) - 1U);
    return (uint8_t)(sizeof(product) - 1U);
}

__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str)
{
    uint8_t mac[6];
    static const char hex[] = "0123456789ABCDEF";
    uint32_t i;

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != 0) {   /* ESP_OK */
        return 0U;
    }
    for (i = 0U; i < 6U; i++) {
        str[i * 2U + 0U] = hex[(mac[i] >> 4) & 0x0FU];
        str[i * 2U + 1U] = hex[(mac[i] >> 0) & 0x0FU];
    }
    return 12U;   /* no NUL terminator: the length byte delimits the string */
}

/* No fixed target device: report empty strings (the host decides). */
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString  (char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString (char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString   (char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) { (void)str; return 0U; }

//**************************************************************************************************
/**
\defgroup DAP_Config_PortIO_gr CMSIS-DAP Hardware I/O Pin Access
@{

GPIO access strategy
--------------------
The one-time pad configuration (IO_MUX function select, input enable, pull
resistors, drive strength) goes through the ESP-IDF gpio driver, because that
is the only thing that knows the ESP32-S3 IO_MUX layout.  Everything on the hot
path -- the per-bit SWCLK/SWDIO toggling -- writes the GPIO registers directly,
which is several times faster than gpio_set_level().

Both SWCLK and SWDIO are configured as GPIO_MODE_INPUT_OUTPUT up front, so the
input buffer stays enabled even while SWDIO is driving.  Direction is then
switched by toggling the single output-enable bit in GPIO_ENABLE_W1TS/W1TC,
which is exactly the SWDIO turnaround the CMSIS-DAP I/O layer expects.
*/

/* Register-bank helpers: ESP32-S3 splits GPIO 0..31 and 32..48 into two banks,
 * so every accessor has to pick a register *and* a bit position.
 *
 * Both branches shift by `(n) & 31` rather than `n` / `n - 32`.  For the pin
 * numbers in use here (0..48) the masked value is identical to the unmasked
 * one, but it keeps the shift count provably inside 0..31 no matter which pin
 * is substituted.  Without the mask, `DAP_GPIO_SET(PIN_SWD_SWCLK)` folds to
 * `1UL << (13 - 32)` in the unreachable else-branch, which GCC flags as
 * "left shift count is negative" -- and a negative shift count is undefined
 * behaviour, so it is worth not writing even in dead code. */
#define DAP_GPIO_SET(n)     do {                                                                   \
                                if ((n) < 32) { REG_WRITE(GPIO_OUT_W1TS_REG,    1UL << ((n) & 31U)); } \
                                else          { REG_WRITE(GPIO_OUT1_W1TS_REG,   1UL << ((n) & 31U)); } \
                            } while (0)
#define DAP_GPIO_CLR(n)     do {                                                                   \
                                if ((n) < 32) { REG_WRITE(GPIO_OUT_W1TC_REG,    1UL << ((n) & 31U)); } \
                                else          { REG_WRITE(GPIO_OUT1_W1TC_REG,   1UL << ((n) & 31U)); } \
                            } while (0)
#define DAP_GPIO_OE_SET(n)  do {                                                                   \
                                if ((n) < 32) { REG_WRITE(GPIO_ENABLE_W1TS_REG, 1UL << ((n) & 31U)); } \
                                else          { REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1UL << ((n) & 31U)); } \
                            } while (0)
#define DAP_GPIO_OE_CLR(n)  do {                                                                   \
                                if ((n) < 32) { REG_WRITE(GPIO_ENABLE_W1TC_REG, 1UL << ((n) & 31U)); } \
                                else          { REG_WRITE(GPIO_ENABLE1_W1TC_REG, 1UL << ((n) & 31U)); } \
                            } while (0)
#define DAP_GPIO_GET(n)     (((n) < 32) ? ((REG_READ(GPIO_IN_REG)  >> ((n) & 31U)) & 1UL)          \
                                        : ((REG_READ(GPIO_IN1_REG) >> ((n) & 31U)) & 1UL))

/* Configure the debug pads once.  Safe to call repeatedly.
 *
 * SWCLK/SWDIO/nRESET are dedicated pins; TDI/TDO/nTRST come from the expansion
 * header (see pinout.h). All six are set to input+output up front so direction
 * can be flipped with the single output-enable bit, exactly like SWDIO. */
__STATIC_INLINE void dap_pads_init(void)
{
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SWD_SWCLK) | (1ULL << PIN_SWD_SWDIO) |
                        (1ULL << PIN_SWD_NRST)  |
                        (1ULL << PIN_DEBUG_TDI) | (1ULL << PIN_DEBUG_TDO) |
                        (1ULL << PIN_DEBUG_NTRST),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* Idle state: both debug lines high, output drivers on (SWD uses a
     * pull-up on SWDIO and SWCLK idles high), nRESET released (high).
     * TDI and nTRST idle high as well; nTRST is asserted low by the host. */
    DAP_GPIO_SET(PIN_SWD_SWCLK);
    DAP_GPIO_SET(PIN_SWD_SWDIO);
    DAP_GPIO_SET(PIN_SWD_NRST);
    DAP_GPIO_SET(PIN_DEBUG_TDI);
    DAP_GPIO_SET(PIN_DEBUG_NTRST);

    /* TDO is driven by the target, so our output driver stays off; the pad is
     * still configured as input+output so DAP_GPIO_GET() can read it. */
    DAP_GPIO_OE_CLR(PIN_DEBUG_TDO);
}

/** Setup JTAG I/O pins: TCK/TMS/TDI/nTRST as outputs driving high, TDO as
    input. PCI (device index) is handled by the DAP core over the same pins. */
__STATIC_INLINE void PORT_JTAG_SETUP(void)
{
    dap_pads_init();
    DAP_GPIO_SET(PIN_SWD_SWCLK);
    DAP_GPIO_SET(PIN_SWD_SWDIO);
    DAP_GPIO_SET(PIN_DEBUG_TDI);
    DAP_GPIO_SET(PIN_DEBUG_NTRST);
    /* Re-enable the drivers: PORT_OFF() tri-states the pads and dap_pads_init()
       is latched, so without this a later DAP_Connect() leaves the lines dead. */
    DAP_GPIO_OE_SET(PIN_SWD_SWCLK);
    DAP_GPIO_OE_SET(PIN_SWD_SWDIO);
    DAP_GPIO_OE_SET(PIN_DEBUG_TDI);
    DAP_GPIO_OE_SET(PIN_DEBUG_NTRST);
    DAP_GPIO_OE_CLR(PIN_DEBUG_TDO);
}

/** Setup SWD I/O pins: SWCLK, SWDIO and nRESET, all driving high. */
__STATIC_INLINE void PORT_SWD_SETUP(void)
{
    dap_pads_init();
    DAP_GPIO_SET(PIN_SWD_SWCLK);
    DAP_GPIO_SET(PIN_SWD_SWDIO);
    DAP_GPIO_SET(PIN_SWD_NRST);
    /* Re-enable the drivers: PORT_OFF() tri-states the pads and dap_pads_init()
       is latched, so without this a later DAP_Connect() leaves SWCLK/NRST dead
       (SWDIO only recovers when a transfer explicitly enables its driver). */
    DAP_GPIO_OE_SET(PIN_SWD_SWCLK);
    DAP_GPIO_OE_SET(PIN_SWD_SWDIO);
    DAP_GPIO_OE_SET(PIN_SWD_NRST);
}

/** Disable JTAG/SWD I/O pins: everything back to high-Z input. */
__STATIC_INLINE void PORT_OFF(void)
{
    DAP_GPIO_OE_CLR(PIN_SWD_SWCLK);
    DAP_GPIO_OE_CLR(PIN_SWD_SWDIO);
    DAP_GPIO_OE_CLR(PIN_SWD_NRST);
    DAP_GPIO_OE_CLR(PIN_DEBUG_TDI);
    DAP_GPIO_OE_CLR(PIN_DEBUG_NTRST);
    /* TDO is never driven by us, so there is nothing to release. */
}

/* --- SWCLK / TCK -------------------------------------------------------- */

__STATIC_INLINE uint32_t PIN_SWCLK_TCK_IN(void)   { return DAP_GPIO_GET(PIN_SWD_SWCLK); }
__STATIC_INLINE void     PIN_SWCLK_TCK_SET(void)  { DAP_GPIO_SET(PIN_SWD_SWCLK); }
__STATIC_INLINE void     PIN_SWCLK_TCK_CLR(void)  { DAP_GPIO_CLR(PIN_SWD_SWCLK); }

/* --- SWDIO / TMS -------------------------------------------------------- */

__STATIC_INLINE uint32_t PIN_SWDIO_TMS_IN(void)   { return DAP_GPIO_GET(PIN_SWD_SWDIO); }
__STATIC_INLINE void     PIN_SWDIO_TMS_SET(void)  { DAP_GPIO_SET(PIN_SWD_SWDIO); }
__STATIC_INLINE void     PIN_SWDIO_TMS_CLR(void)  { DAP_GPIO_CLR(PIN_SWD_SWDIO); }

/* TCK and TMS are the JTAG names of the very same two pads. ARM's reference
 * DAP_config.h files define these aliases, and JTAG_DP.c calls them; SW_DP.c
 * only ever uses the SWD spellings, which is why they could be missing until
 * JTAG was enabled. */
#define PIN_TCK_SET  PIN_SWCLK_TCK_SET
#define PIN_TCK_CLR  PIN_SWCLK_TCK_CLR
#define PIN_TMS_SET  PIN_SWDIO_TMS_SET
#define PIN_TMS_CLR  PIN_SWDIO_TMS_CLR

__STATIC_INLINE uint32_t PIN_SWDIO_IN(void)
{
    return DAP_GPIO_GET(PIN_SWD_SWDIO);
}

__STATIC_INLINE void PIN_SWDIO_OUT(uint32_t bit)
{
    if (bit & 1U) {
        DAP_GPIO_SET(PIN_SWD_SWDIO);
    } else {
        DAP_GPIO_CLR(PIN_SWD_SWDIO);
    }
}

__STATIC_INLINE void PIN_SWDIO_OUT_ENABLE(void)
{
    DAP_GPIO_OE_SET(PIN_SWD_SWDIO);
}

__STATIC_INLINE void PIN_SWDIO_OUT_DISABLE(void)
{
    DAP_GPIO_OE_CLR(PIN_SWD_SWDIO);
}

/* --- JTAG port: TDI / TDO / nTRST on the expansion header ---------------
 *
 * These three are the only DAP signals not on a dedicated pin; they share the
 * expansion IOs and main/debug_pins.c takes them over at boot. The DAP core
 * bit-bangs TDI and samples TDO inside JTAG_Sequence / JTAG_Transfer, so these
 * have to be plain, fast GPIO accesses - same contract as SWCLK/SWDIO above.
 *
 * TDO is input-only from this side: the target drives it.
 * nTRST is driven (and read back) by DAP_ID_SWJ_Pins; on a target without a
 * JTAG reset pin, leaving it unconnected is harmless.
 */

__STATIC_INLINE uint32_t PIN_TDI_IN(void)         { return DAP_GPIO_GET(PIN_DEBUG_TDI); }
__STATIC_INLINE void     PIN_TDI_OUT(uint32_t bit){ if (bit & 1U) DAP_GPIO_SET(PIN_DEBUG_TDI); else DAP_GPIO_CLR(PIN_DEBUG_TDI); }
__STATIC_INLINE uint32_t PIN_TDO_IN(void)         { return DAP_GPIO_GET(PIN_DEBUG_TDO); }
__STATIC_INLINE uint32_t PIN_nTRST_IN(void)       { return DAP_GPIO_GET(PIN_DEBUG_NTRST); }
__STATIC_INLINE void     PIN_nTRST_OUT(uint32_t bit) { if (bit & 1U) DAP_GPIO_SET(PIN_DEBUG_NTRST); else DAP_GPIO_CLR(PIN_DEBUG_NTRST); }

/* --- nRESET: real, on PIN_SWD_NRST -------------------------------------- */

/** nRESET I/O pin: Get Input. */
__STATIC_INLINE uint32_t PIN_nRESET_IN(void)
{
    return DAP_GPIO_GET(PIN_SWD_NRST);
}

/** nRESET I/O pin: Set Output.
\param bit  0 = assert target reset (drive low), 1 = release (drive high).
*/
__STATIC_INLINE void PIN_nRESET_OUT(uint32_t bit)
{
    if (bit & 1U) {
        DAP_GPIO_SET(PIN_SWD_NRST);
    } else {
        DAP_GPIO_CLR(PIN_SWD_NRST);
    }
}

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_LEDs_gr CMSIS-DAP Hardware Status LEDs
@{

There are no dedicated probe status LEDs; this board reports its state on the
SSD1306 OLED and over HTTP, so the LED hooks are intentionally inert.
*/

__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { (void)bit; }
__STATIC_INLINE void LED_RUNNING_OUT  (uint32_t bit) { (void)bit; }

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_Timestamp_gr CMSIS-DAP Timestamp
@{
*/

/** Get timestamp of Test Domain Timer (microseconds, wrapping at ~71 min). */
__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
    return (uint32_t)esp_timer_get_time();
}

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_Initialization_gr CMSIS-DAP Initialization
@{
*/

/** Setup of the Debug Unit I/O pins (called from DAP_Setup()). */
__STATIC_INLINE void DAP_SETUP(void)
{
    dap_pads_init();
}

/** Reset Target Device with a custom sequence.
    No target-specific unlock sequence is needed: plain nRESET works. */
__STATIC_INLINE uint8_t RESET_TARGET(void)
{
    return 0U;
}

///@}

#endif /* __DAP_CONFIG_H__ */
