# -*- coding: utf-8 -*-
"""任务材料的数量要跟着行进发放队列。

链路：选 PVF → 加载任务材料（列表里「数量」列 = 单个任务最多要几个）
      → 双击首行（状态栏应写 ×N）→ 全部加入 → 发放页「数量」列逐条对得上。

跑法：
  DNF_GM_BUILD=<exe 所在目录> python v_tasknum.py
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, OUT, wait_main, switch, grab

PID = lambda page, n: 1000 + page * 100 + n
Q_INFO, Q_ADD, Q_ALL, Q_TASK = PID(1, 3), PID(1, 5), PID(1, 6), PID(1, 10)
G_LIST, G_INFO = PID(2, 12), PID(2, 17)
IDC_CONN, IDC_PVFS, IDC_CHLIST = 104, 107, 113

PVF = r"D:\cache\game\opendnf-win-0.4.0-jbz69\clients\jbz69\Script.pvf"

u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.SendDlgItemMessageW.argtypes = [wt.HWND, ctypes.c_int, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendDlgItemMessageW.restype = wt.LPARAM
u.GetDlgItemTextW.argtypes = [wt.HWND, ctypes.c_int, wt.LPWSTR, ctypes.c_int]
u.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]


def text(h):
    b = ctypes.create_unicode_buffer(512)
    u.GetWindowTextW(h, b, 512)
    return b.value


def ctl(main, cid):
    """页控件挂在 DnfPage 子窗口下，主窗口 GetDlgItem 找不到，得先枚举出来。"""
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


def label(main, cid):
    return text(ctl(main, cid))


def click(h, x=20, y=12):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def dblclick(h, x, y):
    # 跨进程 PostMessage 的两下单击不会让系统补出 DBLCLK，得自己发 WM_LBUTTONDBLCLK
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)
    time.sleep(0.05)
    u.PostMessageW(h, 0x0203, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def rows(lv):
    return u.SendMessageW(lv, 0x1004, 0, 0)   # LVM_GETITEMCOUNT


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


def pick_pvf(main, pid):
    u.PostMessageW(u.GetDlgItem(main, IDC_PVFS), 0x00F5, 0, 0)
    d = find_dlg(pid)
    if not d:
        print("!! 没等到 PVF 选择对话框")
        return False
    ed = u.GetDlgItem(d, 1148)
    b = ctypes.create_unicode_buffer(PVF)
    u.SendMessageW(ed, 0x000C, 0, ctypes.addressof(b))     # WM_SETTEXT
    u.PostMessageW(d, 0x0111, 1, 0)                        # IDOK
    time.sleep(0.5)
    return True


p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main(p)
print("主窗口 ok")

u.PostMessageW(u.GetDlgItem(main, IDC_CONN), 0x00F5, 0, 0)
time.sleep(4.5)
click(u.GetDlgItem(main, IDC_CHLIST), 40, 12)     # 选第一个角色
time.sleep(1.5)
print("角色：%s" % label(main, 114))

if not pick_pvf(main, p.pid):
    p.kill()
    sys.exit(1)
print("PVF 选好，等解析…")
time.sleep(6.0)

switch(main, 1)                                   # 查询页
time.sleep(0.6)

t0 = time.time()
u.PostMessageW(ctl(main, Q_TASK), 0x00F5, 0, 0)
time.sleep(2.0)
lv = ctl(main, PID(1, 2))
n = rows(lv)
print("加载任务材料：%d 行，耗时 %.1fs" % (n, time.time() - t0))
print("说明：%s" % label(main, Q_INFO))
grab(main, os.path.join(OUT, "task_rows.png"))

if n <= 0:
    p.kill()
    sys.exit(2)

# 双击首行 → 单条入队，状态栏应带出这一行的数量
rc = wt.RECT()
u.GetClientRect(lv, ctypes.byref(rc))
dblclick(lv, 40, 12)
time.sleep(1.0)
grab(main, os.path.join(OUT, "task_one.png"))
print("单条入队后状态栏：%s" % label(main, 191))
switch(main, 2)
time.sleep(0.8)
glv0 = ctl(main, G_LIST)
print("单条加入后发放队列：%d 行" % rows(glv0))
grab(main, os.path.join(OUT, "grant_one.png"))
switch(main, 1)
time.sleep(0.6)

# 再全部加入，然后把发放页的数量列拍下来
u.PostMessageW(ctl(main, Q_ALL), 0x00F5, 0, 0)
time.sleep(1.5)
switch(main, 2)
time.sleep(0.8)
glv = ctl(main, PID(2, 12))
print("发放队列：%d 行" % rows(glv))
print("发放页说明：%s" % label(main, G_INFO))
grab(main, os.path.join(OUT, "grant_num.png"))
p.kill()
print("截图：task_rows.png / task_one.png / grant_num.png")
