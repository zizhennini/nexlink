/*
 * dap_usb.c - CMSIS-DAP v2 (WinUSB) transport for the AI Wireless Debugger.
 *
 * Exposes the board as a USB debug probe: a vendor-specific CMSIS-DAP v2
 * interface (interface 0, bulk EPs 0x81/0x02) plus a CDC-ACM virtual COM
 * port (interfaces 1/2, bulk EPs 0x83/0x04, interrupt 0x84) exactly like
 * CherryDAP's ESP32-S3 reference and DAPLink.
 *
 * Windows binding works through the **WCID 1.0** (MS OS 1.0) mechanism:
 * the device answers string descriptor 0xEE with "MSFT100" + vendor code,
 * then the host fetches a compatible ID ("WINUSB") and a DeviceInterfaceGUID
 * through vendor requests wIndex=4/5.  This is the classic DAPLink handshake
 * that works on every Windows version from Win7 to Win11.
 *
 * **Why not MS OS 2.0 (BOS-based)?**
 * The MS OS 2.0 path was measured on Windows 11 24H2 and *rejected*: the
 * host fetched the full 172-byte descriptor set (delivered as 64+64+44 bytes
 * on the wire, every field per Microsoft's spec), then reset the bus and
 * re-enumerated 4 times without ever issuing SET_CONFIGURATION, ending with
 * Code 10 + STATUS_NO_SUCH_DEVICE.  Dropping bcdUSB to 2.00 and switching to
 * WCID 1.0 made the very same composite enumerate and bind WinUSB cleanly.
 * See the long comment around s_wcid_string_descriptor for the full story.
 *
 * Received packets go to the vendored CMSIS-DAP core (dap/DAP.c); the TCP
 * transport in net/dap_server.c uses the separate hand-written handler in
 * swd/cmsis_dap.c on top of swd_bridge.c.
 * The CDC side carries no UART in this mode; it is drained on read so the
 * COM port opens cleanly but stays quiet.
 *
 * Why USB *and* TCP
 * -----------------
 * OpenOCD, pyOCD and Keil look for CMSIS-DAP v2 on USB only -- their cmsis-dap
 * drivers have no TCP transport.  So a USB transport is what makes the probe
 * usable from a normal toolchain; the TCP server is a convenience for scripts
 * and for AI agents that cannot reach the USB bus.
 *
 * Layout of this file
 * -------------------
 *   1. probe identity and endpoint numbers
 *   2. descriptor blobs (MS OS 2.0 & 1.0 + BOS) and the descriptor callbacks
 *   3. the packet ring and the endpoint callbacks (interrupt context)
 *   4. the DAP request task
 *   5. dap_usb_start()
 *   6. descriptor dump (diagnostics, /api/usbdesc)
 *
 * Derived from cherry-embedded/CherryDAP (Apache-2.0) and CherryUSB's own
 * usbd_winusb2.0_cdc_template.c (Apache-2.0).  The transport logic is kept
 * close to upstream on purpose: the packet bookkeeping in the callbacks is
 * subtle and is known to work.  The difference is that upstream busy-polls
 * chry_dap_handle() from app_main; here it runs on a task that sleeps on a
 * semaphore signalled by the OUT callback, so the DAP transport costs no CPU
 * when no debugger is attached -- which matters on a device that is also
 * running WiFi, display and a TCP server.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "usb_config.h"     /* ESP_USBD_BASE, CherryUSB's ESP32-S3 port config */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"   /* composite: the CDC side, same as CherryDAP */

#include "pinout.h"
#include "debug_pins.h"    /* debug_pins_init(): expansion-IO ownership */
#include "DAP.h"
#include "DAP_config.h"
#include "dap_usb.h"
#include "swd_bridge.h"    /* swd_bus_lock()/unlock(): shared SWD pad ownership */

static const char *TAG = "dap_usb";

/* ------------------------------------------------------------------------- */
/* 1. Probe identity                                                         */
/* ------------------------------------------------------------------------- */

/*
 * 0x0D28 / 0x0204 is the VID/PID pair mbed's DAPLink firmware uses, and it is
 * what OpenOCD's cmsis-dap driver and pyOCD match on by default.  Presenting a
 * pair that tools already recognise is the whole difference between "works out
 * of the box" and "works after the user edits a config file".
 */
#define DAP_USBD_VID            0x0D28
#define DAP_USBD_PID            0x0204
#define DAP_USBD_MAX_POWER      500U    /* mA */
#define DAP_USBD_LANGID         0x0409U /* en-US */

/* Bulk endpoints.  IN must carry bit 7 set (host -> device is OUT 0x02). */
#define DAP_IN_EP               0x81U
#define DAP_OUT_EP              0x02U

/*
 * CDC-ACM side of the composite device.  Endpoint numbers follow CherryDAP's
 * dap_main.h with ONE deliberate deviation: the notification/interrupt IN
 * endpoint is 0x84, not CherryDAP's 0x85.
 *
 * Why: the ESP32-S3 DWC2 has only five IN endpoints including EP0 (IN0..IN4,
 * addresses 0x80..0x84; see osal/idf/usb_config.h).  At SET_CONFIGURATION the
 * CherryUSB core walks EVERY endpoint in the config descriptor -- registered
 * or not -- and usbd_ep_open() reads back DIEPTXF[n] for each IN endpoint.
 * For a non-existent IN5 that readback is 0, ep_open fails with "fifo
 * overflow", usbd_set_configuration() returns false and the core STALLs
 * SET_CONFIGURATION: Windows then reports Code 10 ("device cannot start")
 * for the whole composite.  So even a declared-only interrupt endpoint must
 * sit on real hardware: IN4 it is.
 *
 *   IN : 0x81 DAP bulk | 0x83 CDC data bulk | 0x84 CDC interrupt (declared,
 *        never armed -- dwc2 still opens it at SET_CONFIG, which is fine)
 *   OUT: 0x02 DAP bulk | 0x04 CDC data bulk
 */
