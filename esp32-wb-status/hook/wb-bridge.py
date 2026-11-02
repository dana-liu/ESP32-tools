#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wb-bridge.py —— USB 串口 ↔ 本地 HTTP 桥

把 ESP32 的 USB 串口包装成 http://127.0.0.1:18765 上的 HTTP 服务，
接口和固件原来的 HTTP 一模一样，所以 hook 换个 base URL 就能用：

    GET /ping
    GET /state?sid=&st=&tool=&label=
    GET /approve?req=&tool=&label=&cmd=
    GET /decision?req=
    GET /list

好处：完全不依赖 WiFi —— 公司网络的 5GHz-only、portal 认证、客户端隔离
统统不影响。插上线就能用，拔了线 hook 会自动退回 WiFi。

依赖：只用 Python 标准库（termios / select），不需要 pip install pyserial。

用法：
    python3 wb-bridge.py            # 前台跑，Ctrl-C 退出
    python3 wb-bridge.py --check    # 只探测设备在不在，打印结果后退出
"""

import fcntl
import glob
import os
import select
import struct
import sys
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

PORT = int(os.environ.get("WB_BRIDGE_PORT", "18765"))
HOST = "127.0.0.1"
LOCK = threading.Lock()

TIOCMBIS  = 0x8004746C          # macOS: 置位调制解调器线
TIOCM_DTR = 0x0002


# ---------------- 串口 ----------------
def find_ports():
    """按优先级列出可能是 ESP32 的串口"""
    out = []
    for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"):
        out.extend(sorted(glob.glob(pat)))
    return out


def open_port(path):
    """打开串口并设成 raw 模式。

    注意：这里刻意不碰 DTR/RTS，也不开 HUPCL —— 否则关端口时会拉低 DTR，
    某些板子会因此复位。
    """
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        a = termios.tcgetattr(fd)
        a[0] = 0                                              # iflag：关软件流控/换行转换
        a[1] = 0                                              # oflag：关输出加工
        a[2] &= ~(termios.PARENB | termios.CSTOPB |
                  termios.CSIZE | termios.HUPCL)
        a[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL   # 8N1，忽略调制解调器线
        a[3] = 0                                              # lflag：关回显和行缓冲
        a[4] = termios.B115200                                # 波特率对 CDC 无意义，填上无妨
        a[5] = termios.B115200
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)

        # 原生 USB（usbmodem）要拉高 DTR 才会真正开始收发。
        # 只动 DTR 不动 RTS —— CH343 那种 UART 桥靠 DTR+RTS 组合复位，
        # 单独 DTR 是安全的，两个一起拉低会把板子打进下载模式。
        if "usbmodem" in path:
            try:
                fcntl.ioctl(fd, TIOCMBIS, struct.pack("i", TIOCM_DTR))
            except Exception:
                pass
    except Exception:
        os.close(fd)
        raise
    return fd


class Device:
    def __init__(self):
        self.fd = None
        self.port = None

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except Exception:
                pass
        self.fd = None
        self.port = None

    def drain(self):
        """丢掉缓冲里积压的调试日志，避免它们干扰应答匹配"""
        try:
            while select.select([self.fd], [], [], 0)[0]:
                if not os.read(self.fd, 4096):
                    break
        except Exception:
            pass

    def _raw(self, line, prefix, timeout):
        if self.fd is None:
            return None
        self.drain()
        try:
            os.write(self.fd, (line + "\n").encode())
        except Exception:
            self.close()
            return None

        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                r, _, _ = select.select([self.fd], [], [], 0.05)
            except Exception:
                self.close()
                return None
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except Exception:
                self.close()
                return None
            if not chunk:
                self.close()
                return None

            buf += chunk
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                s = ln.decode("utf-8", "replace").strip()
                # 只认 '@' 开头的协议行，其余是固件的中文调试日志
                if s.startswith("@") and s[1:].startswith(prefix):
                    return s[1:]
        return None

    def _open_first(self):
        for path in find_ports():
            try:
                self.fd = open_port(path)
            except Exception:
                continue
            self.port = path
            time.sleep(0.25)                 # 拉高 DTR 后设备要缓一下才认命令
            for _ in range(2):
                if self._raw("PING", "PONG", 1.0):
                    return True
            self.close()
        return False

    def ensure(self):
        return self.fd is not None or self._open_first()

    def request(self, line, prefix, timeout=1.5):
        with LOCK:
            if not self.ensure():
                return None
            return self._raw(line, prefix, timeout)


# ---------------- HTTP ----------------
LAST_TIME_SYNC = 0.0
TIME_SYNC_SEC = 600          # 每 10 分钟给设备对一次时


class Handler(BaseHTTPRequestHandler):
    device = None

    def log_message(self, *args):
        pass

    def _sync_time(self):
        """USB 模式下设备没有 NTP，靠这里把 Mac 的时间推过去，夜间定时才准"""
        global LAST_TIME_SYNC
        now = time.time()
        if now - LAST_TIME_SYNC < TIME_SYNC_SEC:
            return
        if self.device.request("TIME|%d" % int(now), "OK", timeout=1.5):
            LAST_TIME_SYNC = now

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        try:
            self.wfile.write(b)
        except Exception:
            pass

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query, keep_blank_values=True)

        def g(key, default=""):
            v = (q.get(key) or [default])[0]
            # '|' 是协议分隔符，值里不能出现
            return v.replace("|", " ").replace("\r", " ").replace("\n", " ")

        try:
            self._sync_time()

            if u.path in ("/", "/list"):
                r = self.device.request("LIST", "LIST")
                if r is None:
                    return self._send(503, "[]", "application/json")
                return self._send(200, r[5:], "application/json")

            if u.path == "/ping":
                r = self.device.request("PING", "PONG", timeout=1.0)
                return self._send(200, "pong") if r else self._send(503, "down")

            if u.path == "/state":
                st = g("st")
                if not st:
                    return self._send(400, "st required")
                line = "STATE|%s|%s|%s|%s" % (g("sid", "default"), st, g("tool"), g("label"))
                r = self.device.request(line, "OK")
                return self._send(200, "ok") if r else self._send(503, "down")

            if u.path == "/approve":
                req = g("req")
                if not req:
                    return self._send(400, "req required")
                line = "APPROVE|%s|%s|%s|%s" % (req, g("tool"), g("label"), g("cmd"))
                r = self.device.request(line, "OK")
                return self._send(200, "ok") if r else self._send(503, "down")

            if u.path == "/decision":
                r = self.device.request("DEC|" + g("req"), "DEC", timeout=1.0)
                if r is None:
                    return self._send(503, "none")
                parts = r.split("|")
                return self._send(200, parts[2] if len(parts) >= 3 else "none")

            if u.path == "/ctest":                 # 屏幕色彩自检
                r = self.device.request("CTEST", "OK", timeout=3.0)
                return self._send(200, "ok") if r else self._send(503, "down")

            if u.path == "/night":                 # 强制夜间亮度：/night?on=1
                on = "1" if g("on", "1") in ("1", "on", "true") else "0"
                r = self.device.request("NIGHT|" + on, "OK", timeout=2.0)
                return self._send(200, "ok") if r else self._send(503, "down")

            return self._send(404, "not found")
        except Exception as e:                       # 任何异常都别让线程崩掉
            return self._send(500, "bridge error: %s" % e)


def main():
    dev = Device()
    Handler.device = dev

    if "--check" in sys.argv:
        if dev.ensure():
            print("OK %s" % dev.port)
            return 0
        print("未找到设备")
        return 1

    if not dev.ensure():
        print("[bridge] 没找到 ESP32 —— 检查数据线是否插好、串口监视器是否占用端口",
              file=sys.stderr)
        return 1

    print("[bridge] 已连接 %s" % dev.port)
    print("[bridge] 监听 http://%s:%d" % (HOST, PORT))

    try:
        srv = ThreadingHTTPServer((HOST, PORT), Handler)
    except OSError as e:
        print("[bridge] 端口 %d 已被占用（可能已经有一个 bridge 在跑）：%s" % (PORT, e),
              file=sys.stderr)
        return 0

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
