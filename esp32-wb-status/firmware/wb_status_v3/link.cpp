#include "link.h"
#include <string.h>

static char   s_buf[320];
static size_t s_len = 0;

// 把一行按 '|' 切开，返回字段数
static int splitFields(char* s, char* out[], int maxOut) {
  int n = 0;
  out[n++] = s;
  for (char* p = s; *p; p++) {
    if (*p == '|') {
      *p = 0;
      if (n < maxOut) out[n++] = p + 1;
      else break;                 // 字段超了，剩下的都留在最后一个字段里
    }
  }
  return n;
}

static void reply(const char* s) {
  Serial.print('@');
  Serial.println(s);
}

static void handleLine(char* line) {
  char* f[6];
  int   n = splitFields(line, f, 6);
  if (n < 1 || !f[0][0]) return;

  linkOnActivity();

  if (!strcmp(f[0], "PING")) { reply("PONG"); return; }

  if (!strcmp(f[0], "STATE")) {
    if (n < 3) { reply("ERR"); return; }
    linkApplyState(f[1], f[2], n > 3 ? f[3] : "", n > 4 ? f[4] : "");
    reply("OK");
    return;
  }

  if (!strcmp(f[0], "APPROVE")) {
    if (n < 3) { reply("ERR"); return; }
    linkApplyApprove(f[1], f[2], n > 3 ? f[3] : "", n > 4 ? f[4] : "");
    reply("OK");
    return;
  }

  if (!strcmp(f[0], "DEC")) {
    if (n < 2) { reply("ERR"); return; }
    int  d = linkQueryDecision(f[1]);
    char out[80];
    snprintf(out, sizeof(out), "DEC|%s|%s", f[1],
             d == 1 ? "allow" : d == 0 ? "deny" : d == -1 ? "pending" : "none");
    reply(out);
    return;
  }

  if (!strcmp(f[0], "LIST")) {
    String o = "LIST|";
    o += linkBuildList();
    reply(o.c_str());
    return;
  }

  if (!strcmp(f[0], "CTEST")) {
    reply("OK");
    linkColorTest();          // 阻塞约 5 秒
    return;
  }

  if (!strcmp(f[0], "NIGHT")) {
    linkSetNight((n > 1 && f[1][0] == '1') ? 1 : 0);
    reply("OK");
    return;
  }

  if (!strcmp(f[0], "TIME")) {
    if (n < 2) { reply("ERR"); return; }
    linkSetTime(atol(f[1]));
    reply("OK");
    return;
  }

  reply("ERR");
}

void linkBegin() {
  s_len = 0;
  while (Serial.available()) Serial.read();     // 丢掉上电时的残包
}

void linkLoop() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;

    if (c == '\r') continue;

    if (c == '\n') {
      if (s_len) {
        s_buf[s_len] = 0;
        handleLine(s_buf);
        s_len = 0;
      }
    } else if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = (char)c;
    } else {
      s_len = 0;                                // 超长行直接丢弃
    }
  }
}