#define DAP_CDC_INT_EP          0x84U   /* declared only, never armed */
#define DAP_CDC_OUT_EP          0x04U
#define DAP_CDC_IN_EP           0x83U

/* Interface 0 is a single vendor-specific interface with two bulk endpoints. */
#define DAP_INTERFACE_NUMBER    0x00U
#define DAP_INTERFACE_SUBCLASS  0x00U
#define DAP_INTERFACE_PROTOCOL  0x00U
#define DAP_CONFIGURATION_VALUE 0x01U
#define DAP_BUS_ID              0U

/* bDeviceClass/SubClass/Protocol declaring a composite device.  This is what
 * makes the host go looking for Microsoft OS descriptors at all. */
#define DAP_DEVICE_CLASS        0xEFU
#define DAP_DEVICE_SUBCLASS     0x02U
#define DAP_DEVICE_PROTOCOL     0x01U

/*
 * The bRequest value Windows uses to fetch the MS OS 2.0 descriptor set.  It is
 * advertised in the BOS descriptor below and must match here.
 */
#define DAP_MSOS20_VENDOR_CODE  0x20U

/*
 * GUID Windows registers for the WinUSB interface.  It must be the CMSIS-DAP
 * one: it is what a host-side WinUSB handle binds to, and tools that talk to
 * the probe through the Windows API look the device up by it.
 */
#define DAP_MSOS20_GUID_STRING  "{CDB3B5AD-293B-4663-AA36-1AAE46463776}"
#define DAP_MSOS20_GUID_UNITS   40U     /* 38 chars + 2 NUL of REG_MULTI_SZ  */
#define DAP_MSOS20_NAME_UNITS   21U     /* "DeviceInterfaceGUIDs" + NUL      */

/* Size of one feature block: header + name + data.
 * NOTE: dwPropertyDataType is a 4-byte DWORD per the MS OS 2.0 spec. */
#define DAP_MSOS20_REGPROP_LEN  (2U + 2U + 4U + 2U + (DAP_MSOS20_NAME_UNITS * 2U) + 2U + (DAP_MSOS20_GUID_UNITS * 2U))

/* ------------------------------------------------------------------------- */
/* 2. Microsoft OS 2.0 and BOS descriptors                                    */
/* ------------------------------------------------------------------------- */

/*
 * Windows refuses to bind WinUSB to a device whose VID/PID it does not already
 * know unless the firmware tells it to, and the modern way of doing that is the
 * MS OS 2.0 descriptor set advertised through a BOS platform capability.
 *
 * The layout below describes exactly one function (interface 0) with one
 * feature set: "this device is WinUSB compatible, and here is the device
 * interface GUID".  Every length in the blob is derived from the two
 * expressions in the comments; the static asserts after each array make sure a
 * future edit cannot silently desynchronise them, because a wrong length here
 * fails *silently* -- the probe just never appears on the host.
 *
 *  set header (10)
 *    function subset header (8)
 *      compatible ID feature (20)
 *      registry property feature (4 + 42 + 4 + 80 = 134)
 *  ---------------------------------------------------
 *  total 172
 */
#define DAP_MSOS20_SUBSET_LEN   (8U + 20U + DAP_MSOS20_REGPROP_LEN)     /* 162 */
#define DAP_MSOS20_SET_LEN      (10U + DAP_MSOS20_SUBSET_LEN)           /* 172 */

static const uint8_t s_msos20_descriptor_set[] = {
    /* ---- set header ----------------------------------------------------- */
    0x0A, 0x00,                                     /* wLength = 10 */
    0x00, 0x00,                                     /* wDescriptorType = SET_HEADER */
    0x00, 0x00, 0x03, 0x06,                         /* dwWindowsVersion = Win 8.1+ */
    0xAC, 0x00,                                     /* wDescriptorSetTotalLength = 172 */
    /* ---- function subset header ----------------------------------------- */
    0x08, 0x00,                                     /* wLength = 8 */
    0x02, 0x00,                                     /* wDescriptorType = SUBSET_HEADER_FUNCTION */
    0x00,                                           /* bFirstInterface = 0 */
    0x00,                                           /* bReserved */
    0xA2, 0x00,                                     /* wSubsetLength = 162 */
    /* ---- feature: compatible ID ----------------------------------------- */
    0x14, 0x00,                                     /* wLength = 20 */
    0x03, 0x00,                                     /* wDescriptorType = COMPATIBLE_ID */
    0x57, 0x49, 0x4E, 0x55, 0x53, 0x42, 0x00, 0x00, /* CompatibleID = "WINUSB" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* SubCompatibleID = none */
    /* ---- feature: registry property "DeviceInterfaceGUIDs" --------------- */
    0x86, 0x00,                                     /* wLength = 134 */
    0x04, 0x00,                                     /* wDescriptorType = REG_PROPERTY */
    0x07, 0x00, 0x00, 0x00,                         /* dwPropertyDataType = REG_MULTI_SZ */
    0x2A, 0x00,                                     /* wPropertyNameLength = 42 */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    0x50, 0x00,                                     /* wPropertyDataLength = 80 */
    '{', 0,
    'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0,
    '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-', 0,
    '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0, '6', 0,
    '}', 0, 0, 0, 0, 0,
};

/*
 * Binary Object Store: one platform capability, the MS OS 2.0 one.  Its
 * PlatformCapabilityUUID is the Microsoft OS 2.0 descriptor platform UUID and
 * is fixed by the specification; the wTotalLength below must equal the length
 * of the descriptor set above, or Windows will discard the whole thing.
 *
 *   BOS header (5) + platform capability (28) = 33
 */
