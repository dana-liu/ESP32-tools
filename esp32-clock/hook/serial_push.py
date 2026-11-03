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
    EVENTS|<n>                 后面跟 n 条日程
    EV|<MM.DD>|<HH:MM>|<分钟>|<allDay>|<标题>
    WEATHER|<温度>|<文案>|<图标>|<城市>|<空气质量>
    DAYS|<n>                   后面跟 n 天（设备离线时按日期自动切换）
    DAY|<MM-DD>|<周几>|<农历>|<温度>|<文案>|<图标>|<空气质量>
    LUNAR|<农历文本>
    NEXT|<HH:MM>|<分钟>|<标题>  单条（兼容旧固件）
    NONE                       今天没有日程了

    VOL|<0-100>                设音量
    SND|<0/1>                  提醒声音开关
    BEEP                       立刻响一次

用法：
    python3 serial_push.py              # 前台跑，Ctrl-C 停
    python3 serial_push.py --check      # 只探测设备在不在
    python3 serial_push.py --once       # 推一次就退出（调试用）
    python3 serial_push.py --vol 60     # 设音量
    python3 serial_push.py --beep       # 让设备响一次

环境变量：
    CLOCK_BRIDGE         桥接地址，默认 http://127.0.0.1:8787
    CLOCK_PUSH_INTERVAL  日程轮询间隔秒，默认 5
    CLOCK_TIME_INTERVAL  对时间隔秒，默认 60
