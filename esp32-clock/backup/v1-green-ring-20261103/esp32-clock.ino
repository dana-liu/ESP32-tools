// ESP32-S3-Touch-LCD-1.54 科技风时钟 v1.1 (青绿圆环版)
// 界面: 外圈刻度 + 旋转秒点 / 大字时间 / 日期 / 底部下一提醒卡片
// 模式: secrets.h 配了 WiFi 则 NTP 对时 + 轮询桥接 /api/next, 否则演示数据
//
// v1.1 相对 v1 的改动:
//   1. 改用离屏画布(PSRAM)整帧渲染, 每秒一次 flush
//      —— 原来的局部重绘有三个擦除 bug:
//         · drawTopBar 的 fillRect 会擦掉四角科技线框
//         · 擦秒点时 drawCircle(整圈) 会把青色虚线点一起抹掉
//         · 擦秒点的 5px 圆会啃掉附近的刻度线
//   2. WiFi 凭据支持从 NVS 读取(沿用 wb-status 固件存过的)
//   3. 联网模式不再先显示演示日程; 连上就立刻同步一次
//   4. NTP 等对时完成再开始走秒, 避免时间跳变
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

// 离屏画布: 整帧画进 PSRAM, 画完一次 flush, 中间不会出现黑屏或擦除残影
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
};

static Arduino_GFX* gfx = new PsramCanvas(240, 240, panel);

