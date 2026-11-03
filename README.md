# ESP32-tools

自己折腾 ESP32 小硬件时写的东西，每个子目录是一个独立项目。

## 项目列表

### [esp32-wb-status](./esp32-wb-status) — 桌面状态灯

一块放在桌上的 1.54 寸小屏，实时显示 **WorkBuddy** 在干什么：空闲 / 思考中 / 执行中 /
需要确认 / 已完成。需要审批时弹出命令内容提醒你，最多同时跟踪 4 个并行任务。

```
┌─────────────────────────────┐
│ USB              78% 🔋⚡ ⚙ │
├─────────────────────────────┤
│ ▌重构登录模块        需要确认│
│ ▌写周报                执行中│
│ ▌整理会议纪要          执行中│
│ ▌复盘文档              已完成│
└─────────────────────────────┘
```

- **硬件**：微雪 ESP32-S3-Touch-LCD-1.54（ST7789 240×240 + CST816 触摸 + 电池）
- **通道**：USB 串口优先，WiFi 兜底，自动选路
- **栈**：Arduino + Arduino_GFX + U8g2 + Python 标准库（Mac 端无第三方依赖）
- **特点**：离屏画布防闪屏、夜间自动调光、电池电量、触摸配网
- **预览**：`simulator/index.html` 浏览器模拟器，可加删任务、切场景、触发审批

### [esp32-clock](./esp32-clock) — 科技风日程时钟

同一块 1.54 寸小屏的另一种玩法：科技风时钟 + 钉钉日程提醒 + 天气 + 农历。
v1 青绿圆环界面已烧录验证，另有蓝色仪表盘、赛博炫酷两版设计稿待实现。

- **栈**：Arduino + Arduino_GFX + U8g2（中文/大数字字体）+ ArduinoJson
- **模式**：无 WiFi 配置时演示模式（点按切换演示日程），配好后 NTP 对时 + 轮询桥接
- **配套**：数据来自下面的 esp32-dingtalk-bridge

### [esp32-dingtalk-bridge](./esp32-dingtalk-bridge) — 钉钉日历桥接服务

跑在 Mac 上的 Python 服务，把钉钉日历、天气、农历精简成局域网 JSON 接口给板子用。

- **双日程源**：CalDAV（免建钉钉应用，手机生成专用密码即可）/ 开放平台 API（可扩展写日程）
- **天气**：Open-Meteo 免 key，默认北京朝阳；农历由 zhdate 算好直接下发
- **容错**：内存 + 磁盘双缓存，断网/同步失败保留旧数据并标记 `stale`

### [esp32-clock](./esp32-clock) — 科技风日程时钟

同一块 1.54 寸屏，改造成桌面时钟：青绿圆环 + 大字时间 + 底部「下一提醒」卡片。
日程来自钉钉日历，天气和农历也一起显示。

- **数据来源**：钉钉 CalDAV（免建应用）或开放平台，由桥接服务统一拉取
- **通道**：USB 串口优先（Mac 推时间和日程，不需要 WiFi），WiFi + NTP 兜底
- **特点**：离屏画布整帧渲染 + 分带刷新；界面预览见 `simulator/ui-preview.html`

### [esp32-dingtalk-bridge](./esp32-dingtalk-bridge) — 钉钉日历桥接

跑在 Mac 上的 Flask 小服务，把钉钉日程、Open-Meteo 天气、农历精简成 JSON。
板子不直接接触钉钉凭证。

- **接口**：`/api/events` `/api/next` `/api/weather` `/api/sync` `/api/health`
- **容错**：内存 + 磁盘双缓存，同步失败保留旧数据，断网照常可用
- **依赖**：flask / requests / caldav / zhdate

## 目录约定

```
ESP32-tools/
└── <项目名>/
    ├── README.md          项目说明（这是什么、怎么用、踩过什么坑）
    ├── firmware/          Arduino 固件
    ├── hook/              Mac 端配套脚本（如有）
    ├── simulator/         浏览器界面预览（如有，直接双击打开）
    └── renders/           效果图（如有）
```

每个项目的 README 都是自包含的 —— 只看那一个文件就能把这个项目跑起来。

## 通用环境

| 工具 | 版本 | 用途 |
|---|---|---|
| arduino-cli | 1.5+ | 编译烧录 |
| esp32 core | 3.3.12 | ESP32 支持包 |
| Python | 3.13 | Mac 端脚本（只用标准库） |

ESP32 开发板管理器地址：

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

## 说明

`renders/` 里是 AI 生成的产品效果图，仅作示意，和实际外观有出入。
