# WorkBuddy 状态灯 v3 —— 微雪 ESP32-S3-Touch-LCD-1.54

面向成品板 `ESP32-S3-Touch-LCD-1.54`（1.54" 240×240 彩屏 + 电容触摸 + 电池）的完整固件。

## 这版做了什么

| 功能 | 说明 |
|---|---|
| 任务列表 | 最多 4 个并行任务同屏，按优先级排序，左侧色条标状态 |
| 触摸选 WiFi | 齿轮 → 扫描 → 点选 → 虚拟键盘输密码 |
| 多网络 | NVS 存 5 组凭据，开机自动连能连上的那个，掉线轮询重连 |
| 热点兜底 | 全都连不上自动开 `WB-Setup` 热点，手机网页配网 |
| HTTP 接口 | 给 WorkBuddy hook 推状态用 |

## 一、装库

Arduino IDE → 库管理器，装这三个：

- **GFX Library for Arduino**（作者 moononournation）
- **U8g2_for_Adafruit_GFX**（作者 olikraus）
- **U8g2**（作者 olikraus）

## 二、编译设置

| 项 | 值 |
|---|---|
| Board | **ESP32S3 Dev Module** |
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** 或 Huge APP |
| USB CDC On Boot | **Enabled**（否则串口看不到输出） |
| Upload Speed | 921600 |

### 已验证编译通过

用 arduino-cli + esp32 core 3.3.12 实测：

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc,USBMode=hwcdc" \
  firmware/wb_status_v3
