# ESP32 桌面状态灯 · WorkBuddy 版

一块放在桌上的小屏幕，实时显示 WorkBuddy 在干什么，需要审批时闪一下提醒你。

```
┌─────────────────────────────┐
│ USB              78% 🔋⚡ ⚙ │
├─────────────────────────────┤
│ ▌重构登录模块        需要确认│   ← 红
│   Bash  12s                 │
│ ▌写周报                执行中│   ← 蓝
│   Edit  239s                │
│ ▌整理会议纪要          执行中│   ← 蓝
│   Read  232s                │
│ ▌复盘文档              已完成│   ← 绿
│   3 分钟前                   │
└─────────────────────────────┘
```

## 这是什么

WorkBuddy 有 hooks 机制，每个关键节点（收到提问、调用工具、任务结束）会执行一条配置好的命令，
并通过 stdin 传一段 JSON。本项目把这个事件流变成一块看得见的桌面状态灯。

**它做三件事**：

1. **显示状态** —— 最多同时跟踪 4 个并行任务，按优先级排序
2. **提醒审批** —— 需要确认时弹出命令内容，3 秒后自动消失
3. **不参与决策** —— 批准/拒绝永远在电脑上做（原因见下方「为什么设备不参与审批」）

## 硬件

**微雪 ESP32-S3-Touch-LCD-1.54**（带壳、带电池、带触摸）

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3 | 双核 240MHz，8MB PSRAM |
| 屏幕 | ST7789 240×240 | SPI，1.54 寸 IPS |
| 触摸 | CST816 | I2C，地址 0x15 |
| 电池 | 3.7V 锂电池 | 带充电管理 |

### 引脚（已从原理图核对）

| 功能 | GPIO | 功能 | GPIO |
|---|---|---|---|
| LCD_DC | 45 | 触摸 SDA | 42 |
| LCD_CS | 21 | 触摸 SCL | 41 |
| LCD_SCK | 38 | 触摸 RST | 47 |
| LCD_MOSI | 39 | 触摸 INT | 48 |
| LCD_RST | 40 | 电池 ADC | 1 |
| LCD_BL | 46 | 电池使能 | 2 |
| I2S MCLK | 8 | 充电检测 | 3 |
| I2S BCLK | 9 | 功放使能 | 7 |
| I2S LRC | 10 | I2S DIN | 11 |
| I2S DOUT | 12 | | |

> 音频部分当前固件没启用，引脚先留着。

## 架构：两条通道，自动选路

```
                    ┌──────────────────────────┐
WorkBuddy hooks ───►│ wb-status-hook.py        │
                    │ approve-hook.py          │
                    └───────────┬──────────────┘
                                │ wb_common.post()
                    ┌───────────▼──────────────┐
                    │ 优先级：USB 桥 → WiFi     │
                    └───────────┬──────────────┘
                    ┌───────────┴───────────┐
              USB 串口（推荐）          HTTP（WiFi）
                    │                       │
        wb-bridge.py:18765 ──串口──►  http://wbstatus.local
                    └───────────┬───────────┘
                                ▼
                          ESP32 屏幕
```

**为什么优先走 USB**：公司 WiFi 常见三个拦路虎 —— 只有 5GHz（ESP32-S3 只支持 2.4G）、
Web Portal 需要账号认证、客户端隔离（设备之间互不可见）。任意一个都能让「连上 WiFi 但 Mac 访问不到」。
插数据线走 USB 全部绕开。

桥没在跑时 hook 会自动拉起，插上线就生效，拔了线自动退回 WiFi，两边代码都不用改。

## 目录结构

```
esp32-wb-status/
├── firmware/
│   ├── wb_status_v3/          ← 当前固件（微雪 1.54 触摸屏）
│   │   ├── wb_status_v3.ino   主循环、任务表、HTTP 接口
│   │   ├── config.h           引脚、配色、常量
│   │   ├── ui.cpp / ui.h      屏幕绘制、触摸、页面状态机
│   │   ├── net.cpp / net.h    WiFi 扫描、NVS 凭据、配网热点
│   │   └── link.cpp / link.h  USB 串口通道
│   ├── wb_status/             ← 早期版本（ESP32-C3 + OLED，已废弃）
│   ├── i2c_scan/              ← I2C 引脚探测工具
│   └── README-v3.md           固件详细文档（引脚表、踩坑记录）
├── hook/
│   ├── wb-status-hook.py      状态上报 + 审批提醒（7 个事件）
│   ├── approve-hook.py        ⚠️ 已废弃（提醒逻辑已合并进上一个）
│   ├── wb-bridge.py           USB 串口 ↔ 本地 HTTP 桥
│   ├── wb_common.py           找设备的公共逻辑
│   ├── sync-tasks.py          把最近会话一次性推到设备（预览用）
│   └── heartbeat.sh           30 秒心跳（可选）
├── simulator/
│   └── index.html             浏览器模拟器，改 UI 前先在这里看效果
└── renders/                   产品效果图（AI 生成，仅供参考）
```

