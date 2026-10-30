# -*- coding: utf-8 -*-
"""量 PVF 段与页签「物品」的横向关系：PVF 标签左边、页签控件左边、页签文本左边。"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, g, EXE, wait_main

from ctypes import wintypes as wt

def kids(h):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(w, lp):
        out.append(w)
        return True
    u.EnumChildWindows(h, cb(each), 0)
    return out

def text(h):
    b = ctypes.create_unicode_buffer(128)
    u.SendMessageW(h, 0x000D, 128, ctypes.addressof(b))
    return b.value

def rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r

def dpi_scale():
    dc = u.GetDC(0)
    d = g.GetDeviceCaps(dc, 88)          # LOGPIXELSX
    u.ReleaseDC(0, dc)
    return d / 96.0

p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main()
u.PostMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)
time.sleep(4.0)

sc = dpi_scale()
pvf = tab = None
for h in kids(main):
    cls = ctypes.create_unicode_buffer(64)
    u.GetClassNameW(h, cls, 64)
    if cls.value == "SysTabControl32":
        tab = h
    elif cls.value == "Static" and text(h) == "PVF":
        pvf = h
print("dpi scale %.2f" % sc)
if pvf and tab:
    rp, rt = rect(pvf), rect(tab)
    print("PVF 标签   left=%d  top=%d" % (rp.left, rp.top))
    print("页签控件   left=%d  top=%d" % (rt.left, rt.top))
    print("PVF - 页签 = %d px（页签文本内边距约 %.0f）"
          % (rp.left - rt.left, 6 * sc))
else:
    print("没找到：pvf=%s tab=%s" % (pvf, tab))
p.kill()
