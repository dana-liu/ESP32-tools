/*
 * I2C 引脚探测工具
 * ------------------------------------------------------------
 * 用途：微雪没公开 ESP32-S3-Touch-LCD-1.54 的触摸 I2C 引脚，
 *      这个 sketch 会穷举常见引脚组合，报告哪一对能扫到设备。
 *
 * 用法：
 *   1. 只烧这个 sketch（不要和主固件一起烧）
 *   2. 打开串口监视器，115200
 *   3. 看输出里哪一组出现了 0x15 / 0x18 / 0x6B
 *   4. 把那一组的引脚号填进 config.h 的 TOUCH_SDA / TOUCH_SCL
 *
 * 地址对照：
 *   0x15  CST816 系列触摸      ← 这就是你要找的
 *   0x18  ES8311 音频 codec    ← 同一条总线，佐证
 *   0x6B  QMI8658 六轴传感器   ← 同一条总线，佐证
 * ------------------------------------------------------------
 */

#include <Wire.h>

struct Pair { uint8_t sda, scl; };

// 避开 LCD 占用的 21 / 38 / 39 / 40 / 45
static const Pair CANDIDATES[] = {
  {42, 41}, {41, 42}, {47, 48}, {48, 47},
  {8, 9},   {9, 8},   {1, 2},   {2, 1},
  {3, 4},   {4, 3},   {5, 6},   {6, 5},
  {10, 11}, {11, 10}, {13, 14}, {14, 13},
  {15, 16}, {16, 15}, {17, 18}, {18, 17},
  {19, 20}, {20, 19}, {43, 44}, {44, 43},
  {0, 1},   {7, 8}
};
static const int PAIR_COUNT = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

const char* nameOf(uint8_t addr) {
  switch (addr) {
    case 0x15: return "CST816 触摸 ★";
    case 0x18: return "ES8311 codec";
    case 0x6B: return "QMI8658 IMU";
    case 0x38: return "FT6236 触摸";
    case 0x51: return "GT911 触摸";
    case 0x5A: return "GT911 触摸(备用)";
    case 0x14: return "CST820 触摸";
    default:   return "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== I2C 引脚探测 ===");

  for (int p = 0; p < PAIR_COUNT; p++) {
    uint8_t sda = CANDIDATES[p].sda;
    uint8_t scl = CANDIDATES[p].scl;

    Wire.end();
    if (!Wire.begin(sda, scl, 100000)) continue;
    delay(8);

    int found = 0;
    String hits = "";

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        found++;
        hits += " 0x";
        hits += String(addr, HEX);
        const char* nm = nameOf(addr);
        if (nm[0]) { hits += "("; hits += nm; hits += ")"; }
      }
    }

    if (found > 0) {
      Serial.printf("SDA=%2d SCL=%2d  →  %d 个设备:%s\n", sda, scl, found, hits.c_str());
    }
  }

  Wire.end();
  Serial.println("=== 探测结束 ===");
  Serial.println("找到 CST816 那一行的 SDA/SCL 就是你要填的值。");
  Serial.println("若一行都没有：检查板子是否上电、触摸排线是否插好。");
}

void loop() {
  delay(1000);
}
