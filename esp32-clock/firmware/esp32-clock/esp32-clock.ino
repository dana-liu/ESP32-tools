// ESP32-S3-Touch-LCD-1.54 时钟 v2.1 (暖白桌面版)
// 界面: 日期状态栏 + 大字时间 + 次要秒数 + 天气 + 日程
//       长按主屏切换极简模式; 提前提醒弹窗 / 今天明天日程列表
// 数据: 保留桥接服务、USB 串口协议、NVS WiFi 与原提醒状态机
// 渲染: PSRAM 离屏画布, 秒级仅传输变化的横向区域
// USB / WIFI / DEMO 表示数据来源, 不冒充电量或充电状态
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <sys/time.h>
#include "secrets.h"
#include "audio.h"

#define GFX_BL 46
#define BAT_EN 2
#define TP_SDA 42
#define TP_SCL 41
#define TP_RST 47          // CST816 复位脚，不复位的话读出来全是 0

// 板载按键 —— 实测映射（用 hook/key-scan.py 逐个按出来的）
//   GPIO4 = 音量+    GPIO0 = 音量-    GPIO5 = 第三个键（当静音）
// 注意 GPIO0 是 strapping 脚：开机瞬间按住它会进下载模式，正常使用时无影响。
#define KEY_VOL_UP   4
#define KEY_VOL_DOWN 0
#define KEY_MUTE     5

static Arduino_DataBus* bus   = new Arduino_ESP32SPI(45, 21, 38, 39, -1);
static Arduino_GFX*     panel = new Arduino_ST7789(bus, 40, 0, true, 240, 240);

