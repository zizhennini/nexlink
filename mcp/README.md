# NexLink MCP Server

让 AI 工具（Claude / Cursor / OpenCode / Cline）直接远程调试硬件。

MCP Server 是纯主机侧程序：它通过 HTTP 调用 NexLink 设备的 `/api/*` REST 接口，
不占用设备的 TCP 串口通道，也不干扰 Web 页面。

## 安装

### 1. 安装 Python 依赖

```bash
cd <项目路径>/mcp
pip install -r requirements.txt
```

### 2. 注册到 AI 工具（推荐：自动安装脚本）

```bash
python install.py            # 交互式选择
python install.py all        # 全部工具
python install.py claude     # 只装 Claude Desktop
python install.py --print    # 只打印 JSON 不写文件
```

脚本会自动探测当前平台的配置文件路径，**合并**（不覆盖）已有配置，
写入前留 `.bak` 备份，并用「临时文件 + fsync + 原子替换」落盘，中途失败不会留下半截 JSON。

支持的工具与路径：

| 工具 | 配置路径（Windows） |
|------|--------------------|
| Claude Desktop | `%APPDATA%\Claude\claude_desktop_config.json` |
| Cursor | `~/.cursor/mcp.json` |
| OpenCode | `~/.opencode/config.json` |
| Cline (VSCode) | `%APPDATA%\Code\User\globalStorage\saoudrizwan.claude-dev\settings\cline_mcp_settings.json` |

### 3. 手动注册

注册名统一使用 **`nexlink`**。

Claude Code：

```bash
claude mcp add nexlink --scope user -- python <项目路径>/mcp/mcp_server.py
```

Claude Desktop / Cursor / Cline（编辑对应 JSON）：

```json
{
  "mcpServers": {
    "nexlink": {
      "command": "python",
      "args": ["<项目路径>/mcp/mcp_server.py"]
    }
  }
}
```

OpenCode：

```json
{
  "nexlink": {
    "type": "local",
    "command": ["python", "<项目路径>/mcp/mcp_server.py"]
  }
}
```

> 本目录的 `config.json` 就是上述格式的模板（含占位路径），可直接引用。

## 使用

首次连接时 AI 会询问设备 IP（设备 OLED 屏幕上显示，或 AP 模式下用 `192.168.4.1`），
输入后自动保存到 `mcp_config.json`，下次启动自动重连。

```
"连接我的 NexLink 调试器，IP 是 192.168.1.129"
"读取串口数据"
"发送 AT 指令到目标板"
"看一下 I2C 总线上最近 10 笔事务"
"把 PWM 设成 1kHz、50% 占空比"
"把 USB 角色切到 dap"
"复位目标板并强制进入下载模式"
"把抓包历史导成 CSV，我要看 AT 指令之间的间隔"
```

## 可用工具（24 个）

### 连接与状态

| 工具 | 功能 |
|------|------|
| `connect_device(ip)` | 连接设备（留空用上次保存的 IP） |
| `disconnect_device()` | 断开连接 |
| `get_status()` | WiFi/IP/波特率/收发计数/运行时间/当前页面/USB 角色/OTA 槽位 |
| `get_capacity()` | 各缓冲区占用与剩余内存 |

### 串口（UART1 → DUT）

| 工具 | 功能 |
|------|------|
| `read_serial(max_bytes)` | 读目标板串口数据（独立缓冲区，不抢 Web/MCP） |
| `send_serial(data, newline)` | 发串口数据，自动按 480B 分块（固件单请求上限 512B） |
| `set_baud(baud)` | 9600 / 115200 / 460800 / 921600 |

### 抓包历史（带时间戳与方向）

| 工具 | 功能 |
|------|------|
| `read_capture(since, max_chunks, fmt, meta_only)` | 读取带微秒时间戳与方向的历史，可反复读取同一段（分页游标 `since`） |
| `export_capture_csv(since, max_chunks, hex_data)` | 导出 CSV：`seq,uptime_ms,delta_ms,dir,len,data`，`delta_ms` 用于排协议时序 |
| `clear_capture()` | 只清抓包历史，不影响串口流与计数器 |

> `read_serial` 是"读走即消费"的裸字节流；要做协议时序分析请用 `read_capture` / `export_capture_csv`。

### 协议监控与外设

| 工具 | 功能 |
|------|------|
| `read_pwm()` / `send_pwm(freq, duty, enable)` | PWM 测量 / LEDC 输出 |
| `read_spi(count)` / `send_spi(hex, mode, clock)` | SPI 从机抓包 / 主机发送（二者互斥，自动切换） |
| `read_i2c(count)` / `send_i2c(addr, hex, mode, read_len)` | I2C 事务抓包 / 主机收发 |

### 引脚与按键

| 工具 | 功能 |
|------|------|
| `get_pins()` / `set_pins(order)` | 读取/设置 5 个自由 IO 的协议排列 |
| `press_button(btn, action)` | 模拟按键：`btn` 1/2/3，`action` `press`/`long` |

### USB 与目标板控制

| 工具 | 功能 |
|------|------|
| `toggle_usb_dap()` | 开/关 USB CMSIS-DAP 探针（便捷开关） |
| `set_usb_mode(mode)` | `off` / `dap` / `ttl` —— 切换 USB-C 角色 |
| `reset_target(boot, ms)` | 拉低 NRST 复位目标板；`boot=True` 同时保持 BOOT 进入下载模式 |

### 缓冲区管理

| 工具 | 功能 |
|------|------|
| `clear_buffer(target)` | 清空 `serial` / `spi` / `i2c` / `rx` / `capture` / `all` |
| `set_buffer_config(serial_buf, spi_hist, i2c_hist, rx_hist)` | 调整缓冲区大小与历史深度（设备端会钳位到安全范围） |

## 配置文件

| 文件 | 作用 |
|------|------|
| `mcp_config.json` | 设备 IP，键名是 **`ip`**（首次连接后自动生成） |

> 注意：键名是 `ip`，不是 `device_ip`。手改配置时请用 `{"ip": "192.168.1.129"}`。

## 故障排除

- **连接失败**：确认设备已联网、IP 正确、与主机在同一网段；浏览器能打开 `http://<IP>/api/status` 即说明网络正常。
- **`read_serial` 一直为空**：`/api/mcp/data` 是**独立缓冲区**，只包含调用之后新收到的数据；先 `send_serial` 触发目标板回显再读。
- **工具列表里没有 `nexlink`**：检查注册名是否为 `nexlink`，以及 `mcp_server.py` 路径是否为绝对路径。
