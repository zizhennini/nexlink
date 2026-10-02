idf_component_register(SRCS
                       "main.c"
                       "pin_config.c"
                       "capture.c"
                       "pwm_mon.c"
                       "spi_mon.c"
                       "i2c_mon.c"
                       "buttons/buttons.c"
                       "display/oled_ssd1306.c"
                       "display/menu_ui.c"
                       "display/cjk_font.c"
                       "net/tcp_server.c"
                       "net/http_status.c"
                       # USB enumeration trace (diagnostic; /api/usbtrace)
                       "net/usbtrace.c"
                       "serial/serial_bridge.c"
                       "serial/usb_ttl.c"
                       "swd/swd_bridge.c"
                       "swd/cmsis_dap.c"
                       "net/dap_server.c"
                       "wifi/wifi_manager.c"
                       # --- vendored CMSIS-DAP core ---------------------------------
                       # ARM's DAPLink sources, taken from cherry-embedded/CherryDAP
                       # (Apache-2.0).  Kept verbatim apart from the PIN_DELAY_SLOW
                       # fix documented in dap/DAP.h, and DAP_config.h which is ours.
                       # Files deliberately omitted, with the switch that disables
                       # them in dap/DAP_config.h:
                       #   SWO.c      - SWO_UART 0 / SWO_MANCHESTER 0
                       #   UART.c     - DAP_UART 0
                       #   JTAG_DP.c  - DAP_JTAG 0
                       "dap/DAP.c"
                       "dap/SW_DP.c"
                       "dap/DAP_vendor.c"
                       "dap/dap_usb.c"
                       INCLUDE_DIRS
                       "."
                       "buttons"
                       "dap"
                       "display"
                       "net"
                       "serial"
                       "swd"
                       "wifi"
                       REQUIRES
                       esp_event
                       esp_wifi
                       esp_http_server
                       esp_http_client
                       esp_driver_gpio
                       esp_driver_spi
                       esp_driver_i2c
                       esp_driver_rmt
                       esp_driver_uart
                       esp_driver_ledc
                       esp_timer
                       esp_hw_support
                       nvs_flash
                       # esp_ota_* for the web /api/ota firmware upload path +
                       # rollback check-in in main.c.
                       app_update
                       spi_flash
                       freertos
                       lwip
                       # USB device stack behind dap/dap_usb.c.  The component is a
                       # managed one, so its name carries the registry namespace.
                       # `usb` is what actually owns the OTG PHY (usb_new_phy) and
                       # is a private dependency of CherryUSB's ESP port.
                       cherry-embedded__cherryusb
                       usb
                       EMBED_FILES
                       "display/HZK16"
                       "display/uni2gb.bin"
                       )
