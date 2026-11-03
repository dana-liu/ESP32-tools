// ES8311 + I2S 提示音
//
// 这块板子（微雪 ESP32-S3-Touch-LCD-1.54）没有无源蜂鸣器，声音走
// ES8311 编解码器 -> 功放(PA_CTRL) -> 板载喇叭。引脚取自官方原理图：
//   MCLK=GPIO8  BCLK=GPIO9  LRCK=GPIO10  DOUT=GPIO12  PA_CTRL=GPIO7
//   控制总线与触摸共用 I2C（SDA=42 / SCL=41），ES8311 地址 0x18
//
// 初始化失败不影响时钟功能，只是没声音。
#pragma once
#include <Arduino.h>

void audioBegin();              // I2S + ES8311 初始化
bool audioReady();              // 初始化是否成功
void audioSetVolume(int vol);   // 0..100，0 为静音
int  audioVolume();             // 当前音量
void audioTone(uint16_t freq, uint16_t ms);
void audioChime();              // 日程提醒：三声上行
void audioTick();               // 操作反馈：极短一声
