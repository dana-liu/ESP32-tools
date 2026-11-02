#include "ui.h"
#include "net.h"
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <esp_heap_caps.h>

// ---------------- 图形对象 ----------------
static Arduino_DataBus* bus   = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, -1);
static Arduino_GFX*     panel = new Arduino_ST7789(bus, LCD_RST, 0, true, SCREEN_W, SCREEN_H);

// 离屏画布：整帧先画进内存，画完一次 flush() 推给屏幕。
// 如果直接往屏幕上画，每次 fillScreen() 都会让整屏黑一下再重画 —— 那就是
// 「屏幕每秒闪一下」的来源。走画布后每帧只有一次连续 SPI 传输，肉眼看不到闪。
class PsramCanvas : public Arduino_Canvas {
public:
  PsramCanvas(int16_t w, int16_t h, Arduino_G* out) : Arduino_Canvas(w, h, out) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (!_framebuffer) {
      // 240×240×2 = 115KB，优先放 PSRAM，别跟 WiFi 抢内部堆
      size_t s = (size_t)_width * _height * 2;
      _framebuffer = (uint16_t*)heap_caps_aligned_alloc(
          16, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return Arduino_Canvas::begin(speed);   // 已分配就直接初始化面板
  }
};

static Arduino_GFX* gfx = new PsramCanvas(SCREEN_W, SCREEN_H, panel);

// Arduino_GFX 1.6.x 不再继承 Adafruit_GFX（只剩 public Print），
// 而 U8g2_for_Adafruit_GFX 的 begin() 只接受 Adafruit_GFX&，
// 所以用一个极薄的桥接类把绘制调用转发过去。
//
// ⚠️ 这里必须直接转发 drawPixel / drawFastHLine / drawFastVLine，
//    并且让 startWrite / endWrite 变成空操作。
//    Adafruit_GFX 的 drawFastHLine 内部会调 startWrite() + writePixel()，
//    而 Arduino_GFX 的 drawPixel 自己又会 startWrite()/endWrite()；
//    如果桥接类把 startWrite 也转发下去，就会嵌套 beginWrite()/endWrite()，
//    把 SPI 的 CS/DC 状态机搞乱，表现为绘制时死锁、主循环直接卡住。
class GfxBridge : public Adafruit_GFX {
public:
  GfxBridge(Arduino_GFX* g) : Adafruit_GFX(g->width(), g->height()), _g(g) {}

  void drawPixel(int16_t x, int16_t y, uint16_t c) override { _g->drawPixel(x, y, c); }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    _g->drawFastHLine(x, y, w, c);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    _g->drawFastVLine(x, y, h, c);
  }

  // Arduino_GFX 每个绘制原语自己管理总线事务，这里不要再开一层
  void startWrite(void) override {}
  void endWrite(void) override {}

private:
  Arduino_GFX* _g;
};
static GfxBridge gfxBridge(gfx);

static U8G2_FOR_ADAFRUIT_GFX u8f;

// 中文字体：必须用收字完整的版本。
//   wqy12_t_gb2312      → 7757 字，覆盖日常用语（推荐）
//   wqy12_t_chinese1/2/3 → 只有 388 / 501 字，随便一个任务名就掉字
//                          （曾出现「已完成」只画出「成」、「WiFi 断开」掉「断」）
#define FONT_CN u8g2_font_wqy12_t_gb2312
#define FONT_SM u8g2_font_6x12_tf
#define FONT_MD u8g2_font_helvB12_tf

// ---------------- 界面状态 ----------------
enum Screen { SCR_MAIN, SCR_WIFI, SCR_PASS, SCR_MSG, SCR_APPROVE };

static Screen   g_scr = SCR_MAIN;
static bool     g_dirty = true;
static uint32_t g_lastDraw = 0;

static char     g_pass[PASS_MAX + 1] = "";
static int      g_passLen = 0;
static bool     g_shift = false;
static int      g_targetIdx = -1;          // 要连的扫描序号
static char     g_targetSsid[33] = "";

static char     g_msg1[40] = "", g_msg2[40] = "";
static uint32_t g_msgUntil = 0;