// 离屏画布: 整帧画进 PSRAM, 画完一次 flush
class PsramCanvas : public Arduino_Canvas {
public:
  PsramCanvas(int16_t w, int16_t h, Arduino_G* out) : Arduino_Canvas(w, h, out) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (!_framebuffer) {
      size_t s = (size_t)_width * _height * 2;
      _framebuffer = (uint16_t*)heap_caps_aligned_alloc(
          16, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return Arduino_Canvas::begin(speed);
  }
  // 画布与面板均使用 rotation=0; 全宽区域在 RGB565 缓冲区内连续。
  void flushBand(int16_t y, int16_t h) {
    if (!_framebuffer || !_output || y < 0 || h <= 0 || y + h > _height) return;
    _output->draw16bitRGBBitmap(_output_x, _output_y + y,
                               _framebuffer + (size_t)y * _width, _width, h);
  }
};

static PsramCanvas* gfx = new PsramCanvas(240, 240, panel);

// USB 拔掉后 Serial 写会阻塞（HWCDC 的 TX 缓冲满了就卡住），
// 主循环里的日志一律走这个宏，没连接就直接丢掉。
#define DBG(...) do { if (Serial) Serial.printf(__VA_ARGS__); } while (0)

static constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// 暖白底 + 深灰文字 + 陶橙点缀 (RGB565)
const uint16_t C_BG      = C(247, 245, 239);
const uint16_t C_CARD    = C(239, 236, 227);
const uint16_t C_LINE    = C(222, 219, 211);
const uint16_t C_TEXT    = C(44, 44, 42);
const uint16_t C_MUTED   = C(95, 94, 90);
const uint16_t C_ACCENT  = C(153, 60, 29);
const uint16_t C_ACCENTL = C(250, 236, 231);
const uint16_t C_ORANGE  = C(216, 90, 48);
const uint16_t C_GREEN   = C(15, 110, 86);

#define MAX_EVENTS 8
struct EventItem {
  char date[6];     // MM.DD
  char time[8];     // HH:MM 或 UTF-8 "全天" (6 字节 + 结束符)
  char title[48];
  char src[8];      // 钉钉 / 本地 / USB
  int minutesUntil;
  bool allDay;
};
EventItem events[MAX_EVENTS];
int eventCount = 0;
int cardIndex = 0;
int listPage  = 0;          // 日程列表当前页
#define LIST_ROWS 5

enum Screen { SC_MAIN, SC_LIST, SC_POPUP };
Screen screen = SC_MAIN;
int popupIdx = 0;

// 声音
int  volumePct = 60;
bool soundOn   = true;
int  popupBeeps = 0;          // 本次弹窗已经响过几次
unsigned long popupSince = 0; // 弹窗出现时刻，用于兜底自动关闭
unsigned long volToastUntil = 0;   // 音量浮层消失时刻
#define VOL_TOAST_MS 1000
unsigned long lastBeepMs = 0;
#define POPUP_BEEP_GAP 30000  // 未处理时每 30 秒再响一次
#define POPUP_BEEP_MAX 3      // 最多响 3 次

// 未来 3 天的「日期/周几/农历/天气」，USB 断开后靠它撑过去
// 缓冲区按「最长的中文文案」留足空间：天气文案最长 "晴间多云"=12 字节，
// 农历最长 "乙巳蛇年 · 十一月廿四"=31 字节。
#define MAX_DAYS 3
struct DayInfo {
  char date[8];      // 11-03
  char week[16];     // 周二
  char lunar[48];
  int  temp;
  char text[24];
  char icon[16];
  char aqi[24];
};
DayInfo days[MAX_DAYS];
int dayCount = 0;
char tempDate[8] = "";     // tempVal 是哪一天的实时温度
char weekText[16] = "";    // 当前日期对应的周几（来自推送，优先于本地推算）

char lunarText[48] = "";
int  tempVal = -999;
char weatherText[24] = "";
char weatherIcon[16] = "";
char aqiText[24] = "";
char cityText[32] = "";

char ackKey[16] = "";      // 已确认的日程 "MM.DDHH:MM"
char snoozeKey[16] = "";   // 选择"到点再叫我"的日程
bool snoozeDone = false;

bool demoMode = true;
time_t baseEpoch = 0;
unsigned long baseMillis = 0;
unsigned long lastSyncMs = 0;
int lastSec = -1;
int renderedMinute = -1;
int renderedHour = -1;
bool minimalMode = false;
bool forceRender = true;
bool linkOnline = false;
unsigned long lastLinkMs = 0;

// ---------------- 时间 ----------------
time_t nowEpoch() {
  time_t t = time(nullptr);
  if (t > 1760000000) return t;
  return baseEpoch + (millis() - baseMillis) / 1000;
}

// ---------------- 文本工具 ----------------
void textCenter(const char* s, int yBaseline, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((240 - (int)w) / 2 - x1, yBaseline);
  gfx->print(s);
}

// 以 cx 为中心写字(按钮用)，定义在下方
void textCenterAt(const char* s, int cx, int yBaseline, uint16_t color);
void drawVolumeToast();          // 音量浮层，定义在渲染段之前

const char* weekCN(int wday) {
  static const char* d[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  return d[wday % 7];
}

// 当前字体的实际像素宽度; 中文必须配合 setUTF8Print(true)。
int textWidth(const char* s) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// 不拆开 UTF-8 字符, 同时忽略上游固定长度缓冲区截断的末尾字符。
size_t utf8CharBytes(const char* s) {
  uint8_t c = (uint8_t)s[0];
  size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0 ? 2 :
             (c >= 0xE0 && c < 0xF0 ? 3 : (c >= 0xF0 && c < 0xF5 ? 4 : 0)));
  for (size_t i = 1; i < n; i++) {
    if (!s[i] || ((uint8_t)s[i] & 0xC0) != 0x80) return 0;
  }
  return n;
}

// 安全拷贝: 按 UTF-8 字符边界截断, 绝不把一个汉字劈成两半。
// strlcpy 是按字节截的, 遇到中文会切出半个字符, 屏幕上就是乱码。
void copyUtf8(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  size_t used = 0;
  while (*src) {
    size_t n = utf8CharBytes(src);
    if (!n) { src++; continue; }                 // 跳过非法字节
    if (used + n + 1 > cap) break;                // 放不下整个字符就停
    memcpy(dst + used, src, n);
    used += n;
    src += n;
  }
  dst[used] = 0;
}

void fitText(const char* s, char* out, size_t capacity, int maxWidth) {
  if (!capacity) return;
  out[0] = 0;
  size_t used = 0;
  bool truncated = false;
  while (*s) {
    size_t n = utf8CharBytes(s);
    if (!n || used + n + 4 > capacity) { truncated = true; break; }
    memcpy(out + used, s, n);
    out[used + n] = 0;
    // 给省略号留位置, 但完整字符串恰好装得下时不加省略号。
    if (textWidth(out) > maxWidth) { out[used] = 0; truncated = true; break; }
    used += n; s += n;
  }
  if (truncated) {
    while (used) {
      strlcpy(out + used, "...", capacity - used);
      if (textWidth(out) <= maxWidth) return;
      do { used--; } while (used && ((uint8_t)out[used] & 0xC0) == 0x80);
      out[used] = 0;
    }
    strlcpy(out, textWidth("...") <= maxWidth ? "..." : "", capacity);
  }
}

void drawFitText(const char* s, int x, int baseline, int maxWidth, uint16_t color) {
  char fitted[128];
  fitText(s, fitted, sizeof(fitted), maxWidth);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(fitted, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(x - x1, baseline);
  gfx->print(fitted);
}

void drawWrappedText(const char* s, int baseline, int lineHeight, uint16_t color) {
  // 弹窗标题最多两行, 最后一行溢出才省略。
  char line[128] = "";
  size_t used = 0;
  while (*s) {
    size_t n = utf8CharBytes(s);
    if (!n || used + n + 1 >= sizeof(line)) break;
    memcpy(line + used, s, n); line[used + n] = 0;
    if (textWidth(line) > 208) { line[used] = 0; break; }
    used += n; s += n;
  }
  textCenter(line, baseline, color);
  if (*s) {
    fitText(s, line, sizeof(line), 208);
    textCenter(line, baseline + lineHeight, color);
  }
}

void textRight(const char* s, int right, int baseline, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(right - (int)w - x1, baseline);
  gfx->print(s);
}

void toggleClockMode() {
  minimalMode = !minimalMode;
  Preferences p;
  if (p.begin("clockui", false)) {
    p.putBool("minimal", minimalMode);
    p.end();
  }
  forceRender = true;
}

// ---------------- 音量 / 提示音 ----------------
void saveAudioPrefs() {
  Preferences p;
  if (p.begin("clockui", false)) {
    p.putInt("vol", volumePct);
    p.putBool("snd", soundOn);
    p.end();
  }
}

void applyVolume(int v) {
  volumePct = v < 0 ? 0 : (v > 100 ? 100 : v);
  audioSetVolume(soundOn ? volumePct : 0);
  saveAudioPrefs();
  volToastUntil = millis() + VOL_TOAST_MS;   // 顶部浮层显示 1 秒
  drawVolumeToast();                         // 立刻显示，不等下一帧
  gfx->flushBand(0, 48);
  // 调完放一声「试听音」：频率/幅度与提醒音一致，听到的响度就是提醒的响度，
  // 比 35ms 的短促声好判断。音量 0 或静音时自动不出声。
  // 放在 applyVolume 里，按键和串口 VOL| 两个入口都能听到。
  if (soundOn) audioPreviewVolume();
}

void setSoundOn(bool on) {
  soundOn = on;
  audioSetVolume(soundOn ? volumePct : 0);
  saveAudioPrefs();
  volToastUntil = millis() + VOL_TOAST_MS;
  drawVolumeToast();
  gfx->flushBand(0, 48);
}

// 弹窗出现时响一次，之后每 30 秒补一次，最多 3 次
void ringPopup() {
  popupBeeps = 1;
  lastBeepMs = millis();
  if (soundOn) audioChime();
}

void stopPopupRing() { popupBeeps = 0; }

// ---------------- 背光：夜间调光 + 熄屏 ----------------
// GPIO46 接背光，走 PWM。规则：
//   1. 20:00~次日 09:00 默认调暗（夜里当小夜灯刚好）；
//      这段时间内碰到屏幕或按任意键，恢复全亮 30 秒，到期自动暗回去
//   2. 长按静音键(GPIO5) 0.8 秒熄屏；熄屏时触摸或按任意键点亮
#define BL_FREQ             5000    // 背光 PWM 频率
#define BL_RES              8       // 8bit: 0..255
#define BL_DAY              255     // 平时全亮
#define BL_NIGHT            30      // 夜间亮度（约 12%），嫌暗/嫌亮改这里
#define BL_BOOST_MS         30000UL // 夜间触碰后恢复正常的时长
#define SCREEN_OFF_HOLD_MS  800UL   // 静音键长按多久算「熄屏」

bool blPwmOk   = false;             // ledcAttach 失败就退回直连全亮
int  blForce   = -1;                // -1=自动；0..255 由串口 BL| 固定（调试用）
int  blLevel   = -1;                // 当前输出亮度，变了才写 LEDC
bool screenOff = false;             // 熄屏中（触摸/任意键点亮）
unsigned long blBoostUntil = 0;     // 夜间临时提亮的截止时刻

// 20:00 到次日 09:00 之间算夜间。用设备自己的 RTC（USB 对时/NTP/演示时钟都行），
// 不联网也能正确切换。
bool isNightNow() {
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);
  int hm = tm.tm_hour * 100 + tm.tm_min;
  return hm >= 2000 || hm < 900;
}

void applyBrightness() {
  if (!blPwmOk) return;
  int want;
  if (blForce >= 0) {
    want = blForce;                 // 串口调试强制值优先
  } else if (screenOff) {
    want = 0;
  } else if (isNightNow() && millis() >= blBoostUntil) {
    want = BL_NIGHT;                // 夜间且 30 秒提亮已过期
  } else {
    want = BL_DAY;
  }
  if (want != blLevel) {
    blLevel = want;
    ledcWrite(GFX_BL, blLevel);
    DBG("[bl] %d%%\n", blLevel * 100 / 255);
  }
}

// 夜间被碰了一下：提亮 30 秒。白天调它没有副作用（本来就全亮）。
void boostBacklight() {
  blBoostUntil = millis() + BL_BOOST_MS;
  applyBrightness();
}

// 熄屏 -> 点亮。点亮同时按夜间规则提亮 30 秒。
void wakeScreen() {
  if (screenOff) {
    screenOff = false;
    DBG("[bl] 点亮\n");
  }
  boostBacklight();
}

// ---------------- 天气图标 ----------------
void drawWeatherIcon(int cx, int cy, const char* icon) {
  if (!strcmp(icon, "rain") || !strcmp(icon, "snow") || !strcmp(icon, "thunder") ||
      !strcmp(icon, "cloud")) {
    gfx->fillCircle(cx - 5, cy - 2, 5, C_MUTED);
    gfx->fillCircle(cx + 4, cy - 2, 6, C_MUTED);
    gfx->fillRect(cx - 8, cy - 2, 18, 5, C_MUTED);
    if (!strcmp(icon, "rain")) {
      for (int i = 0; i < 3; i++) gfx->drawLine(cx - 6 + i * 6, cy + 6, cx - 8 + i * 6, cy + 10, C_ORANGE);
    } else if (!strcmp(icon, "snow")) {
      for (int i = 0; i < 3; i++) gfx->fillCircle(cx - 6 + i * 6, cy + 8, 1, C_MUTED);
    } else if (!strcmp(icon, "thunder")) {
      gfx->drawLine(cx, cy + 5, cx + 3, cy + 9, C_ORANGE);
      gfx->drawLine(cx + 3, cy + 9, cx - 2, cy + 9, C_ORANGE);
      gfx->drawLine(cx - 2, cy + 9, cx + 1, cy + 12, C_ORANGE);
    }
  } else if (!strcmp(icon, "fog")) {
    for (int i = 0; i < 3; i++) gfx->drawLine(cx - 8, cy - 4 + i * 4, cx + 8, cy - 4 + i * 4, C_MUTED);
  } else {
    gfx->drawCircle(cx, cy, 5, C_ORANGE);
    for (int i = 0; i < 8; i++) {
      float a = i * 45 * DEG_TO_RAD;
      gfx->drawLine(cx + cos(a) * 8, cy + sin(a) * 8, cx + cos(a) * 11, cy + sin(a) * 11, C_ORANGE);
    }
  }
}

// ---------------- 日程卡片 ----------------
void drawBigCard(int y, int h, int idx, bool upcoming) {
  if (idx < 0 || idx >= eventCount) return;
  EventItem* e = &events[idx];
  gfx->fillRoundRect(16, y, 208, h, 7, upcoming ? C_ACCENTL : C_CARD);
  gfx->fillRect(16, y + 6, 2, h - 12, upcoming ? C_ORANGE : C_MUTED);
  char buf[64];
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  snprintf(buf, sizeof(buf), "%s%s%s", e->time, e->src[0] ? " · " : "", e->src);
  drawFitText(buf, 26, y + 14, 112, C_ACCENT);
  if (e->allDay) strlcpy(buf, "全天", sizeof(buf));
  else if (e->minutesUntil > 0) snprintf(buf, sizeof(buf), "%d 分钟后", e->minutesUntil);
  else strlcpy(buf, "已开始", sizeof(buf));
  char status[64];
  fitText(buf, status, sizeof(status), 76);
  textRight(status, 215, y + 14, C_ACCENT);
  drawFitText(e->title, 26, y + 31, eventCount > 1 ? 164 : 189, C_TEXT);
  if (eventCount > 1) {
    gfx->setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "%d/%d", idx + 1, eventCount);
    textRight(buf, 216, y + 31, C_MUTED);
  }
}

