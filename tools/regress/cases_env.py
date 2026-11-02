# -*- coding: utf-8 -*-
"""环境用例：编号悬浮卡与连续取码（真实鼠标、真实按键、前台窗口）。

这两块只有真实输入才会动：编号卡要光标真停在编号行上 400ms 才浮出，连续取码的
热键靠被测程序轮询 GetAsyncKeyState，开关靠真实鼠标点。所以整组只进 manual。

环境不满足一律 ctx.skip 后 return，不拿假的绿顶上去：没有 PVF、没有 ImagePacks2、
拿不到输入桌面（OpenInputDesktop 失败）、没有游戏窗口、没给悬停坐标
（环境变量 DNF_GM_HOVER=X,Y，游戏里某件物品的屏幕坐标）。
"""
import configparser
import ctypes
import ctypes.wintypes as wt
import glob
import os
import re
import subprocess
import time

from . import core
from .core import k32, u

# ---------------------------------------------------------------- 常量
PAGE_QUERY, PAGE_GRANT = 1, 2

Q_LIST = core.PID(PAGE_QUERY, 2)         # 查询页结果列表
Q_MAT = core.PID(PAGE_QUERY, 7)          # 查询页「加载常用材料」
G_ID = core.PID(PAGE_GRANT, 1)           # 发放页编号（编辑框）
G_ADD = core.PID(PAGE_GRANT, 11)         # 加入队列（按钮）
G_LIST = core.PID(PAGE_GRANT, 12)        # 队列列表
G_DEL = core.PID(PAGE_GRANT, 13)         # 移除所选（按钮）
G_GAUTO = core.PID(PAGE_GRANT, 20)       # 连续取码（复选框）

LVM_GETHEADER = 0x101F                   # 列表的列头控件（行区顶边从它底下算）
DESKTOP_READOBJECTS = 0x0001
SW_HIDE, SW_SHOW, SW_MINIMIZE, SW_RESTORE = 0, 5, 6, 9

GAME_CLS = "地下城与勇士"                 # 游戏主窗口类名
ID_CARD = "DnfIdTip"                     # 编号悬浮卡的窗口类名
HOVER_ENV = "DNF_GM_HOVER"               # 实机悬停点：游戏里某件物品的屏幕坐标 X,Y

# ---------------------------------------------------------------- Win32 声明
# 框架没声明的几个：句柄返回值不设宽度会被截成 32 位，回调签名也必须一致
u.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u.SetCursorPos.restype = wt.BOOL
u.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u.GetCursorPos.restype = wt.BOOL
u.IsWindow.argtypes = [wt.HWND]
u.IsWindow.restype = wt.BOOL
u.IsWindowEnabled.argtypes = [wt.HWND]
u.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u.GetForegroundWindow.restype = wt.HWND
u.SetForegroundWindow.argtypes = [wt.HWND]
u.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
u.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.c_size_t]
u.DefWindowProcW.restype = ctypes.c_longlong
u.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
u.CreateWindowExW.restype = wt.HWND
u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u.RegisterClassExW.argtypes = [ctypes.c_void_p]
u.OpenInputDesktop.restype = wt.HANDLE
u.OpenInputDesktop.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
u.CloseDesktop.argtypes = [wt.HANDLE]
k32.GetModuleHandleW.restype = wt.HMODULE
k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]


class _WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HANDLE), ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HANDLE), ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR), ("hIconSm", wt.HANDLE)]


HELPER = None                            # 前台替身窗（跨用例复用，进程内只建一次）
_HELPER_PROC = None


# ---------------------------------------------------------------- 通用小工具
def _rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def _enabled(h):
    return bool(h) and bool(u.IsWindowEnabled(h))


def _input_desktop():
    """输入桌面是否可用：RDP 断开/锁屏时进程不在输入桌面上，这里打不开。"""
    h = u.OpenInputDesktop(0, False, DESKTOP_READOBJECTS)
    if not h:
        return False
    u.CloseDesktop(h)
    return True


# ---------------------------------------------------------------- settings.ini
def _cfg_path():
    return os.path.join(core.BUILD, "config", "settings.ini")