static const uint8_t s_bos_descriptor[] = {
    0x05,                                           /* bLength = 5 */
    0x0F,                                           /* bDescriptorType = BOS */
    0x21, 0x00,                                     /* wTotalLength = 33 */
    0x01,                                           /* bNumDeviceCaps = 1 */
    /* ---- platform capability: Microsoft OS 2.0 --------------------------- */
    0x1C,                                           /* bLength = 28 */
    0x10,                                           /* bDescriptorType = DEVICE_CAPABILITY */
    0x05,                                           /* bDevCapabilityType = PLATFORM */
    0x00,                                           /* bReserved */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, /* PlatformCapabilityUUID */
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F, /* {D8DD60DF-...} */
    0x00, 0x00, 0x03, 0x06,                         /* dwWindowsVersion = Win 8.1+ */
    0xAC, 0x00,                                     /* wMSOSDescriptorSetTotalLength = 172 */
    0x20,                                           /* bVendorCode */
    0x00,                                           /* bAltEnumCode */
};

/* A wrong length here means "device silently never enumerates", so check it at
 * compile time rather than by squinting at the blob. */
_Static_assert(sizeof(s_msos20_descriptor_set) == DAP_MSOS20_SET_LEN,
               "MS OS 2.0 descriptor set length does not match wDescriptorSetTotalLength");
_Static_assert(sizeof(s_bos_descriptor) == 33U,
               "BOS descriptor length does not match wTotalLength");

/* ------------------------------------------------------------------------- */
/* 2b. Microsoft OS 1.0 (WCID) descriptors -- the binding Windows accepts     */
/* ------------------------------------------------------------------------- */

/*
 * Why MS OS 1.0 and not 2.0
 * -------------------------
 * The MS OS 2.0 path (BOS + vendor request 0x20 for the descriptor set) was
 * measured on this bench and REJECTED by Windows 11 24H2: the host read the
 * device/config/BOS descriptors, fetched the 172-byte set (verified complete
 * on the wire: 64+64+44 bytes, no truncation, all fields per spec), then reset
 * the port and restarted enumeration without ever issuing SET_CONFIGURATION.
 * Four attempts, then Code 10 with STATUS_NO_SUCH_DEVICE, and no driver
 * install attempt in the SetupAPI log.
 *
 * Dropping bcdUSB to 2.00 made the very same composite enumerate perfectly
 * (usbccgp started, CDC child came up as a COM port), so the configuration
 * descriptor, endpoints and strings are all good -- only the MS OS 2.0
 * handshake is broken on Windows.
 *
 * WCID 1.0 is the older mechanism that DAPLink itself has always used: the
 * host asks for string descriptor 0xEE, finds "MSFT100" plus a vendor code,
 * then asks that vendor code for the compatible ID (wIndex 4) and the
 * DeviceInterfaceGUID (wIndex 5) -- which is what makes Windows bind WinUSB
 * without an .inf file on every version from Win7 to Win11.
 */
#define DAP_WCID_VENDOR_CODE   0x20U

/* String descriptor 0xEE: bLength 18, "MSFT100" in UTF-16, vendor code. */
static const uint8_t s_wcid_string_descriptor[] = {
    0x12,                                           /* bLength = 18 */
    0x03,                                           /* bDescriptorType = STRING */
    'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00,
    '1', 0x00, '0', 0x00, '0', 0x00,
    DAP_WCID_VENDOR_CODE,                           /* bMS_VendorCode */
    0x00,                                           /* bPad */
};

/* Compatible ID descriptor (returned for vendor request wIndex = 4). */
static const uint8_t s_wcid_compat_id[] = {
    0x28, 0x00, 0x00, 0x00,                         /* dwLength = 40 */
    0x00, 0x01,                                     /* bcdVersion = 1.0 */
    0x04, 0x00,                                     /* wIndex = 4 */
    0x01,                                           /* bCount = 1 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       /* reserved[7] */
    0x00,                                           /* bFirstInterfaceNumber = 0 (DAP) */
    0x01,                                           /* bReserved */
    0x57, 0x49, 0x4E, 0x55, 0x53, 0x42, 0x00, 0x00, /* CompatibleID = "WINUSB" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* SubCompatibleID = none */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* reserved[6] */
};

/* Extended properties (vendor request wIndex = 5): the DeviceInterfaceGUID
 * that tools bind to.  Note this is REG_SZ with the singular name, unlike the
 * REG_MULTI_SZ "DeviceInterfaceGUIDs" of MS OS 2.0. */
static const uint8_t s_wcid_properties[] = {
    0x8E, 0x00, 0x00, 0x00,                         /* dwLength = 142 */
    0x00, 0x01,                                     /* bcdVersion = 1.0 */
    0x05, 0x00,                                     /* wIndex = 5 */
    0x01, 0x00,                                     /* wCount = 1 */
    /* ---- one registry property ------------------------------------------ */
    0x84, 0x00, 0x00, 0x00,                         /* dwSize = 132 */
    0x01, 0x00, 0x00, 0x00,                         /* dwPropertyDataType = REG_SZ */
    0x28, 0x00,                                     /* wPropertyNameLength = 40 */
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,
    'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 0x00, 0x00,    /* "DeviceInterfaceGUID" + NUL */
    0x4E, 0x00, 0x00, 0x00,                         /* dwPropertyDataLength = 78 */
    /* "{CDB3B5AD-293B-4663-AA36-1AAE46463776}" in UTF-16 + NUL */
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00,
    '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00,
    '2', 0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00,
    '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00,
    'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00,
    '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00, '6', 0x00,
    '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00,
    '}', 0x00, 0x00, 0x00,
};