// ---------------- 主屏 ----------------
void drawClockTime(struct tm* t) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  gfx->setFont(u8g2_font_logisoso78_tn);
  // 使用本地字体的真实字宽, 不拉伸字形; 最宽时间也不越过边距。
  if (textWidth(buf) > 208) gfx->setFont(u8g2_font_logisoso62_tn);
  textCenter(buf, minimalMode ? 125 : 122, C_TEXT);
}

void drawClockSeconds(struct tm* t) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d 秒", t->tm_sec);
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  if (minimalMode) {
    gfx->fillRect(24, 150, 192, 34, C_BG);
    gfx->fillRoundRect(24, 150, 192, 2, 1, C_LINE);
    int progress = 192 * t->tm_sec / 60;
    if (progress) gfx->fillRect(24, 150, progress, 2, C_ORANGE);
    textCenter(buf, 178, C_ACCENT);
  } else {
    gfx->fillRect(176, 130, 48, 18, C_BG);
    gfx->fillCircle(181, 139, 2, C_ORANGE);
    textRight(buf, 224, 143, C_ACCENT);
  }
}

void drawClockHeader(struct tm* t) {
  char buf[40];
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  // 周几优先用 Mac 推来的值（跟着农历一起下来的），推不到再按本地时钟算。
  const char* wk = weekText[0] ? weekText : weekCN(t->tm_wday);
  snprintf(buf, sizeof(buf), "%02d月%02d日  %s", t->tm_mon + 1, t->tm_mday, wk);
  drawFitText(buf, 16, 27, 150, C_TEXT);
  // 串口连通不代表在充电; 只画代码实际能够确认的数据来源。
  const char* src = linkOnline ? "USB" : (demoMode ? "DEMO" :
                    (WiFi.status() == WL_CONNECTED ? "WIFI" : "LOCAL"));
  uint16_t color = linkOnline ? C_ACCENT : (demoMode ? C_MUTED : C_GREEN);
  gfx->fillRoundRect(180, 14, 44, 19, 5, C_CARD);
  gfx->setFont(u8g2_font_6x10_tf);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(src, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(202 - (int)w / 2 - x1, 27);
  gfx->setTextColor(color); gfx->print(src);
  if (!minimalMode) gfx->drawFastHLine(16, 40, 208, C_LINE);
}

void drawMainWeather() {
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  if (tempVal <= -900) {
    // 未获得天气时不默认画太阳, 也不显示伪造的温度。
    gfx->drawCircle(29, 174, 9, C_LINE);
    drawFitText("天气暂未同步", 49, 178, 175, C_MUTED);
    drawFitText("等待天气数据", 49, 195, 175, C_MUTED);
    return;
  }
  drawWeatherIcon(29, 173, weatherIcon[0] ? weatherIcon : "cloud");
  char buf[64];
  snprintf(buf, sizeof(buf), "%d", tempVal);
  gfx->setFont(u8g2_font_logisoso18_tn);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(49, 181); gfx->print(buf);
  int temperatureWidth = textWidth(buf);
  gfx->drawCircle(49 + temperatureWidth + 4, 165, 2, C_TEXT);
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  int conditionX = 49 + temperatureWidth + 15;
  drawFitText(weatherText, conditionX, 178, 224 - conditionX, C_TEXT);
  snprintf(buf, sizeof(buf), "%s%s%s", cityText, cityText[0] && aqiText[0] ? " · " : "", aqiText);
  drawFitText(buf, 49, 195, 175, C_MUTED);
}

void drawMain(struct tm* t) {
  gfx->fillScreen(C_BG);
  drawClockHeader(t);
  drawClockTime(t);
  drawClockSeconds(t);
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  if (minimalMode) {
    char fitted[64];
    fitText(lunarText[0] ? lunarText : "北京时间", fitted, sizeof(fitted), 208);
    textCenter(fitted, 216, C_MUTED);
    return;
  }
  drawFitText(lunarText[0] ? lunarText : "北京时间", 16, 143, 152, C_MUTED);
  gfx->drawFastHLine(16, 153, 208, C_LINE);
  drawMainWeather();
  if (eventCount > 0) {
    if (cardIndex < 0 || cardIndex >= eventCount) cardIndex = 0;
    drawBigCard(201, 38, cardIndex, !events[cardIndex].allDay && events[cardIndex].minutesUntil > 0);
  } else {
    gfx->drawFastHLine(16, 208, 208, C_LINE);
    gfx->setFont(u8g2_font_wqy12_t_gb2312);
    drawFitText("今日暂无日程", 16, 231, 182, C_MUTED);
    gfx->drawLine(216, 222, 220, 226, C_MUTED);
    gfx->drawLine(220, 226, 216, 230, C_MUTED);
  }
}

// ---------------- 提醒弹窗 ----------------
void textCenterAt(const char* s, int cx, int yBaseline, uint16_t color);

void drawPopup(struct tm* t) {
  gfx->fillScreen(C_BG);
  char buf[64];
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  drawFitText("日程提醒", 16, 27, 140, C_ACCENT);
  gfx->setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  textRight(buf, 224, 27, C_MUTED);
  gfx->drawFastHLine(16, 40, 208, C_LINE);

  gfx->fillCircle(120, 65, 18, C_ACCENTL);
  gfx->fillCircle(120, 60, 7, C_ORANGE);
  gfx->fillRect(113, 60, 14, 12, C_ORANGE);
  gfx->fillRoundRect(110, 71, 20, 3, 1, C_ORANGE);
  gfx->fillCircle(120, 78, 2, C_ORANGE);

  int mins = events[popupIdx].minutesUntil;
  gfx->setFont(u8g2_font_wqy16_t_gb2312);
  if (mins > 0) snprintf(buf, sizeof(buf), "还有 %d 分钟", mins);
  else strlcpy(buf, "日程开始了", sizeof(buf));
  textCenter(buf, 106, C_TEXT);
  drawWrappedText(events[popupIdx].title, 130, 18, C_TEXT);

  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  snprintf(buf, sizeof(buf), "%s%s%s", events[popupIdx].time,
           events[popupIdx].src[0] ? " · " : "", events[popupIdx].src);
  textCenter(buf, 171, C_MUTED);

  // 按钮坐标与下方触摸命中区域完全一致, 各 40px 高。
  gfx->fillRoundRect(16, 188, 96, 40, 8, C_ACCENT);
  textCenterAt("知道了", 64, 212, C_BG);
  gfx->fillRoundRect(124, 188, 100, 40, 8, C_CARD);
  gfx->drawRoundRect(124, 188, 100, 40, 8, C_LINE);
  textCenterAt("到点再叫我", 174, 212, C_TEXT);
}

// 以 cx 为中心写字(按钮用)
void textCenterAt(const char* s, int cx, int yBaseline, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(cx - (int)w / 2 - x1, yBaseline);
  gfx->print(s);
}

// ---------------- 日程列表 ----------------
// 单行卡片：左边标题、右边时间；一页 4 条，上下滑动翻页
void drawListRow(int y, int idx, bool first) {
  EventItem* e = &events[idx];
  gfx->fillRoundRect(16, y, 208, 26, 6, first ? C_ACCENTL : C_CARD);
  gfx->fillRect(16, y + 4, 2, 18, first ? C_ORANGE : C_LINE);

  // 每一行都带日期，格式 月-日（11-03），后面跟时间
  char when[40];
  char mmdd[8];
  strlcpy(mmdd, e->date, sizeof(mmdd));        // 内部存的是 "11.03"
  if (strlen(mmdd) == 5 && mmdd[2] == '.') mmdd[2] = '-';
  snprintf(when, sizeof(when), "%s %s", mmdd, e->time);

  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  int whenW = textWidth(when);
  int titleMax = 216 - 26 - whenW - 8;
  if (titleMax < 56) titleMax = 56;
  drawFitText(e->title, 26, y + 17, titleMax, C_TEXT);
  textRight(when, 216, y + 17, C_MUTED);
}

void drawList(struct tm* t) {
  gfx->fillScreen(C_BG);

  // 按 日期+时间 排序的索引（插入排序，最多 8 条）
  int order[MAX_EVENTS];
  int n = 0;
  for (int i = 0; i < eventCount; i++) order[n++] = i;
  for (int i = 1; i < n; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0) {
      EventItem& a = events[order[j]];
      EventItem& b = events[k];
      bool later = (strcmp(a.date, b.date) > 0) ||
                   (strcmp(a.date, b.date) == 0 && strcmp(a.time, b.time) > 0);
      if (!later) break;
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }

  int pages = n ? (n + LIST_ROWS - 1) / LIST_ROWS : 1;
  if (listPage < 0) listPage = 0;
  if (listPage > pages - 1) listPage = pages - 1;

  gfx->setFont(u8g2_font_wqy16_t_gb2312);
  drawFitText("日程", 16, 27, 80, C_TEXT);
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  char buf[32];
  if (n) snprintf(buf, sizeof(buf), "%d/%d 页 · %d 条", listPage + 1, pages, n);
  else   strlcpy(buf, "共 0 条", sizeof(buf));
  textRight(buf, 224, 26, C_MUTED);
  gfx->drawFastHLine(16, 37, 208, C_LINE);

  if (n == 0) {
    drawFitText("今天和明天都没有日程", 16, 104, 208, C_MUTED);
  } else {
    int start = listPage * LIST_ROWS;
    for (int r = 0; r < LIST_ROWS && start + r < n; r++) {
      int idx = order[start + r];
      drawListRow(56 + r * 28, idx, listPage == 0 && r == 0);
    }
  }

  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  textCenter(pages > 1 ? "上下滑动翻页 · 点按返回" : "点按返回", 234, C_MUTED);
}

// ---------------- 音量浮层 ----------------
// 顶部深色胶囊 + 进度条，1 秒后自动消失；不切屏、不挡主要内容
void drawVolumeToast() {
  const int w = 132, h = 40, x = (240 - w) / 2, y = 6;
  gfx->fillRoundRect(x, y, w, h, 12, C_TEXT);

  char buf[16];
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  if (!soundOn)                strlcpy(buf, "已静音", sizeof(buf));
  else if (volumePct == 0)     strlcpy(buf, "音量 0%", sizeof(buf));
  else                         snprintf(buf, sizeof(buf), "音量 %d%%", volumePct);
  textCenterAt(buf, 120, y + 20, C_BG);

  int barX = x + 14, barW = w - 28;
  gfx->fillRoundRect(barX, y + 27, barW, 4, 2, C_MUTED);
  int fill = soundOn ? barW * volumePct / 100 : 0;
  if (fill > 0) gfx->fillRoundRect(barX, y + 27, fill < 4 ? 4 : fill, 4, 2, C_BG);
}

// 按当前日期挑出对应的那一天，把农历/天气填进全局变量
void applyDayFor(struct tm* t) {
  char key[8];
  snprintf(key, sizeof(key), "%02d-%02d", t->tm_mon + 1, t->tm_mday);
  for (int i = 0; i < dayCount; i++) {
    if (strcmp(days[i].date, key)) continue;
    // tempDate 是「实时天气所属的那一天」。当天沿用实时温度/文案，
    // 之后的几天用预报的最高温和天气文案。
    bool liveDay = (strcmp(days[i].date, tempDate) == 0);
    if (days[i].lunar[0]) copyUtf8(lunarText, sizeof(lunarText), days[i].lunar);
    if (!liveDay) {
      tempVal = days[i].temp;
      if (days[i].text[0]) copyUtf8(weatherText, sizeof(weatherText), days[i].text);
      if (days[i].icon[0]) copyUtf8(weatherIcon, sizeof(weatherIcon), days[i].icon);
    }
    if (days[i].aqi[0])  copyUtf8(aqiText, sizeof(aqiText), days[i].aqi);
    if (days[i].week[0]) copyUtf8(weekText, sizeof(weekText), days[i].week);
    return;
  }
  // 三天缓存里没有这一天（离线超过 3 天）：周几退回本地推算，农历清空，
  // 都别显示过期的 —— 农历和日期是绑死的，留着就是错的。
  // 注意：dayCount == 0 表示压根没有三天缓存（只推了 LUNAR|），
  // 那种情况下 lunarText 是实时推来的，不能清。
  weekText[0] = 0;
  if (dayCount > 0) lunarText[0] = 0;
}

// ---------------- 渲染 ----------------
void render(bool full = true) {
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);

  // 过 0 点：换到下一份日程/农历/天气，并整屏重绘一次。
  // 用完整的 年月日 做键，只比 tm_mday 稳妥（跨月时不会漏判）。
  static int renderedDate = 0;
  int dateKey = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  if (dateKey != renderedDate) {
    renderedDate = dateKey;
    applyDayFor(&tm);
    full = true;
  }

  bool timeChanged = tm.tm_min != renderedMinute || tm.tm_hour != renderedHour;
  if (full || lastSec < 0 || (screen != SC_MAIN && timeChanged)) {
    if (screen == SC_MAIN)      drawMain(&tm);
    else if (screen == SC_LIST) drawList(&tm);
    else                        drawPopup(&tm);
    gfx->flush();
  } else if (screen == SC_MAIN) {
    if (timeChanged) {
      gfx->fillRect(0, 41, 240, 88, C_BG);
      drawClockTime(&tm);
    }
    drawClockSeconds(&tm);
    if (timeChanged) gfx->flushBand(41, minimalMode ? 143 : 108);
    else gfx->flushBand(minimalMode ? 150 : 130, minimalMode ? 34 : 18);
  }

  // 音量浮层画在所有界面之上，只推顶部这一条
  if (volToastUntil && millis() < volToastUntil) {
    drawVolumeToast();
    gfx->flushBand(0, 48);
  }

  lastSec = tm.tm_sec;
  renderedMinute = tm.tm_min;
  renderedHour = tm.tm_hour;
}

