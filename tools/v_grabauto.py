# -*- coding: utf-8 -*-
"""连取回归：开关链路 / 写盘 / 自动建发放页 / 无命中不入队 / 错误不刷屏 / 热键来回切，
外加手动「取码」按钮：点了以后等悬停回来（5 秒窗口），不回退扫堆，**采到即整行入队**。

实机部分分两种跑法：
  1. 连取：脚本造不出「鼠标悬停在物品上」，要人在游戏里确认——
     开着连取回游戏悬停物品 → 队列多出完整一行；同物品再停几秒 → 不加；
     换一件 → 再加一行。
  2. 手动「取码」：把光标先放到游戏里某个物品上，记下屏幕坐标，然后
         python v_grabauto.py --hover 597,1074
     脚本会：把光标移回工具 → 点「取码」→ 半秒后把光标放回那个物品 →
     断言等待窗口内采到（builtin）、编号框回填、队列 +1 且日志有「入队 #id」；
     再点一次同一件 → 采集仍成功但队列不增（去重）。
     不给 --hover 时只跑「没悬停」用例：点了 5 秒采不到、且不许扫堆填错编号。
"""
import ctypes
import glob
import os
import re
import subprocess
import sys
import time
from configparser import RawConfigParser
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch

u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.SetCursorPos.argtypes = [wt.LONG, wt.LONG]

PID = lambda page, n: 1000 + page * 100 + n
G_ID = PID(2, 1)          # 编号（编辑框）
G_LIST = PID(2, 12)      # 队列列表
G_DEL = PID(2, 13)       # 移除所选
G_GRAB = PID(2, 18)      # 取码（按钮）
G_GAUTO = PID(2, 21)     # 连取（复选框）

SETTINGS = os.path.join(BUILD, "config", "settings.ini")
LOGDIR = os.path.join(BUILD, "config", "logs")

HOVER = None              # --hover X,Y：实机悬停点（屏幕坐标）
argv = sys.argv[1:]
for i, a in enumerate(argv):
    if a == "--hover":
        if i + 1 >= len(argv):
            print("!! --hover 后面要跟 X,Y")
            sys.exit(1)
        try:
            HOVER = tuple(int(v) for v in argv[i + 1].split(","))
        except ValueError:
            print("!! --hover 参数格式不对，应为 X,Y")
            sys.exit(1)


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


def click(h):
    u.SendMessageW(h, 0x00F5, 0, 0)                           # BM_CLICK：按钮/复选框通用


def edit_text(h):
    buf = ctypes.create_unicode_buffer(128)
    u.SendMessageW(h, 0x000D, 128, ctypes.addressof(buf))     # WM_GETTEXT
    return buf.value.strip()


GAME_CLS = "地下城与勇士"     # DNF 主窗口类名（探测输出里那一长串中文）


