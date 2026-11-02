/*
 * WorkBuddy 桌面状态灯 (wb_status) —— 多任务版
 * ------------------------------------------------------------
 * 硬件：
 *   - ESP32-C3 SuperMini 或 ESP32-WROOM-32 DevKit
 *   - SSD1306 128x64 I2C OLED（0.96" / 1.3"）
 *   - WS2812B 灯环 12 灯（可选）
 *   - 有源蜂鸣器（可选）
 *
 * 多任务模型：
 *   - 最多同时跟踪 4 个会话（session）
 *   - 屏幕只显示「优先级最高」的那个，同级任务每 4 秒轮播
 *   - 优先级：确认 > 出错 > 执行中 > 思考中 > 已完成 > 空闲
 *   - 灯环按任务数分段，每段显示各自状态的颜色
 *
 * 接口：
 *   GET /state?sid=ab12cd34&st=working&tool=Bash&label=重构登录
 *   GET /state?sid=ab12cd34&st=off          移除任务
 *   GET /list                               调试：所有任务
 * ------------------------------------------------------------
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>

// ==================== 用户配置区 ====================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* MDNS_HOST = "wbstatus";

#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_LED      4
#define LED_COUNT   12
#define PIN_BUZZER   5
#define HAS_LED      1
#define HAS_BUZZER   1
#define LED_BRIGHT  60

// 1 = 启用中文任务名（u8g2_font_wqy12_t_gb2312，约多占 500KB Flash）
// 0 = 只用 ASCII，任务名取目录名
#define USE_CN_FONT  1

#define MAX_TASKS    4
// ===================================================

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Adafruit_NeoPixel ring(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);
WebServer server(80);

enum State { ST_NONE, ST_IDLE, ST_THINKING, ST_WORKING, ST_CONFIRM, ST_DONE, ST_ERROR };

struct Task {
  bool     active;
  char     id[10];
  char     label[48];
  char     tool[20];
  State    st;
  bool     beeped;
  uint32_t updated;
};

Task g_tasks[MAX_TASKS];
int  g_focusOrd    = 0;   // 当前显示的是第几个活跃任务
int  g_activeCount = 0;

// ---------------- 状态工具 ----------------
const char* stateLabel(State s) {
  switch (s) {
    case ST_IDLE:     return "IDLE";
    case ST_THINKING: return "THINKING";
    case ST_WORKING:  return "WORKING";
    case ST_CONFIRM:  return "CONFIRM?";
    case ST_DONE:     return "DONE";
    case ST_ERROR:    return "ERROR";
    default:          return "IDLE";
  }
}

int statePrio(State s) {
  switch (s) {
    case ST_CONFIRM:  return 5;
    case ST_ERROR:    return 4;
    case ST_WORKING:  return 3;
    case ST_THINKING: return 2;
    case ST_DONE:     return 1;
    default:          return 0;
  }
}

uint32_t stateColor(State s) {
  switch (s) {
    case ST_IDLE:     return ring.Color(0, 0, 24);
    case ST_THINKING: return ring.Color(0, 90, 90);
    case ST_WORKING:  return ring.Color(0, 40, 140);
    case ST_CONFIRM:  return ring.Color(160, 0, 0);
    case ST_DONE:     return ring.Color(0, 120, 0);
    case ST_ERROR:    return ring.Color(150, 60, 0);
    default:          return ring.Color(8, 8, 8);
  }
}

State parseState(const String& s) {
  if (s == "idle")     return ST_IDLE;
  if (s == "thinking") return ST_THINKING;
  if (s == "working")  return ST_WORKING;
  if (s == "confirm")  return ST_CONFIRM;
  if (s == "done")     return ST_DONE;
  if (s == "error")    return ST_ERROR;
  return ST_IDLE;
}

// ---------------- 任务表 ----------------
void tasksInit() {
  for (int i = 0; i < MAX_TASKS; i++) {
    g_tasks[i].active  = false;
    g_tasks[i].id[0]   = 0;
    g_tasks[i].label[0] = 0;
    g_tasks[i].tool[0] = 0;
    g_tasks[i].st      = ST_NONE;
    g_tasks[i].beeped  = false;
    g_tasks[i].updated = 0;
  }
}

int findTask(const char* id) {
  for (int i = 0; i < MAX_TASKS; i++)
    if (g_tasks[i].active && strcmp(g_tasks[i].id, id) == 0) return i;
  return -1;
}

int allocTask() {
  for (int i = 0; i < MAX_TASKS; i++)
    if (!g_tasks[i].active) return i;
  // 满了：淘汰优先级最低、且最久没更新的
  int victim = 0, worst = 999;
  for (int i = 0; i < MAX_TASKS; i++) {
    int score = statePrio(g_tasks[i].st) * 1000 + (int)((millis() - g_tasks[i].updated) / 1000);
    if (score < worst) { worst = score; victim = i; }
  }
  return victim;
}

void updateTask(const String& sid, State st, const String& tool, const String& label) {
  int idx = findTask(sid.c_str());

  if (st == ST_NONE) {                 // st=off：移除
    if (idx >= 0) g_tasks[idx].active = false;
    return;
  }

  if (idx < 0) {
    idx = allocTask();
    g_tasks[idx].active = true;
    strncpy(g_tasks[idx].id, sid.c_str(), sizeof(g_tasks[idx].id) - 1);
    g_tasks[idx].id[sizeof(g_tasks[idx].id) - 1] = 0;
    g_tasks[idx].label[0] = 0;
    g_tasks[idx].beeped = false;
  }

  Task& t = g_tasks[idx];
  if (label.length()) {
    strncpy(t.label, label.c_str(), sizeof(t.label) - 1);
    t.label[sizeof(t.label) - 1] = 0;
  }
  if (tool.length()) {
    strncpy(t.tool, tool.c_str(), sizeof(t.tool) - 1);
    t.tool[sizeof(t.tool) - 1] = 0;
  }

  if (t.st != st) {
    t.st = st;
    if (st != ST_CONFIRM) t.beeped = false;
  }
  t.updated = millis();
}

int countActive() {
  int n = 0;
  for (int i = 0; i < MAX_TASKS; i++) if (g_tasks[i].active) n++;
  return n;
}

// 选焦点：优先级最高者胜出，同级每 4 秒轮播
int pickFocus() {
  int bestPrio = -1, ties = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    int p = statePrio(g_tasks[i].st);
    if (p > bestPrio) { bestPrio = p; ties = 1; }
    else if (p == bestPrio) ties++;
  }
  if (bestPrio < 0) return -1;

  int pick = (millis() / 4000) % ties;
  int seen = 0;
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    if (statePrio(g_tasks[i].st) != bestPrio) continue;
    if (seen == pick) return i;
    seen++;
  }
  return -1;
}

// ---------------- 蜂鸣器 ----------------
void beep(uint16_t freq, uint16_t ms) {
#if HAS_BUZZER
  tone(PIN_BUZZER, freq, ms);
  delay(ms);
  noTone(PIN_BUZZER);
#endif
}

void beepFor(State s) {
  if (s == ST_CONFIRM) { beep(2000, 90); delay(70); beep(2000, 90); }
  else if (s == ST_DONE) { beep(1500, 80); delay(60); beep(2200, 120); }
  else if (s == ST_ERROR) { beep(400, 400); }
}

// ---------------- HTTP ----------------
void handleState() {
  String sid = server.arg("sid");
  String st  = server.arg("st");
  String tl  = server.arg("tool");
  String lb  = server.arg("label");

  if (sid.length() == 0) sid = "default";

  if (st == "off") {
    updateTask(sid, ST_NONE, "", "");
  } else if (st.length()) {
    updateTask(sid, parseState(st), tl, lb);
  }

  // 有新出现的「需要确认」就响
  for (int i = 0; i < MAX_TASKS; i++) {
    if (g_tasks[i].active && g_tasks[i].st == ST_CONFIRM && !g_tasks[i].beeped) {
      g_tasks[i].beeped = true;
      beepFor(ST_CONFIRM);
    }
  }

  char resp[40];
  snprintf(resp, sizeof(resp), "ok %d task(s)", countActive());
  server.send(200, "text/plain", resp);
}

void handleList() {
  String j = "[";
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    if (j.length() > 1) j += ",";
    j += "{\"sid\":\"";  j += g_tasks[i].id;
    j += "\",\"label\":\""; j += g_tasks[i].label;
    j += "\",\"st\":\"";  j += stateLabel(g_tasks[i].st);
    j += "\",\"tool\":\""; j += g_tasks[i].tool;
    j += "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

// ---------------- 显示 ----------------
void drawSpinner(int cx, int cy, int r) {
  static uint8_t a = 0;
  a = (a + 12) % 360;
  for (int i = 0; i < 8; i++) {
    float ang = (a + i * 45) * 3.14159 / 180.0;
    int x = cx + cos(ang) * r;
    int y = cy + sin(ang) * r;
    if (i < 3) u8g2.drawDisc(x, y, 2);
    else       u8g2.drawDisc(x, y, 1);
  }
}

void drawLabel(const char* s, int y, int maxW) {
  if (!s || !*s) return;
  char buf[56];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

#if USE_CN_FONT
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  while (strlen(buf) > 0 && u8g2.getUTF8Width(buf) > maxW) {
    int len = strlen(buf), i = len - 1;
    while (i > 0 && ((uint8_t)buf[i] & 0xC0) == 0x80) i--;   // 回退到 UTF-8 字符边界
    buf[i] = 0;
  }
  int w = u8g2.getUTF8Width(buf);
  u8g2.drawUTF8((128 - w) / 2, y, buf);
#else
  u8g2.setFont(u8g2_font_6x12_tf);
  while (strlen(buf) > 0 && u8g2.getStrWidth(buf) > maxW) buf[strlen(buf) - 1] = 0;
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, y, buf);
#endif
}

void drawUI() {
  u8g2.clearBuffer();
  g_activeCount = countActive();
  int fi = pickFocus();

  if (fi < 0) {
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.setFont(u8g2_font_helvB14_tf);
    const char* s = "IDLE";
    u8g2.drawStr((128 - u8g2.getStrWidth(s)) / 2, 30, s);
    u8g2.setFont(u8g2_font_6x12_tf);
    const char* s2 = "waiting";
    u8g2.drawStr((128 - u8g2.getStrWidth(s2)) / 2, 50, s2);
    u8g2.sendBuffer();
    return;
  }

  Task& t = g_tasks[fi];
  g_focusOrd = 0;
  for (int i = 0; i < fi; i++) if (g_tasks[i].active) g_focusOrd++;

  // 边框
  if (t.st == ST_CONFIRM) {
    if ((millis() / 400) % 2 == 0) u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawFrame(2, 2, 124, 60);
  } else {
    u8g2.drawFrame(0, 0, 128, 64);
  }

  // 状态词
  u8g2.setFont(u8g2_font_helvB14_tf);
  const char* label = stateLabel(t.st);
  u8g2.drawStr((128 - u8g2.getStrWidth(label)) / 2, 26, label);

  // 任务名（关键：多任务时靠这一行区分）
  drawLabel(t.label, 44, 120);

  // 底部
  char buf[24];
  if (t.st == ST_WORKING || t.st == ST_THINKING) drawSpinner(12, 56, 6);
  else if (t.st == ST_DONE) {
    u8g2.drawLine(8, 56, 12, 60); u8g2.drawLine(12, 60, 19, 50);
  } else if (t.st == ST_CONFIRM || t.st == ST_ERROR) {
    u8g2.drawStr(9, 61, "!");
  }

  snprintf(buf, sizeof(buf), "%lus", (unsigned long)((millis() - t.updated) / 1000));
  u8g2.drawStr(24, 61, buf);

  if (g_activeCount > 1) {
    // 右下角：任务圆点，实心表示当前显示的那个
    int dx = 118;
    for (int k = g_activeCount - 1; k >= 0; k--) {
      if (k == g_focusOrd) u8g2.drawDisc(dx, 56, 2);
      else                 u8g2.drawCircle(dx, 56, 2);
      dx -= 8;
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "%ddBm", WiFi.RSSI());
    u8g2.drawStr(122 - u8g2.getStrWidth(buf), 61, buf);
  }

  u8g2.sendBuffer();
}

// ---------------- 灯环：按任务分段 ----------------
void drawLED() {
#if HAS_LED
  uint32_t now = millis();
  ring.clear();

  int n = countActive();
  if (n == 0) {
    for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, ring.Color(0, 0, 18));
    ring.show();
    return;
  }

  int seg = LED_COUNT / n;
  if (seg < 1) seg = 1;
  int idx = 0;

  for (int i = 0; i < MAX_TASKS && idx < n; i++) {
    if (!g_tasks[i].active) continue;
    State s = g_tasks[i].st;
    uint32_t c = stateColor(s);
    bool blink = (s == ST_CONFIRM) && ((now / 300) % 2 == 0);

    for (int j = 0; j < seg; j++) {
      int pos = idx * seg + j;
      if (pos >= LED_COUNT) break;
      ring.setPixelColor(pos, blink ? 0 : c);
    }
    idx++;
  }
  // 除不尽的余数灯补最后一段颜色
  for (int pos = n * seg; pos < LED_COUNT; pos++) {
    ring.setPixelColor(pos, ring.Color(0, 0, 10));
  }
  ring.show();
#endif
}

// ---------------- 初始化 ----------------
void setup() {
  Serial.begin(115200);
  tasksInit();

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();

#if HAS_LED
  ring.begin();
  ring.setBrightness(LED_BRIGHT);
  ring.clear();
  ring.show();
#endif

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(8, 30, "WiFi connecting...");
  u8g2.sendBuffer();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(300);

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
  }

  server.on("/", handleList);
  server.on("/state", handleState);
  server.on("/list", handleList);
  server.begin();
}

void loop() {
  server.handleClient();

  // 僵尸清理：普通任务 5 分钟、确认任务 30 分钟无更新就移除
  uint32_t now = millis();
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    uint32_t limit = (g_tasks[i].st == ST_CONFIRM) ? 1800000UL : 300000UL;
    if (now - g_tasks[i].updated > limit) g_tasks[i].active = false;
  }

  drawUI();
  drawLED();
  delay(30);
}
