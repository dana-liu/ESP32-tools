#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_backlight.py —— 实机验证「夜间调光 + 熄屏」里能自动化的部分

触摸唤醒、长按熄屏要用手按，没法脚本模拟；这个脚本覆盖串口能验证的：
    1. 白天(本地当前时段)自动亮度 = 全亮
    2. BL|<0-100> 固定亮度生效，BL|auto 恢复自动
    3. 把设备时钟拨到 21:00 —— 夜间规则生效，亮度自动降到 BL_NIGHT
    4. 拨回现在 —— 恢复全亮（验证昼夜判断跟着 RTC 走）

用法：python3 hook/verify_backlight.py
"""

import datetime
import glob
import os
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


def drain(fd, wait=0.4):
    out = b""
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if chunk:
            out += chunk
            deadline = time.time() + 0.15
    return out


def send(fd, line):
    os.write(fd, (line + "\n").encode("utf-8"))


def read_bl(fd):
    """发 STATE，解析 STATE|bl|<级别>|<HH:MM>|<夜间>|<熄屏>。"""
    send(fd, "STATE")
    time.sleep(0.5)
    raw = drain(fd, 0.8).decode("utf-8", "replace")
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("STATE|bl|"):
            f = line[len("STATE|bl|"):].split("|")
            # f = [级别, HH:MM, 夜间, 熄屏]
            hhmm = f[1].split(":")
            return {"level": int(f[0]),
                    "hh": int(hhmm[0]), "mm": int(hhmm[1]),
                    "night": f[2] == "1", "off": f[3] == "1"}
    return None


def main():
    port = find_port()
    if not port:
        print("未找到 USB 串口设备")
        return 1
    print("串口: %s" % port)

    fd = open_port(port)
    time.sleep(0.3)
    drain(fd, 0.3)

    ok = True

    def expect(label, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print("  %-30s %s  期望 %-8r 实得 %r" % (
            label, "OK " if good else "FAIL", want, got))

    # ---- 1. 同步时钟，看当前时段的自动亮度 ----
    send(fd, "TIME|%d" % int(time.time()))
    drain(fd)
    send(fd, "BL|auto")      # 先清掉上次测试可能留下的 BL| 强制值，保证测的是自动规则
    drain(fd)
    time.sleep(0.5)
    st = read_bl(fd)
    if not st:
        print("✗ 读不到 STATE|bl|（固件太旧？）")
        return 1
    print("[1] 当前时段 %02d:%02d  夜间=%s" % (st["hh"], st["mm"], st["night"]))
    # 夜间判断和设备自报的时间必须一致：20:00-09:00 为夜间
    want_night = st["hh"] >= 20 or st["hh"] < 9
    expect("夜间判断与时间一致", st["night"], want_night)
    expect("自动亮度(白天=255/夜间=30)", st["level"], 30 if st["night"] else 255)

    # ---- 2. 固定亮度 ----
    send(fd, "BL|12")
    drain(fd)
    time.sleep(0.3)
    st = read_bl(fd)
    expect("BL|12 -> 级别 30", st["level"], 30)

    send(fd, "BL|0")
    drain(fd)
    time.sleep(0.3)
    st = read_bl(fd)
    expect("BL|0 -> 熄屏亮度", st["level"], 0)

    send(fd, "BL|auto")
    drain(fd)
    time.sleep(0.3)
    st = read_bl(fd)
    expect("BL|auto -> 恢复自动", st["level"], 30 if st["night"] else 255)
    expect("熄屏标志仍为 0", st["off"], False)

    # ---- 3. 拨到晚上 21:00，验证夜间规则 ----
    now = datetime.datetime.now()
    night = now.replace(hour=21, minute=0, second=0, microsecond=0)
    send(fd, "TIME|%d" % int(night.timestamp()))
    drain(fd)
    time.sleep(0.8)          # 等 loop 下一轮 applyBrightness
    st = read_bl(fd)
    print("[3] 设备时间拨到 21:00")
    expect("进入夜间", st["night"], True)
    expect("亮度降为 30", st["level"], 30)

    # ---- 4. 拨回现在 ----
    send(fd, "TIME|%d" % int(time.time()))
    drain(fd)
    time.sleep(0.8)
    st = read_bl(fd)
    print("[4] 设备时间还原")
    expect("退出夜间", st["night"], False)
    expect("亮度恢复 255", st["level"], 255)

    os.close(fd)
    print("\n结果: %s" % ("全部通过 ✓" if ok else "有失败项 ✗"))
    print("（触摸点亮 / 长按静音键熄屏需要手动验证）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