def _ini_read():
    """读整份 settings.ini，返回 (文本, 写回用的编码)。

    程序用系统 Profile API 落盘：纯 ASCII 时是 ANSI，带中文的路径会让它写成带 BOM 的
    UTF-16。写回时用同一个编码，别把中文路径改成程序读不懂的字节。
    """
    path = _cfg_path()
    if not os.path.exists(path):
        return "", "utf-8"
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16"), "utf-16"
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace"), "utf-8"


def _ini_parse():
    text, enc = _ini_read()
    cp = configparser.RawConfigParser()      # Raw：口令里带 % 也不会被插值吃掉
    if text:
        cp.read_string(text)
    return cp, enc


def _ini_get(key, sec="grab"):
    cp, _enc = _ini_parse()
    return cp.get(sec, key, fallback=None)


def _ini_set(key, val, sec="grab"):
    cp, enc = _ini_parse()
    if not cp.has_section(sec):
        cp.add_section(sec)
    cp.set(sec, key, str(val))
    with open(_cfg_path(), "w", encoding=enc) as f:
        cp.write(f)


def _ini_del(key, sec="grab"):
    """删一个键（没有就不写，免得凭空造出一份空配置文件）。"""
    cp, enc = _ini_parse()
    if not cp.has_section(sec) or not cp.has_option(sec, key):
        return
    cp.remove_option(sec, key)
    with open(_cfg_path(), "w", encoding=enc) as f:
        cp.write(f)


def _restore_auto(orig):
    """复原 [grab] auto：原来有键写回原值，原来没这个键就还原成没键。"""
    cur = _ini_get("auto")
    if orig is None:
        if cur is not None:
            _ini_del("auto")
    elif cur != orig:
        _ini_set("auto", orig)


def _img_dir():
    """ImagePacks2 目录：[pvf] img 指了就用它，空的话按 PVF 旁边的同名目录猜。"""
    d = _ini_get("img", "pvf") or ""
    if d and os.path.isdir(d):
        return d
    pvf = _ini_get("path", "pvf") or ""
    if pvf:
        d = os.path.join(os.path.dirname(pvf), "ImagePacks2")
        if os.path.isdir(d):
            return d
    return None


# ---------------------------------------------------------------- 日志
def _log_text():
    """最近一份日志的全文：日志按天轮转，按文件名排序取最后一份。"""
    fs = sorted(glob.glob(os.path.join(core.BUILD, "config", "logs", "log-*.txt")))
    if not fs:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            with open(fs[-1], encoding=enc, errors="strict") as f:
                return f.read()
        except (UnicodeDecodeError, OSError):
            continue
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return f.read()


def _log_lines():
    return len(_log_text().splitlines())


def _log_stable(timeout=10.0):
    """等日志文件连续 1 秒不涨，再拿行数当基线：程序攒 1 秒才落盘。"""
    prev = _log_lines()
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(1.0)
        cur = _log_lines()
        if cur == prev:
            return cur
        prev = cur
    return prev


def _new_lines(base):
    return _log_text().splitlines()[base:]


# ---------------------------------------------------------------- 光标与编号卡
def _park(x, y, secs, stop=None):
    """把光标按在 (x,y) 上 secs 秒。

    SetCursorPos 只维持到下一次真实鼠标输入，所以高频重申；stop 返回真值就提前收工。
    返回 (稳不稳, 提前收工否)：稳 = 结束时回读坐标还在目标点附近。
    """
    end = time.time() + secs
    while True:
        u.SetCursorPos(x, y)
        if stop and stop():
            return True, True
        if time.time() >= end:
            break
        time.sleep(0.02)
    p = wt.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return abs(p.x - x) <= 4 and abs(p.y - y) <= 4, False


def _cursor():
    p = wt.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return p.x, p.y


def _tip():
    """此刻显示着的编号卡（顶层 WS_POPUP，类名 DnfIdTip）。"""
    out = []

    def each(h, lp):
        if core.cls_of(h) == ID_CARD and u.IsWindowVisible(h):
            out.append(h)
            return False
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out[0] if out else None