```

结果：**Flash 1,246,939 字节（39%）**，**RAM 53,492 字节（16%）**，无警告。`i2c_scan` 也编译通过（327KB）。

### 一个必须知道的兼容性坑

`GFX Library for Arduino` 从 1.6 起**不再继承 `Adafruit_GFX`**（现在只有 `public Print`），而 `U8g2_for_Adafruit_GFX::begin()` 只接受 `Adafruit_GFX&`，直接传会报：

```
error: cannot convert 'Arduino_GFX' to 'Adafruit_GFX&'
```

`ui.cpp` 里的 `GfxBridge` 类专门解决这个：继承 `Adafruit_GFX`，把 `drawPixel / startWrite / endWrite` 转发给 `Arduino_GFX`。**不要删**，删了编不过。

## 三、引脚定义（已从原理图确认）

微雪文档没给引脚表，但从**原理图 PDF** 里提取出来了，`config.h` 已填好：

| 功能 | GPIO | 说明 |
|---|---|---|
| LCD DC / CS / SCK / MOSI / RST | 45 / 21 / 38 / 39 / 40 | ST7789 |
| **LCD_BL** | **46** | 背光，PWM 可调 |
| TP_SDA / TP_SCL | 42 / 41 | 触摸 I2C（与 codec、IMU 共用一条总线） |
| TP_RST / TP_INT | 47 / 48 | 触摸复位 / 中断 |
| BAT_ADC / BAT_EN / CHG_STAT | 1 / 2 / 3 | 电池电压 / 使能 / 充电状态 |
| I2S MCLK / BCLK / LRC | 8 / 9 / 10 | 音频时钟 |
| I2S 麦克风 / 喇叭 | 11 / 12 | codec 数据线 |
| PA_CTRL | 7 | 功放使能，当前固件拉低省电 |

> ⚠️ **早期版本 `TP_RST` 和 `TP_INT` 填反了**（48/47），已修正为 **47/48**。如果你烧过旧版本触摸没反应，就是这个原因。

### 可选：验证一下

如果触摸还是不动，跑 `firmware/i2c_scan/i2c_scan.ino` 确认。串口应该看到：

```
SDA=42 SCL=41  →  3 个设备: 0x15(CST816 触摸 ★) 0x18(ES8311 codec) 0x6B(QMI8658 IMU)
```

扫不到就先确认板子上电（按一下 PWR）、触摸排线插紧。

## 四、烧主固件

打开 `firmware/wb_status_v3/wb_status_v3.ino`（同目录的 `config.h`、`net.*`、`ui.*` 会被自动带上），直接上传。

首次开机行为：

- **有已存凭据** → 尝试连接，成功则进入任务列表
- **没有凭据 / 连不上** → 自动开热点 `WB-Setup`（密码 `wb12345678`）

## 五、配网

### 方式 A：触摸屏（推荐）

1. 主界面右上角齿轮 → 进入扫描
2. 点选你的 WiFi
3. 键盘输密码（`AB` 切大小写，`删除` 退格，`连接` 提交）
4. 连上后自动回主界面

### 方式 B：热点兜底

1. 手机连 `WB-Setup`，密码 `wb12345678`
2. 浏览器打开 `192.168.4.1`
3. 选 WiFi、输密码、保存
4. 设备自动重启并连接

> 手机提示「无互联网连接」是正常的，选「保持连接」。

## 六、接 WorkBuddy

沿用 `hook/wb-status-hook.py`，只要把设备地址改成新板子的：

```bash
export WB_STATUS_URL="http://wbstatus.local"
```

或者在 hook 脚本里直接写死 IP。

`settings.json` 的 hooks 配置见 `hook/settings-hooks-snippet.json`，改完记得去 `/hooks` 面板确认一次。

验证：

```bash
curl "http://wbstatus.local/list"
curl "http://wbstatus.local/state?sid=test&st=working&tool=Bash&label=测试任务"
```

## 七、屏幕审批

任务列表里显示「需要确认」时，可以直接在屏幕上批准或拒绝，不用回到电脑。

### 交互设计

| 操作 | 行为 | 为什么这样设计 |
|---|---|---|
| **批准** | 必须按住 **800ms**，进度条走满才生效 | 误批准是安全事故 |
| **拒绝** | 单击即可 | 误拒绝只是重来一次 |
| **超时** | 45 秒无操作 → 回退到电脑弹窗 | 绝不自动放行 |

### 多个任务同时要审批

审批是**队列**，不是单槽。设备最多同时挂 3 个待审批请求（`MAX_APPROVALS`）：

- 屏幕一次只显示一个，**最早到期的优先**
- 第二行显示**任务名**，右上角 `共 3 个` 表示队列里还有几个
- 处理完一个，2 秒后自动显示下一个
- 每个请求有独立的 45 秒倒计时，超时的那个自己回退，不影响其他
- 队列满了再进来，会踢掉最早到期的那个（它反正快超时了）

任务名从 `~/.wb-status/sessions.json` 里按 `session_id` 查——和状态显示用的是同一份数据，所以屏幕上的名字和你看到的状态列表是一致的。

`/decision?req=xxx` 按**请求号**查询，所以每个阻塞的 hook 进程拿到的都是自己那个请求的结果，不会串。

### 原理

`PreToolUse` hook 可以往 stdout 写一行 JSON 来决定是否放行：

```json
{"hookSpecificOutput":{
  "hookEventName":"PreToolUse",
  "permissionDecision":"allow",
  "permissionDecisionReason":"在设备上批准"
}}
```

`permissionDecision` 三个值：`allow` / `deny` / `ask`。这个判定**优先级高于所有权限规则和 bypassPermissions**。

hook 阻塞轮询设备，最多 45 秒，拿到结果就返回。

### 设备端接口

| 接口 | 作用 |
|---|---|
| `GET /approve?req=&tool=&cmd=` | 推送审批请求，设备切到审批页 |
| `GET /decision?req=` | 返回 `pending` / `allow` / `deny` / `none` |

### 启用方式

把 `hook/approve-hook.py` 装到 `~/bin/`，然后在 `settings.json` 加：

```json
"PreToolUse": [{
  "matcher": "*",
  "hooks": [{ "type": "command",
    "command": "/usr/bin/env python3 $HOME/bin/approve-hook.py" }]
}]
```

### ⚠️ 安全边界

这不是便利功能，是安全边界。三条默认规则写死在脚本里：

1. **只对白名单工具开放**：`Bash` / `Write` / `Edit`
2. **高危命令永远不在设备审批**：`rm -rf`、`sudo`、`git push --force`、`npm publish`、`docker rm`、`kubectl delete` 等，命中就直接回电脑确认
3. **超时不放行**：45 秒没响应就不输出任何东西，走默认弹窗

另外脚本带**负缓存**：设备离线时写标记，60 秒内不再尝试，避免每次工具调用都卡 1 秒。

## 八、界面说明

| 屏幕 | 内容 |
|---|---|
| 主界面 | `N 个任务` + 列表。每行：左侧色条（状态色）、任务名、右侧状态词、下方 `工具 · 耗时` |
| WiFi 列表 | 最多 6 个网络，带锁标记 |
| 密码页 | 顶部显示 SSID，4 行键盘 + 功能键 |
| 消息页 | 连接中 / 成功 / 失败，带转圈动画 |

状态配色：确认=红、执行=蓝、思考=绿、完成=绿、出错=橙、空闲=灰。

### 电量显示

主界面右上角显示电池图标 + 百分比，每 30 秒刷新一次。

| 颜色 | 含义 |
|---|---|
| 灰 | 正常（26% 以上） |
| 橙 | 偏低（11–25%） |
| 红 | 危险（≤10%） |
| 绿 | 充电中 |

**充电中**电池框内不画电量条，改成暗绿底 + 白色闪电，和手机充电图标一个逻辑——一眼能看出「插着电」而不是「电量满」。

### 离线检测

顶栏按优先级显示：**WiFi 断开 > 离线 > 电量低 > N 个任务**。

| 状态 | 判定 | 显示 |
|---|---|---|
| WiFi 断开 | 连不上路由器 | 红色「WiFi 断开」，列表置灰 |
| 离线 | 90 秒（有心跳）/ 10 分钟（无心跳）无任何请求 | 橙色「离线」，列表置灰 |

**为什么要心跳**：WorkBuddy 空闲时本来就没有事件，设备无法区分「你在休息」和「Mac 挂了」。跑心跳脚本每 30 秒 ping 一次，90 秒收不到就能准确判离线：

```bash
./hook/heartbeat.sh
```

不跑也能用，只是阈值放宽到 10 分钟，且正常空闲可能被误判成离线。

新增接口：`GET /ping` → 返回 `pong`

### 夜间调光

**20:00–09:30** 自动把背光降到 30%，白天恢复。时间靠 NTP 同步（`ntp.aliyun.com`），连上 WiFi 后自动校准，不用手动设。

**夜间想看清怎么办**：碰一下屏幕就临时提亮到白天亮度，**20 秒不操作自动回落到 30%**。不需要进设置、不需要开关，符合「我要看的时候才亮」的直觉。

最终亮度取三者最小值：夜间基线 → 低电量上限 → 熄屏 0；触摸提亮会覆盖夜间基线，但仍受低电量限制。

### 双击熄屏

主界面**双击空白处**熄屏（背光归零 + 停止重绘），**点任意位置**亮屏。

熄屏不是关机——任务照常接收、审批照常排队，只是不画。适合晚上不想看屏幕但不想错过通知的场景。

原理图确认的三个引脚：

| 功能 | GPIO | 说明 |
|---|---|---|
| `BAT_ADC` | **1** | 电池电压采样（ADC1_CH0） |
| `BAT_EN` | 2 | 电池使能，必须拉高 |
| `CHG_STAT` | 3 | 充电状态，低电平 = 正在充 |

电压经分压电阻进 ADC，`BAT_DIV` 是分压比（默认 2.0）。百分比按**锂电池放电曲线查表**，不是线性映射——3.7V 标称其实还有 40%，线性算会显示成 45% 以下的假低电量。

**校准方法**：如果读数不准，用万用表量电池实际电压，然后

```
BAT_DIV = 实际电压 / 串口读到的电压 × 当前 BAT_DIV
```

串口每 30 秒会打印原始值：

```
[bat] 3.92V  72%  charging=0
```

### 低电量保护

背光是这块板最大的耗电项，所以按电量自动降亮度（`LCD_BL` 走 PWM）：

| 电量 | 背光 | 界面 |
|---|---|---|
| >20% | 100% | 正常 |
| 11–20% | 67% | 正常 |
| 6–10% | 39% | 电池图标变红 |
| ≤5% | 18% | 顶部显示「电量低 请充电」 |

**充电中不降亮度**，插上线立刻恢复 100%。

另外 `PA_CTRL`（GPIO7）在启动时拉低——功放常开既费电又有底噪，当前固件没用到音频，先关掉。

## 九、已知限制

1. **音频未接**。引脚已确认（I2S 8/9/10/11/12 + PA_CTRL 7），但播放会占 CPU，建议先跑通显示再加。
2. **审批默认不启用**。脚本写好了（`hook/approve-hook.py`），但需要手动加进 `settings.json` 并在 `/hooks` 面板确认。
3. **中文字体必须用 `u8g2_font_wqy12_t_gb2312`**（chinese1/2/3 只有 388/501 字，会掉字，见第十二节坑 2b）。当前用量 41%。
4. **扫描会短暂中断连接**。已连接状态下扫描，WiFi 会瞬断 1-2 秒，属正常。
5. **分压比 `BAT_DIV` 未校准**，默认 2.0，需按实测调整（见第八节）。

## 十、文件结构

```
firmware/
  wb_status_v3/
    wb_status_v3.ino   主循环、任务表、HTTP 接口
    config.h           引脚与常量（★ 需核对触摸引脚）
    net.h / net.cpp    WiFi 扫描、NVS 凭据、热点兜底
    ui.h  / ui.cpp     屏幕绘制、触摸、三屏状态机
    link.h / link.cpp  USB 串口通道（不依赖 WiFi）
  i2c_scan/
    i2c_scan.ino       引脚探测工具