## 编译与烧录

依赖库（Arduino 库管理器装）：

- **GFX Library for Arduino** (moononournation) — 1.6.9
- **U8g2** (olikraus) — 2.36.19
- **U8g2_for_Adafruit_GFX** (olikraus) — 1.8.0

```bash
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc,USBMode=hwcdc"

arduino-cli compile --fqbn "$FQBN" firmware/wb_status_v3
arduino-cli upload  -p /dev/cu.usbmodem* --fqbn "$FQBN" firmware/wb_status_v3

# ⚠️ 烧完必须补一条硬复位，否则板子停在下载模式，屏幕一片安静
~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool \
  --port /dev/cu.usbmodem* --after hard_reset chip_id
```

编译结果：Flash **1,292,026 字节（41%）**，RAM **55,172 字节（16%）**。

`CDCOnBoot=cdc` 是关键 —— 它让 `Serial` 走 USB CDC，串口通道才能工作。

## Mac 端接入

### 1. 装 hooks

把 `hook/` 下两个脚本注册到 `~/.workbuddy-ai/settings.json` 的 `hooks` 字段：

只需要注册**一个**脚本 `wb-status-hook.py`，挂在 7 个事件上：

| 事件 | 作用 |
|---|---|
| SessionStart | 会话开始 → 空闲 |
| UserPromptSubmit | 收到提问 → 思考中，顺便更新任务名 |
| PreToolUse | 调用工具 → 执行中，并缓存 `tool_input` |
| PostToolUse | 工具完成 → 执行中 |
| **Notification** | **真的要你授权时才推审批提醒** |
| Stop | 一轮结束 → 已完成 |
| SessionEnd | 会话结束 → 移除 |

完整片段见 `hook/settings-hooks-snippet.json`。**合并**进现有配置，不要覆盖。

```json
{
  "type": "command",
  "command": "/usr/bin/env python3 \"$HOME/workspace/ESP32-tools/esp32-wb-status/hook/wb-status-hook.py\""
}
```

> 路径含空格时必须用双引号包住。

### 2. 启动桥接

```bash
python3 hook/wb-bridge.py            # 前台跑
python3 hook/wb-bridge.py --check    # 只探测设备在不在
```

hook 会自动拉起它，一般不用手动启动。端口默认 **18765**（避开常见的 8765）。

## 屏幕说明

### 主界面

顶栏优先级：**USB（蓝）> WiFi 断开（红）> 离线（黄）> 电量低 > N 个任务**

每行结构：

```
▌ 任务名（截 10 个汉字）        状态
  工具名  Ns（进行中读秒 / 已完成显示"3 分钟前"）
```

竖条和状态字同色：**需要确认=红，执行中=蓝，已完成/思考中=绿，出错=黄**。

### 审批提醒页

```
需要确认                    3s
────────────────────────────────
        重构登录模块
Bash
┌──────────────────────────────┐
│ npm run build -- --mode prod │
│ uction && rsync -av dist/    │
│ deploy@10.0.0.8:/var/www/    │
└──────────────────────────────┘
┌──────────────────────────────┐
│      请在电脑上确认           │
│    轻触屏幕关闭提示           │
└──────────────────────────────┘
```

**3 秒后自动消失**，点屏幕可以提前关掉。改时长改 `config.h` 的 `APPROVAL_MS`。

### 配网

连不上已存 WiFi 时自动开热点 `WB-Setup`（密码 `wb12345678`），手机连上后访问 `192.168.4.1`。
也可以点屏幕右上角齿轮，在设备上直接选 WiFi、输密码。

## 调试命令

### 串口协议（`link.cpp`）

行协议，`|` 分隔，`\n` 结尾；回复统一以 `@` 开头，和调试日志区分。

| 命令 | 回复 | 说明 |
|---|---|---|
| `PING` | `@PONG` | 探活 |
| `STATE\|sid\|st\|tool\|label` | `@OK` | 更新任务 |
| `APPROVE\|req\|tool\|label\|cmd` | `@OK` | 推提醒 |
| `DEC\|req` | `@DEC\|req\|allow\|deny\|pending\|none` | 查决策（保留兼容） |
| `LIST` | `@LIST\|<json>` | 任务列表 |
| `CTEST` | `@OK` | 屏幕色彩自检（6 条纯色带） |
| `NIGHT\|1` / `NIGHT\|0` | `@OK` | 强制夜间 / 回到自动 |
| `TIME\|<epoch>` | `@OK` | 对时（USB 模式下没有 NTP） |

### HTTP（走桥接或 WiFi）

```bash
curl http://127.0.0.1:18765/ping
curl http://127.0.0.1:18765/list
curl "http://127.0.0.1:18765/state?sid=t1&st=working&tool=Bash&label=构建固件"
curl "http://127.0.0.1:18765/approve?req=a1&tool=Bash&label=重构&cmd=npm%20run%20build"
curl "http://127.0.0.1:18765/ctest"        # 色彩自检
curl "http://127.0.0.1:18765/night?on=1"   # 强制夜间
```

