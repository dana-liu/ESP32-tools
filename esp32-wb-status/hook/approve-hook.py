#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
⚠️ 已废弃，不要注册到 settings.json

这个脚本最初挂在 PreToolUse 上，每次 Bash/Write/Edit 都往设备推一条提醒。
问题是 PreToolUse 阶段**无法知道客户端到底会不会弹权限框** —— 大部分调用是
自动放行的，结果设备被无意义的提醒刷屏。

现在的做法：审批提醒合并进 `wb-status-hook.py`，只在收到
**Notification 事件且内容含 permission/approval/确认** 时才推设备。
PreToolUse 阶段只把 tool_input 缓存到 ~/.wb-status/pending.json，
等 Notification 来了再取命令内容。

保留这个文件仅作参考。
"""

import json
import os
import sys
import urllib.parse
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_common

PUSH_TIMEOUT = 1.0

# 需要在设备上提醒的工具
ALLOW_TOOLS = {"Bash", "Write", "Edit"}

# 出现这些字样就不打扰设备（这些必须回电脑确认）
DANGER_PATTERNS = [
    "rm -rf", "rm -fr", "sudo ", "git push --force", "git push -f",
    "mkfs", "dd if=", "chmod 777 /", "shutdown", "reboot",
    "npm publish", "vercel --prod", "kubectl delete", "docker rm",
    ":(){", "> /dev/sd",
]


def needs_notice(tool: str, tool_input) -> bool:
    if tool not in ALLOW_TOOLS:
        return False
    blob = json.dumps(tool_input, ensure_ascii=False).lower()
    return not any(p.lower() in blob for p in DANGER_PATTERNS)


def lookup_label(sid: str) -> str:
    """从 wb-status-hook.py 维护的会话表里查任务名"""
    try:
        with open(wb_common.SESS_FILE) as f:
            sessions = json.load(f)
        return sessions.get(sid, {}).get("label", "")
    except Exception:
        return ""


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return

    tool = data.get("tool_name", "")
    tool_input = data.get("tool_input", {})

    if not needs_notice(tool, tool_input):
        return

    if not wb_common.has_serial_port() and wb_common.recently_down():
        return                      # 设备刚失联过，别每次都试

    sid = data.get("session_id", "")
    label = lookup_label(sid)
    if not label:
        cwd = (data.get("cwd") or "").rstrip("/")
        label = os.path.basename(cwd) if cwd else "未命名任务"

    qs = urllib.parse.urlencode({
        "req": uuid.uuid4().hex[:8],
        "tool": tool,
        "label": label,
        "cmd": json.dumps(tool_input, ensure_ascii=False)[:150],
    })

    if wb_common.post("/approve?" + qs, timeout=PUSH_TIMEOUT):
        wb_common.mark_up()
    else:
        wb_common.mark_down()

    # 关键：什么都不输出 → 客户端照常弹窗


if __name__ == "__main__":
    try:
        main()
    finally:
        sys.exit(0)
