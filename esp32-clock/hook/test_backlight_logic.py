#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_backlight_logic.py —— 固件背光逻辑的宿主机测试（不需要板子）

固件里的 C++ 逻辑没法直接在 Mac 上跑，这里把它逐行镜像成 Python，
然后跑一个场景矩阵 + 边界用例。镜像必须和 .ino 里的代码一一对应：
    applyBrightness()  ←→  want_brightness()
    isNightNow()       ←→  is_night()
    BL| 命令换算        ←→  bl_force_from_pct()

场景矩阵覆盖：白天/夜间 × 提亮过期/未过期 × 熄屏 × 串口强制值。

用法：python3 hook/test_backlight_logic.py
"""

import sys

BL_DAY = 255
BL_NIGHT = 30


def is_night(hh, mm):
    """镜像 .ino isNightNow()：20:00~次日 09:00"""
    hm = hh * 100 + mm
    return hm >= 2000 or hm < 900


def want_brightness(night, boost_active, screen_off, bl_force):
    """镜像 .ino applyBrightness() 的取值顺序，一行都不能差"""
    if bl_force >= 0:
        return bl_force
    if screen_off:
        return 0
    if night and not boost_active:
        return BL_NIGHT
    return BL_DAY


def bl_force_from_pct(pct):
    """镜像 .ino BL| 命令的换算：pct<=0 -> 0，pct>100 -> 255，否则 pct*255/100（整数除法）"""
    if pct <= 0:
        return 0
    if pct > 100:
        return 255
    return pct * 255 // 100


def parse_state_bl(line):
    """镜像 verify_backlight.py read_bl() 对 STATE|bl 行的解析"""
    prefix = "STATE|bl|"
    if not line.startswith(prefix):
        return None
    f = line[len(prefix):].split("|")
    hh, mm = f[1].split(":")
    return {"level": int(f[0]), "hh": int(hh), "mm": int(mm),
            "night": f[2] == "1", "off": f[3] == "1"}


def main():
    ok = True

    def check(label, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print("  %-42s %s  期望 %-6r 实得 %r" % (
            label, "OK " if good else "FAIL", want, got))

    print("== 夜间判断（20:00~09:00 跨零点）==")
    for hh, mm, want in [(19, 59, False), (20, 0, True), (20, 1, True),
                         (23, 59, True), (0, 0, True), (8, 59, True),
                         (9, 0, False), (12, 30, False)]:
        check("%02d:%02d -> 夜间=%s" % (hh, mm, want), is_night(hh, mm), want)

    print("\n== 亮度决策矩阵 ==")
    cases = [
        # (夜间, 提亮有效, 熄屏, 串口强制) -> 期望级别
        (False, False, False, -1, BL_DAY),    # 白天平常
        (False, True,  False, -1, BL_DAY),    # 白天被碰（无副作用）
        (True,  False, False, -1, BL_NIGHT),  # 夜间平常
        (True,  True,  False, -1, BL_DAY),    # 夜间被碰：提亮 30 秒内
        (True,  False, True,  -1, 0),         # 夜间熄屏
        (False, False, True,  -1, 0),         # 白天熄屏
        (True,  True,  True,  -1, 0),         # 夜间提亮中熄屏（熄屏优先）
        (True,  False, False, 128, 128),      # 串口强制值优先于夜间
        (True,  False, True,  64,  64),       # 串口强制值优先于熄屏
        (False, False, False, 0,   0),        # 串口强制 0
    ]
    for night, boost, off, force, want in cases:
        check("夜=%d 亮=%d 熄=%d 强制=%d" % (night, boost, off, force),
              want_brightness(night, boost, off, force), want)

    print("\n== BL|<0-100> 换算（与固件同样的整数除法）==")
    for pct, want in [(0, 0), (1, 2), (12, 30), (50, 127), (100, 255), (101, 255), (-5, 0)]:
        check("BL|%d -> %d" % (pct, want), bl_force_from_pct(pct), want)

    print("\n== STATE|bl 解析 ==")
    check("白天全亮", parse_state_bl("STATE|bl|255|18:35|0|0"),
          {"level": 255, "hh": 18, "mm": 35, "night": False, "off": False})
    check("夜间调暗", parse_state_bl("STATE|bl|30|23:10|1|0"),
          {"level": 30, "hh": 23, "mm": 10, "night": True, "off": False})
    check("熄屏", parse_state_bl("STATE|bl|0|02:05|1|1"),
          {"level": 0, "hh": 2, "mm": 5, "night": True, "off": True})
    check("非 STATE|bl 行", parse_state_bl("STATE|ok"), None)
    check("空串", parse_state_bl(""), None)

    print("\n结果: %s" % ("全部通过 ✓" if ok else "有失败项 ✗"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