"""

import datetime
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
WEATHER_INTERVAL = float(os.environ.get("CLOCK_WEATHER_INTERVAL", "300"))

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


def wait_reply(fd, timeout=0.4):
    """等设备回一行确认（OK / PONG / ERR）。

    设备偶尔会插 [wifi] / [scan] 之类的日志，这些不是确认行，跳过继续等。
    超时返回 None —— 不代表设备没收到，只代表没等到回执。
    """
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 512)
        except OSError:
            return None
        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text:
                    continue
                if text.startswith(("OK", "PONG", "ERR")):
                    return text
            continue
        time.sleep(0.005)
    return None


def send_batch(fd, lines, gap=0.012):
    """逐行发，每行之间留一点间隔。

    设备的 USB CDC 收缓冲只有 1KB，一次性灌几百字节会在主循环忙于刷屏时
    被冲爆，表现为「某几行没进去」（而且不报错，很难发现）。
    留点间隔让设备主循环有机会把串口读空。
    """
    for line in lines:
        send(fd, line)
        wait_reply(fd, timeout=0.4)
        if gap:
            time.sleep(gap)


def read_days(fd):
    """读设备里存着的三天条数。读不到（固件不支持 STATE）返回 None。"""
    send(fd, "STATE")
    time.sleep(0.3)
    raw = drain(fd, 0.6).decode("utf-8", "replace")
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("STATE|days|"):
            try:
                return int(line.split("|")[2])
            except (IndexError, ValueError):
                return None
    return None


def push_days(fd, lines, expected, retries=2):
    """推三天数据并核对设备真的收到了。

    设备回读条数和推过去的条数不一致，说明这一批丢东西了，重推一次。
    """
    for attempt in range(retries + 1):
        send_batch(fd, lines)
        got = read_days(fd)
        if got is None:
            return True                  # 固件不支持 STATE，没法核对，算了
        if got == expected:
            return True
        if attempt < retries:
            print("三天数据只到了 %d/%d 条，重推（第 %d 次）" % (got, expected, attempt + 1))
    print("⚠️ 三天数据只到了 %d/%d 条，设备可能没收全" % (got, expected))
    return False


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


def fetch_events():
    """返回桥接的全部日程（今天的、明天的都在里面）"""
    try:
        raw = urllib.request.urlopen(BRIDGE + "/api/events", timeout=3).read()
        data = json.loads(raw)
    except Exception:
        return None
    return data.get("events") or []


def fit_bytes(text, limit=42):
    """按字节截断，但不切断 UTF-8 字符"""
    data = text.encode("utf-8")[:limit]
    return data.decode("utf-8", "ignore")


def build_event_lines(events):
    """把桥接的 JSON 事件转成 EV 行"""
    now = datetime.datetime.now().astimezone()
    lines = []
    for e in events:
        start = e.get("start") or ""
        if len(start) < 10:
            continue
        date = "%s.%s" % (start[5:7], start[8:10])
        all_day = bool(e.get("all_day"))
        title = (e.get("title") or "(无标题)").replace("|", "／").replace("\n", " ")
        title = fit_bytes(title)
        if all_day:
            hhmm, minutes = "全天", 99999
        else:
            hhmm = start[11:16] if len(start) >= 16 else "--:--"
            try:
                st = datetime.datetime.fromisoformat(start)
                minutes = int((st - now).total_seconds() // 60)
            except Exception:
                minutes = 0
        lines.append("EV|%s|%s|%d|%d|%s" % (date, hhmm, minutes, 1 if all_day else 0, title))
    return lines


def clean(text, limit=40):
    """按字节截断但不切断 UTF-8 字符（decode(errors='ignore') 会丢掉残缺尾巴）"""
    text = str(text or "").replace("|", "／").replace("\n", " ").strip()
    data = text.encode("utf-8")[:limit]
    return data.decode("utf-8", "ignore")


# 固件里对应的缓冲区容量（见 esp32-clock.ino）。
# 这里留 1 字节给结束符，两边保持一致，避免 Mac 先截断导致设备上显示不全。
CAP_TEXT = 23      # char weatherText[24] / DayInfo.text[24]
CAP_ICON = 15      # char weatherIcon[16] / DayInfo.icon[16]
CAP_CITY = 31      # char cityText[32]
CAP_AQI = 23       # char aqiText[24] / DayInfo.aqi[24]
CAP_LUNAR = 47     # char lunarText[48] / DayInfo.lunar[48]
CAP_WEEK = 15      # char DayInfo.week[16]


def build_weather_lines():
    """实时天气 + 未来 3 天（日期/周几/农历/天气）。桥接没起来返回 None"""
    try:
        raw = urllib.request.urlopen(BRIDGE + "/api/weather", timeout=3).read()
        data = json.loads(raw)
    except Exception:
        return None

    w = data.get("weather") or {}
    aqi = clean(w.get("aqi_text"), CAP_AQI)
    out = []
    if w:
        out.append("WEATHER|%d|%s|%s|%s|%s" % (
            int(w.get("temp", -999)),
            clean(w.get("text"), CAP_TEXT), clean(w.get("icon"), CAP_ICON),
            clean(w.get("city"), CAP_CITY), aqi))

    forecast = data.get("forecast") or []
    if forecast:
        out.append("DAYS|%d" % len(forecast))
        for f in forecast:
            out.append("DAY|%s|%s|%s|%d|%s|%s|%s" % (
                f.get("date", ""), clean(f.get("week"), CAP_WEEK),
                clean(f.get("lunar"), CAP_LUNAR), int(f.get("temp", -999)),
                clean(f.get("text"), CAP_TEXT), clean(f.get("icon"), CAP_ICON), aqi))
    else:
        lunar = data.get("lunar") or ""
        if lunar:
            out.append("LUNAR|%s" % clean(lunar, CAP_LUNAR))
    return out


def split_days(lines):
    """把天气行拆成 (前面几行, DAYS/DAY 那一组, 后面几行)。

    三天那一组要单独处理 —— 推完要核对设备到底收到了几条。
    """
    if "DAYS|" not in lines:
        return lines, [], []
    i = lines.index("DAYS|")
    head = lines[:i]
    tail = lines[i + 1:]
    block = [lines[i]]
    for line in tail:
        if line.startswith("DAY|"):
            block.append(line)
        else:
            break
    rest = tail[len(block) - 1:]
    return head, block, rest


def push_weather(fd, lines):
    """推天气 + 三天。三天那组推完会核对条数，丢了自己重推。"""
    head, block, rest = split_days(lines)
    send_batch(fd, head)
    if block:
        expected = int(block[0].split("|")[1]) if "|" in block[0] else 0
        push_days(fd, block, expected)
    send_batch(fd, rest)
    return True


def push_once(fd, with_time=False):
    if with_time:
        send(fd, "TIME|%d" % int(time.time()))
    events = fetch_events()
    if events is None:
        return False                # 桥接没起来，这次跳过
    lines = build_event_lines(events)
    send_batch(fd, ["EVENTS|%d" % len(lines)] + lines)
    return True


def one_shot(port, line):
    fd = open_port(port)
    send(fd, line)
    time.sleep(0.4)
    print(drain(fd).decode("utf-8", "replace").strip() or "(无回复)")
    os.close(fd)
    return 0


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

    if "--vol" in sys.argv:
        vol = max(0, min(100, int(sys.argv[sys.argv.index("--vol") + 1])))
        return one_shot(port, "VOL|%d" % vol)
    if "--snd" in sys.argv:
        on = int(sys.argv[sys.argv.index("--snd") + 1]) != 0
        return one_shot(port, "SND|%d" % (1 if on else 0))
    if "--beep" in sys.argv:
        return one_shot(port, "BEEP")

    fd = open_port(port)
    print("已连接 %s" % port)
    print("桥接     %s" % BRIDGE)
    print("推送间隔 %.0fs / 对时 %.0fs" % (PUSH_INTERVAL, TIME_INTERVAL))

    last_time = 0.0
    last_weather = 0.0
    last_state = None
    last_sig = None
    last_wsig = None
    while True:
        try:
            now = time.time()
            need_time = (now - last_time) >= TIME_INTERVAL
            events = fetch_events()
            sig = json.dumps(events, ensure_ascii=False, sort_keys=True) if events is not None else None
            if events is not None and (sig != last_sig or need_time):
                if need_time:
                    send(fd, "TIME|%d" % int(time.time()))
                lines = build_event_lines(events)
                send_batch(fd, ["EVENTS|%d" % len(lines)] + lines)
                last_time = now
                last_sig = sig
            elif need_time:
                send(fd, "TIME|%d" % int(time.time()))
                last_time = now

            # 天气/农历：变了就推，另外每 5 分钟补一次
            wlines = build_weather_lines()
            if wlines:
                wsig = "|".join(wlines)
                if wsig != last_wsig or (now - last_weather) >= WEATHER_INTERVAL:
                    push_weather(fd, wlines)
                    last_wsig = wsig
                    last_weather = now
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
