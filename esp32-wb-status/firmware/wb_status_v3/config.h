#pragma once
#include <stdint.h>

/* ============================================================
 *  硬件配置
 *  ⚠️ 标了 ★ 的需要你核对：微雪没有公开这几个 GPIO，
 *     跑一次 tools/i2c_scan 就能自动探出触摸的 SDA/SCL。
 * ============================================================ */

// ---- 屏幕 ST7789 240x240（原理图确认）----
#define LCD_DC      45
#define LCD_CS      21
#define LCD_SCK     38
#define LCD_MOSI    39
#define LCD_RST     40
#define LCD_BL      46      // 背光，PWM 可调
#define SCREEN_W    240
#define SCREEN_H    240

// ---- 触摸 CST816（原理图确认：TP_SDA/TP_SCL/TP_RST/TP_INT）----
#define TOUCH_SDA   42
#define TOUCH_SCL   41
#define TOUCH_RST   47
#define TOUCH_INT   48
#define TOUCH_ADDR  0x15

// ---- 音频 I2S（原理图确认，当前固件未启用）----
#define I2S_MCLK     8
#define I2S_BCLK     9
#define I2S_LRC     10
#define I2S_MIC_DIN 11      // codec → ESP32（麦克风）
#define I2S_SPK_OUT 12      // ESP32 → codec（喇叭）
#define PIN_PA_CTRL  7      // 功放使能，拉低省电

// ---- 电池（原理图确认：IO1=BAT_ADC, IO2=BAT_EN, IO3=CHG_STAT）----
#define PIN_BAT_ADC   1
#define BAT_EN        2
#define PIN_CHG_STAT  3
#define BAT_DIV       2.0f     // 分压比。实测偏低/偏高就调这个：实际电压 / 读数
#define BAT_FULL      4.20f
#define BAT_EMPTY     3.30f

// ---- 离线检测 ----
#define OFFLINE_MS_HB     90000UL      // 有心跳时：90 秒没请求就判离线
#define OFFLINE_MS_NOHB  600000UL      // 没有心跳时：10 分钟没请求才判（可能只是空闲）

// ---- 夜间调光 ----
#define NIGHT_START_MIN  (20 * 60)       // 20:00 起
#define NIGHT_END_MIN    (9 * 60 + 30)   // 09:30 止
#define BL_DAY           255
#define BL_NIGHT          77             // 30%
#define BL_BOOST_MS    20000UL           // 夜间触摸后临时提亮 20 秒

// ---- 配网热点 ----
#define AP_SSID     "WB-Setup"
#define AP_PASS     "wb12345678"

/* ============================================================
 *  应用常量
 * ============================================================ */

#define MAX_TASKS   4
#define MAX_SCAN    10
#define MAX_SAVED   5
#define PASS_MAX    32

enum State { ST_NONE, ST_IDLE, ST_THINKING, ST_WORKING, ST_CONFIRM, ST_DONE, ST_ERROR };

struct Task {
  bool     active;
  char     id[10];
  char     label[48];
  char     tool[20];
  State    st;
  uint32_t updated;
};

// 待审批请求（多个任务可同时排队）
struct Approval {
  bool     active;
  char     req[12];
  char     tool[16];
  char     label[40];    // 哪个任务要审批
  char     cmd[160];
  uint32_t deadline;
  uint32_t clearAt;
  int8_t   decision;     // -1 待定 / 0 拒绝 / 1 批准 / -2 超时 / -3 用户关掉提醒
};

#define MAX_APPROVALS 3
#define APPROVAL_MS   3000      // 提醒停留时长：3 秒后自动消失（决策在电脑上做）

// RGB565
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))

/* 配色
 * ⚠️ 这块 IPS 屏会把「近黑」明显提亮：#12141A 在屏幕上看起来是一片蓝灰，
 *    而纯黑 #000000 反而是真正的黑。所以底色用纯黑，
 *    但卡片/分割线不能跟着压暗 —— 压到 #0E1014 就和背景分不出来了，
 *    文字看起来像浮在纯黑上。这里保持原来的中间调。
 *    模拟器 simulator/index.html 用的是同一套值，改这里记得同步改那边。
 */
#define C_BG      RGB565(0x00, 0x00, 0x00)
#define C_CARD    RGB565(0x1C, 0x1F, 0x26)
#define C_TEXT    RGB565(0xE8, 0xEA, 0xED)
#define C_DIM     RGB565(0x9A, 0xA0, 0xA6)
#define C_MUTE    RGB565(0x6B, 0x72, 0x80)
#define C_RED     RGB565(0xE2, 0x4B, 0x4A)
#define C_BLUE    RGB565(0x37, 0x8A, 0xDD)
#define C_GREEN   RGB565(0x63, 0x99, 0x22)
#define C_AMBER   RGB565(0xBA, 0x75, 0x17)
#define C_LINE    RGB565(0x2C, 0x2C, 0x2A)
#define C_KEY     RGB565(0x25, 0x28, 0x30)
#define C_GREENBG RGB565(0x12, 0x24, 0x0F)
#define C_REDBG   RGB565(0x2A, 0x14, 0x16)
