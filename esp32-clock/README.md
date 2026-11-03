# esp32-clock — 桌面日程时钟

微雪 ESP32-S3-Touch-LCD-1.54（240×240 触摸屏）上的桌面时钟：暖白界面 + 钉钉日程 +
天气 + 农历。日程数据来自配套的 [esp32-dingtalk-bridge](../esp32-dingtalk-bridge)。

当前固件 **v2.0 暖白双模式**（日常 / 极简），v1 青绿圆环版备份在
`backup/v1-green-ring-20261103/`。

## 界面

浏览器打开 [`simulator/ui-preview.html`](simulator/ui-preview.html) 可以看交互预览：
日常/极简双模式切换、电池供电状态、走时演示。**改界面前先在这里调，不用烧板子。**

```
日常模式                              极简模式
┌───────────────────────────┐        ┌───────────────────────────┐
│ 11月03日 周二    ⚡  🔋   │        │                           │
│ ───────────────────────── │        │                           │
│                           │        │                           │
│        11:35              │        │        11:35              │
│                           │        │                           │
│ 北京时间   ·      27 秒   │        │ 丙午马年 · 九月廿五       │
│ ───────────────────────── │        └───────────────────────────┘
│ ◎  22° 晴                 │
│    北京 · 空气优          │
│ ┌───────────────────────┐ │
│ │▌19:30 提测   IN 10 MIN│ │
│ └───────────────────────┘ │
└───────────────────────────┘
```

配色：暖白底 `#F7F5EF` / 深灰字 `#2C2C2A` / 陶橙点缀 `#993C1D`。

## 三个界面与手势

| 界面 | 操作 |
|---|---|
| 主屏 | 左右滑动切换日程卡片 · 单击进日程列表 · **长按 0.65 秒切换日常/极简** |
| 提醒弹窗 | 日程开始前 10 分钟弹出 ·「知道了」不再打扰 ·「到点再叫我」到点再弹 |
| 日程列表 | 今天/明天分组 · 点任意处返回 |

极简模式的选择存在 NVS（`clockui/minimal`），重启后保持。

弹窗规则：未确认不自动消失；日程已开始未确认则不再弹；全天日程不弹窗。

## 运行模式

| 模式 | 条件 | 行为 | 顶栏标识 |
|---|---|---|---|
| **USB 模式**（推荐） | Mac 上跑 `hook/serial_push.py` | 时间与日程由 Mac 通过数据线推过来，**不需要 WiFi** | 蓝色 `USB` |
| 联网模式 | 填好 WiFi 与 `BRIDGE_BASE` | NTP 对时（阿里云）+ 每 60 秒轮询桥接 | 绿色 `NTP` |
| 演示模式 | 以上都没有 | 本地时钟 + 2 条假日程，点按切换 | 橙色 `DEMO` |

优先级 USB > WiFi。USB 连续 30 秒没有数据才回落。

### USB 模式怎么用

```bash
# 1. 桥接（提供日程数据）
cd ../esp32-dingtalk-bridge && .venv/bin/python bridge.py

# 2. 串口推送（提供时间 + 日程）
cd ../esp32-clock && python3 hook/serial_push.py
```

`serial_push.py`：

- 每 60 秒推 `TIME|<epoch>` —— **没 WiFi 就没有 NTP，这是唯一时间来源**
- 每 5 秒推 `NEXT|<HH:MM>|<分钟>|<标题>` 或 `NONE`

串口协议（`handleLine()`）：

| 命令 | 回复 | 说明 |
|---|---|---|
| `TIME|<epoch>` | `OK` | 对时，同时设 `TZ=CST-8` |
| `NEXT|<HH:MM>|<分钟>|<标题>` | `OK` | 下一条日程，填进 `events[0]` |
| `NONE` | `OK` | 清空日程 |
| `PING` | `PONG` | 探活 |

调试：

```bash
python3 hook/serial_push.py --check   # 只探测设备在不在
python3 hook/serial_push.py --once    # 推一次看设备回什么
```

> ⚠️ 串口同时只能被一个进程占用。装过 `esp32-wb-status` 的 hooks 要先停掉，
> 否则它会拉起 `wb-bridge.py` 抢串口。

### USB 模式拿不到的东西

固件里天气和农历只有 WiFi 路径（`/api/weather`）。走 USB 时：

