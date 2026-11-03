# esp32-dingtalk-bridge — 钉钉日历桥接服务

跑在 Mac 上的 Python 小服务：把钉钉日历日程、天气、农历精简成 JSON，
通过局域网 HTTP 提供给 ESP32 时钟轮询。板子不直接接触钉钉凭证。

```
钉钉日历(CalDAV/开放平台) ──> 本服务(定时同步+缓存+精简) ──WiFi──> ESP32 时钟
Open-Meteo 天气(免 key) ──────┘
```

## 快速开始（CalDAV 模式，免建钉钉应用）

1. 手机钉钉 → 底部「日历」→ 右上角设置 → **日历设置 → CalDAV 同步** → 获取 CalDAV 账号，
   生成服务器地址、用户名、专用密码
2. 把三项填进 `config.json` 的 `caldav_url / caldav_username / caldav_password`
3. 启动：

```bash
cd esp32-dingtalk-bridge
.venv/bin/python bridge.py     # 首次运行若缺配置会自动生成 config.json 模板
```

启动成功会打印板子要访问的地址：`http://<Mac局域网IP>:8787/api/events`。

## HTTP 接口（给 ESP32 用）

| 接口 | 说明 |
|---|---|
| `GET /api/events` | 今天起 N 天日程列表 + 当日农历 `lunar` |
| `GET /api/next` | 下一条日程，含 `minutes_until` 倒计时分钟数 |
| `GET /api/weather` | 实时天气（温度/文案/icon/湿度/空气质量）+ 农历 + **未来三天预报** |
| `GET /api/sync` | 手动触发一次日程同步 |
| `GET /api/health` | 缓存状态与最近错误 |

`/api/weather` 返回示例：

```json
{
  "weather": {"city": "北京·朝阳", "temp": 11, "text": "晴", "icon": "clear",
              "humidity": 64, "aqi_text": "空气良"},
  "lunar": "丙午马年 · 九月廿四",
  "forecast": [
    {"date": "11-03", "week": "周二", "lunar": "丙午马年 · 九月廿四",
     "temp": 17, "text": "晴", "icon": "clear"},
    {"date": "11-04", "week": "周三", "lunar": "丙午马年 · 九月廿六",
     "temp": 13, "text": "阴", "icon": "cloud"},
    {"date": "11-05", "week": "周四", "lunar": "丙午马年 · 九月廿七",
     "temp": 16, "text": "雾", "icon": "fog"}
  ],
  "stale": false,
  "server_time": "2026-11-03T10:30:00+08:00"
}
```

- `stale=true` 表示最近一次同步失败，当前返回的是旧缓存（断网时板子照常用）
- `icon` 取值：`clear / cloud / rain / snow / thunder / fog`，板子按此选天气图标
- 可选鉴权：`config.json` 里 `bridge_token` 非空时，请求需带头 `X-Bridge-Token`
- **`forecast` 是三天预报**（`daily=temperature_2m_max,weather_code` + `forecast_days=3`），
  农历和周几由服务端算好一起下发。板子每次推送都带上这三天，
  **断开 Mac 后仍能按日期自动切换**（过 0 点自动切到下一天）

## 配置项（config.json）

| 字段 | 默认 | 说明 |
|---|---|---|
| `mode` | `caldav` | `caldav` 免建应用只读；`open_api` 开放平台（可写日程/增量同步） |
| `caldav_url/username/password` | 空 | CalDAV 模式必填 |
| `app_key/app_secret/user_id` | 空 | open_api 模式必填，需开「日历应用中日程读权限」 |
| `calendar_id` | `primary` | open_api 模式的日历 ID |
| `sync_interval_seconds` | 600 | 日程同步间隔 |
| `days_ahead` | 2 | 拉取今天起几天的日程 |
| `weather_enabled` | true | 天气开关 |
| `weather_city/lat/lon` | 北京·朝阳 | 天气定位，Open-Meteo 免 key |
| `weather_interval_seconds` | 1800 | 天气刷新间隔 |
| `host` / `port` | 0.0.0.0 / 8787 | 监听地址，板子需同局域网 |
| `bridge_token` | 空 | 可选接口鉴权 |

## 容错设计

- 日程：内存 + `.events_cache.json` 磁盘双缓存，同步失败保留旧数据，重启不丢
- 天气：同上，`.weather_cache.json`；AQI 独立请求，失败不影响主天气
- open_api 模式 accessToken 自动缓存续期（2h 有效期，提前 5 分钟刷新）

## 踩过的坑

- **zhdate 不支持带时区的 datetime**，调用前先 `.replace(tzinfo=None)`，否则农历返回空
- 农历格式转换：`zhdate.chinese()` → 「丙午马年 · 九月廿四」（二十→廿，三十→卅）
- CalDAV 循环日程用 `date_search(expand=True)` 服务端展开，不支持时回退不展开

## 依赖与测试

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

依赖：flask / requests / caldav / zhdate。已做接口级回归测试
（日程解析、农历断言、天气真实拉取、双模式路由），修改后建议照此自测。
