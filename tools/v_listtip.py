# 守卫：列表上不许再挂悬停提示。
#
# 鼠标停在列表行上会弹「编号悬浮卡」（类名 DnfIdTip）。以前列表自己还挂了一条
# 说明提示，整块悬停都弹，跟编号卡撞成一团；后来改成只在列头上弹，边界上还是会
# 撞。现在列表一律不挂提示，用法说明写成列表下方一行常驻小字。
#
# 脚本盯两头，因为只跑一头都不够：
#   · 源：扫 src/*.c，确认没有把 ui_tip() 挂到列表控件上 —— 覆盖所有页面，
#     包括要弹窗才打得开的那些，运行时脚本进不去。
#   · 运行时：悬停列表行，确认屏幕上真不出现系统 tooltip 窗口 —— 光看代码
#     看不出「机制还活着、只是没人调」这类残留。
#
# 系统 tooltip 的类名是 tooltips_class32，编号悬浮卡是自建的 DnfIdTip，两者
# 不会混；只要屏幕上出现了可见的 tooltips_class32，就说明有人给列表挂了提示。
import ctypes
import ctypes.wintypes as wt
import glob
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import u, start, check, rows, ctl, enabled, press

u.GetWindowRect.restype = ctypes.c_bool
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND

SYS_TIP = "tooltips_class32"
ID_CARD = "DnfIdTip"

IDC_TAB, IDC_CHLIST = 190, 113
PAGE_QUERY, PAGE_GRANT = 1, 2
Q_LIST, Q_ALL, Q_MAT = 1102, 1106, 1107
TCM_SETCURSEL = 0x130C


def P(h):
    return ctypes.c_void_p(h.value if hasattr(h, "value") else int(h))


def find_cls(hwnd, want):
    """hwnd 的直接子窗口里第一个可见且类名等于 want 的。

    EnumChildWindows 只枚举直接子级：页面容器 DnfPage 挂在**主窗口**下（只是
    被摆到页签内容区里），而左栏的角色列表同样是主窗口的直接子控件，所以
    必须先认 DnfPage 再往下找一层；图省事直接抓第一个列表会抓成左栏的。
    """
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(P(h), b, 64)
        if b.value == want and u.IsWindowVisible(P(h)):
            out.append(h)
            return False
        return True

    u.EnumChildWindows(P(hwnd), cb(each), 0)
    return out[0] if out else None


def top_cls(want):
    """顶层窗口里第一个可见且类名等于 want 的（系统 tooltip 是顶层 popup）。"""
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(P(h), b, 64)
        if b.value == want and u.IsWindowVisible(P(h)):
            out.append(h)
            return False
        return True

    u.EnumWindows(cb(each), 0)
    return out[0] if out else None


def cursor():
    p = wt.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return p.x, p.y


def park(x, y, secs):
    """把光标按在 (x,y) 上 secs 秒。

    SetCursorPos 只管到下一次真实鼠标输入为止，本机有人动鼠标或挂着远控就会被
    抢回去。所以高频重申，结束时回读一次：坐标没生效的算「跳过」，不算失败。
    """
    end = time.time() + secs
    while True:
        u.SetCursorPos(x, y)
        if time.time() >= end:
            break
        time.sleep(0.02)
    cx, cy = cursor()
    return abs(cx - x) <= 4 and abs(cy - y) <= 4


def row_xy(lv, row=0):
    r = wt.RECT()
    u.GetWindowRect(P(lv), ctypes.byref(r))
    y = r.top + 24                                     # 兜底：拿不到列头时的估计
    hd = u.SendMessageW(P(lv), 0x101F, 0, 0)           # LVM_GETHEADER
    if hd:
        hr = wt.RECT()
        if u.GetWindowRect(ctypes.c_void_p(hd), ctypes.byref(hr)):
            y = hr.bottom + 1
    return (r.left + r.right) // 2, y + 8 + row * 20


def src_guard():
    """静态：找出「既挂了 ui_tip 又 bind 了编号卡」的列表（正常应为空）。

    判定只看 bind 过的列表：冲突是「编号悬浮卡 vs 说明气泡」在同一个列表上
    抢位置，没绑编号卡的列表（比如泡点计划）悬停时压根不会弹卡片，说明气泡
    留在那儿不碍事，删掉反而是白丢信息。
    """
    bad = []
    for fn in sorted(glob.glob(os.path.join(ROOT, "src", "*.c"))):
        s = open(fn, encoding="utf-8").read()
        binds = set(re.findall(r"ui_idtip_bind(?:_ex)?\s*\(\s*([\w.]+)\s*,", s))
        if not binds:
            continue
        for _host, ctl in re.findall(r"ui_tip\s*\(\s*([^,]+?)\s*,\s*([\w.]+)\s*,", s):
            if ctl in binds:
                bad.append("%s → %s" % (os.path.basename(fn), ctl))
    return bad


if __name__ == "__main__":
    p, main = start(wait=5.0)
    ok = True
    try:
        tab = u.GetDlgItem(main, IDC_TAB)

        # 左栏角色列表连库就有行，不用额外操作
        targets = [("左栏·角色列表", u.GetDlgItem(main, IDC_CHLIST))]

        # 查询页：加载一批常用材料，列表才有数据
        u.SendMessageW(P(tab), TCM_SETCURSEL, PAGE_QUERY, 0)
        time.sleep(0.6)
        mat = ctl(main, Q_MAT)
        waited = 0
        while waited < 90 and not enabled(mat):
            time.sleep(1.0)
            waited += 1
            mat = ctl(main, Q_MAT)
        print("== 等 PVF 就绪 %ds" % waited)
        if not mat:
            print("!! PVF 没就绪，查询页列表无数据")
            ok = False
        else:
            press(mat, wait=1.5)
            targets.append(("查询页·结果列表", ctl(main, Q_LIST)))
            press(ctl(main, Q_ALL), wait=1.5)          # 全部加入 → 发放页队列有内容

        u.SendMessageW(P(tab), TCM_SETCURSEL, PAGE_GRANT, 0)
        time.sleep(0.6)
        glv = find_cls(find_cls(main, "DnfPage"), "SysListView32")
        if glv:
            targets.append(("发放页·队列列表", glv))

        for name, lv in targets:
            if not lv:
                continue
            n = rows(lv)
            if n <= 0:
                print("== %s 无数据行，跳过" % name)
                continue
            x, y = row_xy(lv, 0)
            steady = park(x, y, 1.2)
            if not steady:
                print("== %s 光标被抢，跳过" % name)
                continue
            t = top_cls(SYS_TIP)
            print("== %s 悬停第一行 → 说明气泡：%s" % (name, "有（不该有）" if t else "无"))
            ok &= check("%s 不弹说明气泡" % name, t is None, True)

        # 反向确认：编号悬浮卡还在（别把有价值的那个一起删了）
        if glv and rows(glv) > 0:
            x, y = row_xy(glv, 0)
            steady = park(x, y, 12.0)
            card = top_cls(ID_CARD)
            print("== 发放页悬停编号行 → 编号悬浮卡：%s%s" % (
                "出现" if card else "没出现", "" if steady else "（光标被抢）"))
            if steady:
                ok &= check("编号悬浮卡仍然能弹", card is not None, True)

        bad = src_guard()
        print("== 源码里挂在列表上的提示：%s" % ("、".join(bad) if bad else "无"))
        ok &= check("源码里没有给列表挂提示", not bad, True)

        print("\n结果：%s" % ("通过" if ok else "不通过"))
        sys.exit(0 if ok else 1)
    finally:
        p.kill()
