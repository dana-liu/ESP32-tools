#pragma once
#include <Arduino.h>
#include "config.h"

struct ScanItem {
  char    ssid[33];
  int32_t rssi;
  bool    enc;
};

// 生命周期
void netBegin();
void netLoop();

// 状态
bool netConnected();
bool netPortalActive();
IPAddress netIP();

// 扫描（返回数量）
int             netScan();
const ScanItem* netScanItems();
int             netScanCount();

// 连接与凭据
void netConnectByIndex(int idx, const char* pass);
void netSaveCred(const char* ssid, const char* pass);
int  netSavedCount();
const char* netSavedSsid(int i);
void netForgetAll();
void netRestart();

// 当前连接信息
const char* netCurrentSsid();
