# -*- coding: utf-8 -*-
"""连续取码回归：默认开启 / 复选框链路 / 写盘 / 自动建发放页 / 无命中不入队 / 静默，
实机（--hover X,Y）：悬停即采、编号回填、采到即整行入队、同编号去重；
热键（输入可用时）：来回切、写盘、日志。

跑法：
  python v_grabauto.py                # 无实机悬停
  python v_grabauto.py --hover X,Y    # 光标先放在游戏里某个物品上，X,Y 是屏幕坐标

环境说明：热键和 --hover 都要真实输入桌面（合成按键要送进被测程序的
GetAsyncKeyState）。RDP 会话断开时桌面没有前台窗口、按键哪儿都不送，
这两组用例会自动跳过而不是假失败——复选框走 SendMessage，不受影响。
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
G_ADD = PID(2, 11)        # 加入队列（按钮）
G_LIST = PID(2, 12)       # 队列列表
G_DEL = PID(2, 13)        # 移除所选
G_GAUTO = PID(2, 20)      # 连续取码（复选框）

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


# ---------------- settings.ini ----------------
def ini_get(key, sec="grab"):
    cp = RawConfigParser()
    cp.read(SETTINGS, encoding="utf-8")
    return cp.get(sec, key, fallback=None)


def ini_set(key, val, sec="grab"):
    cp = RawConfigParser()
    cp.read(SETTINGS, encoding="utf-8")
    if not cp.has_section(sec):
        cp.add_section(sec)
    cp.set(sec, key, val)
    with open(SETTINGS, "w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=True)


def ini_del(key, sec="grab"):
    cp = RawConfigParser()
    cp.read(SETTINGS, encoding="utf-8")
    if cp.has_section(sec):
        cp.remove_option(sec, key)
    with open(SETTINGS, "w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=True)


# ---------------- 日志 ----------------
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


# ---------------- 被测程序控件 ----------------
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
    为什么要它：测试期间人可能就在游戏里操作，任何"这 8 秒一定没有悬停"的
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


# ---------------- 输入合成（热键 / 悬停要真实输入桌面）----------------
k32 = ctypes.windll.kernel32
u.GetForegroundWindow.restype = wt.HWND
u.SetForegroundWindow.argtypes = [wt.HWND]
u.DefWindowProcW.restype = ctypes.c_longlong
u.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u.CreateWindowExW.restype = wt.HWND
u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u.RegisterClassExW.argtypes = [ctypes.c_void_p]
k32.GetModuleHandleW.restype = wt.HMODULE


def _tid(h):
    return u.GetWindowThreadProcessId(h, None)


def force_foreground(h):
    """把前台切到 h。SetForegroundWindow 受前台锁限制，先把输入挂到当前
    前台线程上再调。"""
    fid = _tid(u.GetForegroundWindow())
    sid = k32.GetCurrentThreadId()
    try:
        u.AttachThreadInput(sid, fid, True)
    except Exception:
        pass
    try:
        u.SetForegroundWindow(h)
    finally:
        try:
            u.AttachThreadInput(sid, fid, False)
        except Exception:
            pass


HELPER = None
_HELPER_PROC = None


class _WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HANDLE), ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HANDLE), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HANDLE)]


def make_helper():
    """一个自己的最小顶层窗，用来当"别的程序"的前台替身。"""
    global HELPER, _HELPER_PROC
    if HELPER and u.IsWindow(HELPER):
        return HELPER
    cls = "GmVGrabHelper"
    WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, ctypes.c_uint,
                                wt.WPARAM, wt.LPARAM)
    _HELPER_PROC = WNDPROC(lambda h, m, w, l: u.DefWindowProcW(h, m, w, l))
    wc = _WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(_WNDCLASSEXW)
    wc.lpfnWndProc = ctypes.cast(_HELPER_PROC, ctypes.c_void_p).value
    wc.hInstance = k32.GetModuleHandleW(None)
    wc.lpszClassName = cls
    u.RegisterClassExW(ctypes.byref(wc))
    HELPER = u.CreateWindowExW(0, cls, "gm-test-helper", 0x00CF0000, 40, 40, 220, 120,
                               0, 0, wc.hInstance, 0)
    return HELPER


def input_dead():
    """输入桌面不可用（RDP 断开、锁屏）：GetForegroundWindow 恒为 NULL，
    SetForegroundWindow 激不活任何窗口，合成按键哪儿都不送。"""
    helper = make_helper()
    if not helper:
        return True
    u.ShowWindow(helper, 5)
    time.sleep(0.3)
    force_foreground(helper)
    time.sleep(0.3)
    fg = u.GetForegroundWindow()
    u.ShowWindow(helper, 0)
    return fg in (None, 0)


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


def hotkey_tap(main, vk, tag=""):
    """最小化本窗口 → 前台让给帮助窗 → 按热键 → 恢复。返回前台切换是否成功。"""
    helper = make_helper()
    u.ShowWindow(helper, 5)
    u.ShowWindow(main, 6)                                      # SW_MINIMIZE
    time.sleep(0.5)
    force_foreground(helper)
    time.sleep(0.3)
    ok = u.GetForegroundWindow() not in (None, 0, main)
    tap(vk)
    time.sleep(1.5)
    u.ShowWindow(main, 9)                                      # SW_RESTORE
    time.sleep(1.2)          # 恢复是异步的：不等等再取控件，句柄和矩形都不可靠
    if not ok:
        print("   !! 前台没切到帮助窗（%s），这次按键可能被程序按设计忽略" % tag)
    return ok


# ---------------- 主流程 ----------------
AUTO0 = ini_get("auto")
HOTKEY = ini_get("hotkey", "ui") or "Tab"
INPUT_OK = not input_dead()
print("初始：settings.ini [grab] auto=%s · 热键=%s · 输入桌面=%s" %
      (AUTO0, HOTKEY, "可用" if INPUT_OK else "不可用（热键/实机用例将跳过）"))

# 场景一（默认开启）：把 auto 键整个拿掉，验证程序自己默认开
ini_del("auto")

p = None
try:
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main(p)
    u.SendMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)     # 连接
    time.sleep(4.5)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0201, 1, (12 << 16) | 40)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0202, 0, (12 << 16) | 40)
    time.sleep(2.5)

    # 1) 默认开启：没写过 auto 键 → 程序自己开，且发放页被自动建出来
    #    （tick 一见 grab_auto 开着而发放页没建，就切过去建页收货）
    ga = ctl(main, G_GAUTO)
    g_add = ctl(main, G_ADD)
    g_del = ctl(main, G_DEL)
    g_list = ctl(main, G_LIST)
    print("发放页控件：连续取码=%s 加入队列=%s 移除=%s 队列=%s" %
          (bool(ga), bool(g_add), bool(g_del), bool(g_list)))
    ok_built = all([ga, g_add, g_del, g_list])
    ok_def = bool(ga) and checked(ga)
    print("1) 默认开启（无 auto 键）：自动建页=%s 复选框勾上=%s" %
          ("OK" if ok_built else "FAIL", "OK" if ok_def else "FAIL"))
    if not ok_built:
        sys.exit(1)

    g_id = ctl(main, G_ID)

    # 2) 复选框关：ini=0、勾选去掉、日志「连续取码：已关闭」
    l0 = log_lines()
    click(ga)
    time.sleep(1.2)
    off_ini = ini_get("auto") == "0"
    off_chk = not checked(ga)
    off_lg = any("连续取码：已关闭" in x for x in log_text().splitlines()[l0:])
    print("2) 复选框关：ini=%s 勾选=%s 日志=%s" % (off_ini, off_chk, off_lg))

    # 3) 复选框开：ini=1、勾选回来、日志「连续取码：已开启」
    l0 = log_lines()
    click(ga)
    time.sleep(1.2)
    on_ini = ini_get("auto") == "1"
    on_chk = checked(ga)
    on_lg = any("连续取码：已开启" in x for x in log_text().splitlines()[l0:])
    print("3) 复选框开：ini=%s 勾选=%s 日志=%s" % (on_ini, on_chk, on_lg))

    # 4) 布局：复选框夹在「加入队列」和「移除所选」之间
    rects = {}
    for name, h in (("加入", g_add), ("连续取码", ga), ("移除", g_del)):
        r = wt.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        rects[name] = r.left
    ok_order = rects["加入"] < rects["连续取码"] < rects["移除"]
    print("4) 按钮行顺序（加入 %d < 连续取码 %d < 移除 %d）: %s" %
          (rects["加入"], rects["连续取码"], rects["移除"], "OK" if ok_order else "FAIL"))

    # 5) 无命中时段：队列必须不动，日志不许暴涨（失败不写日志，同一件只记一条）
    clear_hover("用例 5：保证这 8 秒没有悬停")
    n0 = u.SendMessageW(g_list, 0x1004, 0, 0)                 # LVM_GETITEMCOUNT
    l0 = log_lines()
    slot = probe_slot()
    ok_quiet = None
    if slot:
        # 探到槽里有物品：人正在游戏里操作。这条用例的前提不成立，跳过而不是判错
        print("5) 8 秒无悬停：跳过（悬停槽里有 id=%d，游戏正在被人操作）" % slot)
    else:
        time.sleep(8.0)
        n1 = u.SendMessageW(g_list, 0x1004, 0, 0)
        dl = log_lines() - l0
        ok_quiet = (n1 == n0) and dl <= 2
        print("5) 8 秒无悬停：队列 %d→%d · 日志 +%d 行" % (n0, n1, dl))
        print("   OK: 不入队=%s 日志不暴涨=%s" % (
            "OK" if n1 == n0 else "FAIL", "OK" if dl <= 2 else "FAIL"))

    # 6) 实机悬停（--hover X,Y）：光标放回游戏物品 → 悬停即采（≤2.5s）
    #    采到就该整行入队（不是只填编号）；光标停着不动时同编号去重
    ok_live = ok_add = ok_dup = None
    if HOVER and INPUT_OK and game_clear_point():
        nb = u.SendMessageW(g_list, 0x1004, 0, 0)              # 入队前队列长度
        l1 = log_lines()
        u.SetCursorPos(HOVER[0], HOVER[1])                     # 光标放到游戏物品上
        m = None
        hit = []
        for _ in range(25):                                    # 最多等 2.5s（250ms 一轮）
            time.sleep(0.1)
            new_lines = log_text().splitlines()[l1:]
            hit = [x for x in new_lines if "取码 #" in x]
            if hit:
                m = re.search(r"取码 #(\d+) .*?· (builtin|scan) ·", hit[-1])
                break
        filled = bool(m) and wait_box(g_id, m.group(1))        # 编号框晚一两个 tick，等它
        txt = edit_text(g_id)
        ok_live = bool(m) and m.group(2) == "builtin" and filled
        print("6) 实机悬停 %s：日志 %s · 编号框 %r" % (
            HOVER, hit[-1] if hit else "（无命中）", txt))
        print("   OK: 悬停即采=%s 回填编号=%s" % (
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
        # 这件可能在本会话早先就入过队：那种情况这次"跳过"才是对的
        already = any(tag in x for x in all_lines[:l1])
        ok_add = bool(m) and ((na == nb + 1 and added_now) or
                              (na == nb and not added_now and already))
        print("7) 采到即入队：队列 %d→%d · 新增入队行 %d · 早先已有=%s" %
              (nb, na, len(add_lines), already))
        print("   OK: 入队=%s（%s）" % (
            "OK" if ok_add else "FAIL",
            "本次新增一行" if added_now else ("本来就已入队，跳过正确" if already else "既没入队也没记录")))

        # 8) 光标不动再悬停 2 秒：同编号去重，队列不增、不再写「入队」
        l2 = log_lines()
        time.sleep(2.0)
        nd = u.SendMessageW(g_list, 0x1004, 0, 0)
        dup_add = [x for x in log_text().splitlines()[l2:] if "入队 #" in x]
        ok_dup = nd == na and not dup_add
        print("8) 同编号去重：悬停保持 2s · 队列 %d→%d · 新增入队行 %d" %
              (na, nd, len(dup_add)))
        print("   OK: 去重=%s" % ("OK" if ok_dup else "FAIL"))
    elif HOVER:
        print("6) 实机悬停用例：跳过（输入桌面不可用或没找到游戏窗口）")
    else:
        print("6) 实机悬停用例：跳过（要跑加 --hover X,Y，X,Y 是游戏里某物品的屏幕坐标）")

    # 9/10) 热键来回切：要真实输入桌面
    ok_hot_off = ok_hot_on = None
    if INPUT_OK:
        clear_hover("热键用例前")
        vk = vk_of(HOTKEY)
        if not vk:
            print("!! 热键 %r 无法映射成虚拟键，跳过热键用例" % HOTKEY)
        else:
            # 热键是总开关：当前（用例 3 之后）是开着的，按一下=关
            l_hot = log_lines()
            hotkey_tap(main, vk, "用例 9")
            ok_hot_off = (ini_get("auto") == "0") and (not checked(ga)) and \
                any("连续取码：已关闭" in x for x in log_text().splitlines()[l_hot:])
            hotkey_tap(main, vk, "用例 10")
            ok_hot_on = (ini_get("auto") == "1") and checked(ga)
            print("9)  热键（关）: ini=%s 勾选=%s 日志=%s" %
                  (ini_get("auto") == "0", not checked(ga), ok_hot_off))
            print("10) 热键（开）: ini=%s 勾选=%s" % (ini_get("auto") == "1", checked(ga)))
    else:
        print("9/10) 热键用例：跳过（输入桌面不可用——RDP 断开/锁屏时合成按键送不进被测程序）")

    checks = [ok_built, ok_def,
              off_ini and off_chk and off_lg,
              on_ini and on_chk and on_lg,
              ok_order]
    # None = 环境不满足、用例跳过（不算通过也不算失败）
    for extra in (ok_quiet, ok_live, ok_add, ok_dup, ok_hot_off, ok_hot_on):
        if extra is not None:
            checks.append(extra)
    print("\n结果：%s（%d/%d）" % ("通过" if all(checks) else "失败",
                                 sum(1 for c in checks if c), len(checks)))
    print("待人工确认（游戏里连续悬停多件）：每件都入队带名字、同编号跳过、换物品再加行")
finally:
    if p:
        p.kill()
    # 复原：原来有键就写回原值；原来没这个键就还原成"没键"（走代码默认）
    cur = ini_get("auto")
    if AUTO0 is None:
        if cur is not None:
            ini_del("auto")
        print("已复原 [grab] auto=（无键，走默认）")
    elif cur != AUTO0:
        ini_set("auto", AUTO0)
        print("已复原 [grab] auto=%s" % AUTO0)
    else:
        print("[grab] auto 未变，无需复原")
