/*
 * WorkBuddy 桌面状态灯 v3
 * 目标硬件：微雪 ESP32-S3-Touch-LCD-1.54（1.54" 240x240 + 触摸 + 电池）
 *
 * 功能：
 *   1. 显示最多 4 个并行任务的实时状态
 *   2. 触摸屏上直接选 WiFi / 输密码，凭据存 NVS
 *   3. 连不上自动开热点兜底（手机配网）
 *   4. 对 Mac 暴露 HTTP 接口，供 WorkBuddy hook 推送状态
 *
 * 依赖库（库管理器安装）：
 *   - GFX Library for Arduino  (moononournation)
 *   - U8g2_for_Adafruit_GFX    (olikraus)
 *   - U8g2                     (olikraus)
 */

#include "config.h"
#include "net.h"
#include "ui.h"
#include "link.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

WebServer server(80);
bool      g_serverUp = false;

Task g_tasks[MAX_TASKS];
Task g_view[MAX_TASKS];
int  g_viewCount = 0;

Approval g_aps[MAX_APPROVALS];
static int g_apFocus = -1;

// 链路活性
static uint32_t g_lastHttp     = 0;
static bool     g_hasHeartbeat = false;   // Mac 端有心跳脚本时才按 90 秒判离线
static uint32_t g_nextClock    = 0;

static void touchHttp() { g_lastHttp = millis(); }

// ---------------- 任务表 ----------------
static void tasksInit() {
  for (int i = 0; i < MAX_TASKS; i++) {
    g_tasks[i].active = false;
    g_tasks[i].id[0] = 0;
    g_tasks[i].label[0] = 0;
    g_tasks[i].tool[0] = 0;
    g_tasks[i].st = ST_NONE;
    g_tasks[i].updated = 0;
  }
}

static int findTask(const char* id) {
  for (int i = 0; i < MAX_TASKS; i++)
    if (g_tasks[i].active && strcmp(g_tasks[i].id, id) == 0) return i;
  return -1;
}

static int prio(State s) {
  switch (s) {
    case ST_CONFIRM:  return 5;
    case ST_ERROR:    return 4;
    case ST_WORKING:  return 3;
    case ST_THINKING: return 2;
    case ST_DONE:     return 1;
    default:          return 0;
  }
}

static int allocTask() {
  for (int i = 0; i < MAX_TASKS; i++)
    if (!g_tasks[i].active) return i;

  // 满了：淘汰优先级最低的；同级淘汰最久没更新的。
  // 注意是 -idle 不是 +idle —— 取最小值时才不会反过来淘汰掉最新的那个。
  int victim = 0, worst = 0x7FFFFFFF;
  for (int i = 0; i < MAX_TASKS; i++) {
    int idle = (int)((millis() - g_tasks[i].updated) / 1000);
    int score = prio(g_tasks[i].st) * 10000 - idle;
    if (score < worst) { worst = score; victim = i; }
  }
  return victim;
}

static State parseState(const String& s) {
  if (s == "idle")     return ST_IDLE;
  if (s == "thinking") return ST_THINKING;
  if (s == "working")  return ST_WORKING;
  if (s == "confirm")  return ST_CONFIRM;
  if (s == "done")     return ST_DONE;
  if (s == "error")    return ST_ERROR;
  return ST_IDLE;
}

static void updateTask(const String& sid, State st, const String& tool, const String& label) {
  int idx = findTask(sid.c_str());

  if (st == ST_NONE) {                       // st=off
    if (idx >= 0) g_tasks[idx].active = false;
    uiMarkDirty();
    return;
  }

  if (idx < 0) {
    idx = allocTask();
    g_tasks[idx].active = true;
    strncpy(g_tasks[idx].id, sid.c_str(), sizeof(g_tasks[idx].id) - 1);
    g_tasks[idx].id[sizeof(g_tasks[idx].id) - 1] = 0;
    g_tasks[idx].label[0] = 0;
  }

  Task& t = g_tasks[idx];
  if (label.length()) {
    strncpy(t.label, label.c_str(), sizeof(t.label) - 1);
    t.label[sizeof(t.label) - 1] = 0;
  }
  // 工具名要无条件覆盖：任务结束时 hook 会传空串，
  // 只在非空时才写的话，屏幕上会一直挂着上一个工具名
  strncpy(t.tool, tool.c_str(), sizeof(t.tool) - 1);
  t.tool[sizeof(t.tool) - 1] = 0;
  if (t.st != st) t.st = st;
  t.updated = millis();
  uiMarkDirty();
}

