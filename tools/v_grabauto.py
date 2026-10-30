# -*- coding: utf-8 -*-
"""连取回归：开关链路 / 写盘 / 自动建发放页 / 无命中不入队 / 错误不刷屏 / 热键来回切。

脚本造不出「游戏里鼠标悬停在某件物品上」这个状态，所以采集与去重要人在游戏里确认：
  1. 开着连取，回游戏把鼠标放到任一物品上 → 队列应多出完整一行（带名字）
  2. 鼠标在同一件物品上再停几秒 → 队列不应再加（同编号跳过）
  3. 换一件物品 → 队列再加一行
这里验的是其余的机制：切换、落盘、状态/日志文案、没东西可采时不乱动。
"""
import ctypes
import glob
import os
import subprocess
import sys
import time
from configparser import RawConfigParser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch

PID = lambda page, n: 1000 + page * 100 + n
G_LIST = PID(2, 12)      # 队列列表
G_DEL = PID(2, 13)       # 移除所选
G_GRAB = PID(2, 18)      # 取码（按钮）
G_GAUTO = PID(2, 21)     # 连取（复选框）

SETTINGS = os.path.join(BUILD, "config", "settings.ini")
LOGDIR = os.path.join(BUILD, "config", "logs")


def ini_get(key, sec="grab"):
    cp = RawConfigParser()
    cp.read(SETTINGS, encoding="utf-8")
    return cp.get(sec, key, fallback=None)


def log_text():
    fs = sorted(glob.glob(os.path.join(LOGDIR, "log-*.txt")))
    if not fs:
        return ""
    # 日志是 UTF-8 直接落盘；GBK 是早期的老文件，读不动就换
    for enc in ("utf-8", "gbk"):
        try:
            with open(fs[-1], encoding=enc, errors="strict") as f:
                return f.read()
        except (UnicodeDecodeError, OSError):
            continue
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return f.read()


def log_lines():
    return len(log_text().splitlines())


def ctl(main, cid):
    """在可见的 DnfPage 下找控件。页面控件都挂在页窗口上，不在主窗口上。"""
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "DnfPage" and u.IsWindowVisible(h):
            w = u.GetDlgItem(h, cid)
            if w:
                found.append(w)
                return False
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return found[0] if found else None


def checked(h):
    return u.SendMessageW(h, 0x00F0, 0, 0) == 1               # BM_GETCHECK（不是 0x00F2 的 GETSTATE，那带焦点位）


def vk_of(name):
    if not name:
        return None
    if name[0] == "F" and len(name) > 1 and name[1].isdigit():
        return 0x70 + int(name[1:]) - 1
    m = {"Tab": 0x09, "Insert": 0x2D, "Home": 0x24, "End": 0x23, "PageUp": 0x21, "PageDown": 0x22}
    if name in m:
        return m[name]
    if len(name) == 1:
        c = name.upper()
        if c.isalnum():
            return ord(c)
    return None


def tap(vk):
    # 按住 200ms：程序 100ms 一个 tick，30ms 的点按会被整个漏过去
    u.keybd_event(vk, 0, 0, 0)
    time.sleep(0.2)
    u.keybd_event(vk, 0, 2, 0)
    time.sleep(0.1)


AUTO0 = ini_get("auto") or "0"
HOTKEY = ini_get("hotkey", "ui") or "Tab"
print("初始：settings.ini [grab] auto=%s · 热键=%s" % (AUTO0, HOTKEY))

