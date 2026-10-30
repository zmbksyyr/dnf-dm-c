import ctypes, ctypes.wintypes as wt, subprocess, time, sys, os
from PIL import Image

u = ctypes.windll.user32
g = ctypes.windll.gdi32
u.FindWindowW.restype = wt.HWND
u.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u.GetWindowDC.restype = wt.HDC
u.GetWindowDC.argtypes = [wt.HWND]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u.RedrawWindow.argtypes = [wt.HWND, ctypes.c_void_p, wt.HANDLE, wt.UINT]
u.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendMessageW.restype = wt.LPARAM
# 不设 argtypes 的话 HWND 会被当成 32 位 int 截断，句柄一大点就发丢了
u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.PostMessageW.restype = wt.BOOL
u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.IsWindowVisible.argtypes = [wt.HWND]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
# 不设这两个的 argtypes，HWND 会被当 32 位 int 传，句柄一大点就发丢
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


class BI(ctypes.Structure):
    _fields_ = [("s", wt.DWORD), ("w", wt.LONG), ("h", wt.LONG), ("p", wt.WORD), ("b", wt.WORD),
                ("c", wt.DWORD), ("si", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG),
                ("cu", wt.DWORD), ("ci", wt.DWORD)]


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", wt.HWND), ("idFrom", ctypes.c_size_t), ("code", wt.UINT)]


def grab(hwnd, out):
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
    Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1).save(out)
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(hwnd, hdc)


def switch(main, idx):
    tab = u.GetDlgItem(main, 190)
    u.SendMessageW(tab, 0x130C, idx, 0)  # TCM_SETCURSEL
    nh = NMHDR(tab, 190, 0xFFFFFDD9)   # TCN_SELCHANGE = -551
    u.SendMessageW(main, 0x004E, 190, ctypes.cast(ctypes.byref(nh), ctypes.c_void_p).value)
    time.sleep(1.1)


# ── 路径 ─────────────────────────────────────────────────────────────
# 全部从本文件位置推导，不写死盘符，clone 到任何目录都能跑：
#   <项目根>/tools/pages.py  →  ROOT  = <项目根>
#                               BUILD = <项目父目录>/_build/DNF_GM_C
# 产物刻意放在项目目录之外，仓库里不会出现 exe / obj / 截图 / config。
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.environ.get("DNF_GM_BUILD") or os.path.join(os.path.dirname(ROOT), "_build", "DNF_GM_C")
EXE = os.path.join(BUILD, "DNF_GM.exe")
OUT = os.path.join(BUILD, "shots")          # 截图落在产物旁边
os.makedirs(OUT, exist_ok=True)

NAMES = ["item", "query", "grant", "mail", "pet", "avatar", "quest", "event", "gm", "ban", "log"]


def db():
    """连测试库，参数取自 exe 同目录的 config/settings.ini（程序自己写的）。
    读不到就用默认值——改过库密码只改配置文件，脚本一处都不用动。
    注意用 RawConfigParser：密码里带 % 时普通 ConfigParser 会做插值而报错。"""
    import configparser
    import pymysql

    host, port, user, pwd = "127.0.0.1", 3306, "game", "uu5!^%jg"
    ini = os.path.join(BUILD, "config", "settings.ini")
    if os.path.exists(ini):
        cfg = configparser.RawConfigParser()
        cfg.read(ini, encoding="utf-8")
        if cfg.has_section("db"):
            host = cfg.get("db", "host", fallback=host)
            port = int(cfg.get("db", "port", fallback=port))
            user = cfg.get("db", "user", fallback=user)
            pwd = cfg.get("db", "pwd", fallback=pwd)
    return pymysql.connect(host=host, port=port, user=user, password=pwd,
                           charset="utf8mb4", autocommit=True)


def wait_main(proc=None, timeout=20.0):
    """等主窗口出现。FindWindow 拿到 0 时如果接着发 SendMessage，
    HWND=0 是广播，会等到所有顶层窗口都应答——启动稍慢就会卡死几分钟。
    另外桌面上可能同时开着另一个实例（用户自己跑的正式版），类名一样，
    FindWindow 按 Z 序取第一个——拿到别人的窗口，脚本操作的、最小化的、
    按热键的就全是人家的界面。所以传 proc 进来：只认自己启动的这个进程。"""
    t0 = time.time()
    want = proc.pid if proc else None
    while time.time() - t0 < timeout:
        found = []
        cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def each(h, lp):
            if not u.IsWindowVisible(h):
                return True
            cls = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, cls, 64)
            if cls.value != "DnfGmMain":
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

        u.EnumWindows(cb(each), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    raise RuntimeError("主窗口没出现，检查程序是否启动失败")


if __name__ == "__main__":
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main(p)
    u.SendMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)
    time.sleep(4.0)
    combo = u.GetDlgItem(main, 108)
    if u.SendMessageW(combo, 0x0146, 0, 0) > 0:
        u.SendMessageW(combo, 0x014E, 0, 0)
        u.SendMessageW(main, 0x0111, (0x0001 << 16) | 108, combo)
        time.sleep(3.0)

    for i, nm in enumerate(NAMES):
        switch(main, i)
        grab(main, os.path.join(OUT, "pg_%d_%s.png" % (i, nm)))
        print("shot", nm)
    p.kill()