static constexpr uint16_t C(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t C_BG = C(16, 16, 24), C_RING = C(38, 38, 46), C_TICK = C(58, 58, 68);
const uint16_t C_GREEN = C(93, 202, 165), C_TEXT = C(230, 241, 251);
const uint16_t C_MUTED = C(95, 94, 90), C_AMBER = C(239, 159, 39);
const uint16_t C_AMBERT = C(250, 199, 117), C_CARD = C(28, 28, 38);
const uint16_t C_CBORD = C(99, 56, 6), C_TEAL = C(29, 158, 117);
const uint16_t C_BLUE = C(55, 138, 221);

struct Reminder { char time[6]; char title[64]; int minutesUntil; bool valid; };
Reminder demoList[2] = {
  {"19:30", "备赛课二期提测", 27, true},
  {"21:00", "new-api 每日签到", 117, true},
};
Reminder current = {"19:30", "备赛课二期提测", 27, true};
bool demoMode = true;
int demoIdx = 0;
time_t baseEpoch = 0;           // 演示模式: 固定起点 + millis 推算
unsigned long baseMillis = 0;
unsigned long lastSyncMs = 0;
int lastSec = -1;
bool forceRender = true;
bool linkOnline = false;        // USB 串口通道是否在推数据
unsigned long lastLinkMs = 0;

// ---------------- 时间 ----------------
time_t nowEpoch() {
  time_t t = time(nullptr);
  if (t > 1760000000) return t;  // NTP 已对时(>2025)
  return baseEpoch + (millis() - baseMillis) / 1000;
}

// ---------------- 文本 ----------------
void drawCentered(const char* s, int yBaseline, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((240 - (int)w) / 2 - x1, yBaseline);
  gfx->print(s);
}

// ---------------- 绘制(每次都是整帧) ----------------
void drawFrame(struct tm* tm, int sec) {
  gfx->fillScreen(C_BG);

  // 圆环
  gfx->drawCircle(120, 120, 98, C_RING);
  // 12 个刻度
  for (int i = 0; i < 12; i++) {
    float a = (i * 30 - 90) * DEG_TO_RAD;
    gfx->drawLine(120 + cos(a) * 96, 120 + sin(a) * 96,
                  120 + cos(a) * 86, 120 + sin(a) * 86, C_TICK);
  }
  // 青色虚线内圈(每 6 度一点)
  for (int d = 0; d < 360; d += 6) {
    float a = d * DEG_TO_RAD;
    gfx->drawPixel(120 + cos(a) * 98, 120 + sin(a) * 98, C_TEAL);
  }

  // 四角科技线框
  gfx->drawLine(14, 40, 14, 26, C_TICK); gfx->drawLine(14, 26, 28, 26, C_TICK);
  gfx->drawLine(212, 26, 226, 26, C_TICK); gfx->drawLine(226, 26, 226, 40, C_TICK);
  gfx->drawLine(226, 200, 226, 214, C_TICK); gfx->drawLine(226, 214, 212, 214, C_TICK);
  gfx->drawLine(28, 214, 14, 214, C_TICK); gfx->drawLine(14, 214, 14, 200, C_TICK);

  // 顶栏: 日期 + 数据源
  static const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%s %02d.%02d", days[tm->tm_wday], tm->tm_mon + 1, tm->tm_mday);
  gfx->setFont(u8g2_font_6x10_tf);
  gfx->setTextColor(C_MUTED);
  gfx->setCursor(26, 26); gfx->print(buf);
  const char* src = linkOnline ? "USB" : (demoMode ? "DEMO" : "NTP");
  gfx->setTextColor(linkOnline ? C_BLUE : (demoMode ? C_AMBER : C_GREEN));
  gfx->setCursor(198, 26); gfx->print(src);

  // 大字时间(居中在 44..184 区间)
  snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
  gfx->setFont(u8g2_font_logisoso42_tn);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(C_TEXT);
  gfx->setCursor(112 - (int)w / 2 - x1, 130);
  gfx->print(buf);

  // 秒数
  snprintf(buf, sizeof(buf), "%02d", sec);
  gfx->setFont(u8g2_font_logisoso18_tn);
  gfx->setTextColor(C_GREEN);
  gfx->setCursor(188, 128); gfx->print(buf);

  // 秒点 + 指针
  {
    float a = (sec * 6 - 90) * DEG_TO_RAD;
    gfx->drawLine(120 + cos(a) * 90, 120 + sin(a) * 90,
                  120 + cos(a) * 78, 120 + sin(a) * 78, C_GREEN);
    gfx->fillCircle(120 + cos(a) * 98, 120 + sin(a) * 98, 4, C_GREEN);
  }

  // 日期行
  snprintf(buf, sizeof(buf), "%04d.%02d.%02d %s",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, days[tm->tm_wday]);
  gfx->setFont(u8g2_font_6x10_tf);
  drawCentered(buf, 153, C_MUTED);

  // 底部提醒卡片 —— 只有真的有日程时才画。
  // 卡片在 y=192..232，而圆环最低点到 y=218，空卡片会横切圆环底部，
  // 看起来像一块挡板。没日程就干脆不画，让表盘完整露出来。
  if (current.valid) {
    gfx->fillRoundRect(30, 192, 180, 40, 10, C_CARD);
    gfx->drawRoundRect(30, 192, 180, 40, 10, C_CBORD);
    gfx->fillCircle(40, 203, 3, C_AMBER);

    gfx->setFont(u8g2_font_wqy12_t_gb2312);
    gfx->setTextColor(C_AMBERT);
    gfx->setCursor(50, 208);
    snprintf(buf, sizeof(buf), "%s %s", current.time, current.title);
    gfx->print(buf);
    gfx->setFont(u8g2_font_6x10_tf);
    gfx->setTextColor(C_MUTED);
    gfx->setCursor(50, 224);
    snprintf(buf, sizeof(buf), current.minutesUntil > 0 ? "IN %d MIN" : "NOW",
             current.minutesUntil);
    gfx->print(buf);
  }
}

void render() {
  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);
  drawFrame(&tm, tm.tm_sec);
  gfx->flush();
  lastSec = tm.tm_sec;
}

