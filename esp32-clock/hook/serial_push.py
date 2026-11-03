#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_push.py —— 把钉钉日程通过 USB 串口推给 ESP32 时钟

为什么需要它：
    时钟固件走 USB 时不连 WiFi，既没有 NTP 也没有 HTTP 客户端。
    所以「当前时间」和「下一条日程」都得由 Mac 主动推过去。

数据来源：本地桥接服务 http://127.0.0.1:8787/api/next
推送协议（行式，| 分隔，\\n 结尾）：
    TIME|<epoch>               对时
    NEXT|<HH:MM>|<分钟>|<标题>  下一条日程
    NONE                       今天没有日程了

用法：
    python3 serial_push.py              # 前台跑，Ctrl-C 停
    python3 serial_push.py --check      # 只探测设备在不在
    python3 serial_push.py --once       # 推一次就退出（调试用）

环境变量：
    CLOCK_BRIDGE         桥接地址，默认 http://127.0.0.1:8787
    CLOCK_PUSH_INTERVAL  日程轮询间隔秒，默认 5
    CLOCK_TIME_INTERVAL  对时间隔秒，默认 60
"""

import glob
import json
import os
import select
import sys
import termios
import time
import urllib.request

BRIDGE = os.environ.get("CLOCK_BRIDGE", "http://127.0.0.1:8787").rstrip("/")
PUSH_INTERVAL = float(os.environ.get("CLOCK_PUSH_INTERVAL", "5"))
TIME_INTERVAL = float(os.environ.get("CLOCK_TIME_INTERVAL", "60"))

PORT_PATTERNS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*")


def find_port():
    for pat in PORT_PATTERNS:
        found = sorted(glob.glob(pat))
        if found:
            return found[0]
    return None


def open_port(path):
    """打开串口。注意不要碰 DTR/RTS —— ESP32-S3 的 USB-Serial/JTAG 会把它当复位信号"""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0                                          # iflag
    attr[1] = 0                                          # oflag
    attr[2] = (attr[2] & ~(termios.PARENB | termios.CSTOPB
                           | termios.CSIZE | termios.HUPCL)) \
        | termios.CS8 | termios.CREAD | termios.CLOCAL   # cflag
    attr[3] = 0                                          # lflag
    attr[4] = termios.B115200                            # ispeed
    attr[5] = termios.B115200                            # ospeed
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    return fd


def drain(fd):
    """读掉设备回的 OK/PONG，避免缓冲区堆积"""
    out = b""
    while True:
        r, _, _ = select.select([fd], [], [], 0)
        if not r:
            break
        try:
            chunk = os.read(fd, 512)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    return out


def send(fd, line):
    os.write(fd, (line + "\n").encode("utf-8"))


def fetch_next():
    """返回 (HH:MM, minutes, title) 或 None"""
    try:
        raw = urllib.request.urlopen(BRIDGE + "/api/next", timeout=3).read()
        data = json.loads(raw)
    except Exception:
        return None
    n = data.get("next")
    if not n:
        return ("", 0, "")          # 明确表示「没有」
    start = n.get("start") or ""
    hhmm = start[11:16] if len(start) >= 16 else "--:--"
    title = (n.get("title") or "").replace("|", "／").replace("\n", " ")
    return (hhmm, int(n.get("minutes_until") or 0), title)


def push_once(fd, with_time=False):
    if with_time:
        send(fd, "TIME|%d" % int(time.time()))
    item = fetch_next()
    if item is None:
        return False                # 桥接没起来，这次跳过
    if item[0] == "":
        send(fd, "NONE")
    else:
        send(fd, "NEXT|%s|%d|%s" % item)
    return True


def main():
    check_only = "--check" in sys.argv
    once = "--once" in sys.argv

    port = find_port()
    if not port:
        print("未找到 USB 串口设备（/dev/cu.usbmodem*）")
        return 1
    if check_only:
        print("OK %s" % port)
        return 0

    fd = open_port(port)
    print("已连接 %s" % port)
    print("桥接     %s" % BRIDGE)
    print("推送间隔 %.0fs / 对时 %.0fs" % (PUSH_INTERVAL, TIME_INTERVAL))

    last_time = 0.0
    last_state = None
    while True:
        try:
            now = time.time()
            need_time = (now - last_time) >= TIME_INTERVAL
            if push_once(fd, with_time=need_time):
                last_time = now
            if once:
                time.sleep(0.5)
                print(drain(fd).decode("utf-8", "replace").strip() or "(无回复)")
                return 0
            reply = drain(fd).decode("utf-8", "replace")
            state = reply.strip()
            if state and state != last_state:
                last_state = state
            time.sleep(PUSH_INTERVAL)
        except OSError as exc:
            print("串口断开（%s），2 秒后重连…" % exc)
            try:
                os.close(fd)
            except OSError:
                pass
            time.sleep(2)
            port = find_port()
            if not port:
                continue
            fd = open_port(port)
            print("已重连 %s" % port)
            last_time = 0.0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已停止")
        sys.exit(0)
