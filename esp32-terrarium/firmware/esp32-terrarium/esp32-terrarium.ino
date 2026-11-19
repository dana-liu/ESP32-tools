#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
}

Arduino_ESP32SPI bus(45, 21, 38, 39, -1);
Arduino_ST7789 panel(&bus, 40, 0, true, 240, 240);

class TerrariumCanvas : public Arduino_Canvas {
public:
  TerrariumCanvas() : Arduino_Canvas(240, 240, &panel) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    _framebuffer = static_cast<uint16_t*>(heap_caps_aligned_alloc(
        16, 240 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return _framebuffer && Arduino_Canvas::begin(speed);
  }
  uint16_t pixelAt(int x, int y) const { return _framebuffer[y * 240 + x]; }
  void repaint() { draw16bitRGBBitmap(0, 0, backdrop, 240, 240); }
  uint16_t* backdrop = nullptr;
  void saveBackdrop() { memcpy(backdrop, _framebuffer, 240 * 240 * sizeof(uint16_t)); }
};

TerrariumCanvas screen;
Preferences prefs;
enum Weather : uint8_t { Moon, Rain, Sun };
Weather weather = Moon;
Weather weatherBeforeRain = Moon;
constexpr uint32_t rainDuration = 3000;
constexpr int rainTop = 77, rainBottom = 157;
uint32_t rainStarted = 0;

struct Raindrop {
  float y;
  int x, speed, length;
};
Raindrop raindrops[22];

void resetRaindrop(Raindrop& drop, bool stagger);

void resetRaindrop(Raindrop& drop, bool stagger) {
  drop.x = random(57, 183);
  drop.y = rainTop - (stagger ? random(0, 75) : 0);
  drop.speed = random(95, 195);
  drop.length = random(2, 7);
}
int water = 54, day = 1, growth = 35, waterline = 169;
bool savedState = false;

float noise(unsigned value) {
  return ((value * 2654435761u) >> 16) / 65535.0f;
}

void oval(int x, int y, int rx, int ry, uint16_t color) {
  screen.fillEllipse(x, y, rx, ry, color);
}

constexpr int batteryAdcPin = 1, chargeStatusPin = 3;
int batteryPercent = -1;
bool charging = false;
constexpr uint32_t quotaTimeout = 150000;
uint32_t quotaUpdated = 0;
int quotaMinutes[2] = {0, 0};
int quotaRemaining[2] = {-1, -1};

static int lipoPct(float v) {
  static const float tab[10][2] = {
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 70}, {3.80f, 55},
    {3.70f, 40},  {3.60f, 25}, {3.50f, 12}, {3.40f, 5},  {3.30f, 0}
  };
  if (v >= 4.15f) return 100;
  if (v <= tab[9][0]) return 0;
  for (int i = 0; i < 9; i++) {
    if (v >= tab[i + 1][0]) {
      float t = (v - tab[i + 1][0]) / (tab[i][0] - tab[i + 1][0]);
      return (int)(tab[i + 1][1] + t * (tab[i][1] - tab[i + 1][1]) + 0.5f);
    }
  }
  return 0;
}

void sampleBattery() {
  uint32_t millivolts = 0;
  for (int i = 0; i < 8; ++i) {
    millivolts += analogReadMilliVolts(batteryAdcPin);
    delay(2);
  }
  const float voltage = millivolts * 3.0f / 8000.0f;
  charging = digitalRead(chargeStatusPin) == LOW;
  batteryPercent = voltage < 2.8f || voltage > 4.5f
      ? -1 : lipoPct(voltage - (charging ? 0.11f : 0.0f));
}

