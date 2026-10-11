/*
 * http_status.c - HTTP server with LCD simulation + serial log
 *
 * GET /            - HTML page: centered 160x80 LCD sim (crisp) + 3 buttons below, serial log
 * GET /api/status  - JSON: wifi, ip, baud, rx, tx, uptime, tcp, page, sel, usbdap
 * GET /api/data    - drain serial RX as text
 * GET /api/capture - timestamped, direction-tagged capture history
 *                    (?since=&max=&fmt=text|hex|csv&want=chunks|meta)
 * GET /api/btn?b=1&a=press - simulate button (b=1/2/3; a=press fires instantly,
 *                            a=long = the SW2 held gesture: cancel edit / home)
 * GET /api/usb_dap?toggle=1 - flip the USB CMSIS-DAP probe (on: starts now and
 *                            persists; off: persists, USB freed at next reboot)
 * GET /api/baud?b= - set baud rate
 * GET  /api/usb_mode - report / select the USB-C role: ?m=off|dap|ttl
 *                      (off -> active starts live; active -> active reboots)
 * GET  /api/dut/reset - pulse the target's NRST; ?boot=1 also holds BOOT low
 *                      across the release (enter ESP ROM download mode)
 * POST /api/send   - send data to DUT
 * POST /api/ota    - raw app-image upload into the inactive OTA slot;
 *                    answers JSON, then reboots into the new firmware.
 *                    Bootloader auto-rolls back if the image never reaches
 *                    app_main (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 */
#include "http_status.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial_bridge.h"
#include "usb_ttl.h"
#include "wifi_manager.h"
#include "pinout.h"
#include "dash_ui.h"
#include "oled_ssd1306.h"
#include "pin_config.h"
#include "capture.h"
#include "debug_pins.h"
#include "ws_server.h"
#include "swd_bridge.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "usbtrace.h"
#include "dap_usb.h"

static const char *TAG = "http";

extern int tcp_server_client_count(void);

