# 枚举 GM 页所有子控件的屏幕矩形，检查三件事：
#   1) 组框是否等高、底部是否齐平；
#   2) 有没有控件互相重叠；
#   3) 有没有控件跑出页面边界。
# 比肉眼看截图靠谱，改布局后跑一次。
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, start

u.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]

BOX = {1860: "角色", 1861: "资产", 1862: "PVP", 1863: "一键操作", 1864: "账号", 1865: "维护",
       1866: "泡点"}


def kids(hwnd):
    out = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(h, lp):
        out.append(h)
        return True

    u.EnumChildWindows(hwnd, cb, 0)
    return out


if __name__ == "__main__":
    p, main = start(wait=4.0)
    combo = u.GetDlgItem(main, 108)
    if u.SendMessageW(combo, 0x0146, 0, 0) > 0:
        u.SendMessageW(combo, 0x014E, 0, 0)
        u.SendMessageW(main, 0x0111, (0x0001 << 16) | 108, combo)
        time.sleep(3.0)
    switch(main, 8)
    time.sleep(1.0)

    # 页面窗口 = 主窗口后代里 class 为 DnfPage 的那个（找不到就取 id 段落在 1800+ 的父窗口）
    # 只认"当前可见"的那一页：同类名的页窗口有多个（懒创建），
    # 取第一个会把物品页当 GM 页，越界/重叠全成误报
    page = None
    for h in kids(main):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "DnfPage" and u.IsWindowVisible(h):
            page = h
            break
    if not page:
        print("找不到页面窗口")
        p.kill()
        sys.exit(1)
    pr = wt.RECT()
    u.GetWindowRect(page, ctypes.byref(pr))
    print("page rect", pr.left, pr.top, pr.right, pr.bottom,
          "size", pr.right - pr.left, pr.bottom - pr.top)

    boxes, others = [], []
    allkids = kids(page)          # 只看本页的控件，主窗口的工具栏/左栏不算
    print("page kids:", len(allkids), "main kids:", len(kids(main)))
    for h in allkids:
        r = wt.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        wid = u.GetWindowLongW(h, -12) & 0xFFFF     # GWL_ID
        if wid in BOX:
            boxes.append((BOX[wid], r))
        elif r.right - r.left > 0 and r.bottom - r.top > 0:
            others.append((wid, r))

    print("\n-- 组框（相对页面） --")
    for nm, r in sorted(boxes, key=lambda t: (t[1].left, t[1].top)):
        print("  %-6s x=%4d y=%4d  w=%4d h=%4d  bottom=%4d" %
              (nm, r.left - pr.left, r.top - pr.top,
               r.right - r.left, r.bottom - r.top, r.bottom - pr.top))
    if boxes:
        # 比的是"两栏各自最后一组"的底边，不是全部组框的最大最小差：
        # 一栏三组，组框本身高度不同，max-min 永远很大，说明不了整齐与否
        lanes = {}
        for nm, r in boxes:
            lanes.setdefault(r.left, []).append(r)
        ends = []
        for l, rs in sorted(lanes.items()):
            last = max(rs, key=lambda r: r.top)
            ends.append(last.bottom)
            print("  栏 x=%4d 末组 bottom=%4d" % (l - pr.left, last.bottom - pr.top))
        if len(ends) > 1:
            print("  两栏底部极差 = %d px" % (max(ends) - min(ends)))

    print("\n-- 越界 / 重叠 --")
    bad = 0
    for wid, r in others:
        if r.left < pr.left or r.top < pr.top or r.right > pr.right or r.bottom > pr.bottom:
            print("  越界 id=%d rect=%s" % (wid, (r.left, r.top, r.right, r.bottom)))
            bad += 1
    for i in range(len(others)):
        for j in range(i + 1, len(others)):
            a, b = others[i][1], others[j][1]
            ox = min(a.right, b.right) - max(a.left, b.left)
            oy = min(a.bottom, b.bottom) - max(a.top, b.top)
            if ox > 2 and oy > 2:
                print("  重叠 id=%d/%d overlap=%dx%d" % (others[i][0], others[j][0], ox, oy))
                bad += 1
    print("  合计问题：%d" % bad)
    p.kill()