| 元素 | 现状 |
|---|---|
| 时间 | ✅ 正常 |
| 日程卡片 | ✅ 只有 1 条（`NEXT` 只填 `events[0]`） |
| 天气 | ❌ 显示「天气暂未同步 / 等待天气数据」 |
| 农历 | ❌ 显示「北京时间」占位 |

要用天气和农历就配 WiFi，或者给串口协议加 `WEATHER` / `LUNAR` 命令
（固件 `handleLine()` + `serial_push.py` 两边都要动）。

## 配置

```bash
cp firmware/esp32-clock/secrets.example.h firmware/esp32-clock/secrets.h
```

`secrets.h` 已被 `.gitignore` 排除（会放 WiFi 密码，不能进仓库）。

| 字段 | 说明 |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | 留空则尝试沿用设备 NVS 里 `wbnet` 命名空间保存的凭据（wb-status 固件存过的），再失败才进演示模式 |
| `BRIDGE_BASE` | Mac 桥接地址，**必须是 Mac 当前局域网 IP**，`bridge.py` 启动时会打印 |

## 编译与烧录

库隔离在项目内 `arduino-user/`（不污染全局库目录，已在 `.gitignore` 里）：

```bash
cd esp32-clock
export ARDUINO_DIRECTORIES_USER=$PWD/arduino-user

FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc"

arduino-cli compile --fqbn "$FQBN" --warnings none firmware/esp32-clock
arduino-cli upload  -p /dev/cu.usbmodem* --fqbn "$FQBN" firmware/esp32-clock
```

⚠️ **必须带 `PartitionScheme=app3M_fat9M_16MB`**。默认 1.31MB 分区会到 98%，
加一行就爆。当前占用：Flash **1,612,511 字节（51%）**，RAM 49,804 字节（15%）。

依赖库（已在 arduino-user 内）：GFX Library for Arduino **1.6.9**、U8g2、ArduinoJson。
esp32 core 3.3.12。

## 硬件引脚（微雪官方示例核实）

| 功能 | 引脚 |
|---|---|
| LCD ST7789 (SPI) | DC45 / CS21 / SCK38 / MOSI39 / RST40 / 背光 BL46 |
| 触摸 CST816 (I2C 0x15) | SDA42 / SCL41 / INT48 / RST47 |
| I2S 音频 | MCLK8 / BCLK9 / LRC10 / DOUT12 / DIN11 |
| 电池供电保持 | BAT_EN = GPIO2，需置高 |
| IMU QMI8658 | 与触摸共用 I2C（SDA42/SCL41） |

## 渲染方式

整帧画进 PSRAM 离屏画布（`PsramCanvas`），再按需推屏：

```cpp
gfx->flush();                              // 整屏
gfx->flushBand(41, 108);                   // 只推某一段，用于走秒
```

分带刷新的原因：秒数每秒变一次，整屏推一次约 25ms；只推变化的那几十行可以省下
大部分带宽，也避免整屏闪。

> v1 用的是「局部擦除 + 局部重绘」，踩了三个擦除 bug（四角线框被擦掉、青色虚线与
> 圆环同半径被抹、秒点圆啃刻度）。改成画布后这些都不存在了——见 `backup/` 里的 v1 代码。

## 踩坑记录

- **GFX 1.6.0 与 esp32 core 3.3.12 不兼容**（`spiFrequencyToClockDiv` 签名变更），
  必须 1.6.9+；官方 demo 包内置的是 1.6.0，别直接用
- **烧录末尾 `Serial data stream stopped / exit status 2`**：S3 原生 USB 复位
  重新枚举导致，写入与校验其实已成功，无视即可
- **烧完板子不启动**：USB-Serial/JTAG 不吃 RTS 复位，必要时补
  `esptool --after hard_reset chip_id`
- 中文显示：GFX 直接 `setFont(u8g2_font_wqy12_t_gb2312)` + `setUTF8Print(true)`；
  大数字用 `u8g2_font_logisoso42_tn`，中文标题用 `wqy16`
- **RGB565 深色陷阱**：低于 `(16,16,16)` 的颜色基本塌缩成黑，深色卡片色不要低于 `#181A1E`

## 已知限制

1. **天气/农历只有 WiFi 路径** —— USB 模式下显示占位文案
2. **USB 只推 1 条日程** —— `NEXT` 只填 `events[0]`，列表页最多 1 条
3. **没有提示音** —— I2S 引脚已确认，固件未启用
4. **`BRIDGE_BASE` 是硬编码 IP** —— Mac 换网络后要重新烧录，建议路由器绑定静态 IP
