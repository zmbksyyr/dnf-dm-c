# -*- coding: utf-8 -*-
"""Tab 键顺序检查。

Win32 对话框的 Tab 导航顺序 = 子窗口的 Z 序 = 控件的创建顺序。这个脚本按创建顺序
枚举页面上带 WS_TABSTOP 的控件，然后断言两件事：

1. **同一张卡片内**，Tab 顺序和视觉顺序（先上后下、同行先左后右）一致；
2. **每张卡片的控件在 Tab 序列里是连续的一段**，不会跳出去再跳回来。

卡片之间的先后（先左栏还是先右栏）是取舍，不做断言 —— 两种都算合理。
只按 (y,x) 全页排序当基准是错的：GM 页是两栏，那样排会把两栏混成一条线。
"""
import ctypes, subprocess, time, os, sys
from ctypes import wintypes as wt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch

GW_CHILD, GW_HWNDNEXT = 5, 2
WS_TABSTOP = 0x00010000
GWL_STYLE = -16
BS_TYPEMASK = 0x0000000F
BS_GROUPBOX = 0x00000007

u.GetWindow.restype = wt.HWND
u.GetWindow.argtypes = [wt.HWND, wt.UINT]
u.GetWindowLongW.restype = ctypes.c_long
u.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u.IsWindowVisible.argtypes = [wt.HWND]


def kids_zorder(h):
    """按 Z 序（= 创建顺序）返回子窗口"""
    out = []
    c = u.GetWindow(h, GW_CHILD)
    while c:
        out.append(c)
        c = u.GetWindow(c, GW_HWNDNEXT)
    return out


def cls(h):
    b = ctypes.create_unicode_buffer(64)
    u.GetClassNameW(h, b, 64)
    return b.value


def text(h):
    b = ctypes.create_unicode_buffer(64)
    u.SendMessageW(h, 0x000D, 64, ctypes.addressof(b))
    return b.value


def rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def style(h):
    return u.GetWindowLongW(h, GWL_STYLE) & 0xFFFFFFFF


def inside(r, x, y):
    return r.left <= x <= r.right and r.top <= y <= r.bottom


p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main()
u.PostMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)   # 连接
time.sleep(4.0)
# 页号可传参：python v_tab.py 2 = 发放页；不传默认 GM 页
PAGE = int(sys.argv[1]) if len(sys.argv) > 1 else 8
switch(main, PAGE)                                      # GM 页
time.sleep(1.0)

page = None
for h in kids_zorder(main):
    if cls(h) == "DnfPage" and u.IsWindowVisible(h):
        page = h
        break
if not page:
    print("没找到可见的页")
    p.kill()
    sys.exit(1)

kids = kids_zorder(page)

# 组框（自绘的 BUTTON + BS_GROUPBOX），用来把控件归到卡片
groups = []
for h in kids:
    # GetClassNameW 给的是 "Button"（首字母大写），别拿 "BUTTON" 比
    if cls(h).lower() == "button" and (style(h) & BS_TYPEMASK) == BS_GROUPBOX and u.IsWindowVisible(h):
        groups.append((h, text(h), rect(h)))

items = []
for h in kids:
    st = style(h)
    if not (st & WS_TABSTOP) or not u.IsWindowVisible(h):
        continue
    r = rect(h)
    items.append({"h": h, "cls": cls(h), "txt": text(h), "x": r.left, "y": r.top})

print("可 Tab 控件 %d 个，按创建顺序：" % len(items))
for i, it in enumerate(items):
    print("  %2d. %-10s %-10s @(%4d,%4d)"
          % (i + 1, it["cls"], it["txt"][:10], it["x"], it["y"]))

# 归卡片：取「包含控件中心且面积最小」的那个组框
for it in items:
    cx, cy = it["x"] + 2, it["y"] + 2
    best, barea = None, None
    for gh, gname, gr in groups:
        if inside(gr, cx, cy):
            a = (gr.right - gr.left) * (gr.bottom - gr.top)
            if barea is None or a < barea:
                best, barea = gname, a
    it["grp"] = best or "（组框外）"

# 断言 1 + 2
bad = 0
print("\n按卡片分组：")
seen, order = {}, []
for i, it in enumerate(items):
    g = it["grp"]
    if g not in seen:
        seen[g] = []
        order.append(g)
    seen[g].append((i, it))

for g in order:
    row = seen[g]
    # 卡片是否连续
    idx = [i for i, _ in row]
    contiguous = idx == list(range(idx[0], idx[0] + len(idx)))
    # 卡片内视觉顺序
    vis = sorted(row, key=lambda p: (round(p[1]["y"] / 12.0), p[1]["x"]))
    same = [i for i, _ in vis] == idx
    tag = "OK" if (contiguous and same) else "!!"
    if not (contiguous and same):
        bad += 1
    print("  %s %-14s %d 个控件，Tab 位 %s" % (tag, g, len(row), "%d..%d" % (idx[0] + 1, idx[-1] + 1)))
    if not contiguous:
        print("      卡片被拆开：位次 %s" % ([i + 1 for i in idx],))
    if not same:
        for k, (i, it) in enumerate(row):
            if vis[k][0] != i:
                print("      位 %d Tab 到 %s「%s」@(%d,%d)，卡内视觉上这里应是 %s「%s」@(%d,%d)"
                      % (i + 1, it["cls"], it["txt"][:8], it["x"], it["y"],
                         vis[k][1]["cls"], vis[k][1]["txt"][:8], vis[k][1]["x"], vis[k][1]["y"]))
                break

print("\n卡片遍历顺序：" + " → ".join(order))
print("结果：%s" % ("全部一致" if bad == 0 else "%d 张卡片有问题" % bad))
p.kill()
