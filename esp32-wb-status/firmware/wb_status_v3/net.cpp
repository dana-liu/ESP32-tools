#include "net.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

static Preferences  prefs;
static WebServer    portal(80);
static DNSServer    dns;

static ScanItem     g_scan[MAX_SCAN];
static int          g_scanCount = 0;
static char         g_savedSsid[MAX_SAVED][33];
static char         g_savedPass[MAX_SAVED][65];
static int          g_savedCount = 0;

static bool         g_portal = false;
static uint32_t     g_lastRetry = 0;
static int          g_retryIdx = 0;
static char         g_curSsid[33] = "";

// ---------------- 凭据存储 ----------------
static void loadCreds() {
  prefs.begin("wbnet", true);
  g_savedCount = prefs.getInt("n", 0);
  if (g_savedCount > MAX_SAVED) g_savedCount = MAX_SAVED;
  for (int i = 0; i < g_savedCount; i++) {
    String ks = "s" + String(i), kp = "p" + String(i);
    String s = prefs.getString(ks.c_str(), "");
    String p = prefs.getString(kp.c_str(), "");
    strncpy(g_savedSsid[i], s.c_str(), 32); g_savedSsid[i][32] = 0;
    strncpy(g_savedPass[i], p.c_str(), 64); g_savedPass[i][64] = 0;
  }
  prefs.end();
}

void netSaveCred(const char* ssid, const char* pass) {
  // 已存在就更新，否则追加（满了淘汰最旧的）
  int idx = -1;
  for (int i = 0; i < g_savedCount; i++)
    if (strcmp(g_savedSsid[i], ssid) == 0) { idx = i; break; }

  if (idx < 0) {
    if (g_savedCount < MAX_SAVED) {
      idx = g_savedCount++;
    } else {
      // 整体前移一位，挤掉最旧的
      for (int i = 0; i < MAX_SAVED - 1; i++) {
        strcpy(g_savedSsid[i], g_savedSsid[i + 1]);
        strcpy(g_savedPass[i], g_savedPass[i + 1]);
      }
      idx = MAX_SAVED - 1;
    }
  }
  strncpy(g_savedSsid[idx], ssid, 32); g_savedSsid[idx][32] = 0;
  strncpy(g_savedPass[idx], pass, 64); g_savedPass[idx][64] = 0;

  prefs.begin("wbnet", false);
  prefs.putInt("n", g_savedCount);
  for (int i = 0; i < g_savedCount; i++) {
    prefs.putString(("s" + String(i)).c_str(), g_savedSsid[i]);
    prefs.putString(("p" + String(i)).c_str(), g_savedPass[i]);
  }
  prefs.end();
}

int netSavedCount() { return g_savedCount; }
const char* netSavedSsid(int i) { return (i >= 0 && i < g_savedCount) ? g_savedSsid[i] : ""; }

void netForgetAll() {
  prefs.begin("wbnet", false);
  prefs.clear();
  prefs.end();
  g_savedCount = 0;
}

// ---------------- 扫描 ----------------
int netScan() {
  if (g_portal) return g_scanCount;        // 热点模式下用缓存，不打断配网
  if (WiFi.getMode() != WIFI_MODE_STA) WiFi.mode(WIFI_MODE_STA);
  delay(120);

  int n = WiFi.scanNetworks(false, false);
  g_scanCount = 0;
  if (n <= 0) return 0;

  for (int i = 0; i < n && g_scanCount < MAX_SCAN; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;

    // 同名去重，保留信号最强的
    bool dup = false;
    for (int k = 0; k < g_scanCount; k++)
      if (strcmp(g_scan[k].ssid, ssid.c_str()) == 0) { dup = true; break; }
    if (dup) continue;

    strncpy(g_scan[g_scanCount].ssid, ssid.c_str(), 32);
    g_scan[g_scanCount].ssid[32] = 0;
    g_scan[g_scanCount].rssi = WiFi.RSSI(i);
    g_scan[g_scanCount].enc  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    g_scanCount++;
  }
  WiFi.scanDelete();
  return g_scanCount;
}

const ScanItem* netScanItems() { return g_scan; }
int netScanCount() { return g_scanCount; }

// ---------------- 连接 ----------------
static bool tryConnect(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    strncpy(g_curSsid, ssid, 32); g_curSsid[32] = 0;
    return true;
  }
  WiFi.disconnect(true, false);
  return false;
}