static int countActive() {
  int n = 0;
  for (int i = 0; i < MAX_TASKS; i++) if (g_tasks[i].active) n++;
  return n;
}

// 整理成紧凑数组：优先级降序；同优先级按最近活跃降序
static void buildView() {
  g_viewCount = 0;
  for (int i = 0; i < MAX_TASKS; i++)
    if (g_tasks[i].active) g_view[g_viewCount++] = g_tasks[i];

  for (int i = 1; i < g_viewCount; i++) {
    Task key = g_view[i];
    int j = i - 1;
    while (j >= 0) {
      int pj = prio(g_view[j].st), pk = prio(key.st);
      bool keyFirst = (pj < pk) || (pj == pk && g_view[j].updated < key.updated);
      if (!keyFirst) break;
      g_view[j + 1] = g_view[j];
      j--;
    }
    g_view[j + 1] = key;
  }
}

// ---------------- HTTP ----------------
static void handleState() {
  touchHttp();
  String sid = server.arg("sid");
  String st  = server.arg("st");
  String tl  = server.arg("tool");
  String lb  = server.arg("label");

  if (sid.length() == 0) sid = "default";

  if (st == "off") updateTask(sid, ST_NONE, "", "");
  else if (st.length()) updateTask(sid, parseState(st), tl, lb);

  char resp[32];
  snprintf(resp, sizeof(resp), "ok %d task(s)", countActive());
  server.send(200, "text/plain", resp);
}

static String buildListJson() {
  String j = "[";
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    if (j.length() > 1) j += ",";
    j += "{\"sid\":\"";   j += g_tasks[i].id;
    j += "\",\"label\":\""; j += g_tasks[i].label;
    j += "\",\"st\":\"";  j += stateLabelEN(g_tasks[i].st);
    j += "\",\"tool\":\""; j += g_tasks[i].tool;
    j += "\"}";
  }
  j += "]";
  return j;
}

static void handleList() {
  touchHttp();
  server.send(200, "application/json", buildListJson());
}

// ---------------- 审批队列 ----------------
static void handlePing() {
  touchHttp();
  g_hasHeartbeat = true;
  server.send(200, "text/plain", "pong");
}

static void pushApproval(const String& req, const String& tool,
                         const String& label, const String& cmd) {
  // 找空槽；满了就踢掉最早到期的那个
  int slot = -1;
  for (int i = 0; i < MAX_APPROVALS; i++)
    if (!g_aps[i].active) { slot = i; break; }
  if (slot < 0) {
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < MAX_APPROVALS; i++)
      if (g_aps[i].deadline < oldest) { oldest = g_aps[i].deadline; slot = i; }
  }

  Approval& a = g_aps[slot];
  strncpy(a.req,   req.c_str(),   sizeof(a.req)   - 1); a.req[sizeof(a.req) - 1] = 0;
  strncpy(a.tool,  tool.c_str(),  sizeof(a.tool)  - 1); a.tool[sizeof(a.tool) - 1] = 0;
  strncpy(a.label, label.c_str(), sizeof(a.label) - 1); a.label[sizeof(a.label) - 1] = 0;
  strncpy(a.cmd,   cmd.c_str(),   sizeof(a.cmd)   - 1); a.cmd[sizeof(a.cmd) - 1] = 0;
  a.deadline = millis() + APPROVAL_MS;
  a.clearAt  = 0;
  a.decision = -1;
  a.active   = true;

  uiMarkDirty();
  Serial.printf("[approve] %s | %s | %s\n", a.label, a.tool, a.cmd);
}

// 1 = 批准, 0 = 拒绝, -1 = 待定, -2 = 没有这条请求
static int queryDecision(const String& req) {
  for (int i = 0; i < MAX_APPROVALS; i++) {
    Approval& a = g_aps[i];
    if (!a.active || req != a.req) continue;
    if (a.decision < 0) return -1;
    return a.decision == 1 ? 1 : 0;
  }
  return -2;
}