// ---------------- 日程数据 ----------------
char* dateFromTm(struct tm* e, char* out, size_t len) {
  snprintf(out, len, "%02d.%02d", e->tm_mon + 1, e->tm_mday);
  return out;
}

void setEvent(int i, time_t when, const char* title, const char* src, bool allDay) {
  struct tm e;
  localtime_r(&when, &e);
  dateFromTm(&e, events[i].date, sizeof(events[i].date));
  if (allDay) strlcpy(events[i].time, "全天", sizeof(events[i].time));
  else snprintf(events[i].time, sizeof(events[i].time), "%02d:%02d", e.tm_hour, e.tm_min);
  copyUtf8(events[i].title, sizeof(events[i].title), title);
  strlcpy(events[i].src, src, sizeof(events[i].src));
  events[i].allDay = allDay;
  events[i].minutesUntil = allDay ? 99999 : (int)((when - nowEpoch()) / 60);
}

void initDemoEvents() {
  time_t nowT = nowEpoch();
  setEvent(0, nowT + 25 * 60, "备赛课二期提测", "钉钉", false);
  setEvent(1, nowT + 70 * 60, "new-api 每日签到", "本地", false);
  time_t t1 = nowT + 86400;
  struct tm a; localtime_r(&t1, &a);
  a.tm_hour = 10; a.tm_min = 0; a.tm_sec = 0;
  setEvent(2, mktime(&a), "课程组周会", "钉钉", false);
  a.tm_hour = 20;
  setEvent(3, mktime(&a), "版本上线窗口", "本地", false);
  eventCount = 4;
  strlcpy(lunarText, "丙午马年 · 九月廿四", sizeof(lunarText));
  strlcpy(cityText, "北京·朝阳", sizeof(cityText));
  strlcpy(aqiText, "空气优", sizeof(aqiText));
  strlcpy(weatherText, "晴", sizeof(weatherText));
  strlcpy(weatherIcon, "clear", sizeof(weatherIcon));
  tempVal = 22;
}