static const char s_index_html[] = 
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">\n"
"<title>NexLink 无线调试器</title>\n"
"<link rel=\"icon\" href=\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect width='24' height='24' rx='5' fill='%2322d3ee'/><g fill='none' stroke='%2305303a' stroke-width='2' stroke-linecap='round'><path d='M4.5 12a10.5 10.5 0 0 1 15 0'/><path d='M7.8 15.2a6 6 0 0 1 8.4 0'/><circle cx='12' cy='18.6' r='1.5' fill='%2305303a' stroke='none'/></g></svg>\">\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}\n"
":root{\n"
"  --bg:#080c13;--card:#121b27;--card2:#0e1621;--inset:#0a1019;--border:#202e42;\n"
"  --fg:#e9eef6;--muted:#8b99ad;--faint:#5c697d;\n"
"  --accent:#2dd4ee;--accent2:#38bdf8;--ok:#34d399;--warn:#fbbf24;--err:#fb7185;\n"
"  --rx:#34d399;--tx:#fbbf24;\n"
"}\n"
"html,body{height:100%}\n"
"body{\n"
"  font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",\"PingFang SC\",\"Microsoft YaHei\",sans-serif;\n"
"  background:var(--bg);color:var(--fg);font-size:14px;line-height:1.5;\n"
"  padding:14px 12px calc(72px + env(safe-area-inset-bottom));\n"
"  background-image:radial-gradient(900px 380px at 50% -120px,rgba(34,211,238,.14),transparent 70%);\n"
"  background-repeat:no-repeat;\n"
"}\n"
".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}\n"
".wrap{max-width:860px;margin:0 auto}\n"
"::-webkit-scrollbar{width:8px;height:8px}\n"
"::-webkit-scrollbar-thumb{background:#243448;border-radius:8px}\n"
"::-webkit-scrollbar-track{background:transparent}\n"
"\n"
"/* ---------- brand hero ---------- */\n"
".hero{\n"
"  position:relative;overflow:hidden;border-radius:18px;margin-bottom:14px;\n"
"  padding:16px 18px;border:1px solid #1c3a47;\n"
"  background:linear-gradient(135deg,#0d2a34 0%,#101c29 52%,#0d151f 100%);\n"
"  box-shadow:0 12px 32px rgba(0,0,0,.4),inset 0 1px 0 rgba(255,255,255,.05);\n"
"}\n"
".hero-glow{position:absolute;top:-60px;right:-30px;width:200px;height:200px;border-radius:50%;\n"
"  background:radial-gradient(circle,rgba(45,212,238,.35),transparent 65%);pointer-events:none}\n"
".hero-row{position:relative;z-index:1;display:flex;align-items:center;gap:13px}\n"
".logo{\n"
"  width:44px;height:44px;flex:0 0 auto;border-radius:13px;display:flex;align-items:center;justify-content:center;\n"
"  background:linear-gradient(135deg,#46e6f5,#0ea5e9);\n"
"  box-shadow:0 8px 18px rgba(34,211,238,.4),inset 0 1px 0 rgba(255,255,255,.45);\n"
"}\n"
".logo svg{width:27px;height:27px}\n"
".hero-txt{min-width:0}\n"
".brand-name{\n"
"  font-size:23px;font-weight:800;letter-spacing:2px;line-height:1.05;\n"
"  background:linear-gradient(90deg,#cffafe,#67e8f9 45%,#38bdf8);\n"
"  -webkit-background-clip:text;background-clip:text;color:transparent;\n"
"}\n"
".brand-sub{font-size:11.5px;color:#8aa0b6;margin-top:5px;letter-spacing:2px}\n"
".badge{\n"
"  margin-left:auto;flex:0 0 auto;\n"
"  display:inline-flex;align-items:center;gap:6px;\n"
"  font-size:12px;font-weight:600;padding:5px 11px;border-radius:999px;\n"
"  background:#1a2533;color:var(--muted);border:1px solid var(--border);\n"
"}\n"
".badge::before{content:\"\";width:7px;height:7px;border-radius:50%;background:currentColor}\n"
".badge.ok{color:var(--ok);background:rgba(52,211,153,.12);border-color:rgba(52,211,153,.35)}\n"
".badge.warn{color:var(--warn);background:rgba(251,191,36,.12);border-color:rgba(251,191,36,.35)}\n"
".badge.err{color:var(--err);background:rgba(251,113,133,.12);border-color:rgba(251,113,133,.35)}\n"
"\n"
"/* ---------- cards ---------- */\n"
".card{\n"
"  background:linear-gradient(180deg,#131d29,#0f1823);\n"
"  border:1px solid var(--border);border-radius:15px;\n"
"  padding:15px 17px;margin-bottom:12px;\n"
"  box-shadow:0 8px 22px rgba(0,0,0,.28),inset 0 1px 0 rgba(255,255,255,.03);\n"
"}\n"
".card h2{display:flex;align-items:center;gap:9px;font-size:13.5px;font-weight:700;margin-bottom:3px}\n"
".card h2::before{content:\"\";width:3px;height:15px;border-radius:2px;\n"
"  background:linear-gradient(var(--accent),var(--accent2));box-shadow:0 0 8px rgba(45,212,238,.6)}\n"
".card .sub{font-size:11.5px;color:var(--faint);margin-bottom:12px}\n"
".kv{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:8px 0;border-bottom:1px dashed rgba(32,46,66,.8);font-size:13px}\n"
".kv:last-child{border-bottom:none}\n"
".kv .k{color:var(--muted);flex-shrink:0}\n"
".kv .v{text-align:right;word-break:break-all}\n"
".kv .v.good{color:var(--ok)} .kv .v.bad{color:var(--err)} .kv .v.hot{color:var(--warn)}\n"
"\n"
"/* ---------- controls ---------- */\n"
"input,select{\n"
"  background:var(--inset);color:var(--fg);border:1px solid var(--border);\n"
"  border-radius:10px;padding:10px 11px;font-size:13px;font-family:inherit;width:100%;\n"
"  transition:border-color .15s,box-shadow .15s;\n"
"}\n"
"input:focus,select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 3px rgba(45,212,238,.15)}\n"
"input[type=file]{font-size:12px;color:var(--muted);padding:8px}\n"
"select{appearance:none;background-image:url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='10' height='6'><path d='M1 1l4 4 4-4' stroke='%238b99ad' fill='none' stroke-width='1.5'/></svg>\");background-repeat:no-repeat;background-position:right 11px center;padding-right:28px}\n"
".btn{\n"
"  display:inline-flex;align-items:center;justify-content:center;gap:6px;\n"
"  border:1px solid var(--border);background:linear-gradient(180deg,#16222f,#101925);color:var(--fg);\n"
"  border-radius:10px;padding:9px 16px;font-size:13px;font-weight:600;\n"
"  font-family:inherit;cursor:pointer;flex-shrink:0;\n"
"  box-shadow:0 3px 10px rgba(0,0,0,.25),inset 0 1px 0 rgba(255,255,255,.05);\n"
"  transition:transform .1s,box-shadow .15s,filter .15s;\n"
"}\n"
".btn:hover{filter:brightness(1.12)}\n"
".btn:active{transform:scale(.97)}\n"
".btn.primary{background:linear-gradient(180deg,#39e0f3,#14b8d8);border-color:#2dd4ee;color:#042730;font-weight:700;\n"
"  box-shadow:0 6px 16px rgba(34,211,238,.32),inset 0 1px 0 rgba(255,255,255,.4)}\n"
".btn.ok{background:linear-gradient(180deg,#41dca5,#1fae82);border-color:#34d399;color:#05302a;font-weight:700;\n"
"  box-shadow:0 6px 16px rgba(52,211,153,.28),inset 0 1px 0 rgba(255,255,255,.4)}\n"
".btn.danger{background:transparent;border-color:rgba(251,113,133,.5);color:var(--err);box-shadow:none}\n"
".btn.block{width:100%;margin-top:8px}\n"
".btn:disabled{opacity:.5;cursor:not-allowed}\n"
".field{margin-bottom:10px}\n"
".field label{display:block;font-size:12px;color:var(--muted);margin-bottom:5px}\n"
".row{display:flex;gap:8px;align-items:center}\n"
".row>*{flex:1}\n"
".row .keep{flex:0 0 auto}\n"
".switch{display:inline-flex;align-items:center;gap:6px;font-size:12px;color:var(--muted);cursor:pointer;user-select:none;flex-shrink:0}\n"
".switch input{width:auto;accent-color:var(--accent)}\n"
".hint{font-size:11.5px;color:var(--faint);margin-top:8px;line-height:1.6}\n"
"\n"
"/* segmented control */\n"
".seg{display:flex;gap:6px;background:var(--inset);border:1px solid var(--border);border-radius:12px;padding:5px}\n"
".seg button{\n"
"  flex:1;border:none;background:transparent;color:var(--muted);\n"
"  border-radius:9px;padding:9px 4px;font-size:12.5px;font-weight:600;\n"
"  font-family:inherit;cursor:pointer;line-height:1.3;transition:background .15s,color .15s;\n"
"}\n"
".seg button small{display:block;font-size:10px;font-weight:400;opacity:.8;margin-top:2px}\n"
".seg button.on{background:linear-gradient(180deg,#39e0f3,#14b8d8);color:#042730;box-shadow:0 4px 12px rgba(34,211,238,.3)}\n"
".seg button.on small{opacity:.85}\n"
"\n"
"/* sub tabs */\n"
".subtabs{display:flex;gap:6px;margin-bottom:12px;background:var(--inset);border:1px solid var(--border);border-radius:12px;padding:5px}\n"
".subtabs button{flex:1;border:none;background:transparent;color:var(--muted);border-radius:9px;padding:9px;font-size:13px;font-weight:600;font-family:inherit;cursor:pointer;transition:background .15s,color .15s}\n"
".subtabs button.on{background:#153040;color:var(--accent);box-shadow:inset 0 0 0 1px rgba(45,212,238,.25)}\n"
"\n"
"/* ---------- serial terminal ---------- */\n"
".term-toolbar{display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap}\n"
".term-toolbar select{width:auto;padding:7px 26px 7px 9px;font-size:12px}\n"
"#term{\n"
"  background:#060a10;border:1px solid var(--border);border-radius:11px;\n"
"  padding:11px 13px;height:50vh;max-height:560px;overflow-y:auto;\n"
"  font-size:12.5px;line-height:1.65;white-space:pre-wrap;word-break:break-all;\n"
"  box-shadow:inset 0 2px 10px rgba(0,0,0,.45);\n"
"}\n"
"#term .rx{color:var(--rx)}\n"
"#term .tx{color:var(--tx)}\n"
"#term .sy{color:var(--faint)}\n"
"#term .ts{color:#46536a}\n"
".send-row{display:flex;gap:8px;margin-top:8px}\n"
".send-row select{width:84px;flex:0 0 auto;padding:9px 24px 9px 8px}\n"
".send-row input{flex:1;min-width:0}\n"
"\n"
"/* ---------- OLED ---------- */\n"
".lcd-wrap{background:#000;border-radius:11px;padding:12px;display:flex;justify-content:center;margin-bottom:10px;\n"
"  box-shadow:inset 0 0 0 1px #101826,0 6px 16px rgba(0,0,0,.35)}\n"
".lcd-wrap canvas{width:100%;max-width:320px;height:auto;image-rendering:pixelated}\n"
".lcd-btns{display:flex;gap:8px}\n"
".lcd-btns button{flex:1;padding:12px 0;font-weight:700}\n"
"\n"
"/* ---------- waves / logs ---------- */\n"
"canvas.wave{width:100%;height:90px;background:#060a10;border:1px solid var(--border);border-radius:10px;margin:8px 0;box-shadow:inset 0 2px 8px rgba(0,0,0,.4)}\n"
".txn-log{background:#060a10;border:1px solid var(--border);border-radius:10px;padding:8px 11px;max-height:170px;overflow-y:auto;font-size:11.5px;margin:8px 0;box-shadow:inset 0 2px 8px rgba(0,0,0,.4)}\n"
".txn-log div{white-space:pre-wrap;word-break:break-all;line-height:1.7}\n"
".txn-log .mosi{color:var(--ok)} .txn-log .miso{color:var(--warn)} .txn-log .i2cw{color:var(--ok)} .txn-log .i2cr{color:var(--accent)}\n"
".result{font-size:12px;margin-top:8px;color:var(--muted);word-break:break-all}\n"
".result.err{color:var(--err)} .result.ok{color:var(--ok)}\n"
"\n"
"/* ---------- wifi list / OTA ---------- */\n"
".wifi-list{display:flex;flex-direction:column;gap:6px;margin:8px 0}\n"
".wifi-list button{text-align:left;padding:9px 12px;font-size:12.5px;display:flex;justify-content:space-between;width:100%}\n"
".wifi-list button .sig{color:var(--muted);font-size:11px;flex-shrink:0;margin-left:8px}\n"
".progress{height:8px;background:var(--inset);border-radius:99px;overflow:hidden;margin-top:10px;border:1px solid var(--border)}\n"
".progress i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--accent),var(--accent2));box-shadow:0 0 10px rgba(45,212,238,.6);transition:width .2s}\n"
"\n"
"/* ---------- pages / bottom nav ---------- */\n"
".page{display:none;animation:fade .18s ease}\n"
".page.on{display:block}\n"
"@keyframes fade{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}\n"
".tabbar{\n"
"  position:fixed;left:0;right:0;bottom:0;z-index:50;\n"
"  display:flex;background:rgba(12,17,25,.92);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);\n"
"  border-top:1px solid var(--border);padding-bottom:env(safe-area-inset-bottom);\n"
"}\n"
".tabbar button{\n"
"  position:relative;flex:1;border:none;background:transparent;color:var(--faint);\n"
"  font-family:inherit;font-size:11.5px;font-weight:600;\n"
"  padding:9px 0 10px;cursor:pointer;display:flex;flex-direction:column;align-items:center;gap:3px;\n"
"  transition:color .15s;\n"
"}\n"
".tabbar button svg{width:21px;height:21px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;transition:transform .15s}\n"
".tabbar button.on{color:var(--accent)}\n"
".tabbar button.on svg{transform:translateY(-1px) scale(1.08)}\n"
".tabbar button.on::before{content:\"\";position:absolute;top:3px;width:46px;height:30px;border-radius:999px;\n"
"  background:rgba(45,212,238,.14);border:1px solid rgba(45,212,238,.22);z-index:-1}\n"
"\n"
"/* ---------- toast ---------- */\n"
"#toasts{position:fixed;top:14px;left:50%;transform:translateX(-50%);z-index:100;display:flex;flex-direction:column;gap:8px;width:min(90vw,420px)}\n"
".toast{\n"
"  background:rgba(24,36,52,.96);backdrop-filter:blur(8px);border:1px solid var(--border);color:var(--fg);\n"
"  border-radius:12px;padding:10px 16px;font-size:13px;text-align:center;\n"
"  box-shadow:0 10px 28px rgba(0,0,0,.5);animation:fade .15s ease;\n"
"}\n"
".toast.ok{border-color:rgba(52,211,153,.55)} .toast.err{border-color:rgba(251,113,133,.55)}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"wrap\">\n"
"\n"
"  <div class=\"hero\">\n"
"  <div class=\"hero-glow\"></div>\n"
"  <div class=\"hero-row\">\n"
"    <div class=\"logo\">\n"
"      <svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#05303a\" stroke-width=\"2.1\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n"
"        <path d=\"M4.5 12a10.5 10.5 0 0 1 15 0\"/>\n"
"        <path d=\"M7.8 15.2a6 6 0 0 1 8.4 0\"/>\n"
"        <circle cx=\"12\" cy=\"18.6\" r=\"1.5\" fill=\"#05303a\" stroke=\"none\"/>\n"
"      </svg>\n"
"    </div>\n"
"    <div class=\"hero-txt\">\n"
"      <div class=\"brand-name\">NexLink</div>\n"
"      <div class=\"brand-sub\">无线硬件调试器</div>\n"
"    </div>\n"
"    <span class=\"badge err\" id=\"topBadge\">连接中…</span>\n"
"  </div>\n"
"</div>\n"
"\n"
"  <!-- ============ 页面 1：串口终端 ============ -->\n"
"  <div class=\"page on\" id=\"page-serial\">\n"
"    <div class=\"card\">\n"
"      <h2>串口终端</h2>\n"
"      <div class=\"sub\">和目标板之间收发数据，相当于无线版串口助手</div>\n"
"      <div class=\"term-toolbar\">\n"
"        <select id=\"baud\" title=\"波特率\">\n"
"          <option value=\"9600\">9600</option>\n"
"          <option value=\"115200\" selected>115200</option>\n"
"          <option value=\"460800\">460800</option>\n"
"          <option value=\"921600\">921600</option>\n"
"        </select>\n"
"        <label class=\"switch\"><input type=\"checkbox\" id=\"hexMode\"> HEX 十六进制</label>\n"
"        <label class=\"switch\"><input type=\"checkbox\" id=\"tsMode\" checked> 时间</label>\n"
"        <button class=\"btn\" id=\"clearBtn\" style=\"padding:7px 12px;font-size:12px\">清空</button>\n"
"      </div>\n"
"      <div id=\"term\" class=\"mono\"><div class=\"sy\">等待目标板数据…</div></div>\n"
"      <div class=\"send-row\">\n"
"        <select id=\"newline\" title=\"结尾换行\">\n"
"          <option value=\"0\">无换行</option>\n"
"          <option value=\"1\" selected>LF</option>\n"
"          <option value=\"2\">CR</option>\n"
"          <option value=\"3\">CRLF</option>\n"
"        </select>\n"
"        <input id=\"sendText\" placeholder=\"输入要发送的内容，回车发送\" autocomplete=\"off\">\n"
"        <button class=\"btn primary\" id=\"sendBtn\">发送</button>\n"
"      </div>\n"
"      <div class=\"hint\">绿色 RX = 目标板发来的数据；黄色 TX = 你发出的数据。勾选 HEX 后按十六进制显示和发送。</div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- ============ 页面 2：设备状态 ============ -->\n"
"  <div class=\"page\" id=\"page-status\">\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>无线连接</h2>\n"
"      <div class=\"sub\">设备当前的 WiFi 工作状态</div>\n"
"      <div class=\"kv\"><span class=\"k\">状态</span><span class=\"v\" id=\"sWifi\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">网络名称</span><span class=\"v\" id=\"sSsid\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">信号强度</span><span class=\"v\" id=\"sRssi\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">IP 地址</span><span class=\"v mono\" id=\"sIp\">--</span></div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>运行信息</h2>\n"
"      <div class=\"kv\"><span class=\"k\">已运行</span><span class=\"v\" id=\"sUptime\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">TCP 客户端</span><span class=\"v\" id=\"sTcp\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">串口波特率</span><span class=\"v mono\" id=\"sBaud\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">串口接收 RX</span><span class=\"v mono good\" id=\"sRx\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">串口发送 TX</span><span class=\"v mono hot\" id=\"sTx\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">本次开机原因</span><span class=\"v\" id=\"sRst\">--</span></div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>USB-C 接口状态</h2>\n"
"      <div class=\"sub\">当前 USB 口扮演的角色，以及调试探针的通信计数</div>\n"
"      <div class=\"kv\"><span class=\"k\">当前角色</span><span class=\"v\" id=\"sUsb\">--</span></div>\n"
"      <div id=\"usbExtra\"></div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>OLED 屏幕远程预览</h2>\n"
"      <div class=\"sub\">实时显示板子屏幕内容，也可以远程按实体键</div>\n"
"      <div class=\"lcd-wrap\"><canvas id=\"lcdCv\"></canvas></div>\n"
"      <div class=\"lcd-btns\">\n"
"        <button class=\"btn\" id=\"b1\">SW1 上</button>\n"
"        <button class=\"btn primary\" id=\"b2\">SW2 选择</button>\n"
"        <button class=\"btn\" id=\"b3\">SW3 下</button>\n"
"      </div>\n"
"      <div class=\"hint\">SW1 = 向上，SW3 = 向下，SW2 = 确认；SW2 按住不放 = 从任意位置回到主屏。</div>\n"
"      <div class=\"kv\" style=\"margin-top:8px\"><span class=\"k\">当前菜单</span><span class=\"v mono\" id=\"sMenu\">--</span></div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>固件信息</h2>\n"
"      <div class=\"kv\"><span class=\"k\">运行分区</span><span class=\"v mono\" id=\"sSlot\">--</span></div>\n"
"      <div class=\"kv\"><span class=\"k\">OTA 状态</span><span class=\"v\" id=\"sOta\">--</span></div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- ============ 页面 3：总线工具 ============ -->\n"
"  <div class=\"page\" id=\"page-bus\">\n"
"    <div class=\"card\">\n"
"      <div class=\"subtabs\">\n"
"        <button class=\"on\" data-bus=\"pwm\">PWM</button>\n"
"        <button data-bus=\"spi\">SPI</button>\n"
"        <button data-bus=\"i2c\">I2C</button>\n"
"      </div>\n"
"\n"
"      <!-- PWM -->\n"
"      <div class=\"bus-pane\" id=\"bus-pwm\">\n"
"        <h2>PWM 测量 / 输出</h2>\n"
"        <div class=\"sub\">测量引脚上信号的频率和占空比，也可以让板子自己输出一路 PWM</div>\n"
"        <div class=\"kv\"><span class=\"k\">测量频率</span><span class=\"v mono good\" id=\"pwmFreq\">--</span></div>\n"
"        <div class=\"kv\"><span class=\"k\">测量占空比</span><span class=\"v mono\" id=\"pwmDuty\">--</span></div>\n"
"        <canvas class=\"wave\" id=\"pwmWave\"></canvas>\n"
"        <div class=\"field\"><label>输出频率 (Hz)</label><input id=\"pwmOFreq\" type=\"number\" value=\"1000\"></div>\n"
"        <div class=\"field\"><label>输出占空比 (%)</label><input id=\"pwmODuty\" type=\"number\" value=\"50\" min=\"0\" max=\"100\"></div>\n"
"        <div class=\"row\">\n"
"          <button class=\"btn ok\" id=\"pwmStart\">开始输出</button>\n"
"          <button class=\"btn\" id=\"pwmStop\">停止输出</button>\n"
"        </div>\n"
"        <div class=\"result\" id=\"pwmResult\"></div>\n"
"      </div>\n"
"\n"
"      <!-- SPI -->\n"
"      <div class=\"bus-pane\" id=\"bus-spi\" style=\"display:none\">\n"
"        <h2>SPI 抓包 / 发送</h2>\n"
"        <div class=\"sub\">默认作从机抓总线上的数据；点「发送」时自动切为主机主动读写</div>\n"
"        <div class=\"kv\"><span class=\"k\">抓包状态</span><span class=\"v\" id=\"spiState\">--</span></div>\n"
"        <div class=\"kv\"><span class=\"k\">引脚映射</span><span class=\"v mono\" id=\"spiPins\" style=\"font-size:11px\">--</span></div>\n"
"        <div class=\"txn-log\" id=\"spiLog\"><div style=\"color:#5d687c\">（暂无 SPI 事务）</div></div>\n"
"        <canvas class=\"wave\" id=\"spiWave\"></canvas>\n"
"        <div class=\"field\"><label>发送数据（HEX，如 AA 55 01）</label><input id=\"spiHex\" placeholder=\"AA5501\"></div>\n"
"        <div class=\"row\">\n"
"          <select id=\"spiMode\" class=\"keep\" style=\"flex:1\">\n"
"            <option value=\"0\">Mode 0</option><option value=\"1\">Mode 1</option>\n"
"            <option value=\"2\">Mode 2</option><option value=\"3\">Mode 3</option>\n"
"          </select>\n"
"          <select id=\"spiClk\" style=\"flex:1\">\n"
"            <option value=\"100000\">100 kHz</option>\n"
"            <option value=\"500000\" selected>500 kHz</option>\n"
"            <option value=\"1000000\">1 MHz</option>\n"
"            <option value=\"4000000\">4 MHz</option>\n"
"            <option value=\"8000000\">8 MHz</option>\n"
"          </select>\n"
"        </div>\n"
"        <button class=\"btn primary block\" id=\"spiSend\">发送并读取 MISO</button>\n"
"        <div class=\"result\" id=\"spiResult\"></div>\n"
"      </div>\n"
"\n"
"      <!-- I2C -->\n"
"      <div class=\"bus-pane\" id=\"bus-i2c\" style=\"display:none\">\n"
"        <h2>I2C 抓包 / 收发</h2>\n"
"        <div class=\"sub\">抓取 I2C 总线上的读写事务，也可以作为主机主动读写从机</div>\n"
"        <div class=\"kv\"><span class=\"k\">抓包状态</span><span class=\"v\" id=\"i2cState\">--</span></div>\n"
"        <div class=\"txn-log\" id=\"i2cLog\"><div style=\"color:#5d687c\">（暂无 I2C 事务）</div></div>\n"
"        <div class=\"field\"><label>从机地址（十六进制，如 50 表示 0x50）</label><input id=\"i2cAddr\" placeholder=\"50\"></div>\n"
"        <div class=\"field\"><label>数据（HEX，写操作时使用）</label><input id=\"i2cHex\" placeholder=\"00 01 02\"></div>\n"
"        <div class=\"row\">\n"
"          <select id=\"i2cMode\" class=\"keep\" style=\"flex:1\">\n"
"            <option value=\"w\">写 W</option>\n"
"            <option value=\"r\">读 R</option>\n"
"            <option value=\"wr\">写后读 WR</option>\n"
"          </select>\n"
"          <input id=\"i2cLen\" type=\"number\" value=\"1\" min=\"1\" placeholder=\"读取字节数\" style=\"flex:1\" title=\"读取字节数\">\n"
"        </div>\n"
"        <button class=\"btn primary block\" id=\"i2cSend\">执行</button>\n"
"        <div class=\"result\" id=\"i2cResult\"></div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <!-- ============ 页面 4：系统设置 ============ -->\n"
"  <div class=\"page\" id=\"page-system\">\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>USB-C 角色切换</h2>\n"
"      <div class=\"sub\">一个 USB 口同一时间只能做一件事，选择会被保存</div>\n"
"      <div class=\"seg\" id=\"usbSeg\">\n"
"        <button data-m=\"off\">关闭<small>给本板刷固件</small></button>\n"
"        <button data-m=\"dap\">CMSIS-DAP<small>Keil/pyOCD 调试</small></button>\n"
"        <button data-m=\"ttl\">USB-TTL<small>给目标板下载</small></button>\n"
"      </div>\n"
"      <div class=\"hint\">off：USB 口用于给 NexLink 自身刷固件；dap：无线/USB 调试探针；ttl：虚拟串口连目标板。dap 与 ttl 之间切换后设备会自动重启。</div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>目标板控制</h2>\n"
"      <div class=\"sub\">通过排针上的 NRST（IO12）控制目标板复位</div>\n"
"      <div class=\"row\">\n"
"        <button class=\"btn\" id=\"dutReset\">复位目标板</button>\n"
"        <button class=\"btn ok\" id=\"dutBoot\">复位并进入下载模式</button>\n"
"      </div>\n"
"      <div class=\"hint\">「下载模式」会同时拉低 BOOT：需要先把一个拓展 IO 设为 GPIO 协议并接到目标板的 BOOT/IO0。</div>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>WiFi 配网</h2>\n"
"      <div class=\"sub\">让 NexLink 连接到你的 WiFi 路由器</div>\n"
"      <button class=\"btn block\" id=\"wifiScan\" style=\"margin-top:0\">扫描附近 WiFi</button>\n"
"      <div class=\"wifi-list\" id=\"wifiList\"></div>\n"
"      <div class=\"field\"><label>WiFi 名称 SSID</label><input id=\"wifiSsid\"></div>\n"
"      <div class=\"field\"><label>WiFi 密码</label><input id=\"wifiPass\" type=\"password\"></div>\n"
"      <button class=\"btn primary block\" id=\"wifiConnect\">连接</button>\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"      <h2>固件无线升级（OTA）</h2>\n"
"      <div class=\"sub\">选择新的 nexlink.bin，上传后自动重启；刷坏会自动回滚</div>\n"
"      <input type=\"file\" id=\"otaFile\" accept=\".bin\">\n"
"      <div class=\"progress\"><i id=\"otaBar\"></i></div>\n"
"      <button class=\"btn primary block\" id=\"otaBtn\">刷入固件</button>\n"
"      <div class=\"result\" id=\"otaResult\"></div>\n"
"    </div>\n"
"  </div>\n"
"\n"
"</div>\n"
"\n"
"<!-- 底部导航 -->\n"
"<nav class=\"tabbar\">\n"
"  <button class=\"on\" data-page=\"serial\">\n"
"    <svg viewBox=\"0 0 24 24\"><path d=\"M4 17l6-6-6-6\"/><path d=\"M12 19h8\"/></svg>串口\n"
"  </button>\n"
"  <button data-page=\"status\">\n"
"    <svg viewBox=\"0 0 24 24\"><path d=\"M3 12h4l3 8 4-16 3 8h4\"/></svg>状态\n"
"  </button>\n"
"  <button data-page=\"bus\">\n"
"    <svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"3\" width=\"7\" height=\"7\" rx=\"1\"/><rect x=\"14\" y=\"3\" width=\"7\" height=\"7\" rx=\"1\"/><rect x=\"3\" y=\"14\" width=\"7\" height=\"7\" rx=\"1\"/><path d=\"M17.5 14v7M14 17.5h7\"/></svg>总线\n"
"  </button>\n"
"  <button data-page=\"system\">\n"
"    <svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"3\"/><path d=\"M19.4 15a1.7 1.7 0 00.34 1.87l.06.06a2 2 0 11-2.83 2.83l-.06-.06a1.7 1.7 0 00-1.87-.34 1.7 1.7 0 00-1 1.55V21a2 2 0 11-4 0v-.09A1.7 1.7 0 009 19.4a1.7 1.7 0 00-1.87.34l-.06.06a2 2 0 11-2.83-2.83l.06-.06A1.7 1.7 0 004.6 15a1.7 1.7 0 00-1.55-1H3a2 2 0 110-4h.09A1.7 1.7 0 004.6 9a1.7 1.7 0 00-.34-1.87l-.06-.06a2 2 0 112.83-2.83l.06.06A1.7 1.7 0 009 4.6a1.7 1.7 0 001-1.55V3a2 2 0 114 0v.09a1.7 1.7 0 001 1.55 1.7 1.7 0 001.87-.34l.06-.06a2 2 0 112.83 2.83l-.06.06A1.7 1.7 0 0019.4 9c.2.47.64.8 1.15 1H21a2 2 0 110 4h-.09a1.7 1.7 0 00-1.51 1z\"/></svg>系统\n"
"  </button>\n"
"</nav>\n"
"\n"
"<div id=\"toasts\"></div>\n"
"\n"
"<script>\n"
"\"use strict\";\n"
"var $ = function (id) { return document.getElementById(id); };\n"
"var S = null;                 // 最近一次 /api/status\n"
"var curPage = \"serial\", curBus = \"pwm\";\n"
"var enc = new TextEncoder();\n"
"\n"
"/* ---------------- Toast ---------------- */\n"
"function toast(msg, type) {\n"
"    var d = document.createElement(\"div\");\n"
"    d.className = \"toast\" + (type ? \" \" + type : \"\");\n"
"    d.textContent = msg;\n"
"    $(\"toasts\").appendChild(d);\n"
"    setTimeout(function () { d.style.opacity = \"0\"; d.style.transition = \"opacity .3s\"; }, 2200);\n"
"    setTimeout(function () { d.remove(); }, 2600);\n"
"}\n"
"\n"
"/* ---------------- 页面 / 子标签切换 ---------------- */\n"
"document.querySelectorAll(\".tabbar button\").forEach(function (b) {\n"
"    b.onclick = function () { goPage(b.dataset.page); };\n"
"});\n"
"function goPage(p) {\n"
"    curPage = p;\n"
"    document.querySelectorAll(\".tabbar button\").forEach(function (b) {\n"
"        b.classList.toggle(\"on\", b.dataset.page === p);\n"
"    });\n"
"    document.querySelectorAll(\".page\").forEach(function (pg) {\n"
"        pg.classList.toggle(\"on\", pg.id === \"page-\" + p);\n"
"    });\n"
"}\n"
"document.querySelectorAll(\".subtabs button\").forEach(function (b) {\n"
"    b.onclick = function () {\n"
"        curBus = b.dataset.bus;\n"
"        document.querySelectorAll(\".subtabs button\").forEach(function (x) {\n"
"            x.classList.toggle(\"on\", x === b);\n"
"        });\n"
"        [\"pwm\", \"spi\", \"i2c\"].forEach(function (n) {\n"
"            $(\"bus-\" + n).style.display = n === curBus ? \"\" : \"none\";\n"
"        });\n"
"    };\n"
"});\n"
"\n"
"/* ---------------- 工具函数 ---------------- */\n"
"function fmtUptime(s) {\n"
"    s = Number(s) || 0;\n"
"    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),\n"
"        m = Math.floor(s % 3600 / 60), ss = s % 60;\n"
"    if (d) return d + \" 天 \" + h + \" 小时\";\n"
"    if (h) return h + \" 小时 \" + m + \" 分\";\n"
"    if (m) return m + \" 分 \" + ss + \" 秒\";\n"
"    return ss + \" 秒\";\n"
"}\n"
"function sigText(rssi) {\n"
"    if (!rssi) return \"--\";\n"
"    var q = rssi >= -55 ? \"强\" : rssi >= -70 ? \"中\" : \"弱\";\n"
"    return rssi + \" dBm（\" + q + \"）\";\n"
"}\n"
"function hex2(u) {\n"
"    var s = \"\";\n"
"    for (var i = 0; i < u.length; i++) s += (\"0\" + u[i].toString(16)).slice(-2).toUpperCase() + \" \";\n"
"    return s.trim();\n"
"}\n"
"function nowTs() {\n"
"    var d = new Date(), p = function (n, l) { return String(n).padStart(l || 2, \"0\"); };\n"
"    return p(d.getHours()) + \":\" + p(d.getMinutes()) + \":\" + p(d.getSeconds());\n"
"}\n"
"\n"
"/* ---------------- 串口终端 ---------------- */\n"
"var term = $(\"term\"), firstLine = true;\n"
"function addTerm(cls, text) {\n"
"    if (firstLine) { term.innerHTML = \"\"; firstLine = false; }\n"
"    var d = document.createElement(\"div\");\n"
"    d.className = cls;\n"
"    if ($(\"tsMode\").checked) {\n"
"        var t = document.createElement(\"span\");\n"
"        t.className = \"ts\";\n"
"        t.textContent = nowTs() + \" \";\n"
"        d.appendChild(t);\n"
"    }\n"
"    d.appendChild(document.createTextNode(text));\n"
"    term.appendChild(d);\n"
"    while (term.childNodes.length > 400) term.removeChild(term.firstChild);\n"
"    term.scrollTop = term.scrollHeight;\n"
"}\n"
"function pollData() {\n"
"    fetch(\"/api/data\").then(function (r) { return r.text(); }).then(function (t) {\n"
"        if (!t.length) return;\n"
"        if ($(\"hexMode\").checked) {\n"
"            var bytes = [];\n"
"            for (var i = 0; i < t.length; i++) bytes.push(t.charCodeAt(i) & 0xff);\n"
"            addTerm(\"rx\", \"RX  \" + hex2(bytes));\n"
"        } else {\n"
"            addTerm(\"rx\", \"RX  \" + t.replace(/\\s+$/, \"\"));\n"
"        }\n"
"    }).catch(function () {});\n"
"}\n"
"function doSend() {\n"
"    var t = $(\"sendText\").value;\n"
"    if (!t) return;\n"
"    var payload;\n"
"    if ($(\"hexMode\").checked) {\n"
"        var h = t.replace(/[^0-9a-fA-F]/g, \"\");\n"
"        if (h.length % 2) { toast(\"HEX 长度必须为偶数\", \"err\"); return; }\n"
"        payload = new Uint8Array(h.length / 2);\n"
"        for (var i = 0; i < h.length; i += 2) payload[i / 2] = parseInt(h.substr(i, 2), 16);\n"
"        addTerm(\"tx\", \"TX  \" + hex2(payload));\n"
"    } else {\n"
"        var nl = $(\"newline\").value;\n"
"        if (nl === \"1\") t += \"\\n\";\n"
"        else if (nl === \"2\") t += \"\\r\";\n"
"        else if (nl === \"3\") t += \"\\r\\n\";\n"
"        payload = enc.encode(t);\n"
"        addTerm(\"tx\", \"TX  \" + t.replace(/[\\r\\n]+$/, \"\"));\n"
"    }\n"
"    fetch(\"/api/send\", { method: \"POST\", headers: { \"Content-Type\": \"text/plain\" }, body: payload })\n"
"        .catch(function () { toast(\"发送失败\", \"err\"); });\n"
"    $(\"sendText\").value = \"\";\n"
"}\n"
"$(\"sendBtn\").onclick = doSend;\n"
"$(\"sendText\").addEventListener(\"keydown\", function (e) { if (e.key === \"Enter\") doSend(); });\n"
"$(\"clearBtn\").onclick = function () {\n"
"    term.innerHTML = \"\"; firstLine = true;\n"
"    addTerm(\"sy\", \"（已清空）\");\n"
"};\n"
"$(\"baud\").onchange = function () {\n"
"    fetch(\"/api/baud?b=\" + $(\"baud\").value).then(function () {\n"
"        toast(\"波特率已切换为 \" + $(\"baud\").value);\n"
"    }).catch(function () { toast(\"切换失败\", \"err\"); });\n"
"};\n"
"\n"
"/* ---------------- 状态轮询 ---------------- */\n"
"var missCount = 0;\n"
"function pollStatus() {\n"
"    fetch(\"/api/status\").then(function (r) { return r.json(); }).then(function (d) {\n"
"        S = d; missCount = 0;\n"
"        renderTop(d); renderStatus(d);\n"
"    }).catch(function () {\n"
"        missCount++;\n"
"        $(\"topBadge\").className = \"badge err\";\n"
"        $(\"topBadge\").textContent = \"离线 · 重连中\";\n"
"        if (missCount >= 2 && curPage === \"status\")\n"
"            $(\"sWifi\").innerHTML = '<span class=\"bad\">连不上设备</span>';\n"
"    });\n"
"}\n"
"function wifiLabel(d) {\n"
"    if (d.wifi === \"connected\") return [\"ok\", \"已连接路由器\"];\n"
"    if (d.wifi === \"ap\") return [\"warn\", \"AP 热点模式\"];\n"
"    return [\"err\", \"未连接\"];\n"
"}\n"
"function renderTop(d) {\n"
"    var w = wifiLabel(d);\n"
"    $(\"topBadge\").className = \"badge \" + w[0];\n"
"    $(\"topBadge\").textContent = w[1];\n"
"}\n"
"function usbDesc(m) {\n"
"    return m === \"dap\" ? \"CMSIS-DAP 调试探针\" : m === \"ttl\" ? \"USB-TTL 虚拟串口\" : \"关闭\";\n"
"}\n"
"function renderStatus(d) {\n"
"    var w = wifiLabel(d);\n"
"    $(\"sWifi\").innerHTML = '<span class=\"' + (w[0] === \"ok\" ? \"good\" : w[0] === \"warn\" ? \"hot\" : \"bad\") + '\">' + w[1] + \"</span>\";\n"
"    $(\"sSsid\").textContent = d.ssid && d.ssid !== \"-\" ? d.ssid : \"--\";\n"
"    $(\"sRssi\").textContent = d.wifi === \"connected\" ? sigText(d.rssi) : \"--\";\n"
"    $(\"sIp\").textContent = d.ip || \"--\";\n"
"    $(\"sUptime\").textContent = fmtUptime(d.uptime);\n"
"    $(\"sTcp\").textContent = d.tcp + \" 个连接\";\n"
"    $(\"sBaud\").textContent = d.baud;\n"
"    $(\"sRx\").textContent = d.rx;\n"
"    $(\"sTx\").textContent = d.tx;\n"
"    $(\"sRst\").textContent = d.rst;\n"
"    $(\"sUsb\").textContent = usbDesc(d.usbmode);\n"
"    $(\"sMenu\").textContent = d.menu || \"--\";\n"
"    $(\"sSlot\").textContent = d.slot;\n"
"    $(\"sOta\").textContent = d.ota === \"pending\" ? \"待确认（异常将回滚）\" : \"正常\";\n"
"\n"
"    // USB 详细计数\n"
"    var html = \"\";\n"
"    if (d.usbmode === \"dap\") {\n"
"        html =\n"
"            kv(\"主机已配置\", d.dapcfg ? '<span class=\"good\">是 (' + d.dapcfg + ')</span>' : '<span class=\"hot\">否</span>') +\n"
"            kv(\"DAP 命令包\", d.daprx) + kv(\"DAP 响应包\", d.daptx);\n"
"    } else if (d.usbmode === \"ttl\") {\n"
"        html = kv(\"发往目标\", d.ttl_to_dut) + kv(\"发往主机\", d.ttl_to_host);\n"
"    }\n"
"    if (d.debug) {\n"
"        html += kv(\"探针占用 IO\", '<span class=\"hot\">' + (d.debug_io || \"--\") + \"</span>\");\n"
"    }\n"
"    $(\"usbExtra\").innerHTML = html;\n"
"\n"
"    // 波特率下拉与 USB 分段保持同步\n"
"    var baudSel = $(\"baud\");\n"
"    if (document.activeElement !== baudSel) baudSel.value = String(d.baud);\n"
"    document.querySelectorAll(\"#usbSeg button\").forEach(function (b) {\n"
"        b.classList.toggle(\"on\", b.dataset.m === d.usbmode);\n"
"    });\n"
"}\n"
"function kv(k, v) {\n"
"    return '<div class=\"kv\"><span class=\"k\">' + k + '</span><span class=\"v mono\">' + v + \"</span></div>\";\n"
"}\n"
"\n"
"/* ---------------- OLED 预览 ---------------- */\n"
"var lcdCv = $(\"lcdCv\"), lctx = lcdCv.getContext(\"2d\");\n"
"var offCv = document.createElement(\"canvas\");\n"
"offCv.width = 128; offCv.height = 64;\n"
"var offCtx = offCv.getContext(\"2d\");\n"
"var lcdImg = offCtx.createImageData(128, 64);\n"
"function sizeLcd() {\n"
"    var dpr = window.devicePixelRatio || 1, w = lcdCv.clientWidth || 320;\n"
"    lcdCv.width = Math.round(w * dpr);\n"
"    lcdCv.height = Math.round(w * dpr * 64 / 128);\n"
"}\n"
"sizeLcd(); window.addEventListener(\"resize\", sizeLcd);\n"
"function pollLcd() {\n"
"    if (curPage !== \"status\") return;\n"
"    fetch(\"/api/lcd\").then(function (r) { return r.arrayBuffer(); }).then(function (ab) {\n"
"        var u = new Uint8Array(ab), d = lcdImg.data;\n"
"        for (var pg = 0; pg < 8; pg++) {\n"
"            for (var x = 0; x < 128; x++) {\n"
"                var b = u[pg * 128 + x] || 0;\n"
"                for (var bit = 0; bit < 8; bit++) {\n"
"                    var j = ((pg * 8 + bit) * 128 + x) * 4;\n"
"                    var c = ((b >> bit) & 1) ? 255 : 0;\n"
"                    d[j] = c; d[j + 1] = c; d[j + 2] = c; d[j + 3] = 255;\n"
"                }\n"
"            }\n"
"        }\n"
"        offCtx.putImageData(lcdImg, 0, 0);\n"
"        lctx.setTransform(lcdCv.width / 128, 0, 0, lcdCv.height / 64, 0, 0);\n"
"        lctx.imageSmoothingEnabled = false;\n"
"        lctx.clearRect(0, 0, 128, 64);\n"
"        lctx.drawImage(offCv, 0, 0);\n"
"    }).catch(function () {});\n"
"}\n"
"\n"
"/* OLED 按键（含长按） */\n"
"function bindBtn(el, b) {\n"
"    var timer = null, longFired = false;\n"
"    el.addEventListener(\"mousedown\", start);\n"
"    el.addEventListener(\"touchstart\", function (e) { e.preventDefault(); start(); }, { passive: false });\n"
"    function start() {\n"
"        longFired = false;\n"
"        timer = setTimeout(function () {\n"
"            longFired = true; timer = null;\n"
"            fetch(\"/api/btn?b=\" + b + \"&a=long\");\n"
"        }, 550);\n"
"    }\n"
"    function end() {\n"
"        if (timer) { clearTimeout(timer); timer = null; }\n"
"        if (!longFired) fetch(\"/api/btn?b=\" + b + \"&a=press\");\n"
"    }\n"
"    el.addEventListener(\"mouseup\", end);\n"
"    el.addEventListener(\"mouseleave\", function () { if (timer) { clearTimeout(timer); timer = null; } });\n"
"    el.addEventListener(\"touchend\", function (e) { e.preventDefault(); end(); }, { passive: false });\n"
"}\n"
"bindBtn($(\"b1\"), 1); bindBtn($(\"b2\"), 2); bindBtn($(\"b3\"), 3);\n"
"\n"
"/* ---------------- USB 角色 / 目标复位 ---------------- */\n"
"document.querySelectorAll(\"#usbSeg button\").forEach(function (b) {\n"
"    b.onclick = function () {\n"
"        var m = b.dataset.m;\n"
"        if (S && S.usbmode === m) return;\n"
"        if (!confirm(\"切换 USB 角色为「\" + usbDesc(m) + \"」？\\ndap 与 ttl 之间切换设备会自动重启。\")) return;\n"
"        fetch(\"/api/usb_mode?m=\" + m).then(function (r) { return r.json(); }).then(function (d) {\n"
"            if (d.reboot) toast(\"设备正在重启…\", \"ok\");\n"
"            setTimeout(pollStatus, 2000);\n"
"        }).catch(function () { toast(\"切换失败\", \"err\"); });\n"
"    };\n"
"});\n"
"$(\"dutReset\").onclick = function () {\n"
"    fetch(\"/api/dut/reset?ms=25\").then(function (r) { return r.json(); }).then(function () {\n"
"        toast(\"目标板已复位\", \"ok\");\n"
"    }).catch(function () { toast(\"复位失败\", \"err\"); });\n"
"};\n"
"$(\"dutBoot\").onclick = function () {\n"
"    fetch(\"/api/dut/reset?ms=25&boot=1\").then(function (r) { return r.json(); }).then(function () {\n"
"        toast(\"目标板已进入下载模式\", \"ok\");\n"
"    }).catch(function () { toast(\"操作失败\", \"err\"); });\n"
"};\n"
"\n"
"/* ---------------- WiFi 配网 ---------------- */\n"
"$(\"wifiScan\").onclick = function () {\n"
"    var box = $(\"wifiList\");\n"
"    box.innerHTML = '<button disabled>扫描中…</button>';\n"
"    fetch(\"/api/wifi\").then(function (r) { return r.json(); }).then(function (list) {\n"
"        box.innerHTML = \"\";\n"
"        if (!list || !list.length) { box.innerHTML = '<button disabled>没有发现网络</button>'; return; }\n"
"        list.forEach(function (a) {\n"
"            var b = document.createElement(\"button\");\n"
"            b.className = \"btn\";\n"
"            b.innerHTML = \"<span>\" + (a.ssid || \"(隐藏网络)\") + '</span><span class=\"sig\">' + a.rssi + \" dBm</span>\";\n"
"            b.onclick = function () { $(\"wifiSsid\").value = a.ssid || \"\"; };\n"
"            box.appendChild(b);\n"
"        });\n"
"    }).catch(function () { box.innerHTML = '<button disabled>扫描失败</button>'; });\n"
"};\n"
"$(\"wifiConnect\").onclick = function () {\n"
"    var s = $(\"wifiSsid\").value;\n"
"    if (!s) { toast(\"请填写 WiFi 名称\", \"err\"); return; }\n"
"    var body = \"ssid=\" + encodeURIComponent(s) + \"&pass=\" + encodeURIComponent($(\"wifiPass\").value);\n"
"    fetch(\"/api/wifi\", {\n"
"        method: \"POST\",\n"
"        headers: { \"Content-Type\": \"application/x-www-form-urlencoded\" },\n"
"        body: body\n"
"    }).then(function () { toast(\"正在连接，稍后查看状态页\", \"ok\"); })\n"
"      .catch(function () { toast(\"提交失败\", \"err\"); });\n"
"};\n"
"\n"
"/* ---------------- OTA ---------------- */\n"
"$(\"otaBtn\").onclick = function () {\n"
"    var f = $(\"otaFile\").files[0];\n"
"    if (!f) { toast(\"请先选择 .bin 固件文件\", \"err\"); return; }\n"
"    if (!confirm(\"确定刷入 \" + f.name + \" ？\\n刷入后设备自动重启，失败会自动回滚。\")) return;\n"
"    var bar = $(\"otaBar\"), res = $(\"otaResult\");\n"
"    res.className = \"result\"; res.textContent = \"上传中 0%\";\n"
"    var x = new XMLHttpRequest();\n"
"    x.open(\"POST\", \"/api/ota\");\n"
"    x.upload.onprogress = function (e) {\n"
"        if (e.lengthComputable) {\n"
"            var p = Math.round(e.loaded / e.total * 100);\n"
"            bar.style.width = p + \"%\";\n"
"            res.textContent = \"上传中 \" + p + \"%\";\n"
"        }\n"
"    };\n"
"    x.onload = function () {\n"
"        bar.style.width = \"100%\";\n"
"        res.className = \"result ok\";\n"
"        res.textContent = \"HTTP \" + x.status + \"，设备正在重启…\";\n"
"    };\n"
"    x.onerror = function () {\n"
"        res.className = \"result\";\n"
"        res.textContent = \"连接已断开（设备可能正在重启），请稍后查看状态页\";\n"
"    };\n"
"    x.send(f);\n"
"};\n"
"\n"
"/* ---------------- 总线：PWM ---------------- */\n"
"function sizeWave(cv) {\n"
"    var dpr = window.devicePixelRatio || 1;\n"
"    cv.width = Math.round(cv.clientWidth * dpr);\n"
"    cv.height = Math.round(cv.clientHeight * dpr);\n"
"}\n"
"function drawPwmWave(duty) {\n"
"    var cv = $(\"pwmWave\"); sizeWave(cv);\n"
"    var x = cv.getContext(\"2d\"), W = cv.width, H = cv.height, dpr = window.devicePixelRatio || 1;\n"
"    x.fillStyle = \"#070b11\"; x.fillRect(0, 0, W, H);\n"
"    x.strokeStyle = \"#223044\"; x.beginPath();\n"
"    x.moveTo(0, H / 2); x.lineTo(W, H / 2); x.stroke();\n"
"    var cycles = 10, cw = W / cycles, top = 6 * dpr, bot = H - 6 * dpr, hi = cw * Math.max(0, Math.min(1, duty));\n"
"    x.strokeStyle = \"#34d399\"; x.lineWidth = Math.max(1.5, dpr);\n"
"    x.beginPath(); x.moveTo(0, bot);\n"
"    for (var i = 0; i < cycles; i++) {\n"
"        var px = i * cw;\n"
"        x.lineTo(px, top); x.lineTo(px + hi, top);\n"
"        x.lineTo(px + hi, bot); x.lineTo(px + cw, bot);\n"
"    }\n"
"    x.stroke();\n"
"}\n"
"function pollPwm() {\n"
"    fetch(\"/api/pwm\").then(function (r) { return r.json(); }).then(function (d) {\n"
"        $(\"pwmFreq\").textContent = Number(d.freq).toFixed(2) + \" Hz\";\n"
"        $(\"pwmDuty\").textContent = Number(d.duty).toFixed(1) + \" %\";\n"
"        drawPwmWave(Number(d.duty) / 100);\n"
"    }).catch(function () {});\n"
"}\n"
"function pwmOut(en) {\n"
"    var body = \"freq=\" + $(\"pwmOFreq\").value + \"&duty=\" + $(\"pwmODuty\").value + \"&enable=\" + en;\n"
"    fetch(\"/api/pwm/out\", {\n"
"        method: \"POST\",\n"
"        headers: { \"Content-Type\": \"application/x-www-form-urlencoded\" },\n"
"        body: body\n"
"    }).then(function (r) { return r.json(); }).then(function (d) {\n"
"        var res = $(\"pwmResult\");\n"
"        res.className = \"result ok\";\n"
"        res.textContent = d.running ? \"正在输出 \" + Math.round(d.freq) + \" Hz，占空比 \" + Math.round(d.duty) + \"%\" : \"已停止输出\";\n"
"    }).catch(function () {\n"
"        var res = $(\"pwmResult\"); res.className = \"result err\"; res.textContent = \"操作失败\";\n"
"    });\n"
"}\n"
"$(\"pwmStart\").onclick = function () { pwmOut(1); };\n"
"$(\"pwmStop\").onclick = function () { pwmOut(0); };\n"
"\n"
"/* ---------------- 总线：SPI ---------------- */\n"
"function drawSpiWave(tx, rx) {\n"
"    var cv = $(\"spiWave\"); sizeWave(cv);\n"
"    var x = cv.getContext(\"2d\"), W = cv.width, H = cv.height, dpr = window.devicePixelRatio || 1;\n"
"    x.fillStyle = \"#070b11\"; x.fillRect(0, 0, W, H);\n"
"    if (!tx || !tx.length) {\n"
"        x.fillStyle = \"#5d687c\"; x.font = 11 * dpr + \"px monospace\";\n"
"        x.fillText(\"（暂无 SPI 事务）\", 10 * dpr, H / 2);\n"
"        return;\n"
"    }\n"
"    var bytes = Math.min(tx.length, 4), bits = bytes * 8, bw = W / bits;\n"
"    var sy = H * 0.28, my = H * 0.72, amp = H * 0.13;\n"
"    x.strokeStyle = \"#34d399\"; x.lineWidth = Math.max(1, dpr);\n"
"    x.beginPath(); x.moveTo(0, sy + amp);\n"
"    for (var i = 0; i < bits; i++) {\n"
"        var px = i * bw, x2 = Math.min(px + bw, W);\n"
"        x.lineTo(px, sy + amp); x.lineTo(px, sy - amp);\n"
"        x.lineTo(x2, sy - amp); x.lineTo(x2, sy + amp);\n"
"    }\n"
"    x.stroke();\n"
"    x.fillStyle = \"#34d399\"; x.font = 9 * dpr + \"px monospace\";\n"
"    x.fillText(\"MOSI\", 4 * dpr, sy - amp - 2 * dpr);\n"
"    x.strokeStyle = \"#fbbf24\"; x.beginPath();\n"
"    var lvl = my + amp; x.moveTo(0, lvl);\n"
"    for (var j = 0; j < bits; j++) {\n"
"        var bi = Math.floor(j / 8), bk = 7 - (j % 8);\n"
"        var bit = (rx && rx[bi] != null) ? (rx[bi] >> bk) & 1 : 0;\n"
"        var tg = bit ? my - amp : my + amp, qx = j * bw, q2 = Math.min(qx + bw, W);\n"
"        x.lineTo(qx, lvl); x.lineTo(qx, tg); lvl = tg;\n"
"        x.lineTo(q2, tg);\n"
"    }\n"
"    x.stroke();\n"
"    x.fillStyle = \"#fbbf24\";\n"
"    x.fillText(\"MISO\", 4 * dpr, my + amp + 11 * dpr);\n"
"}\n"
"function pollSpi() {\n"
"    fetch(\"/api/spi\").then(function (r) { return r.json(); }).then(function (d) {\n"
"        $(\"spiState\").textContent = d.running ? \"抓包中 · \" + d.count + \" 个事务\" : \"未启动\";\n"
"        fetch(\"/api/pins\").then(function (r) { return r.json(); }).then(function (p) {\n"
"            var m = {};\n"
"            for (var i = 0; i < p.order.length; i++) m[p.order[i]] = p.pos_io[i];\n"
"            $(\"spiPins\").textContent = \"SCK=\" + (m.SCK || \"?\") + \" MOSI=\" + (m.MOSI || \"?\") +\n"
"                \" MISO=\" + (m.MISO || \"?\") + \" CS=\" + (m.CS || \"-\");\n"
"        }).catch(function () {});\n"
"        var log = $(\"spiLog\"); log.innerHTML = \"\";\n"
"        if (d.history && d.history.length) {\n"
"            var h = d.history;\n"
"            for (var i = Math.max(0, h.length - 6); i < h.length; i++) {\n"
"                var t = h[i];\n"
"                var a = document.createElement(\"div\"); a.className = \"mosi\";\n"
"                a.textContent = \">> \" + hex2(t.mosi);\n"
"                var b = document.createElement(\"div\"); b.className = \"miso\";\n"
"                b.textContent = \"<< \" + hex2(t.miso);\n"
"                log.appendChild(a); log.appendChild(b);\n"
"            }\n"
"            log.scrollTop = log.scrollHeight;\n"
"            drawSpiWave(h[h.length - 1].mosi, h[h.length - 1].miso);\n"
"        } else {\n"
"            log.innerHTML = '<div style=\"color:#5d687c\">（暂无 SPI 事务）</div>';\n"
"            drawSpiWave(null, null);\n"
"        }\n"
"    }).catch(function () {});\n"
"}\n"
"$(\"spiSend\").onclick = function () {\n"
"    var h = $(\"spiHex\").value.replace(/[^0-9a-fA-F]/g, \"\");\n"
"    var res = $(\"spiResult\");\n"
"    if (!h) { res.className = \"result err\"; res.textContent = \"请输入要发送的 HEX 数据\"; return; }\n"
"    if (h.length % 2) { res.className = \"result err\"; res.textContent = \"HEX 长度必须为偶数\"; return; }\n"
"    var body = \"hex=\" + h + \"&mode=\" + $(\"spiMode\").value + \"&clk=\" + $(\"spiClk\").value;\n"
"    fetch(\"/api/spi/send\", {\n"
"        method: \"POST\",\n"
"        headers: { \"Content-Type\": \"application/x-www-form-urlencoded\" },\n"
"        body: body\n"
"    }).then(function (r) { return r.json(); }).then(function (d) {\n"
"        res.className = \"result \" + (d.ok ? \"ok\" : \"err\");\n"
"        res.textContent = d.ok ? \"MISO 返回：\" + hex2(d.rx) : \"错误：\" + (d.err || JSON.stringify(d));\n"
"    }).catch(function (e) {\n"
"        res.className = \"result err\"; res.textContent = \"请求失败：\" + e;\n"
"    });\n"
"};\n"
"\n"
"/* ---------------- 总线：I2C ---------------- */\n"
"function pollI2c() {\n"
"    fetch(\"/api/i2c\").then(function (r) { return r.json(); }).then(function (d) {\n"
"        $(\"i2cState\").textContent = d.running ? \"抓包中 · \" + d.count + \" 个事务\" : \"未启动\";\n"
"        var log = $(\"i2cLog\"); log.innerHTML = \"\";\n"
"        if (d.history && d.history.length) {\n"
"            var h = d.history;\n"
"            for (var i = Math.max(0, h.length - 8); i < h.length; i++) {\n"
"                var t = h[i];\n"
"                var a = document.createElement(\"div\");\n"
"                a.className = t.read ? \"i2cr\" : \"i2cw\";\n"
"                var addr = (\"0\" + t.addr.toString(16)).slice(-2).toUpperCase();\n"
"                a.textContent = \"0x\" + addr + (t.read ? \"R \" : \"W \") + hex2(t.data);\n"
"                log.appendChild(a);\n"
"            }\n"
"            log.scrollTop = log.scrollHeight;\n"
"        } else {\n"
"            log.innerHTML = '<div style=\"color:#5d687c\">（暂无 I2C 事务）</div>';\n"
"        }\n"
"    }).catch(function () {});\n"
"}\n"
"$(\"i2cSend\").onclick = function () {\n"
"    var res = $(\"i2cResult\");\n"
"    var body = \"addr=\" + encodeURIComponent($(\"i2cAddr\").value) +\n"
"        \"&hex=\" + $(\"i2cHex\").value.replace(/\\s/g, \"\") +\n"
"        \"&mode=\" + $(\"i2cMode\").value + \"&len=\" + $(\"i2cLen\").value;\n"
"    fetch(\"/api/i2c/send\", {\n"
"        method: \"POST\",\n"
"        headers: { \"Content-Type\": \"application/x-www-form-urlencoded\" },\n"
"        body: body\n"
"    }).then(function (r) { return r.json(); }).then(function (d) {\n"
"        res.className = \"result \" + (d.ok ? \"ok\" : \"err\");\n"
"        res.textContent = d.ok\n"
"            ? (d.rx && d.rx.length ? \"读到数据：\" + hex2(d.rx) : \"写入成功\")\n"
"            : \"错误：\" + d.err;\n"
"    }).catch(function () {\n"
"        res.className = \"result err\"; res.textContent = \"请求失败\";\n"
"    });\n"
"};\n"
"\n"
"/* ---------------- 轮询调度 ---------------- */\n"
"function pollBus() {\n"
"    if (curPage !== \"bus\") return;\n"
"    if (curBus === \"pwm\") pollPwm();\n"
"    else if (curBus === \"spi\") pollSpi();\n"
"    else pollI2c();\n"
"}\n"
"drawPwmWave(0);\n"
"drawSpiWave(null, null);\n"
"pollStatus();\n"
"setInterval(pollStatus, 2000);\n"
"setInterval(pollData, 200);\n"
"setInterval(pollLcd, 1500);\n"
"setInterval(pollBus, 1000);\n"
"</script>\n"
"</body>\n"
"</html>";