p = None
try:
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main()
    u.SendMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)     # 连接
    time.sleep(4.5)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0201, 1, (12 << 16) | 40)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0202, 0, (12 << 16) | 40)
    time.sleep(3.0)

    # 关键场景：发放页从未打开过。人在别的页按热键开连取，工具得自己把发放页建出来
    switch(main, 3)                                            # 先切到邮件页
    time.sleep(1.2)
    u.ShowWindow(main, 6)                                      # 最小化：热键要求前台不是本工具
    vk = vk_of(HOTKEY)
    if not vk:
        print("!! 热键 %r 无法映射成虚拟键，跳过热键用例" % HOTKEY)
        sys.exit(1)
    tap(vk)
    time.sleep(1.5)
    u.ShowWindow(main, 9)                                      # 恢复
    time.sleep(1.2)          # 恢复是异步的：不等等再取控件，句柄和矩形都不可靠

    ga = ctl(main, G_GAUTO)
    g_grab = ctl(main, G_GRAB)
    g_del = ctl(main, G_DEL)
    g_list = ctl(main, G_LIST)
    print("发放页控件：连取=%s 取码=%s 移除=%s 队列=%s" %
          (bool(ga), bool(g_grab), bool(g_del), bool(g_list)))
    ok_built = all([ga, g_grab, g_del, g_list])
    print("1) 热键开启时自动建出发放页: %s" % ("OK" if ok_built else "FAIL"))
    if not ok_built:
        sys.exit(1)

    ok_ini = (ini_get("auto") == "1")
    ok_chk = checked(ga)
    lg = log_text()
    ok_lg = "连取：已开启" in lg
    print("2) 热键开启后：ini=%s 勾选=%s" % (ok_ini, ok_chk))
    print("   OK: 写盘=%s 勾选=%s 日志=%s" % (ok_ini, ok_chk, "OK" if ok_lg else "FAIL"))

    # 布局：复选框夹在「取码」和「移除所选」之间
    rects = {}
    from ctypes import wintypes as wt
    u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
    for name, h in (("取码", g_grab), ("连取", ga), ("移除", g_del)):
        r = wt.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        rects[name] = r.left
    ok_order = rects["取码"] < rects["连取"] < rects["移除"]
    print("3) 按钮行顺序（取码 %d < 连取 %d < 移除 %d）: %s" %
          (rects["取码"], rects["连取"], rects["移除"], "OK" if ok_order else "FAIL"))

    # 无命中时段：队列必须不动，日志不许暴涨（失败不写日志）。
    # 「取码失败」只进状态栏，而状态栏文本跨进程读不到，错误去重靠代码评审
    n0 = u.SendMessageW(g_list, 0x1004, 0, 0)                 # LVM_GETITEMCOUNT
    l0 = log_lines()
    time.sleep(8.0)
    n1 = u.SendMessageW(g_list, 0x1004, 0, 0)
    dl = log_lines() - l0
    ok_quiet = (n1 == n0) and dl <= 2
    print("4) 8 秒无悬停：队列 %d→%d · 日志 +%d 行" % (n0, n1, dl))
    print("   OK: 不入队=%s 日志不暴涨=%s" % (
        "OK" if n1 == n0 else "FAIL", "OK" if dl <= 2 else "FAIL"))

    # 热键来回切。每次按之前先最小化：程序要求前台不是它自己才认这个键
    def tap_hot(main, vk):
        u.ShowWindow(main, 6)
        time.sleep(0.4)
        tap(vk)
        u.ShowWindow(main, 9)
        time.sleep(1.2)

    tap_hot(main, vk)
    off_ini = ini_get("auto") == "0"
    off_chk = not checked(ga)
    off_lg = "连取：已关闭" in log_text()
    tap_hot(main, vk)
    on_ini = ini_get("auto") == "1"
    on_chk = checked(ga)
    print("5) 热键再按（关）: ini=%s 勾选=%s 日志=%s" % (off_ini, off_chk, off_lg))
    print("6) 热键再按（开）: ini=%s 勾选=%s" % (on_ini, on_chk))
    print("   OK: 热键关=%s 热键开=%s" % (
        "OK" if (off_ini and off_chk and off_lg) else "FAIL",
        "OK" if (on_ini and on_chk) else "FAIL"))

    checks = [ok_built, ok_ini, ok_chk, ok_lg,
              ok_order, ok_quiet, off_ini and off_chk and off_lg, on_ini and on_chk]
    print("\n结果：%s（%d/%d）" % ("通过" if all(checks) else "失败",
                                 sum(1 for c in checks if c), len(checks)))
    print("待人工确认（游戏里悬停物品）：入队带名字、同编号跳过、换物品再加行")
finally:
    if p:
        p.kill()
    # 复原 ini，别把连取状态留给下次启动
    if (ini_get("auto") or "0") != AUTO0:
        cp = RawConfigParser()
        cp.read(SETTINGS, encoding="utf-8")
        cp.set("grab", "auto", AUTO0)
        with open(SETTINGS, "w", encoding="utf-8") as f:
            cp.write(f, space_around_delimiters=True)
        print("已复原 [grab] auto=%s" % AUTO0)
    else:
        print("[grab] auto 未变，无需复原")
