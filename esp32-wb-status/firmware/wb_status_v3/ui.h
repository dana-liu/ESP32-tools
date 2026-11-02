#pragma once
#include "config.h"

void uiBegin();
void uiTick(Task* tasks, int count);
void uiMarkDirty();
void uiSetApproval(Approval* a);
void uiSetApprovalCount(int pending);
void uiSetBattery(int pct, bool charging);
void uiSetLink(bool wifiOk, bool usbOk, bool stale);
void uiSetNight(bool night);
void uiColorTest();          // 色彩自检：画 6 条纯色，5 秒后恢复

const char* stateLabelCN(State s);
const char* stateLabelEN(State s);
uint16_t    stateColor(State s);