// ---------------- 桥接服务 ----------------
int parseIsoToMinutes(const char* start, bool allDay, struct tm* now) {
  if (allDay || strlen(start) < 10) return 99999;
  struct tm ev = {0};
  ev.tm_year = atoi(String(start).substring(0, 4).c_str()) - 1900;
  ev.tm_mon  = atoi(String(start).substring(5, 7).c_str()) - 1;
  ev.tm_mday = atoi(String(start).substring(8, 10).c_str());
  if (strlen(start) >= 16) {
    ev.tm_hour = atoi(String(start).substring(11, 13).c_str());
    ev.tm_min  = atoi(String(start).substring(14, 16).c_str());
  }
  ev.tm_isdst = -1;
  return (int)((mktime(&ev) - nowEpoch()) / 60);
}

void syncEvents() {
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  http.begin(String(BRIDGE_BASE) + "/api/events");
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      copyUtf8(lunarText, sizeof(lunarText), doc["lunar"] | "");
      JsonArray arr = doc["events"];
      int n = 0;
      for (JsonObject o : arr) {
        if (n >= MAX_EVENTS) break;
        const char* start = o["start"] | "";
        bool allDay = o["all_day"] | false;
        if (strlen(start) < 10) continue;
        memset(&events[n], 0, sizeof(EventItem));
        snprintf(events[n].date, sizeof(events[n].date), "%c%c.%c%c",
                 start[5], start[6], start[8], start[9]);
        if (allDay) {
          strlcpy(events[n].time, "全天", sizeof(events[n].time));
        } else if (strlen(start) >= 16) {
          memcpy(events[n].time, start + 11, 5);
          events[n].time[5] = 0;
        }
        copyUtf8(events[n].title, sizeof(events[n].title), o["title"] | "(无标题)");
        strlcpy(events[n].src, "钉钉", sizeof(events[n].src));
        events[n].allDay = allDay;
        events[n].minutesUntil = parseIsoToMinutes(start, allDay, nullptr);
        n++;
      }
      eventCount = n;
      forceRender = true;
      Serial.printf("[bridge] 日程 %d 条, 农历: %s\n", n, lunarText);
    }
  } else {
    Serial.printf("[bridge] /api/events HTTP %d\n", code);
  }
  http.end();

  http.begin(String(BRIDGE_BASE) + "/api/weather");
  code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      JsonObject w = doc["weather"];
      if (!w.isNull()) {
        tempVal = w["temp"] | -999;
        copyUtf8(weatherText, sizeof(weatherText), w["text"] | "");
        copyUtf8(weatherIcon, sizeof(weatherIcon), w["icon"] | "clear");
        copyUtf8(cityText, sizeof(cityText), w["city"] | "");
        copyUtf8(aqiText, sizeof(aqiText), w["aqi_text"] | "");
      }
      if (lunarText[0] == 0) copyUtf8(lunarText, sizeof(lunarText), doc["lunar"] | "");
      forceRender = true;
    }
  }
  http.end();
}


// ---------------- 按键扫描（诊断用） ----------------
// 串口发 KEYSCAN，30 秒内把 GPIO0/4/5 的电平变化打出来。
// 期间屏幕不刷新，属于一次性排查工具。
void keyScan(int seconds) {
  const uint8_t pins[] = {0, 4, 5};
  int last[3];
  for (int i = 0; i < 3; i++) {
    pinMode(pins[i], INPUT_PULLUP);
    last[i] = digitalRead(pins[i]);
  }
  Serial.printf("[scan] 初始  GPIO0=%d GPIO4=%d GPIO5=%d  (1=松开 0=按下)\n",
                last[0], last[1], last[2]);
  unsigned long end = millis() + (unsigned long)seconds * 1000UL;
  while (millis() < end) {
    for (int i = 0; i < 3; i++) {
      int v = digitalRead(pins[i]);
      if (v != last[i]) {
        Serial.printf("[scan] GPIO%d -> %s\n", pins[i], v == LOW ? "LOW 按下" : "HIGH 松开");
        last[i] = v;
      }
    }
    delay(12);
  }
  Serial.println("[scan] 结束");
}

// ---------------- USB 串口通道 ----------------
// Mac 端 hook/serial_push.py 行协议, | 分隔
//   TIME|<epoch>               对时
//   NEXT|<HH:MM>|<分钟>|<标题>  下一条日程
//   NONE                       今天没有日程了
//   PING                       -> PONG
static char s_buf[192];
static uint8_t s_len = 0;
static int dayExpected = -1;     // DAYS|n 里声明的条数
static int evExpected = -1;      // EVENTS|n 里声明的条数
static int evGot = 0;

void linkTouch() { linkOnline = true; lastLinkMs = millis(); }

