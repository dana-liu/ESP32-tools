#!/usr/bin/env python3
"""Send the signed-in Codex account's quota windows to the terrarium over USB."""

import argparse
import asyncio
import json
import os
import select
import signal
import termios
import time
import tty


async def read_quota(codex):
    process = await asyncio.create_subprocess_exec(
        codex, "app-server", "--listen", "stdio://",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
        start_new_session=True,
    )

    async def send(message):
        process.stdin.write((json.dumps(message) + "\n").encode())
        await process.stdin.drain()

    async def response(request_id):
        while line := await process.stdout.readline():
            message = json.loads(line)
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(message["error"].get("message", "Codex 请求失败"))
                return message["result"]
        raise RuntimeError("Codex App Server 已退出")

    try:
        async with asyncio.timeout(20):
            await send({"id": 1, "method": "initialize", "params": {
                "clientInfo": {"name": "esp32_terrarium", "version": "1.0"}
            }})
            await response(1)
            await send({"method": "initialized"})
            await send({"id": 2, "method": "account/rateLimits/read"})
            result = await response(2)
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        await process.wait()

    limits = (result.get("rateLimitsByLimitId") or {}).get("codex") or result.get("rateLimits") or {}
    fields = []
    for name in ("primary", "secondary"):
        window = limits.get(name) or {}
        minutes = window.get("windowDurationMins")
        used = window.get("usedPercent")
        fields.extend((
            minutes if isinstance(minutes, int) and 0 < minutes <= 99999 else 0,
            max(0, min(100, round(100 - used))) if isinstance(used, (int, float)) else -1,
        ))
    if fields[1] == fields[3] == -1:
        raise RuntimeError("当前 Codex 登录未返回额度数据")
    return fields


def send_quota(port, fields):
    packet = ("\nQUOTA " + " ".join(map(str, fields)) + "\n").encode()
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    settings = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        active = termios.tcgetattr(fd)
        active[4] = active[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, active)
        termios.tcflush(fd, termios.TCIFLUSH)

        for _ in range(3):
            if os.write(fd, packet) != len(packet):
                continue
            received = b""
            deadline = time.monotonic() + 1
            while (remaining := deadline - time.monotonic()) > 0:
                if not select.select([fd], [], [], remaining)[0]:
                    break
                chunk = os.read(fd, 1024)
                if not chunk:
                    break
                received += chunk
                if b"QUOTA OK" in received:
                    return
        raise RuntimeError("ESP32 未确认收到额度")
    finally:
        try:
            termios.tcsetattr(fd, termios.TCSANOW, settings)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="ESP32 的 USB 串口设备路径")
    parser.add_argument("--codex", default="codex", help="Codex CLI 可执行文件")
    parser.add_argument("--once", action="store_true", help="同步一次后退出")
    args = parser.parse_args()

    while True:
        try:
            fields = asyncio.run(read_quota(args.codex))
            send_quota(args.port, fields)
            print("ESP32 已确认额度同步", flush=True)
            if args.once:
                return 0
        except (OSError, RuntimeError, TimeoutError, ValueError) as error:
            print(str(error) or "读取 Codex 额度超时", flush=True)
            if args.once:
                return 1
        time.sleep(60)


if __name__ == "__main__":
    raise SystemExit(main())