```

## 十一、USB 串口通道（推荐主力）

公司 WiFi 常见三个拦路虎：只有 5GHz、Web Portal 认证、客户端隔离。任意一个都能让
「设备连上 WiFi 但 Mac 访问不到」。**插数据线走 USB 就全部绕开。**

### 怎么工作

```
WorkBuddy hook ──HTTP──> 127.0.0.1:18765 ──串口──> ESP32-S3 (CDC)
                            wb-bridge.py
```

hook 依次尝试：`WB_STATUS_URL` → USB 桥 → WiFi，谁先应答用谁。桥没在跑时 hook 会
**自动拉起**，所以插上线就生效，拔了线自动退回 WiFi，两边代码都不用改。

### 协议（`link.cpp`）

行协议，`|` 分隔，`\n` 结尾；回复统一以 `@` 开头，和调试日志区分：

| 命令 | 回复 |
|---|---|
| `PING` | `@PONG` |
| `STATE\|<sid>\|<st>\|<tool>\|<label>` | `@OK` |
| `APPROVE\|<req>\|<tool>\|<label>\|<cmd>` | `@OK` |
| `DEC\|<req>` | `@DEC\|<req>\|allow\|deny\|pending\|none` |
| `LIST` | `@LIST\|<json>` |

### 手动跑

```bash
python3 hook/wb-bridge.py            # 前台运行
python3 hook/wb-bridge.py --check    # 只探测设备在不在
```

端口可用 `WB_BRIDGE_PORT` 改（默认 18765，刻意避开常见的 8765）。

## 十二、踩过的坑（真机调试记录）

### 1. 烧录后板子不启动，串口一片安静

`arduino-cli upload` 结尾虽然打印了 `Hard resetting via RTS pin...`，但
**USB-Serial/JTAG 模式不吃 RTS 复位**，板子会停在下载模式。

```bash
ESPTOOL=~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool
$ESPTOOL --port /dev/cu.usbmodem21201 --after hard_reset chip_id
```

或者直接按一下板上的 RST 键。

### 2. 一画中文，主循环直接卡死

**真正的原因是 GfxBridge 嵌套了 SPI 事务**，和字体无关：

- **GfxBridge 嵌套了 SPI 事务**：`Adafruit_GFX::drawFastHLine()` 内部会
  `startWrite() → writePixel() → drawPixel()`，而 `Arduino_GFX::drawPixel()`
  自己又会 `startWrite()/endWrite()`。桥接类如果把 `startWrite` 也转发下去，
  就会嵌套 `beginWrite()`，把 CS/DC 状态机搞乱。
  **正确做法**：直接转发 `drawPixel` / `drawFastHLine` / `drawFastVLine`，
  并让 `startWrite` / `endWrite` 保持空实现。

### 2b. 中文字掉字（只画出「成」）

`u8g2_font_wqy12_t_chinese1/2/3` 看着像中文全集，其实只是小字库：

| 字体 | 大小 | 收字数 |
|---|---|---|
| `wqy12_t_chinese1` | 9.4KB | **388** |
| `wqy12_t_chinese2/3` | 13.5KB | **501** |
| `wqy12_t_gb2312` | 203KB | **7757** |

用 chinese1 时，「已完成」只画出「成」，「WiFi 断开」掉「断」，
任务名里随便一个常用字（析、画、操、候）就没了。

**用 `u8g2_font_wqy12_t_gb2312`**，Flash 从 1.09MB 回到 1.29MB（41%），
换来 7757 字覆盖。

顺带一个教训：调试时用 `head -N` 截断串口输出，很容易把「还在继续打印」
误判成「卡死了」。定位死循环要用心跳或者超时判断，不能只看前几行。

### 3. 串口能收不能发（或反过来）

- 打开 `/dev/cu.*` 后**不要**去拉 DTR/RTS。ESP32-S3 的 USB-Serial/JTAG 会把
  这两个信号当成自动复位序列，一拉就可能进下载模式。
- 原生 USB 是 `usbmodem`，CH343 桥是 `usbserial`，两者复位行为不同，别混用。

### 4. 屏幕每秒闪一下

主循环为了刷「已运行秒数」每秒整屏重绘一次，而 `drawMain()` 开头就是
`fillScreen()` —— 先黑一下再画回来，看起来就是闪。审批页更严重，100ms 重绘一次。

改成**离屏画布**：整帧先画进 PSRAM（240×240×2 = 115KB），画完一次 `flush()`
连续推给屏幕，中间不会出现黑屏。

```cpp
static Arduino_GFX* gfx = new PsramCanvas(SCREEN_W, SCREEN_H, panel);
...
gfx->flush();     // 每帧画完调一次
```

`PsramCanvas` 是 `Arduino_Canvas` 的子类，只是把帧缓冲改成优先从 PSRAM 分配
（`heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)`），失败时退回内部 RAM。
启动日志会打印落在哪：

```
[ui] 画布 PSRAM @0x3c1009e0  内部堆 300468
```

注意：`onTap()` 里有几处直接调 `drawMsg()` 后要阻塞（扫描 / 连接），
这些地方必须手动补 `gfx->flush()`，否则那两秒屏幕是空的。

### 5. 端口冲突

macOS 上 `8765` 常被别的服务占用，所以桥接默认用 `18765`。

## 十三、电池读数异常排查

实测读到 `2.80V / 0%`，但插着 USB 且 `charging=1`。这个组合不正常，按顺序查：

1. **电池是否真的接上**了（排线有没有插到底）
2. `BAT_DIV` 是否合适：用万用表量电池正极电压，`BAT_DIV = 实测电压 / ADC 读数`
3. `BAT_EN`（GPIO2）电平是否需要反过来
4. `PIN_BAT_ADC`（GPIO1）是否被别的功能复用

空载或没接电池时，分压点会浮在一个中间电压，读出来就是这样。

## 十四、同步真实任务（预览用）

`hook/sync-tasks.py` 会扫 `~/.workbuddy-ai/projects/*/*.jsonl`，把最近的会话
推到设备上，用来快速看效果：

```bash
python3 hook/sync-tasks.py --dry-run   # 只打印解析结果
python3 hook/sync-tasks.py             # 推到设备
```

解析规则：

| 字段 | 来源 |
|---|---|
| 任务名 | 会话第一条用户消息（去掉 `system-reminder`、附件标记，截 10 汉字 / 20 字符） |
| 状态 | 最后一条消息：`user` → thinking；`assistant` 带 `tool_use` → working；否则 done |
| 工具 | 最后一条 assistant 消息里的 `tool_use.name` |

两个注意点：

1. **推之前必须先清空设备**。设备的任务表满了会按优先级淘汰，旧的高优先级任务
   会把新推的「已完成」挤掉，结果屏幕上只剩一两个。脚本里已经先发一轮 `st=off`。
2. **快照会过期**。设备 5 分钟收不到更新就自动移除任务（`ST_CONFIRM` 是 30 分钟），
   所以这只是一次性预览。要持续更新得挂 `wb-status-hook.py`。

实时更新的链路是：

```
UserPromptSubmit / PreToolUse / Stop ...
        ↓  wb-status-hook.py
        ↓  （USB 桥 → WiFi 自动选路）
      ESP32 屏幕
```
