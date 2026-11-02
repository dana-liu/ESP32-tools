#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync-tasks.py —— 把真实的 WorkBuddy 会话列表推到状态灯上

一次性快照，用来预览效果。真正的实时更新靠 wb-status-hook.py。

数据来源：~/.workbuddy-ai/projects/<项目>/<会话>.jsonl
  - 任务名 = 该会话第一条用户消息（去掉 system-reminder，截 10 汉字 / 20 字符）
  - 状态   = 最后一条消息：user → thinking；assistant 带 tool_use → working；否则 done

用法：
    python3 sync-tasks.py --dry-run   # 只看解析结果，不推设备
    python3 sync-tasks.py             # 推到设备
"""

import glob
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_common

PROJ_DIR = os.path.expanduser("~/.workbuddy-ai/projects")
MAX_TASKS = 4
CJK_MAX = 10
ASCII_MAX = 20
RECENT_HOURS = 72


# ---------------- 解析 ----------------
def parts_text(msg):
    out = []
    for p in msg.get("content") or []:
        if isinstance(p, dict) and p.get("type") in ("input_text", "text"):
            out.append(p.get("text", ""))
    return "\n".join(out)


def clean(text):
    text = " ".join(str(text).split())
    # 去掉截图之类的附件标记
    text = re.sub(r"@image#\d+:\S+\s*", "", text).strip()
    # 有些会话第一句直接甩一个路径过来，取目录名更有意义
    if text.startswith("/"):
        first = text.split()[0]
        text = os.path.basename(first.rstrip("/")) or text
    has_cjk = any("\u4e00" <= c <= "\u9fff" for c in text)
    limit = CJK_MAX if has_cjk else ASCII_MAX
    return text[:limit].strip(" ，。！？、,.!?：:；;")


def strip_reminders(s):
    s = re.sub(r"<system-reminder[\s\S]*?</system-reminder>", "", s)
    m = re.search(r"<user_query>([\s\S]*?)</user_query>", s)
    if m:
        return m.group(1).strip()
    return s.strip()


def head_lines(path, n=120):
    out = []
    try:
        with open(path, errors="replace") as f:
            for i, ln in enumerate(f):
                if i >= n:
                    break
                out.append(ln)
    except Exception:
        pass
    return out


def tail_lines(path, n=400, max_bytes=512 * 1024):
    """从文件尾部倒着读，避免把 6MB 全吞进来"""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            if size <= max_bytes:
                f.seek(0)
                data = f.read()
            else:
                f.seek(size - max_bytes)
                data = f.read()
        lines = data.decode("utf-8", "replace").splitlines()
        return lines[-n:]
    except Exception:
        return []


def first_prompt(path):
    for ln in head_lines(path):
        try:
            d = json.loads(ln)
        except Exception:
            continue
        if d.get("type") != "message" or d.get("role") != "user":
            continue
        t = strip_reminders(parts_text(d))
        if t:
            return t
    return ""


def last_state(path):
    for ln in reversed(tail_lines(path)):
        try:
            d = json.loads(ln)
        except Exception:
            continue
        if d.get("type") != "message":
            continue
        role = d.get("role")
        if role == "user":
            return "thinking", ""
        if role == "assistant":
            tool = ""
            for p in d.get("content") or []:
                if isinstance(p, dict) and p.get("type") == "tool_use":
                    tool = p.get("name") or ""
            return ("working", tool) if tool else ("done", "")
    return "idle", ""


def scan():
    now = time.time()
    items = []
    for f in glob.glob(os.path.join(PROJ_DIR, "*", "*.jsonl")):
        try:
            mtime = os.path.getmtime(f)
        except Exception:
            continue
        if now - mtime > RECENT_HOURS * 3600:
            continue
        sid = os.path.basename(f).split(".")[0][:8]
        label = clean(first_prompt(f))
        if not label:
            label = os.path.basename(os.path.dirname(f))[-10:]
        state, tool = last_state(f)
        items.append({"sid": sid, "label": label, "st": state,
                      "tool": tool, "mtime": mtime})
    items.sort(key=lambda x: x["mtime"], reverse=True)
    return items


# ---------------- 推送 ----------------
def device_sids():
    base = wb_common.BRIDGE_URL if wb_common.bridge_alive() else wb_common.WIFI_URL
    try:
        raw = urllib.request.urlopen(base + "/list", timeout=2).read().decode()
        return {t.get("sid", "") for t in json.loads(raw)}
    except Exception:
        return set()


def main():
    dry = "--dry-run" in sys.argv
    items = scan()
    top = items[:MAX_TASKS]

    print("%-9s %-8s %-6s %-12s %s" % ("sid", "状态", "工具", "时间", "任务名"))
    for t in top:
        print("%-9s %-8s %-6s %-12s %s" % (
            t["sid"], t["st"], t["tool"] or "-",
            time.strftime("%m-%d %H:%M", time.localtime(t["mtime"])), t["label"]))

    if dry:
        print("\n（--dry-run，没有推送）")
        return 0

    if not top:
        print("没找到会话")
        return 1

    ok = 0
    # 先把设备上已有的任务全部清掉再推。
    # 不能只推不清：设备满了会按优先级淘汰，旧的高优先级任务会把新推的
    # 「已完成」挤掉，最后屏幕上只剩一两个。
    for sid in device_sids():
        wb_common.post("/state?sid=%s&st=off" % urllib.parse.quote(sid), timeout=1.5)
        time.sleep(0.05)

    for t in top:
        qs = urllib.parse.urlencode({
            "sid": t["sid"], "st": t["st"],
            "tool": t["tool"][:16], "label": t["label"],
        })
        if wb_common.post("/state?" + qs, timeout=1.5):
            ok += 1
        time.sleep(0.05)

    print("\n已推送 %d/%d 个任务到设备" % (ok, len(top)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
