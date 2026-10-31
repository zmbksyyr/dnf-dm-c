# -*- coding: utf-8 -*-
"""GM 页「+数量」快捷按钮：点一下在原有数值上加，并立刻写库。

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

print("框里：金币=%s 复活币=%s 等级=%s"
      % (text(ctl(main, G_MONEY)), text(ctl(main, G_COIN)), text(ctl(main, G_LEV))))
grab(main, os.path.join(OUT, "gm_qadd.png"))


def check(tag, got, want):
    print("%s %s（得到 %s，期望 %s）" % ("OK " if got == want else "!! ", tag, got, want))
    return got == want


ok = True
# 框里是「设成多少」：进页面就该显示库里的当前值，不是 0
ok &= check("金币框=库值", text(ctl(main, G_MONEY)), str(m0))
ok &= check("复活币框=库值", text(ctl(main, G_COIN)), str(c0))
ok &= check("等级框=库值", text(ctl(main, G_LEV)), str(l0))


def press(idx, name, waitdb=1.6):
    b = ctl(main, G_QADD + idx)
    u.SendMessageW(b, 0x00F5, 0, 0)            # BM_CLICK
    time.sleep(waitdb)
    print("点 %s → 金币框里 %r" % (name, text(ctl(main, G_MONEY))))


def set_text(h, s):
    b = ctypes.create_unicode_buffer(s)
    u.SendMessageW(h, 0x000C, 0, ctypes.addressof(b))     # WM_SETTEXT


press(1, "+1000万")
ok &= check("金币库值", db_money(CNO)[0], m0 + 10000000)
# + 是「在现有值上加」：结果要写回框，框和库是同一条数
ok &= check("金币框跟上", text(ctl(main, G_MONEY)), str(m0 + 10000000))

press(2, "复活币+100")
ok &= check("复活币库值", db_money(CNO)[1], c0 + 100)

press(0, "等级+10")
want = min(l0 + 10, 100)
ok &= check("等级库值", db_lev(CNO), want)
ok &= check("经验清零", db_exp(CNO), 0)

# 再点两次：85 → 95 → 100，第三次该顶住不动（等级上限 100，不能加到 110）
press(0, "等级+10 第二次")
ok &= check("等级到顶", db_lev(CNO), 100)
press(0, "等级+10 第三次")
ok &= check("顶住不再加", db_lev(CNO), 100)

# 连点 3 次金币：每次都在上一次的结果上加，不是回到原值再加
for _ in range(3):
    u.SendMessageW(ctl(main, G_QADD + 1), 0x00F5, 0, 0)
    time.sleep(1.2)
time.sleep(1.5)
ok &= check("连点三次金币", db_money(CNO)[0], m0 + 40000000)
grab(main, os.path.join(OUT, "gm_qadd_after.png"))

# 手输绝对值：框是「设成多少」，失焦按这个数写库，框不回 0
e = ctl(main, G_MONEY)
set_text(e, "30000")
u.SendMessageW(e, 0x0008, 0, 0)                       # WM_KILLFOCUS → 触发提交
time.sleep(1.8)
print("手输 30000 后：框里 %r" % text(ctl(main, G_MONEY)))
ok &= check("手输 30000 设为库值", db_money(CNO)[0], 30000)
ok &= check("框保持 30000", text(ctl(main, G_MONEY)), "30000")

# 非法输入不接受：字母串会被 ui_num 在失焦时规范成 0，不会原样落到库里
set_text(e, "abc")
u.SendMessageW(e, 0x0008, 0, 0)
time.sleep(1.6)
ok &= check("字母串被规范化", text(ctl(main, G_MONEY)), "0")
ok &= check("库里不是 abc", db_money(CNO)[0], 0)

# 超上限：失焦时夹到 4294967295，写成别的数就说明截断了
set_text(e, "99999999999")
u.SendMessageW(e, 0x0008, 0, 0)
time.sleep(1.6)
ok &= check("超上限夹住", text(ctl(main, G_MONEY)), "4294967295")
ok &= check("库值=上限", db_money(CNO)[0], 4294967295)

# 清空后失焦：框兜回上一次的值，不把「清空重打」当成 0 写进库
set_text(e, "")
u.SendMessageW(e, 0x0008, 0, 0)
time.sleep(1.5)
ok &= check("空框兜回原值", text(ctl(main, G_MONEY)), "4294967295")
ok &= check("空框不动库", db_money(CNO)[0], 4294967295)

# 等级框填 0（越界）：ui_num 先夹到 1，再按 1 级提交
le = ctl(main, G_LEV)
set_text(le, "0")
u.SendMessageW(le, 0x0008, 0, 0)
time.sleep(1.6)
ok &= check("等级 0 夹到 1", db_lev(CNO), 1)

# 输入框照原样写回，别把测试污染留在库里
set_text(e, str(m0))
u.SendMessageW(e, 0x0008, 0, 0)
time.sleep(1.5)

# 还原
cur.execute("UPDATE taiwan_cain_2nd.inventory SET money=%d, coin=%d WHERE charac_no=%d"
            % (m0, c0, CNO))
cur.execute("UPDATE taiwan_cain.charac_info SET lev=%d WHERE charac_no=%d" % (l0, CNO))
cur.execute("UPDATE taiwan_cain.charac_stat SET exp=%d WHERE charac_no=%d" % (e0, CNO))
print("已还原：金币 %d 复活币 %d 等级 %d 经验 %d" % (m0, c0, l0, e0))
p.kill()
print("结果：%s" % ("全部通过" if ok else "有失败"))