_Static_assert(sizeof(s_wcid_string_descriptor) == 18U, "WCID string must be 18 bytes");
_Static_assert(sizeof(s_wcid_compat_id) == 40U, "WCID compat ID must be 40 bytes");
_Static_assert(sizeof(s_wcid_properties) == 142U, "WCID properties must be 142 bytes");

/*
 * "No extended properties" descriptor, handed out for every interface other
 * than the DAP one (the CDC pair).
 *
 * This array exists for a security-of-execution reason, not cosmetics:
 * CherryUSB's MS OS 1.0 vendor handler indexes comp_id_property[] with the
 * request's wValue, which Windows sets to the INTERFACE NUMBER when it asks
 * for extended properties (wIndex = 5).  With a single-element array a host
 * that asks for interface 1 or 2 makes the handler read a pointer past the
 * end of the array and dereference it -- which is a panic, and was observed
 * here as a PANIC reset right after WinUSB bound for the first time.
 * Padding the array out to 8 entries covers any interface count this device
 * can ever present, so the index is always in range.
 */
static const uint8_t s_wcid_properties_none[] = {
    0x0A, 0x00, 0x00, 0x00,                         /* dwLength = 10 */
    0x00, 0x01,                                     /* bcdVersion = 1.0 */
    0x05, 0x00,                                     /* wIndex = 5 */
    0x00, 0x00,                                     /* wCount = 0 (no properties) */
};
_Static_assert(sizeof(s_wcid_properties_none) == 10U, "WCID none-properties must be 10 bytes");

static const uint8_t *s_wcid_property_array[8] = {
    s_wcid_properties,          /* interface 0: the CMSIS-DAP WinUSB interface */
    s_wcid_properties_none,     /* interface 1: CDC control                     */
    s_wcid_properties_none,     /* interface 2: CDC data                        */
    s_wcid_properties_none,     /* spare, keeps any stray index in range       */
    s_wcid_properties_none,
    s_wcid_properties_none,
    s_wcid_properties_none,
    s_wcid_properties_none,
};

static const struct usb_msosv1_descriptor s_msos1_descriptor = {
    .string = s_wcid_string_descriptor,
    .vendor_code = DAP_WCID_VENDOR_CODE,
    .compat_id = s_wcid_compat_id,
    .comp_id_property = s_wcid_property_array,
};

/* --- standard USB descriptors ------------------------------------------- */

#define DAP_CONFIG_SIZE  (9U + 9U + 7U + 7U + 66U)   /* config + DAP intf/2 EPs + CDC (66) */
_Static_assert(DAP_PACKET_SIZE == 64U,
               "the full-speed bulk endpoint and CMSIS-DAP v2 both want 64-byte packets");

static const uint8_t s_device_descriptor[] = {
    /*
     * USB 2.00 on purpose: the MS OS 2.0 (BOS) handshake is rejected by this
     * Windows build, while WCID 1.0 via the 0xEE string descriptor binds
     * WinUSB on everything from Win7 to Win11 -- and Windows only asks for
     * the 0xEE string while bcdUSB is below 2.10.
     *
     * bcdDevice is 0x0101 rather than 0x0100 for a reason worth remembering:
     * Windows caches the verdict "this device has no OS descriptors" in
     *   HKLM\SYSTEM\CurrentControlSet\Control\usbflags\<VID><PID><bcdDevice>
     * as osvc = 0,0 -- and that key includes bcdDevice.  A bench machine that
     * once saw a build which stalled the 0xEE probe (exactly what a device
     * without WCID support does) keeps that verdict and then skips the WCID
     * handshake forever, even for a brand-new device instance.  Bumping the
     * firmware revision gives a fresh cache key -- and a new machine has no
     * cached verdict anyway.
     */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, DAP_DEVICE_CLASS, DAP_DEVICE_SUBCLASS,
                               DAP_DEVICE_PROTOCOL, DAP_USBD_VID, DAP_USBD_PID,
                               0x0101, 0x01),
};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(DAP_CONFIG_SIZE, 3, DAP_CONFIGURATION_VALUE,
                               USB_CONFIG_BUS_POWERED, DAP_USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(DAP_INTERFACE_NUMBER, 0x00, 0x02,
                                  0xFF, DAP_INTERFACE_SUBCLASS, DAP_INTERFACE_PROTOCOL,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    /* CDC-ACM (interfaces 1 and 2, with its own IAD): the virtual COM port */
    CDC_ACM_DESCRIPTOR_INIT(0x01, DAP_CDC_INT_EP, DAP_CDC_OUT_EP, DAP_CDC_IN_EP,
                            DAP_PACKET_SIZE, 0x00),
};

/*
 * This is a full-speed-only device, but USB 2.1 hosts still ask for the
 * other-speed and device-qualifier descriptors, so they have to exist.
 */
static const uint8_t s_other_speed_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(DAP_CONFIG_SIZE, 3, DAP_CONFIGURATION_VALUE,
                               USB_CONFIG_BUS_POWERED, DAP_USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(DAP_INTERFACE_NUMBER, 0x00, 0x02,
                                  0xFF, DAP_INTERFACE_SUBCLASS, DAP_INTERFACE_PROTOCOL,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DAP_IN_EP, USB_ENDPOINT_TYPE_BULK, DAP_PACKET_SIZE, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, DAP_CDC_INT_EP, DAP_CDC_OUT_EP, DAP_CDC_IN_EP,
                            DAP_PACKET_SIZE, 0x00),
};

/* These two were never checked before: if a macro expansion produced a
 * different number of bytes than DAP_CONFIG_SIZE, wTotalLength would lie and
 * the host would refuse to configure the device (Code 10, no children). */
_Static_assert(sizeof(s_config_descriptor) == DAP_CONFIG_SIZE,
               "wTotalLength must match the bytes actually present");
_Static_assert(sizeof(s_other_speed_config_descriptor) == DAP_CONFIG_SIZE,
               "other-speed descriptor must have the same size");

static const uint8_t s_device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_1, DAP_DEVICE_CLASS, DAP_DEVICE_SUBCLASS,
                                         DAP_DEVICE_PROTOCOL, 0x01),
};

