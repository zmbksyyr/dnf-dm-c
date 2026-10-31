# -*- coding: utf-8 -*-
"""GM 页数值框：框里填「设成多少」，交出去就回 0；右边 + 按钮在当前值上加。

跑法：
  DNF_GM_BUILD=<exe 所在目录> python v_qadd.py

会改真实库里的金币/复活币/等级，跑完按原值写回。
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, OUT, wait_main, switch, grab, db as open_db

PID = lambda page, n: 1000 + page * 100 + n
G_LEV, G_MONEY, G_COIN = PID(8, 2), PID(8, 5), PID(8, 6)
G_QADD = PID(8, 70)                     # 1870 起，顺序见源码 kQAdd
IDC_CONN, IDC_CHLIST = 104, 113

u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]


def ctl(main, cid):
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    f = []

    def each(h, lp):
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value == "DnfPage" and u.IsWindowVisible(h):
            w = u.GetDlgItem(h, cid)
            if w:
                f.append(w)
                return False
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return f[0] if f else None


def text(h):
    # GetWindowText 对别的进程里的 EDIT 返回空（它走的是窗口标题那条路），
    # WM_GETTEXT 在 WM_USER 以下会被系统编组，能读到真正的编辑框内容
    b = ctypes.create_unicode_buffer(64)
    u.SendMessageW(h, 0x000D, 64, ctypes.addressof(b))
    return b.value


def click(h, x=20, y=12):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


db = open_db()
cur = db.cursor()


def db_money(cno):
    cur.execute("SELECT money, coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s" % cno)
    return cur.fetchone()


def db_lev(cno):
    cur.execute("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=%s" % cno)
    return cur.fetchone()[0]


def db_exp(cno):
    cur.execute("SELECT exp FROM taiwan_cain.charac_stat WHERE charac_no=%s" % cno)
    return cur.fetchone()[0]


CNO = 1
m0, c0 = db_money(CNO)
l0 = db_lev(CNO)
e0 = db_exp(CNO)
print("原值：金币 %d 复活币 %d 等级 %d 经验 %d" % (m0, c0, l0, e0))

p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main(p)
u.PostMessageW(u.GetDlgItem(main, IDC_CONN), 0x00F5, 0, 0)
time.sleep(4.5)
click(u.GetDlgItem(main, IDC_CHLIST), 40, 12)
time.sleep(1.5)
switch(main, 8)          # GM 页
time.sleep(0.8)

if not ctl(main, G_MONEY):
    print("!! GM 页没建出来")
    p.kill()
    sys.exit(1)

ok = True


def check(tag, got, want):
    global ok
    good = got == want
    ok = ok and good
    print("%s %s（得到 %s，期望 %s）" % ("OK " if good else "!! ", tag, got, want))
    return good


def set_text(h, s):
    b = ctypes.create_unicode_buffer(s)
    u.SendMessageW(h, 0x000C, 0, ctypes.addressof(b))     # WM_SETTEXT


def blur(h, wait=1.8):
    u.SendMessageW(h, 0x0008, 0, 0)                      # WM_KILLFOCUS → 触发提交
    time.sleep(wait)


def press(idx, name, wait=1.6):
    u.SendMessageW(ctl(main, G_QADD + idx), 0x00F5, 0, 0)   # BM_CLICK
    time.sleep(wait)
    print("点 %s → 金币框 %r" % (name, text(ctl(main, G_MONEY))))


# 1) 框里进页面就是 0：它是「这次设成多少」的一次性输入，不是当前值的镜子
ok &= check("金币框初始为 0", text(ctl(main, G_MONEY)), "0")
ok &= check("复活币框初始为 0", text(ctl(main, G_COIN)), "0")
ok &= check("等级框初始为 0", text(ctl(main, G_LEV)), "0")
grab(main, os.path.join(OUT, "gm_qadd.png"))

# 2) + 按钮：从「当前值」上加，不经过框
press(1, "金币 +1000万")
ok &= check("金币库值", db_money(CNO)[0], m0 + 10000000)
ok &= check("点完框仍是 0", text(ctl(main, G_MONEY)), "0")

press(2, "复活币 +100")
ok &= check("复活币库值", db_money(CNO)[1], c0 + 100)

press(0, "等级 +10")
ok &= check("等级库值", db_lev(CNO), min(l0 + 10, 100))
ok &= check("经验清零", db_exp(CNO), 0)

# 3) 连点三次金币：每次都在上一次结果上加
for _ in range(3):
    u.SendMessageW(ctl(main, G_QADD + 1), 0x00F5, 0, 0)
    time.sleep(1.2)
time.sleep(1.5)
ok &= check("连点三次金币", db_money(CNO)[0], m0 + 40000000)
grab(main, os.path.join(OUT, "gm_qadd_after.png"))

# 4) 手输一个绝对数：失焦按这个数写库，框随即回 0
e = ctl(main, G_MONEY)
set_text(e, "30000")
blur(e)
print("手输 30000 后：框里 %r" % text(ctl(main, G_MONEY)))
ok &= check("手输 30000 设为库值", db_money(CNO)[0], 30000)
ok &= check("交完框回 0", text(ctl(main, G_MONEY)), "0")

# 5) 填 0 = 不改：不能因为「框清空了再点别处」就把金币清零
set_text(e, "0")
blur(e)
ok &= check("填 0 不动库", db_money(CNO)[0], 30000)
ok &= check("填 0 后框仍是 0", text(ctl(main, G_MONEY)), "0")

# 6) 非法输入：字母串被规范成 0，等于没填，库里不动
set_text(e, "abc")
blur(e)
ok &= check("字母串规范成 0", text(ctl(main, G_MONEY)), "0")
ok &= check("库里没被 abc 污染", db_money(CNO)[0], 30000)

# 7) 超上限：失焦时夹到 4294967295，再写库（不能静默截断成别的数）
set_text(e, "99999999999")
blur(e)
ok &= check("超上限夹住并写入", db_money(CNO)[0], 4294967295)
ok &= check("写完框回 0", text(ctl(main, G_MONEY)), "0")

# 8) 空框失焦：兜回上一次（0），等于没填，库里不动
set_text(e, "")
blur(e)
ok &= check("空框不动库", db_money(CNO)[0], 4294967295)

# 9) 等级填 0：不改等级（不能再出现「点一下空框就掉到 1 级」）
le = ctl(main, G_LEV)
lev_now = db_lev(CNO)
set_text(le, "0")
blur(le)
ok &= check("等级填 0 不动库", db_lev(CNO), lev_now)

# 10) 等级填正常值：写进去
set_text(le, "60")
blur(le)
ok &= check("等级设为 60", db_lev(CNO), 60)
ok &= check("等级框回 0", text(ctl(main, G_LEV)), "0")

# 还原
cur.execute("UPDATE taiwan_cain_2nd.inventory SET money=%d, coin=%d WHERE charac_no=%d"
            % (m0, c0, CNO))
cur.execute("UPDATE taiwan_cain.charac_info SET lev=%d WHERE charac_no=%d" % (l0, CNO))
cur.execute("UPDATE taiwan_cain.charac_stat SET exp=%d WHERE charac_no=%d" % (e0, CNO))
print("已还原：金币 %d 复活币 %d 等级 %d 经验 %d" % (m0, c0, l0, e0))
p.kill()
print("结果：%s" % ("全部通过" if ok else "有失败"))
sys.exit(0 if ok else 1)