def _row_xy(lv, row=0):
    """列表第 row 行的屏幕坐标：行区顶边要从列头（SysHeader32）底边往下算，
    固定按 24px 猜会落进列头里，那里不弹编号卡。"""
    r = _rect(lv)
    y = r.top + 24
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    if hd:
        hr = wt.RECT()
        if u.GetWindowRect(hd, ctypes.byref(hr)):
            y = hr.bottom + 1
    return (r.left + r.right) // 2, y + 8 + row * 20


def _hover_row(ctx, lv, row, tag, secs=12.0):
    """悬停第 row 行等编号卡浮出；光标被真实鼠标抢走时跳过这一条。"""
    x, y = _row_xy(lv, row)
    steady, _early = _park(x, y, secs, stop=lambda: _tip() is not None)
    got = _tip()
    if got is None and not steady:
        ctx.skip("%s：光标被真实鼠标抢走，悬停没能成立" % tag)
        return False
    return ctx.ok("%s 悬停后浮出编号卡" % tag, got is not None)


def _prep_query(ctx):
    """查询页：等 PVF 就绪 → 加载常用材料，结果列表里才有编号行可悬停。"""
    ctx.page(PAGE_QUERY)
    time.sleep(0.6)
    if not core.wait_until(lambda: _enabled(ctx.ctl(Q_MAT)), 90.0):
        ctx.skip("PVF 未就绪：查询页「加载常用材料」一直是灰的")
        return False
    core.press(ctx.ctl(Q_MAT), wait=1.5)
    return True


@core.case("env.hover", needs=("gui", "db", "pvf", "desktop"), group="manual",
           desc="悬停查询页列表的编号行浮出编号卡，换行换卡、移开收起")
def hover(ctx):
    """验：编号卡只在光标停在编号行上时出现，换行先收起再重出，移开后收起。"""
    if not _input_desktop():
        ctx.skip("输入桌面不可用（RDP 断开/锁屏）：SetCursorPos 送不进真实桌面")
        return
    if not _img_dir():
        ctx.skip("没有 ImagePacks2 目录（settings.ini 的 [pvf] img 为空或不存在）")
        return
    if not _prep_query(ctx):
        return
    lv = ctx.ctl(Q_LIST)
    if not ctx.ok("找得到查询页结果列表", bool(lv)):
        return
    n = core.rows(lv)
    if not ctx.ok("查询页结果列表有数据行", n > 0, "行数 %d" % n):
        return

    _park(4, 4, 1.5)
    ctx.ok("光标不在列表上时不弹编号卡", _tip() is None)

    if not _hover_row(ctx, lv, 0, "第 1 行"):
        return
    if n < 4:
        ctx.skip("查询结果不足 4 行，换行换卡不验证")
        return

    # 换行：换格子那一刻程序先收起旧卡、重新开始 400ms 计时，所以新旧之间必有一段空窗。
    # 盯着「卡消失」这个瞬间，比在固定 0.2 秒后取一次快照稳（界面的 tick 可能被拖慢）
    x, y = _row_xy(lv, 3)
    _gone, hid = _park(x, y, 2.0, stop=lambda: _tip() is None)
    ctx.ok("换行时旧编号卡先收起", hid)

    if not _hover_row(ctx, lv, 3, "第 4 行"):
        return
    _park(4, 4, 1.2)
    ctx.ok("光标移开后编号卡收起", _tip() is None)


# ---------------------------------------------------------------- 连续取码
def _grant_page(ctx):
    """切到发放页（连续取码的开关、队列都在这一页），返回它的控件。"""
    ctx.page(PAGE_GRANT)
    time.sleep(0.6)
    return ctx.ctl(G_GAUTO), ctx.ctl(G_ADD), ctx.ctl(G_DEL), ctx.ctl(G_LIST)


def _flip(ctx, ga, on):
    """点一下复选框：勾选态、settings.ini、日志三处都要跟着翻。"""
    word = "开启" if on else "关闭"
    l0 = _log_lines()
    core.press(ga, wait=0.6)
    time.sleep(1.2)                       # 日志攒 1 秒才落盘
    ctx.check("%s：复选框勾选态" % word, core.checked(ga), on)
    ctx.check("%s：settings.ini [grab] auto" % word, _ini_get("auto"), "1" if on else "0")
    ctx.ok("%s：日志记下「连续取码：已%s」" % (word, word),
           any("连续取码：已%s" % word in x for x in _new_lines(l0)))


