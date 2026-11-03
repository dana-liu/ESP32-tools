#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_rollover.py —— 实机验证「过 0 点自动切换到下一天」

做法：
    1. 推三天数据（今天/明天/后天）。
    2. 读 STATE，确认当前显示的是「今天」那份。
    3. 把设备时钟拨到明天（发 TIME|<明天的 epoch>），再读 STATE，
       确认农历/天气/温度/周几都换成了「明天」那份。
    4. 再拨回今天，把设备时钟还原。

用法：python3 hook/verify_rollover.py
"""

import datetime
import glob
import os
import sys
import termios
import time

PORT_PATTERNS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*")

# 三天的数据：字段差异要足够大，才能一眼看出切没切
DAYS = [
    ("TODAY",    "周一", "丙午马年 · 九月廿五", 23, "晴间多云", "thunder", "中度污染"),
    ("TOMORROW", "周二", "丙午马年 · 九月廿六", 19, "小雨转多云", "rain", "轻度污染"),
    ("DAYAFTER", "周三", "丙午马年 · 九月廿七", 16, "雷阵雨", "thunder", "良"),
]


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
        chunk = os.read(fd, 4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.15
    return out


def send(fd, line):
    os.write(fd, (line + "\n").encode("utf-8"))


def read_state(fd):
    send(fd, "STATE")
    time.sleep(0.5)
    raw = drain(fd, 0.8).decode("utf-8", "replace")
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("STATE|now|"):
            f = line[len("STATE|now|"):].split("|")
            # now|lunar|weather|icon|city|aqi|temp|week|tempDate
            return {"lunar": f[0], "weather": f[1], "icon": f[2],
                    "city": f[3], "aqi": f[4], "temp": f[5], "week": f[6]}
    return None


def main():
    port = find_port()
    if not port:
        print("未找到 USB 串口设备")
        return 1

    today = datetime.date.today()
    dates = [(today + datetime.timedelta(days=i)).strftime("%m-%d") for i in range(3)]

    fd = open_port(port)
    time.sleep(0.3)
    drain(fd, 0.3)

    # 先把设备时钟同步到 Mac 的当前时间 —— 否则测试结果取决于设备上残留的
    # 旧时间（之前别的测试可能把时钟拨到别的日子去了），结果不可复现。
    send(fd, "TIME|%d" % int(time.time()))
    drain(fd)
    time.sleep(0.4)

    # 实时天气（属于今天）
    send(fd, "WEATHER|23|晴间多云|thunder|北京·朝阳|中度污染")
    drain(fd)

    # 三天
    send(fd, "DAYS|3")
    drain(fd)
    for i, d in enumerate(dates):
        _, week, lunar, temp, text, icon, aqi = DAYS[i]
        send(fd, "DAY|%s|%s|%s|%d|%s|%s|%s" % (d, week, lunar, temp, text, icon, aqi))
        drain(fd)

    ok = True

    def expect(label, st, field, want):
        nonlocal ok
        got = st[field] if st else "(读不到)"
        good = got == want
        ok = ok and good
        print("  %-26s %s  期望 %-14r 实得 %r" % (
            label, "OK " if good else "FAIL", want, got))

    print("日期映射: 今天=%s 明天=%s 后天=%s" % tuple(dates))

    # ---- 今天 ----
    st = read_state(fd)
    print("\n[1] 设备时间=今天")
    expect("农历", st, "lunar", DAYS[0][2])
    expect("周几", st, "week", DAYS[0][1])
    expect("天气文案", st, "weather", DAYS[0][4])
    expect("温度", st, "temp", str(DAYS[0][3]))

    # ---- 明天 ----
    tomorrow_epoch = int(datetime.datetime.combine(
        today + datetime.timedelta(days=1),
        datetime.time(10, 0)).timestamp())
    send(fd, "TIME|%d" % tomorrow_epoch)
    drain(fd)
    time.sleep(0.6)          # 等主循环跑到 render()，触发换日
    st = read_state(fd)
    print("\n[2] 设备时间=明天（模拟过 0 点）")
    expect("农历", st, "lunar", DAYS[1][2])
    expect("周几", st, "week", DAYS[1][1])
    expect("天气文案", st, "weather", DAYS[1][4])
    expect("温度", st, "temp", str(DAYS[1][3]))

    # ---- 后天 ----
    dayafter_epoch = int(datetime.datetime.combine(
        today + datetime.timedelta(days=2),
        datetime.time(10, 0)).timestamp())
    send(fd, "TIME|%d" % dayafter_epoch)
    drain(fd)
    time.sleep(0.6)
    st = read_state(fd)
    print("\n[3] 设备时间=后天")
    expect("农历", st, "lunar", DAYS[2][2])
    expect("周几", st, "week", DAYS[2][1])
    expect("天气文案", st, "weather", DAYS[2][4])
    expect("温度", st, "temp", str(DAYS[2][3]))

    # ---- 还原 ----
    now_epoch = int(time.time())
    send(fd, "TIME|%d" % now_epoch)
    drain(fd)
    time.sleep(0.6)
    st = read_state(fd)
    print("\n[4] 设备时间还原为现在")
    expect("农历", st, "lunar", DAYS[0][2])
    expect("周几", st, "week", DAYS[0][1])

    os.close(fd)
    print("\n结果: %s" % ("全部通过 ✓" if ok else "有失败项 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