// 触摸去抖
static bool     g_down = false;
static int16_t  g_tx = -1, g_ty = -1;
static uint32_t g_downAt = 0;

// 审批
static Approval* g_ap = nullptr;
static int       g_apCount = 0;

// 电量
static int  g_batPct = -1;      // -1 = 还没读到
static bool g_charging = false;
static int  g_blLevel = 255;    // 当前背光 0-255

// 链路 / 夜间 / 熄屏
static bool     g_wifiOk    = true;
static bool     g_usbOk     = false;
static bool     g_stale     = false;
static bool     g_night     = false;
static bool     g_screenOff = false;
static bool     g_colorTest = false;    // 色彩自检中
static uint32_t g_boostUntil = 0;      // 夜间触摸后临时提亮到什么时候
static uint32_t g_lastTapMs = 0;
static int16_t  g_lastTapX  = -1, g_lastTapY = -1;

// ================= 基础工具 =================
const char* stateLabelCN(State s) {
  switch (s) {
    case ST_IDLE:     return "空闲";
    case ST_THINKING: return "思考中";
    case ST_WORKING:  return "执行中";
    case ST_CONFIRM:  return "需要确认";
    case ST_DONE:     return "已完成";
    case ST_ERROR:    return "出错";
    default:          return "空闲";
  }
}

const char* stateLabelEN(State s) {
  switch (s) {
    case ST_IDLE:     return "IDLE";
    case ST_THINKING: return "THINKING";
    case ST_WORKING:  return "WORKING";
    case ST_CONFIRM:  return "CONFIRM";
    case ST_DONE:     return "DONE";
    case ST_ERROR:    return "ERROR";
    default:          return "IDLE";
  }
}

uint16_t stateColor(State s) {
  switch (s) {
    case ST_THINKING: return C_GREEN;
    case ST_WORKING:  return C_BLUE;
    case ST_CONFIRM:  return C_RED;
    case ST_DONE:     return C_GREEN;
    case ST_ERROR:    return C_AMBER;
    default:          return C_MUTE;
  }
}

void uiMarkDirty() { g_dirty = true; }

static void txt(const char* s, int x, int y, uint16_t color, const uint8_t* font) {
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  u8f.setCursor(x, y);
  u8f.print(s);
}

static void txtRight(const char* s, int xRight, int y, uint16_t color, const uint8_t* font) {
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  int w = u8f.getUTF8Width(s);
  u8f.setCursor(xRight - w, y);
  u8f.print(s);
}

static void txtCenter(const char* s, int cx, int y, uint16_t color, const uint8_t* font) {
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  int w = u8f.getUTF8Width(s);
  u8f.setCursor(cx - w / 2, y);
  u8f.print(s);
}

// 居中显示 UTF-8 文本，超宽时按字符边界截断（不能按字节切，会切坏汉字）
static void drawLabel(const char* s, int y, int maxW) {
  if (!s || !*s) return;
  char buf[56];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;

  u8f.setFont(FONT_CN);
  u8f.setForegroundColor(C_TEXT);
  while (strlen(buf) > 0 && u8f.getUTF8Width(buf) > maxW) {
    int len = strlen(buf), i = len - 1;
    while (i > 0 && ((uint8_t)buf[i] & 0xC0) == 0x80) i--;
    buf[i] = 0;
  }
  int w = u8f.getUTF8Width(buf);
  u8f.setCursor((SCREEN_W - w) / 2, y);
  u8f.print(buf);
}

static void drawGear(int cx, int cy, int r, uint16_t c) {
  gfx->drawCircle(cx, cy, r, c);
  gfx->drawCircle(cx, cy, r / 3, c);
  for (int i = 0; i < 6; i++) {
    float a = i * 60.0 * PI / 180.0;
    gfx->drawLine(cx + cos(a) * (r + 1), cy + sin(a) * (r + 1),
                  cx + cos(a) * (r + 4), cy + sin(a) * (r + 4), c);
  }
}