/* The 0xEE (MS OS 1.0) string index.  CherryUSB replies to it only when an
 * msosv1 descriptor is registered; we use MS OS 2.0 instead, so it stalls --
 * which is correct and expected. */
#define DAP_LANGID_STRING_INDEX 0U
#define DAP_VENDOR_STRING_INDEX 1U
#define DAP_PRODUCT_STRING_INDEX 2U
#define DAP_SERIAL_STRING_INDEX 3U

/**
 * Serial number, taken from the WiFi MAC so every board is distinguishable.
 *
 * CherryUSB's string callback returns a plain NUL-terminated ASCII string and
 * the core expands it to UTF-16 itself, so this has to survive strlen().
 *
 * Filled in by dap_usb_start() before the stack is initialised: the callback
 * runs from interrupt context, and esp_read_mac() takes a lock that must not be
 * taken from an ISR.
 */
static char s_serial_string[16];

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;

    switch (index) {
    case DAP_LANGID_STRING_INDEX: {
        /* Not a real string: the core reads these two bytes as the LANGID
         * little-endian pair.  en-US = 0x0409. */
        static const char langid[2] = { 0x09, 0x04 };
        return langid;
    }
    case DAP_VENDOR_STRING_INDEX:
        return "NexLink";
    case DAP_PRODUCT_STRING_INDEX:
        return "NexLink CMSIS-DAP";
    case DAP_SERIAL_STRING_INDEX:
        /* NULL until dap_usb_start() has filled it in, which makes the host
         * see "no serial number" rather than garbage. */
        return (s_serial_string[0] != '\0') ? s_serial_string : NULL;
    default:
        return NULL;
    }
}

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_quality_descriptor;
}

static const uint8_t *other_speed_config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_other_speed_config_descriptor;
}

/* NOTE: the MS OS 2.0 set (s_msos20_descriptor_set) and the BOS
 * (s_bos_descriptor) are still built below and dumped by /api/usbdesc for
 * reference, but they are deliberately NOT registered: they are exactly the
 * descriptors Windows 11 24H2 rejected (see the comment above
 * s_wcid_string_descriptor).  WCID 1.0 replaces them. */

static const struct usb_descriptor s_dap_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    /* WCID 1.0: the mechanism Windows actually accepts (see the long comment
     * above s_wcid_string_descriptor).  The MS OS 2.0 set and BOS are kept in
     * this file for reference and for the /api/usbdesc dump, but deliberately
     * not registered: with bcdUSB 2.00 the host never asks for them. */
    .msosv1_descriptor = &s_msos1_descriptor,
};

/* ------------------------------------------------------------------------- */
/* 3. Packet ring and endpoint callbacks                                      */
/* ------------------------------------------------------------------------- */

/*
 * The counters deliberately count monotonically and are compared with wrapping
 * (uint16_t) subtraction, which is the upstream scheme and works for any
 * DAP_PACKET_COUNT.  With the current DAP_PACKET_COUNT == 1 the ring is a
 * single slot: at most one request is in flight, and the host's own pipelining
 * provides the queueing.
 */
static volatile uint16_t s_req_index_in;
static volatile uint16_t s_req_index_out;
static volatile uint16_t s_req_count_in;
static volatile uint16_t s_req_count_out;
static volatile uint8_t  s_req_idle = 1U;

static volatile uint16_t s_resp_index_in;
static volatile uint16_t s_resp_index_out;
static volatile uint16_t s_resp_count_in;
static volatile uint16_t s_resp_count_out;
static volatile uint8_t  s_resp_idle = 1U;

/* DMA targets: internal RAM, naturally aligned to the cache line granularity. */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t s_request[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t s_response[DAP_PACKET_COUNT][DAP_PACKET_SIZE];
static uint16_t s_response_size[DAP_PACKET_COUNT];

/* CDC-ACM OUT scratch buffer (declared here; referenced from the event
 * handler below and the cdc_out_callback further down). */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t s_cdc_read_buf[512];

/* Woken by the OUT callback; the task does the actual DAP work. */
static SemaphoreHandle_t s_dap_signal;

/* Tracks whether the USB device stack + DAP task are live. dap_usb_start()
 * is idempotent: the Config menu may call it again after it was already
 * started at boot. Disabling is not done at runtime (a live USB stack is
 * never torn down mid-transfer); it takes effect at the next reboot. */
static bool s_usb_started;

static volatile uint32_t s_rx_packets;
static volatile uint32_t s_tx_packets;
/* Incremented on every USBD_EVENT_CONFIGURED: proves the host's
 * SET_CONFIGURATION was accepted (i.e. the EP walk in the core succeeded). */
static volatile uint32_t s_configured_count;

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
    case USBD_EVENT_RESET:
        /* The host is re-enumerating: drop the pipeline state so the next
         * CONFIGURED starts from a clean slate.  Any partially transferred
         * response is discarded, which is what the host expects. */
        s_req_idle = 1U;
        s_resp_idle = 1U;
        s_resp_count_in = 0U;
        s_resp_count_out = 0U;
        break;

    case USBD_EVENT_CONFIGURED:
        s_configured_count++;
        /* Arm the first OUT transfer.  Without this the probe enumerates but
         * never receives a single command. */
        s_req_idle = 0U;
        s_req_index_in = 0U;
        s_req_index_out = 0U;
        s_req_count_in = 0U;
        s_req_count_out = 0U;
        usbd_ep_start_read(DAP_BUS_ID, DAP_OUT_EP, s_request[0], DAP_PACKET_SIZE);
        /* Arm the CDC OUT pipe too so the virtual COM never stalls the host. */
        usbd_ep_start_read(DAP_BUS_ID, DAP_CDC_OUT_EP, s_cdc_read_buf, sizeof(s_cdc_read_buf));
        break;

    default:
        break;
    }
}