bool acceptQuota(const char* line, uint32_t now) {
  int firstMinutes, firstRemaining, secondMinutes, secondRemaining;
  char extra;
  if (sscanf(line, "QUOTA %5d %3d %5d %3d %c",
        &firstMinutes, &firstRemaining, &secondMinutes, &secondRemaining, &extra) != 4 ||
      firstMinutes < 0 || secondMinutes < 0 ||
      firstRemaining < -1 || firstRemaining > 100 ||
      secondRemaining < -1 || secondRemaining > 100) return false;
  quotaMinutes[0] = firstMinutes;
  quotaMinutes[1] = secondMinutes;
  quotaRemaining[0] = firstRemaining;
  quotaRemaining[1] = secondRemaining;
  quotaUpdated = now;
  return true;
}

#include "TerrariumBle.h"

void readQuota(uint32_t now) {
  static char line[48];
  static size_t length = 0;
  static bool overflow = false;
  for (int read = 0; read < 64 && Serial.available(); ++read) {
    const char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (length < sizeof(line) - 1) line[length++] = ch;
      else overflow = true;
      continue;
    }
    line[length] = 0;
    if (!overflow) {
      if (acceptQuota(line, now)) Serial.println("QUOTA OK");
      else if (strcmp(line, "BLE OTA ARM") == 0) {
        TerrariumBle::arm(now);
        if (TerrariumBle::updateCode) {
          Serial.printf("BLE OTA CODE %06lu\n", (unsigned long)TerrariumBle::updateCode);
        }
      }
    }
    length = 0;
    overflow = false;
  }
}

void drawStatus(uint32_t now) {
  const uint16_t textColor = rgb(217, 214, 177);
  const uint16_t track = rgb(59, 79, 64);
  const uint16_t green = rgb(156, 205, 86);
  screen.setTextSize(1);
  const uint16_t batteryColor = !charging && batteryPercent >= 0 && batteryPercent <= 15
      ? rgb(239, 135, 76) : green;
  screen.drawRect(170, 12, 20, 10, batteryColor);
  screen.fillRect(190, 15, 2, 4, batteryColor);
  if (batteryPercent > 0) screen.fillRect(172, 14, max(1, 16 * batteryPercent / 100), 6, batteryColor);
  if (charging) {
    screen.drawLine(181, 13, 178, 17, textColor);
    screen.drawFastHLine(178, 17, 5, textColor);
    screen.drawLine(182, 17, 179, 21, textColor);
  }
  screen.setTextColor(batteryColor);
  screen.setCursor(199, 14);
  if (batteryPercent >= 0) screen.printf("%d%%", batteryPercent);
  else screen.print("--");

  screen.setTextColor(textColor);
  screen.drawFastHLine(15, 203, 210, track);
  screen.setCursor(15, 214);
  screen.print("CODEX");
  for (int i = 0; i < 2; ++i) {
    const int x = 91 + i * 72;
    const int minutes = quotaMinutes[i];
    const int remaining = now - quotaUpdated < quotaTimeout ? quotaRemaining[i] : -1;
    screen.setCursor(x, 214);
    if (minutes > 0 && minutes % 1440 == 0) screen.printf("%dd ", minutes / 1440);
    else if (minutes > 0 && minutes % 60 == 0) screen.printf("%dh ", minutes / 60);
    else if (minutes > 0) screen.printf("%dm ", minutes);
    if (remaining >= 0) screen.printf("%d%%", remaining);
    else screen.print("--");
    screen.fillRect(x, 226, 57, 3, track);
    if (remaining > 0) screen.fillRect(x, 226, max(1, 57 * remaining / 100), 3, green);
  }
}

