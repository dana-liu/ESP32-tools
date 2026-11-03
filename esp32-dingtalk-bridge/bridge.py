#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 时钟桥接服务
钉钉日历日程 -> 定时同步/精简缓存 -> 本地 HTTP 接口供 ESP32 轮询

两种日程来源(config.json 里 mode 二选一):
  caldav   免建应用。手机钉钉 -> 日历 -> 设置 -> 日历设置 -> CalDAV 同步,
           生成 caldav_url / caldav_username / caldav_password 填入即可
  open_api 钉钉开放平台企业内部应用, 填 app_key / app_secret / user_id,
           需开通「日历应用中日程读权限」, 支持写日程等扩展能力

接口:
  GET /api/events   日程列表(今天起 days_ahead 天) + 当日农历, 含本地缓存兜底
  GET /api/next     下一条即将到来的日程, 含倒计时分钟数
  GET /api/weather  实时天气(Open-Meteo 免 key) + 农历, 30 分钟刷新
  GET /api/sync     手动触发一次日程同步
  GET /api/health   服务状态
"""
import json
import os
import socket
import threading
import time
from datetime import datetime, timedelta, timezone

import requests
from flask import Flask, jsonify, request

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")
TOKEN_CACHE = os.path.join(BASE_DIR, ".token_cache.json")
EVENTS_CACHE = os.path.join(BASE_DIR, ".events_cache.json")
WEATHER_CACHE = os.path.join(BASE_DIR, ".weather_cache.json")
TZ = timezone(timedelta(hours=8))  # Asia/Shanghai, 不依赖系统 tzdata

CONFIG_TEMPLATE = {
    "mode": "caldav",
    "caldav_url": "",
    "caldav_username": "",
    "caldav_password": "",
    "app_key": "",
    "app_secret": "",
    "user_id": "",
    "calendar_id": "primary",
    "weather_enabled": True,
    "weather_city": "北京·朝阳",
    "weather_lat": 39.9219,
    "weather_lon": 116.4435,
    "weather_interval_seconds": 1800,
    "sync_interval_seconds": 600,
    "days_ahead": 2,
    "host": "0.0.0.0",
    "port": 8787,
    "bridge_token": ""
}


def now():
    return datetime.now(TZ)


def iso(dt):
    return dt.astimezone(TZ).isoformat(timespec="seconds")


class CalDAVClient:
    """钉钉日历 CalDAV 客户端(免建应用), 只读, 循环日程由服务端展开"""

    def __init__(self, cfg):
        import caldav  # 延迟导入, open_api 模式不依赖该库
        self._caldav = caldav
        self.url = cfg["caldav_url"]
        self.username = cfg["caldav_username"]
        self.password = cfg["caldav_password"]
        self._calendar = None

    def _get_calendar(self):
        if self._calendar is None:
            client = self._caldav.DAVClient(
                url=self.url, username=self.username, password=self.password)
            calendars = client.principal().calendars()
            if not calendars:
                raise RuntimeError("CalDAV 账号下没有可用日历")
            self._calendar = calendars[0]
        return self._calendar

    def get_events(self, time_min, time_max):
        cal = self._get_calendar()
        try:
            items = cal.date_search(start=time_min, end=time_max, expand=True)
        except Exception:
            items = cal.date_search(start=time_min, end=time_max, expand=False)
        events = [self._simplify(item) for item in items]
        events.sort(key=lambda e: e["start"] or "")
        return events

    @staticmethod
    def _simplify(item):
        vevent = item.vobject_instance.vevent
        dtstart = vevent.dtstart.value
        dtend = vevent.dtend.value if hasattr(vevent, "dtend") else dtstart
        is_all_day = not isinstance(dtstart, datetime)

        def fmt(dt):
            if not isinstance(dt, datetime):
                return dt.isoformat()  # 全天日程是 date
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=TZ)
            return iso(dt)

        return {
            "id": str(vevent.uid.value) if hasattr(vevent, "uid") else None,
            "title": str(vevent.summary.value) if hasattr(vevent, "summary")
            else "(无标题)",
            "start": fmt(dtstart),
            "end": fmt(dtend),
            "all_day": is_all_day,
            "location": str(vevent.location.value)
            if hasattr(vevent, "location") else "",
            "source": "dingtalk"
        }


class DingTalkClient:
    """钉钉企业内部应用客户端, accessToken 自动缓存/续期"""

    def __init__(self, cfg):
        self.app_key = cfg["app_key"]
        self.app_secret = cfg["app_secret"]
        self.user_id = cfg["user_id"]
        self.calendar_id = cfg.get("calendar_id") or "primary"
        self._lock = threading.Lock()
        self._token = None
        self._token_expire_at = 0.0
        self._load_token_cache()

    def _load_token_cache(self):
        try:
            with open(TOKEN_CACHE, "r", encoding="utf-8") as f:
                data = json.load(f)
            if data.get("expire_at", 0) > time.time():
                self._token = data["access_token"]
                self._token_expire_at = data["expire_at"]
        except (OSError, ValueError, KeyError):
            pass

    def _save_token_cache(self):
        try:
            with open(TOKEN_CACHE, "w", encoding="utf-8") as f:
                json.dump({"access_token": self._token,
                           "expire_at": self._token_expire_at}, f)
        except OSError:
            pass

    def token(self):
        with self._lock:
            if self._token and time.time() < self._token_expire_at - 300:
                return self._token
            resp = requests.post(
                "https://api.dingtalk.com/v1.0/oauth2/accessToken",
                json={"appKey": self.app_key, "appSecret": self.app_secret},
                timeout=15)
            resp.raise_for_status()
            data = resp.json()
            self._token = data["accessToken"]
            self._token_expire_at = time.time() + int(data.get("expireIn", 7200))
            self._save_token_cache()
            return self._token

    def get_events(self, time_min, time_max):
        """查询日程列表, 自动翻页, 返回精简后的列表"""
        url = "https://api.dingtalk.com/v1.0/calendar/users/%s/calendars/%s/events" % (
            self.user_id, self.calendar_id)
        headers = {"x-acs-dingtalk-access-token": self.token()}
        params = {"timeMin": iso(time_min), "timeMax": iso(time_max),
                  "maxResults": 100}
        events, pages = [], 0
        while True:
            resp = requests.get(url, headers=headers, params=params, timeout=20)
            resp.raise_for_status()
            data = resp.json()
            for item in data.get("events", []):
                events.append(self._simplify(item))
            next_token = data.get("nextToken")
            pages += 1
            if not next_token or pages >= 10:
                break
            params["nextToken"] = next_token
        events.sort(key=lambda e: e["start"])
        return events

    @staticmethod
    def _simplify(item):
        start, end = item.get("start", {}), item.get("end", {})
        is_all_day = bool(item.get("isAllDay"))
        return {
            "id": item.get("id"),
            "title": item.get("summary") or "(无标题)",
            "start": start.get("date") if is_all_day else start.get("dateTime"),
            "end": end.get("date") if is_all_day else end.get("dateTime"),
            "all_day": is_all_day,
            "location": (item.get("location") or {}).get("displayName", "")
            if isinstance(item.get("location"), dict) else "",
            "source": "dingtalk"
        }


def lunar_display(day=None):
    """当日农历, 如 '丙午马年 · 九月廿四'; 库缺失/解析失败时降级不报错"""
    try:
        from zhdate import ZhDate
        d = (day or now()).replace(tzinfo=None)  # zhdate 不支持 tz-aware
        text = ZhDate.from_datetime(d).chinese()
        parts = text.split(" ")  # 二零二六年九月二十四 丙午年 (马年)
        date_part = parts[0].split("年", 1)[1]
        date_part = date_part.replace("二十", "廿").replace("三十", "卅")
        return "%s%s · %s" % (parts[1].rstrip("年"), parts[2].strip("()"), date_part)
    except Exception:
        return None


WEATHER_CODES = {
    0: ("晴", "clear"), 1: ("晴间多云", "clear"), 2: ("多云", "cloud"),
    3: ("阴", "cloud"), 45: ("雾", "fog"), 48: ("雾", "fog"),
    51: ("毛毛雨", "rain"), 53: ("毛毛雨", "rain"), 55: ("毛毛雨", "rain"),
    56: ("冻雨", "rain"), 57: ("冻雨", "rain"),
    61: ("小雨", "rain"), 63: ("中雨", "rain"), 65: ("大雨", "rain"),
    66: ("冻雨", "rain"), 67: ("冻雨", "rain"),
    71: ("小雪", "snow"), 73: ("中雪", "snow"), 75: ("大雪", "snow"),
    77: ("雪", "snow"), 80: ("阵雨", "rain"), 81: ("阵雨", "rain"),
    82: ("暴雨", "rain"), 85: ("阵雪", "snow"), 86: ("阵雪", "snow"),
    95: ("雷雨", "thunder"), 96: ("雷雨", "thunder"), 99: ("雷雨", "thunder"),
}


def aqi_text(us_aqi):
    if us_aqi is None:
        return ""
    for limit, label in ((50, "空气优"), (100, "空气良"), (150, "轻度污染"),
                         (200, "中度污染"), (300, "重度污染")):
        if us_aqi <= limit:
            return label
    return "严重污染"


class WeatherService:
    """Open-Meteo 实时天气(免 key), 独立线程刷新, 磁盘缓存兜底"""

    def __init__(self, cfg):
        self.cfg = cfg
        self._lock = threading.Lock()
        self._state = {"weather": None, "updated_at": None, "error": None}
        try:
            with open(WEATHER_CACHE, "r", encoding="utf-8") as f:
                self._state.update(json.load(f))
        except (OSError, ValueError):
            pass

    def sync_once(self):
        try:
            resp = requests.get(
                "https://api.open-meteo.com/v1/forecast",
                params={"latitude": self.cfg["weather_lat"],
                        "longitude": self.cfg["weather_lon"],
                        "current": "temperature_2m,relative_humidity_2m,weather_code",
                        "timezone": "Asia/Shanghai"}, timeout=15)
            resp.raise_for_status()
            cur = resp.json()["current"]
            text, icon = WEATHER_CODES.get(cur.get("weather_code"), ("未知", "cloud"))
            weather = {
                "city": self.cfg.get("weather_city", ""),
                "temp": round(cur["temperature_2m"]),
                "text": text, "icon": icon,
                "humidity": cur.get("relative_humidity_2m"),
                "aqi_text": self._fetch_aqi(),
            }
            with self._lock:
                self._state.update({"weather": weather, "updated_at": iso(now()),
                                    "error": None})
            with open(WEATHER_CACHE, "w", encoding="utf-8") as f:
                json.dump(self._state, f, ensure_ascii=False)
            print("[%s] 天气同步成功: %s %s°" % (iso(now()), text, weather["temp"]))
            return True
        except Exception as exc:
            with self._lock:
                self._state["error"] = str(exc)
            print("[%s] 天气同步失败: %s" % (iso(now()), exc))
            return False

    def _fetch_aqi(self):
        """AQI 独立获取, 失败不影响主天气数据"""
        try:
            resp = requests.get(
                "https://air-quality-api.open-meteo.com/v1/air-quality",
                params={"latitude": self.cfg["weather_lat"],
                        "longitude": self.cfg["weather_lon"],
                        "current": "us_aqi", "timezone": "Asia/Shanghai"},
                timeout=10)
            resp.raise_for_status()
            return aqi_text(resp.json().get("current", {}).get("us_aqi"))
        except Exception:
            return ""

    def snapshot(self):
        with self._lock:
            return dict(self._state)

    def run_forever(self):
        interval = int(self.cfg.get("weather_interval_seconds", 1800))
        while True:
            self.sync_once()
            time.sleep(interval)


class SyncService:
    """后台定时同步, 内存 + 磁盘双缓存, 失败时保留旧数据"""

    def __init__(self, cfg):
        self.cfg = cfg
        mode = cfg.get("mode", "caldav")
        if mode == "open_api":
            self.client = DingTalkClient(cfg)
        elif mode == "caldav":
            self.client = CalDAVClient(cfg)
        else:
            raise ValueError("config.json 的 mode 只支持 caldav / open_api")
        self._lock = threading.Lock()
        self._state = {"events": [], "updated_at": None, "error": None}
        self._load_cache()

    def _load_cache(self):
        try:
            with open(EVENTS_CACHE, "r", encoding="utf-8") as f:
                data = json.load(f)
            self._state.update({"events": data.get("events", []),
                                "updated_at": data.get("updated_at")})
        except (OSError, ValueError):
            pass

    def _save_cache(self):
        try:
            with open(EVENTS_CACHE, "w", encoding="utf-8") as f:
                json.dump({"events": self._state["events"],
                           "updated_at": self._state["updated_at"]},
                          f, ensure_ascii=False)
        except OSError:
            pass

    def sync_once(self):
        try:
            start = now().replace(hour=0, minute=0, second=0, microsecond=0)
            end = start + timedelta(days=int(self.cfg.get("days_ahead", 2)))
            events = self.client.get_events(start, end)
            with self._lock:
                self._state.update({"events": events, "updated_at": iso(now()),
                                    "error": None})
            self._save_cache()
            print("[%s] 同步成功, %d 条日程" % (iso(now()), len(events)))
            return True
        except Exception as exc:  # 网络/鉴权/限流等, 保留旧缓存
            with self._lock:
                self._state["error"] = str(exc)
            print("[%s] 同步失败: %s" % (iso(now()), exc))
            return False

    def snapshot(self):
        with self._lock:
            return dict(self._state)

    def run_forever(self):
        interval = int(self.cfg.get("sync_interval_seconds", 600))
        while True:
            self.sync_once()
            time.sleep(interval)


app = Flask(__name__)
sync_service = None
weather_service = None


@app.before_request
def check_bridge_token():
    """可选鉴权: config.json 里 bridge_token 非空时, 请求需带 X-Bridge-Token"""
    expected = (sync_service.cfg.get("bridge_token") or "") if sync_service else ""
    if expected and request.path.startswith("/api/"):
        if request.headers.get("X-Bridge-Token") != expected:
            return jsonify({"error": "unauthorized"}), 401


@app.route("/api/events")
def api_events():
    state = sync_service.snapshot()
    return jsonify({
        "events": state["events"],
        "count": len(state["events"]),
        "lunar": lunar_display(),
        "updated_at": state["updated_at"],
        "stale": state["error"] is not None,
        "error": state["error"],
        "server_time": iso(now())
    })


@app.route("/api/weather")
def api_weather():
    state = weather_service.snapshot() if weather_service else {}
    return jsonify({
        "weather": state.get("weather"),
        "lunar": lunar_display(),
        "updated_at": state.get("updated_at"),
        "stale": state.get("error") is not None,
        "error": state.get("error"),
        "server_time": iso(now())
    })


@app.route("/api/next")
def api_next():
    state = sync_service.snapshot()
    upcoming = []
    for e in state["events"]:
        if e["all_day"] or not e["start"]:
            continue
        try:
            start_dt = datetime.fromisoformat(e["start"])
            if start_dt.tzinfo is None:
                start_dt = start_dt.replace(tzinfo=TZ)
            if start_dt >= now():
                upcoming.append((start_dt, e))
        except ValueError:
            continue
    if not upcoming:
        return jsonify({"next": None, "server_time": iso(now())})
    start_dt, event = min(upcoming, key=lambda x: x[0])
    result = dict(event)
    result["minutes_until"] = int((start_dt - now()).total_seconds() // 60)
    return jsonify({"next": result, "server_time": iso(now())})


@app.route("/api/sync")
def api_sync():
    ok = sync_service.sync_once()
    return jsonify({"ok": ok, "updated_at": sync_service.snapshot()["updated_at"],
                    "error": sync_service.snapshot()["error"]})


@app.route("/api/health")
def api_health():
    state = sync_service.snapshot()
    return jsonify({"ok": True, "events_cached": len(state["events"]),
                    "updated_at": state["updated_at"], "error": state["error"]})


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def load_config():
    if not os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(CONFIG_TEMPLATE, f, ensure_ascii=False, indent=2)
        print("已生成配置模板: %s" % CONFIG_PATH)
        print("请填写 app_key / app_secret / user_id 后重新启动")
        return None
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    if cfg.get("mode", "caldav") == "open_api":
        required = ("app_key", "app_secret", "user_id")
    else:
        required = ("caldav_url", "caldav_username", "caldav_password")
    missing = [k for k in required if not cfg.get(k)]
    if missing:
        print("config.json 缺少必填项: %s" % ", ".join(missing))
        return None
    return cfg


def main():
    global sync_service, weather_service
    cfg = load_config()
    if cfg is None:
        raise SystemExit(1)
    sync_service = SyncService(cfg)
    sync_service.sync_once()  # 启动先同步一次, 失败也继续提供服务(用缓存)
    threading.Thread(target=sync_service.run_forever, daemon=True).start()
    if cfg.get("weather_enabled", True):
        weather_service = WeatherService(cfg)
        weather_service.sync_once()
        threading.Thread(target=weather_service.run_forever, daemon=True).start()
    port = int(cfg.get("port", 8787))
    print("ESP32 访问地址: http://%s:%d/api/events" % (lan_ip(), port))
    print("下一条日程:     http://%s:%d/api/next" % (lan_ip(), port))
    app.run(host=cfg.get("host", "0.0.0.0"), port=port, threaded=True)


if __name__ == "__main__":
    main()
