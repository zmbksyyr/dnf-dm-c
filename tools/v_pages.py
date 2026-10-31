# -*- coding: utf-8 -*-
"""逐页截图：看分隔线、被删掉的说明行有没有留下空洞、左栏标签有没有被裁。

跑法：
  DNF_GM_BUILD=<exe 所在目录> python v_pages.py
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, OUT, NAMES, wait_main, switch, grab

IDC_CONN, IDC_CHLIST = 104, 113
PVF = r"D:\cache\game\opendnf-win-0.4.0-jbz69\clients\jbz69\Script.pvf"


def click(h, x=20, y=12):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def find_dlg(pid, timeout=6.0):
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = []

        def each(h, lp):
            if not u.IsWindowVisible(h):
                return True
            c = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, c, 64)
            if c.value != "#32770":
                return True
            w = wt.DWORD(0)
            u.GetWindowThreadProcessId(h, ctypes.byref(w))
            if w.value == pid:
                found.append(h)
                return False
            return True

        u.EnumWindows(cb(each), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main(p)
u.PostMessageW(u.GetDlgItem(main, IDC_CONN), 0x00F5, 0, 0)
time.sleep(4.5)
click(u.GetDlgItem(main, IDC_CHLIST), 40, 12)
time.sleep(1.5)

# 选 PVF：有了它，查询页/发放页的按钮才是可用的样子
u.PostMessageW(u.GetDlgItem(main, 107), 0x00F5, 0, 0)
d = find_dlg(p.pid)
if d:
    b = ctypes.create_unicode_buffer(PVF)
    u.SendMessageW(u.GetDlgItem(d, 1148), 0x000C, 0, ctypes.addressof(b))
    u.PostMessageW(d, 0x0111, 1, 0)
    time.sleep(6.0)

for i, nm in enumerate(NAMES):
    switch(main, i)
    time.sleep(0.5)
    grab(main, os.path.join(OUT, "p%d_%s.png" % (i, nm)))
    print("shot", i, nm)
p.kill()
print("done ->", OUT)