// 电池图标：22×11，右侧带正极凸起；充电时整块变绿
static void drawBattery() {
  if (g_batPct < 0) return;

  const int bx = 162, by = 14, bw = 22, bh = 11;
  uint16_t c = g_charging ? C_GREEN
             : (g_batPct <= 10 ? C_RED : (g_batPct <= 25 ? C_AMBER : C_DIM));

  gfx->drawRect(bx, by, bw, bh, c);
  gfx->fillRect(bx + bw, by + 3, 2, 5, c);

  if (g_charging) {
    // 充电中：暗绿底 + 闪电，比填充电量条更直观
    gfx->fillRect(bx + 2, by + 2, bw - 4, bh - 4, RGB565(0x10, 0x2A, 0x12));
    gfx->drawLine(bx + 12, by + 1,  bx + 9,  by + 5,  C_TEXT);
    gfx->drawLine(bx + 9,  by + 5,  bx + 12, by + 5,  C_TEXT);
    gfx->drawLine(bx + 12, by + 5,  bx + 9,  by + 10, C_TEXT);
  } else {
    int fillW = (bw - 4) * g_batPct / 100;
    if (fillW > 0) gfx->fillRect(bx + 2, by + 2, fillW, bh - 4, c);
  }

  char p[8];
  snprintf(p, sizeof(p), "%d%%", g_batPct);
  txtRight(p, bx - 6, by + 9, c, FONT_SM);
}

// 背光 = 夜间/白天基线，再被低电量压低；熄屏时为 0
// 夜间只要有人碰屏幕就临时提亮，20 秒不动再回落
static void applyBrightness() {
  int bl;
  if (g_screenOff) {
    bl = 0;
  } else {
    bool boost = (g_boostUntil && millis() < g_boostUntil);
    bl = (g_night && !boost) ? BL_NIGHT : BL_DAY;
    if (!g_charging && g_batPct >= 0) {
      int cap = 255;
      if      (g_batPct <= 5)  cap = 45;
      else if (g_batPct <= 10) cap = 100;
      else if (g_batPct <= 20) cap = 170;
      if (cap < bl) bl = cap;
    }
  }
  if (bl != g_blLevel) {
    g_blLevel = bl;
    ledcWrite(LCD_BL, bl);
    Serial.printf("[ui] 背光 -> %d/255\n", bl);
  }
}

static void drawBack(int x, int y, uint16_t c) {
  gfx->drawLine(x + 10, y + 4, x + 2, y + 12, c);
  gfx->drawLine(x + 2, y + 12, x + 10, y + 20, c);
}

// ================= 触摸 =================
static bool touchRaw(int16_t* x, int16_t* y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write((uint8_t)0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)6) != 6) return false;

  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if ((d[1] & 0x0F) == 0) return false;

  *x = ((d[2] & 0x0F) << 8) | d[3];
  *y = ((d[4] & 0x0F) << 8) | d[5];
  return true;
}

// ================= 各屏幕绘制 =================
// 把「多久以前」压成短文本，给已完成的任务用
static void agoText(char* out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60)         snprintf(out, n, "刚刚");
  else if (s < 3600)  snprintf(out, n, "%lu 分钟前", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(out, n, "%lu 小时前", (unsigned long)(s / 3600));
  else                snprintf(out, n, "%lu 天前",   (unsigned long)(s / 86400));
}

