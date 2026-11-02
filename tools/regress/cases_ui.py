# -*- coding: utf-8 -*-
"""UI 回归用例：布局几何、Tab 顺序、逐页截图、日志页、密码控件、模态对话框生命周期。

每个用例只做自己那一段操作；表名与控件编号用固定常量，不写猜测值。
"""
import ctypes
import ctypes.wintypes as wt
import os
import time

from PIL import Image

from . import core
from .core import g, k32, u

# ---------------------------------------------------------------- 常量
# 主窗口控件编号
IDC_PWD, IDC_CONN, IDC_CHLIST = 103, 104, 113
IDC_ONLINE, IDC_GRP_CHARS, IDC_GRP_CARD = 114, 115, 116
IDC_TAB, IDC_STATUS = 190, 191
# 顶部那一行：PVF 输入框 / 选择 PVF 按钮 / 贴图包输入框 / ImagePacks2 按钮
LABEL = {106: "PVF 输入框", 107: "选择 PVF 按钮", 117: "贴图输入框", 118: "ImagePacks2 按钮"}

PAGE_ITEM, PAGE_QUERY, PAGE_GRANT, PAGE_GM, PAGE_LOG = 0, 1, 2, 8, 11

# GM 页的组框（编号表中的角色/资产/PVP/一键操作/账号/维护/泡点）
BOX = {1860: "角色", 1861: "资产", 1862: "PVP", 1863: "一键操作", 1864: "账号",
       1865: "维护", 1866: "泡点"}

# 物品页 / 查询页 / GM 页 / 日志页
I_LIST, I_EDIT, I_BAD = 1002, 1003, 1007
Q_LIST, Q_ALL, Q_MAT = 1102, 1106, 1107
G_SCANBAD = 1827
L_TEXT = core.PID(PAGE_LOG, 1)                 # 2101
S_RESCAN = 6003
SCAN_TITLE = "全服清错误物品"

WM_CLOSE, BM_CLICK, WM_GETFONT = 0x0010, 0x00F5, 0x0031
LVM_GETHEADER = 0x101F
CB_GETCOUNT, CB_GETLBTEXT, CB_SETCURSEL = 0x0146, 0x0148, 0x014E
GW_CHILD, GW_HWNDNEXT = 5, 2
GWL_STYLE, GWL_ID = -16, -12
WS_TABSTOP = 0x00010000
BS_TYPEMASK, BS_GROUPBOX = 0x0000000F, 0x00000007
CBS_DROPDOWN = 2
ES_PASSWORD = 0x0020
SRCCOPY = 0x00CC0020
SYS_TIP, ID_CARD = "tooltips_class32", "DnfIdTip"

# ---------------------------------------------------------------- Win32 声明
# 框架没声明的几个：句柄返回值必须按 64 位取，否则句柄一大就截断
u.GetWindow.restype = wt.HWND
u.GetWindow.argtypes = [wt.HWND, wt.UINT]
u.GetWindowLongW.restype = ctypes.c_long
u.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetDlgCtrlID.restype = ctypes.c_int
u.GetDlgCtrlID.argtypes = [wt.HWND]
u.IsWindow.argtypes = [wt.HWND]
u.IsWindowEnabled.argtypes = [wt.HWND]
u.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.FindWindowExW.restype = wt.HWND
u.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u.GetDC.restype = wt.HDC
u.GetDC.argtypes = [wt.HWND]
u.GetGuiResources.restype = wt.DWORD
u.GetGuiResources.argtypes = [wt.HANDLE, wt.DWORD]
u.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
g.BitBlt.restype = ctypes.c_int
g.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                     wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
g.GetDeviceCaps.argtypes = [wt.HDC, ctypes.c_int]
g.GetTextExtentPoint32W.argtypes = [wt.HDC, wt.LPCWSTR, ctypes.c_int, ctypes.POINTER(wt.SIZE)]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]


# ---------------------------------------------------------------- 通用小工具
def _rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def _inside(a, b, tol=1):
    """a 完全落在 b 里（留 tol 像素容差）。"""
    return (a.left >= b.left - tol and a.right <= b.right + tol and
            a.top >= b.top - tol and a.bottom <= b.bottom + tol)


def _dpi():
    dc = u.GetDC(0)
    d = g.GetDeviceCaps(dc, 88)                 # LOGPIXELSX
    u.ReleaseDC(0, dc)
    return d / 96.0


def _kids_z(h):
    """按 Z 序（= 创建顺序 = 对话框的 Tab 顺序）返回直接子窗口。"""
    out = []
    c = u.GetWindow(h, GW_CHILD)
    while c:
        out.append(c)
        c = u.GetWindow(c, GW_HWNDNEXT)
    return out


def _style(h):
    return u.GetWindowLongW(h, GWL_STYLE) & 0xFFFFFFFF


def _find_cls(parent, want):
    """parent 下第一个可见、类名等于 want 的控件。"""
    if not parent:
        return None
    for h in core.kids(parent):
        if core.cls_of(h) == want and u.IsWindowVisible(h):
            return h
    return None


def _page_hwnd(main):
    """当前可见的页面容器（懒创建的同类页窗口有多个，只能认可见的那个）。"""
    return _find_cls(main, "DnfPage")


def _enabled(h):
    return bool(h) and bool(u.IsWindowEnabled(h))


def _top_cls(want):
    """顶层窗口里第一个可见、类名等于 want 的（系统 tooltip / 编号卡都是顶层 popup）。"""
    out = []

    def each(h, lp):
        if core.cls_of(h) == want and u.IsWindowVisible(h):
            out.append(h)
            return False
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out[0] if out else None


def _first_row_xy(lv):
    """列表第一行的客户区坐标：表头下方 2px，必落在第 0 行。"""
    r = _rect(lv)
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    y = 26
    if hd:
        hr = wt.RECT()
        u.GetWindowRect(hd, ctypes.byref(hr))
        y = hr.bottom - r.top + 2
    return 40, max(2, y)


