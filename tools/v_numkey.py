# -*- coding: utf-8 -*-
"""数值框的真实键盘输入测试：数字能进、字母和汉字进不去。

跑法：
  DNF_GM_BUILD=<exe 所在目录> python v_numkey.py

走前台窗口 + 真实鼠标点击 / keybd_event，而不是 PostMessage：
EDIT 对没有焦点的 WM_CHAR 会直接丢弃，只有走真实的输入路径才测得到
ES_NUMBER 和子类里那两道过滤。
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, OUT, wait_main, switch, grab, db as open_db, PID, start
from pages import check, ctl, set_text, txt as text


G_MONEY = PID(8, 5)
IDC_CONN, IDC_CHLIST = 104, 113

u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.SetForegroundWindow.argtypes = [wt.HWND]
u.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u.keybd_event.argtypes = [wt.BYTE, wt.BYTE, wt.DWORD, ctypes.c_size_t]
u.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]


def key(h, ch):
    # 键盘事件要有前台窗口才落得到目标上，这个会话里抢不到前台；
    # 焦点已经在框内、输入队列也附加过，直接把 WM_CHAR 发给它，
    # 走的是和真实按键同一条处理路径（子类 + ES_NUMBER 都在）。
    u.SendMessageW(h, 0x0102, ord(ch), 0)
    time.sleep(0.05)


db = open_db()
cur = db.cursor()
CNO = 1
cur.execute("SELECT money FROM taiwan_cain_2nd.inventory WHERE charac_no=%d" % CNO)
m0 = cur.fetchone()[0]
print("金币原值 %d" % m0)

p, main = start()
r = wt.RECT()
lp = (12 << 16) | 40
u.PostMessageW(u.GetDlgItem(main, IDC_CHLIST), 0x0201, 1, lp)
u.PostMessageW(u.GetDlgItem(main, IDC_CHLIST), 0x0202, 0, lp)
time.sleep(1.5)
switch(main, 8)
time.sleep(0.8)

e = ctl(main, G_MONEY)
ok = True
if not e:
    print("!! GM 页没建出来")
    p.kill()
    sys.exit(1)

# 真实点击输入框：把窗口推到前台，鼠标点进框里。
# 直接 SetForegroundWindow 会被前台锁拒掉，先把本线程和目标线程的输入队列
# 附加到一起，SetForegroundWindow / SetFocus 才生效。
k32 = ctypes.WinDLL("kernel32")
k32.GetCurrentThreadId.restype = wt.DWORD
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u.GetWindowThreadProcessId.restype = wt.DWORD
u.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
u.SetFocus.argtypes = [wt.HWND]
pid_d = wt.DWORD()
tgt_tid = u.GetWindowThreadProcessId(main, ctypes.byref(pid_d))
u.AttachThreadInput(k32.GetCurrentThreadId(), tgt_tid, True)
u.ShowWindow(main, 9)              # SW_RESTORE
u.SetForegroundWindow(main)
time.sleep(0.5)
fg = u.GetForegroundWindow()
print("前台窗口 == 主窗口：%s" % (fg == main))
u.GetWindowRect(e, ctypes.byref(r))
cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2
u.SetCursorPos(cx, cy)
time.sleep(0.15)
u.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
u.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
time.sleep(0.3)
u.SetFocus(e)
time.sleep(0.2)
print("焦点在金币框：%s" % (u.GetFocus() == e))

# 清空后依次敲 A（字母）、5（数字）、B（字母）
set_text(e, "")
time.sleep(0.2)
key(e, "A")
key(e, "5")
key(e, "B")
time.sleep(0.4)
got = text(e)
ok &= check("字母进不去、数字能进", got, "5")
grab(main, os.path.join(OUT, "gm_numkey.png"))

# 超上限的输入同样进不去：先填满 4294967295，再敲一位
set_text(e, "4294967295")
time.sleep(0.2)
key(e, "5")   # -> 42949672955 超上限，应被拒
time.sleep(0.3)
ok &= check("超上限敲不进去", text(e), "4294967295")

# 写回原值并失焦，别把测试数据留在库里
set_text(e, str(m0))
u.SendMessageW(e, 0x0008, 0, 0)
time.sleep(1.5)
cur.execute("SELECT money FROM taiwan_cain_2nd.inventory WHERE charac_no=%d" % CNO)
print("还原后金币 %d" % cur.fetchone()[0])
p.kill()
print("结果：%s" % ("全部通过" if ok else "有失败"))
