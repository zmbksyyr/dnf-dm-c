"""失焦保存 vs 即时刷新的冲突回归。

验三件事：
1) 物品页选中行/滚动位置能扛过一次 reload（写库完成、切页往返都会重填列表）
2) GM 页连改两个字段都不丢（乐观更新 + 队列串行）
3) GM 写库期间强制 tick 若干次，字段值不被旧值顶掉

页面上的控件挂在子窗口 "DnfPage" 下，必须从那里按控件 ID 找。
"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, grab, EXE, OUT, switch, NAMES, wait_main, PID as PAGE_ID, start
from pages import click_at as click, ctl, txt as gettext, set_text as settext

WM_TIMER = 0x0113
WM_KILLFOCUS = 0x0008
WM_SETTEXT = 0x000C
BM_CLICK = 0x00F5
LVM_GETNEXTITEM = 0x100C
LVM_GETTOPINDEX = 0x1027
LVM_GETITEMCOUNT = 0x1004
LVNI_SELECTED = 0x0002

PAGE_ITEM, PAGE_GM = 0, 8
I_LIST = PAGE_ID(PAGE_ITEM, 2)
I_INFO = PAGE_ID(PAGE_ITEM, 6)
G_MONEY = PAGE_ID(PAGE_GM, 5)
G_COIN = PAGE_ID(PAGE_GM, 6)


class LVITEM(ctypes.Structure):
    _fields_ = [("mask", ctypes.c_uint), ("iItem", ctypes.c_int), ("iSubItem", ctypes.c_int),
                ("state", ctypes.c_uint), ("stateMask", ctypes.c_uint),
                ("pszText", ctypes.c_void_p), ("cchTextMax", ctypes.c_int),
                ("iImage", ctypes.c_int), ("lParam", ctypes.c_void_p),
                ("iIndent", ctypes.c_int), ("iGroupId", ctypes.c_int), ("cColumns", ctypes.c_uint),
                ("puColumns", ctypes.c_void_p), ("piColFmt", ctypes.c_void_p),
                ("iGroup", ctypes.c_int)]


def lv_select(lv, i):
    it = LVITEM()
    it.mask = 0x0008               # LVIF_STATE（不置 mask，SETITEMSTATE 会当成空操作）
    it.state = 0x0002 | 0x0001     # LVIS_SELECTED | LVIS_FOCUSED
    it.stateMask = 0x0002 | 0x0001
    u.SendMessageW(lv, 0x100B, i, ctypes.addressof(it))   # LVM_SETITEMSTATE


p, main = start()
lv = u.GetDlgItem(main, 113)
click(lv, 40, 12)
time.sleep(3.0)

# ---------------- 1. 物品页选中行能不能扛住 reload
switch(main, PAGE_ITEM)
time.sleep(1.0)
il = ctl(main, I_LIST)
n = u.SendMessageW(il, LVM_GETITEMCOUNT, 0, 0)
print("items =", n)
if n < 4:
    print("物品太少，跳过选中测试")
else:
    # 点第 3 行（用鼠标点，比 SETITEMSTATE 更接近真实操作）
    r = ctypes.c_int(0)
    for y in (12, 34, 56):
        click(il, 40, y)
        time.sleep(0.15)
    sel = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
    print("selected row before =", sel)
    # 触发一次「无关」刷新：切到 GM 再切回来（两次 reload）
    switch(main, PAGE_GM)
    time.sleep(0.4)
    switch(main, PAGE_ITEM)
    time.sleep(0.8)
    sel2 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
    top = u.SendMessageW(il, LVM_GETTOPINDEX, 0, 0)
    print("selected row after  =", sel2, "top =", top)
    print("1) 选中保持:", "OK" if sel2 == sel else "FAIL")

# ---------------- 2/3. GM 连改两个字段，中间灌 tick
switch(main, PAGE_GM)
time.sleep(0.6)
money = ctl(main, G_MONEY)
coin = ctl(main, G_COIN)
print("gm controls:", money, coin)
old_money = gettext(money)
old_coin = gettext(coin)
print("before:", old_money, "/", old_coin)

settext(money, "54321")
u.SendMessageW(money, WM_KILLFOCUS, 0, 0)
# 写库还没落地时连灌几次 tick：修复前这里会拿 g.data 的旧值把框顶回去
for _ in range(6):
    u.PostMessageW(main, WM_TIMER, 1, 0)
    time.sleep(0.03)
mid = gettext(money)
print("money right after commit =", mid)

settext(coin, "7")
u.SendMessageW(coin, WM_KILLFOCUS, 0, 0)
for _ in range(6):
    u.PostMessageW(main, WM_TIMER, 1, 0)
    time.sleep(0.03)
time.sleep(2.5)

print("after:", gettext(money), "/", gettext(coin))
print("2) 两个字段都写进去了:", "OK" if gettext(money) == "54321" and gettext(coin) == "7" else "FAIL")
grab(main, os.path.join(OUT, "v13_stable.png"))

# 左栏角色卡里的金币应同步成 54,321
print("---- 恢复原值 ----")
settext(money, old_money or "0")
u.SendMessageW(money, WM_KILLFOCUS, 0, 0)
time.sleep(1.2)
settext(coin, old_coin or "0")
u.SendMessageW(coin, WM_KILLFOCUS, 0, 0)
time.sleep(2.0)
print("restored:", gettext(money), "/", gettext(coin))
p.kill()
