# -*- coding: utf-8 -*-
"""空表/无记录时的报错：account_cargo 在这份库里是空表，改账号金库以前报「完成」，
界面再弹回原值；现在应该直接说清「没有 m_id=1 的记录」。"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch

PID = lambda page, n: 1000 + page * 100 + n
G_ACCM = PID(8, 9)

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
        if c.value == "DnfPage":
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

p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main(p)
u.PostMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)
time.sleep(4.5)
click(u.GetDlgItem(main, 113), 40, 12)
time.sleep(3.0)
switch(main, 8)
time.sleep(0.6)

w = ctl(main, G_ACCM)
print("账号金币框 原值 =", repr(gettext(w)))
settext(w, "777")
u.SendMessageW(w, 0x0008, 0, 0)          # WM_KILLFOCUS
time.sleep(2.0)
print("提交后 框内 =", repr(gettext(w)), "(应退回原值)")
p.kill()