static void dap_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    s_rx_packets++;

    if (s_request[s_req_index_in][0] == ID_DAP_TransferAbort) {
        /* Not a queued command: it cancels the transfers already in flight.
         * Swallow it instead of handing it to the DAP core. */
        DAP_TransferAbort = 1U;
    } else {
        s_req_index_in++;
        if (s_req_index_in == DAP_PACKET_COUNT) {
            s_req_index_in = 0U;
        }
        s_req_count_in++;
    }

    /* Re-arm if there is room; otherwise leave the ring full and let the task
     * re-arm it once it has consumed a packet. */
    if ((uint16_t)(s_req_count_in - s_req_count_out) != DAP_PACKET_COUNT) {
        usbd_ep_start_read(DAP_BUS_ID, DAP_OUT_EP, s_request[s_req_index_in], DAP_PACKET_SIZE);
    } else {
        s_req_idle = 1U;
    }

    if (s_dap_signal) {
        BaseType_t woke = pdFALSE;
        xSemaphoreGiveFromISR(s_dap_signal, &woke);
        portYIELD_FROM_ISR(woke);
    }
}

static void dap_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    s_tx_packets++;

    if (s_resp_count_in != s_resp_count_out) {
        usbd_ep_start_write(DAP_BUS_ID, DAP_IN_EP, s_response[s_resp_index_out],
                            s_response_size[s_resp_index_out]);
        s_resp_index_out++;
        if (s_resp_index_out == DAP_PACKET_COUNT) {
            s_resp_index_out = 0U;
        }
        s_resp_count_out++;
    } else {
        s_resp_idle = 1U;
    }
}

static struct usbd_endpoint s_dap_out_endpoint = {
    .ep_addr = DAP_OUT_EP,
    .ep_cb = dap_out_callback,
};

static struct usbd_endpoint s_dap_in_endpoint = {
    .ep_addr = DAP_IN_EP,
    .ep_cb = dap_in_callback,
};

static struct usbd_interface s_dap_interface = {
    /* A vendor-specific interface has no class requests to serve, so all the
     * handlers stay NULL and any class request is answered with a stall. */
    .intf_num = DAP_INTERFACE_NUMBER,
};

/* --- CDC-ACM side (virtual COM port) --------------------------------------
 * Present so the composite matches the DAPLink/CherryDAP enumeration Windows
 * remembers for 0d28:0204.  No UART is wired to it here (that is the TTL
 * role's job), so OUT data is drained and IN stays idle; the endpoints must
 * still be registered or the class driver will stall them.
 */
static struct usbd_interface s_cdc_comm_intf;
static struct usbd_interface s_cdc_data_intf;
/* s_cdc_read_buf declared above with the other DMA buffers. */

static void cdc_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)nbytes;
    /* Drain: accept host writes and immediately re-arm so the pipe never
     * stalls.  The bytes are discarded (no loopback target in DAP mode). */
    usbd_ep_start_read(busid, DAP_CDC_OUT_EP, s_cdc_read_buf, sizeof(s_cdc_read_buf));
}

static void cdc_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
}

static struct usbd_endpoint s_cdc_out_endpoint = {
    .ep_addr = DAP_CDC_OUT_EP,
    .ep_cb = cdc_out_callback,
};

static struct usbd_endpoint s_cdc_in_endpoint = {
    .ep_addr = DAP_CDC_IN_EP,
    .ep_cb = cdc_in_callback,
};

/* ------------------------------------------------------------------------- */
/* 4. DAP request task                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Coalesce queued DAP_QueueCommands packets into a single DAP_ExecuteCommands
 * and hand the result to the CMSIS-DAP core.
 *
 * ID_DAP_QueueCommands exists so a host can hand over several commands at once
 * and have them execute back to back without an intervening round trip; the
 * rewrite below is how the reference implementation turns the queue into one
 * batch.  A single request packet is always its own batch, which is why this
 * loop matters mainly if DAP_PACKET_COUNT is ever raised.
 */