def game_clear_point():
    """游戏主窗口客户区里一个「没有物品」的点（左下角场景/技能栏一带）。
    返回屏幕坐标；找不到游戏窗口返回 None。
    为什么要它：悬停槽只在鼠标于游戏窗口内移动时才更新，瞬移到工具窗口
    游戏根本收不到通知，槽里会留着上一件物品——真实物理移动才会清。"""
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == GAME_CLS:
            found.append(h)
            return False
        return True

    u.EnumWindows(cb(each), 0)
    if not found:
        return None
    r = wt.RECT()
    u.GetWindowRect(found[0], ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    return (r.left + int(w * 0.15), r.top + int(h * 0.80))


def clear_hover(tag):
    """把光标放到游戏内非物品点，让悬停槽失效。找不到游戏窗口就只报警。"""
    pt = game_clear_point()
    if pt:
        u.SetCursorPos(pt[0], pt[1])
        time.sleep(0.3)                                        # 等游戏处理这次移动
        print("   （光标已放到游戏内非物品点 %s：%s）" % (pt, tag))
        return True
    print("   !! 没找到游戏窗口（类名 %s），无法可靠清空悬停：%s" % (GAME_CLS, tag))
    return False


def probe_slot():
    """跑一次 --grab-probe 看悬停槽里现在有没有物品，返回 id（0 = 空槽）。
    为什么要它：测试期间人可能就在游戏里操作，任何"这 5 秒一定没有悬停"的
    假设都不成立；先探一下，有物品就把"无悬停"用例跳过，别把人的操作记成缺陷。"""
    exe = os.path.join(BUILD, "DNF_GM.exe")
    try:
        subprocess.run([exe, "--grab-probe"], cwd=BUILD, timeout=60,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(os.path.join(BUILD, "config", "grab-probe.txt"),
                  encoding="utf-8", errors="replace") as f:
            m = re.search(r"命中: id=(\d+)", f.read())
        return int(m.group(1)) if m else 0
    except (OSError, subprocess.SubprocessError):
        return None                                            # 探针不可用：不做判断


def wait_box(h, expected, timeout=1.5):
    """等编号框回填到 expected。取码结果先落到 g.grab，再由 tick 写进编辑框，
    比日志晚一两个 tick；读早了会拿到上一件的编号。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if edit_text(h) == expected:
            return True
        time.sleep(0.05)
    return False


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


k32 = ctypes.windll.kernel32
u.GetForegroundWindow.restype = wt.HWND
u.SetForegroundWindow.argtypes = [wt.HWND]
u.GetShellWindow.restype = wt.HWND


def _tid(h):
    return u.GetWindowThreadProcessId(h, None)


def force_foreground(h):
    """把前台切到 h。最小化**不会**改变前台窗口——被最小化的窗口仍然可以是
    前台窗口，这时程序按设计"自己在前面就不认这个键"，热键用例就会假失败。
    SetForegroundWindow 受前台锁限制，先把输入挂到当前前台线程上再调。"""
    fid = _tid(u.GetForegroundWindow())
    sid = k32.GetCurrentThreadId()
    try:
        u.AttachThreadInput(sid, fid, True)
    except Exception:
        pass
    try:
        u.SetForegroundWindow(h)
    finally:
        u.AttachThreadInput(sid, fid, False)


def hotkey_tap(main, vk, tag=""):
    """最小化本窗口 → 把前台让给桌面 → 按热键 → 恢复。返回前台切换是否成功。"""
    u.ShowWindow(main, 6)
    time.sleep(0.5)
    force_foreground(u.GetShellWindow())                       # 桌面：前台不是本工具，且按键无副作用
    time.sleep(0.3)
    ok = u.GetForegroundWindow() != main
    tap(vk)
    time.sleep(1.5)
    u.ShowWindow(main, 9)
    time.sleep(1.2)          # 恢复是异步的：不等等再取控件，句柄和矩形都不可靠
    if not ok:
        print("   !! 最小化后前台仍是本工具（%s），这次按键会被程序按设计忽略" % tag)
    return ok


AUTO0 = ini_get("auto") or "0"
HOTKEY = ini_get("hotkey", "ui") or "Tab"
print("初始：settings.ini [grab] auto=%s · 热键=%s" % (AUTO0, HOTKEY))

p = None
try:
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main(p)
    u.SendMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)     # 连接
    time.sleep(4.5)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0201, 1, (12 << 16) | 40)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0202, 0, (12 << 16) | 40)
    time.sleep(3.0)

    # 关键场景：发放页从未打开过。人在别的页按热键开连取，工具得自己把发放页建出来
    switch(main, 3)                                            # 先切到邮件页
    time.sleep(1.2)
    vk = vk_of(HOTKEY)
    if not vk:
        print("!! 热键 %r 无法映射成虚拟键，跳过热键用例" % HOTKEY)
        sys.exit(1)
    hotkey_tap(main, vk, "用例 1")

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
    for name, h in (("取码", g_grab), ("连取", ga), ("移除", g_del)):
        r = wt.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        rects[name] = r.left
    ok_order = rects["取码"] < rects["连取"] < rects["移除"]
    print("3) 按钮行顺序（取码 %d < 连取 %d < 移除 %d）: %s" %
          (rects["取码"], rects["连取"], rects["移除"], "OK" if ok_order else "FAIL"))

    # 无命中时段：队列必须不动，日志不许暴涨（失败不写日志）。
    # 「取码失败」只进状态栏，而状态栏文本跨进程读不到，错误去重靠代码评审
    # 先把光标请出物品：悬停着的话连取每 250ms 采一次，日志会唰唰地涨
    clear_hover("用例 4：保证这 8 秒没有悬停")
    n0 = u.SendMessageW(g_list, 0x1004, 0, 0)                 # LVM_GETITEMCOUNT
    l0 = log_lines()
    time.sleep(8.0)
    n1 = u.SendMessageW(g_list, 0x1004, 0, 0)
    dl = log_lines() - l0
    ok_quiet = (n1 == n0) and dl <= 2
    print("4) 8 秒无悬停：队列 %d→%d · 日志 +%d 行" % (n0, n1, dl))
    print("   OK: 不入队=%s 日志不暴涨=%s" % (
        "OK" if n1 == n0 else "FAIL", "OK" if dl <= 2 else "FAIL"))

    # ── 手动「取码」按钮：点了等悬停回来（5 秒窗口），不再扫堆猜 ──────────
    # 连取先关掉：它每 250ms 起一次快轮次，和手动作业抢同一条作业线程
    if checked(ga):
        click(ga)
        time.sleep(0.8)
    print("   （连取已关：按钮用例要的是作业线程清清白白）")

    g_id = ctl(main, G_ID)
    # 光标放到游戏内非物品点：悬停槽失效，等同于"用户没在看任何物品"
    clear_hover("用例 7：无悬停")

    # 7) 没悬停时点「取码」：等满 5 秒也采不到；关键是不许走扫堆回退
    #    （老行为会在这里填个"全堆最高分"的错编号，比如悬停着银魂却填复活币）
    #    编号框可能早就被连取的头一回采集填过了，所以断言"没变化"而不是"为空"
    #    日志是 1 秒级的缓冲落盘，断言只认「取码」相关行，免得被别行的落盘时机绊倒
    slot = probe_slot()
    if slot:
        # 探到槽里有物品：人正在游戏里操作。这条用例的前提不成立，跳过而不是判错
        print("7) 无悬停用例：跳过（悬停槽里有 id=%d，游戏正在被人操作）" % slot)
        ok_wait = None
    else:
        id_before = edit_text(g_id)
        l0 = log_lines()
        click(g_grab)
        time.sleep(6.5)                                        # 等过 5s 等待窗口
        n1 = u.SendMessageW(g_list, 0x1004, 0, 0)
        new_lines = log_text().splitlines()[l0:]
        grab_lines = [x for x in new_lines if "取码 #" in x or "scan" in x]
        id_txt = edit_text(g_id)
        ok_wait = (n1 == n0) and not grab_lines and id_txt == id_before
        print("7) 无悬停点「取码」：等 5s · 队列 %d→%d · 新日志 %d 行 · 编号框 %r→%r" %
              (n0, n1, len(new_lines), id_before, id_txt))
        print("   OK: 没乱填新编号=%s 不扫堆=%s 队列不动=%s" % (
            "OK" if id_txt == id_before else "FAIL",
            "OK" if not grab_lines else "FAIL",
            "OK" if n1 == n0 else "FAIL"))

    # 8) 实机悬停（--hover X,Y）：点「取码」→ 把鼠标放回游戏物品 → 等待窗口内采到
    #    采到就该整行入队（不是只填编号）
    ok_live = None
    ok_add = None
    ok_dup = None
    if HOVER:
        nb = u.SendMessageW(g_list, 0x1004, 0, 0)              # 入队前队列长度
        l1 = log_lines()
        click(g_grab)
        time.sleep(0.7)                                        # 等待窗口进行中
        u.SetCursorPos(HOVER[0], HOVER[1])                     # 鼠标放回游戏物品上
        m = None
        hit = []
        for _ in range(50):                                    # 最多再等 5s
            time.sleep(0.1)
            new_lines = log_text().splitlines()[l1:]
            hit = [x for x in new_lines if "取码 #" in x]
            if hit:
                m = re.search(r"取码 #(\d+) .*?· (builtin|scan) ·", hit[-1])
                break
        filled = bool(m) and wait_box(g_id, m.group(1))        # 编号框晚一两个 tick，等它
        txt = edit_text(g_id)
        ok_live = bool(m) and m.group(2) == "builtin" and filled
        print("8) 实机悬停 %s：日志 %s · 编号框 %r" % (
            HOVER, hit[-1] if hit else "（无命中）", txt))
        print("   OK: 等待窗口内采到=%s 回填编号=%s" % (
            "OK" if (m and m.group(2) == "builtin") else "FAIL",
            "OK" if (m and txt == m.group(1)) else "FAIL"))

        # 入队：跨进程读列表文字要往目标堆里写，危险（SB_GETTEXTW 那次教训），
        # 所以只用行数 + 日志断言：日志里的「入队 #id」就是那一行的内容
        time.sleep(0.6)                                        # 等 tick 把结果落进队列
        na = u.SendMessageW(g_list, 0x1004, 0, 0)
        all_lines = log_text().splitlines()
        add_lines = [x for x in all_lines[l1:] if "入队 #" in x]
        tag = "入队 #" + (m.group(1) + " " if m else "")
        added_now = any(tag in x for x in add_lines)
        # 这件可能在本会话早先就入过队（比如连取开着的时候被悬停采到）：
        # 那种情况这次"跳过"才是对的，不能按"队列没加"判失败
        already = any(tag in x for x in all_lines[:l1])
        ok_add = bool(m) and ((na == nb + 1 and added_now) or
                              (na == nb and not added_now and already))
        print("9) 采到即入队：队列 %d→%d · 新增入队行 %d · 早先已有=%s" %
              (nb, na, len(add_lines), already))
        print("   OK: 入队=%s（%s）" % (
            "OK" if ok_add else "FAIL",
            "本次新增一行" if added_now else ("本来就已入队，跳过正确" if already else "既没入队也没记录")))

        # 10) 同一件再采一次：必须跳过（队列不增），但采集本身仍要成功
        l2 = log_lines()
        click(g_grab)
        time.sleep(0.7)
        u.SetCursorPos(HOVER[0], HOVER[1])
        again = None
        for _ in range(50):
            time.sleep(0.1)
            ls = [x for x in log_text().splitlines()[l2:] if "取码 #" in x]
            if ls:
                mm = re.search(r"取码 #(\d+) ", ls[-1])
                again = mm.group(1) if mm else ""
                break
        time.sleep(0.6)
        nd = u.SendMessageW(g_list, 0x1004, 0, 0)
        dup_add = [x for x in log_text().splitlines()[l2:] if "入队 #" in x]
        if again is not None and m and again != m.group(1):
            print("10) 同编号再采：跳过（第二次采到的是 #%s，环境变了）" % again)
            ok_dup = None
        else:
            ok_dup = bool(again) and nd == na and not dup_add
            print("10) 同编号再采：再采成功=%s · 队列 %d→%d · 新增入队行 %d" %
                  (bool(again), na, nd, len(dup_add)))
            print("   OK: 去重=%s" % ("OK" if (nd == na and not dup_add) else "FAIL"))
    else:
        print("8) 实机悬停用例：跳过（要跑加 --hover X,Y，X,Y 是游戏里某物品的屏幕坐标）")

    # 光标请回非物品点：热键用例期间连取是开的，别让它一直采同一件
    clear_hover("热键用例前")

    # 连取恢复开启：下面的热键用例接着按"再按=关"的预期走
    if not checked(ga):
        click(ga)
        time.sleep(0.8)

    # 热键来回切。每次按之前先最小化：程序要求前台不是它自己才认这个键
    # 日志按新增行看：前面按钮用例也开关过连取，整份日志里早就有关闭记录
    l_hot = log_lines()
    hotkey_tap(main, vk, "用例 5")
    off_ini = ini_get("auto") == "0"
    off_chk = not checked(ga)
    off_lg = any("连取：已关闭" in x for x in log_text().splitlines()[l_hot:])
    hotkey_tap(main, vk, "用例 6")
    on_ini = ini_get("auto") == "1"
    on_chk = checked(ga)
    print("5) 热键再按（关）: ini=%s 勾选=%s 日志=%s" % (off_ini, off_chk, off_lg))
    print("6) 热键再按（开）: ini=%s 勾选=%s" % (on_ini, on_chk))
    print("   OK: 热键关=%s 热键开=%s" % (
        "OK" if (off_ini and off_chk and off_lg) else "FAIL",
        "OK" if (on_ini and on_chk) else "FAIL"))

    checks = [ok_built, ok_ini, ok_chk, ok_lg,
              ok_order, ok_quiet,
              off_ini and off_chk and off_lg, on_ini and on_chk]
    # None = 环境不满足、用例跳过（不算通过也不算失败）
    for extra in (ok_wait, ok_live, ok_add, ok_dup):
        if extra is not None:
            checks.append(extra)
    print("\n结果：%s（%d/%d）" % ("通过" if all(checks) else "失败",
                                 sum(1 for c in checks if c), len(checks)))
    print("待人工确认（连取，游戏里连续悬停多件）：每件都入队带名字、同编号跳过、换物品再加行")
    if not HOVER:
        print("手动「取码」的实机命中用例：python %s --hover X,Y" %
              os.path.basename(__file__))
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
