# -*- coding: utf-8 -*-
"""空表/无记录时的报错：account_cargo 在这份库里是空表，改账号金库以前报「完成」，
界面再弹回原值；现在应该直接说清「没有 m_id=1 的记录」。"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, PID, start
from pages import click_at as click, ctl, txt as gettext, set_text as settext


G_ACCM = PID(8, 9)


p, main = start()
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