void netConnectByIndex(int idx, const char* pass) {
  if (idx < 0 || idx >= g_scanCount) return;
  if (tryConnect(g_scan[idx].ssid, pass, 12000)) {
    netSaveCred(g_scan[idx].ssid, pass);
  }
}

bool netConnected()      { return WiFi.status() == WL_CONNECTED; }
bool netPortalActive()   { return g_portal; }
IPAddress netIP()        { return WiFi.localIP(); }
const char* netCurrentSsid() { return g_curSsid; }

void netRestart() { ESP.restart(); }

// ---------------- 兜底热点 ----------------
static String portalPage() {
  String h;
  h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>WB Status 配网</title><style>"
         "body{font-family:-apple-system,sans-serif;margin:0;padding:24px;background:#f5f5f4;color:#2c2c2a}"
         "h1{font-size:18px;margin:0 0 4px}p{font-size:13px;color:#5f5e5a;margin:0 0 20px}"
         "label{display:block;font-size:13px;margin:14px 0 6px;color:#444}"
         "select,input{width:100%;padding:10px;font-size:15px;border:1px solid #d3d1c7;"
         "border-radius:8px;box-sizing:border-box;background:#fff}"
         "button{width:100%;margin-top:20px;padding:12px;font-size:15px;border:0;"
         "border-radius:8px;background:#185fa5;color:#fff}"
         "</style></head><body>"
         "<h1>WorkBuddy 状态灯</h1><p>选择 WiFi 并输入密码</p>"
         "<form method='POST' action='/save'>");

  h += F("<label>WiFi</label><select name='ssid'>");
  for (int i = 0; i < g_scanCount; i++) {
    h += "<option value='";
    h += g_scan[i].ssid;
    h += "'>";
    h += g_scan[i].ssid;
    h += " (";
    h += String(g_scan[i].rssi);
    h += "dBm)</option>";
  }
  h += F("</select>");

  h += F("<label>密码</label>"
         "<input name='pass' type='password' placeholder='留空表示开放网络'>"
         "<button type='submit'>保存并连接</button></form></body></html>");
  return h;
}

static void startPortal() {
  if (g_portal) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());

  portal.on("/", []() { portal.send(200, "text/html; charset=utf-8", portalPage()); });
  portal.on("/save", []() {
    String s = portal.arg("ssid");
    String p = portal.arg("pass");
    if (s.length()) {
      netSaveCred(s.c_str(), p.c_str());
      portal.send(200, "text/html; charset=utf-8",
                  String(F("<meta charset='utf-8'><body style='font-family:-apple-system;padding:24px'>"
                           "<h1 style='font-size:18px'>已保存</h1><p style='font-size:14px;color:#5f5e5a'>"
                           "设备正在重启并连接新网络，热点即将关闭。</p></body>")));
      delay(1200);
      ESP.restart();
    } else {
      portal.send(400, "text/plain", "ssid required");
    }
  });
  portal.onNotFound([]() {
    portal.sendHeader("Location", "http://192.168.4.1/", true);
    portal.send(302, "text/plain", "");
  });
  portal.begin();

  g_portal = true;
}

// ---------------- 生命周期 ----------------
void netBegin() {
  loadCreds();

  int n = netScan();
  Serial.printf("[net] 扫描到 %d 个网络，已存 %d 组凭据\n", n, g_savedCount);

  // 按已存顺序依次尝试，能连上就停
  for (int i = 0; i < g_savedCount; i++) {
    Serial.printf("[net] 尝试 %s\n", g_savedSsid[i]);
    if (tryConnect(g_savedSsid[i], g_savedPass[i], 12000)) {
      Serial.printf("[net] 已连接 %s -> %s\n", g_savedSsid[i], WiFi.localIP().toString().c_str());
      return;
    }
  }

  // 全失败 → 开热点兜底
  Serial.println("[net] 全部失败，进入配网模式");
  startPortal();
}

void netLoop() {
  if (g_portal) {
    dns.processNextRequest();
    portal.handleClient();
    return;
  }

  // 掉线重连：每 15 秒轮一个已存网络
  if (!netConnected() && g_savedCount > 0 && millis() - g_lastRetry > 15000) {
    g_lastRetry = millis();
    g_retryIdx = (g_retryIdx + 1) % g_savedCount;
    tryConnect(g_savedSsid[g_retryIdx], g_savedPass[g_retryIdx], 8000);
  }
}