void handleLine(char* s) {
  if (!strcmp(s, "PING")) { Serial.println("PONG"); linkTouch(); return; }

  if (!strncmp(s, "TIME|", 5)) {
    long epoch = atol(s + 5);
    if (epoch > 1760000000) {
      struct timeval tv;
      tv.tv_sec = (time_t)epoch; tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      setenv("TZ", "CST-8", 1);
      tzset();
      demoMode = false;
      forceRender = true;
      linkTouch();
      Serial.println("OK");
    }
    return;
  }

  if (!strcmp(s, "NONE")) {
    eventCount = 0;
    demoMode = false;
    forceRender = true;
    linkTouch();
    Serial.println("OK");
    return;
  }

  // 批量日程：EVENTS|<n> 然后 n 行 EV|MM.DD|HH:MM|分钟|allDay|标题
  if (!strncmp(s, "EVENTS|", 7)) {
    int n = atoi(s + 7);
    if (n < 0) n = 0;
    if (n > MAX_EVENTS) n = MAX_EVENTS;
    eventCount = 0;
    evExpected = n;
    evGot = 0;
    demoMode = false;
    if (n == 0) { evExpected = -1; forceRender = true; }
    linkTouch();
    Serial.println("OK");
    return;
  }

  if (!strncmp(s, "EV|", 3)) {
    char* p = s + 3;
    char* f[5];
    int nf = 0;
    f[nf++] = p;
    while (*p && nf < 5) {
      if (*p == '|') { *p = 0; f[nf++] = p + 1; }
      p++;
    }
    if (nf < 5) { Serial.println("ERR"); return; }
    if (eventCount < MAX_EVENTS) {
      EventItem& e = events[eventCount];
      memset(&e, 0, sizeof(e));
      strlcpy(e.date, f[0], sizeof(e.date));
      strlcpy(e.time, f[1], sizeof(e.time));
      e.minutesUntil = atoi(f[2]);
      e.allDay = atoi(f[3]) != 0;
      copyUtf8(e.title, sizeof(e.title), f[4]);
      strlcpy(e.src, "USB", sizeof(e.src));
      eventCount++;
    }
    evGot++;
    if (evExpected >= 0 && evGot >= evExpected) {
      evExpected = -1;
      forceRender = true;
    }
    linkTouch();
    Serial.println("OK");
    return;
  }

  // 三天数据：DAYS|n 然后 n 行 DAY|日期|周几|农历|温度|文案|图标|空气质量
  if (!strncmp(s, "DAYS|", 5)) {
    int n = atoi(s + 5);
    if (n < 0) n = 0;
    if (n > MAX_DAYS) n = MAX_DAYS;
    dayCount = 0;
    dayExpected = n;
    if (n == 0) dayExpected = -1;
    linkTouch();
    Serial.println("OK");
    return;
  }

  if (!strncmp(s, "DAY|", 4)) {
    char* p = s + 4;
    char* f[7];
    int nf = 0;
    f[nf++] = p;
    while (*p && nf < 7) {
      if (*p == '|') { *p = 0; f[nf++] = p + 1; }
      p++;
    }
    if (nf >= 7 && dayCount < MAX_DAYS) {
      DayInfo& d = days[dayCount];
      memset(&d, 0, sizeof(d));
      copyUtf8(d.date,  sizeof(d.date),  f[0]);
      copyUtf8(d.week,  sizeof(d.week),  f[1]);
      copyUtf8(d.lunar, sizeof(d.lunar), f[2]);
      d.temp = atoi(f[3]);
      copyUtf8(d.text,  sizeof(d.text),  f[4]);
      copyUtf8(d.icon,  sizeof(d.icon),  f[5]);
      copyUtf8(d.aqi,   sizeof(d.aqi),   f[6]);
      dayCount++;
    }
    if (dayExpected >= 0 && dayCount >= dayExpected) {
      dayExpected = -1;
      struct tm nowTm;
      time_t tt = nowEpoch();
      localtime_r(&tt, &nowTm);
      applyDayFor(&nowTm);
      forceRender = true;
    }
    linkTouch();
    Serial.println("OK");
    return;
  }

  // 天气：WEATHER|温度|文案|图标|城市|空气质量
  if (!strncmp(s, "WEATHER|", 8)) {
    char* p = s + 8;
    char* f[5];
    int nf = 0;
    f[nf++] = p;
    while (*p && nf < 5) {
      if (*p == '|') { *p = 0; f[nf++] = p + 1; }
      p++;
    }
    if (nf >= 5) {
      tempVal = atoi(f[0]);
      struct tm wt; time_t wtt = nowEpoch(); localtime_r(&wtt, &wt);
      snprintf(tempDate, sizeof(tempDate), "%02d-%02d", wt.tm_mon + 1, wt.tm_mday);
      copyUtf8(weatherText, sizeof(weatherText), f[1]);
      copyUtf8(weatherIcon, sizeof(weatherIcon), f[2]);
      copyUtf8(cityText,    sizeof(cityText),    f[3]);
      copyUtf8(aqiText,     sizeof(aqiText),     f[4]);
      forceRender = true;
    }
    linkTouch();
    Serial.println("OK");
    return;
  }

  // 农历：LUNAR|丙午马年 · 九月廿五
  if (!strncmp(s, "LUNAR|", 6)) {
    copyUtf8(lunarText, sizeof(lunarText), s + 6);
    forceRender = true;
    linkTouch();
    Serial.println("OK");
    return;
  }

  // 调试用：把设备当前缓存的三天数据/天气/农历原样读回去。
  // 用来验证 Mac 推过来的中文有没有被截断或劈坏（Mac 端对比字节数即可）。
  if (!strcmp(s, "STATE")) {
    Serial.printf("STATE|days|%d\n", dayCount);
    for (int i = 0; i < dayCount; i++) {
      Serial.printf("STATE|day|%d|%s|%s|%s|%d|%s|%s|%s\n", i,
                    days[i].date, days[i].week, days[i].lunar, days[i].temp,
                    days[i].text, days[i].icon, days[i].aqi);
    }
    Serial.printf("STATE|now|%s|%s|%s|%s|%s|%d|%s|%s\n",
                  lunarText, weatherText, weatherIcon, cityText, aqiText,
                  tempVal, weekText, tempDate);
    Serial.printf("STATE|events|%d\n", eventCount);
    for (int i = 0; i < eventCount && i < MAX_EVENTS; i++) {
      Serial.printf("STATE|ev|%d|%s|%s|%s|%d\n", i, events[i].date,
                    events[i].time, events[i].title, events[i].minutesUntil);
    }
    // 本地时钟算出来的日期和周几 —— 用来核对「三天缓存之外」的兜底逻辑。
    {
      time_t t = nowEpoch();
      struct tm lt; localtime_r(&t, &lt);
      char dk[16];
      snprintf(dk, sizeof(dk), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
      Serial.printf("STATE|local|%s|%s\n", dk, weekCN(lt.tm_wday));
      // 背光状态：当前级别 / 设备时间 / 是否夜间 / 是否熄屏 —— 调夜间调光用
      Serial.printf("STATE|bl|%d|%02d:%02d|%d|%d\n", blLevel,
                    lt.tm_hour, lt.tm_min, isNightNow() ? 1 : 0,
                    screenOff ? 1 : 0);
    }
    Serial.println("STATE|ok");
    return;
  }

  // 音量 / 提示音，可从 Mac 端调：VOL|60  SND|1  BEEP
  if (!strncmp(s, "VOL|", 4)) {
    applyVolume(atoi(s + 4));
    Serial.printf("OK %d\n", volumePct);
    linkTouch();
    return;
  }

  if (!strncmp(s, "SND|", 4)) {
    setSoundOn(atoi(s + 4) != 0);
    Serial.printf("OK %d\n", soundOn ? 1 : 0);
    linkTouch();
    return;
  }

  // 背光调试：BL|<0-100> 固定亮度，BL|auto 恢复自动。
  // 白天测夜间调光不用等到 20:00。
  if (!strncmp(s, "BL|", 3)) {
    const char* v = s + 3;
    if (!strcmp(v, "auto")) {
      blForce = -1;
      Serial.println("OK auto");
    } else {
      int pct = atoi(v);
      blForce = pct <= 0 ? 0 : (pct > 100 ? 255 : pct * 255 / 100);
      Serial.printf("OK %d\n", pct);
    }
    blBoostUntil = 0;
    applyBrightness();
    linkTouch();
    return;
  }

  if (!strncmp(s, "KEYSCAN", 7)) {
    int sec = atoi(s + 7);
    if (sec <= 0 || sec > 120) sec = 30;
    Serial.printf("OK %d\n", sec);
    keyScan(sec);
    return;
  }

  if (!strcmp(s, "BEEP")) {
    if (soundOn) audioChime();
    Serial.println("OK");
    linkTouch();
    return;
  }

  if (!strncmp(s, "NEXT|", 5)) {
    char* p = s + 5;
    char* bar1 = strchr(p, '|');
    if (!bar1) return;
    *bar1 = 0;
    char* bar2 = strchr(bar1 + 1, '|');
    if (!bar2) return;
    *bar2 = 0;
    if (eventCount == 0) eventCount = 1;
    memset(&events[0], 0, sizeof(EventItem));
    struct tm nowTm; time_t tt = nowEpoch(); localtime_r(&tt, &nowTm);
    dateFromTm(&nowTm, events[0].date, sizeof(events[0].date));
    copyUtf8(events[0].time, sizeof(events[0].time), p);
    events[0].minutesUntil = atoi(bar1 + 1);
    copyUtf8(events[0].title, sizeof(events[0].title), bar2 + 1);
    strlcpy(events[0].src, "USB", sizeof(events[0].src));
    demoMode = false;
    forceRender = true;
    linkTouch();
    Serial.println("OK");
    return;
  }
}

void linkLoop() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      if (s_len) { s_buf[s_len] = 0; handleLine(s_buf); s_len = 0; }
    } else if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = (char)c;
    }
  }
  if (linkOnline && millis() - lastLinkMs > 30000) {
    linkOnline = false;
    forceRender = true;
  }
}