/* GET / - serve HTML page */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* HTML lives in firmware flash; never let the browser serve a stale copy */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, s_index_html, -1);
}

/* GET /api/status - return JSON status */
static esp_err_t status_api_handler(httpd_req_t *req)
{
    char ipbuf[16] = {0};
    wifi_manager_get_ip_str(ipbuf, sizeof(ipbuf));
    wifi_state_t st = wifi_manager_get_state();
    const char *wstate = "disconnected";
    if (st == WIFI_STATE_CONNECTED_STA) wstate = "connected";
    else if (st == WIFI_STATE_AP_MODE) wstate = "ap";

    uint32_t baud = 0;
    uart_get_baudrate(UART1_PORT_NUM, &baud);

    /* Radio details straight from the driver: actual SSID + RSSI in STA. */
    char ssid[40] = "-";
    int rssi = 0;
    if (st == WIFI_STATE_CONNECTED_STA) {
        wifi_ap_record_t ar;
        if (esp_wifi_sta_get_ap_info(&ar) == ESP_OK) {
            ar.ssid[sizeof(ar.ssid) - 1] = '\0';   /* driver may fill it all */
            snprintf(ssid, sizeof ssid, "%s", (char *)ar.ssid);
            rssi = ar.rssi;
        }
    } else if (st == WIFI_STATE_AP_MODE) {
        snprintf(ssid, sizeof ssid, "AP-mode");
    }

    /* Which OTA slot is running, and has it been confirmed healthy? */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t ostate;
    const char *ostate_s = "ok";
    if (run && esp_ota_get_state_partition(run, &ostate) == ESP_OK &&
        ostate == ESP_OTA_IMG_PENDING_VERIFY)
        ostate_s = "pending";

    /* Which dashboard screen is showing. There is no cursor any more: the
     * dashboard shows one screen in full, so "sel" is always 0. */
    extern int dash_get_screen(void);
    int page = dash_get_screen();
    int sel = 0;

    extern bool dap_usb_is_started(void);
    extern uint32_t dap_usb_configured_count(void);
    extern const char *main_boot_reason(void);
    static const char *usbmode_s[3] = { "off", "dap", "ttl" };
    uint8_t usbmode = pin_config_usb_mode();

    static volatile uint32_t s_api_hits;
    s_api_hits++;

    /* Which expansion IOs the debug probe took over. Surfaced so that a
     * stopped SPI/I2C monitor has a visible cause instead of looking like a
     * spontaneous failure. */
    char debug_io[24];
    debug_pins_report(debug_io, sizeof(debug_io));

    /* Menu state as text. The numeric "page" id cannot distinguish an editor
     * from the list it was opened from, which makes the key model impossible
     * to verify on a board whose screen nobody can see. */
    char menustate[48];
    dash_get_state(menustate, sizeof(menustate));

    char json[768];
    snprintf(json, sizeof(json),
             "{\"wifi\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,"
             "\"baud\":%lu,\"rx\":%lu,\"tx\":%lu,"
             "\"uptime\":%lu,\"tcp\":%d,\"http\":%lu,\"page\":%d,\"sel\":%d,"
             "\"usbdap\":%d,\"usbmode\":\"%s\",\"usb_ttl\":%d,\"rst\":\"%s\","
             "\"dapcfg\":%lu,\"daprx\":%lu,\"daptx\":%lu,"
             "\"ttl_to_dut\":%lu,\"ttl_to_host\":%lu,"
             "\"debug\":%d,\"debug_io\":\"%s\","
             "\"menu\":\"%s\","
             "\"slot\":\"%s\",\"ota\":\"%s\"}",
             wstate, ipbuf, ssid, rssi, (unsigned long)baud,
             (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count(),
             (unsigned long)(esp_timer_get_time() / 1000000),
             tcp_server_client_count(), (unsigned long)s_api_hits, page, sel,
             dap_usb_is_started() ? 1 : 0,
             usbmode_s[usbmode], usb_ttl_is_started() ? 1 : 0,
             main_boot_reason(),
             (unsigned long)dap_usb_configured_count(),
             (unsigned long)dap_usb_get_rx_packets(),
             (unsigned long)dap_usb_get_tx_packets(),
             (unsigned long)usb_ttl_get_to_dut(),
             (unsigned long)usb_ttl_get_to_host(),
             debug_pins_claimed() ? 1 : 0, debug_io,
             menustate,
             run ? run->label : "?", ostate_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/data - drain received serial data as plain text */
static esp_err_t data_api_handler(httpd_req_t *req)
{
    /* Static buffer so MCP / pollers can drain up to 4KB per call without
     * stressing the httpd stack. */
    static uint8_t buf[4096];
    size_t n = serial_bridge_read(buf, sizeof(buf) - 1, 0);
    if (n == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    }
    buf[n] = '\0';
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, (char *)buf, n);
}

/* GET /api/mcp/data - drain MCP's independent serial buffer (no web/TCP competition) */
static esp_err_t mcp_data_handler(httpd_req_t *req)
{
    static uint8_t buf[4096];
    size_t n = serial_bridge_read_mcp(buf, sizeof(buf) - 1, 0);
    if (n == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    }
    buf[n] = '\0';
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, (char *)buf, n);
}

/* GET /api/btn?b=1&a=press - simulate button */
static esp_err_t btn_api_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char bv[8] = {0}, av[8] = {0};
    httpd_query_key_value(query, "b", bv, sizeof(bv));
    httpd_query_key_value(query, "a", av, sizeof(av));
    if (bv[0]) {
        int btn = atoi(bv);
        dash_simulate_button(btn, av[0] ? av : "press");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capture - timestamped, direction-tagged capture history  */
/*                                                                    */
/*  The raw /api/data streams are byte pipes: no timing, no direction, */
/*  and each poll consumes what it reads. This endpoint serves the     */
/*  bounded history kept by capture.c instead, so a client can page    */
/*  through it, or export the whole log as CSV.                        */
/*                                                                    */
/*    ?since=N   first chunk seq to return (client's paging cursor)    */
/*    ?max=N     cap on chunks in this response (default 64)           */
/*    ?fmt=      text (default) | hex | csv                            */
/*    ?want=     chunks (default) | meta (summary only, no payload)    */
/*                                                                    */
/*  Clients page with the returned "next" value; if it lags the        */
/*  current "next" the log overflowed and older chunks are gone.       */
/* ------------------------------------------------------------------ */
#define CAP_BATCH     64
#define CAP_CSV_MAX   96
/* Hex mode carries two characters per payload byte, so the same number of
 * chunks produces a response roughly twice as large as text mode; the batch is
 * capped lower for that reason. This is a batch limit, NOT a buffer size - the
 * hex scratch buffer is sized from the payload maximum (CAP_HEX_BUF). */
#define CAP_HEX_BATCH 48
/* A chunk's payload is at most CAPTURE_PAYLOAD_MAX bytes, so its hex form is at
 * most 2x that plus termination: sizing it here means the hex branch can never
 * silently emit a short string. */
#define CAP_HEX_BUF (CAPTURE_PAYLOAD_MAX * 2 + 4)

static esp_err_t capture_get_handler(httpd_req_t *req)
{
    static capture_chunk_t chunks[CAP_BATCH];
    static char js[8192];
    static char hexbuf[CAP_HEX_BUF];
    static char csvbuf[1024];

    char query[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        query[0] = '\0';

    char p_since[12] = {0}, p_max[12] = {0}, p_fmt[8] = {0}, p_want[12] = {0};
    httpd_query_key_value(query, "since", p_since, sizeof(p_since));
    httpd_query_key_value(query, "max",   p_max,   sizeof(p_max));
    httpd_query_key_value(query, "fmt",   p_fmt,   sizeof(p_fmt));
    httpd_query_key_value(query, "want",  p_want,  sizeof(p_want));

    int v = atoi(p_max);
    size_t req_max = (v > 0) ? (size_t)v : CAP_BATCH;
    if (req_max > CAP_BATCH) req_max = CAP_BATCH;

    bool want_csv = !strcasecmp(p_fmt, "csv");
    bool want_hex = !strcasecmp(p_fmt, "hex");
    bool meta_only = !strcasecmp(p_want, "meta");
    if (want_csv && req_max > CAP_CSV_MAX) req_max = CAP_CSV_MAX;
    if (want_hex && req_max > CAP_HEX_BATCH) req_max = CAP_HEX_BATCH;

    uint32_t since = (uint32_t)strtoul(p_since, NULL, 10);
    uint32_t next = since;
    size_t n = meta_only ? 0 : capture_read(since, chunks, req_max, &next);

    if (want_csv) {
        httpd_resp_set_type(req, "text/csv; charset=utf-8");
        const char *hdr =
            "# NexLink capture log: seq,uptime_ms,delta_ms,dir,len,data\r\n"
            "# dir: rx = target -> host, tx = host -> target\r\n"
            "# data: hex bytes when hex=1, otherwise printable ASCII with . for non-printable\r\n"
            "seq,uptime_ms,delta_ms,dir,len,data\r\n";
        if (httpd_resp_send_chunk(req, hdr, strlen(hdr)) != ESP_OK)
            return ESP_FAIL;

        for (size_t i = 0; i < n; i++) {
            const capture_chunk_t *c = &chunks[i];
            int64_t prev_us = (i > 0) ? chunks[i - 1].timestamp_us
                                      : (int64_t)c->timestamp_us;
            int64_t delta_ms = (c->timestamp_us - prev_us) / 1000;
            size_t p = 0;

            p += (size_t)snprintf(csvbuf + p, sizeof(csvbuf) - p,
                                  "%lu,%lld,%lld,%s,%u,",
                                  (unsigned long)c->seq,
                                  (long long)(c->timestamp_us / 1000),
                                  (long long)delta_ms,
                                  c->dir ? "tx" : "rx",
                                  (unsigned)c->len);
            for (size_t k = 0; k < c->len && p + 4 < sizeof(csvbuf); k++) {
                uint8_t b = c->data[k];
                if (want_hex) {
                    p += (size_t)snprintf(csvbuf + p, sizeof(csvbuf) - p, "%02X", b);
                } else {
                    csvbuf[p++] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
            }
            csvbuf[p++] = '\r';
            csvbuf[p++] = '\n';
            csvbuf[p]   = '\0';

            if (httpd_resp_send_chunk(req, csvbuf, p) != ESP_OK)
                return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    httpd_resp_set_type(req, "application/json");
    size_t off = (size_t)snprintf(js, sizeof(js),
        "{\"oldest\":%lu,\"next\":%lu,\"count\":%u,\"cap\":%u,\"bytes\":%u,"
        "\"dropped\":%s,\"returned\":%u,\"fmt\":\"%s\",\"chunks\":[",
        (unsigned long)capture_oldest_seq(),
        (unsigned long)capture_next_seq(),
        (unsigned)capture_count(),
        (unsigned)capture_capacity(),
        (unsigned)capture_bytes(),
        capture_dropped() ? "true" : "false",
        (unsigned)n,
        want_hex ? "hex" : "text");

    size_t emitted = 0;
    bool   cut = false;

    for (size_t i = 0; i < n; i++) {
        const capture_chunk_t *c = &chunks[i];
        if (off > sizeof(js) - 1024) { cut = true; break; }   /* keep room for the tail */

        if (want_hex) {
            size_t hp = 0;
            for (size_t k = 0; k < c->len && hp + 2 < sizeof(hexbuf); k++)
                hp += (size_t)snprintf(hexbuf + hp, sizeof(hexbuf) - hp, "%02X", c->data[k]);
            hexbuf[hp] = '\0';
            off += (size_t)snprintf(js + off, sizeof(js) - off,
                "%s{\"seq\":%lu,\"t\":%lld,\"dir\":\"%s\",\"len\":%u,\"trunc\":%s,\"hex\":\"%s\"}",
                emitted ? "," : "", (unsigned long)c->seq,
                (long long)(c->timestamp_us / 1000), c->dir ? "tx" : "rx",
                (unsigned)c->len, c->truncated ? "true" : "false", hexbuf);
        } else {
            size_t k = 0;
            off += (size_t)snprintf(js + off, sizeof(js) - off,
                "%s{\"seq\":%lu,\"t\":%lld,\"dir\":\"%s\",\"len\":%u,\"trunc\":%s,\"text\":\"",
                emitted ? "," : "", (unsigned long)c->seq,
                (long long)(c->timestamp_us / 1000), c->dir ? "tx" : "rx",
                (unsigned)c->len, c->truncated ? "true" : "false");
            /* The bound already reserves room for a 6-char \\uXXXX escape and
             * the three closing characters, so every branch below stays in
             * bounds and the object is ALWAYS closed. Emitting a half-open
             * string here would hand the client invalid JSON, which is worse
             * than truncating the payload. */
            for (; k < c->len && off < sizeof(js) - 32; k++) {
                uint8_t b = c->data[k];
                const char *esc = NULL;
                switch (b) {
                case '\\': esc = "\\\\"; break;
                case '"':  esc = "\\\""; break;
                case '\n': esc = "\\n";  break;
                case '\r': esc = "\\r";  break;
                case '\t': esc = "\\t";  break;
                default: break;
                }
                if (esc) {
                    js[off++] = esc[0];
                    js[off++] = esc[1];
                } else if (b >= 0x20 && b < 0x7F) {
                    js[off++] = (char)b;
                } else {
                    off += (size_t)snprintf(js + off, sizeof(js) - off, "\\u%04X", b);
                }
            }
            if (k < c->len) { js[off++] = '.'; js[off++] = '.'; js[off++] = '.'; }
            js[off++] = '"';
            js[off++] = '}';
            js[off]   = '\0';
        }
        emitted++;
    }

    /* Say so when the batch was clipped, so a paging client does not mistake a
     * short response for the end of the log: it should keep following "next". */
    if (cut)
        off += (size_t)snprintf(js + off, sizeof(js) - off,
                                "%s{\"cut\":true}", emitted ? "," : "");
    if (off > sizeof(js) - 4) off = sizeof(js) - 4;
    off += (size_t)snprintf(js + off, sizeof(js) - off, "]}");
    return httpd_resp_send(req, js, (ssize_t)off);
}

/* GET /api/usbdesc - hex dump of the USB descriptors as really sent.
 * Diagnostic aid: verifies wTotalLength / MS OS 2.0 lengths / BOS fields. */
static esp_err_t usbdesc_api_handler(httpd_req_t *req)
{
    static char buf[6144];

    dap_usb_dump_descriptors(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, -1);
}

/* GET /api/usbtrace - dump the last control SETUP packets + failed EP opens.
 * Diagnostic aid: works when no UART console is reachable. */
static esp_err_t usbtrace_api_handler(httpd_req_t *req)
{
    static char buf[4096];

    usbtrace_dump(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, -1);
}

/* GET /api/usb_dap?toggle=1 - flip the USB CMSIS-DAP probe.
 * Enabling starts the stack immediately (and persists ON for next boot).
 * Disabling persists OFF and releases IO19/IO20 at the next reboot; the live
 * stack is deliberately never torn down mid-transfer. */
static esp_err_t usb_dap_api_handler(httpd_req_t *req)
{
    extern bool dap_usb_is_started(void);
    extern esp_err_t dap_usb_start(void);

    if (pin_config_usb_mode() == USB_MODE_TTL || usb_ttl_is_started()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"USB-TTL owns the port; use /api/usb_mode\"}");
        return ESP_OK;
    }

    bool on = !dap_usb_is_started();
    esp_err_t e = pin_config_set_usb_dap(on);
    if (on && e == ESP_OK) e = dap_usb_start();
    if (e != ESP_OK && on) pin_config_set_usb_dap(false);   /* keep flag honest */

    char json[96];
    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"usb_dap\":%s,\"reboot\":%s}",
             e == ESP_OK ? "true" : "false",
             on ? "true" : "false",
             (!on && e == ESP_OK) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/usb_mode            - report the current USB-C role
 * GET /api/usb_mode?m=off|dap|ttl - select a role.
 * OFF -> DAP/TTL starts live (nothing owns the PHY yet). Any change while a
 * USB stack is running persists and reboots the board ~1.2 s after the
 * response: a live USB device stack is never torn down mid-transfer. */
static esp_err_t usb_mode_api_handler(httpd_req_t *req)
{
    extern bool dap_usb_is_started(void);
    extern esp_err_t dap_usb_start(void);

    static const char *mode_s[3] = { "off", "dap", "ttl" };
    char query[48] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char mv[8] = {0};
    httpd_query_key_value(query, "m", mv, sizeof(mv));

    uint8_t cur = pin_config_usb_mode();
    char json[128];

    if (!mv[0]) {   /* pure query */
        snprintf(json, sizeof(json),
                 "{\"usb_mode\":\"%s\",\"usb_dap\":%d,\"usb_ttl\":%d}",
                 mode_s[cur], dap_usb_is_started() ? 1 : 0,
                 usb_ttl_is_started() ? 1 : 0);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, -1);
    }

    uint8_t want;
    if (!strcasecmp(mv, "off")) want = USB_MODE_OFF;
    else if (!strcasecmp(mv, "dap")) want = USB_MODE_DAP;
    else if (!strcasecmp(mv, "ttl")) want = USB_MODE_TTL;
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"m must be off|dap|ttl\"}");
        return ESP_OK;
    }

    bool reboot = false;
    esp_err_t e = ESP_OK;
    if (want != cur) {
        bool phy_busy = dap_usb_is_started() || usb_ttl_is_started();
        e = pin_config_set_usb_mode(want);
        if (e == ESP_OK && !phy_busy) {
            if (want == USB_MODE_DAP) e = dap_usb_start();
            else if (want == USB_MODE_TTL) e = usb_ttl_start();
            if (e != ESP_OK) (void)pin_config_set_usb_mode(cur);  /* revert */
        } else if (e == ESP_OK && phy_busy) {
            reboot = true;
        }
    }

    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"usb_mode\":\"%s\",\"reboot\":%s}",
             e == ESP_OK ? "true" : "false", mode_s[want],
             reboot ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    e = httpd_resp_send(req, json, -1);

    if (reboot && e == ESP_OK) {
        ESP_LOGI(TAG, "USB mode switch: restarting in 1.2 s to re-enumerate");
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
    return e;
}

/* POST /api/ota - the request body is a raw app .bin (the exact file
 * build/nexlink.bin).  It is streamed into the INACTIVE
 * ota slot; on success we boot it and answer, then reboot 1.5 s later so
 * the browser sees the response first.
 *
 * Safety: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new slot starts in
 * PENDING_VERIFY state and main.c only cancels the rollback once the whole
 * system initialised - a bad image crash-loops and the bootloader falls
 * back to the current firmware automatically. */
static esp_err_t ota_api_handler(httpd_req_t *req)
{
    const int total = req->content_len;
    if (total <= 0x1000) {                       /* reject junk/truncated */
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid image size\"}");
        return ESP_OK;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "{\"error\":\"no spare OTA partition\"}");
        return ESP_OK;
    }
    if ((uint32_t)total > target->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"error\":\"image larger than OTA slot\"}");
        return ESP_OK;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, total, &ota);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "{\"error\":\"esp_ota_begin failed\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA: writing %d bytes to partition %s @0x%lx",
             total, target->label, (unsigned long)target->address);

    /* Static chunk: the httpd task stack must not carry 4KB buffers around. */
    static uint8_t chunk[4096];
    int received = 0;
    while (received < total) {
        int want = total - received;
        if (want > (int)sizeof(chunk)) want = (int)sizeof(chunk);
        int n = httpd_req_recv(req, (char *)chunk, want);
        if (n <= 0) {
            err = (n == HTTPD_SOCK_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        if (err != ESP_OK) break;
        received += n;
        if ((received & 0xFFFFF) < (int)sizeof(chunk))
            ESP_LOGI(TAG, "OTA: %d / %d bytes", received, total);
    }

    if (err == ESP_OK) err = esp_ota_end(ota);
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "{\"error\":\"image write failed\"}");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(target);   /* -> PENDING_VERIFY */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set-boot failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "{\"error\":\"set boot partition failed\"}");
        return ESP_OK;
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"partition\":\"%s\",\"bytes\":%d,"
             "\"message\":\"flashed - rebooting into new firmware\"}",
             target->label, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);

    ESP_LOGI(TAG, "OTA done, restarting into %s in 1.5 s", target->label);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;   /* never reached */
}

