#include "audio.h"
#include <Wire.h>
#include <math.h>
#include <driver/i2s_std.h>

#define ES8311_ADDR 0x18
#define PIN_PA      7
#define PIN_MCLK    8
#define PIN_BCLK    9
#define PIN_LRCK    10
#define PIN_DOUT    12

#define AUDIO_RATE  16000

static i2s_chan_handle_t s_tx = nullptr;
static bool s_ready  = false;
static int  s_volume = 60;

// ---------------- ES8311 寄存器 ----------------
static bool esWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool esRead(uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return false;
  *val = Wire.read();
  return true;
}

// 16 kHz / MCLK 4.096 MHz（256×fs）下的分频系数，取自 ES8311 官方系数表
static void codecInit() {
  uint8_t v;
  esWrite(0x00, 0x1F); delay(20);       // 复位
  esWrite(0x00, 0x00);
  esWrite(0x00, 0x80);                  // power on
  esWrite(0x01, 0x3F);                  // 打开所有时钟，MCLK 取自 MCLK 引脚
  if (esRead(0x02, &v)) esWrite(0x02, v & 0x07);        // pre_div=1, pre_multi=1x
  esWrite(0x03, 0x10);                  // adc_osr
  esWrite(0x04, 0x10);                  // dac_osr
  esWrite(0x05, 0x00);                  // adc_div=dac_div=1
  if (esRead(0x06, &v)) esWrite(0x06, (v & 0xE0) | 0x03);  // bclk_div=4
  if (esRead(0x07, &v)) esWrite(0x07, v & 0xC0);           // lrck_h=0
  esWrite(0x08, 0xFF);                  // lrck_l
  if (esRead(0x00, &v)) esWrite(0x00, v & 0xBF);           // 从机模式
  esWrite(0x09, 0x0C);                  // SDP in  16bit
  esWrite(0x0A, 0x0C);                  // SDP out 16bit
  esWrite(0x0D, 0x01);                  // 模拟电路上电
  esWrite(0x0E, 0x02);                  // 模拟 PGA + ADC 调制器
  esWrite(0x12, 0x00);                  // DAC 上电
  esWrite(0x13, 0x10);                  // 打开 HP 驱动
  esWrite(0x1C, 0x6A);                  // ADC 均衡器旁路
  esWrite(0x37, 0x08);                  // DAC 均衡器旁路
}

static void applyCodecVolume() {
  int reg = s_volume <= 0 ? 0 : (s_volume * 256 / 100) - 1;
  if (reg > 255) reg = 255;
  esWrite(0x32, (uint8_t)reg);
}

// ---------------- I2S ----------------
static bool i2sInit() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;
  if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t cfg = {};
  cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = (gpio_num_t)PIN_MCLK;
  cfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
  cfg.gpio_cfg.ws   = (gpio_num_t)PIN_LRCK;
  cfg.gpio_cfg.dout = (gpio_num_t)PIN_DOUT;
  cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

  if (i2s_channel_init_std_mode(s_tx, &cfg) != ESP_OK) return false;
  return i2s_channel_enable(s_tx) == ESP_OK;
}

void audioBegin() {
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);          // 先关功放，避免初始化时的爆音
  s_ready = i2sInit();                // 先起时钟，ES8311 才有 MCLK
  if (!s_ready) { Serial.println("[audio] I2S 初始化失败"); return; }
  codecInit();
  applyCodecVolume();
  digitalWrite(PIN_PA, HIGH);         // 打开功放
  Serial.printf("[audio] ES8311 就绪，音量 %d\n", s_volume);
}

bool audioReady() { return s_ready; }

void audioSetVolume(int vol) {
  s_volume = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
  if (s_ready) applyCodecVolume();
}

int audioVolume() { return s_volume; }

// ---------------- 发声 ----------------
// 立体声 16bit，左右同值；带 5ms 淡入淡出，避免咔哒声。
static void playSine(float freq, uint16_t ms, float amp) {
  if (!s_ready || s_volume <= 0 || ms == 0) return;
  const int frames = AUDIO_RATE * ms / 1000;
  const int chunk  = 256;
  int16_t buf[chunk * 2];
  float phase = 0.0f;
  const float step = 2.0f * PI * freq / AUDIO_RATE;
  const int fade = AUDIO_RATE / 200;              // 5 ms
  int done = 0;
  while (done < frames) {
    int n = frames - done; if (n > chunk) n = chunk;
    for (int i = 0; i < n; i++) {
      int idx = done + i;
      float env = 1.0f;
      if (idx < fade) env = (float)idx / fade;
      else if (idx > frames - fade) env = (float)(frames - idx) / fade;
      int16_t s = (int16_t)(sinf(phase) * amp * env * 32767.0f);
      buf[i * 2] = s; buf[i * 2 + 1] = s;
      phase += step;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
    }
    size_t written = 0;
    i2s_channel_write(s_tx, buf, n * 4, &written, 200);
    done += n;
  }
}

void audioTone(uint16_t freq, uint16_t ms) {
  playSine(freq, ms, 0.45f);
}

// 三声上行提示音，约 0.55 秒
void audioChime() {
  if (!s_ready || s_volume <= 0) return;
  playSine(880.0f,  110, 0.42f);
  delay(40);
  playSine(1174.7f, 110, 0.42f);
  delay(40);
  playSine(1568.0f, 170, 0.42f);
}

void audioTick() {
  if (!s_ready || s_volume <= 0) return;
  playSine(1760.0f, 35, 0.30f);
}