def _stop_gui(ctx):
    """停掉当前实例：settings.ini 只在启动时读一次，改完必须重启才生效。"""
    try:
        ctx.proc.kill()
    except Exception:
        pass
    core.kill_stale()


def _start_gui(ctx):
    ctx.proc, ctx.main = core.boot()


@core.case("env.grab_default", needs=("gui", "db"), group="manual",
           desc="没有 [grab] auto 键时连续取码默认开启，并自动切到发放页建页")
def grab_default(ctx):
    """验：删掉 auto 键重启后复选框自己勾上，发放页被自动建出来。"""
    orig = _ini_get("auto")
    try:
        _stop_gui(ctx)
        _ini_del("auto")
        if _ini_get("auto") is not None:
            ctx.skip("settings.ini 的 [grab] auto 删不掉，默认值无从验证")
            return
        _start_gui(ctx)
        got = [ctx.ctl(c) for c in (G_GAUTO, G_ADD, G_DEL, G_LIST)]
        if not ctx.ok("默认开启：发放页被自动建出来", all(got),
                      "连续取码=%s 加入=%s 移除=%s 队列=%s" % tuple(bool(h) for h in got)):
            return
        ctx.ok("默认开启：复选框已勾上", core.checked(got[0]))
    finally:
        _restore_auto(orig)


def _helper_window():
    """一个最小的顶层窗，当「别的程序」的前台替身：热键只在工具不在前台时才认。"""
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


def _tid(h):
    return u.GetWindowThreadProcessId(h, None)


def _force_foreground(h):
    """SetForegroundWindow 受前台锁限制：先把输入挂到当前前台线程上再调。"""
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


def _vk_of(name):
    """热键名 → 虚拟键码，认与程序同一套写法（F1~F9、Tab/Insert/Home/End/PageUp/PageDown、单字符）。"""
    if not name:
        return None
    if name[0] == "F" and len(name) > 1 and name[1].isdigit():
        return 0x70 + int(name[1:]) - 1
    m = {"Tab": 0x09, "Insert": 0x2D, "Home": 0x24, "End": 0x23,
         "PageUp": 0x21, "PageDown": 0x22}
    if name in m:
        return m[name]
    if len(name) == 1:
        c = name.upper()
        if c.isalnum():
            return ord(c)
    return None


def _tap(vk):
    """按住 200ms 再松：程序 100ms 一个 tick，点按太短会被整拍漏过去。"""
    u.keybd_event(vk, 0, 0, 0)
    time.sleep(0.2)
    u.keybd_event(vk, 0, 2, 0)
    time.sleep(0.1)


def _hotkey_tap(main, vk):
    """最小化工具窗 → 前台让给替身窗 → 按热键 → 恢复。返回前台切换是否成功。"""
    helper = _helper_window()
    if not helper:
        return False
    u.ShowWindow(helper, SW_SHOW)
    u.ShowWindow(main, SW_MINIMIZE)
    time.sleep(0.5)
    _force_foreground(helper)
    time.sleep(0.3)
    ok = u.GetForegroundWindow() not in (None, 0, main)
    _tap(vk)
    time.sleep(1.5)
    u.ShowWindow(main, SW_RESTORE)
    time.sleep(1.2)                       # 恢复是异步的：不等再取控件，句柄和矩形都不可靠
    u.ShowWindow(helper, SW_HIDE)
    return ok


@core.case("env.grab_switch", needs=("gui", "db", "desktop"), group="manual",
           desc="连续取码的两条开关链路：复选框与热键都把勾选态、ini、日志一起翻过去")
