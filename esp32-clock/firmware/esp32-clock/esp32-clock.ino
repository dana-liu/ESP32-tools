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

#define GFX_BL 46
#define BAT_EN 2
#define TP_SDA 42
#define TP_SCL 41

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

enum Screen { SC_MAIN, SC_LIST, SC_POPUP };
Screen screen = SC_MAIN;
int popupIdx = 0;

char lunarText[32] = "";
int  tempVal = -999;
char weatherText[8] = "";
char weatherIcon[8] = "";
char aqiText[16] = "";
char cityText[20] = "";

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
  snprintf(buf, sizeof(buf), "%02d月%02d日  %s", t->tm_mon + 1, t->tm_mday, weekCN(t->tm_wday));
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
void drawListCard(int y, int idx, bool first) {
  EventItem* e = &events[idx];
  gfx->fillRoundRect(16, y, 208, 30, 6, first ? C_ACCENTL : C_CARD);
  gfx->fillRect(16, y + 5, 2, 20, first ? C_ORANGE : C_LINE);
  char buf[64];
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  drawFitText(e->title, 26, y + 12, 188, C_TEXT);
  snprintf(buf, sizeof(buf), "%s%s%s", e->time, e->src[0] ? " · " : "", e->src);
  drawFitText(buf, 26, y + 26, 188, C_MUTED);
}

void drawList(struct tm* t) {
  gfx->fillScreen(C_BG);
  char today[6], tomorrow[6];
  snprintf(today, sizeof(today), "%02d.%02d", t->tm_mon + 1, t->tm_mday);
  time_t tt = nowEpoch() + 86400;
  struct tm nxt; localtime_r(&tt, &nxt);
  snprintf(tomorrow, sizeof(tomorrow), "%02d.%02d", nxt.tm_mon + 1, nxt.tm_mday);

  gfx->setFont(u8g2_font_wqy16_t_gb2312);
  drawFitText("日程", 16, 27, 100, C_TEXT);
  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  textRight("今天 / 明天", 224, 26, C_MUTED);
  gfx->drawFastHLine(16, 37, 208, C_LINE);
  drawFitText("今天", 16, 54, 100, C_ACCENT);

  int y = 61, shownToday = 0, drawn = 0;
  for (int i = 0; i < eventCount && drawn < 2; i++) {
    if (!strcmp(events[i].date, today)) {
      drawListCard(y, i, shownToday == 0);
      y += 34; shownToday++; drawn++;
    }
  }
  if (shownToday == 0) {
    gfx->setFont(u8g2_font_wqy12_t_gb2312);
    drawFitText("今天没有日程", 26, 81, 188, C_MUTED);
  }

  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  drawFitText("明天", 16, 144, 100, C_ACCENT);
  y = 151;

  int shownTomorrow = 0, drawn2 = 0;
  for (int i = 0; i < eventCount && drawn2 < 2; i++) {
    if (!strcmp(events[i].date, tomorrow)) {
      drawListCard(y, i, false);
      y += 34; shownTomorrow++; drawn2++;
    }
  }
  if (shownTomorrow == 0) {
    gfx->setFont(u8g2_font_wqy12_t_gb2312);
    drawFitText("明天没有日程", 26, 172, 188, C_MUTED);
  }

  gfx->setFont(u8g2_font_wqy12_t_gb2312);
  textCenter("点按返回", 233, C_MUTED);
}

// ---------------- 渲染 ----------------
void render(bool full = true) {
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);
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
  strlcpy(events[i].title, title, sizeof(events[i].title));
  strlcpy(events[i].src, src, sizeof(events[i].src));
  events[i].allDay = allDay;
  events[i].minutesUntil = allDay ? 99999 : (int)((when - nowEpoch()) / 60);
}

void initDemoEvents() {
  time_t nowT = nowEpoch();
  setEvent(0, nowT + 10 * 60, "备赛课二期提测", "钉钉", false);
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
      strlcpy(lunarText, doc["lunar"] | "", sizeof(lunarText));
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
        strlcpy(events[n].title, o["title"] | "(无标题)", sizeof(events[n].title));
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
        strlcpy(weatherText, w["text"] | "", sizeof(weatherText));
        strlcpy(weatherIcon, w["icon"] | "clear", sizeof(weatherIcon));
        strlcpy(cityText, w["city"] | "", sizeof(cityText));
        strlcpy(aqiText, w["aqi_text"] | "", sizeof(aqiText));
      }
      if (lunarText[0] == 0) strlcpy(lunarText, doc["lunar"] | "", sizeof(lunarText));
      forceRender = true;
    }
  }
  http.end();
}

