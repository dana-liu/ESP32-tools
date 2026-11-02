#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WorkBuddy hook -> ESP32 状态灯（多任务版）

相比单任务版的变化：
1. 按 session_id 分桶，每个会话独立一条状态
2. 任务名取自 UserPromptSubmit 的 prompt（截前 14 字），回退到 cwd 目录名
3. SessionEnd 发 st=off，让设备移除该任务
4. 本地 sessions.json 记录，方便排障

用法（在 ~/.workbuddy-ai/settings.json 的 hooks 里调用）：
    /usr/bin/env python3 ~/bin/wb-status-hook.py

原则：永远 exit 0，超时 0.4s，不往 stdout 写任何东西。
"""

import json
import os
import sys
import time
import urllib.parse
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_common

STATE_DIR = wb_common.STATE_DIR
SESS_FILE = wb_common.SESS_FILE
TIMEOUT = 0.4
CJK_MAX = 10            # 中文：wqy12 字库每字 12px，128px 宽最多 10 字
ASCII_MAX = 20          # 英文：6x12 字库每字 6px
STALE_SEC = 1800        # 30 分钟没动静的会话从本地表里清掉


def load_sessions():
    try:
        with open(SESS_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_sessions(sess):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(SESS_FILE, "w") as f:
            json.dump(sess, f, ensure_ascii=False)
    except Exception:
        pass


# ---------------- 审批提醒 ----------------
# 只在 WorkBuddy 真的弹权限框（Notification 事件）时才提醒设备。
# 不能挂在 PreToolUse 上 —— 那里无法知道客户端会不会问你，
# 结果是每次 Bash/Write/Edit 都弹一次，绝大多数根本不需要确认。
PENDING_FILE = os.path.join(STATE_DIR, "pending.json")
NOTICE_LOG = os.path.join(STATE_DIR, "notifications.log")


def save_pending(data):
    """记下最近一次工具调用，等 Notification 来了取命令内容用"""
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        rec = {
            "tool": data.get("tool_name", "") or "",
            "cmd": json.dumps(data.get("tool_input", {}), ensure_ascii=False)[:150],
            "sid": data.get("session_id", "") or "",
            "ts": time.time(),
        }
        with open(PENDING_FILE, "w") as f:
            json.dump(rec, f, ensure_ascii=False)
    except Exception:
        pass


def load_pending(max_age=120):
    try:
        with open(PENDING_FILE) as f:
            rec = json.load(f)
        return rec if time.time() - rec.get("ts", 0) <= max_age else None
    except Exception:
        return None


def is_permission_notice(data):
    """Notification 是不是「要你授权」那种"""
    parts = [str(data.get(k, "")) for k in
             ("notification_type", "type", "title", "message", "content")]
    blob = " ".join(parts).lower()
    return any(k in blob for k in
               ("permission", "approval", "confirm", "authorize",
                "approve", "授权", "确认"))


def log_notice(data):
    """把所有 Notification 原文记下来，方便排查到底什么时候弹"""
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(NOTICE_LOG, "a") as f:
            f.write(json.dumps(data, ensure_ascii=False)[:800] + "\n")
    except Exception:
        pass


def clean(text: str) -> str:
    """压成一行，按中英文分别截断，去掉尾部标点"""
    text = " ".join(str(text).split())
    has_cjk = any("\u4e00" <= c <= "\u9fff" for c in text)
    limit = CJK_MAX if has_cjk else ASCII_MAX
    return text[:limit].strip(" ，。！？、,.!?：:；;")


def map_event(data: dict):
    """返回 (state, tool) 或 None"""
    ev = data.get("hook_event_name", "")
    tool = data.get("tool_name", "") or ""

    if ev == "SessionStart":
        return ("idle", "")
    if ev == "UserPromptSubmit":
        return ("thinking", "")
    if ev == "PreToolUse":
        return ("working", tool)
    if ev == "PostToolUse":
        return ("working", tool)
    if ev == "PostToolUseFailure":
        return ("error", tool)
    if ev == "Stop":
        return ("done", "")
    if ev == "SubagentStop":
        return ("working", "subagent")
    if ev == "SessionEnd":
        return ("off", "")
    if ev == "Notification":
        nt = (data.get("notification_type")
              or data.get("type")
              or data.get("message")
              or "").lower()
        if "permission" in nt or "approval" in nt or "confirm" in nt:
            return ("confirm", tool)
        if "idle" in nt:
            return ("idle", "")
        return ("working", tool)
    return None


def main():
    try:
        raw = sys.stdin.read()
        data = json.loads(raw) if raw.strip() else {}
    except Exception:
        return

    mapped = map_event(data)
    if not mapped:
        return
    state, tool = mapped

    ev = data.get("hook_event_name", "")

    # 记下工具调用，等 Notification 来了取命令内容
    if ev == "PreToolUse":
        save_pending(data)

    sid = data.get("session_id") or "default"
    cwd = data.get("cwd") or ""
    prompt = data.get("prompt") or ""

    sessions = load_sessions()
    entry = sessions.get(sid, {})

    # ---- 任务名：最新 prompt 优先，其次目录名 ----
    if prompt:
        entry["label"] = clean(prompt)
    if not entry.get("label"):
        base = os.path.basename(cwd.rstrip("/")) if cwd else ""
        entry["label"] = clean(base) if base else sid[:8]

    entry["cwd"] = cwd
    entry["state"] = state
    entry["tool"] = tool
    entry["ts"] = time.time()
    sessions[sid] = entry

    # ---- 清理过期会话 ----
    now = time.time()
    for k in [k for k, v in sessions.items()
              if now - v.get("ts", 0) > STALE_SEC]:
        sessions.pop(k, None)
    save_sessions(sessions)

    # ---- 推给设备（USB 桥 → WiFi 自动选路）----
    qs = urllib.parse.urlencode({
        "sid": sid[:8],
        "st": state,
        "tool": tool[:16],
        "label": entry["label"],
    })
    wb_common.post(f"/state?{qs}", timeout=TIMEOUT)

    # ---- 真的弹权限框了才提醒设备 ----
    if ev == "Notification":
        log_notice(data)
        if is_permission_notice(data):
            rec = load_pending() or {}
            aqs = urllib.parse.urlencode({
                "req": uuid.uuid4().hex[:8],
                "tool": (rec.get("tool") or tool)[:16],
                "label": entry["label"],
                "cmd": rec.get("cmd", ""),
            })
            wb_common.post(f"/approve?{aqs}", timeout=TIMEOUT)


if __name__ == "__main__":
    try:
        main()
    finally:
        sys.exit(0)