static void dap_process_pending(void)
{
    /* The shared bit-bang bus is also used by the TCP DAP path and the
     * menu/HTTP diagnostics; hold it for the whole batch. */
    swd_bus_lock();

    while (s_req_count_in != s_req_count_out) {
        uint16_t n = s_req_index_out;

        while (s_request[n][0] == ID_DAP_QueueCommands) {
            s_request[n][0] = ID_DAP_ExecuteCommands;
            n++;
            if (n == DAP_PACKET_COUNT) {
                n = 0U;
            }
            if (n == s_req_index_in) {
                break;      /* the whole ring is one batch */
            }
        }

        const uint8_t *req = s_request[s_req_index_out];
        uint8_t cmd = req[0];
        uint32_t count = 0U;
        bool reject = false;

        /* Bounds guard: the vendored DAP.c does not clamp the transfer count
         * against the 64-byte response buffer.  A host (or bug) asking for
         * more than DAP_PACKET_SIZE/4 words would make DAP.c scribble past
         * s_response; answer with an error packet instead. */
        if (cmd == ID_DAP_Transfer) {
            count = req[2];
            reject = count > ((DAP_PACKET_SIZE - 3U) / 4U);
        } else if (cmd == ID_DAP_TransferBlock) {
            count = (uint32_t)req[2] | ((uint32_t)req[3] << 8);
            reject = count > ((DAP_PACKET_SIZE - 4U) / 4U);
        }

        uint16_t resp_len;
        if (reject) {
            uint8_t *rs = s_response[s_resp_index_in];
            if (cmd == ID_DAP_Transfer) {
                rs[0] = ID_DAP_Transfer;
                rs[1] = 0U;
                rs[2] = 8U;             /* protocol error flag */
                resp_len = 3U;
            } else {
                rs[0] = ID_DAP_TransferBlock;
                rs[1] = 0U;
                rs[2] = 0U;
                rs[3] = 8U;             /* protocol error flag */
                resp_len = 4U;
            }
        } else {
            resp_len = (uint16_t)DAP_ExecuteCommand(req, s_response[s_resp_index_in]);
        }
        s_response_size[s_resp_index_in] = resp_len;

        s_req_index_out++;
        if (s_req_index_out == DAP_PACKET_COUNT) {
            s_req_index_out = 0U;
        }
        s_req_count_out++;

        /* If the OUT callback had to stop arming reads because the ring was
         * full, it is our job to get it going again. */
        if (s_req_idle) {
            if ((uint16_t)(s_req_count_in - s_req_count_out) != DAP_PACKET_COUNT) {
                s_req_idle = 0U;
                usbd_ep_start_read(DAP_BUS_ID, DAP_OUT_EP, s_request[s_req_index_in],
                                   DAP_PACKET_SIZE);
            }
        }

        s_resp_index_in++;
        if (s_resp_index_in == DAP_PACKET_COUNT) {
            s_resp_index_in = 0U;
        }
        s_resp_count_in++;

        /* Kick off transmission if the IN endpoint is idle. */
        if (s_resp_idle) {
            if (s_resp_count_in != s_resp_count_out) {
                uint16_t idx = s_resp_index_out++;
                if (s_resp_index_out == DAP_PACKET_COUNT) {
                    s_resp_index_out = 0U;
                }
                s_resp_count_out++;
                s_resp_idle = 0U;
                usbd_ep_start_write(DAP_BUS_ID, DAP_IN_EP, s_response[idx],
                                    s_response_size[idx]);
            }
        }
    }

    swd_bus_unlock();
}

static void dap_usb_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Nothing to do until a host actually sends a command. */
        if (xSemaphoreTake(s_dap_signal, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        dap_process_pending();
    }
}

/* ------------------------------------------------------------------------- */
/* 5. Bring-up                                                                */
/* ------------------------------------------------------------------------- */

bool dap_usb_is_configured(void)
{
    return usb_device_is_configured(DAP_BUS_ID);
}

uint32_t dap_usb_configured_count(void)
{
    return s_configured_count;
}

uint32_t dap_usb_get_rx_packets(void)
{
    return s_rx_packets;
}

uint32_t dap_usb_get_tx_packets(void)
{
    return s_tx_packets;
}

bool dap_usb_is_started(void)
{
    return s_usb_started;
}