/* GET /api/dut/reset?ms=25&boot=1 - pulse the target's reset line.
 * In TTL mode the bridge owns the lines: the pulse drives NRST low for ms
 * milliseconds, and with boot=1 keeps BOOT(=IO0) low across the release so
 * an ESP target samples IO0=0 on reset and drops into its ROM bootloader
 * (the software equivalent of the two-transistor auto-download circuit).
 * Outside TTL mode NRST is still reachable through the SWD bridge. */
static esp_err_t dut_reset_api_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char bv[8] = {0}, qv[8] = {0};
    httpd_query_key_value(query, "ms", bv, sizeof(bv));
    httpd_query_key_value(query, "boot", qv, sizeof(qv));
    int ms = bv[0] ? atoi(bv) : 25;
    if (ms < 1) ms = 1;
    if (ms > 1000) ms = 1000;
    bool boot = !strcmp(qv, "1");

    char json[96];
    if (usb_ttl_is_started()) {
        int nrst_io = pin_config_nrst(), boot_io = pin_config_func_io(PIN_GPIO);
        usb_ttl_reset_pulse(ms, boot);
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"via\":\"ttl\",\"nrst_io\":%d,\"boot_io\":%d,\"boot\":%d}",
                 nrst_io, boot_io, (boot && boot_io >= 0) ? 1 : 0);
    } else {
        extern esp_err_t swd_reset_target(bool);
        swd_reset_target(true);
        vTaskDelay(pdMS_TO_TICKS(ms));
        swd_reset_target(false);
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"via\":\"swd\",\"boot\":0,\"note\":\"TTL mode not active: BOOT line idle\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/lcd - raw 128x64 1bpp OLED framebuffer for screen mirroring.
 * Layout: 8 pages of 128 bytes. Bit 0 of each byte is the topmost pixel of
 * that page, which is exactly the native SSD1306 memory order. */
static esp_err_t lcd_api_handler(httpd_req_t *req)
{
    const uint8_t *fb = oled_get_fb();
    httpd_resp_set_type(req, "application/octet-stream");
    if (!fb) {
        return httpd_resp_send(req, "", 0);   /* panel not ready yet -> empty */
    }
    /* Snapshot before send: the UI task is redrawing s_fb concurrently, so
     * handing out the live pointer would let the client read a half-updated
     * frame. 1 KB memcpy beats tearing across the whole TCP send window. */
    static uint8_t snap[OLED_FB_BYTES];
    memcpy(snap, fb, OLED_FB_BYTES);
    return httpd_resp_send(req, (const char *)snap, OLED_FB_BYTES);
}

/* GET /api/pins - current function-per-position order + the fixed IOs */
static esp_err_t pins_get_handler(httpd_req_t *req)
{
    char json[160];
    int n = 0;
    n += snprintf(json + n, sizeof(json) - n, "{\"order\":[");
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"%s\"",
                      i ? "," : "", pin_config_kind_label(pin_config_pos_func(i)));
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"pos_io\":[");
    for (int i = 0; i < PIN_NUM_SIGNAL_SLOTS; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s%d",
                      i ? "," : "", pin_config_pos_io(i));
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* Minimal URL-decode in place (%XX and '+'). httpd_query_key_value does not
 * decode the value, so a form-encoded body (e.g. commas as %2C from the
 * browser) must be decoded explicitly. */