def _select_first_char(ctx):
    """选中左栏角色列表第一行（列表按编号降序，第一行就是编号最大的角色）。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not lv or core.rows(lv) <= 0:
        return False
    core.click_at(lv, *_first_row_xy(lv), wait=4.0)
    return True


def _img_of(h):
    """控件客户区的像素（BitBlt，要求控件当前真的画在屏幕上）。"""
    rc = wt.RECT()
    u.GetClientRect(h, ctypes.byref(rc))
    w, hh = rc.right, rc.bottom
    if w <= 0 or hh <= 0:
        return None
    dc = u.GetDC(h)
    mdc = g.CreateCompatibleDC(dc)
    bmp = g.CreateCompatibleBitmap(dc, w, hh)
    g.SelectObject(mdc, bmp)
    g.BitBlt(mdc, 0, 0, w, hh, dc, 0, 0, SRCCOPY)
    bi = core.BI(ctypes.sizeof(core.BI), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * hh * 4)
    g.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGB", (w, hh), buf, "raw", "BGRX", 0, 1).convert("RGB")
    g.DeleteObject(bmp)
    g.DeleteDC(mdc)
    u.ReleaseDC(h, dc)
    return img


def _ncol(img):
    """颜色数：1 表示整块纯色（该有字却没画出来）。"""
    cols = img.getcolors(maxcolors=1 << 20)
    return len(cols) if cols else (1 << 20)


# ---------------------------------------------------------------- 主窗口布局
@core.case("ui.align_pvf", needs=("gui",), group="always",
           desc="PVF 标签左边界与页签控件左边界的对齐关系")
def align_pvf(ctx):
    """验：PVF 标签落在页签左边界内缩一个页内边距（6px×缩放）的位置。"""
    main = ctx.main
    tab = u.GetDlgItem(main, IDC_TAB)
    pvf = None
    for h in core.kids(main):
        c = core.cls_of(h)
        if c == "SysTabControl32":
            tab = h
        elif c == "Static" and core.txt(h) == "PVF":
            pvf = h
    if not ctx.ok("找得到 PVF 标签", bool(pvf)):
        return
    ctx.ok("找得到页签控件", bool(tab))
    if not tab:
        return
    sc = _dpi()
    d = _rect(pvf).left - _rect(tab).left
    ctx.ok("PVF 标签左边界 = 页签左边界 + 6px×缩放",
           abs(d - 6 * sc) <= 3 * sc, "内缩 %d px，缩放 %.2f" % (d, sc))


def _gm_page(ctx):
    """切到 GM 页，返回可见页面容器。"""
    ctx.page(PAGE_GM)
    time.sleep(0.5)
    return _page_hwnd(ctx.main)


def _page_boxes(page):
    """本页的组框（按控件编号认）与其余可见控件。"""
    boxes, others = [], []
    for h in core.kids(page):
        r = _rect(h)
        wid = u.GetWindowLongW(h, GWL_ID) & 0xFFFF
        if wid in BOX:
            boxes.append((BOX[wid], r))
        elif r.right - r.left > 0 and r.bottom - r.top > 0:
            others.append((wid, r))
    return boxes, others


@core.case("ui.geom_bounds", needs=("gui", "db"), group="always",
           desc="GM 页控件不越出页面边界、也不互相重叠")
def geom_bounds(ctx):
    """验：GM 页所有可见控件都在页面矩形内，两两不重叠（重叠 > 2px 即算）。"""
    _select_first_char(ctx)
    page = _gm_page(ctx)
    if not ctx.ok("找得到可见的 GM 页面", bool(page)):
        return
    pr = _rect(page)
    boxes, others = _page_boxes(page)
    ctx.ok("GM 页的组框都建出来了", len(boxes) > 0, "组框 %d 个" % len(boxes))

    out = [(wid, (r.left, r.top, r.right, r.bottom)) for wid, r in others
           if r.left < pr.left or r.top < pr.top or r.right > pr.right or r.bottom > pr.bottom]
    ctx.ok("没有控件越出页面边界", not out, "越界 %d 个：%s" % (len(out), out[:4]))

    ov = []
    for i in range(len(others)):
        for j in range(i + 1, len(others)):
            a, b = others[i][1], others[j][1]
            ox = min(a.right, b.right) - max(a.left, b.left)
            oy = min(a.bottom, b.bottom) - max(a.top, b.top)
            if ox > 2 and oy > 2:
                ov.append((others[i][0], others[j][0], ox, oy))
    ctx.ok("没有控件互相重叠", not ov, "重叠 %d 对：%s" % (len(ov), ov[:4]))


@core.case("ui.geom_lanes", needs=("gui", "db"), group="always",
           desc="GM 页两栏组框的末组底边齐平")
def geom_lanes(ctx):
    """验：左右两栏最后一组组框的底边在同一水平线上（组框吃掉剩余高度）。"""
    _select_first_char(ctx)
    page = _gm_page(ctx)
    if not ctx.ok("找得到可见的 GM 页面", bool(page)):
        return
    pr = _rect(page)
    boxes, _others = _page_boxes(page)
    if not ctx.ok("GM 页有组框", len(boxes) > 0, "组框 %d 个" % len(boxes)):
        return
    lanes = {}
    for nm, r in boxes:
        lanes.setdefault(r.left, []).append(r)
    ends = []
    for left, rs in sorted(lanes.items()):
        last = max(rs, key=lambda r: r.top)
        ends.append(last.bottom)
        print("    栏 x=%4d 末组 bottom=%4d" % (left - pr.left, last.bottom - pr.top))
    if len(ends) < 2:
        ctx.skip("只找到一栏组框，无法比较两栏底部")
        return
    ctx.ok("两栏末组底边齐平（极差 ≤ 2px）", max(ends) - min(ends) <= 2,
           "极差 %d px" % (max(ends) - min(ends)))


@core.case("ui.left_layout", needs=("gui",), group="always",
           desc="左栏上下两张卡与页签、状态栏的位置关系")
def left_layout(ctx):
    """验：左栏在页签左侧、尺寸正常、顶边贴齐、底边贴齐，且不压状态栏。"""
    main = ctx.main
    tab = _rect(u.GetDlgItem(main, IDC_TAB))
    st = _rect(u.GetDlgItem(main, IDC_STATUS))
    gc = _rect(u.GetDlgItem(main, IDC_GRP_CHARS))    # 上卡：角色组框
    cd = _rect(u.GetDlgItem(main, IDC_GRP_CARD))     # 下卡：资料组框，贴底
    ctx.ok("上卡在页签左侧、不重叠", gc.right <= tab.left,
           "上卡 R=%d 页签 L=%d" % (gc.right, tab.left))
    ctx.ok("上卡尺寸正常（不是创建时的小方块）",
           gc.right - gc.left > 120 and gc.bottom - gc.top > 120,
           "%dx%d" % (gc.right - gc.left, gc.bottom - gc.top))
    ctx.ok("左栏顶边与页签顶边齐平", abs(gc.top - tab.top) <= 2,
           "上卡 T=%d 页签 T=%d" % (gc.top, tab.top))
    ctx.ok("下卡底边与页签底边齐平（两栏底部对齐）", abs(cd.bottom - tab.bottom) <= 2,
           "下卡 B=%d 页签 B=%d" % (cd.bottom, tab.bottom))
    ctx.ok("上下两卡不重叠", gc.bottom <= cd.top,
           "上卡 B=%d 下卡 T=%d" % (gc.bottom, cd.top))
    ctx.ok("左栏不压到状态栏", cd.bottom <= st.top,
           "下卡 B=%d 状态栏 T=%d" % (cd.bottom, st.top))


@core.case("ui.left_widgets", needs=("gui",), group="always",
           desc="角色列表可见且在上卡内，在线人数挂在上卡标题行")
def left_widgets(ctx):
    """验：左栏内容控件落在所属组框里，列表可见。"""
    main = ctx.main
    gc = _rect(u.GetDlgItem(main, IDC_GRP_CHARS))
    lv = u.GetDlgItem(main, IDC_CHLIST)
    lr = _rect(lv)
    on = _rect(u.GetDlgItem(main, IDC_ONLINE))
    ctx.ok("角色列表可见且在上卡内", bool(u.IsWindowVisible(lv)) and _inside(lr, gc),
           "列表 (%d,%d,%d,%d) 上卡 (%d,%d,%d,%d)"
           % (lr.left, lr.top, lr.right, lr.bottom, gc.left, gc.top, gc.right, gc.bottom))
    ctx.ok("在线人数挂在上卡标题行", _inside(on, gc))


def _text_w(h):
    """控件上那行字在当前字体下的像素宽：查字有没有被按钮宽度挤掉。"""
    s = core.txt(h)
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


@core.case("ui.top_row", needs=("gui",), group="always",
           desc="顶部 PVF/贴图包两组的等长与右端对齐")
def top_row(ctx):
    """验：两个输入框等长、两个按钮等长、整行右端与下方列表右端对齐、不越过窗口右边界。"""
    main = ctx.main
    r = {cid: _rect(u.GetDlgItem(main, cid)) for cid in LABEL}
    for cid in LABEL:
        b = r[cid]
        print("    %-14s L=%4d R=%4d W=%3d" % (LABEL[cid], b.left, b.right, b.right - b.left))
    ctx.check("两个输入框等长", r[106].right - r[106].left, r[117].right - r[117].left)
    ctx.check("两个按钮等长", r[107].right - r[107].left, r[118].right - r[118].left)

    page = _page_hwnd(main)
    lv = _find_cls(page, "SysListView32") if page else None
    if not lv:
        ctx.skip("没找到可见列表，跳过右端对齐检查")
    else:
        lr = _rect(lv)
        print("    下方列表      L=%4d R=%4d" % (lr.left, lr.right))
        ctx.ok("整行右端与下方列表右端对齐", abs(r[118].right - lr.right) <= 2,
               "行 R=%d 列表 R=%d" % (r[118].right, lr.right))
    ctx.ok("没有顶出主窗口右边界", r[118].right <= _rect(main).right,
           "按钮 R=%d 窗口 R=%d" % (r[118].right, _rect(main).right))


@core.case("ui.top_fit", needs=("gui",), group="always",
           desc="顶部两个按钮的宽度容得下各自文字")
def top_fit(ctx):
    """验：按钮宽度 ≥ 文字宽 + 4px（字号放大也不会把字截断）。"""
    main = ctx.main
    for cid in (107, 118):
        b = _rect(u.GetDlgItem(main, cid))
        w = b.right - b.left
        tw = _text_w(u.GetDlgItem(main, cid))
        ctx.ok("%s 容得下文字" % LABEL[cid], w >= tw + 4, "按钮 %d px / 文字 %d px" % (w, tw))


# ---------------------------------------------------------------- Tab 顺序
def _tab_report(ctx, page_idx):
    """验：同一张卡内 Tab 顺序与视觉顺序一致，且每张卡的控件在 Tab 序列里是连续一段。"""
    ctx.page(page_idx)
    time.sleep(0.6)
    page = None
    for h in _kids_z(ctx.main):
        if core.cls_of(h) == "DnfPage" and u.IsWindowVisible(h):
            page = h
            break
    if not ctx.ok("页 %d 找得到可见的页容器" % page_idx, bool(page)):
        return
    kids = _kids_z(page)

    groups = []                                  # 组框 = 自绘 BUTTON + BS_GROUPBOX
    for h in kids:
        if (core.cls_of(h).lower() == "button" and (_style(h) & BS_TYPEMASK) == BS_GROUPBOX
                and u.IsWindowVisible(h)):
            groups.append((core.txt(h), _rect(h)))

    items = []
    for h in kids:
        if not (_style(h) & WS_TABSTOP) or not u.IsWindowVisible(h):
            continue
        r = _rect(h)
        items.append({"cls": core.cls_of(h), "txt": core.txt(h), "x": r.left, "y": r.top})
    if not ctx.ok("页 %d 上有可 Tab 的控件" % page_idx, bool(items), "共 %d 个" % len(items)):
        return

    for it in items:                             # 归卡片：含控件中心且面积最小的组框
        cx, cy = it["x"] + 2, it["y"] + 2
        best, barea = None, None
        for gname, gr in groups:
            if gr.left <= cx <= gr.right and gr.top <= cy <= gr.bottom:
                a = (gr.right - gr.left) * (gr.bottom - gr.top)
                if barea is None or a < barea:
                    best, barea = gname, a
        it["grp"] = best or "（组框外）"

    seen, order = {}, []
    for i, it in enumerate(items):
        if it["grp"] not in seen:
            seen[it["grp"]] = []
            order.append(it["grp"])
        seen[it["grp"]].append((i, it))

    for gname in order:
        row = seen[gname]
        idx = [i for i, _ in row]
        contiguous = idx == list(range(idx[0], idx[0] + len(idx)))
        vis = sorted(row, key=lambda p: (round(p[1]["y"] / 12.0), p[1]["x"]))
        same = [i for i, _ in vis] == idx
        tag = gname if gname.strip() else "（无标题组框）"
        ctx.ok("%s：%d 个控件的 Tab 位连成一段" % (tag, len(row)), contiguous,
               "位次 %s" % ([i + 1 for i in idx],))
        ctx.ok("%s：Tab 顺序与卡内视觉顺序一致" % tag, same,
               "Tab 序 %s" % ([i + 1 for i, _ in row],))


@core.case("ui.tab_gm", needs=("gui",), group="always",
           desc="GM 页各卡片内的 Tab 顺序与视觉顺序一致、卡片不被打断")
def tab_gm(ctx):
    """验：GM 页（两栏）的 Tab 序列按卡片成段，段内先上后下、同行先左后右。"""
    _tab_report(ctx, PAGE_GM)


@core.case("ui.tab_grant", needs=("gui",), group="always",
           desc="发放页各卡片内的 Tab 顺序与视觉顺序一致、卡片不被打断")
def tab_grant(ctx):
    """验：发放页的 Tab 序列按卡片成段，段内先上后下、同行先左后右。"""
    _tab_report(ctx, PAGE_GRANT)


# ---------------------------------------------------------------- 逐页截图
@core.case("ui.screens", needs=("gui", "db"), group="always",
           desc="逐页切一次各截一张图：截图落盘、页面不是一块纯色")
def screens(ctx):
    """验：每个页签都能截出图，且图里不止一种颜色（页面真的画了东西）。"""
    if not _select_first_char(ctx):
        ctx.skip("角色列表没有行，截图里各页会是空态")
    if not core.pvf_ready(ctx.cur):
        ctx.skip("settings.ini 未配置 PVF：查询/发放页按钮是灰态，截图只当留档")
    for i, nm in enumerate(core.PAGES):
        ctx.page(i)
        time.sleep(0.3)
        path = ctx.shot("ui_p%d_%s" % (i, nm))
        if not ctx.ok("%s 截图落盘" % nm,
                      os.path.exists(path) and os.path.getsize(path) > 0, path):
            continue
        with Image.open(path) as im:
            n = _ncol(im.convert("RGB"))
        ctx.ok("%s 页面不是一块纯色" % nm, n > 1, "颜色数 %d" % n)


# ---------------------------------------------------------------- 日志页
def _log_file():
    """日志按 exe 目录落：<build>/config/logs/log-YYYYMMDD.txt。"""
    return os.path.join(core.BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))


def _lines(s):
    return [l for l in s.replace("\r\n", "\n").split("\n") if l.strip()]


def _read_log_page(ctx):
    """返回 (文件行, 页面行)；页面用大 cap 读，默认 256 字符会把内容截断。

    页面文本在切页那一刻就从盘上读进来了，所以紧接着读文件即可：中间的间隔越短，
    文件在两次读之间继续增长造成的差异越小。
    """
    ctx.page(PAGE_LOG)
    time.sleep(0.3)
    path = _log_file()
    if not ctx.ok("当天的日志文件存在", os.path.exists(path), path):
        return None, None
    raw = open(path, encoding="utf-8", errors="replace").read()
    flines = _lines(raw)
    box = _lines(core.txt(ctx.ctl(L_TEXT), 1 << 20))
    ctx.ok("日志文件有内容", bool(flines), "%d 行" % len(flines))
    ctx.ok("页面读到了日志内容", bool(box), "%d 行" % len(box))
    if not flines or not box:
        return None, None
    return flines, box


@core.case("ui.log_reverse", needs=("gui",), group="always",
           desc="日志页倒序：编辑框第一行就是文件最后一行")
def log_reverse(ctx):
    """验：最新的记录排在最上面。"""
    flines, box = _read_log_page(ctx)
    if flines is None:
        return
    ctx.check("页面第一行 == 文件最后一行", box[0][:40], flines[-1][:40])


@core.case("ui.log_cap", needs=("gui",), group="always",
           desc="日志页的显示上限：只截掉旧的，末尾仍与文件对齐")
def log_cap(ctx):
    """验：页面行数不超过文件行数，且页面最后一行能对上文件里的真实行尾。"""
    flines, box = _read_log_page(ctx)
    if flines is None:
        return
    ctx.check("倒序：页面第一行 == 文件最后一行", box[0][:40], flines[-1][:40])
    ctx.ok("页面行数不超过文件行数", len(box) <= len(flines),
           "页面 %d 行 / 文件 %d 行" % (len(box), len(flines)))
    # 截断只发生在最旧那一侧：页面最后一行要么是整行、要么是某个整行的行尾
    ctx.ok("页面最后一行能对上文件里的真实行", any(l.endswith(box[-1]) for l in flines),
           box[-1][:60])
    if len(box) >= len(flines):
        ctx.skip("日志未触及显示上限（文件 %d 行 / 页面 %d 行）" % (len(flines), len(box)))


# ---------------------------------------------------------------- 连库口令
def _combo_items(h):
    out = []
    n = int(u.SendMessageW(h, CB_GETCOUNT, 0, 0))
    for i in range(max(0, n)):
        b = ctypes.create_unicode_buffer(128)
        u.SendMessageW(h, CB_GETLBTEXT, i, ctypes.addressof(b))
        out.append(b.value)
    return out


def _cfg_pwd():
    """直接按行读 settings.ini：口令里可能有 %，走 configparser 会被插值吃掉。"""
    path = os.path.join(core.BUILD, "config", "settings.ini")
    if not os.path.exists(path):
        return None
    for line in open(path, encoding="utf-8-sig", errors="replace"):
        if line.strip().startswith("pwd") and "=" in line:
            return line.split("=", 1)[1].strip()
    return None


@core.case("ui.pwd", needs=("gui",), group="always",
           desc="口令控件：可编辑下拉、明文显示 ini 原值、可手填可选")
def pwd(ctx):
    """验：控件是可编辑下拉，预置两项，明文不掩码，显示 ini 原值，能自己敲也能选。"""
    h = u.GetDlgItem(ctx.main, IDC_PWD)
    if not ctx.ok("找得到口令下拉框", bool(h)):
        return
    ctx.check("控件类型", core.cls_of(h), "ComboBox")
    ctx.check("可编辑下拉样式", u.GetWindowLongW(h, GWL_STYLE) & 0xF, CBS_DROPDOWN)

    items = _combo_items(h)
    print("    下拉项 %r" % (items,))
    ctx.ok("下拉里含 uu5!^%jg", "uu5!^%jg" in items, items)
    ctx.ok("下拉里含 123456", "123456" in items, items)

    edit = u.GetDlgItem(h, 1001)
    masked = bool(u.GetWindowLongW(h, GWL_STYLE) & ES_PASSWORD)
    if edit:
        masked = masked or bool(u.GetWindowLongW(edit, GWL_STYLE) & ES_PASSWORD)
    ctx.check("未做掩码（明文显示）", masked, False)

    old = core.txt(h)
    want = _cfg_pwd()
    if want is None:
        ctx.skip("没有 settings.ini，跳过与 ini 原值的比对")
    else:
        ctx.check("显示 ini 里的口令原值", old, want)
    try:
        core.set_text(h, "hand-typed-pwd")
        time.sleep(0.2)
        ctx.check("可手填", core.txt(h), "hand-typed-pwd")
        if "123456" in items:
            u.SendMessageW(h, CB_SETCURSEL, items.index("123456"), 0)
            time.sleep(0.2)
            ctx.check("可从下拉选 123456", core.txt(h), "123456")
    finally:
        core.set_text(h, old)                    # 复原，别把测试值留在界面上


# ---------------------------------------------------------------- 全页签画质
def _scan_paint(ctx, tag):
    """验：当前页上面积够大的 Static/Button/ComboBox 不能是一整块纯色。

    空的 Edit / 空列表本来就是纯色（白纸），不算留白，所以按类名过滤。
    """
    page = _page_hwnd(ctx.main)
    if not ctx.ok("%s：找得到可见页面" % tag, bool(page)):
        return
    bad, checked = [], 0
    for h in core.kids(page):
        if not u.IsWindowVisible(h):
            continue
        c = core.cls_of(h)
        if c not in ("Static", "Button", "ComboBox"):
            continue
        rc = wt.RECT()
        u.GetClientRect(h, ctypes.byref(rc))
        if rc.right * rc.bottom <= 1500:
            continue
        img = _img_of(h)
        if img is None:
            continue
        checked += 1
        if _ncol(img) <= 1:
            bad.append((u.GetDlgCtrlID(h), c, rc.right, rc.bottom))
    ctx.ok("%s：没有该有内容却只剩一色的控件" % tag, not bad,
           "检查 %d 个，可疑 %d %s" % (checked, len(bad), bad[:4]))
    ctx.ok("%s：确实扫到了控件" % tag, checked > 0, "%d 个" % checked)


@core.case("ui.pageall_enter", needs=("gui", "db"), group="manual",
           desc="每个页签首次进入后，可见控件不能只剩一色（没画出来）")
def pageall_enter(ctx):
    """验：首次进入每个页时控件都真的画出了内容。"""
    _select_first_char(ctx)
    for i, nm in enumerate(core.PAGES):
        ctx.page(i)
        time.sleep(0.5)
        u.SetCursorPos(2, 2)                     # 光标挪开，别触发悬停高亮
        _scan_paint(ctx, "首次进入·%s" % nm)


@core.case("ui.pageall_back", needs=("gui", "db"), group="manual",
           desc="切走再切回后，可见控件不能只剩一色（重绘丢了）")
def pageall_back(ctx):
    """验：页面从后台切回时子控件会被重新画出来，不留白块。"""
    _select_first_char(ctx)
    n = len(core.PAGES)
    for i, nm in enumerate(core.PAGES):
        ctx.page((i + 1) % n)                    # 先切走
        time.sleep(0.4)
        ctx.page(i)                              # 再切回
        time.sleep(0.5)
        u.SetCursorPos(2, 2)
        _scan_paint(ctx, "切回·%s" % nm)


# ---------------------------------------------------------------- 列表悬停提示
def _park(x, y, secs):
    """把光标按在 (x,y) 上 secs 秒；结束时回读，坐标被抢走的不算数。"""
    end = time.time() + secs
    while True:
        u.SetCursorPos(x, y)
        if time.time() >= end:
            break
        time.sleep(0.02)
    p = wt.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return abs(p.x - x) <= 4 and abs(p.y - y) <= 4


def _row_screen_xy(lv, row=0):
    """列表第 row 行在屏幕上的坐标（SetCursorPos 用屏幕坐标）。"""
    r = _rect(lv)
    y = r.top + 24
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    if hd:
        hr = wt.RECT()
        u.GetWindowRect(hd, ctypes.byref(hr))
        y = hr.bottom + 1
    return (r.left + r.right) // 2, y + 8 + row * 20


def _prep_query(ctx):
    """查询页：等 PVF 就绪 → 加载常用材料 → 全部加入（发放队列才有内容）。"""
    ctx.page(PAGE_QUERY)
    time.sleep(0.6)
    mat = ctx.ctl(Q_MAT)
    ready = bool(mat) and core.wait_until(lambda: _enabled(ctx.ctl(Q_MAT)), 45.0)
    if not ctx.ok("查询页「常用材料」按钮可用（PVF 已就绪）", ready):
        return False
    core.press(ctx.ctl(Q_MAT), wait=1.5)
    core.press(ctx.ctl(Q_ALL), wait=1.5)         # 全部加入 → 发放页队列有内容
    return True


def _grant_list(ctx):
    ctx.page(PAGE_GRANT)
    time.sleep(0.6)
    return _find_cls(_page_hwnd(ctx.main), "SysListView32")


def _hover_quiet(ctx, name, lv):
    """悬停列表第一行，断言屏幕上没有冒出系统 tooltip。

    列表必须此刻可见（悬停只对最上层窗口生效），所以调用方要保证它所在的页在前台。
    """
    if not lv:
        ctx.skip("%s 找不到控件" % name)
        return
    if core.rows(lv) <= 0:
        ctx.skip("%s 无数据行" % name)
        return
    x, y = _row_screen_xy(lv)
    if not _park(x, y, 1.2):
        ctx.skip("%s 光标被抢" % name)
        return
    ctx.ok("%s 不弹说明气泡" % name, _top_cls(SYS_TIP) is None)


@core.case("ui.tip_no_bubble", needs=("gui", "db"), group="manual",
           desc="列表上不挂说明气泡（左栏角色列表、查询页结果、发放页队列）")
def tip_no_bubble(ctx):
    """验：鼠标停在列表行上，屏幕上不出现系统 tooltip。"""
    main = ctx.main
    _hover_quiet(ctx, "左栏·角色列表", u.GetDlgItem(main, IDC_CHLIST))
    if not _select_first_char(ctx) or not _prep_query(ctx):
        ctx.skip("查询页没有数据，只验左栏列表")
        return
    _hover_quiet(ctx, "查询页·结果列表", ctx.ctl(Q_LIST))
    _hover_quiet(ctx, "发放页·队列列表", _grant_list(ctx))


@core.case("ui.tip_idcard", needs=("gui", "db"), group="manual",
           desc="编号悬浮卡仍然能弹（别把有价值的那个一起删了）")
def tip_idcard(ctx):
    """验：悬停发放页队列行的编号列，浮出 DnfIdTip 卡片。"""
    if not _select_first_char(ctx):
        ctx.skip("角色列表没有行")
        return
    if not _prep_query(ctx):
        return
    lv = _grant_list(ctx)
    if not ctx.ok("发放页队列列表存在", bool(lv)):
        return
    n = core.rows(lv)
    if not ctx.ok("发放页队列有数据行", n > 0, "行数 %d" % n):
        return
    x, y = _row_screen_xy(lv)
    if not _park(x, y, 3.0):                     # 卡片延迟 400ms，留足 3 秒
        ctx.skip("光标被抢，跳过编号卡检查")
        return
    ctx.ok("编号悬浮卡仍然能弹", _top_cls(ID_CARD) is not None)


# ---------------------------------------------------------------- 模态对话框
def _pid_of(h):
    pid = wt.DWORD(0)
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def _enum_top(want=None, pid=None):
    """顶层可见窗口（可选按类名、按进程过滤）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        if pid is not None and _pid_of(h) != pid:
            return True
        c = core.cls_of(h)
        if want and c != want:
            return True
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        out.append((h, c, t.value))
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _dlg_by_title(ctx, title):
    """本进程的模态框（DnfDlg）里标题匹配的那个。"""
    for h, _c, t in _enum_top("DnfDlg", ctx.proc.pid):
        if t == title:
            return h
    return None