static void drawMain(Task* tasks, int count) {
  gfx->fillScreen(C_BG);

  char hdr[24];
  // 顶栏优先级：USB 在线 > WiFi 断开 > 离线 > 电量低 > 任务数
  if (g_usbOk) {
    txt("USB", 14, 26, C_BLUE, FONT_CN);
  } else if (!g_wifiOk) {
    txt("WiFi 断开", 14, 26, C_RED, FONT_CN);
  } else if (g_stale) {
    txt("离线", 14, 26, C_AMBER, FONT_CN);
  } else if (g_batPct >= 0 && g_batPct <= 5 && !g_charging) {
    txt("电量低 请充电", 14, 26, C_RED, FONT_CN);
  } else {
    snprintf(hdr, sizeof(hdr), "%d 个任务", count);
    txt(hdr, 14, 26, C_TEXT, FONT_CN);
  }
  drawBattery();
  drawGear(224, 20, 9, C_DIM);
  gfx->drawFastHLine(0, 40, SCREEN_W, C_LINE);

  if (count == 0) {
    txtCenter("空闲", SCREEN_W / 2, 120, C_DIM, FONT_CN);
    txtCenter("等待 WorkBuddy 任务", SCREEN_W / 2, 146, C_MUTE, FONT_CN);
    return;
  }

  for (int i = 0; i < count && i < MAX_TASKS; i++) {
    int y = 44 + i * 48;
    uint16_t c = stateColor(tasks[i].st);
    // 离线时整列表置灰，一眼看出「数据不新鲜了」。
    // 注意 USB 也算在线 —— 只看 WiFi 的话，插着数据线会把整张表刷成灰色。
    bool dead = (!g_wifiOk && !g_usbOk) || g_stale;
    if (dead) c = C_MUTE;

    gfx->fillRect(12, y + 8, 4, 32, c);
    txt(tasks[i].label, 24, y + 22, dead ? C_MUTE : C_TEXT, FONT_CN);
    txtRight(stateLabelCN(tasks[i].st), 228, y + 22, c, FONT_CN);

    char sub[56], tail[24];
    uint32_t el = millis() - tasks[i].updated;
    // 只有进行中的任务才读秒；已经结束的显示「多久以前」，
    // 否则一条「已完成」的任务会永远挂着一个越涨越大的秒数
    bool active = (tasks[i].st == ST_THINKING ||
                   tasks[i].st == ST_WORKING  ||
                   tasks[i].st == ST_CONFIRM);
    if (active) {
      snprintf(tail, sizeof(tail), "%lus", (unsigned long)(el / 1000));
    } else {
      agoText(tail, sizeof(tail), el);
    }
    if (tasks[i].tool[0])
      snprintf(sub, sizeof(sub), "%s  %s", tasks[i].tool, tail);
    else
      snprintf(sub, sizeof(sub), "%s", tail);
    txt(sub, 24, y + 40, C_MUTE, FONT_CN);
  }
}

static void drawWifi() {
  gfx->fillScreen(C_BG);
  drawBack(10, 8, C_TEXT);
  txt("选择 WiFi", 38, 24, C_TEXT, FONT_CN);
  gfx->drawFastHLine(0, 40, SCREEN_W, C_LINE);

  int n = netScanCount();
  if (n == 0) {
    txtCenter("没有扫描到网络", SCREEN_W / 2, 120, C_DIM, FONT_CN);
    txtCenter("请确认是 2.4GHz", SCREEN_W / 2, 146, C_MUTE, FONT_CN);
    return;
  }

  for (int i = 0; i < n && i < 6; i++) {
    int y = 46 + i * 32;
    gfx->fillRect(8, y, 224, 28, C_CARD);
    const ScanItem& it = netScanItems()[i];
    txt(it.ssid, 18, y + 19, C_TEXT, FONT_CN);
    if (it.enc) txtRight("锁", 224, y + 19, C_MUTE, FONT_CN);
  }
}

static const char* KB[4] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl.",
  "zxcvbnm_-"
};

static void drawPass() {
  gfx->fillScreen(C_BG);
  drawBack(10, 8, C_TEXT);
  txt("输入密码", 38, 24, C_TEXT, FONT_CN);
  gfx->drawFastHLine(0, 40, SCREEN_W, C_LINE);

  txt(g_targetSsid, 14, 60, C_DIM, FONT_CN);

  gfx->fillRect(8, 68, 224, 34, C_CARD);
  char shown[PASS_MAX + 1];
  if (g_passLen > 16) {
    // 太长就显示尾部
    strncpy(shown, g_pass + g_passLen - 16, 16);
    shown[16] = 0;
  } else {
    strcpy(shown, g_pass);
  }
  txt(shown, 16, 90, C_TEXT, FONT_SM);
  if ((millis() / 500) % 2 == 0) {
    u8f.setFont(FONT_SM);
    int w = u8f.getUTF8Width(shown);
    gfx->drawFastVLine(16 + w + 2, 76, 18, C_TEXT);
  }

  for (int r = 0; r < 4; r++) {
    int y = 108 + r * 28;
    for (int c = 0; c < 10; c++) {
      char ch = KB[r][c];
      if (g_shift && ch >= 'a' && ch <= 'z') ch -= 32;
      int x = c * 24;
      gfx->fillRect(x + 1, y, 22, 25, C_KEY);
      char s[2] = { ch, 0 };
      txtCenter(s, x + 12, y + 18, C_TEXT, FONT_MD);
    }
  }

  // 功能键
  int fy = 222;
  gfx->fillRect(4, fy, 48, 16, g_shift ? C_BLUE : C_KEY);
  txtCenter("AB", 28, fy + 13, C_TEXT, FONT_SM);
  gfx->fillRect(56, fy, 56, 16, C_KEY);
  txtCenter("删除", 84, fy + 13, C_TEXT, FONT_CN);
  gfx->fillRect(116, fy, 120, 16, C_BLUE);
  txtCenter("连接", 176, fy + 13, C_TEXT, FONT_CN);
}