def grab_switch(ctx):
    """验：复选框与热键都能翻转开关，且勾选态、settings.ini、日志三处一致。"""
    orig = _ini_get("auto")
    try:
        ga, add, dele, lv = _grant_page(ctx)
        if not ctx.ok("发放页的连续取码相关控件都在", all([ga, add, dele, lv]),
                      "连续取码=%s 加入=%s 移除=%s 队列=%s"
                      % (bool(ga), bool(add), bool(dele), bool(lv))):
            return
        left = [_rect(h).left for h in (add, ga, dele)]
        ctx.ok("按钮行顺序：加入队列 < 连续取码 < 移除所选",
               left[0] < left[1] < left[2], "left=%s" % left)

        # 从启动时的状态出发，翻到相反状态再翻回来：两个方向都断言一遍
        was = core.checked(ga)
        _flip(ctx, ga, not was)
        _flip(ctx, ga, was)

        if not _input_desktop():
            ctx.skip("输入桌面不可用（RDP 断开/锁屏）：合成按键送不进被测程序")
            return
        if not core.checked(ga):
            _flip(ctx, ga, True)          # 热键这一组从「开着」出发
        vk = _vk_of(_ini_get("hotkey", "ui") or "Tab")
        if not vk:
            ctx.skip("settings.ini 的热键名映射不出虚拟键，热键不验证")
            return

        l0 = _log_lines()
        if not _hotkey_tap(ctx.main, vk):
            ctx.skip("替身窗或前台没就位，热键这一下没送进被测程序")
            return
        ctx.check("热键（关）：settings.ini [grab] auto", _ini_get("auto"), "0")
        ctx.check("热键（关）：复选框勾选态", core.checked(ga), False)
        ctx.ok("热键（关）：日志记下「连续取码：已关闭」",
               any("连续取码：已关闭" in x for x in _new_lines(l0)))

        if not _hotkey_tap(ctx.main, vk):
            ctx.skip("第二次热键的替身窗/前台没就位，热键（开）不验证")
            return
        ctx.check("热键（开）：settings.ini [grab] auto", _ini_get("auto"), "1")
        ctx.check("热键（开）：复选框勾选态", core.checked(ga), True)
    finally:
        _restore_auto(orig)


# ---------------------------------------------------------------- 实机取码
def _game_clear_point():
    """游戏客户区里一个没有物品的点（左下角场景/技能栏一带），屏幕坐标。

    悬停槽只在鼠标于游戏窗口内移动时才更新，所以清空得把光标放到游戏里的空处。
    """
    found = []

    def each(h, lp):
        if not u.IsWindowVisible(h) or core.cls_of(h) != GAME_CLS:
            return True
        found.append(h)
        return False

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    if not found:
        return None
    r = _rect(found[0])
    w, h = r.right - r.left, r.bottom - r.top
    return r.left + int(w * 0.15), r.top + int(h * 0.80)


def _clear_hover():
    """把光标放到游戏内的非物品点，让悬停槽失效。"""
    pt = _game_clear_point()
    if not pt:
        return False
    u.SetCursorPos(pt[0], pt[1])
    time.sleep(0.3)                       # 等游戏处理这次移动
    return True


