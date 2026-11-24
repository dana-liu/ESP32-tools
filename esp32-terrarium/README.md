# esp32-terrarium · 掌心生态瓶

运行在微雪 ESP32-S3-Touch-LCD-1.54 的 240×240 互动生态瓶。右上角显示电量，底部显示 Codex 额度。屏幕上的 `W` 是生态瓶的虚拟水位。

## 实机操作

| 操作 | 效果 |
| --- | --- |
| 左右滑动 | 在生态瓶和 Codex 额度页之间切换 |
| 轻点屏幕 | 下雨并增加水位，约 3 秒后停止生成雨滴 |
| 长按屏幕约半秒 | 切到晴天并降低水位 |
| 短按左键 | 天数减一天，最低 DAY 1 |
| 短按中间键 | 熄屏或亮屏 |
| 短按原音量加键 | 推进一天 |
| 长按原音量加键约两秒 | 在屏幕上显示本次蓝牙更新码 |

每日 20:00 到次日 09:30 自动熄屏，夜间按中间键或轻点可临时亮屏 30 秒。熄屏会关闭背光和液晶显示并暂停刷新，蓝牙继续工作。Mac 的 BLE 同步会发送时间和时区，设备完全断电后需要重新校时。

小鱼会随机冲刺、掉头和吐泡泡。天数、水位、天气与苔藓进度保存在 NVS。浏览器可打开 `simulator/index.html` 体验早期互动原型，实机为准。

## 首次编译和 USB 烧录

需要 Arduino CLI、ESP32 Arduino core 3.3.12 和 Arduino_GFX 1.6.9。开发板使用 16 MB Flash、OPI PSRAM 和 `app3M_fat9M_16MB` 分区。

```bash
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install 'GFX Library for Arduino@1.6.9'
ls /dev/cu.usbmodem*
bash flash.sh /dev/cu.usbmodemXXXX
```

把示例中的串口替换成实际设备。串口被推送器或监视器占用时，先结束对应程序。如果哈希校验成功、只在最后复位时出错，可重新上电并查看屏幕。运行后可用 115200 波特率查看 `TERRARIUM RUN`。

## Mac 蓝牙同步与运行日志

需要 Python 3.11 或更新版本、`bleak` 和已登录的 Codex CLI。首次连接时按 macOS 提示允许蓝牙访问。

```bash
python3 -m venv .venv
.venv/bin/python -m pip install bleak
.venv/bin/python terrarium-ble.py sync
```

- `sync` 每分钟同步一次，`sync --once` 只同步一次。
- `status` 读取运行分区、编译时间、电量、水位、天气、额度和 OTA 状态。加 `--watch` 每两秒输出一次，按 Ctrl+C 结束。监看期间，常驻同步会在下一次周期重试。
- USB 同步可用 `python3 sync-quota.py /dev/cu.usbmodemXXXX`，适用于没有启用蓝牙的情况。

额度页顶部显示时间、日期与星期，两组额度各自显示剩余比例和本地重置时间。

Mac 调用 Codex App Server 的 `account/rateLimits/read`，只向板子发送额度窗口长度、剩余百分比和重置时间。Codex 登录凭据留在 Mac。收到额度后约 150 秒没有更新，屏幕会显示 `--`。

## 蓝牙更新

先编译，再在板子上长按原音量加键，等六位更新码出现后运行：

```bash
bash flash.sh --build-only build
.venv/bin/python terrarium-ble.py ota build/esp32-terrarium.ino.bin --code 123456
```

将示例数字替换成本次更新码。更新窗口在 60 秒内接受启动请求，传输开始后可持续到整包写完。Mac 经 BLE 分批发送应用镜像，ESP32 写入备用分区并验证镜像，然后切换分区重启。脚本会重连并核对分区是否切换。传输中断时可以重新开启更新窗口再试。

OTA 只更新应用固件。启动日志、断点调试、修改分区和故障恢复继续用 USB。

## 代码结构

- `firmware/esp32-terrarium/esp32-terrarium.ino` 绘制、触摸、电量及串口同步。
- `firmware/esp32-terrarium/TerrariumBle.h` 加密 BLE 特征、数据接收、状态和备用分区更新。
- `terrarium-ble.py` Mac 蓝牙同步、状态监看和无线更新。
- `sync-quota.py` Codex App Server 额度读取与 USB 同步。

## Gemini 额度页

左右滑动可在生态瓶、Codex 和 Gemini 之间切换。Gemini 页显示 Antigravity 的 Gemini Models 5 小时额度和周额度。重置时间根据面板倒计时换算，精度随面板显示而定。

在 Antigravity 打开 Settings → Models → Models & Usage，并保持面板打开。首次使用可能需要在 macOS「隐私与安全性 → 辅助功能」允许同步进程读取该面板。编译读取程序并启动同步：

```bash
mkdir -p build
swiftc antigravity-quota.swift -o build/antigravity-quota
.venv/bin/python terrarium-ble.py sync --gemini-reader ./build/antigravity-quota
```

面板关闭或暂时不可读时，Gemini 页的上次结果约 150 秒后显示 `--`。如果面板数据未刷新，可点击面板的 Refresh quota and credits data。
