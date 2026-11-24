#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_sleep_logic.py —— 「熄屏后深度睡眠」决策逻辑的宿主机测试（不需要板子）

固件里的 C++ 没法在 Mac 上跑，这里把它逐行镜像成 Python 再跑场景矩阵。
镜像必须和 .ino 一一对应：
    maybeAutoSleep()   ←→  should_sleep()
    autoOffActive()    ←→  auto_off_active()
    secsUntilOffEnd()  ←→  secs_until_off_end()

三条前提缺一不睡：真的熄屏了 / 没插 USB / 处在自动熄屏时段。
另外熄屏要满 2 分钟 —— 夜里碰一下屏幕看时间，不该每次都重启。

用法：python3 hook/test_sleep_logic.py
"""

import sys

AUTO_SLEEP_AFTER_MS = 120000     # 镜像 .ino AUTO_SLEEP_AFTER_MS


def auto_off_active(hh, start, end):
    """镜像 .ino autoOffActive()：支持跨 0 点"""
    if start < 0:
        return False
    if start <= end:
        return start <= hh < end
    return hh >= start or hh < end


def secs_until_off_end(hh, mm, end):
    """镜像 .ino secsUntilOffEnd()：距离时段结束还有多少秒"""
    now_min = hh * 60 + mm
    end_min = end * 60
    diff = end_min - now_min
    if diff <= 0:
        diff += 24 * 60
    return diff * 60


def should_sleep(auto_sleep, panel_asleep, bat_chg, bat_full,
                 off_active, off_ms, after_ms=AUTO_SLEEP_AFTER_MS):
    """镜像 .ino maybeAutoSleep()，判断顺序一行都不能差"""
    if not auto_sleep or not panel_asleep:
        return False
    if bat_chg or bat_full:          # 插着线，不省这点电
        return False
    if not off_active:               # 只在夜间睡
        return False
    if off_ms < after_ms:
        return False
    return True


def main():
    ok = True

    def check(label, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print("  %-46s %s  期望 %-6r 实得 %r" % (
            label, "OK " if good else "FAIL", want, got))

    print("== 自动熄屏时段（默认 20~10，跨 0 点）==")
    for hh, want in [(19, False), (20, True), (23, True), (0, True),
                     (7, True), (9, True), (10, False), (12, False)]:
        check("%02d 点 -> 时段内=%s" % (hh, want),
              auto_off_active(hh, 20, 10), want)
    check("OFF|off 关闭后永不生效", auto_off_active(23, -1, -1), False)

    print("\n== 睡眠时长（睡到时段结束）==")
    for hh, mm, want in [(20, 5, 50100),    # 20:05 -> 次日 10:00
                         (23, 30, 37800),   # 23:30 -> 次日 10:00
                         (9, 59, 60),       # 09:59 -> 10:00
                         (0, 0, 36000)]:    # 00:00 -> 10:00
        check("%02d:%02d -> 睡 %d 秒" % (hh, mm, want),
              secs_until_off_end(hh, mm, 10), want)

    print("\n== 睡眠决策矩阵 ==")
    cases = [
        # (auto_sleep, panel_asleep, chg, full, off_active, off_ms) -> 期望
        (True,  True,  False, False, True,  120000, True),   # 夜间熄屏满 2 分钟
        (True,  True,  False, False, True,  119000, False),  # 差 1 秒，不睡
        (True,  True,  False, False, True,  60000,  False),  # 熄屏 1 分钟
        (True,  True,  True,  False, True,  600000, False),  # 充电中不睡
        (True,  True,  False, True,  True,  600000, False),  # 已充满不睡
        (True,  True,  False, False, False, 600000, False),  # 白天熄屏不睡
        (True,  False, False, False, True,  600000, False),  # 只是背光灭了，面板没睡
        (False, True,  False, False, True,  600000, False),  # SLEEP|0 关掉
        (True,  True,  False, False, True,  0,      False),  # 刚熄屏
    ]
    for a, p, c, f, oa, ms, want in cases:
        check("开关=%d 面板睡=%d 充电=%d 满=%d 夜间=%d 熄屏%dms" % (a, p, c, f, oa, ms),
              should_sleep(a, p, c, f, oa, ms), want)

    print("\n结果: %s" % ("全部通过 ✓" if ok else "有失败项 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