static void drawMsg() {
  gfx->fillScreen(C_BG);
  txtCenter(g_msg1, SCREEN_W / 2, 110, C_TEXT, FONT_CN);
  if (g_msg2[0]) txtCenter(g_msg2, SCREEN_W / 2, 136, C_DIM, FONT_CN);
  // 转圈
  static uint8_t a = 0;
  a += 9;
  for (int i = 0; i < 8; i++) {
    float ang = (a + i * 45) * PI / 180.0;
    gfx->fillCircle(SCREEN_W / 2 + cos(ang) * 18, 168 + sin(ang) * 18,
                    i < 3 ? 2 : 1, i < 3 ? C_TEXT : C_MUTE);
  }
}

// ================= 审批提醒页 =================
// 只做提醒：真正的批准/拒绝在电脑客户端完成，这里点一下只是关掉提示
static void drawApprove() {
  Approval* a = g_ap;
  gfx->fillScreen(C_BG);

  uint32_t left = (a->deadline > millis()) ? (a->deadline - millis()) / 1000 : 0;
  txt("需要确认", 14, 26, C_RED, FONT_CN);
  char buf[12];
  snprintf(buf, sizeof(buf), "%lus", (unsigned long)left);
  txtRight(buf, 228, 26, left <= 1 ? C_RED : C_MUTE, FONT_SM);
  gfx->drawFastHLine(0, 36, SCREEN_W, C_LINE);

  // 是哪个任务要审批 —— 多任务时靠这一行区分
  drawLabel(a->label, 58, 200);

  // 工具名 + 队列里还有几个
  txt(a->tool, 14, 74, C_DIM, FONT_SM);
  if (g_apCount > 1) {
    snprintf(buf, sizeof(buf), "共 %d 个", g_apCount);
    txtRight(buf, 228, 74, C_AMBER, FONT_CN);
  }

  gfx->fillRect(8, 80, 224, 68, C_CARD);

  // 命令按 34 字符/行折行，最多 3 行
  int n = strlen(a->cmd);
  for (int i = 0; i < 3; i++) {
    int off = i * 34;
    if (off >= n) break;
    char line[44];
    int len = n - off;
    if (len > 34) len = 34;
    strncpy(line, a->cmd + off, len);
    line[len] = 0;
    if (i == 2 && off + len < n) strcat(line, "...");
    txt(line, 16, 98 + i * 18, C_TEXT, FONT_SM);
  }

  // 提示区：不再有按钮，决策只在电脑上做
  gfx->fillRect(8, 156, 224, 62, C_CARD);
  gfx->drawRect(8, 156, 224, 62, C_LINE);
  txtCenter("请在电脑上确认", 120, 182, C_AMBER, FONT_CN);
  txtCenter("轻触屏幕关闭提示", 120, 206, C_MUTE, FONT_CN);
}

// ================= 交互 =================
static void goWifi() {
  g_scr = SCR_WIFI;
  g_dirty = true;
  netScan();          // 阻塞约 2 秒，进入前先画一帧
}

static void goMain() {
  g_scr = SCR_MAIN;
  g_dirty = true;
}

static void showMsg(const char* l1, const char* l2, uint32_t ms) {
  strncpy(g_msg1, l1, sizeof(g_msg1) - 1);
  strncpy(g_msg2, l2, sizeof(g_msg2) - 1);
  g_msgUntil = millis() + ms;
  g_scr = SCR_MSG;
  g_dirty = true;
}

