#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wb_common.py —— 两个 hook 共用的「找设备」逻辑

通道优先级：WB_STATUS_URL > USB 串口桥 > WiFi。
USB 桥没在跑时顺手拉起来（插着数据线的情况下），拉起来之后会常驻。
"""

import fcntl
import glob
import os
import subprocess
import sys
import time
import urllib.request

STATE_DIR = os.path.expanduser("~/.wb-status")
DOWN_FLAG = os.path.join(STATE_DIR, "device-down")
SESS_FILE = os.path.join(STATE_DIR, "sessions.json")
BRIDGE_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wb-bridge.py")

DEVICE_ENV = os.environ.get("WB_STATUS_URL", "").strip()
BRIDGE_URL = "http://127.0.0.1:%s" % os.environ.get("WB_BRIDGE_PORT", "18765")
WIFI_URL = os.environ.get("WB_WIFI_URL", "http://wbstatus.local")


def has_serial_port():
    return bool(glob.glob("/dev/cu.usbmodem*")
                or glob.glob("/dev/cu.usbserial*")
                or glob.glob("/dev/cu.wchusbserial*"))


def bridge_alive(timeout=0.4):
    try:
        urllib.request.urlopen(BRIDGE_URL + "/ping", timeout=timeout).read()
        return True
    except Exception:
        return False


def ensure_bridge():
    if bridge_alive():
        return True
    if not has_serial_port() or not os.path.exists(BRIDGE_SCRIPT):
        return False

    lock_file = None
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        lock_file = open(os.path.join(STATE_DIR, "bridge.lock"), "w")
        fcntl.flock(lock_file, fcntl.LOCK_EX)
    except Exception:
        lock_file = None

    try:
        if bridge_alive():
            return True
        subprocess.Popen(
            [sys.executable, BRIDGE_SCRIPT],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        for _ in range(15):
            time.sleep(0.1)
            if bridge_alive():
                return True
    except Exception:
        pass
    finally:
        if lock_file:
            try:
                lock_file.close()
            except Exception:
                pass
    return False


def candidates():
    out = []
    if DEVICE_ENV:
        out.append(DEVICE_ENV.rstrip("/"))
    if ensure_bridge():
        out.append(BRIDGE_URL)
    if WIFI_URL:
        out.append(WIFI_URL.rstrip("/"))
    return out


def post(path_and_query, timeout=1.0):
    """依次尝试各通道，返回第一个成功的 base URL；全失败返回 None"""
    for base in candidates():
        try:
            urllib.request.urlopen(base + path_and_query, timeout=timeout).read()
            return base
        except Exception:
            continue
    return None


def mark_down():
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        open(DOWN_FLAG, "w").close()
    except Exception:
        pass


def mark_up():
    try:
        os.remove(DOWN_FLAG)
    except Exception:
        pass


def recently_down(ttl=60):
    try:
        return (time.time() - os.path.getmtime(DOWN_FLAG)) < ttl
    except Exception:
        return False