static void url_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static pin_kind_t parse_pin_kind(const char *s)
{
    if (!s) return PIN_KIND_COUNT;
    if (!strcmp(s, "TX"))    return PIN_TX;
    if (!strcmp(s, "RX"))    return PIN_RX;
    if (!strcmp(s, "PWM"))   return PIN_PWM;
    if (!strcmp(s, "SCK"))   return PIN_SPI_SCK;
    if (!strcmp(s, "MOSI"))  return PIN_SPI_MOSI;
    if (!strcmp(s, "MISO"))  return PIN_SPI_MISO;
    if (!strcmp(s, "CS"))    return PIN_SPI_CS;
    if (!strcmp(s, "SDA"))   return PIN_I2C_SDA;
    if (!strcmp(s, "SCL"))   return PIN_I2C_SCL;
    if (!strcmp(s, "GPIO"))  return PIN_GPIO;
    return PIN_KIND_COUNT;
}

/* POST /api/pins - update the function-per-position order.
 * Body (form-encoded): order=SWCLK,RX,NRST,SWDIO,TX  (a permutation of the 5
 * signal kinds). Re-applies the mapping to SWD and UART. */
static esp_err_t pins_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char order_v[128] = {0};
    esp_err_t pin_err = httpd_query_key_value(body, "order", order_v, sizeof(order_v));
    ESP_LOGI(TAG, "pins POST raw body=[%s] query_err=%s order_v=[%s]",
             body, esp_err_to_name(pin_err), order_v);
    url_decode_inplace(order_v);   /* handle browser-encoded commas (%2C) */
    ESP_LOGI(TAG, "pins POST decoded order_v=[%s]", order_v);

    esp_err_t err = ESP_OK;
    if (order_v[0]) {
        pin_kind_t ord[PIN_NUM_SIGNAL_SLOTS];
        int cnt = 0;
        char *tok = strtok(order_v, ",");
        while (tok && cnt < PIN_NUM_SIGNAL_SLOTS) {
            ord[cnt] = parse_pin_kind(tok);
            ESP_LOGI(TAG, "pins POST tok[%d]=[%s] -> kind=%d", cnt, tok, (int)ord[cnt]);
            cnt++;
            tok = strtok(NULL, ",");
        }
        int o_pw   = pin_config_pwm();
        int o_sck  = pin_config_spi_sck(), o_mosi = pin_config_spi_mosi();
        int o_miso = pin_config_spi_miso(), o_cs   = pin_config_spi_cs();
        int o_sda  = pin_config_i2c_sda(), o_scl  = pin_config_i2c_scl();
        ESP_LOGI(TAG, "pins POST cnt=%d (need %d)", cnt, PIN_NUM_SIGNAL_SLOTS);
        err = (cnt == PIN_NUM_SIGNAL_SLOTS) ? pin_config_set_signal_order(ord)
                                            : ESP_ERR_INVALID_ARG;
        ESP_LOGI(TAG, "pins POST set_signal_order returned %s", esp_err_to_name(err));
        if (err == ESP_OK) {
            /* re-apply the new function->GPIO mapping to every subsystem */
            swd_bus_lock();   /* do not re-wire pads under an active DAP session */
            swd_init();
            swd_bus_unlock();
            serial_bridge_repin();
            int n_pw = pin_config_pwm();
            if (n_pw != o_pw) {
                pwm_mon_stop();
                if (n_pw >= 0) pwm_mon_start(n_pw);
            }
            int n_sck = pin_config_spi_sck(), n_mosi = pin_config_spi_mosi();
            int n_miso = pin_config_spi_miso(), n_cs = pin_config_spi_cs();
            if (n_sck != o_sck || n_mosi != o_mosi || n_miso != o_miso || n_cs != o_cs) {
                spi_mon_stop();
                if (n_sck >= 0 && n_mosi >= 0 && n_miso >= 0 && n_cs >= 0) {
                    spi_mon_start(n_sck, n_mosi, n_miso, n_cs, 0);
                }
            }
            int n_sda = pin_config_i2c_sda(), n_scl = pin_config_i2c_scl();
            if (n_sda != o_sda || n_scl != o_scl) {
                i2c_mon_stop();
                if (n_sda >= 0 && n_scl >= 0) {
                    i2c_mon_start(n_sda, n_scl, 0);
                }
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", -1);
    }
    ESP_LOGW(TAG, "pins POST rejected: %s", esp_err_to_name(err));
    return httpd_resp_send(req, "{\"ok\":false}", -1);
}

/* GET /api/pwm - latest PWM frequency + duty + diagnostic info */
static esp_err_t pwm_api_handler(httpd_req_t *req)
{
    float f, d;
    pwm_mon_get(&f, &d);
    int pwm_pin = pin_config_pwm();
    int lvl = (pwm_pin >= 0) ? gpio_get_level(pwm_pin) : -1;
    char json[120];
    snprintf(json, sizeof(json),
             "{\"freq\":%.2f,\"duty\":%.2f,\"isr\":%lu,\"pin\":%d,\"level\":%d}",
             (double)f, (double)d, (unsigned long)pwm_mon_isr_count(), pwm_pin, lvl);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/spi - captured SPI transaction history */
static esp_err_t spi_api_handler(httpd_req_t *req)
{
    /* Return up to 30 recent transactions so MCP can fetch deep history */
    spi_txn_t txns[30];
    int n = spi_mon_get_history(txns, 30);

    static char json[8192];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p,
                  "{\"running\":%s,\"count\":%lu,\"timeouts\":%lu,\"history\":[",
                  spi_mon_running() ? "true" : "false",
                  (unsigned long)spi_mon_get_count(),
                  (unsigned long)spi_mon_get_timeouts());
    for (int i = 0; i < n && p < (int)sizeof(json) - 500; i++) {
        int show = txns[i].len;
        if (show > 16) show = 16;
        p += snprintf(json + p, sizeof(json) - p, "%s{\"mosi\":[", i ? "," : "");
        for (int b = 0; b < show; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].mosi[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"miso\":[");
        for (int b = 0; b < show; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].miso[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"seq\":%lu}", (unsigned long)txns[i].seq);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/i2c - captured I2C transaction history */
static esp_err_t i2c_api_handler(httpd_req_t *req)
{
    i2c_txn_t txns[30];
    int n = i2c_mon_get_history(txns, 30);
    static char json[6144];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p,
                  "{\"running\":%s,\"mode\":\"%s\",\"count\":%lu,\"isr\":%lu,\"history\":[",
                  i2c_mon_running() ? "true" : "false",
                  i2c_mon_mode() == I2C_MON_SLAVE ? "slave" : "passive",
                  (unsigned long)i2c_mon_get_count(),
                  (unsigned long)i2c_mon_get_isr_count());
    for (int i = 0; i < n && p < (int)sizeof(json) - 200; i++) {
        p += snprintf(json + p, sizeof(json) - p,
                      "%s{\"addr\":%d,\"read\":%s,\"data\":[",
                      i ? "," : "", txns[i].addr >> 1,
                      txns[i].read ? "true" : "false");
        for (int b = 0; b < txns[i].len && p < (int)sizeof(json) - 40; b++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", b ? "," : "", txns[i].data[b]);
        p += snprintf(json + p, sizeof(json) - p, "],\"seq\":%lu}", (unsigned long)txns[i].seq);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/capacity - free heap + per-buffer usage (n/max) */
static esp_err_t capacity_api_handler(httpd_req_t *req)
{
        char json[256];
    snprintf(json, sizeof(json),
             "{\"free_heap\":%lu,"
             "\"serial\":{\"n\":%lu,\"max\":%lu},"
             "\"spi\":{\"n\":%d,\"max\":%d},"
             "\"i2c\":{\"n\":%d,\"max\":%d},"
             "\"rx\":{\"n\":%d,\"max\":%d}}",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)serial_bridge_get_buffered(),
             (unsigned long)serial_bridge_get_bufsize_current(),
             spi_mon_get_hist_n(), spi_mon_get_history_max(),
             i2c_mon_get_hist_n(), i2c_mon_get_history_max(),
             0, 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/clear?what=serial|spi|i2c|rx|all */
static esp_err_t clear_api_handler(httpd_req_t *req)
{
    extern void dash_clear_rx(void);
    char query[24] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char what[8] = {0};
    httpd_query_key_value(query, "what", what, sizeof(what));

    bool known = false;
    if (!strcmp(what, "serial") || !strcmp(what, "all")) { serial_bridge_clear(); known = true; }
    if (!strcmp(what, "spi")    || !strcmp(what, "all")) { spi_mon_clear();      known = true; }
    if (!strcmp(what, "i2c")    || !strcmp(what, "all")) { i2c_mon_clear();      known = true; }
    if (!strcmp(what, "rx")     || !strcmp(what, "all")) { dash_clear_rx();      known = true; }
    if (!strcmp(what, "capture")|| !strcmp(what, "all")) { capture_reset_counters(); known = true; }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        known ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unknown target\"}", -1);
}

/* POST /api/cfg?key=buf|spihist|i2chist|rxhist&val=N — adjust buffer sizes.
 * Safety bounds enforced to avoid heap/partition exhaustion. */
static esp_err_t cfg_set_handler(httpd_req_t *req)
{
    
    char query[40] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char key[12] = {0}, valv[12] = {0};
    httpd_query_key_value(query, "key", key, sizeof(key));
    httpd_query_key_value(query, "val", valv, sizeof(valv));
    int v = atoi(valv);

    if (!strcmp(key, "buf")) {
        /* serial buffer: 512 .. 16384 bytes */
        if (v < 512) v = 512;
        if (v > 16384) v = 16384;
        serial_bridge_set_bufsize((size_t)v);
    } else if (!strcmp(key, "spihist")) {
        if (v < 2) v = 2;
        if (v > SPI_MON_MAX) v = SPI_MON_MAX;
        spi_mon_set_history_max(v);
    } else if (!strcmp(key, "i2chist")) {
        if (v < 2) v = 2;
        if (v > I2C_MON_MAX) v = I2C_MON_MAX;
        i2c_mon_set_history_max(v);
    } else if (!strcmp(key, "rxhist")) {
        /* The dashboard keeps a fixed 4-line RX history; the depth is not tunable
         * any more, so this key is accepted and ignored. */
        (void)v;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key");
        return ESP_FAIL;
    }

    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":true,\"key\":\"%s\",\"val\":%d}", key, v);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/pwm/out - start/stop/set PWM output. Body: freq=&duty=&enable=1 */
static esp_err_t pwm_out_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) {
        int got = 0;
        while (got < total) {
            int n = httpd_req_recv(req, body + got, total - got);
            if (n <= 0) break;
            got += n;
        }
        body[got] = '\0';
    }

    char freq_v[16] = {0}, duty_v[16] = {0}, en_v[8] = {0};
    httpd_query_key_value(body, "freq", freq_v, sizeof(freq_v));
    httpd_query_key_value(body, "duty", duty_v, sizeof(duty_v));
    httpd_query_key_value(body, "enable", en_v, sizeof(en_v));

    if (en_v[0] == '0') {
        pwm_out_stop();
    } else if (freq_v[0]) {
        float freq = strtof(freq_v, NULL);
        float duty = duty_v[0] ? strtof(duty_v, NULL) : 50.0f;
        if (pwm_out_running()) {
            pwm_out_set(freq, duty);
        } else {
            int pin = pin_config_pwm();
            if (pin >= 0) pwm_out_start(pin, freq, duty);
        }
    }

    float f = 0, d = 0;
    pwm_out_get(&f, &d);
    char json[96];
    snprintf(json, sizeof(json), "{\"running\":%s,\"freq\":%.2f,\"duty\":%.1f}",
             pwm_out_running() ? "true" : "false", (double)f, (double)d);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/spi/send - send hex bytes via SPI master. Body: hex=AABBCC&mode=0&clk=1000000 */
static esp_err_t spi_send_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char hex_v[256] = {0}, mode_v[8] = {0}, clk_v[16] = {0};
    httpd_query_key_value(body, "hex", hex_v, sizeof(hex_v));
    httpd_query_key_value(body, "mode", mode_v, sizeof(mode_v));
    httpd_query_key_value(body, "clk", clk_v, sizeof(clk_v));

    /* Parse hex string to bytes */
    uint8_t tx_buf[128];
    int tx_len = 0;
    for (int i = 0; hex_v[i] && hex_v[i + 1] && tx_len < (int)sizeof(tx_buf); i += 2) {
        char byte[3] = { hex_v[i], hex_v[i + 1], 0 };
        tx_buf[tx_len++] = (uint8_t)strtol(byte, NULL, 16);
    }

    if (tx_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no data");
        return ESP_FAIL;
    }

    int mode = mode_v[0] ? atoi(mode_v) : 0;
    int clk = clk_v[0] ? atoi(clk_v) : 1000000;

    /* Start SPI master if not running, or restart if mode/clock changed */
    if (!spi_send_running()) {
        int sck = pin_config_spi_sck(), mosi = pin_config_spi_mosi();
        int miso = pin_config_spi_miso(), cs = pin_config_spi_cs();
        if (sck < 0 || mosi < 0 || cs < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SPI pins not assigned");
            return ESP_FAIL;
        }
        /* Stop slave monitor if running on same pins */
        if (spi_mon_running()) spi_mon_stop();
        esp_err_t e = spi_send_start(sck, mosi, miso, cs, mode, clk);
        if (e != ESP_OK) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "SPI init failed: %s", esp_err_to_name(e));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, emsg);
            return ESP_FAIL;
        }
    }

    uint8_t rx_buf[128] = {0};
    esp_err_t e = spi_send_bytes(tx_buf, rx_buf, tx_len);

    char json[512];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p, "{\"ok\":%s,\"rx\":[",
                  e == ESP_OK ? "true" : "false");
    if (e == ESP_OK) {
        for (int i = 0; i < tx_len; i++)
            p += snprintf(json + p, sizeof(json) - p, "%s%d", i ? "," : "", rx_buf[i]);
    }
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/i2c/send - send/receive I2C data.
 * Body: addr=0x50&hex=AABB&mode=w|r|wr */