void scene() {
  const bool sunny = weather == Sun;
  const bool raining = weather == Rain;
  screen.fillScreen(rgb(16, 25, 24));
  for (int r = 105; r >= 15; r -= 5) {
    int light = (105 - r) / 3;
    oval(120, 115, r, r - 5, rgb(20 + light, 31 + light, 27 + light / 2));
  }
  oval(120, 190, 92, 10, rgb(15, 22, 21));
  screen.fillRoundRect(35, 68, 170, 122, 30, rgb(134, 164, 139));
  screen.fillRoundRect(37, 69, 166, 119, 29, rgb(37, 61, 50));
  screen.fillRoundRect(76, 43, 88, 39, 10, rgb(134, 164, 139));
  screen.fillRoundRect(78, 44, 84, 39, 10, rgb(47, 70, 53));
  screen.fillRect(79, 69, 82, 17, rgb(47, 70, 53));
  for (int y = 82; y <= 161; ++y) {
    int edge = y < 100 ? (100 - y) * (100 - y) / 26 : 0;
    int light = (161 - y) / 4;
    int left = 38 + edge;
    int right = 202 - edge;
    int red = 50 + light, green = 62 + light, blue = 38 + light / 2;
    screen.drawFastHLine(left, y, right - left,
        sunny ? rgb(red, green, blue)
              : raining ? rgb(27 + light, 52 + light, 54 + light)
                        : rgb(34 + light, 59 + light, 46 + light));
    if (sunny && y >= 92) {
      // 光束向左下方展开，边缘逐渐淡入瓶内背景。
      const int distance = y - 92;
      const int center = 151 - distance * 3 / 4;
      const int width = 3 + distance / 6;
      for (int x = max(left, center - width); x < min(right, center + width); ++x) {
        const int glow = 56 * (width - abs(x - center)) / width;
        screen.drawPixel(x, y, rgb(red + glow, green + glow / 2, blue + glow / 8));
      }
      const int second = 156 - distance / 3;
      const int secondWidth = 2 + distance / 9;
      for (int x = max(left, second - secondWidth); x < min(right, second + secondWidth); ++x) {
        const int glow = 34 * (secondWidth - abs(x - second)) / secondWidth;
        screen.drawPixel(x, y, rgb(red + glow, green + glow / 2, blue + glow / 8));
      }
    }
  }
  if (sunny) {
    // 用连续径向衰减绘制暖色光晕和金黄色日面。
    for (int dy = -24; dy <= 24; ++dy) {
      for (int dx = -24; dx <= 24; ++dx) {
        const int radiusSquared = dx * dx + dy * dy;
        if (radiusSquared > 24 * 24) continue;
        const int x = 149 + dx, y = 91 + dy;
        if (radiusSquared > 10 * 10) {
          const int glow = (24 * 24 - radiusSquared) * 100 / (24 * 24);
          const uint16_t base = screen.pixelAt(x, y);
          const int red = ((base >> 11) & 31) * 255 / 31;
          const int green = ((base >> 5) & 63) * 255 / 63;
          const int blue = (base & 31) * 255 / 31;
          screen.drawPixel(x, y, rgb(red + (255 - red) * glow / 255,
              green + (183 - green) * glow / 255, blue + (55 - blue) * glow / 255));
        } else {
          screen.drawPixel(x, y, rgb(255, 220 - radiusSquared / 2, 56 - radiusSquared / 3));
        }
      }
    }
  } else if (!raining) {
    oval(149, 91, 16, 16, rgb(83, 89, 53));
    oval(149, 91, 7, 7, rgb(248, 219, 151));
    oval(152, 88, 6, 6, rgb(83, 89, 53));
  }

  oval(120, 157, 81, 23, rgb(48, 77, 53));
  oval(101, 164, 63, 20, rgb(62, 88, 52));
  oval(149, 170, 51, 14, rgb(61, 82, 43));
  waterline = 183 - water * 40 / 100;
  if (water > 0) {
    for (int y = waterline; y < 185; ++y) {
      int inset = max(0, y - 170);
      screen.drawFastHLine(40 + inset, y, 160 - 2 * inset,
          y == waterline ? rgb(126, 184, 158) : rgb(33, 79 + (185 - y) / 2, 66));
    }
  }
  for (int i = 0; i < 54 + growth; ++i) {
    int x = 53 + noise(i + 34) * 133;
    int y = 166 + noise(i + 412) * 16;
    oval(x, y, 1 + i % 4, 1 + i % 2,
        i % 3 ? rgb(70, 104, 53) : rgb(139, 157, 77));
  }
  const int stems[] = {59, 78, 164, 181};
  for (int x : stems) {
    int h = 22 + (x % 13) + growth / 6;
    screen.drawLine(x, 174, x + 3, 174 - h, rgb(166, 181, 101));
    for (int i = 1; i <= 5; ++i) {
      int y = 174 - h * i / 6;
      oval(x - 3 + i / 2, y, 6 - i / 2, 2, rgb(118, 151, 71));
      oval(x + 6 + i / 2, y - 2, 5 - i / 2, 2, rgb(153, 170, 83));
    }
  }
  if (sunny) {
    // 为迎光的叶缘、玻璃和水面补上暖色反光。
    const int litStems[] = {78, 164};
    for (int x : litStems) {
      const int height = 22 + x % 13 + growth / 6;
      for (int i = 2; i <= 5; ++i) {
        const int y = 174 - height * i / 6;
        screen.drawFastHLine(x + 5 + i / 2, y - 3, 4, rgb(206, 187, 89));
      }
    }
    screen.drawFastVLine(192, 107, 19, rgb(182, 158, 90));
    screen.drawLine(186, 92, 191, 100, rgb(202, 173, 97));
    if (water > 0) {
      const int reflection = 151 - (waterline - 92) * 3 / 4;
      screen.drawFastHLine(reflection - 11, waterline, 23, rgb(211, 185, 106));
      screen.drawFastHLine(reflection - 6, waterline + 3, 13, rgb(151, 150, 84));
    }
  }
  screen.fillRoundRect(80, 34, 80, 15, 4, rgb(146, 113, 70));
  oval(120, 35, 39, 3, rgb(201, 171, 114));
  screen.drawFastVLine(46, 103, 32, rgb(143, 177, 150));
  screen.drawLine(48, 99, 53, 90, rgb(143, 177, 150));

  screen.setTextSize(1);
  screen.setTextColor(rgb(217, 214, 177));
  screen.setCursor(15, 14);
  screen.printf("DAY %d  %s", day, raining ? "RAIN" : sunny ? "SUN" : "MOON");
  screen.setCursor(121, 14);
  screen.printf("W %d%%", water);
  screen.saveBackdrop();
}