static void onTap(int16_t x, int16_t y) {
  // 熄屏时任意触摸先亮屏（不然点不到东西）
  if (g_screenOff) {
    g_screenOff = false;
    applyBrightness();
    g_dirty = true;
    return;
  }

  if (g_scr == SCR_APPROVE) {
    // 只做提醒：点任意位置关掉提示，真正的决策在电脑客户端
    if (g_ap && g_ap->decision < 0) g_ap->decision = -3;
    return;
  }

  if (g_scr == SCR_MAIN) {
    if (x > 200 && y < 40) {          // 齿轮
      showMsg("正在扫描", "", 400);
      drawMsg();
      gfx->flush();          // 后面 netScan() 要阻塞两秒，先把这帧推上去
      goWifi();
      return;
    }
    // 双击空白处熄屏
    if (millis() - g_lastTapMs < 400 &&
        abs(x - g_lastTapX) < 50 && abs(y - g_lastTapY) < 50) {
      g_screenOff = true;
      g_lastTapMs = 0;
      applyBrightness();
      return;
    }
    g_lastTapMs = millis();
    g_lastTapX = x;
    g_lastTapY = y;
    return;
  }

  if (g_scr == SCR_WIFI) {
    if (x < 38 && y < 40) { goMain(); return; }
    int n = netScanCount();
    for (int i = 0; i < n && i < 6; i++) {
      int ry = 46 + i * 32;
      if (y >= ry && y <= ry + 28) {
        g_targetIdx = i;
        strncpy(g_targetSsid, netScanItems()[i].ssid, 32);
        g_pass[0] = 0;
        g_passLen = 0;
        g_scr = SCR_PASS;
        g_dirty = true;
        return;
      }
    }
    return;
  }

  if (g_scr == SCR_PASS) {
    if (x < 38 && y < 40) { goMain(); return; }

    // 功能键区
    if (y >= 220 && y <= 238) {
      if (x < 54) { g_shift = !g_shift; g_dirty = true; return; }
      if (x < 114) {
        if (g_passLen > 0) g_pass[--g_passLen] = 0;
        g_dirty = true;
        return;
      }
      // 连接
      showMsg("正在连接", g_targetSsid, 20000);
      drawMsg();
      gfx->flush();          // 连接要阻塞十几秒，先推一帧
      netConnectByIndex(g_targetIdx, g_pass);
      if (netConnected()) {
        showMsg("已连接", netCurrentSsid(), 1500);
      } else {
        showMsg("连接失败", "检查密码或信号", 2500);
      }
      return;
    }

    // 键盘区（4 行，每行 28px，从 y=108 开始）
    if (y < 108 || y >= 220) return;
    int r = (y - 108) / 28;
    int c = x / 24;
    if (r >= 0 && r < 4 && c >= 0 && c < 10 && g_passLen < PASS_MAX) {
      char ch = KB[r][c];
      if (g_shift && ch >= 'a' && ch <= 'z') ch -= 32;
      g_pass[g_passLen++] = ch;
      g_pass[g_passLen] = 0;
      g_dirty = true;
    }
    return;
  }
}

// ================= 生命周期 =================
void uiBegin() {
  // 背光 PWM（ESP32 core 3.x 的 ledcAttach 直接按引脚走）
  ledcAttach(LCD_BL, 5000, 8);
  ledcWrite(LCD_BL, 255);
  g_blLevel = 255;

  gfx->begin();
  gfx->fillScreen(C_BG);
  gfx->flush();

  {
    void* fb = (void*)((Arduino_Canvas*)gfx)->getFramebuffer();
    bool inPsram = ((uintptr_t)fb >> 28) == 0x3;    // ESP32-S3 外部 RAM 从 0x3C000000 起
    Serial.printf("[ui] 画布 %s @%p  内部堆 %u\n",
                  inPsram ? "PSRAM" : "内部RAM", fb, ESP.getFreeHeap());
  }

  u8f.begin(gfxBridge);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);
  u8f.setForegroundColor(C_TEXT);
  u8f.setBackgroundColor(C_BG);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  if (TOUCH_RST >= 0) {
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(60);
  }
  g_dirty = true;
}