static esp_err_t i2c_send_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char addr_v[8] = {0}, hex_v[256] = {0}, mode_v[8] = {0}, len_v[8] = {0};
    httpd_query_key_value(body, "addr", addr_v, sizeof(addr_v));
    httpd_query_key_value(body, "hex", hex_v, sizeof(hex_v));
    httpd_query_key_value(body, "mode", mode_v, sizeof(mode_v));
    httpd_query_key_value(body, "len", len_v, sizeof(len_v));

    if (!addr_v[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need addr");
        return ESP_FAIL;
    }
    uint8_t addr = (uint8_t)strtol(addr_v, NULL, 0);

    if (!i2c_send_running()) {
        int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
        if (sda < 0 || scl < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "I2C pins not assigned");
            return ESP_FAIL;
        }
        if (i2c_mon_running()) i2c_mon_stop();
        esp_err_t e = i2c_send_start(sda, scl, 400000);
        if (e != ESP_OK) {
            char emsg[64];
            snprintf(emsg, sizeof(emsg), "I2C init failed: %s", esp_err_to_name(e));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, emsg);
            return ESP_FAIL;
        }
    }

    /* Parse hex data */
    uint8_t data_buf[128];
    int data_len = 0;
    for (int i = 0; hex_v[i] && hex_v[i + 1] && data_len < (int)sizeof(data_buf); i += 2) {
        char byte[3] = { hex_v[i], hex_v[i + 1], 0 };
        data_buf[data_len++] = (uint8_t)strtol(byte, NULL, 16);
    }

    esp_err_t e = ESP_OK;
    uint8_t rx_buf[128] = {0};
    int rx_len = 0;

    if (strcmp(mode_v, "r") == 0) {
        int rlen = len_v[0] ? atoi(len_v) : 1;
        if (rlen > 128) rlen = 128;
        e = i2c_read(addr, rx_buf, rlen);
        rx_len = (e == ESP_OK) ? rlen : 0;
    } else if (strcmp(mode_v, "wr") == 0 && data_len > 0) {
        int rlen = len_v[0] ? atoi(len_v) : 1;
        if (rlen > 128) rlen = 128;
        e = i2c_write_read(addr, data_buf, data_len, rx_buf, rlen);
        rx_len = (e == ESP_OK) ? rlen : 0;
    } else {
        if (data_len > 0) e = i2c_write(addr, data_buf, data_len);
    }

    char json[512];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p, "{\"ok\":%s,\"err\":\"%s\",\"rx\":[",
                  e == ESP_OK ? "true" : "false",
                  e == ESP_OK ? "" : esp_err_to_name(e));
    for (int i = 0; i < rx_len; i++)
        p += snprintf(json + p, sizeof(json) - p, "%s%d", i ? "," : "", rx_buf[i]);
    p += snprintf(json + p, sizeof(json) - p, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* ---------- AI config (NVS "aiconf") ---------- */
static void ai_nvs_load(char *base, int blen, char *model, int mlen, bool *has_key)
{
    nvs_handle_t h;
    *base = '\0'; *model = '\0'; *has_key = false;
    if (nvs_open("aiconf", NVS_READONLY, &h) != ESP_OK) return;
    size_t l = blen;  nvs_get_str(h, "base", base, &l);
    l = mlen;         nvs_get_str(h, "model", model, &l);
    l = 0;
    if (nvs_get_str(h, "key", NULL, &l) == ESP_OK && l > 1) *has_key = true;
    nvs_close(h);
}

static void ai_nvs_save(const char *base, const char *key, const char *model)
{
    nvs_handle_t h;
    if (nvs_open("aiconf", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "base", base);
    if (key && key[0]) nvs_set_str(h, "key", key);
    nvs_set_str(h, "model", model);
    nvs_commit(h);
    nvs_close(h);
}

/* GET /api/ai/config - return AI settings (key masked) */
static esp_err_t ai_config_get_handler(httpd_req_t *req)
{
    char base[128] = {0}, model[64] = {0};
    bool has_key = false;
    ai_nvs_load(base, sizeof(base), model, sizeof(model), &has_key);

    char json[256];
    snprintf(json, sizeof(json), "{\"base\":\"%s\",\"model\":\"%s\",\"hasKey\":%s}",
             base, model, has_key ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/ai/config - save AI settings (form: base=...&key=...&model=...) */
static esp_err_t ai_config_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) break;
        got += n;
    }
    body[got] = '\0';

    char base[128] = {0}, key[256] = {0}, model[64] = {0};
    httpd_query_key_value(body, "base", base, sizeof(base));
    httpd_query_key_value(body, "key", key, sizeof(key));
    httpd_query_key_value(body, "model", model, sizeof(model));
    url_decode_inplace(base);
    url_decode_inplace(key);
    url_decode_inplace(model);

    ai_nvs_save(base, key, model);
    ESP_LOGI(TAG, "AI config saved: base=%s model=%s", base, model);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

/* GET /api/swd/debug - SWD diagnostic. ?test=clk toggles SWCLK ~2kHz. */
static esp_err_t swd_debug_handler(httpd_req_t *req)
{
    char query[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char test[8] = {0};
    httpd_query_key_value(query, "test", test, sizeof(test));

    if (strcmp(test, "clk") == 0) {
        /* Toggle SWCLK for ~2 seconds so the user can measure it */
        int swclk = pin_config_swclk();
        swd_bus_lock();
        gpio_set_direction(swclk, GPIO_MODE_INPUT_OUTPUT);
        for (int i = 0; i < 4000; i++) {
            gpio_set_level(swclk, i & 1);
            vTaskDelay(1);  /* ~1ms per toggle with FreeRTOS 1kHz tick */
        }
        gpio_set_level(swclk, 1);       /* back to idle-high */
        swd_bus_unlock();
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "SWCLK toggled 2s on IO, check with multimeter", -1);
    }
    if (strcmp(test, "io") == 0) {
        /* Drive SWDIO high/low so the user can measure */
        int swdio = pin_config_swdio();
        swd_bus_lock();
        gpio_set_direction(swdio, GPIO_MODE_INPUT_OUTPUT);
        for (int i = 0; i < 2000; i++) {
            gpio_set_level(swdio, i & 1);
            vTaskDelay(2);
        }
        gpio_set_level(swdio, 1);       /* back to idle-high */
        swd_bus_unlock();
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "SWDIO toggled 4s, check with multimeter", -1);
    }

    uint32_t ack = swd_get_last_ack();
    const char *desc = "?";
    switch (ack) {
    case 1: desc = "OK"; break;
    case 2: desc = "WAIT"; break;
    case 4: desc = "FAULT"; break;
    case 0: case 7: desc = "NO_RESPONSE"; break;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ack\":%lu,\"desc\":\"%s\"}",
             (unsigned long)ack, desc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* GET /api/wifi - scan and return SSID list as JSON array */
static esp_err_t wifi_scan_api_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t serr = esp_wifi_scan_start(&scan_cfg, true);
    if (serr != ESP_OK) {
        /* Scanning needs an active STA interface; in pure-AP mode this
         * fails. Report it instead of pretending there are no networks. */
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(serr));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"scan failed\"}", -1);
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 20) ap_num = 20;

    wifi_ap_record_t aps[20];
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    char json[2048];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos, "[");

    /* Deduplicate SSIDs */
    char seen[20][33];
    int seen_n = 0;

    for (int i = 0; i < ap_num && pos < (int)sizeof(json) - 100; i++) {
        bool dup = false;
        for (int j = 0; j < seen_n; j++) {
            if (strcmp(seen[j], (char *)aps[i].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (seen_n < 20) {
            strncpy(seen[seen_n], (char *)aps[i].ssid, 32);
            seen[seen_n][32] = '\0';
            seen_n++;
        }
        if (pos > 1) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}",
                        (char *)aps[i].ssid, aps[i].rssi);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

/* POST /api/wifi - set WiFi credentials and connect.
 * Body: "ssid=xxx&pass=yyy" (URL-encoded form data) */
static esp_err_t wifi_set_api_handler(httpd_req_t *req)
{
    char buf[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) break;
        received += n;
    }
    buf[received] = '\0';

    char ssid_enc[64] = {0}, pass_enc[128] = {0};
    httpd_query_key_value(buf, "ssid", ssid_enc, sizeof(ssid_enc));
    httpd_query_key_value(buf, "pass", pass_enc, sizeof(pass_enc));
    /* httpd_query_key_value does NOT percent-decode; SSIDs/passwords with
     * spaces, % or + survive only after url_decode_inplace(). */
    url_decode_inplace(ssid_enc);
    url_decode_inplace(pass_enc);

    char ssid[33], pass[65];
    /* strlcpy always NUL-terminates and silences -Wstringop-truncation. */
    strlcpy(ssid, ssid_enc, sizeof(ssid));
    strlcpy(pass, pass_enc, sizeof(pass));

    if (ssid[0]) {
        if (strlen(pass) > 0 && strlen(pass) < 8) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pass too short");
            return ESP_FAIL;
        }
        wifi_manager_set_credentials(ssid, pass);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "OK", 2);
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
    return ESP_FAIL;
}