### 一次性同步真实会话

```bash
python3 hook/sync-tasks.py --dry-run   # 只看解析结果
python3 hook/sync-tasks.py             # 推到设备
```

扫 `~/.workbuddy-ai/projects/*/*.jsonl`，取每个会话的第一条用户消息当任务名。

## 为什么设备不参与审批

`PreToolUse` hook 的返回值会**替代**客户端的确认弹窗：

```json
{"permissionDecision": "allow"}   → 工具直接执行，客户端不弹窗
{"permissionDecision": "deny"}    → 工具被拒绝，客户端不弹窗
什么都不输出                       → 客户端弹正常确认框
```

两者互斥，没法「两边同时弹」。所以选了**客户端为准、设备只提醒**：

- 永远不输出 `permissionDecision`，客户端照常弹窗
- 工具调用不会被设备卡住

### 只在真的要授权时才提醒

这一点很关键。**不能把提醒挂在 `PreToolUse` 上** —— 那个阶段无法知道客户端到底会不会弹权限框，
结果每次 `Bash` / `Write` / `Edit` 都往设备推一条，绝大多数是自动放行的，屏幕被无意义提醒刷屏。

正确做法是用 **`Notification` 事件**，它只在真的弹出权限框时才触发：

```
PreToolUse   → 推状态「执行中/Bash」，同时把 tool_input 缓存到 ~/.wb-status/pending.json
                （不推审批提醒）
Notification → 内容含 permission / approval / 确认 时，取缓存里的命令，推审批提醒
```

所有 `Notification` 原文会追加到 `~/.wb-status/notifications.log`，方便排查到底什么时候弹。

如果你更想要「设备优先审批」，让 hook 等设备返回即可 —— 但那样人不在设备旁边时，
每次工具调用都要等到超时才回退，体验很差。

## 踩过的坑

完整记录见 `firmware/README-v3.md` 第十二节，这里列最要命的几条：

**1. 烧完板子不启动**
`arduino-cli upload` 打印了 `Hard resetting via RTS pin...`，但 USB-Serial/JTAG 不吃 RTS 复位，
板子停在下载模式。必须补 `esptool --after hard_reset chip_id`，或者按板上的 RST。

**2. GfxBridge 嵌套 SPI 事务会死锁**
`Adafruit_GFX::drawFastHLine()` 内部会 `startWrite() → writePixel() → drawPixel()`，
而 `Arduino_GFX::drawPixel()` 自己又会 `startWrite()/endWrite()`。
桥接类如果把 `startWrite` 也转发下去，就会嵌套 `beginWrite()`，把 CS/DC 状态机搞乱，
表现为**主循环直接卡住**。正确做法：直接转发 `drawPixel` / `drawFastHLine` / `drawFastVLine`，
`startWrite` / `endWrite` 保持空实现。

**3. 中文字库选错会掉字**
`u8g2_font_wqy12_t_chinese1/2/3` 看着像中文全集，实际只有 **388 / 501** 个字。
「已完成」只画出「成」，「WiFi 断开」掉「断」。必须用 `u8g2_font_wqy12_t_gb2312`（**7757 字**，203KB）。

**4. RGB565 的深色陷阱**
只有 5-6-5 位精度，低于 `(16,16,16)` 的颜色基本都塌缩成黑。
深色主题的卡片色不要低于 `#181A1E`，否则卡片框等于没画。

**5. 屏幕每秒闪**
主循环为刷秒数每秒整屏重绘，`drawMain()` 开头 `fillScreen()` 会先黑一下。
改成离屏画布（`PsramCanvas`），整帧画进 PSRAM 后一次 `flush()` 推屏，中间不出现黑屏。

**6. USB 模式下没有 NTP**
夜间定时依赖 `getLocalTime()`，没 WiFi 就拿不到时间，20:00–9:30 的调光永远不触发。
解决：Mac 端 `wb-bridge.py` 每 10 分钟推一次 `TIME|<epoch>`。

## 已知限制

1. **电池读数不准** —— 实测读到 `2.80V / 0%` 却显示充电中，需核对电池是否接好 / 校准 `BAT_DIV`
2. **音频未接** —— 引脚已确认，当前固件没启用
3. **只显示运行中的会话** —— 空闲对话不产生 hook 事件，所以不会出现在屏幕上
4. **子 agent 归到父任务** —— 共享 `session_id`，不会单独占一行
5. **4 个任务上限** —— 超出按「优先级 + 最近活跃」淘汰

## 相关文档

- `firmware/README-v3.md` —— 固件详细文档：引脚表、配色、夜间逻辑、完整踩坑记录
- `simulator/index.html` —— 浏览器模拟器，改 UI 前先在这里看效果（不用烧板子）
