# 🔧 NexLink

**ESP32-S3 无线硬件调试器** —— 一块板子上同时具备 USB CMSIS-DAP 探针、无线串口桥、
SPI/I2C/PWM 协议分析仪、以及让 AI 直接操控硬件的 MCP 接口。

- 无线：WiFi STA/AP + TCP 串口透传 + HTTP REST + Web 状态页
- 有线：USB-C 一键切换 **CMSIS-DAP v2 探针** / **USB-TTL 下载桥** / 关闭
- 离线可用：OLED 菜单 + 三个物理按键，不依赖网络
- AI 可驱动：内置 MCP Server，Claude / Cursor / OpenCode / Cline 开箱即用

> 本项目基于 [JasonYANG170/ai-wireless-debugger](https://gitee.com/JasonYANG170/ai-wireless-debugger)
> 修改而来，已大幅重构：新增 USB CMSIS-DAP 探针（三态角色）、以 SSD1306 OLED 取代
> ST7735S 彩屏并释放 5 个 IO、新增 OTA 双槽回滚、DUT 复位控制与 USB 免串口诊断，
> 并修复了 I2C/SWD/TCP/WiFi/按键的既有缺陷。

## 🧩 板载硬件

| 部件 | 说明 |
|------|------|
| MCU | ESP32-S3-WROOM-1-**N16R8**（16 MB quad flash + 8 MB octal PSRAM） |
| 供电 | **USB-C** 5V 输入 → 板载 **LDO** → 3.3V 供 S3；3V3 同时引到排针给目标供电 |
| 按键 | **BOOT**（下载模式）+ **EN**（复位），均带 3V3 上拉；另有 SW1/SW2/SW3 三个功能键（IO1/IO2/IO42） |
| 显示 | SSD1306 0.96" 128×64 OLED，I2C0（IO10/IO11），地址 0x3C |
| 扩展 | **16 Pin 排针**：5V、3V3、GND×2、TXD0/RXD0、TXD1/RXD1、SCL/SDA、NRST/SWCLK/SWDIO、拓展 IO×5 |
| 电平转换 | **无**。当前 PCB 已取消 TXB0106，所有信号直连 ESP32-S3（**整板 3.3V**） |

> ⚠️ 排针**不带电平转换**：接 3.3V 目标安全，接 **5V 逻辑目标必须自行加转换**——
> 原来的 TXB0106 已经取消。详见 [硬件与引脚](#-硬件与引脚)。

---

## ✨ 功能特点

### 🔌 USB-C 三态角色（核心特性）

原生 USB 外设只有一个 PHY，三种人格互斥，选择会持久化到 NVS：

| 模式 | 作用 | 生效方式 |
|------|------|----------|
| `off` | 释放 IO19/IO20，USB-C 口作 USB-Serial-JTAG 给**本板**刷固件 | 立即 |
| `dap` | **CMSIS-DAP v2.1 探针**（WinUSB 免驱），Keil / pyOCD / OpenOCD 直接认 | off→dap 立即；dap↔ttl 需重启 |
| `ttl` | CDC-ACM 虚拟 COM ↔ 目标板 UART1 桥，带 BOOT/NRST 自动复位线 | 同上 |

- **免驱绑定**：通过 WCID 1.0（MS OS 1.0）让 Windows 自动装 WinUSB，**无需 .inf、无需 Zadig**
- **真 DAPLink 内核**：`main/dap/` 是 ARM DAPLink 源码（Apache-2.0，取自 [CherryDAP](https://github.com/cherry-embedded/CherryDAP)），
  由 CherryUSB 提供 USB 设备栈与 ESP32-S3 DWC2 控制器驱动
- **安全模式守卫**：DAP/TTL 模式下若发生 panic / 看门狗重启，下次开机**自动把 USB 切回 `off`**
  并持久化，防止崩溃循环把设备彻底锁死

### 📶 无线连接

- **WiFi STA/AP**：默认优先连已保存网络；无凭据或连接失败自动回落 AP 配网模式
  （AP SSID `NexLink-xxxx`，密码 `12345678`，IP `192.168.4.1`）
- **TCP :3333**：原始串口透传，支持多客户端同时连接
- **HTTP :80**：Web 状态页 + 完整 REST API
- **TCP :5555**：原始 CMSIS-DAP over TCP（脚本 / AI 用；标准工具链请走 USB `dap` 模式）

### 📡 多协议调试

| 功能 | 说明 |
|------|------|
| **串口桥** | UART1（IO47/IO21）↔ DUT，波特率 9600 / 115200 / 460800 / 921600 动态切换 |
| **PWM 测量** | RMT 测频 + 占空比 |
| **PWM 输出** | LEDC 生成，频率/占空比可设 |
| **SPI** | 默认作 SPI 从机实时抓包（MOSI/MISO/CS），可切主机主动发送（两者互斥，自动切换） |
| **I2C** | 从机模式（默认地址 0x50，可靠 ACK 抓写）或 RMT 被动嗅探（监听外部主机↔从机） |
| **SWD** | GPIO bit-bang + TCP 侧 CMSIS-DAP 处理器（读 IDCODE 等） |
| **JTAG** | DAPLink 内核原生支持（`main/dap/JTAG_DP.c`），覆盖 RISC-V / FPGA / 菊花链 |
| **SWO / ITM** | 单线跟踪，目标的 `printf` 经 UART 模式接收（4 KB 环形缓冲，最高 4 Mbaud） |

### 🖥️ OLED 菜单（SSD1306 128×64，I2C）

8 个页面的功能环，竖屏滑动切换动画，支持 180° 翻转安装：

```
RX Monitor · I2C Bus · SPI Bus · PWM · SWD/DAP · Status · Config · AI/MCP
```

- **主页**：SW3 移动光标，SW2 进入所选功能
- **子页**：SW3 / SW1 下一页 / 上一页
- **SW2 长按**：全局返回主页（万能逃生键，编辑态也生效）
- **Config 页**：波特率 / 亮度 / 缓冲区大小 / 历史深度 / USB 角色 / 清空
- **编辑态 5 秒无操作自动退出**，避免忘记按键手势
- **无面板也能用**：OLED 初始化失败仅告警，所有功能仍可通过网络访问

### 🕒 带时间戳的抓包历史

`/api/data` 是"读走即消费"的裸字节流——没有时间、没有方向，轮询一次就没了。
抓包模块（[`main/capture.c`](main/capture.c)）另外维护一份**有界环形历史**：

- **微秒级时间戳 + 方向标记**（rx = 目标→主机，tx = 主机→目标）
- 按**数据块**记录，不是按字节：8 字节开销对应最多 128 字节负载
- 默认 **256 条 / ≈35 KB**，写满后自动覆盖最旧的（`dropped` 标志会告诉你）
- 可通过 `?since=` 游标**反复分页读取同一段历史**，不是破坏性读取
- 导出 CSV：`seq,uptime_ms,delta_ms,dir,len,data`，`delta_ms` 直接给出报文的相对间隔

```bash
# 看最近 32 条（文本）
curl "http://<IP>/api/capture?max=32"
# 导出 CSV（十六进制数据），用 Excel / Python 排时序
curl "http://<IP>/api/capture?since=0&max=96&fmt=csv" -o capture.csv
# 只取摘要，判断有没有新数据
curl "http://<IP>/api/capture?want=meta"
```

### ⚡ WebSocket 实时推送

轮询 `/api/data` 每次都要一个请求往返，突发数据还会被合并或丢失。WebSocket 改成**服务端主动推**：

- **端点**：`ws://<IP>/ws` —— 与 REST 共用 80 端口，不需要额外开端口
- **UART 数据即时下发**：二进制帧，首字节是方向标记（`0x00` = RX 目标→主机，`0x01` = TX 主机→目标）
- **反向命令**：客户端发 JSON 文本帧即可控制设备

| 客户端发送 | 作用 |
|---|---|
| `{"cmd":"send","data":"AT\r\n"}` | 发送文本到目标（`{"cmd":"send","v":"AT"}` 为简写） |
| `{"cmd":"raw","hex":"AA55"}` | 发送任意二进制 |
| `{"cmd":"baud","v":921600}` | 切换波特率 |
| `{"cmd":"clear","v":"capture"}` | 清空抓包历史（`v` 可为 `serial`/`capture`/`all`） |
| `{"cmd":"status"}` | 请求一次状态快照 |
| `{"cmd":"capture","v":20}` | 让设备回推最近 N 条抓包历史 |

设备回推的文本帧是 JSON：`{"t":"status"|"ack"|"baud"|"capture", ...}`。

**参考客户端** [tools/ws_console.py](tools/ws_console.py)：

```bash
pip install -r tools/requirements.txt
python tools/ws_console.py --host <IP>                 # 实时控制台（彩色区分 TX/RX）
python tools/ws_console.py --host <IP> --raw > log.bin # 只导出目标板数据
```

控制台里的本地命令：`/status`、`/capture [n]`、`/clear [what]`、`/baud <n>`。

> 设计上刻意做成**有界队列 + 丢帧**：浏览器卡住时丢的是网页帧，绝不会给 UART 数据通路施加反压。
> 无客户端连接时完全不入队，空闲零开销。

### 🚀 OTA 无线升级 + 自动回滚

- 双 OTA 槽（各 4 MB）+ `otadata`，`POST /api/ota` 把固件写入**非活动槽**后自动重启
- **崩溃自动回滚**：新镜像启动失败（没走到 `app_main` 末尾）会被 bootloader 自动回退
- OTA 与 USB 角色无关，`off` / `dap` / `ttl` 三种模式下都能推固件

### 🔎 USB 免串口诊断

| 接口 | 内容 |
|------|------|
| `GET /api/usbtrace` | 每条 SETUP 报文 + 结果（ok/STALL）+ EP0 分包事件 |
| `GET /api/usbdesc` | 设备真实发出的描述符字节 |
| `GET /api/status` | `dapcfg`（主机是否完成配置）、`daprx`/`daptx`（命令/响应计数）、`rst`、`slot`、`ota`、`debug`/`debug_io`（调试引脚占用） |

跑 Keil/pyOCD 时 `daprx`/`daptx` 应同步增长；命令/响应配对即 SWD 链路正常。

### 🎯 DUT 复位控制

`GET /api/dut/reset?ms=25&boot=1` 直接拉低排针上的 **NRST（IO12）** 复位目标板，
`boot=1` 时同时保持 **BOOT** 为低 —— 配合 `ttl` 模式可**全程无线**给另一块 MCU 烧录。

> BOOT 没有专用引脚：它用的是当前被分配为 `GPIO` 协议的那个拓展 IO。
> 所以要在 Web 页把某个拓展 IO 设成 `GPIO`，再把它接到目标的 BOOT/IO0。
> NRST（IO12）是固定的，随排针引出。

### 🤖 AI 集成（MCP Server）

纯主机侧 Python 程序，通过 HTTP 调用设备 `/api/*`，**不占用串口通道、不干扰 Web 页面**。
24 个工具覆盖连接、串口收发、抓包历史与 CSV 导出、PWM/SPI/I2C 读写、引脚排列、按键模拟、USB 角色、目标复位、缓冲区管理。
详见 [mcp/README.md](mcp/README.md)。

---

## 🔌 硬件与引脚

板载硬件见上文 [板载硬件](#-板载硬件)。本节是排针的完整信号定义。

### 16 Pin 排针

| 排针信号 | ESP32-S3 | 用途 |
|---------|----------|------|
| 5V | — | USB-C 5V 直出（给目标供电或取电） |
| 3V3 | — | LDO 输出（给目标供电） |
| GND ×2 | — | 共地 |
| **TXD0 / RXD0** | IO43 / IO44 | **本板控制台**（日志，115200） |
| **TXD1 / RXD1** | IO47 / IO21 | 到目标板的串口桥（TCP 3333 / USB-TTL 走这条） |
| **SCL / SDA** | IO10 / IO11 | I2C0，与 OLED 共用总线；可嗅探外部 I2C |
| **NRST** | IO12 | 目标复位（`/api/dut/reset`） |
| **SWCLK / SWDIO** | IO13 / IO14 | **CMSIS-DAP 探针**（`dap` 模式） |
| **TDI / TDO / nTRST** | IO48 / IO38 / IO39 | **JTAG 调试**（探针独占，见下） |
| **SWO** | IO40 | **ITM 跟踪**（目标 printf 单向输出到主机） |
| **拓展 IO** | IO45（剩 1 个） | 协议可排列：`PWM / GPIO / SCK / MOSI / MISO / CS / SDA / SCL` |

### JTAG 与 SWO 的引脚占用（重要）

板子只剩 5 个拓展 IO，而调试探针除了 SWD 的三根专用线之外还需要四根信号线，
它们和 SPI/I2C 协议分析抢同一批拓展 IO：

| 信号 | 占用 | 用途 |
|------|------|------|
| TDI | IO48 | JTAG 数据输入（DAP 内核位翻转输出） |
| TDO | IO38 | JTAG 数据输出（目标驱动，只读） |
| nTRST | IO39 | JTAG 测试复位 |
| SWO | IO40 | ITM 跟踪，UART 方式接收 |

**仲裁规则**：探针优先。启动时（`dap` 模式）[`main/debug_pins.c`](main/debug_pins.c) 接管这四个 IO，
如果某个监控（SPI 抓包 / I2C 嗅探）正占着其中一个，**该监控会被停止并在日志与 `/api/status` 里说明原因**
（`debug` / `debug_io` 两个字段），不会静默失效。

所以实际可用的协议排列只剩 **IO45 一个槽位**。这是一次明确的取舍：
要么要 JTAG + SWO + 一个协议通道，要么要完整的 SPI+I2C 排列。
不用 JTAG/SWO 时把 USB 角色切回 `off` 或 `ttl`，四个 IO 就都还给协议排列。

> IO45 被刻意排除在调试信号之外：它是 VDD_SPI 的 strapping 引脚，
> 不该有任何调试信号在每次复位时挂在上面。

### JTAG 支持

MCU 用的是真 DAPLink 内核（`main/dap/JTAG_DP.c`，ARM Apache-2.0 源码），
所以主机侧无需区分——Keil / pyOCD / OpenOCD 通过 `DAP_Connect` 选择 `DAP_PORT_JTAG` 即可。

JTAG 的价值在于覆盖 SWD 覆盖不到的目标：**RISC-V**（ESP32-C3/C6、GD32VF103）、
**FPGA 配置口**、以及**多器件菊花链**（`DAP_JTAG_DEV_CNT` 支持最多 4 个器件）。

### SWO / ITM 单线跟踪

目标的 `ITM_SendChar()` 输出通过单根 SWO 线送到主机，不用额外接 UART：

- **模式**：UART（异步）。Manchester 模式未启用——它需要 RMT 外设，且现在所有探针都用 UART 模式
- **速率**：主机通过 `DAP_SWO_Baudrate` 设置，上限 4 Mbaud
- **缓冲**：4 KB 环形缓冲，写满时**丢最旧**而不是停止采集
- **实现**：[`main/swo_uart.c`](main/swo_uart.c)。ARM 原版 `SWO.c` 依赖 CMSIS `Driver_USART`（ESP-IDF 没有），
  所以按 ESP-IDF UART 驱动重写了字节来源，**命令层、响应编码、错误位语义与 ARM v2.0.1 完全一致**
- 溢出通过标准的 `DAP_SWO_BUFFER_OVERRUN` 状态位上报，主机的既有判断逻辑无需改动

在 IDE 里这样用：Keil 勾选 *Trace → SWO*；pyOCD 用 `--swo`；OpenOCD 配 `cmsis_dap` 的 SWO 通道。

### ⚡ 逻辑电平警告

**整板 3.3V，排针不带电平转换。**

- 所有信号直连 ESP32-S3，接 **3.3V 目标**是安全的
- 接 **5V 逻辑目标**（老式 Arduino、某些 8051/AVR 板）**必须自行加电平转换**
  （原来的 TXB0106 已经取消了）
- 反向同理：目标板往 ESP32-S3 灌 5V 会损坏 IO

唯一带 3V3 上拉的是 **BOOT 和 EN 两个按键**，与排针无关。

### 引脚配置的真实含义

拓展 IO 的**协议功能可排列并持久化到 NVS**，但 **IO 号本身不可改**——
改的是拓展 IO（IO48/IO45/IO38/IO39/IO40）上各自承载哪种协议。
Web 页、OLED Config 页或 MCP 的 `set_pins` 都能改，自动交换、拒绝非法排列。

引脚真源是 [`main/pinout.h`](main/pinout.h)。

> `IO45` 同时是 strapping 引脚（VDD_SPI），复位瞬间被外部拉低可能影响启动模式。
> 它也是**唯一没有分配给调试探针**的拓展 IO。

### 不可用的 IO

`IO19/IO20` 原生 USB（DAP/TTL 占用）· `IO26–IO32` SPI0/1 flash · `IO33–IO37` Octal PSRAM
（`IO43/IO44` 已作为 TXD0/RXD0 引出，可用但会与日志输出冲突）

---

## 📦 编译与烧录

### 环境要求

- **ESP-IDF v5.5.x**（本项目在 v5.5.5 上验证）
- 目标芯片 **ESP32-S3-WROOM-1-N16R8**（16 MB quad flash + 8 MB octal PSRAM）
- 首次构建会自动拉取托管组件（CherryUSB 1.4.3、cJSON）

> ⚠️ **`sdkconfig.defaults` 改了不一定生效**：IDF 只在 `sdkconfig` 不存在时才用 defaults 生成它。
> 改完 defaults 后必须先删除 `sdkconfig` 再构建，否则会按旧配置编译。

### 编译

```bash
idf.py set-target esp32s3
idf.py build
```

产物：`build/nexlink.bin`、`build/nexlink.elf`
（固件约 1.31 MB，单个 OTA 槽 4 MB，余量约 59%）

### 烧录

```bash
idf.py -p COMx flash monitor
```

### 控制台

控制台在 **UART0（IO43 TX / IO44 RX，115200）**，已作为 **TXD0 / RXD0** 引到排针，
因为原生 USB 已被 DAP 探针占用。
用 3.3V USB-TTL 转接板交叉接线（转接板 RX→TXD0、TX→RXD0、GND↔GND）即可看日志。

**刷坏后的恢复**：按住 BOOT → 点 RESET → 松开 BOOT，ROM 会恢复 USB-Serial-JTAG，
`idf.py -p COMx flash` 仍可救回。

---

## 🚀 使用指南

### 首次使用

1. **烧录固件**：`idf.py -p COMx flash monitor`
2. **联网**：设备无凭据时自动开 AP
   - 手机/电脑连接热点 `NexLink-xxxx`（密码 `12345678`）
   - 浏览器访问 `http://192.168.4.1`，在 Config 页填入目标 WiFi 的 SSID/密码
   - OLED 屏幕会显示拿到的 IP
3. **日常访问**：浏览器 `http://<IP>`，或用 TCP 客户端连 `<IP>:3333`

### 三种典型用法

**A. 当无线串口调试器**

```
PuTTY / SSCOM ──TCP <IP>:3333──> NexLink ──UART1──> 目标板
```
主机端也可以用 [`tools/wifi_serial_bridge.py`](tools/wifi_serial_bridge.py)：它借助 com0com
自动创建虚拟串口对，让**只认 COM 口的串口工具**（Arduino IDE、某些烧录器）直接连上无线设备。

```bash
pip install -r tools/requirements.txt
python tools/wifi_serial_bridge.py --host <IP>    # 全自动
python tools/wifi_serial_bridge.py --status       # 只看状态
```

**B. 当 USB 调试探针（烧录/调试 Cortex-M）**

1. Web 页或 OLED Config 页把 USB 角色切到 `dap`（或 `GET /api/usb_mode?m=dap`）
2. USB-C 插电脑 → 设备管理器出现 **NexLink CMSIS-DAP**（WinUSB，免驱）
3. Keil / pyOCD / OpenOCD 选择该 CMSIS-DAP 探针即可
4. 验证：`/api/status` 的 `dapcfg ≥ 1`，且 `daprx`/`daptx` 同步增长
5. 要刷**本板**固件时先切回 `off`

**C. 当 USB-TTL 下载器（给另一块 MCU 烧录）**

1. 角色切到 `ttl`，把目标板的 UART 接到排针 **TXD1 / RXD1**（交叉：TXD1→目标 RX、RXD1←目标 TX），
   目标复位接 **NRST**，目标 BOOT/IO0 接任意一个被设为 `GPIO` 协议的拓展 IO，并共地
2. 电脑出现虚拟 COM 口，直接用 esptool / STM32 Flash Loader 下载
3. 可用 `GET /api/dut/reset?boot=1` 让目标进下载模式（无需按按键）

**D. 当协议分析仪**

在 Web 页把拓展 IO 配上 SPI 或 I2C 协议，然后把探针接到目标总线；
`/api/spi` / `/api/i2c` 会返回抓到的历史事务，OLED 上也能实时滚屏查看。
注意探针同样是 3.3V 直连，5V 总线请先做电平转换，否则会打坏 IO。

### 无线固件升级（OTA）

```powershell
curl.exe -H "Expect:" -X POST --data-binary "@build\nexlink.bin" http://<IP>/api/ota
```

- **必须带 `-H "Expect:"`**：curl 对大 POST 默认发 `Expect: 100-continue`，
  设备的轻量 HTTP 服务器处理不了会重置连接（表现为 curl 错误 56）
- 升级写入非活动分区并自动重启；用 `/api/status` 的 `slot` / `ota` / `rst` 字段确认结果

---

## 🌐 HTTP API 速查

共 29 个路由。常用：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | Web 状态页（内嵌完整控制面板） |
| GET | `/api/status` | 状态大 JSON：WiFi/IP/波特率/收发计数/USB 角色/DAP 计数/OTA 槽位 |
| GET | `/api/data` · `/api/mcp/data` | 读串口缓冲（后者是 MCP 独立缓冲，不抢 Web） |
| GET | `/api/capture` | **带时间戳与方向的抓包历史**：`?since=&max=&fmt=text\|hex\|csv&want=chunks\|meta` |
| WS | `/ws` | **实时推送**：UART 数据即时下发 + JSON 反向命令（详见「WebSocket 实时推送」） |
| POST | `/api/send` | 发串口数据（单请求上限 512 B） |
| GET | `/api/baud?b=115200` | 切波特率 |
| GET | `/api/pins` · POST `/api/pins` | 读/写引脚协议排列 |
| GET | `/api/pwm` · POST `/api/pwm/out` | 读 PWM / 输出 PWM |
| GET | `/api/spi` · POST `/api/spi/send` | SPI 抓包 / 主机发送 |
| GET | `/api/i2c` · POST `/api/i2c/send` | I2C 抓包 / 主机收发 |
| GET | `/api/swd/debug` | SWD 调试（读 IDCODE 等） |
| GET | `/api/wifi` · POST `/api/wifi` | 扫描 / 配网 |
| GET | `/api/usb_mode?m=off\|dap\|ttl` | 读/切 USB 角色 |
| GET | `/api/usb_dap?toggle=1` | 快速开关 DAP 探针 |
| GET | `/api/dut/reset?ms=25&boot=1` | 复位目标板（可进下载模式） |
| GET | `/api/usbtrace` · `/api/usbdesc` | USB 诊断 |
| POST | `/api/ota` | 无线升级 |
| GET | `/api/capacity` · POST `/api/clear` · POST `/api/cfg` | 缓冲区查询/清空/调整（`clear` 支持 `capture`） |
| GET | `/api/ai/config` · POST `/api/ai/config` | AI/MCP 相关配置 |
| GET | `/api/btn?b=1\|2\|3&a=press\|long` | 远程注入按键（建议间隔 ≥250 ms） |

---

## 📁 项目结构

```
nexlink/
├── main/
│   ├── main.c                 # 入口：启动链 + USB 安全模式守卫 + OTA 健康检查
│   ├── pinout.h               # ★ 引脚真源
│   ├── pin_config.c/h         # 拓展 IO 的协议排列 + USB 角色持久化 (NVS)
│   ├── capture.c/h            # 带时间戳与方向的抓包环形缓冲
│   ├── debug_pins.c/h         # 调试探针对拓展 IO 的归属仲裁 (TDI/TDO/nTRST/SWO)
│   ├── swo_uart.c             # SWO/ITM 跟踪（UART 模式，替换 ARM 的 SWO.c）
│   ├── pwm_mon.c/h            # PWM 测量 (RMT) + LEDC 输出
│   ├── spi_mon.c/h            # SPI 从机抓包 / 主机发送
│   ├── i2c_mon.c/h            # I2C 从机抓包 / RMT 被动嗅探
│   ├── buttons/               # 按键驱动（去抖 / 长按，无双击延迟）
│   ├── display/               # OLED SSD1306 + 菜单 UI + 中文字库
│   ├── dap/                   # CMSIS-DAP 内核 (ARM DAPLink) + USB 传输 (dap_usb.c)
│   ├── net/                   # TCP 3333 / HTTP 80 / WebSocket /ws / DAP 5555 / USB 诊断
│   ├── serial/                # UART1 桥 + USB-TTL 虚拟串口
│   ├── swd/                   # SWD bit-bang + TCP 侧 CMSIS-DAP 处理器
│   └── wifi/                  # WiFi STA/AP 管理
├── managed_components/        # 托管组件（CherryUSB 内含 USB 诊断钩子）
├── mcp/                       # MCP Server（AI 集成）+ 一键安装脚本
├── tools/                     # 主机侧脚本：com0com 虚拟串口桥 / WebSocket 控制台
├── partitions.csv             # 双 OTA 分区表
├── sdkconfig.defaults         # 默认配置（含 CherryUSB / 控制台 / I2C slave v2 / WebSocket）
└── CMakeLists.txt
```

---

## 🔧 配置说明

### WiFi

- **STA**：连接已有网络，凭据存 NVS
- **AP**：`NexLink-xxxx` / 密码 `12345678` / `http://192.168.4.1`（最多 2 个客户端）
- 空密码表示连接**开放网络**（认证门限会自动降为 OPEN）

### 串口

默认 115200，支持 9600 / 115200 / 460800 / 921600。

### 关键 sdkconfig 开关

| 开关 | 为什么必须 |
|------|-----------|
| `CONFIG_CHERRYUSB` + `_DEVICE` + `_DEVICE_SPEED_FS` + `_DEVICE_DWC2_ESP` | DAP 探针的 USB 设备栈 |
| `CONFIG_CHERRYUSB_DEVICE_CDC_ACM` | USB-TTL 下载桥 |
| `CONFIG_ESP_PHY_ENABLE_USB=y` | 否则 `esp_wifi_init()` 会关掉 USB PHY，探针一联网就掉线 |
| `CONFIG_ESP_CONSOLE_UART_DEFAULT` + `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` | 让出原生 USB 给 DAP |
| `CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2=y` | `i2c_mon.c` 用的是 IDF 5.5 的 v2 从机 API |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | OTA 崩溃自动回滚 |
| `CONFIG_HTTPD_WS_SUPPORT=y` | `net/ws_server.c` 编译的前提；关闭时 `esp_http_server.h` 会隐藏全部 `httpd_ws_*` 声明 |
| `CONFIG_LWIP_MAX_SOCKETS=16` | WebSocket 客户端列表的容量上限 |

根 `CMakeLists.txt` 另需 `add_definitions(-DCONFIG_USBDEV_ADVANCE_DESC=1)`（CherryUSB 高级描述符路径，
MS OS 描述符靠它才能生成）与 `add_compile_options(-pipe)`（避免临时 `.s` 文件写 `%TEMP%` 失败）。

---

## 🐛 故障排除

### USB 探针相关

**1. 设备管理器里 "NexLink CMSIS-DAP" 报代码 28（未安装驱动）**
Windows 会把"此设备没有 OS 描述符"的结论按 `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\<VID><PID><bcdDevice>`
缓存（`osvc=0,0` = 不再探测）。处理：把固件的 `bcdDevice` 上调（如 0x0100→0x0101）换新缓存键，重刷重启后会自动重握手。

**2. 代码 10 / 43，主机反复复位、从不发 SET_CONFIGURATION**
通常是 MS OS 描述符集被主机拒绝。免串口诊断：`GET /api/usbtrace`（每条 SETUP + 结果）、
`GET /api/usbdesc`（真实描述符字节）。本项目采用 `bcdUSB=2.00` + WCID 1.0 方案，Win7~Win11 通用。

**3. 崩溃过一次后 USB 角色自动变成 off**
这是安全模式守卫的**有意行为**（见 `main/main.c`）。恢复：Web 页 / OLED Config 页切回，
或 `GET /api/usb_mode?m=dap`。

**4. 确认探针真的在工作**
`/api/status` 的 `dapcfg ≥ 1` 表示主机已完成配置；跑 Keil/pyOCD 时 `daprx`/`daptx` 应同步增长。
`debug` / `debug_io` 显示调试探针占用了哪些拓展 IO。

### JTAG / SWO 相关

**5. 切到 JTAG 后目标连不上**
确认目标的 TDI/TDO/nTRST 已接到 IO48/IO38/IO39，且 `dap` 模式下 `/api/status` 的 `debug=1`。
JTAG 需要 TCK/TMS/TDI/TDO 四线（TCK/TMS 复用 SWCLK/SWDIO）；nTRST 在多数目标上可以不接。

**6. 切到 JTAG 后 SPI 抓包 / I2C 嗅探停了**
这是预期行为：调试探针接管 IO48/IO38/IO39/IO40，日志与 `/api/status` 会写明是哪个监控被停止。
把 USB 角色切回 `off` / `ttl` 即可让这些 IO 回到协议排列。

**7. IDE 里看不到 SWO 输出**
依次确认：目标已使能 ITM 且 SWO 引脚接到了 IO40；IDE 的 trace 时钟与目标 CPU 一致；
速率不超过 4 Mbaud。目标侧 `TRACE_CR` 没使能时不会产生任何数据，宿主侧看起来就像"毫无反应"。

### 网络与串口

**8. 连不上 WiFi** — 检查密码与信号；若设备进了 AP 模式，重连 `NexLink-xxxx` 重新配网。

**6. 串口无数据** — 核对波特率（`/api/status` 可查当前值）、确认 TX/RX 交叉接线、确认目标板有输出。

**7. 接 5V 目标板后 IO 损坏 / 行为异常** — 排针**没有电平转换**（TXB0106 已取消），
信号线是 ESP32-S3 原生 3.3V，5V 目标必须自行加转换后再接。

**8. OTA 报 curl 错误 56** — 忘了加 `-H "Expect:"`。

### 调试手段

```bash
idf.py monitor          # UART0 控制台日志（IO43/IO44，115200）
```

- Web 页实时查看状态与波形
- OLED 显示当前页面、IP 与 USB 角色
- `GET /api/usbtrace`、`GET /api/usbdesc` 看 USB 层

---

## 📄 许可证

本项目采用 **Apache License 2.0**，见 [LICENSE](LICENSE)。

`main/dap/` 下的 CMSIS-DAP 内核来自 ARMmbed DAPLink（经
[cherry-embedded/CherryDAP](https://github.com/cherry-embedded/CherryDAP) 移植），
同样为 Apache-2.0，详见 [NOTICE](NOTICE)。

上游项目：[JasonYANG170/ai-wireless-debugger](https://gitee.com/JasonYANG170/ai-wireless-debugger)。
