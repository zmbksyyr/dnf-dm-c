# 左栏（角色卡 + 资料卡）是主窗口的直接子控件，不在页签里。
# v_geom 只枚举页签内部，所以 do_layout 里漏调一次 left_layout()，
# 整栏会停在创建时的初始位置甚至看不见，而其余回归照样全绿 —— 出过一次。
# 这个脚本就盯这一件事：左栏在不在、位置对不对、有没有压到状态栏。
#
# 注意：主窗口大小被 WM_GETMINMAXINFO 锁死，别指望靠改窗口尺寸来触发重排，
# 首帧布局就是唯一一次布局。
import ctypes
import ctypes.wintypes as wt
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, start, check

u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

IDC_STATUS, IDC_TAB, IDC_CHLIST, IDC_ONLINE, IDC_GRP_CHARS, IDC_GRP_CARD = 191, 190, 113, 114, 115, 116


def rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def inside(a, b):
    """a 的矩形完全落在 b 里（留 1px 容差）"""
    return (a.left >= b.left - 1 and a.right <= b.right + 1 and
            a.top >= b.top - 1 and a.bottom <= b.bottom + 1)


if __name__ == "__main__":
    p, main = start(wait=4.0)
    try:
        tab = rect(u.GetDlgItem(main, IDC_TAB))
        st = rect(u.GetDlgItem(main, IDC_STATUS))
        g = rect(u.GetDlgItem(main, IDC_GRP_CHARS))     # 上卡：角色组框
        c = rect(u.GetDlgItem(main, IDC_GRP_CARD))      # 下卡：资料组框，贴底
        lv = rect(u.GetDlgItem(main, IDC_CHLIST))
        on = rect(u.GetDlgItem(main, IDC_ONLINE))

        ok = check("上卡在页签左侧、不重叠", g.right <= tab.left, True)
        ok &= check("上卡尺寸正常（不是创建时的小方块）",
                    (g.right - g.left > 120 and g.bottom - g.top > 120), True)
        ok &= check("左栏顶边与页签顶边齐平", abs(g.top - tab.top) <= 2, True)
        ok &= check("下卡底边与页签底边齐平（两栏底部对齐）", abs(c.bottom - tab.bottom) <= 2, True)
        ok &= check("上下两卡不重叠", g.bottom <= c.top, True)
        ok &= check("左栏不压到状态栏", c.bottom <= st.top, True)
        ok &= check("角色列表可见且在上卡内",
                    bool(u.IsWindowVisible(u.GetDlgItem(main, IDC_CHLIST))) and inside(lv, g), True)
        ok &= check("在线人数挂在上卡标题行", inside(on, g), True)
        print("   左栏 L=%d T=%d R=%d B=%d（下卡 B=%d，页签 B=%d，状态栏 T=%d）"
              % (g.left, g.top, g.right, g.bottom, c.bottom, tab.bottom, st.top))
        print("==", "通过" if ok else "失败")
        sys.exit(0 if ok else 1)
    finally:
        p.kill()