// ---------------- USB 串口通道 ----------------
// Mac 端 hook/serial_push.py 行协议, | 分隔
//   TIME|<epoch>               对时
//   NEXT|<HH:MM>|<分钟>|<标题>  下一条日程
//   NONE                       今天没有日程了
//   PING                       -> PONG
static char s_buf[192];
static uint8_t s_len = 0;

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
    strlcpy(events[0].time, p, sizeof(events[0].time));
    events[0].minutesUntil = atoi(bar1 + 1);
    strlcpy(events[0].title, bar2 + 1, sizeof(events[0].title));
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
bool readTouch(int16_t& x, int16_t& y) {
  Wire.beginTransmission(0x15);
  Wire.write(0x02);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(0x15, 5) != 5) return false;
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return fingers > 0;
}

void handleGesture(int16_t dx, int16_t dy, int16_t endX, int16_t endY, unsigned long heldMs) {
  if (screen == SC_POPUP) {
    bool leftButton = endX >= 16 && endX < 112;
    bool rightButton = endX >= 124 && endX < 224;
    if (abs(dx) < 20 && abs(dy) < 20 && endY >= 188 && endY < 228 &&
        (leftButton || rightButton)) {
      char key[16];
      eventKey(popupIdx, key, sizeof(key));
      if (leftButton) {
        strlcpy(ackKey, key, sizeof(ackKey));   // 知道了
        snoozeKey[0] = 0;
      } else {
        strlcpy(snoozeKey, key, sizeof(snoozeKey));  // 到点再叫我
        snoozeDone = false;
      }
      screen = SC_MAIN;
      forceRender = true;
    }
    return;
  }
  if (screen == SC_LIST) {
    screen = SC_MAIN;
    forceRender = true;
    return;
  }
  // 主屏
  if (abs(dx) > 40 && abs(dx) > abs(dy)) {
    if (dx < 0 && cardIndex + 1 < eventCount) cardIndex++;
    if (dx > 0 && cardIndex > 0) cardIndex--;
    forceRender = true;
  } else if (abs(dx) < 20 && abs(dy) < 20) {
    if (heldMs >= 650) toggleClockMode();
    else {
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
  Serial.begin(115200);
  delay(200);
  pinMode(BAT_EN, OUTPUT); digitalWrite(BAT_EN, HIGH);
  if (!gfx->begin()) Serial.println("[gfx] begin failed");
  pinMode(GFX_BL, OUTPUT); digitalWrite(GFX_BL, HIGH);
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  Preferences ui;
  if (ui.begin("clockui", true)) {
    minimalMode = ui.getBool("minimal", false);
    ui.end();
  }
  Wire.begin(TP_SDA, TP_SCL);
  setenv("TZ", "CST-8", 1);
  tzset();

  // 演示起点: 2026-11-02 19:03 周一
  struct tm tm = {0};
  tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 2;
  tm.tm_hour = 19; tm.tm_min = 3; tm.tm_wday = 1;
  baseEpoch = mktime(&tm);
  baseMillis = millis();

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
    if (screen == SC_MAIN) {
      int idx = findPopupIdx();
      if (idx >= 0) {
        popupIdx = idx;
        screen = SC_POPUP;
        full = true;
      }
    }
    render(full);
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
  if (millis() - lastTouchCheck > 60) {
    lastTouchCheck = millis();
    int16_t x, y;
    bool touching = readTouch(x, y);
    if (touching && !wasTouch) {
      startX = lastX = x; startY = lastY = y;
      touchStartMs = millis();
      Serial.printf("[touch] down %d,%d\n", x, y);
    } else if (touching) {
      lastX = x; lastY = y;
    } else if (!touching && wasTouch) {
      Serial.printf("[touch] up %d,%d\n", lastX, lastY);
      handleGesture(lastX - startX, lastY - startY, lastX, lastY, millis() - touchStartMs);
    }
    wasTouch = touching;
  }
  delay(15);
}
