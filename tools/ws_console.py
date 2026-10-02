#!/usr/bin/env python3
"""
ws_console.py — NexLink WebSocket 实时控制台

把 NexLink 的 UART 流实时显示出来（不再轮询 /api/data），并支持交互式发送。
既是一个能直接用的工具，也是 WebSocket 协议的参考实现。

    pip install -r tools/requirements.txt
    python ws_console.py --host 192.168.1.129                 # 只看流
    python ws_console.py --host 192.168.1.129 --baud 115200    # 顺带设波特率
    python ws_console.py --host 192.168.1.129 --dump-capture 30# 顺带导出抓包

    # 非交互模式（用于脚本/CI）：把流原样写到 stdout
    python ws_console.py --host 192.168.1.129 --raw > log.bin

协议（设备端 main/net/ws_server.c）
-----------------------------------
服务端 → 客户端
  二进制帧            原始 UART 数据。帧首字节是方向标记：
                        b'\\x00' = RX（目标 → 主机）
                        b'\\x01' = TX（主机 → 目标）
  文本帧              JSON 控制消息：{"t":"status"|"ack"|"baud"|"capture", ...}

客户端 → 服务端（文本帧，JSON）
  {"cmd":"send","data":"AT\\r\\n"}   发送文本（JSON 字符串里的 \\n 会被还原成换行）
  {"cmd":"send","v":"AT"}            同上，简写形式
  {"cmd":"raw","hex":"AA55"}         发送任意二进制
  {"cmd":"baud","v":921600}          切换波特率
  {"cmd":"clear","v":"capture"}      清空抓包历史（v 可为 serial/capture/all）
  {"cmd":"status"}                   请求一次状态快照
  {"cmd":"capture","v":20}           让设备回推最近 N 条抓包（文本帧形式）
"""

import argparse
import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    print("需要 websockets：pip install -r tools/requirements.txt", file=sys.stderr)
    raise SystemExit(1)

DIR_RX, DIR_TX = 0, 1


def render(data: bytes, our_tx: bool = False) -> str:
    """把原始字节变成可读文本（保留换行，非可打印字符转义）。"""
    out = []
    for b in data:
        if b == 0x0A:
            out.append("\n")
        elif b == 0x0D:
            pass                      # 与 \n 合并，避免 Windows 上双换行
        elif 0x20 <= b < 0x7F or b >= 0x80:
            out.append(chr(b))
        else:
            out.append(f"\\x{b:02X}")
    return "".join(out)


async def reader(ws, raw: bool):
    async for msg in ws:
        if isinstance(msg, (bytes, bytearray)) and msg:
            direction, payload = msg[0], bytes(msg[1:])
            if raw:
                # 只输出目标板发来的数据，便于直接重定向到文件
                if direction == DIR_RX:
                    sys.stdout.buffer.write(payload)
                    sys.stdout.buffer.flush()
                continue
            tag = "[TX]" if direction == DIR_TX else "[RX]"
            text = render(payload, direction == DIR_TX)
            if direction == DIR_TX:
                # 我们发出去的，回显成一行提示，避免和目标的输出混在一起
                print(f"\033[36m{tag} {text!r}\033[0m")
            else:
                print(f"\033[32m{tag}\033[0m {text}", end="")
                sys.stdout.flush()
        else:
            if raw:
                continue
            try:
                j = json.loads(msg)
            except (ValueError, TypeError):
                print(f"[ws] {msg}")
                continue
            print(f"\033[33m[ws] {json.dumps(j, ensure_ascii=False)}\033[0m")


async def stdin_sender(ws):
    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:                      # EOF
            break
        stripped = line.rstrip("\r\n")
        if stripped.startswith("/"):
            # 本地命令，不发给设备
            if stripped == "/status":
                await ws.send(json.dumps({"cmd": "status"}))
            elif stripped.startswith("/capture"):
                parts = stripped.split()
                n = int(parts[1]) if len(parts) > 1 else 20
                await ws.send(json.dumps({"cmd": "capture", "v": n}))
            elif stripped.startswith("/clear"):
                parts = stripped.split()
                await ws.send(json.dumps({"cmd": "clear",
                                          "v": parts[1] if len(parts) > 1 else "capture"}))
            elif stripped.startswith("/baud"):
                parts = stripped.split()
                if len(parts) > 1:
                    await ws.send(json.dumps({"cmd": "baud", "v": int(parts[1])}))
            elif stripped == "/quit":
                break
            else:
                print("本地命令: /status /capture [n] /clear [what] /baud <n> /quit")
            continue
        # 普通输入按行发给目标
        await ws.send(json.dumps({"cmd": "send", "data": stripped + "\n"}))


async def main_async(args):
    url = f"ws://{args.host}:{args.port}/ws"
    print(f"连接 {url} ...", file=sys.stderr)

    async with websockets.connect(url, max_size=None) as ws:
        print("已连接。Ctrl-C 退出。", file=sys.stderr)

        if args.baud:
            await ws.send(json.dumps({"cmd": "baud", "v": args.baud}))
        if args.dump_capture:
            await ws.send(json.dumps({"cmd": "capture", "v": args.dump_capture}))
        await ws.send(json.dumps({"cmd": "status"}))

        tasks = [asyncio.create_task(reader(ws, args.raw))]
        if not args.raw and sys.stdin.isatty():
            tasks.append(asyncio.create_task(stdin_sender(ws)))

        done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for t in pending:
            t.cancel()


def main():
    p = argparse.ArgumentParser(description="NexLink WebSocket 实时控制台")
    p.add_argument("--host", required=True, help="设备 IP，如 192.168.1.129")
    p.add_argument("--port", type=int, default=80, help="HTTP/WS 端口（默认 80）")
    p.add_argument("--baud", type=int, default=0, help="连接后设置目标波特率")
    p.add_argument("--dump-capture", type=int, default=0, metavar="N",
                   help="连接后让设备回推最近 N 条抓包历史")
    p.add_argument("--raw", action="store_true",
                   help="非交互：只把目标板数据原样写到 stdout（可重定向到文件）")
    args = p.parse_args()

    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass
    except OSError as e:
        print(f"连接失败: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
