#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
key-scan.py —— 找出板载三个按键分别接在哪个 GPIO

用法：
    python3 hook/key-scan.py            # 扫 45 秒
    python3 hook/key-scan.py 60         # 扫 60 秒

会通过串口给设备发 KEYSCAN，然后实时打印引脚电平变化。
扫描期间屏幕不刷新（固件在忙等），属于一次性排查工具。

注意：串口独占。跑之前要先停掉 serial_push.py，否则打不开端口。
"""

import glob
import os
import select
import sys
import termios
import time

PORT_PATTERNS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*")


def find_port():
    for pat in PORT_PATTERNS:
        found = sorted(glob.glob(pat))
        if found:
            return found[0]
    return None


def open_port(path):
    """不要碰 DTR/RTS —— S3 的 USB-Serial/JTAG 会当成复位"""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0
    attr[1] = 0
    attr[2] = (attr[2] & ~(termios.PARENB | termios.CSTOPB
                           | termios.CSIZE | termios.HUPCL)) \
        | termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0
    attr[4] = termios.B115200
    attr[5] = termios.B115200
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    return fd


def main():
    duration = 45
    if len(sys.argv) > 1:
        try:
            duration = int(sys.argv[1])
        except ValueError:
            pass

    port = find_port()
    if not port:
        print("未找到 USB 串口（/dev/cu.usbmodem*）")
        return 1

    try:
        fd = open_port(port)
    except OSError as exc:
        print("打不开 %s：%s" % (port, exc))
        print("可能 serial_push.py 还占着串口，先 pkill -f serial_push.py")
        return 1

    print("端口 %s，扫描 %d 秒" % (port, duration))
    print("现在请依次按一下三个按键，每按一下松手，间隔 1 秒以上。\n")

    os.write(fd, ("KEYSCAN %d\n" % duration).encode())

    buf = b""
    t0 = time.time()
    counts = {}
    order = []
    while time.time() - t0 < duration + 6:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 512)
        except OSError:
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            ts = time.time() - t0
            if text.startswith("[scan]"):
                print("%6.1fs  %s" % (ts, text))
                if "GPIO" in text and "->" in text:
                    pin = text.split("GPIO")[1].split()[0]
                    state = "按下" if "LOW" in text else "松开"
                    if state == "按下":
                        counts[pin] = counts.get(pin, 0) + 1
                        if pin not in order:
                            order.append(pin)
            elif text.startswith("OK"):
                print("      设备已进入扫描：%s" % text)
            else:
                print("       %s" % text)

    print("\n—— 汇总 ——")
    if not counts:
        print("没有检测到任何按下。检查：串口是否被占用 / 按键是否需要更大力度 / 引脚是否接在别的 GPIO")
    else:
        for i, pin in enumerate(order, 1):
            print("第 %d 个按下的键 → GPIO%s（共 %d 次）" % (i, pin, counts[pin]))
        rest = [p for p in ("0", "4", "5") if p not in counts]
        if rest:
            print("没响应的引脚：%s" % ", ".join("GPIO" + p for p in rest))

    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