void drawFish(uint32_t frameElapsed) {
  enum FishAction { Cruise, Dart, Turn, Bubbles };
  static FishAction action = Cruise;
  static float x = 120, depthRatio = 0.52f, speed = 15, tailPhase = 0;
  static int facing = 1;
  static int untilAction = 0, actionElapsed = 0;
  static int bubbleX = 0;

  const int depth = 185 - waterline;
  if (water == 0 || depth < 4) {
    action = Cruise;
    untilAction = 0;
    return;
  }
  const int elapsed = min(frameElapsed, uint32_t(100));
  if (action == Cruise) {
    if (untilAction <= 0) untilAction = random(3500, 8500);
    untilAction -= elapsed;
    if (untilAction <= 0) {
      // 水深足够时才会上浮吐泡泡。
      action = static_cast<FishAction>(random(Dart, depth >= 16 ? Bubbles + 1 : Turn + 1));
      actionElapsed = 0;
      if (action == Turn) facing = -facing;
      if (action == Dart && (x < 100 || x > 140)) facing = x < 120 ? 1 : -1;
    }
  } else {
    actionElapsed += elapsed;
    if (action == Bubbles && depth < 16) action = Cruise;
  }

  const bool atSurface = action == Bubbles && actionElapsed < 1050;
  const float targetDepth = atSurface ? 0.12f : 0.52f;
  const float targetSpeed = action == Dart ? 63.0f : action == Turn ? 33.0f :
      atSurface ? 4.0f : 15.0f;
  speed += (targetSpeed - speed) * min(1.0f, elapsed / 170.0f);
  depthRatio += (targetDepth - depthRatio) * min(1.0f, elapsed / 210.0f);

  x += facing * speed * elapsed / 1000.0f;
  if (x >= 166 || x <= 74) {
    x = constrain(x, 74.0f, 166.0f);
    facing = x >= 166 ? -1 : 1;
  }
  if (action == Bubbles && actionElapsed < 650) bubbleX = int(x) + facing * 6;
  if (action == Bubbles) {
    for (int i = 0; i < 2; ++i) {
      const int age = actionElapsed - 650 - i * 240;
      if (age >= 0 && age < 480) {
        screen.drawCircle(bubbleX + i * 5, waterline + 2 - age / 38,
            1 + i, rgb(160, 215, 190));
      }
    }
  }

  const int size = constrain(depth / 2, 1, 7);
  const int finHeight = min(4, (depth - 2) / 2);
  const int y = constrain(waterline + int(depth * depthRatio),
      waterline + finHeight, 184 - finHeight);
  tailPhase = fmodf(tailPhase + (9.0f + speed * 0.38f) * elapsed / 1000.0f, 6.283185f);
  const int wag = int(sinf(tailPhase) * min(2, finHeight));
  const int tailY = constrain(y + wag, waterline + finHeight, 184 - finHeight);
  const int bodyX = int(x);
  const int tailX = bodyX - (size + 5) * facing;
  screen.fillTriangle(bodyX - facing * 3, y, tailX, tailY - finHeight,
      tailX, tailY + finHeight, rgb(224, 134, 81));
  oval(bodyX, y, size, finHeight, rgb(253, 176, 93));
  if (size > 2) screen.drawPixel(bodyX + 3 * facing, y - 1, rgb(50, 37, 28));

  if ((action == Dart && actionElapsed >= 820) ||
      (action == Turn && actionElapsed >= 480) ||
      (action == Bubbles && actionElapsed >= 1480)) {
    action = Cruise;
  }
}