/* GET /api/baud?b=115200 - set baud rate */
static esp_err_t baud_api_handler(httpd_req_t *req)
{
    char buf[16] = {0};
    httpd_req_get_url_query_str(req, buf, sizeof(buf));
    char val[16] = {0};
    httpd_query_key_value(buf, "b", val, sizeof(val));
    if (val[0]) {
        uint32_t b = (uint32_t)strtoul(val, NULL, 10);
        serial_bridge_set_baud(b);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

/* POST /api/send - send data to DUT via UART */
static esp_err_t send_api_handler(httpd_req_t *req)
{
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total > (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, total - received);
        if (n <= 0) break;
        received += n;
    }
    if (received > 0) {
        serial_bridge_write((uint8_t *)buf, received);
        /* Mirror our own TX to WebSocket clients so a remote console shows
         * both directions of the conversation. */
        ws_broadcast_data(1, (const uint8_t *)buf, (size_t)received);
        /* Echo sent data onto the hardware LCD's RX monitor ('<' prefix) */
        dash_push_tx((uint8_t *)buf, received);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", 2);
}

esp_err_t http_status_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 31 REST routes are registered below, plus /ws from ws_server_start().
     * The limit must cover both: esp_http_server fails a registration with
     * ESP_ERR_HTTPD_HANDLERS_FULL once hd_calls[] has no free slot, which
     * silently made the WebSocket endpoint unreachable (404) when this was set
     * to exactly 31. One slot of headroom is kept for future routes. */
    cfg.max_uri_handlers = 34;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    /* HTTPD_DEFAULT_CONFIG() uses 5 s here, and the WebSocket broadcast task
     * writes through this socket layer: a browser that stops reading (closed
     * tab, stalled TCP window) would otherwise hold the single ws_tx task in
     * send() for 5 s, starving every other client. */
    cfg.send_wait_timeout = 2;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=index_handler };
    httpd_uri_t uri_status = { .uri="/api/status", .method=HTTP_GET, .handler=status_api_handler };
    httpd_uri_t uri_data = { .uri="/api/data", .method=HTTP_GET, .handler=data_api_handler };
    httpd_uri_t uri_mcp_data = { .uri="/api/mcp/data", .method=HTTP_GET, .handler=mcp_data_handler };
    httpd_uri_t uri_capture = { .uri="/api/capture", .method=HTTP_GET, .handler=capture_get_handler };
    httpd_uri_t uri_btn = { .uri="/api/btn", .method=HTTP_GET, .handler=btn_api_handler };
    httpd_uri_t uri_usbdap = { .uri="/api/usb_dap", .method=HTTP_GET, .handler=usb_dap_api_handler };
    httpd_uri_t uri_usbtrace = { .uri="/api/usbtrace", .method=HTTP_GET, .handler=usbtrace_api_handler };
    httpd_uri_t uri_usbdesc = { .uri="/api/usbdesc", .method=HTTP_GET, .handler=usbdesc_api_handler };
    httpd_uri_t uri_usbmode = { .uri="/api/usb_mode", .method=HTTP_GET, .handler=usb_mode_api_handler };
    httpd_uri_t uri_dutreset = { .uri="/api/dut/reset", .method=HTTP_GET, .handler=dut_reset_api_handler };
    httpd_uri_t uri_ota = { .uri="/api/ota", .method=HTTP_POST, .handler=ota_api_handler };
    httpd_uri_t uri_baud = { .uri="/api/baud", .method=HTTP_GET, .handler=baud_api_handler };
    httpd_uri_t uri_send = { .uri="/api/send", .method=HTTP_POST, .handler=send_api_handler };
    httpd_uri_t uri_wscan = { .uri="/api/wifi", .method=HTTP_GET, .handler=wifi_scan_api_handler };
    httpd_uri_t uri_wset = { .uri="/api/wifi", .method=HTTP_POST, .handler=wifi_set_api_handler };
    httpd_uri_t uri_lcd = { .uri="/api/lcd", .method=HTTP_GET, .handler=lcd_api_handler };
    httpd_uri_t uri_pins_get = { .uri="/api/pins", .method=HTTP_GET, .handler=pins_get_handler };
    httpd_uri_t uri_pins_post = { .uri="/api/pins", .method=HTTP_POST, .handler=pins_post_handler };
    httpd_uri_t uri_pwm = { .uri="/api/pwm", .method=HTTP_GET, .handler=pwm_api_handler };
    httpd_uri_t uri_spi = { .uri="/api/spi", .method=HTTP_GET, .handler=spi_api_handler };
    httpd_uri_t uri_i2c = { .uri="/api/i2c", .method=HTTP_GET, .handler=i2c_api_handler };
    httpd_uri_t uri_pwm_out = { .uri="/api/pwm/out", .method=HTTP_POST, .handler=pwm_out_handler };
    httpd_uri_t uri_spi_send = { .uri="/api/spi/send", .method=HTTP_POST, .handler=spi_send_handler };
    httpd_uri_t uri_i2c_send = { .uri="/api/i2c/send", .method=HTTP_POST, .handler=i2c_send_handler };
    httpd_uri_t uri_swd_debug = { .uri="/api/swd/debug", .method=HTTP_GET, .handler=swd_debug_handler };
    httpd_uri_t uri_capacity = { .uri="/api/capacity", .method=HTTP_GET, .handler=capacity_api_handler };
    httpd_uri_t uri_clear = { .uri="/api/clear", .method=HTTP_POST, .handler=clear_api_handler };
    httpd_uri_t uri_cfg = { .uri="/api/cfg", .method=HTTP_POST, .handler=cfg_set_handler };
    httpd_uri_t uri_ai_cfg_get = { .uri="/api/ai/config", .method=HTTP_GET, .handler=ai_config_get_handler };
    httpd_uri_t uri_ai_cfg_post = { .uri="/api/ai/config", .method=HTTP_POST, .handler=ai_config_post_handler };

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_data);
    httpd_register_uri_handler(server, &uri_mcp_data);
    httpd_register_uri_handler(server, &uri_capture);
    httpd_register_uri_handler(server, &uri_btn);
    httpd_register_uri_handler(server, &uri_usbdap);
    httpd_register_uri_handler(server, &uri_usbtrace);
    httpd_register_uri_handler(server, &uri_usbdesc);
    httpd_register_uri_handler(server, &uri_usbmode);
    httpd_register_uri_handler(server, &uri_dutreset);
    httpd_register_uri_handler(server, &uri_ota);
    httpd_register_uri_handler(server, &uri_baud);
    httpd_register_uri_handler(server, &uri_send);
    httpd_register_uri_handler(server, &uri_wscan);
    httpd_register_uri_handler(server, &uri_wset);
    httpd_register_uri_handler(server, &uri_lcd);
    httpd_register_uri_handler(server, &uri_pins_get);
    httpd_register_uri_handler(server, &uri_pins_post);
    httpd_register_uri_handler(server, &uri_pwm);
    httpd_register_uri_handler(server, &uri_spi);
    httpd_register_uri_handler(server, &uri_i2c);
    httpd_register_uri_handler(server, &uri_pwm_out);
    httpd_register_uri_handler(server, &uri_spi_send);
    httpd_register_uri_handler(server, &uri_i2c_send);
    httpd_register_uri_handler(server, &uri_swd_debug);
    httpd_register_uri_handler(server, &uri_capacity);
    httpd_register_uri_handler(server, &uri_clear);
    httpd_register_uri_handler(server, &uri_cfg);
    httpd_register_uri_handler(server, &uri_ai_cfg_get);
    httpd_register_uri_handler(server, &uri_ai_cfg_post);

    /* WebSocket shares this server (and therefore port 80). It is started
     * after the REST routes so an upgrade request can never race the route
     * table setup, and its failure is not fatal: the polling API still works.
     * The old upstream ws_server.c was never called from anywhere and stayed
     * dead code; this one is part of the boot path and reports its state. */
    if (ws_server_start(server) != ESP_OK)
        ESP_LOGW(TAG, "WebSocket %s unavailable - REST polling remains", WS_PATH);

    ESP_LOGI(TAG, "HTTP status page on :80 (WebSocket %s)", WS_PATH);
    return ESP_OK;
}
