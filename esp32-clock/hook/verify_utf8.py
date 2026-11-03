#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_utf8.py —— 实机验证固件里的中文缓冲区有没有被截断/劈坏

做法：
    1. 往设备推一组「故意挑最长」的中文（天气文案 4 字、农历 12 字、城市、AQI）。
    2. 发 STATE 指令，把设备内部缓冲区原样读回来。
    3. 逐字节比对：设备存下来的必须和推过去的一模一样，
       且必须能被 UTF-8 正常解码（能被解码 = 没有劈开半个汉字）。

用法：python3 hook/verify_utf8.py
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
        chunk = os.read(fd, 4096)
        if chunk:
            out += chunk
            deadline = time.time() + 0.15
    return out


def send(fd, line):
    os.write(fd, (line + "\n").encode("utf-8"))


# ---------------------------------------------------------------- 测试数据
# 全部按固件缓冲区允许的最长值来挑，专门压边界。
TEST_WEATHER = {
    "temp": 23,
    "text": "晴间多云",      # 12 字节 —— 旧版 weatherText[8] 装不下
    "icon": "thunder",      #  7 字节
    "city": "北京·朝阳",     # 14 字节 —— 旧版 cityText[20] 装得下，但城市名可能更长
    "aqi":  "中度污染",       # 12 字节 —— 旧版 aqiText[16] 勉强够
}

WEEK_CN = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"]


def build_test_days():
    """按「今天起三天」现场生成测试数据。

    不写死日期 —— 写死的话，隔天跑测试，设备上缓存的 11-03 就和系统日期对不上，
    applyDayFor() 找不到今天那一档，回读出来是空值，测试会莫名失败。
    """
    now = time.localtime()
    out = []
    lunar_tail = ["廿四", "廿五", "廿六"]
    for i in range(3):
        # 用 datetime 做「加一天」的日历运算，别用 epoch + 86400 ——
        # 跨夏令时那天 86400 秒不是一天，日期会错位一天。
        d = datetime.date(now.tm_year, now.tm_mon, now.tm_mday) + datetime.timedelta(days=i)
        out.append((
            "%02d-%02d" % (d.month, d.day),       # date
            WEEK_CN[d.weekday()],                  # week
            "乙巳蛇年 · 十一月" + lunar_tail[i],   # lunar（最长 31 字节）
            [23, 19, 16][i],                       # temp
            ["晴间多云", "小雨转多云", "雷阵雨"][i],  # text
            ["thunder", "rain", "thunder"][i],      # icon
            ["中度污染", "轻度污染", "良"][i],        # aqi
        ))
    return out


def main():
    port = find_port()
    if not port:
        print("未找到 USB 串口设备")
        return 1
    print("串口: %s" % port)

    fd = open_port(port)
    time.sleep(0.3)
    drain(fd, 0.3)

    # 先把设备时钟同步到 Mac 的当前时间 —— 否则测试结果取决于设备上残留的
    # 旧时间（之前别的测试可能把时钟拨到别的日子去了），结果不可复现。
    send(fd, "TIME|%d" % int(time.time()))
    drain(fd)
    time.sleep(0.4)

    test_days = build_test_days()

    # 1) 推实时天气
    send(fd, "WEATHER|%d|%s|%s|%s|%s" % (
        TEST_WEATHER["temp"], TEST_WEATHER["text"], TEST_WEATHER["icon"],
        TEST_WEATHER["city"], TEST_WEATHER["aqi"]))
    print("WEATHER -> %s" % drain(fd).decode("utf-8", "replace").strip())

    # 2) 推三天
    send(fd, "DAYS|%d" % len(test_days))
    print("DAYS    -> %s" % drain(fd).decode("utf-8", "replace").strip())
    for d in test_days:
        send(fd, "DAY|%s|%s|%s|%d|%s|%s|%s" % d)
        print("DAY     -> %s" % drain(fd).decode("utf-8", "replace").strip())

    # 3) 读回设备内部状态
    send(fd, "STATE")
    time.sleep(0.5)
    raw = drain(fd, 0.8).decode("utf-8", "replace")
    os.close(fd)

    print("\n===== STATE 回读 =====")
    print(raw.strip())

    # ---- 逐条校验 ----
    print("\n===== 校验 =====")
    ok = True

    def check(label, sent, got):
        nonlocal ok
        s_bytes, g_bytes = sent.encode("utf-8"), got.encode("utf-8")
        same = s_bytes == g_bytes
        # 设备回读能整体解码 = 没有半个汉字被劈开
        try:
            g_bytes.decode("utf-8")
            decodable = True
        except UnicodeDecodeError:
            decodable = False
        good = same and decodable
        ok = ok and good
        print("%-28s %s  送 %2d字节 / 收 %2d字节  %s" % (
            label, "OK " if good else "FAIL", len(s_bytes), len(g_bytes),
            "" if same else "→ 内容不一致：%r vs %r" % (sent, got)))

    got_now = None
    got_days = []
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("STATE|now|"):
            got_now = line[len("STATE|now|"):].split("|")
        elif line.startswith("STATE|day|"):
            f = line[len("STATE|day|"):].split("|")
            got_days.append(f)

    if got_now:
        # now|lunarText|weatherText|weatherIcon|cityText|aqiText|tempVal|weekText|tempDate
        check("weatherText", TEST_WEATHER["text"], got_now[1])
        check("weatherIcon", TEST_WEATHER["icon"], got_now[2])
        check("cityText", TEST_WEATHER["city"], got_now[3])
        check("aqiText", TEST_WEATHER["aqi"], got_now[4])
        check("tempVal", str(TEST_WEATHER["temp"]), got_now[5])
    else:
        print("FAIL  没读到 STATE|now|")
        ok = False

    if len(got_days) == len(test_days):
        for i, (sent, g) in enumerate(zip(test_days, got_days)):
            # STATE|day|<i>|<date>|<week>|<lunar>|<temp>|<text>|<icon>|<aqi>
            # 去掉前缀 "STATE|day|" 之后，g[0] 是序号，所以字段从 g[1] 开始。
            check("day%d.date" % i, sent[0], g[1])
            check("day%d.week" % i, sent[1], g[2])
            check("day%d.lunar" % i, sent[2], g[3])
            check("day%d.temp" % i, str(sent[3]), g[4])
            check("day%d.text" % i, sent[4], g[5])
            check("day%d.icon" % i, sent[5], g[6])
            check("day%d.aqi" % i, sent[6], g[7])
    else:
        print("FAIL  天数不对：送 %d 天，收到 %d 天" % (len(test_days), len(got_days)))
        ok = False

    print("\n结果: %s" % ("全部通过 ✓" if ok else "有失败项 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