def _dlg_text(h):
    parts = []
    for c in core.kids(h):
        if core.cls_of(c) in ("Static", "Button"):
            t = core.txt(c, 2048)
            if t:
                parts.append(t)
    return " / ".join(parts)


def _msgbox(ctx):
    """本进程弹的提示/确认框（#32770），返回 (hwnd, 文字)。"""
    w = _enum_top("#32770", ctx.proc.pid)
    return (w[0][0], _dlg_text(w[0][0])) if w else (None, "")


def _wait_msgbox(ctx, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h, _t = _msgbox(ctx)
        if h:
            time.sleep(0.25)
            return h, _dlg_text(h)
        time.sleep(0.05)
    return None, ""


def _click_box(ctx, cid):
    h, _t = _msgbox(ctx)
    if not h:
        return False
    b = u.GetDlgItem(h, cid) or u.GetDlgItem(h, 1)   # 只有「确定」的提示框也能关掉
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 8:
        if not _enum_top("#32770", ctx.proc.pid):
            return True
        time.sleep(0.05)
    return False


def _wait_dlg(ctx, title, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = _dlg_by_title(ctx, title)
        if h:
            time.sleep(0.4)
            return h
        time.sleep(0.1)
    return None


def _wait_gone(ctx, title, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not _dlg_by_title(ctx, title):
            return True
        time.sleep(0.05)
    return False


def _close_all_dlgs(ctx):
    for h, _c, _t in _enum_top("DnfDlg", ctx.proc.pid):
        u.PostMessageW(h, WM_CLOSE, 0, 0)
        time.sleep(0.3)


def _settle(ctx):
    """把上一轮残留的提示框/对话框收干净，免得模态把下一轮挡掉。"""
    for _ in range(4):
        b, _t = _msgbox(ctx)
        if b:
            _click_box(ctx, 2)
            time.sleep(0.2)
            continue
        _close_all_dlgs(ctx)
        time.sleep(0.2)
        if not _enum_top("DnfDlg", ctx.proc.pid) and not _msgbox(ctx)[0]:
            return True
    return False


def _close_dlg(h, how):
    if how == "cancel":
        b = u.GetDlgItem(h, 2)                   # IDCANCEL
        if b:
            u.PostMessageW(b, BM_CLICK, 0, 0)
            return
    u.PostMessageW(h, WM_CLOSE, 0, 0)


def _gui_res(proc):
    """进程的 GDI/USER 句柄数：窗口没被 DestroyWindow 的话这里会一路涨。"""
    h = k32.OpenProcess(0x1000, False, proc.pid)     # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    gd = u.GetGuiResources(h, 0)                     # GR_GDIOBJECTS
    us = u.GetGuiResources(h, 1)                     # GR_USEROBJECTS
    k32.CloseHandle(h)
    return gd, us


def _open_edit(ctx):
    lv = ctx.ctl(I_LIST)
    if not lv:
        return None, "找不到物品列表"
    core.click_at(lv, *_first_row_xy(lv), wait=0.5)
    core.press(ctx.ctl(I_EDIT), wait=0.4)
    h = _wait_dlg(ctx, "编辑物品", 8.0)
    return (h, "") if h else (None, "窗口没出来")


def _open_bad(ctx):
    core.press(ctx.ctl(I_BAD), wait=0.4)
    h = _wait_dlg(ctx, "清除错误物品", 15.0)
    if h:
        return h, ""
    b, t = _msgbox(ctx)
    if b:
        _click_box(ctx, 2)
        return None, "没弹窗，只有提示：%s" % t[:60]
    return None, "窗口没出来"


def _run_cycles(ctx, name, opener, samples):
    """同一个对话框连开/关 5 轮，轮换「取消」与「WM_CLOSE」两种关法。"""
    good, whys = 0, []
    for i in range(5):
        _settle(ctx)
        h, why = opener()
        if not h:
            whys.append("第 %d 轮：%s" % (i + 1, why))
        else:
            _close_dlg(h, "cancel" if i % 2 == 0 else "close")
            if _wait_gone(ctx, name, 6.0):
                good += 1
            else:
                whys.append("第 %d 轮：关不掉" % (i + 1))
        _settle(ctx)
        samples.append((name, _gui_res(ctx.proc)))
        time.sleep(0.3)
    ctx.ok("%s：5 轮开/关全部成功" % name, good == 5,
           "%d/5 %s" % (good, "；".join(whys)))
    ctx.ok("%s：关掉后没有残留窗口" % name, not _dlg_by_title(ctx, name))
    ctx.ok("%s：关掉后主窗恢复可用" % name, _enabled(ctx.main))
    ctx.ok("%s：进程仍然活着" % name, ctx.proc.poll() is None)


def _check_handles(ctx, names, samples):
    """每组对话框 5 轮内的句柄极差 ≤ 2：真漏的话它随轮数线性涨，不是平的。"""
    for k, nm in ((0, "GDI"), (1, "USER")):
        for name in names:
            seq = [s[k] for t, s in samples if t == name and s]
            span = (max(seq) - min(seq)) if seq else 99
            ctx.ok("%s 连开 5 次 %s 句柄不涨" % (name, nm), span <= 2, "采样 %s" % (seq,))


@core.case("ui.dlg_cycle", needs=("gui", "db"), group="always",
           desc="编辑物品/清除错误物品各开关 5 轮：窗口真销毁、主窗恢复、句柄不涨")
def dlg_cycle(ctx):
    """验：两个模态窗能开、能取消、能关、关了能再开，且不泄漏 GDI/USER 句柄。"""
    main = ctx.main
    _select_first_char(ctx)
    ctx.page(PAGE_ITEM)
    time.sleep(0.8)
    lv = ctx.ctl(I_LIST)
    n = core.rows(lv)
    ctx.ok("物品页列表有行数", lv is not None, "行数 %d" % n)

    samples, names = [], []
    if n > 0:
        _run_cycles(ctx, "编辑物品", lambda: _open_edit(ctx), samples)
        names.append("编辑物品")
    else:
        ctx.skip("当前角色背包为空，跳过「编辑物品」对话框")
    # 「清除错误物品」只在真扫出错误时才开对话框；背包干净时程序给的是提示框
    h, why = _open_bad(ctx)
    if h:
        _close_dlg(h, "cancel")
        _wait_gone(ctx, "清除错误物品", 6.0)
        _run_cycles(ctx, "清除错误物品", lambda: _open_bad(ctx), samples)
        names.append("清除错误物品")
    else:
        ctx.skip("背包里没有错误物品，跳过「清除错误物品」对话框：%s" % why)
    _settle(ctx)
    _check_handles(ctx, names, samples)


def _open_scan(ctx):
    """点「全服清错误物品」→ 确认框点确定 → 等结果窗。"""
    if not ctx.ctl(G_SCANBAD):
        ctx.page(PAGE_GM)
        time.sleep(1.0)
    b = ctx.ctl(G_SCANBAD)
    if not b:
        return None, "找不到「全服清错误物品」入口"
    core.press(b, wait=0.4)
    h, t = _wait_msgbox(ctx, 8.0)
    if not h:
        return None, "入口没弹确认框"
    if "扫描全服所有角色" not in t:
        _click_box(ctx, 1)
        return None, "入口弹的是：%s" % t[:60]
    _click_box(ctx, 1)                           # 确认
    if not _wait_dlg(ctx, SCAN_TITLE, 60.0):
        b2, t2 = _msgbox(ctx)
        if b2:
            _click_box(ctx, 1)
            return None, "提示：%s" % t2[:60]
        return None, "等不到结果窗"
    return _dlg_by_title(ctx, SCAN_TITLE), ""


@core.case("ui.dlg_scan", needs=("gui", "db"), group="manual",
           desc="全服扫描结果窗开关 5 轮：窗口真销毁、主窗恢复、句柄不涨")
def dlg_scan(ctx):
    """验：扫描结果窗（带 WM_DESTROY 清理）能开、能关，关了之后不会锁死入口。"""
    main = ctx.main
    _select_first_char(ctx)
    ctx.page(PAGE_GM)
    time.sleep(0.8)
    samples, good, whys = [], 0, []
    for i in range(5):
        _settle(ctx)
        h, why = _open_scan(ctx)
        if not h:
            whys.append("第 %d 轮：%s" % (i + 1, why))
        else:
            _close_dlg(h, "cancel" if i % 2 == 0 else "close")
            if _wait_gone(ctx, SCAN_TITLE, 6.0):
                good += 1
            else:
                whys.append("第 %d 轮：关不掉" % (i + 1))
        _settle(ctx)
        samples.append(_gui_res(ctx.proc))
        time.sleep(0.3)
    ctx.ok("扫描结果窗：5 轮开/关全部成功", good == 5, "%d/5 %s" % (good, "；".join(whys)))
    ctx.ok("扫描结果窗：关掉后没有残留窗口", not _dlg_by_title(ctx, SCAN_TITLE))
    ctx.ok("扫描结果窗：关掉后主窗恢复可用", _enabled(main))
    ctx.ok("扫描结果窗：进程仍然活着", ctx.proc.poll() is None)
    for k, nm in ((0, "GDI"), (1, "USER")):
        seq = [s[k] for s in samples if s]
        span = (max(seq) - min(seq)) if len(seq) > 1 else 0
        ctx.ok("扫描结果窗连开 5 次 %s 句柄不涨" % nm, span <= 2, "采样 %s" % (seq,))


@core.case("ui.dlg_jobgate", needs=("gui", "db"), group="manual",
           desc="作业还在跑时关窗再点入口 10 轮：要么被拦、要么安全重开，进程不崩")
def dlg_jobgate(ctx):
    """验：扫描作业存活期间关窗+重进不会崩，闸门给出明确结论，锁表结束后库不再被占。"""
    main = ctx.main
    _select_first_char(ctx)
    ctx.page(PAGE_GM)
    time.sleep(0.8)
    blocked = restarted = slow = 0
    lk = core.db()
    lc = lk.cursor()
    try:
        for r in range(1, 11):
            # 1..5 轮走快路径；6..10 轮锁住 inventory，让扫描的 SELECT 卡住，
            # 强制走到「作业还在跑」那条闸门分支
            locked = r > 5
            _close_all_dlgs(ctx)
            h, why = None, ""
            t0 = time.time()
            while time.time() - t0 < 25:
                if _msgbox(ctx)[0]:
                    _click_box(ctx, 2)
                    time.sleep(0.5)
                h, why = _open_scan(ctx)
                if h:
                    break
                if "上一轮还没跑完" in why:
                    _click_box(ctx, 1)           # app_info 的「确定」
                    time.sleep(0.6)
                    continue
                time.sleep(0.4)
            if not h:
                ctx.skip("第 %d 轮等不到空闲：%s" % (r, why))
                continue

            if locked:
                lc.execute("LOCK TABLES taiwan_cain_2nd.inventory WRITE")
            # 重新扫描（同步）后立刻关窗
            b = u.GetDlgItem(h, S_RESCAN)
            if b:
                u.PostMessageW(b, BM_CLICK, 0, 0)
            u.PostMessageW(h, WM_CLOSE, 0, 0)
            if not _wait_gone(ctx, SCAN_TITLE, 3.0):
                ctx.skip("第 %d 轮关不掉" % r)
            time.sleep(0.02)
            if locked:
                time.sleep(0.3)                  # 让作业真的堵在那条 SELECT 上

            b = ctx.ctl(G_SCANBAD)               # 立刻再点入口
            if b:
                core.press(b, wait=0)
            h2, t = _wait_msgbox(ctx, 6.0)
            if h2 and "上一轮还没跑完" in t:
                blocked += 1
                _click_box(ctx, 1)
            elif h2 and "扫描全服所有角色" in t:
                _click_box(ctx, 1)               # 确定 → 走 scan_start
                h3, t3 = _wait_msgbox(ctx, 4.0)
                if h3 and "上一轮还没跑完" in t3:
                    blocked += 1
                    _click_box(ctx, 1)
                elif h3:
                    _click_box(ctx, 1)
                else:
                    restarted += 1               # 安全地重新开始扫描
            elif h2:
                _click_box(ctx, 1)
            else:
                slow += 1
            if locked:
                lc.execute("UNLOCK TABLES")
            if ctx.proc.poll() is not None:
                ctx.ok("第 %d 轮：进程不能崩" % r, False, "poll=%s" % (ctx.proc.poll(),))
                break
            time.sleep(0.2)

        ctx.ok("10 轮作业存活期间关窗+重进，进程没有崩", ctx.proc.poll() is None,
               "poll=%s" % (ctx.proc.poll(),))
        ctx.ok("每轮都有明确结论（被拦或重新开始）", blocked + restarted >= 8,
               "被拦 %d · 重新开始 %d · 无反应 %d" % (blocked, restarted, slow))
        ctx.ok("至少有一轮撞上「作业还在跑」并被闸门处理", blocked + restarted > 0,
               "被拦 %d · 重新开始 %d" % (blocked, restarted))
        ctx.ok("锁表那 5 轮都走到了「被拦」分支", blocked >= 5, "被拦 %d 次" % blocked)
        _close_all_dlgs(ctx)
        rest = _enum_top("DnfDlg", ctx.proc.pid)
        ctx.ok("收尾后没有残留对话框", not rest, str(rest)[:120])
        ctx.ok("收尾后进程仍活着", ctx.proc.poll() is None)
    finally:
        try:
            lc.execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            lk.close()
        except Exception:
            pass

@core.case("ui.single_instance", needs=("gui",), group="always",
           desc="再开一个实例会提示「已有实例在运行」，选「否」后退出")
def single_instance(ctx):
    """验：两个窗口同时写库的风险被拦住；命令行模式不受影响。"""
    import subprocess

    def dlg_for(pid):
        found = []

        def each(h, lp):
            if not u.IsWindowVisible(h):
                return True
            cls = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, cls, 64)
            if cls.value == "#32770":
                q = ctypes.wintypes.DWORD(0)
                u.GetWindowThreadProcessId(h, ctypes.byref(q))
                if q.value == pid:
                    found.append(h)
            return True
        u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)(each), 0)
        return found

    p2 = subprocess.Popen([core.EXE], cwd=os.path.dirname(core.EXE))
    try:
        d = []
        t0 = time.time()
        while time.time() - t0 < 10:
            d = dlg_for(p2.pid)
            if d:
                break
            time.sleep(0.2)
        if not ctx.ok("第二个实例弹出提醒", bool(d)):
            return
        core.press(u.GetDlgItem(d[0], 7), 0.5)      # 「否」
        try:
            rc = p2.wait(timeout=10)
        except Exception:
            rc = None
        ctx.ok("选「否」后第二个实例退出", rc == 0, "rc=%s" % (rc,))
        ctx.ok("第一个实例不受影响", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        try:
            p2.kill()
        except Exception:
            pass
