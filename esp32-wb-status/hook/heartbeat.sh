#!/bin/bash
# ------------------------------------------------------------
# 心跳脚本：每 30 秒 ping 一次设备
#
# 为什么需要它：
#   设备靠「多久没收到请求」判断是否失联。但 WorkBuddy 空闲时
#   本来就没有事件，设备无法区分「你在休息」和「Mac 挂了」。
#   有了心跳，90 秒收不到就能准确判定离线。
#
# 用法：
#   ./heartbeat.sh                 # 前台跑，Ctrl-C 停
#   ./heartbeat.sh &               # 后台跑
#   nohup ./heartbeat.sh >/dev/null 2>&1 &
#
# 想让它在开机时自动跑，需要配 launchd LaunchAgent
#（属于系统级配置，请自行决定，脚本本身不碰）
# ------------------------------------------------------------

DEVICE="${WB_STATUS_URL:-http://wbstatus.local}"
INTERVAL="${HB_INTERVAL:-30}"

echo "心跳开始 -> $DEVICE/ping  每 ${INTERVAL}s"
ok=0; fail=0

while true; do
  if curl -s -m 3 -o /dev/null "$DEVICE/ping"; then
    ok=$((ok+1))
    [ $((ok % 20)) -eq 1 ] && echo "[$(date +%H:%M:%S)] 设备在线（累计 $ok 次）"
    fail=0
  else
    fail=$((fail+1))
    [ $fail -eq 3 ] && echo "[$(date +%H:%M:%S)] 连续 3 次 ping 失败，设备可能离线"
  fi
  sleep "$INTERVAL"
done