void uiTick(Task* tasks, int count) {
  // 色彩自检：触摸任意位置退出
  if (g_colorTest) {
    int16_t cx, cy;
    if (touchRaw(&cx, &cy)) {
      g_down = true;
    } else if (g_down) {
      g_down = false;
      g_colorTest = false;
      g_dirty = true;
    }
    return;
  }

  // 有审批请求就抢占屏幕
  if (g_ap && g_ap->active && g_scr != SCR_APPROVE) {
    g_scr = SCR_APPROVE;
    g_dirty = true;
  }

  // 触摸
  int16_t tx, ty;
  bool touching = touchRaw(&tx, &ty);
  if (touching && !g_down) {
    g_down = true; g_downAt = millis(); g_tx = tx; g_ty = ty;
    // 夜间：一碰就提亮，方便看清；20 秒不动再回落
    if (g_night) {
      g_boostUntil = millis() + BL_BOOST_MS;
      applyBrightness();
    }
  } else if (!touching && g_down) {
    g_down = false;
    if (millis() - g_downAt > 30 && g_tx >= 0) {
      onTap(g_tx, g_ty);
    }
  }

  // 夜间临时提亮到期 → 回落到夜间亮度
  if (g_boostUntil && millis() > g_boostUntil) {
    g_boostUntil = 0;
    applyBrightness();
  }

  // 消息页超时回主页
  if (g_scr == SCR_MSG && millis() > g_msgUntil) goMain();

  // 审批结束回主页
  if (g_scr == SCR_APPROVE && (!g_ap || !g_ap->active)) goMain();

  // 熄屏时不重绘，省电（触摸仍在上面的逻辑里处理）
  if (g_screenOff) { g_lastDraw = millis(); return; }

  // 按需重绘
  uint32_t now = millis();
  bool need;
  if (g_scr == SCR_MSG || g_scr == SCR_APPROVE) need = (now - g_lastDraw > 100);
  else need = (g_dirty || (now - g_lastDraw > 1000));
  if (!need) return;

  g_dirty = false;
  g_lastDraw = now;

  switch (g_scr) {
    case SCR_MAIN:    drawMain(tasks, count); break;
    case SCR_WIFI:    drawWifi();   break;
    case SCR_PASS:    drawPass();   break;
    case SCR_MSG:     drawMsg();    break;
    case SCR_APPROVE: drawApprove(); break;
  }
  gfx->flush();          // 整帧画完一次性推屏，中间不会出现黑屏
}

void uiSetApproval(Approval* a) {
  if (a == g_ap) return;          // 同一个槽位不用重画
  g_ap = a;
  g_dirty = true;
}

void uiSetApprovalCount(int pending) {
  if (pending == g_apCount) return;
  g_apCount = pending;
  g_dirty = true;
}

void uiSetBattery(int pct, bool charging) {
  bool changed = (pct != g_batPct) || (charging != g_charging);
  g_batPct = pct;
  g_charging = charging;
  applyBrightness();
  if (changed) g_dirty = true;
}

void uiSetLink(bool wifiOk, bool usbOk, bool stale) {
  if (wifiOk == g_wifiOk && usbOk == g_usbOk && stale == g_stale) return;
  g_wifiOk = wifiOk;
  g_usbOk  = usbOk;
  g_stale  = stale;
  g_dirty  = true;
}

void uiSetNight(bool night) {
  if (night == g_night) return;
  g_night = night;
  applyBrightness();
}

// ================= 色彩自检 =================
// 从上到下 6 条：红 / 绿 / 蓝 / 白 / 黑 / 界面背景色
// 用串口命令 CTEST 触发，触摸任意位置退出
void uiColorTest() {
  static const uint16_t bars[6] = {
    RGB565(0xFF, 0x00, 0x00),
    RGB565(0x00, 0xFF, 0x00),
    RGB565(0x00, 0x00, 0xFF),
    RGB565(0xFF, 0xFF, 0xFF),
    RGB565(0x00, 0x00, 0x00),
    C_BG,
  };
  for (int i = 0; i < 6; i++) {
    gfx->fillRect(0, i * 40, SCREEN_W, 40, bars[i]);
  }
  gfx->flush();
  g_colorTest = true;
}