// ---------------- 提醒逻辑 ----------------
void eventKey(int i, char* out, size_t len) {
  snprintf(out, len, "%s%s", events[i].date, events[i].time);
}

int findPopupIdx() {
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) continue;
    char key[16];
    eventKey(i, key, sizeof(key));
    if (!strcmp(key, ackKey)) continue;
    if (!strcmp(key, snoozeKey) && !snoozeDone && events[i].minutesUntil <= 0) {
      return i;  // 到点再叫我
    }
    if (events[i].minutesUntil > 0 && events[i].minutesUntil <= 10) return i;
  }
  return -1;
}

// ---------------- 触摸 ----------------
uint32_t touchErr = 0;       // I2C 失败次数，用于区分「总线挂了」和「没触摸」

bool readTouch(int16_t& x, int16_t& y) {
  // CST816 寄存器：0x01 手势 / 0x02 手指数 / 0x03-04 X / 0x05-06 Y
  Wire.beginTransmission(0x15);
  Wire.write((uint8_t)0x01);
  if (Wire.endTransmission(false) != 0) { touchErr++; return false; }
  if (Wire.requestFrom((uint8_t)0x15, (uint8_t)6) != 6) { touchErr++; return false; }

  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if ((d[1] & 0x0F) == 0) return false;      // 没有手指

  x = ((d[2] & 0x0F) << 8) | d[3];
  y = ((d[4] & 0x0F) << 8) | d[5];
  return true;
}

void handleGesture(int16_t dx, int16_t dy, int16_t endX, int16_t endY, unsigned long heldMs) {
  if (screen == SC_POPUP) {
    // 只要点一下就能关：只有右下角「到点再叫我」才延后，其余位置一律当「知道了」。
    // 这样即使按钮坐标判断有偏差，也不会再出现「卡在提醒页出不来」。
    if (abs(dx) < 30 && abs(dy) < 30) {
      bool snooze = endX >= 120 && endY >= 180;
      char key[16];
      eventKey(popupIdx, key, sizeof(key));
      if (snooze) {
        strlcpy(snoozeKey, key, sizeof(snoozeKey));  // 到点再叫我
        snoozeDone = false;
      } else {
        strlcpy(ackKey, key, sizeof(ackKey));        // 知道了
        snoozeKey[0] = 0;
      }
      screen = SC_MAIN;
      stopPopupRing();
      forceRender = true;
    }
    return;
  }
  if (screen == SC_LIST) {
    if (abs(dy) > 30 && abs(dy) > abs(dx)) {
      if (dy < 0) listPage++;          // 上滑下一页
      else if (listPage > 0) listPage--;
      forceRender = true;
    } else {
      screen = SC_MAIN;
      forceRender = true;
    }
    return;
  }
  // 主屏
  if (abs(dx) > 40 && abs(dx) > abs(dy)) {
    if (dx < 0 && cardIndex + 1 < eventCount) cardIndex++;
    if (dx > 0 && cardIndex > 0) cardIndex--;
    forceRender = true;
  } else if (abs(dx) < 30 && abs(dy) < 30) {
    if (heldMs >= 650) toggleClockMode();
    else {
      listPage = 0;
      screen = SC_LIST;
      forceRender = true;
    }
  }
}

// ---------------- WiFi ----------------
bool loadWifiFromNVS(String& ssid, String& pass) {
  Preferences p;
  if (!p.begin("wbnet", true)) return false;
  int n = p.getInt("n", 0);
  if (n > 0) {
    ssid = p.getString("s0", "");
    pass = p.getString("p0", "");
  }
  p.end();
  return ssid.length() > 0;
}

void setup() {
  // USB CDC 的收发队列默认各 256 字节。Mac 一次推送（三天数据 + 天气 + 日程）
  // 大概 400~500 字节，256 装不下会被静默丢弃 —— 表现为「某几条 DAY 没进去」。
  // 必须在 begin() 之前调，begin() 只在队列为空时才用默认值创建。
  Serial.setRxBufferSize(1024);
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);        // 写不出去立刻返回，绝不阻塞主循环
  delay(200);
  pinMode(BAT_EN, OUTPUT); digitalWrite(BAT_EN, HIGH);
  if (!gfx->begin()) Serial.println("[gfx] begin failed");
  // 背光走 PWM（夜间调光/熄屏都需要）。attach 失败就退回直连全亮。
  blPwmOk = ledcAttach(GFX_BL, BL_FREQ, BL_RES);
  if (!blPwmOk) {
    Serial.println("[bl] ledcAttach 失败，背光直连全亮");
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);
  }
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  Preferences ui;
  if (ui.begin("clockui", true)) {
    minimalMode = ui.getBool("minimal", false);
    volumePct   = ui.getInt("vol", 60);
    soundOn     = ui.getBool("snd", true);
    ui.end();
  }
  Wire.begin(TP_SDA, TP_SCL);       // 触摸 / ES8311 / IMU 共用这条 I2C
  // CST816 必须硬复位一次才会正常应答；缺了这步读出来永远是「没有手指」
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(60);
  pinMode(KEY_VOL_UP, INPUT_PULLUP);
  pinMode(KEY_VOL_DOWN, INPUT_PULLUP);
  pinMode(KEY_MUTE, INPUT_PULLUP);
  audioBegin();                     // 失败也不影响时钟
  audioSetVolume(soundOn ? volumePct : 0);
  setenv("TZ", "CST-8", 1);
  tzset();

  // 演示起点: 2026-11-02 19:03 周一
  struct tm tm = {0};
  tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 2;
  tm.tm_hour = 19; tm.tm_min = 3; tm.tm_wday = 1;
  baseEpoch = mktime(&tm);
  baseMillis = millis();
  // 必须在 baseEpoch 就位之后：nowEpoch() = baseEpoch + 走时，
  // baseEpoch 还是 0 的时候算出来是 1970 年早上 8 点，会被误判成夜间，
  // 背光带着错误亮度熬过整个 WiFi/NTP 连接窗口（最长 25 秒）才被 loop 纠正。
  applyBrightness();

  String ssid = WIFI_SSID, pass = WIFI_PASS;
  if (ssid.isEmpty() && loadWifiFromNVS(ssid, pass)) {
    Serial.printf("[wifi] 使用 NVS 凭据: %s\n", ssid.c_str());
  }
  demoMode = ssid.isEmpty();

  if (!demoMode) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.print("[wifi] 连接中");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(300); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[wifi] 已连接 %s\n", WiFi.localIP().toString().c_str());
      configTime(8 * 3600, 0, "ntp.aliyun.com", "time.nist.gov");
      unsigned long t0 = millis();
      while (time(nullptr) < 1760000000 && millis() - t0 < 10000) {
        delay(200); Serial.print(".");
      }
      Serial.printf("\n[ntp] %s\n", time(nullptr) > 1760000000 ? "已同步" : "超时");
      eventCount = 0;
      syncEvents();
      lastSyncMs = millis();
    } else {
      demoMode = true;
      Serial.println("\n[wifi] 失败, 回退演示模式");
    }
  }

  if (eventCount == 0 && demoMode) initDemoEvents();
  render();
}

