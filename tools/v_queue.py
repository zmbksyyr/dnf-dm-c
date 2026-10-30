# 连点四个 GM 字段（不等待），验证写入队列不再互相覆盖、且都落到正确的角色。
# 旧实现只有一个待办槽：第二个动作会把第一个顶掉，四个里可能只落两个。
import ctypes
import os
import subprocess
import sys
import time


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, db as open_db

PID = lambda page, n: 1000 + page * 100 + n
G_MONEY, G_COIN, G_CERA, G_LEV = PID(8, 5), PID(8, 6), PID(8, 7), PID(8, 2)


def click(h, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


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


def gettext(h):
    b = ctypes.create_unicode_buffer(64)
    u.SendMessageW(h, 0x000D, 64, ctypes.addressof(b))
    return b.value


def settext(h, s):
    b = ctypes.create_unicode_buffer(s)
    u.SendMessageW(h, 0x000C, 0, ctypes.addressof(b))


db = open_db()
cur = db.cursor()
cur.execute("SELECT money, coin FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
m0, c0 = cur.fetchone()
cur.execute("SELECT cera FROM taiwan_billing.cash_cera WHERE account=1")
ce0 = cur.fetchone()
cur.execute("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=1")
pt0 = cur.fetchone()[0]
cur.execute("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=1")
a0 = cur.fetchone()[0]
cur.execute("SELECT exp FROM taiwan_cain.charac_stat WHERE charac_no=1")
e0 = cur.fetchone()[0]
print("原值 money=%s coin=%s cera=%s lev=%s" % (m0, c0, ce0[0], a0))

p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main()
u.PostMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)
time.sleep(4.5)
click(u.GetDlgItem(main, 113), 40, 12)
time.sleep(3.0)
switch(main, 8)
time.sleep(0.8)

# 连着改四个字段，中间只留 80ms：模拟用户按 Tab 快速跳着改
for cid, val in ((G_MONEY, "111111"), (G_COIN, "2222"), (G_CERA, "3333"), (G_LEV, "86")):
    w = ctl(main, cid)
    settext(w, val)
    u.SendMessageW(w, 0x0008, 0, 0)          # WM_KILLFOCUS → 失焦提交
    time.sleep(0.08)

time.sleep(6.0)
cur.execute("SELECT money, coin FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
m1, c1 = cur.fetchone()
cur.execute("SELECT cera FROM taiwan_billing.cash_cera WHERE account=1")
ce1 = cur.fetchone()
cur.execute("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=1")
pt1 = cur.fetchone()[0]
cur.execute("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=1")
a1 = cur.fetchone()[0]
print("写后 money=%s coin=%s cera=%s lev=%s" % (m1, c1, ce1[0], a1))

ok = (m1 == 111111 and c1 == 2222 and ce1[0] == 3333 and a1 == 86)
print("四个字段全部落地:", "OK" if ok else "FAIL")

# 复原
cur.execute("UPDATE taiwan_cain_2nd.inventory SET money=%s, coin=%s WHERE charac_no=1", (m0, c0))
cur.execute("UPDATE taiwan_billing.cash_cera SET cera=%s WHERE account=1", (ce0[0],))
cur.execute("UPDATE taiwan_billing.cash_cera_point SET cera_point=%s WHERE account=1", (pt0,))
cur.execute("UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=1", (a0,))
cur.execute("UPDATE taiwan_cain.charac_stat SET exp=%s WHERE charac_no=1", (e0,))
cur.execute("SELECT money, coin FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
print("复原:", cur.fetchone())
p.kill()
db.close()
