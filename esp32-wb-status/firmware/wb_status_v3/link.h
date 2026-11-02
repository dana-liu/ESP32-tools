#pragma once
#include <Arduino.h>

/* ============================================================
 *  USB 串口通道（走 CDC，不依赖 WiFi）
 *
 *  行协议，字段用 '|' 分隔，命令以 '\n' 结尾：
 *      PING
 *      STATE|<sid>|<st>|<tool>|<label>
 *      APPROVE|<req>|<tool>|<label>|<cmd>
 *      DEC|<req>
 *      LIST
 *
 *  回复一律以 '@' 开头，和普通调试日志区分开：
 *      @PONG
 *      @OK
 *      @DEC|<req>|allow|deny|pending|none
 *      @LIST|<json>
 *      @ERR
 * ============================================================ */

void linkBegin();
void linkLoop();

// ---- 以下由主程序实现 ----
void linkOnActivity();                                   // 收到任意命令
void linkApplyState(const char*, const char*, const char*, const char*);
void linkApplyApprove(const char*, const char*, const char*, const char*);
int  linkQueryDecision(const char*);                     // 1 allow / 0 deny / -1 pending / -2 none
String linkBuildList();                                  // 任务列表 JSON
void linkColorTest();                                    // 色彩自检
void linkSetNight(int on);                               // 强制夜间亮度（调试用）
void linkSetTime(long epoch);                            // 用 Mac 的时间对时（USB 模式下没有 NTP）