void loop() {
  linkLoop();
  applyBrightness();   // 每轮算一次目标亮度（夜间 30 秒提亮到期会自动暗回去）

  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);

  // 每分钟刷新一次日程倒计时
  static int lastMinute = -1;
  if (tm.tm_min != lastMinute) {
    lastMinute = tm.tm_min;
    for (int i = 0; i < eventCount; i++) {
      if (!events[i].allDay && events[i].minutesUntil > 0) events[i].minutesUntil--;
    }
    forceRender = true;
  }

  // 秒级重绘
  if (tm.tm_sec != lastSec || forceRender) {
    bool full = forceRender;
    forceRender = false;
    // 包括极简模式在内, 提醒检查仍然每秒运行。
    bool ringNow = false;
    if (screen == SC_MAIN) {
      int idx = findPopupIdx();
      if (idx >= 0) {
        popupIdx = idx;
        // 「到点再叫我」只补提醒一次: 弹过就置位, 否则关掉后下一秒又弹(还会再响)
        char k[16];
        eventKey(idx, k, sizeof(k));
        if (snoozeKey[0] && !strcmp(k, snoozeKey)) snoozeDone = true;
        screen = SC_POPUP;
        popupSince = millis();
        full = true;
        ringNow = true;
      }
    }
    render(full);                    // 先画，再响：声音是阻塞的，别卡住首帧
    if (ringNow) ringPopup();
  }

  // 音量浮层到期：整屏重绘一次把它擦掉
  if (volToastUntil && millis() >= volToastUntil) {
    volToastUntil = 0;
    forceRender = true;
  }

  // 兜底：提醒页最多停留 3 分钟。触摸或按键万一都失灵，也不会把时钟永久锁死。
  if (screen == SC_POPUP && popupSince && millis() - popupSince > 180000UL) {
    char k[16];
    eventKey(popupIdx, k, sizeof(k));
    strlcpy(ackKey, k, sizeof(ackKey));
    snoozeKey[0] = 0;
    screen = SC_MAIN;
    stopPopupRing();
    forceRender = true;
  }

  // 弹窗没处理就每 30 秒再响一次，最多 3 次
  if (screen == SC_POPUP && soundOn && popupBeeps < POPUP_BEEP_MAX &&
      millis() - lastBeepMs > POPUP_BEEP_GAP) {
    lastBeepMs = millis();
    popupBeeps++;
    audioChime();
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastSyncMs > 60000) {
    lastSyncMs = millis();
    syncEvents();
  }

  // 触摸手势
  static bool wasTouch = false;
  static int16_t startX = 0, startY = 0, lastX = 0, lastY = 0;
  static unsigned long lastTouchCheck = 0;
  static unsigned long touchStartMs = 0;
  static unsigned long lastTouchErrPrint = 0;
  static bool touchWasWake = false;   // 这次按下是用来点屏的，抬指时不触发手势
  if (millis() - lastTouchCheck > 60) {
    lastTouchCheck = millis();
    if (touchErr && millis() - lastTouchErrPrint > 5000) {
      lastTouchErrPrint = millis();
      DBG("[touch] I2C 失败 %lu 次\n", (unsigned long)touchErr);
      touchErr = 0;
    }
    int16_t x, y;
    bool touching = readTouch(x, y);
    if (touching && !wasTouch) {
      startX = lastX = x; startY = lastY = y;
      touchStartMs = millis();
      if (screenOff) {
        wakeScreen();                 // 熄屏中：这次触摸只负责点亮
        touchWasWake = true;
      } else {
        boostBacklight();             // 夜间调光被碰了：提亮 30 秒
      }
      DBG("[touch] down %d,%d\n", x, y);
    } else if (touching) {
      lastX = x; lastY = y;
    } else if (!touching && wasTouch) {
      // 命中判定用「按下点」：抬指那一帧的坐标可能是 0,0 或抖动值，
      // 用它做命中会导致「点了没反应」。
      DBG("[touch] up %d,%d (down %d,%d)\n", lastX, lastY, startX, startY);
      if (touchWasWake) {
        DBG("[touch] 点亮用的触摸，忽略手势\n");
      } else {
        handleGesture(lastX - startX, lastY - startY, startX, startY, millis() - touchStartMs);
      }
      touchWasWake = false;
    }
    wasTouch = touching;
  }

  // 板载按键：GPIO4 音量+ / GPIO0 音量- / GPIO5 静音(短按)，静音长按熄屏
  static bool kUp = false, kDown = false, kMute = false;
  static unsigned long muteDownMs = 0;
  static bool muteLongFired = false;
  static unsigned long lastKeyCheck = 0;
  if (millis() - lastKeyCheck > 40) {
    lastKeyCheck = millis();
    bool up   = digitalRead(KEY_VOL_UP)   == LOW;
    bool down = digitalRead(KEY_VOL_DOWN) == LOW;
    bool mute = digitalRead(KEY_MUTE)     == LOW;
    bool pressed = (up && !kUp) || (down && !kDown) || (mute && !kMute);

    if (screenOff) {
      // 熄屏中：任何按键都只负责点亮，这一次事件不再执行别的动作
      if (pressed) wakeScreen();
      if (!mute && kMute) { muteDownMs = 0; muteLongFired = false; }
    } else if (pressed && screen == SC_POPUP) {
      boostBacklight();
      char key[16];
      eventKey(popupIdx, key, sizeof(key));
      strlcpy(ackKey, key, sizeof(ackKey));
      snoozeKey[0] = 0;
      screen = SC_MAIN;
      stopPopupRing();
      forceRender = true;
    } else {
      if (up && !kUp) {
        boostBacklight();
        DBG("[key] GPIO%d 音量+ -> ", KEY_VOL_UP);
        applyVolume(volumePct + 5);      // 试听音在 applyVolume 里放
        DBG("%d%%\n", volumePct);
      }
      if (down && !kDown) {
        boostBacklight();
        DBG("[key] GPIO%d 音量- -> ", KEY_VOL_DOWN);
        applyVolume(volumePct - 5);
        DBG("%d%%\n", volumePct);
      }
      // 静音键：短按=静音切换，长按 0.8 秒=熄屏
      if (mute && !kMute) {
        muteDownMs = millis();
        muteLongFired = false;
      }
      if (mute && muteDownMs && !muteLongFired &&
          millis() - muteDownMs >= SCREEN_OFF_HOLD_MS) {
        muteLongFired = true;
        screenOff = true;
        applyBrightness();             // 立刻熄屏，不等 loop 下一轮
        DBG("[bl] 熄屏（长按静音键）\n");
      }
      if (!mute && kMute) {            // 松开
        if (muteDownMs && !muteLongFired) {
          boostBacklight();
          DBG("[key] GPIO%d 静音切换\n", KEY_MUTE);
          setSoundOn(!soundOn);
          if (soundOn) audioTick();
        }
        muteDownMs = 0;
        muteLongFired = false;
      }
    }
    kUp = up; kDown = down; kMute = mute;
  }

  delay(15);
}