static void handleApprove() {
  touchHttp();
  String req   = server.arg("req");
  String tool  = server.arg("tool");
  String label = server.arg("label");
  String cmd   = server.arg("cmd");
  if (!req.length()) { server.send(400, "text/plain", "req required"); return; }

  pushApproval(req, tool, label, cmd);
  server.send(200, "text/plain", "ok");
}

static void handleDecision() {
  touchHttp();
  int d = queryDecision(server.arg("req"));
  server.send(200, "text/plain",
              d == 1 ? "allow" : d == 0 ? "deny" : d == -1 ? "pending" : "none");
}

// ---------------- USB 串口通道（link.cpp 的回调）----------------
void linkOnActivity() {
  touchHttp();          // 串口有动静也算链路活着，避免误判离线
  g_hasHeartbeat = true;
}

void linkApplyState(const char* sid, const char* st, const char* tool, const char* label) {
  String id(sid);
  if (!id.length()) id = "default";
  if (!strcmp(st, "off")) updateTask(id, ST_NONE, "", "");
  else                    updateTask(id, parseState(String(st)), String(tool), String(label));
}

void linkApplyApprove(const char* req, const char* tool, const char* label, const char* cmd) {
  pushApproval(String(req), String(tool), String(label), String(cmd));
}

int linkQueryDecision(const char* req) { return queryDecision(String(req)); }

String linkBuildList() { return buildListJson(); }

void linkColorTest() { uiColorTest(); }

// -1 = 按时间自动，1 = 强制夜间（调试用）
static int8_t g_nightForce = -1;

void linkSetNight(int on) {
  g_nightForce = on ? 1 : -1;      // 传 0 就是「回到自动」
  uiSetNight(on != 0);
}

// USB 模式下没有 NTP，让 Mac 把时间推过来，夜间定时才有意义
void linkSetTime(long epoch) {
  struct timeval tv;
  tv.tv_sec  = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  setenv("TZ", "CST-8", 1);        // 设备本地时间按东八区
  tzset();
}

static void startStatusServer() {
  if (g_serverUp) return;
  server.on("/", handleList);
  server.on("/list", handleList);
  server.on("/state", handleState);
  server.on("/approve", handleApprove);
  server.on("/decision", handleDecision);
  server.on("/ping", handlePing);
  server.begin();
  if (MDNS.begin("wbstatus")) MDNS.addService("http", "tcp", 80);
  g_serverUp = true;

  // 夜间调光要用本地时间，连上 WiFi 后对一次 NTP
  configTzTime("CST-8", "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");

  Serial.printf("[http] 已启动 http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---------------- 电池 ----------------
static uint32_t g_batNext = 0;

// 锂电池放电曲线，比线性映射准得多（3.7V 标称其实还有 40%）
static int lipoPct(float v) {
  static const float tab[10][2] = {
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 70}, {3.80f, 55},
    {3.70f, 40},  {3.60f, 25}, {3.50f, 12}, {3.40f, 5},  {3.30f, 0}
  };
  if (v >= tab[0][0]) return 100;
  if (v <= tab[9][0]) return 0;
  for (int i = 0; i < 9; i++) {
    if (v >= tab[i + 1][0]) {
      float t = (v - tab[i + 1][0]) / (tab[i][0] - tab[i + 1][0]);
      return (int)(tab[i + 1][1] + t * (tab[i][1] - tab[i + 1][1]) + 0.5f);
    }
  }
  return 0;
}

static void updateBattery() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) {          // 多次采样取平均，抑制抖动
    mv += analogReadMilliVolts(PIN_BAT_ADC);
    delay(2);
  }
  float v = (mv / 8.0f / 1000.0f) * BAT_DIV;
  int  pct = lipoPct(v);
  bool chg = (digitalRead(PIN_CHG_STAT) == LOW);   // 多数充电 IC：低电平 = 正在充
  uiSetBattery(pct, chg);
  Serial.printf("[bat] %.2fV  %d%%  charging=%d\n", v, pct, chg);
}