esp_err_t dap_usb_start(void)
{
    /* Idempotent: the Config menu may enable the probe after boot already
     * started it. A second full bring-up would double-create the semaphore
     * and the task, so bail out early. */
    if (s_usb_started) {
        return ESP_OK;
    }
    /* Same rule mirrored from usb_ttl_start(): one PHY, one live stack. */
    extern bool usb_ttl_is_started(void);
    if (usb_ttl_is_started()) {
        ESP_LOGE(TAG, "USB PHY is held by the TTL bridge - reboot to switch");
        return ESP_ERR_INVALID_STATE;
    }

    /* Claim the expansion-header debug IOs (TDI/TDO/nTRST/SWO) before any pad
     * is configured. This lives here, not at the boot call site, because the
     * probe can also be enabled at runtime from /api/usb_dap, /api/usb_mode and
     * the OLED Config page - all of which land in this function. With the
     * default pin map the I2C monitor owns IO39/IO40, so without this the
     * monitor would keep driving pads that the probe and the SWO UART then
     * take over: JTAG/SWO fail and the monitor breaks silently.
     * debug_pins_init() is idempotent. */
    debug_pins_init();

    /* Build the serial number while we are still in task context: the string
     * callback runs from the USB interrupt. */
    {
        char raw[16] = { 0 };
        uint8_t len = DAP_GetSerNumString(raw);
        if (len > sizeof(s_serial_string) - 1U) {
            len = sizeof(s_serial_string) - 1U;
        }
        memcpy(s_serial_string, raw, len);
        s_serial_string[len] = '\0';
    }

    s_dap_signal = xSemaphoreCreateBinary();
    if (s_dap_signal == NULL) {
        ESP_LOGE(TAG, "out of memory creating the DAP signal");
        return ESP_ERR_NO_MEM;
    }

    /* Configure SWCLK/SWDIO/nRESET and put them in their idle state.  Must
     * happen before the core can execute anything, and only once: DAP_Setup()
     * is idempotent, but it also resets the DAP state machine, so calling it
     * again at runtime would drop an in-progress transfer. */
    DAP_Setup();

    usbd_desc_register(DAP_BUS_ID, &s_dap_descriptor);
    usbd_add_interface(DAP_BUS_ID, &s_dap_interface);
    usbd_add_endpoint(DAP_BUS_ID, &s_dap_out_endpoint);
    usbd_add_endpoint(DAP_BUS_ID, &s_dap_in_endpoint);

    /* CDC-ACM virtual COM side of the composite (interfaces 1 and 2). */
    usbd_add_interface(DAP_BUS_ID, usbd_cdc_acm_init_intf(DAP_BUS_ID, &s_cdc_comm_intf));
    usbd_add_interface(DAP_BUS_ID, usbd_cdc_acm_init_intf(DAP_BUS_ID, &s_cdc_data_intf));
    usbd_add_endpoint(DAP_BUS_ID, &s_cdc_out_endpoint);
    usbd_add_endpoint(DAP_BUS_ID, &s_cdc_in_endpoint);

    if (usbd_initialize(DAP_BUS_ID, ESP_USBD_BASE, usbd_event_handler) != 0) {
        ESP_LOGE(TAG, "USB device stack failed to start");
        vSemaphoreDelete(s_dap_signal);
        s_dap_signal = NULL;
        return ESP_FAIL;
    }

    if (xTaskCreate(dap_usb_task, "dap_usb", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create the DAP task");
        usbd_deinitialize(DAP_BUS_ID);
        vSemaphoreDelete(s_dap_signal);
        s_dap_signal = NULL;
        return ESP_FAIL;
    }

    s_usb_started = true;
    ESP_LOGI(TAG, "CMSIS-DAP v2 over USB (WinUSB) ready, serial %s", s_serial_string);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* 6. Descriptor dump (diagnostics; served as GET /api/usbdesc)               */
/* ------------------------------------------------------------------------- */

static size_t dump_hex(char *out, size_t out_len, size_t used, const char *label,
                       const uint8_t *p, size_t n)
{
    used += (size_t)snprintf(out + used, out_len - used, "%s (%u bytes):\n",
                             label, (unsigned)n);
    for (size_t i = 0U; i < n && used + 64U < out_len; i++) {
        used += (size_t)snprintf(out + used, out_len - used, "%02x%s", p[i],
                                 ((i % 16U) == 15U) ? "\n" : " ");
    }
    if ((n % 16U) != 0U && used + 2U < out_len) {
        used += (size_t)snprintf(out + used, out_len - used, "\n");
    }
    return used;
}

void dap_usb_dump_descriptors(char *out, size_t out_len)
{
    size_t used = 0U;

    used += (size_t)snprintf(out + used, out_len - used,
        "device: bcdUSB=%02x%02x class=%02x/%02x/%02x maxpkt0=%u vid=%04x pid=%04x bcdDev=%02x%02x imfr=%u ipro=%u iser=%u numcfg=%u\n",
        s_device_descriptor[3], s_device_descriptor[2],
        s_device_descriptor[4], s_device_descriptor[5], s_device_descriptor[6],
        s_device_descriptor[7],
        (unsigned)(s_device_descriptor[8] | (s_device_descriptor[9] << 8)),
        (unsigned)(s_device_descriptor[10] | (s_device_descriptor[11] << 8)),
        s_device_descriptor[13], s_device_descriptor[12],
        s_device_descriptor[14], s_device_descriptor[15], s_device_descriptor[16],
        s_device_descriptor[17]);

    used += (size_t)snprintf(out + used, out_len - used,
        "config: declared=%u actual=%u wTotalLength=%u bNumInterfaces=%u cfgValue=%u bmAttr=%02x bMaxPower=%u\n",
        (unsigned)DAP_CONFIG_SIZE, (unsigned)sizeof(s_config_descriptor),
        (unsigned)(s_config_descriptor[2] | (s_config_descriptor[3] << 8)),
        s_config_descriptor[4], s_config_descriptor[5], s_config_descriptor[7],
        s_config_descriptor[8]);

    used += (size_t)snprintf(out + used, out_len - used,
        "msos20: actual=%u set_total=%u subset_wLength=%u subset_first_intf=%u subset_wSubsetLength=%u compat_wLength=%u regprop_wLength=%u regprop_dwType=%u name_len=%u data_len=%u\n",
        (unsigned)sizeof(s_msos20_descriptor_set),
        (unsigned)(s_msos20_descriptor_set[8] | (s_msos20_descriptor_set[9] << 8)),
        (unsigned)(s_msos20_descriptor_set[10] | (s_msos20_descriptor_set[11] << 8)),
        s_msos20_descriptor_set[14],
        (unsigned)(s_msos20_descriptor_set[16] | (s_msos20_descriptor_set[17] << 8)),
        (unsigned)(s_msos20_descriptor_set[18] | (s_msos20_descriptor_set[19] << 8)),
        (unsigned)(s_msos20_descriptor_set[38] | (s_msos20_descriptor_set[39] << 8)),
        (unsigned)(s_msos20_descriptor_set[42] | (s_msos20_descriptor_set[43] << 8) |
                   ((uint32_t)s_msos20_descriptor_set[44] << 16) |
                   ((uint32_t)s_msos20_descriptor_set[45] << 24)),
        (unsigned)(s_msos20_descriptor_set[46] | (s_msos20_descriptor_set[47] << 8)),
        (unsigned)(s_msos20_descriptor_set[90] | (s_msos20_descriptor_set[91] << 8)));

    used += (size_t)snprintf(out + used, out_len - used,
        "bos: actual=%u wTotalLength=%u numCaps=%u capLen=%u capType=%u setTotal=%u vendorCode=%u winVer=%02x%02x%02x%02x\n",
        (unsigned)sizeof(s_bos_descriptor),
        (unsigned)(s_bos_descriptor[2] | (s_bos_descriptor[3] << 8)),
        s_bos_descriptor[4], s_bos_descriptor[5], s_bos_descriptor[7],
        (unsigned)(s_bos_descriptor[29] | (s_bos_descriptor[30] << 8)),
        s_bos_descriptor[31],
        s_bos_descriptor[28], s_bos_descriptor[27], s_bos_descriptor[26], s_bos_descriptor[25]);

    used = dump_hex(out, out_len, used, "device desc", s_device_descriptor, sizeof(s_device_descriptor));
    used = dump_hex(out, out_len, used, "config desc", s_config_descriptor, sizeof(s_config_descriptor));
    used = dump_hex(out, out_len, used, "bos desc", s_bos_descriptor, sizeof(s_bos_descriptor));
    used = dump_hex(out, out_len, used, "msos20 set", s_msos20_descriptor_set, sizeof(s_msos20_descriptor_set));
}