void render(uint32_t now, uint32_t frameElapsed) {
  const float t = now / 1000.0f;
  if (weather == Rain) {
    const bool spawning = now - rainStarted < rainDuration;
    bool visibleOrPending = false;
    for (auto& drop : raindrops) {
      if (drop.y < rainBottom) {
        drop.y += drop.speed * (frameElapsed / 1000.0f);
      }
      if (drop.y >= rainBottom && spawning) resetRaindrop(drop, false);
      visibleOrPending |= drop.y < rainBottom;
    }
    if (!visibleOrPending) {
      weather = weatherBeforeRain;
      scene();
    }
  }
  screen.repaint();
  drawFish(frameElapsed);
  if (weather == Rain) {
    for (const auto& drop : raindrops) {
      const int y = static_cast<int>(drop.y);
      const int top = max(rainTop, y);
      const int bottom = min(rainBottom, y + drop.length);
      if (bottom > top) screen.drawFastVLine(drop.x, top, bottom - top, rgb(139, 192, 188));
    }
  } else {
    for (int i = 0; i < 8; ++i) {
      int x = 57 + noise(i + 103) * 125;
      int y = 77 + static_cast<int>(noise(i + 412) * 100 + t * 5) % 80;
      screen.drawPixel(x, y, rgb(237, 210, 132));
    }
  }
  drawStatus(now);
  TerrariumBle::drawUpdate();
  screen.flush();
}

bool readFinger(bool& down) {
  Wire.beginTransmission(0x15);
  Wire.write(uint8_t(0x02));
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(uint8_t(0x15), uint8_t(1)) != 1) {
    return false;
  }
  down = (Wire.read() & 0x0f) != 0;
  return true;
}