// ---------------- 桥接 ----------------
void syncBridge() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  http.begin(String(BRIDGE_BASE) + "/api/next");
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      if (doc["next"].isNull()) {
        current.valid = false;
      } else {
        JsonObject n = doc["next"].as<JsonObject>();
        strlcpy(current.title, n["title"] | "(无标题)", sizeof(current.title));
        const char* start = n["start"] | "";
        if (strlen(start) >= 16) {
          memcpy(current.time, start + 11, 5);
          current.time[5] = 0;
        }
        current.minutesUntil = n["minutes_until"] | 0;
        current.valid = true;
      }
      forceRender = true;
      Serial.printf("[bridge] 下一日程: %s %s (%d 分钟后)\n",
                    current.valid ? current.time : "--",
                    current.valid ? current.title : "无", current.minutesUntil);
    }
  } else {
    Serial.printf("[bridge] HTTP %d\n", code);
  }
  http.end();
}


// ---------------- USB 串口通道 ----------------
// Mac 端 hook/serial_push.py 推来的行协议, | 分隔
//   TIME|<epoch>               对时(没 WiFi 就没有 NTP, 时间只能靠推)
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
    current.valid = false;
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
    strlcpy(current.time, p, sizeof(current.time));
    current.minutesUntil = atoi(bar1 + 1);
    strlcpy(current.title, bar2 + 1, sizeof(current.title));
    current.valid = true;
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
    linkOnline = false;          // 30 秒没数据就认为 USB 断了
    forceRender = true;
  }
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

// ---------------- WiFi ----------------
// secrets.h 没填就沿用 wb-status 固件存进 NVS("wbnet")的凭据
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
  Wire.begin(TP_SDA, TP_SCL);
  Serial.printf("[gfx] 画布 %s\n",
                ((uintptr_t)((Arduino_Canvas*)gfx)->getFramebuffer() >> 28) == 0x3
                    ? "PSRAM" : "内部RAM");

  // 演示起点: 2026-11-02 19:03 周一
  struct tm tm = {0};
  tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 2;
  tm.tm_hour = 19; tm.tm_min = 3; tm.tm_wday = 1;
  baseEpoch = mktime(&tm);
  baseMillis = millis();

  String ssid = WIFI_SSID, pass = WIFI_PASS;
  if (ssid.isEmpty() && loadWifiFromNVS(ssid, pass)) {
    Serial.printf("[wifi] 使用 NVS 保存的凭据: %s\n", ssid.c_str());
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
      Serial.print("[ntp] 对时中");
      unsigned long t0 = millis();
      while (time(nullptr) < 1760000000 && millis() - t0 < 10000) {
        delay(200); Serial.print(".");
      }
      Serial.printf("\n[ntp] %s\n", time(nullptr) > 1760000000 ? "已同步" : "超时");
      current.valid = false;          // 联网模式先不显示演示日程
      syncBridge();                   // 立刻拉一次, 不等 60 秒
      lastSyncMs = millis();
    } else {
      demoMode = true;
      Serial.println("\n[wifi] 失败, 回退演示模式");
    }
  }

  render();
}

void loop() {
  linkLoop();

  time_t t = nowEpoch();
  struct tm tm;
  localtime_r(&t, &tm);

  if (tm.tm_sec != lastSec || forceRender) {
    forceRender = false;
    render();
    if (tm.tm_sec == 0 && current.valid && current.minutesUntil > 0) {
      current.minutesUntil--;         // 每分钟本地递减, 桥接同步会覆盖
    }
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastSyncMs > 60000) {
    lastSyncMs = millis();
    syncBridge();
  }

  // 演示模式: 点按切换下一条提醒
  static bool wasTouch = false;
  static unsigned long lastTouchCheck = 0;
  if (millis() - lastTouchCheck > 120) {
    lastTouchCheck = millis();
    int16_t x, y;
    bool touching = readTouch(x, y);
    if (touching && !wasTouch && demoMode && !linkOnline) {
      Serial.printf("[touch] %d,%d\n", x, y);
      demoIdx = (demoIdx + 1) % 2;
      current = demoList[demoIdx];
      forceRender = true;
    }
    wasTouch = touching;
  }
  delay(20);
}
