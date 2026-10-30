"""全页签扫测：每个页签「首次进入」和「切走再切回」后，逐个检查页面上
尺寸足够大的子控件是不是「只有一种颜色」（= 没画出来）。
空标签本身就是一色，所以用面积过滤：面积 > 1500 且只有 1 色才算可疑。
"""
import ctypes, ctypes.wintypes as wt, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, g, EXE, OUT
from PIL import Image

u.GetDlgItem.restype = wt.HWND
u.FindWindowExW.restype = wt.HWND
u.GetDlgCtrlID.restype = ctypes.c_int
u.SendMessageW.restype = wt.LPARAM
u.GetDC.restype = wt.HDC
u.GetDC.argtypes = [wt.HWND]
u.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                           ctypes.c_int, wt.UINT]
g.BitBlt.restype = ctypes.c_int
g.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                     wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
g.GetDIBits.restype = ctypes.c_int


class BI(ctypes.Structure):
    _fields_ = [("s", wt.DWORD), ("w", wt.LONG), ("h", wt.LONG), ("p", wt.WORD), ("b", wt.WORD),
                ("c", wt.DWORD), ("si", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG),
                ("cu", wt.DWORD), ("ci", wt.DWORD)]


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", wt.HWND), ("idFrom", ctypes.c_size_t), ("code", wt.UINT)]


NAMES = ["物品", "查询", "发放", "邮件", "宠物", "时装", "任务", "活动", "GM", "封停", "日志"]


def switch(m, idx):
    tab = u.GetDlgItem(m, 190)
    u.SendMessageW(tab, 0x130C, idx, 0)
    nh = NMHDR(tab, 190, 0xFFFFFDD9)
    u.SendMessageW(m, 0x004E, 190, ctypes.cast(ctypes.byref(nh), ctypes.c_void_p).value)


def page_of(m):
    c = u.FindWindowExW(m, None, "DnfPage", None)
    while c:
        if u.IsWindowVisible(c):
            return c
        c = u.FindWindowExW(m, c, "DnfPage", None)
    return 0


def px(h):
    rc = wt.RECT()
    u.GetClientRect(h, ctypes.byref(rc))
    w, hh = rc.right, rc.bottom
    if w <= 0 or hh <= 0:
        return None
    dc = u.GetDC(h)
    mdc = g.CreateCompatibleDC(dc)
    bmp = g.CreateCompatibleBitmap(dc, w, hh)
    g.SelectObject(mdc, bmp)
    g.BitBlt(mdc, 0, 0, w, hh, dc, 0, 0, 0x00CC0020)
    bi = BI(ctypes.sizeof(BI), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * hh * 4)
    g.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGB", (w, hh), buf, "raw", "BGRX", 0, 1).convert("RGB")
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(h, dc)
    return img


def scan(m, tag):
    page = page_of(m)
    bad = []
    checked = 0
    stack = [page]
    while stack:                          # 递归子控件（含组框里的）
        parent = stack.pop()
        c = u.FindWindowExW(parent, None, None, None)
        while c:
            if u.IsWindowVisible(c):
                rc = wt.RECT()
                u.GetClientRect(c, ctypes.byref(rc))
                area = (rc.right) * (rc.bottom)
                if area > 1500:
                    im = px(c)
                    if im:
                        checked += 1
                        cols = im.getcolors(maxcolors=200000) or []
                        b = ctypes.create_unicode_buffer(48)
                        u.GetClassNameW(c, b, 48)
                        # 空 Edit / 空列表本来就是纯色（白纸），不算留白；
                        # 要抓的是 Static/Button 这类"该有字却白成一块"的控件
                        if len(cols) <= 1 and b.value in ("Static", "Button", "ComboBox"):
                            bad.append((u.GetDlgCtrlID(c), b.value, rc.right, rc.bottom,
                                        im.getpixel((1, 1))))
                stack.append(c)
            c = u.FindWindowExW(parent, c, None, None)
    print("  %-14s 检查 %2d 个控件，可疑 %d %s"
          % (tag, checked, len(bad), "" if not bad else bad), flush=True)
    return len(bad)


subprocess.run(["taskkill", "/F", "/IM", "DNF_GM.exe"], capture_output=True)
time.sleep(0.5)
p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
time.sleep(2.5)
m = u.FindWindowW("DnfGmMain", None)
u.SetWindowPos(m, (-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040)
time.sleep(0.4)
u.SendMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)
time.sleep(4.0)
u.SetCursorPos(2, 2)
time.sleep(0.3)

total = 0
for i, name in enumerate(NAMES):
    switch(m, i)
    time.sleep(1.2)
    print("页签 %d %s" % (i, name), flush=True)
    total += scan(m, "首次进入")
    switch(m, (i + 1) % len(NAMES))          # 切走
    time.sleep(0.7)
    u.SetCursorPos(2, 2)
    switch(m, i)                            # 切回
    time.sleep(1.2)
    total += scan(m, "切回")

print("可疑控件合计 =", total, flush=True)
try:
    p.kill()
except Exception:
    pass
