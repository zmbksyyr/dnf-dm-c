# -*- coding: utf-8 -*-
"""回归框架：驱动真实窗口、连测试库、注册并执行用例。

用例是一个函数，签名为 `f(ctx)`，用 ctx.check() 断言；任何未捕获异常或失败的
断言都会让该用例判失败。用例通过 @case 注册，入口见 tools/regress.py。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import Image

u = ctypes.windll.user32
g = ctypes.windll.gdi32
k32 = ctypes.windll.kernel32

# 跨进程发消息时 HWND/WPARAM 必须显式声明宽度，默认会被截成 32 位
u.FindWindowW.restype = wt.HWND
u.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u.GetWindowDC.restype = wt.HDC
u.GetWindowDC.argtypes = [wt.HWND]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u.RedrawWindow.argtypes = [wt.HWND, ctypes.c_void_p, wt.HANDLE, wt.UINT]
u.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendMessageW.restype = wt.LPARAM
u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.PostMessageW.restype = wt.BOOL
u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.IsWindowVisible.argtypes = [wt.HWND]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p),
                          ctypes.c_void_p]
u.EnumChildWindows.argtypes = [wt.HWND,
                               ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p),
                               ctypes.c_void_p]
g.CreateCompatibleDC.restype = wt.HDC
g.CreateCompatibleDC.argtypes = [wt.HDC]
g.CreateCompatibleBitmap.restype = wt.HBITMAP
g.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g.SelectObject.restype = wt.HGDIOBJ
g.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]
g.DeleteObject.argtypes = [wt.HGDIOBJ]
g.DeleteDC.argtypes = [wt.HDC]
u.ReleaseDC.argtypes = [wt.HWND, wt.HDC]

WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH = 0x000C, 0x000D, 0x000E
WM_KILLFOCUS, WM_CLOSE, WM_COMMAND = 0x0008, 0x0010, 0x0111
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
BM_CLICK, BM_GETCHECK, BM_SETCHECK = 0x00F5, 0x00F0, 0x00F1
LVM_GETITEMCOUNT = 0x1004
TCM_SETCURSEL, TCN_SELCHANGE = 0x130C, 0xFFFFFDD9
CB_GETCOUNT, CB_SETCURSEL, CB_GETCURSEL = 0x0146, 0x014E, 0x0147

IDC_CONN, IDC_CHLIST, IDC_TAB = 104, 113, 190

PAGES = ["item", "query", "grant", "mail", "pet", "avatar", "quest", "event", "gm", "bubble",
         "ban", "log"]


def PID(page, n):
    """页面内控件编号：每个页 100 个号段。"""
    return 1000 + page * 100 + n


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.environ.get("DNF_GM_BUILD") or os.path.join(os.path.dirname(ROOT), "_build", "DNF_GM_C")
EXE = os.path.join(BUILD, "DNF_GM.exe")
SHOTS = os.path.join(BUILD, "shots")


class BI(ctypes.Structure):
    _fields_ = [("s", wt.DWORD), ("w", wt.LONG), ("h", wt.LONG), ("p", wt.WORD), ("b", wt.WORD),
                ("c", wt.DWORD), ("si", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG),
                ("cu", wt.DWORD), ("ci", wt.DWORD)]


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", wt.HWND), ("idFrom", ctypes.c_size_t), ("code", wt.UINT)]


# ---------------------------------------------------------------- 窗口
def grab(hwnd, out):
    """整窗截图（PrintWindow，含子控件）。"""
    u.RedrawWindow(hwnd, None, None, 0x0001 | 0x0100 | 0x0080 | 0x0004)
    time.sleep(0.3)
    rc = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    hdc = u.GetWindowDC(hwnd)
    mdc = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mdc, bmp)
    u.PrintWindow(hwnd, mdc, 2)
    bi = BI(ctypes.sizeof(BI), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1).save(out)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(hwnd, hdc)


def cls_of(h):
    b = ctypes.create_unicode_buffer(64)
    u.GetClassNameW(h, b, 64)
    return b.value


def ctl(main, cid):
    """在可见的 DnfPage 下按编号找控件。"""
    found = []

    def each(h, lp):
        if cls_of(h) == "DnfPage" and u.IsWindowVisible(h):
            w = u.GetDlgItem(h, cid)
            if w:
                found.append(w)
                return False
        return True

    u.EnumChildWindows(main, ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                                ctypes.c_void_p)(each), 0)
    return found[0] if found else None


def kids(h):
    out = []

    def each(w, lp):
        out.append(w)
        return True

    u.EnumChildWindows(h, ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                             ctypes.c_void_p)(each), 0)
    return out


def txt(h, cap=4096):
    """读控件文本。跨进程只有 WM_GETTEXT 会被系统编组，GetWindowText 对别的进程返回空。"""
    if not h:
        return ""
    b = ctypes.create_unicode_buffer(cap)
    u.SendMessageW(h, WM_GETTEXT, cap, ctypes.addressof(b))
    return b.value


def set_text(h, s):
    b = ctypes.create_unicode_buffer(str(s))
    u.SendMessageW(h, WM_SETTEXT, 0, ctypes.addressof(b))


def blur(wait=1.5):
    """失焦即提交：界面在 EN_KILLFOCUS 上写库。"""
    return wait


def press(h, wait=0.6):
    """BM_CLICK 必须异步：模态处理不返回会把脚本卡死。"""
    u.PostMessageW(h, BM_CLICK, 0, 0)
    if wait:
        time.sleep(wait)


def click_at(h, x=20, y=12, wait=0.4):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    u.PostMessageW(h, WM_LBUTTONDOWN, 1, lp)
    u.PostMessageW(h, WM_LBUTTONUP, 0, lp)
    if wait:
        time.sleep(wait)


def rows(h):
    """列表行数。只数不取文本：跨进程 LVM_GETITEMTEXT 会让对方进程写野指针。"""
    return u.SendMessageW(h, LVM_GETITEMCOUNT, 0, 0) if h else 0


def checked(h):
    """0x00F0 是 BM_GETCHECK；0x00F2（BM_GETSTATE）带焦点位，不能用来判勾选。"""
    return u.SendMessageW(h, BM_GETCHECK, 0, 0) == 1


def switch(main, idx, wait=1.1):
    tab = u.GetDlgItem(main, IDC_TAB)
    u.SendMessageW(tab, TCM_SETCURSEL, idx, 0)
    nh = NMHDR(tab, IDC_TAB, TCN_SELCHANGE)
    u.SendMessageW(main, WM_COMMAND, IDC_TAB,
                   ctypes.cast(ctypes.byref(nh), ctypes.c_void_p).value)
    time.sleep(wait)


def wait_main(proc=None, timeout=20.0):
    """等本进程的主窗口。按类名 + PID 认，避免抓到用户自己开着的另一个实例。"""
    t0 = time.time()
    want = proc.pid if proc else None
    while time.time() - t0 < timeout:
        found = []

        def each(h, lp):
            if not u.IsWindowVisible(h) or cls_of(h) != "DnfGmMain":
                return True
            if want is None:
                found.append(h)
                return False
            wpid = wt.DWORD(0)
            u.GetWindowThreadProcessId(h, ctypes.byref(wpid))
            if wpid.value == want:
                found.append(h)
                return False
            return True

        u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                         ctypes.c_void_p)(each), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    raise RuntimeError("主窗口没出现：程序启动失败？")


def wait_until(fn, timeout=30.0, step=0.5):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if fn():
            return True
        time.sleep(step)
    return False


def kill_stale():
    """跑 GUI 之前清场：残留实例会占住 exe 和数据库。"""
    subprocess.run(["taskkill", "/F", "/IM", "DNF_GM.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.6)


# ---------------------------------------------------------------- 数据库
def db():
    """连测试库。参数取产物目录下程序自己写的 settings.ini，读不到就用默认值。"""
    import configparser
    import pymysql

    host, port, user, pwd = "127.0.0.1", 3306, "game", "uu5!^%jg"
    ini = os.path.join(BUILD, "config", "settings.ini")
    if os.path.exists(ini):
        cfg = configparser.RawConfigParser()   # 密码含 % 时普通插值会报错
        cfg.read(ini, encoding="utf-8")
        if cfg.has_section("db"):
            host = cfg.get("db", "host", fallback=host)
            port = int(cfg.get("db", "port", fallback=port))
            user = cfg.get("db", "user", fallback=user)
            pwd = cfg.get("db", "pwd", fallback=pwd)
    return pymysql.connect(host=host, port=port, user=user, password=pwd,
                           charset="utf8mb4", autocommit=True)


# ---------------------------------------------------------------- 用例框架
CASES = []


def case(name, needs=(), group="always", desc=""):
    """注册用例。needs 取 "gui" / "db" / "pvf" / "desktop"，由入口检查。"""
    def deco(fn):
        CASES.append({"name": name, "fn": fn, "needs": tuple(needs),
                      "group": group, "desc": desc or (fn.__doc__ or "").strip().split("\n")[0]})
        return fn
    return deco


class Ctx:
    """单个用例的上下文：进程、主窗口、游标、失败计数。"""

    def __init__(self, cur, proc, main):
        self.cur = cur
        self.proc = proc
        self.main = main
        self.fails = []
        self.skips = []

    # --- 断言
    def check(self, tag, got, want):
        ok = got == want
        print("%s %s（得到 %r，期望 %r）" % ("OK  " if ok else "!!  ", tag, got, want))
        if not ok:
            self.fails.append(tag)
        return ok

    def ok(self, tag, cond, detail=""):
        print("%s %s%s" % ("OK  " if cond else "!!  ", tag, (" | " + str(detail)) if detail else ""))
        if not cond:
            self.fails.append(tag)
        return bool(cond)

    def skip(self, why):
        self.skips.append(why)

    # --- 界面
    def ctl(self, cid):
        return ctl(self.main, cid)

    def page(self, idx):
        switch(self.main, idx, wait=0.8)

    def shot(self, name):
        path = os.path.join(SHOTS, name + ".png")
        grab(self.main, path)
        return path

    # --- 数据
    def char_no(self):
        """当前选中的角色：列表按 charac_no 降序，第一行就是最大的那个。"""
        self.cur.execute("SELECT MAX(charac_no) FROM taiwan_cain.charac_info WHERE delete_flag=0")
        return self.cur.fetchone()[0]

    def q(self, sql, args=None):
        self.cur.execute(sql, args)
        return self.cur.fetchall()

    def q1(self, sql, args=None):
        r = self.q(sql, args)
        return r[0][0] if r else None


def boot(connect=True, page=None, wait=4.5):
    """启动程序并返回 (proc, main)。调用方负责 kill。"""
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main(p)
    if connect:
        u.PostMessageW(u.GetDlgItem(main, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(wait)
    if page is not None:
        switch(main, page)
    return p, main


CASE_TIMEOUT = [420]        # 秒；由 --timeout 覆盖。用例卡死时由看门狗杀进程


def run_case(c, cur, needs_ok):
    """执行一个用例，返回 (状态, 说明)。"""
    missing = [n for n in c["needs"] if not needs_ok(n)]
    if missing:
        return "skip", "缺少前置：" + ", ".join(missing)
    kill_stale()
    p = m = None
    ctx = Ctx(cur, None, None)
    fired = [False]

    def watchdog():
        fired[0] = True
        if p:
            try:
                p.kill()
            except Exception:
                pass

    import threading
    timer = threading.Timer(CASE_TIMEOUT[0], watchdog)
    timer.daemon = True
    timer.start()
    try:
        p, m = boot()
        ctx.proc, ctx.main = p, m
        c["fn"](ctx)
    except Exception as e:                       # 用例本身崩了也算失败
        import traceback
        traceback.print_exc()
        ctx.fails.append("异常：%s" % e)
    finally:
        timer.cancel()
        if p:
            try:
                p.kill()
            except Exception:
                pass
        kill_stale()
    if fired[0]:
        return "fail", "超时 %ds，已终止" % CASE_TIMEOUT[0]
    if ctx.fails:
        return "fail", "失败 %d 项：%s" % (len(ctx.fails), ", ".join(ctx.fails[:6]))
    return "pass", ""


def main(argv=None):
    import argparse

    ap = argparse.ArgumentParser(prog="regress.py", description="DNF GM 回归套件")
    ap.add_argument("--list", action="store_true", help="列出全部用例")
    ap.add_argument("--group", default="always", choices=["always", "manual", "all"])
    ap.add_argument("--only", action="append", default=[], help="按名字前缀过滤，可重复")
    ap.add_argument("--timeout", type=int, default=420)
    a = ap.parse_args(argv)
    CASE_TIMEOUT[0] = a.timeout

    names = None
    if a.only:
        names = set()
        for c in CASES:
            for pat in a.only:
                if c["name"].startswith(pat):
                    names.add(c["name"])

    picked = [c for c in CASES
              if (names is None or c["name"] in names)
              and (a.group == "all" or c["group"] == a.group or (a.group == "always" and c["group"] == "always"))]

    if a.list:
        for c in CASES:
            print("%-26s %-7s %-6s %s" % (c["name"], c["group"], ",".join(c["needs"]), c["desc"]))
        print("\n共 %d 个用例" % len(CASES))
        return 0

    if not picked:
        print("没有匹配的用例")
        return 1
    if not os.path.exists(EXE):
        print("找不到被测程序：%s\n先跑 tools\\build.bat" % EXE)
        return 1

    state = {"db": None, "pvf": None}

    def needs_ok(n):
        if n == "gui":
            return True
        if n == "db":
            return state["db"] is not None
        if n == "pvf":
            return state["pvf"] is not None
        return True

    try:
        try:
            state["db"] = db()
        except Exception as e:
            print("!! 连不上测试库：%s" % e)
        if state["db"]:
            state["pvf"] = pvf_ready(state["db"].cursor())

        cur = state["db"].cursor() if state["db"] else None
        print("regress: %d 个用例\n" % len(picked))
        results = []
        for c in picked:
            t0 = time.time()
            status, why = run_case(c, cur, needs_ok)
            results.append((c, status, why, time.time() - t0))
            print("%-26s %-5s %6.1fs %s" % (c["name"], status, results[-1][3], why))
    finally:
        if state["db"]:
            state["db"].close()
        kill_stale()

    print("\n" + "=" * 78)
    bad = [r for r in results if r[1] == "fail"]
    for c, st, why, _ in results:
        print("%-26s %-5s %s" % (c["name"], st, why))
    print("-" * 78)
    print("合计 %d：pass %d · fail %d · skip %d" % (
        len(results), sum(1 for r in results if r[1] == "pass"), len(bad),
        sum(1 for r in results if r[1] == "skip")))
    return 1 if bad else 0


def pvf_ready(cur):
    """PVF 是否已配置：settings.ini 里的路径存在即视为就绪。"""
    try:
        p = cur.execute("SELECT 1")
        del p
    except Exception:
        return None
    import configparser
    ini = os.path.join(BUILD, "config", "settings.ini")
    if not os.path.exists(ini):
        return None
    cfg = configparser.RawConfigParser()
    cfg.read(ini, encoding="utf-8")
    pvf = cfg.get("pvf", "path", fallback="") if cfg.has_section("pvf") else ""
    return pvf if pvf and os.path.exists(pvf) else None
