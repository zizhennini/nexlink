cmake_minimum_required(VERSION 3.16)

# NOTE: EXTRA_COMPONENT_DIRS is deliberately not set. The upstream project
# pointed it at ./components for the ST7735S LCD panel driver; that display was
# replaced by the I2C SSD1306 OLED, the component was removed, and a stale
# reference to a non-existent directory makes CMake configure fail outright.
# Add a components/ directory back (with real content) if a local component is
# ever needed again.

# --- CherryUSB (USB device stack behind the CMSIS-DAP probe) -----------------
# CherryUSB's ESP32-S3 port needs one compile-time switch that has no Kconfig
# entry:
#
#   CONFIG_USBDEV_ADVANCE_DESC - CherryUSB generates the descriptor sets
#                                (device/config/quality/other-speed/BOS/MS OS)
#                                from a struct usb_descriptor instead of one
#                                registration call per descriptor.  The CMSIS-DAP
#                                probe needs this: its MS OS 2.0 descriptor set
#                                is what makes Windows bind WinUSB without an
#                                .inf file, and that is only reachable through
#                                the advanced-descriptor path.
#
# Deliberately NOT set here: CONFIG_CHERRYUSB_DEVICE_DCD.  It is a CMake
# variable used purely to pick a source file, and CherryUSB's own
# CMakeLists.txt already hard-codes "dwc2_esp" for ESP_PLATFORM.  Defining it
# here as well would only leak a macro into every compilation unit.
add_definitions(-DCONFIG_USBDEV_ADVANCE_DESC=1)

# Temporary: enable per-SETUP-packet trace logging to UART0 (FTDI COM6).
# Remove once DAP enumeration is fixed; this is diagnostic only.
add_definitions(-DCONFIG_USBDEV_SETUP_LOG_PRINT)

# --- Compiler transport ------------------------------------------------------
# Build with -pipe so the compiler passes talk over pipes instead of dropping
# temporary .s files into %TEMP%.  Without it, a parallel build here fails
# intermittently with:
#
#   cc1.exe: fatal error: cannot open '...\ccQ4oL8i.s' for writing:
#     Permission denied
#
# (whatever the temp directory is -- %TEMP% or a scratch dir inside the
# workspace).  -pipe removes the temporary files entirely, so the problem
# cannot happen.  It also makes the build marginally faster.
add_compile_options(-pipe)

# CherryUSB's ESP port does `#include "usb_config.h"`.  That file comes from
# CherryUSB itself (osal/idf/usb_config.h) and reaches the main component as a
# public include directory of the cherry-embedded__cherryusb component, so
# nothing has to be added to the global include path here.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(nexlink)
