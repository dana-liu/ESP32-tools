#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_roundtrip.py —— 端到端验证：桥接服务 → 推送器 → 设备

这是整条链路的唯一「真相测试」：
    bridge.py  取天气/农历 → serial_push.py 拼成串口行 → ESP32 固件解析存下来
任何一个环节把中文截断了、把字段串位了、把某一行整条丢了，这里都能查出来。

做法：
    1. 调 bridge 的 /api/weather，拿到权威数据（实时天气 + 三天预报）。
    2. 跑 serial_push.py --once，把这些数据真推到设备上。
    3. 发 STATE，把设备内部缓冲区读回来。
    4. 逐字段比对：实时天气 5 个字段 + 三天 × 6 个字段 = 23 项，全部要一致。

用法：python3 hook/verify_roundtrip.py
"""

import glob
import json
import os
import subprocess
import sys
import termios
import time
import urllib.request

BRIDGE = os.environ.get("CLOCK_BRIDGE", "http://127.0.0.1:8787").rstrip("/")
HERE = os.path.dirname(os.path.abspath(__file__))

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


def main():
    # ---- 1. 拿桥接的权威数据 ----
    try:
        with urllib.request.urlopen(BRIDGE + "/api/weather", timeout=8) as resp:
            api = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        print("取桥接数据失败：%s" % exc)
        print("先起桥接服务：cd ../esp32-dingtalk-bridge && python3 bridge.py")
        return 1

    fc = api.get("forecast") or []
    wx = api.get("weather") or {}
    print("桥接: %s" % BRIDGE)
    print("  实时  %s %s℃ %s (%s / %s)" % (
        wx.get("city"), wx.get("temp"), wx.get("text"),
        wx.get("icon"), wx.get("aqi_text")))
    for i, d in enumerate(fc):
        print("  +%d   %s %s %s℃ %s (%s)" % (
            i, d.get("date"), d.get("week"), d.get("temp"),
            d.get("text"), d.get("icon")))

    if not fc or not wx:
        print("桥接返回的数据不完整")
        return 1

    # ---- 2. 真推一次 ----
    port = find_port()
    if not port:
        print("未找到 USB 串口设备")
        return 1
    print("\n串口: %s" % port)

    proc = subprocess.run(
        [sys.executable, os.path.join(HERE, "serial_push.py"), "--once"],
        capture_output=True, text=True, timeout=90)
    print("推送器输出:\n%s" % (proc.stdout.strip() or "(无)"))
    if proc.stderr.strip():
        print("推送器 stderr:\n%s" % proc.stderr.strip())

    # ---- 3. 读回设备状态 ----
    fd = open_port(port)
    time.sleep(0.3)
    drain(fd, 0.3)
    send(fd, "STATE")
    time.sleep(0.6)
    raw = drain(fd, 1.0).decode("utf-8", "replace")
    os.close(fd)

    got_now, got_days, got_count = None, [], 0
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("STATE|days|"):
            got_count = int(line.split("|")[2])
        elif line.startswith("STATE|now|"):
            got_now = line[len("STATE|now|"):].split("|")
        elif line.startswith("STATE|day|"):
            got_days.append(line[len("STATE|day|"):].split("|"))

    # ---- 4. 逐字段比对 ----
    ok = True
    checks = []

    def check(label, want, got):
        nonlocal ok
        good = (want == got)
        ok = ok and good
        checks.append((label, good, want, got))

    if not got_now:
        print("\n✗ 没读到 STATE|now|，设备可能没收到数据")
        print(raw)
        return 1

    # now|lunar|weather|icon|city|aqi|temp|week|tempDate
    check("实时.天气文案", wx.get("text"), got_now[1])
    check("实时.图标", wx.get("icon"), got_now[2])
    check("实时.城市", wx.get("city"), got_now[3])
    check("实时.空气质量", wx.get("aqi_text"), got_now[4])
    check("实时.温度", str(wx.get("temp")), got_now[5])

    check("三天.条数", len(fc), got_count)

    for i, d in enumerate(fc):
        if i >= len(got_days):
            check("第%d天.存在" % i, True, False)
            continue
        g = got_days[i]
        # day|<i>|<date>|<week>|<lunar>|<temp>|<text>|<icon>|<aqi>
        check("第%d天.日期" % i, d.get("date"), g[1])
        check("第%d天.周几" % i, d.get("week"), g[2])
        check("第%d天.农历" % i, d.get("lunar"), g[3])
        check("第%d天.温度" % i, str(d.get("temp")), g[4])
        check("第%d天.天气文案" % i, d.get("text"), g[5])
        check("第%d天.图标" % i, d.get("icon"), g[6])
        check("第%d天.空气质量" % i, api.get("weather", {}).get("aqi_text"), g[7])

    print("\n===== 逐字段比对 =====")
    for label, good, want, got in checks:
        print("  %-16s %s  期望 %-18r 实得 %r" % (
            label, "OK " if good else "FAIL", want, got))

    passed = sum(1 for _, g, _, _ in checks if g)
    print("\n结果: %d/%d 通过 — %s" % (
        passed, len(checks), "全部一致 ✓" if ok else "有不一致 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