def _probe_slot():
    """跑一次 --grab-probe 看悬停槽里现在有没有物品：0 = 空槽，None = 探针不可用。

    测试期间人可能就在游戏里操作，任何「这 8 秒一定没有悬停」的假设都不成立，
    所以静默窗口之前先探一下。
    """
    try:
        subprocess.run([core.EXE, "--grab-probe"], cwd=core.BUILD, timeout=60,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        path = os.path.join(core.BUILD, "config", "grab-probe.txt")
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.search(r"命中: id=(\d+)", f.read())
        return int(m.group(1)) if m else 0
    except (OSError, subprocess.SubprocessError):
        return None


def _hover_point():
    """实机悬停点：环境变量 DNF_GM_HOVER=X,Y（游戏里某件物品的屏幕坐标）。"""
    raw = os.environ.get(HOVER_ENV, "").strip()
    if not raw:
        return None
    try:
        x, y = (int(v) for v in raw.split(","))
    except ValueError:
        return None
    return x, y


def _wait_box(h, expected, timeout=1.5):
    """等编号框回填：取码结果先落到程序内部，再由 tick 写进编辑框，比日志晚一两个 tick。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if core.txt(h) == expected:
            return True
        time.sleep(0.05)
    return False


def _quiet_window(ctx, lv):
    """8 秒无悬停：队列长度不变、日志不暴涨（≤2 行）。悬停槽不空就不判断。"""
    slot = _probe_slot()
    if slot is None:
        ctx.skip("取码探针不可用，静默窗口不判断")
        return
    if slot:
        ctx.skip("悬停槽里有物品 id=%d（游戏正被人操作），静默窗口不判断" % slot)
        return
    n0 = core.rows(lv)
    l0 = _log_stable()
    time.sleep(8.0)
    n1 = core.rows(lv)
    dl = _log_lines() - l0
    ctx.check("8 秒无悬停：队列长度", n1, n0)
    ctx.ok("8 秒无悬停：日志不暴涨（≤2 行）", dl <= 2, "日志 +%d 行" % dl)


@core.case("env.grab_live", needs=("gui", "db", "pvf", "desktop"), group="manual",
           desc="实机取码：无悬停时队列不动；悬停游戏物品即整行入队，同一编号不重复入队")
def grab_live(ctx):
    """验：静默窗内不入队；悬停物品采到编号、回填编号框并整行入队，停住不重复入队。"""
    if not _input_desktop():
        ctx.skip("输入桌面不可用（RDP 断开/锁屏）：光标与按键送不进真实桌面")
        return
    if not _game_clear_point():
        ctx.skip("没找到游戏窗口（类名 %s）：既清不掉悬停槽也没处悬停" % GAME_CLS)
        return
    orig = _ini_get("auto")
    try:
        ga, _add, _del, lv = _grant_page(ctx)
        gid = ctx.ctl(G_ID)
        if not ctx.ok("发放页的连续取码相关控件都在", all([ga, gid, lv]),
                      "连续取码=%s 编号框=%s 队列=%s" % (bool(ga), bool(gid), bool(lv))):
            return
        if not core.checked(ga):
            core.press(ga, wait=0.6)
            if not core.wait_until(lambda: core.checked(ga), 5.0):
                ctx.skip("连续取码打不开，取码轮询不会跑")
                return

        _clear_hover()                     # 先把光标挪到游戏里的空处，悬停槽才是空的
        _quiet_window(ctx, lv)

        hover = _hover_point()
        if hover is None:
            ctx.skip("没给 %s=X,Y（游戏里某件物品的屏幕坐标），实机悬停不验证" % HOVER_ENV)
            return
        nb = core.rows(lv)
        l1 = _log_lines()
        u.SetCursorPos(hover[0], hover[1])
        hit = []
        for _ in range(25):                # 最多等 2.5 秒（程序 250ms 一轮）
            time.sleep(0.1)
            hit = [x for x in _new_lines(l1) if "取码 #" in x]
            if hit:
                break
        m = re.search(r"取码 #(\d+) .*?· (builtin|scan) ·", hit[-1]) if hit else None
        if m is None:
            cx, cy = _cursor()
            if abs(cx - hover[0]) > 4 or abs(cy - hover[1]) > 4:
                ctx.skip("光标被真实鼠标抢走，悬停没能成立")
                return
        if not ctx.ok("悬停游戏物品 2.5 秒内采到编号", m is not None,
                      hit[-1][:70] if hit else "日志里没有「取码 #」"):
            return
        ctx.check("取码走 builtin 特征码路径", m.group(2), "builtin")
        ctx.ok("编号回填到发放页编号框", _wait_box(gid, m.group(1)),
               "编号框 %r，期望 %s" % (core.txt(gid), m.group(1)))

        time.sleep(0.6)                    # 等 tick 把结果落进队列
        na = core.rows(lv)
        all_lines = _log_text().splitlines()
        added = [x for x in all_lines[l1:] if "入队 #" in x]
        tag = "入队 #" + m.group(1) + " "
        added_now = any(tag in x for x in added)
        already = any(tag in x for x in all_lines[:l1])   # 这件本会话早先入过队
        ctx.ok("采到即整行入队（队列 +1 且有入队日志；早先入过的就不重复）",
               (na == nb + 1 and added_now) or (na == nb and not added_now and already),
               "队列 %d→%d · 新增入队行 %d · 早先已有=%s"
               % (nb, na, len(added), already))

        l2 = _log_lines()
        time.sleep(2.0)                    # 光标不动再停 2 秒
        nd = core.rows(lv)
        dup = [x for x in _new_lines(l2) if "入队 #" in x]
        ctx.ok("同编号去重：停住 2 秒队列不增、不再写入队日志", nd == na and not dup,
               "队列 %d→%d · 新增入队行 %d" % (na, nd, len(dup)))
    finally:
        _restore_auto(orig)