void input(uint32_t now) {
  static bool pressed = false, nextWasDown = false, longPress = false;
  static uint32_t started = 0, buttonStarted = 0;
  static bool updateArmed = false;
  bool down;
  bool changed = false;
  if (readFinger(down)) {
    if (down && !pressed) {
      started = now;
      longPress = false;
    }
    if (down && !longPress && now - started >= 550) {
      weather = Sun;
      water = max(0, water - 12);
      changed = longPress = true;
    }
    if (!down && pressed && !longPress) {
      if (weather != Rain) {
        weatherBeforeRain = weather;
        for (auto& drop : raindrops) resetRaindrop(drop, true);
      }
      rainStarted = now;
      weather = Rain;
      water = min(100, water + 16);
      changed = true;
    }
    pressed = down;
  }
  const bool nextDown = digitalRead(4) == LOW;
  if (nextDown && !nextWasDown) {
    buttonStarted = now;
    updateArmed = false;
  }
  if (nextDown && !updateArmed && now - buttonStarted >= 2000) {
    TerrariumBle::arm(now);
    updateArmed = true;
  }
  if (!nextDown && nextWasDown && !updateArmed) {
    ++day;
    growth = constrain(growth + (water >= 35 && water <= 80 ? 13 : -5), 10, 100);
    water = max(0, water - 9);
    weather = Moon;
    changed = true;
  }
  nextWasDown = nextDown;
  if (changed) {
    if (savedState) {
      const Weather persistentWeather = weather == Rain ? weatherBeforeRain : weather;
      const uint32_t packed = (uint32_t(day) << 16) | (growth << 9) | (water << 2) | persistentWeather;
      prefs.putUInt("scene", packed);
    }
    scene();
    TerrariumBle::publishStatus();
  }
}

void setup() {
  gpio_deep_sleep_hold_dis();
  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);
  pinMode(chargeStatusPin, INPUT_PULLUP);
  sampleBattery();
  gpio_hold_dis(GPIO_NUM_2);
  Serial.setRxBufferSize(1024);
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  pinMode(46, OUTPUT);
  digitalWrite(46, LOW);
  pinMode(4, INPUT_PULLUP);
  Wire.begin(42, 41);
  Wire.setTimeOut(8);
  pinMode(47, OUTPUT);
  digitalWrite(47, LOW);
  delay(10);
  digitalWrite(47, HIGH);
  delay(60);
  savedState = prefs.begin("terrarium", false);
  if (savedState) {
    const uint32_t packed = prefs.getUInt("scene");
    if (packed) {
      day = packed >> 16;
      growth = (packed >> 9) & 127;
      water = (packed >> 2) & 127;
      weather = (packed & 3) == Sun ? Sun : Moon;
    }
  }
  screen.backdrop = static_cast<uint16_t*>(ps_malloc(240 * 240 * sizeof(uint16_t)));
  if (!screen.backdrop || !screen.begin()) {
    Serial.println("TERRARIUM ERROR display or PSRAM");
    while (true) delay(1000);
  }
  scene();
  render(0, 0);
  digitalWrite(46, HIGH);
  TerrariumBle::begin();
  TerrariumBle::publishStatus();
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (TerrariumBle::progress) esp_ota_mark_app_valid_cancel_rollback();
    else esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  Serial.println("TERRARIUM READY");
}

void loop() {
  static uint32_t lastFrame = 0, lastInput = 0, lastLog = 0, lastBattery = 0;
  const uint32_t now = millis();
  readQuota(now);
  TerrariumBle::tick(now);
  if (now - lastBattery >= 5000) {
    lastBattery = now;
    sampleBattery();
  }
  if (now - lastInput >= 18) {
    lastInput = now;
    input(now);
  }
  if (now - lastFrame >= 42) {
    render(now, now - lastFrame);
    lastFrame = now;
  }
  if (now - lastLog >= 3000) {
    lastLog = now;
    TerrariumBle::publishStatus();
    if (Serial) Serial.printf("TERRARIUM RUN day=%d water=%d\n", day, water);
  }
  delay(1);
}
