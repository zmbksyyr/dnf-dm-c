# 顶部那一行（PVF 目录 / ImagePacks2 目录）的排版规则：
#   两个输入框等长、两个按钮等长、整行右端与下方列表的右端对齐。
# 之前 6:4 分宽度且手工预算间隔时漏算两个 gap，最后一个按钮右端 1244
# 超出客户区 1240，右边被裁掉一截。这里直接拿「页签里可见列表的右端」
# 当基准来比，不复用布局里的算式，免得跟着一起错。
import ctypes
import ctypes.wintypes as wt
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, g, start, check, txt

u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetDC.restype = wt.HDC
u.GetDC.argtypes = [wt.HWND]
g.SelectObject.restype = wt.HGDIOBJ
g.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g.GetTextExtentPoint32W.argtypes = [wt.HDC, wt.LPCWSTR, ctypes.c_int,
                                    ctypes.POINTER(wt.SIZE)]
WM_GETFONT = 0x0031

IDC_TAB = 190
IDC_PVFB, IDC_PVFS, IDC_IMGB, IDC_IMGS = 106, 107, 117, 118
LABEL = {106: "PVF 输入框", 107: "选择 PVF 按钮", 117: "贴图输入框", 118: "ImagePacks2 按钮"}


def rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def find_cls(hwnd, want):
    """hwnd 的直接子窗口里第一个可见且类名等于 want 的。

    EnumChildWindows 只枚举直接子级，所以页面里的列表得先下探到 DnfPage，
    再在里面找一层。
    """
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, b, 64)
        if b.value == want and u.IsWindowVisible(h):
            out.append(h)
            return False
        return True

    u.EnumChildWindows(hwnd, cb(each), 0)
    return out[0] if out else None


def text_w(h):
    """按钮上那行字在当前字体下的像素宽，用来查字有没有被挤掉。"""
    s = txt(h)
    if not s:
        return 0
    hdc = u.GetDC(h)
    f = u.SendMessageW(h, WM_GETFONT, 0, 0)
    old = g.SelectObject(hdc, f)
    sz = wt.SIZE()
    g.GetTextExtentPoint32W(hdc, ctypes.c_wchar_p(s), len(s), ctypes.byref(sz))
    g.SelectObject(hdc, old)
    u.ReleaseDC(h, hdc)
    return sz.cx


if __name__ == "__main__":
    p, main = start(wait=4.0)
    try:
        r = {cid: rect(u.GetDlgItem(main, cid)) for cid in LABEL}
        for cid in LABEL:
            b = r[cid]
            print("   %-14s L=%4d R=%4d W=%3d" % (LABEL[cid], b.left, b.right,
                                                  b.right - b.left))

        ok = check("两个输入框等长",
                   r[106].right - r[106].left == r[117].right - r[117].left, True)
        ok &= check("两个按钮等长",
                    r[107].right - r[107].left == r[118].right - r[118].left, True)

        # 页面容器（DnfPage）是主窗口的直接子窗口，只是被摆到页签内容区里；
        # 而左栏的角色列表同样是主窗口的直接子控件，所以必须显式认 DnfPage，
        # 不能图省事直接抓第一个列表
        page = find_cls(main, "DnfPage")
        lv = find_cls(page, "SysListView32") if page else None
        if not lv:
            print("!! 没找到可见列表，跳过右对齐检查")
        else:
            lr = rect(lv)
            print("   下方列表      L=%4d R=%4d" % (lr.left, lr.right))
            ok &= check("整行右端与下方列表右端对齐", abs(r[118].right - lr.right) <= 2, True)

        mr = rect(main)
        ok &= check("没有顶出客户区右边界", r[118].right <= mr.right, True)
        for cid in (107, 118):
            ok &= check("%s 容得下文字" % LABEL[cid],
                        (r[cid].right - r[cid].left) >= text_w(u.GetDlgItem(main, cid)) + 4,
                        True)
        print("==", "通过" if ok else "失败")
        sys.exit(0 if ok else 1)
    finally:
        p.kill()