// ---------------- 生命周期 ----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BAT_EN, OUTPUT);
  digitalWrite(BAT_EN, HIGH);          // 电池供电保持
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);

  // 功放常开既费电又有底噪，当前固件没用到音频，先关掉
  pinMode(PIN_PA_CTRL, OUTPUT);
  digitalWrite(PIN_PA_CTRL, LOW);

  tasksInit();
  for (int i = 0; i < MAX_APPROVALS; i++) {
    g_aps[i].active = false;
    g_aps[i].decision = -1;
    g_aps[i].clearAt = 0;
  }
  uiBegin();
  uiSetApproval(nullptr);
  updateBattery();
  netBegin();
  linkBegin();               // USB 串口通道，不依赖 WiFi

  g_lastHttp = millis();     // 从启动开始算，避免一上电就判离线

  if (netConnected()) startStatusServer();
}

void loop() {
  netLoop();
  linkLoop();                // 插线时走 USB，拔线时这行什么也不做

  if (netConnected() && !g_serverUp) startStatusServer();
  if (g_serverUp) server.handleClient();

  // 电量：每 30 秒读一次
  if (millis() > g_batNext) {
    g_batNext = millis() + 30000;
    updateBattery();
  }

  // 每分钟对一次本地时间，决定要不要进夜间亮度
  if (millis() > g_nextClock) {
    g_nextClock = millis() + 60000;
    struct tm t;
    if (getLocalTime(&t, 0) && g_nightForce < 0) {   // 强制模式下不覆盖
      int m = t.tm_hour * 60 + t.tm_min;
      uiSetNight((m >= NIGHT_START_MIN) || (m < NIGHT_END_MIN));
    }
  }

  // 链路活性：WiFi 断了立刻标红；长时间没请求标「离线」
  {
    bool wifiOk = netConnected();
    bool usbOk  = Serial.isPlugged();     // 数据线插着就显示 USB，别误报「WiFi 断开」
    uint32_t limit = g_hasHeartbeat ? OFFLINE_MS_HB : OFFLINE_MS_NOHB;
    bool stale = wifiOk && (millis() - g_lastHttp > limit);
    uiSetLink(wifiOk, usbOk, stale);
  }

  // 审批队列：过期清理
  int pending = 0;
  for (int i = 0; i < MAX_APPROVALS; i++) {
    Approval& a = g_aps[i];
    if (!a.active) continue;
    if (a.decision == -3) {              // 用户点掉了提醒，直接清掉
      a.active = false;
    } else if (a.decision < 0) {
      if (millis() > a.deadline) {
        a.active = false;
        a.decision = -2;                 // 超时自动消失
        Serial.printf("[approve] timeout %s\n", a.req);
      } else {
        pending++;
      }
    } else {
      if (a.clearAt == 0) a.clearAt = millis() + 2000;
      else if (millis() > a.clearAt) a.active = false;
    }
  }

  // 焦点：先显示刚决定的（让用户看到结果），否则显示最早到期的
  int focus = -1;
  for (int i = 0; i < MAX_APPROVALS; i++)
    if (g_aps[i].active && g_aps[i].decision >= 0) { focus = i; break; }
  if (focus < 0) {
    uint32_t best = 0xFFFFFFFF;
    for (int i = 0; i < MAX_APPROVALS; i++)
      if (g_aps[i].active && g_aps[i].decision < 0 && g_aps[i].deadline < best) {
        best = g_aps[i].deadline;
        focus = i;
      }
  }
  g_apFocus = focus;
  uiSetApproval(focus >= 0 ? &g_aps[focus] : nullptr);
  uiSetApprovalCount(pending);

  // 僵尸清理：普通 5 分钟、确认 30 分钟
  uint32_t now = millis();
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!g_tasks[i].active) continue;
    uint32_t limit = (g_tasks[i].st == ST_CONFIRM) ? 1800000UL : 300000UL;
    if (now - g_tasks[i].updated > limit) {
      g_tasks[i].active = false;
      uiMarkDirty();
    }
  }

  buildView();
  uiTick(g_view, g_viewCount);

  delay(20);
}
