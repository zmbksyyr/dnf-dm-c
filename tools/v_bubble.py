# -*- coding: utf-8 -*-
"""泡点回归：开 → 等两轮 → 赠送点券必须涨 → 关 → 必须不再涨。跑完复原。

现库没有在线账号（login_account_3 只有 1 行且 login_status=0），所以这里临时
把 m_id=1 置为在线，finally 里改回 0——和 v_selfcheck 改表名是同一类做法。
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, db

PID = lambda page, n: 1000 + page * 100 + n
G_BON = PID(8, 28)        # 启用泡点（复选框）
G_BSEC = PID(8, 29)       # 间隔（秒）
G_BVAL = PID(8, 30)       # 每次加（点）
G_BAPPLY = PID(8, 31)     # 应用
G_BINFO = PID(8, 32)      # 状态行

M_ID = 1
ACC = "1"
SEC, VAL = 5, 1           # 5 秒一轮，每轮 1 点：13 秒内应结算 2 次


def ctl(main, cid):
    """在可见的 DnfPage 下找控件。GM 页控件都挂在页窗口上，不在主窗口上。"""
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "DnfPage" and u.IsWindowVisible(h):
            w = u.GetDlgItem(h, cid)
            if w:
                found.append(w)
                return False
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return found[0] if found else None


def gettext(h):
    b = ctypes.create_unicode_buffer(256)
    u.SendMessageW(h, 0x000D, 256, ctypes.addressof(b))
    return b.value


def settext(h, s):
    b = ctypes.create_unicode_buffer(s)
    u.SendMessageW(h, 0x000C, 0, ctypes.addressof(b))


def click(lv, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(lv, 0x0201, 1, lp)
    u.PostMessageW(lv, 0x0202, 0, lp)


def cera():
    c = db()
    cur = c.cursor()
    cur.execute("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s", (ACC,))
    row = cur.fetchone()
    c.close()
    return row[0] if row else None


def set_online(v):
    c = db()
    cur = c.cursor()
    cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s", (v, M_ID))
    c.close()


def set_cera(v):
    c = db()
    cur = c.cursor()
    cur.execute("UPDATE taiwan_billing.cash_cera_point SET cera_point=%s WHERE account=%s", (v, ACC))
    c.close()


c0 = db()
cur = c0.cursor()
cur.execute("SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s", (M_ID,))
row = cur.fetchone()
LOGIN0 = row[0] if row else None
c0.close()
P0 = cera()
print("初始：login_status=%s cera_point=%s" % (LOGIN0, P0))

p = None
try:
    set_online(1)
    print("已把 m_id=%d 置为在线（测完改回）" % M_ID)

    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main()
    u.SendMessageW(u.GetDlgItem(main, 104), 0x00F5, 0, 0)     # 连接
    time.sleep(4.5)
    click(u.GetDlgItem(main, 113), 40, 12)                     # 选中第一个角色
    time.sleep(3.0)
    switch(main, 8)                                            # 切到 GM 页
    time.sleep(1.0)

    bon = ctl(main, G_BON)
    bsec = ctl(main, G_BSEC)
    bval = ctl(main, G_BVAL)
    bap = ctl(main, G_BAPPLY)
    binfo = ctl(main, G_BINFO)
    if not all([bon, bsec, bval, bap, binfo]):
        print("!! 找不到泡点控件：", bon, bsec, bval, bap, binfo)
        sys.exit(1)
    print("状态行：", repr(gettext(binfo)))

    # 开：勾上 + 5 秒 + 1 点 + 应用
    u.SendMessageW(bon, 0x00F5, 0, 0)          # BM_CLICK 切换勾选
    settext(bsec, str(SEC))
    settext(bval, str(VAL))
    u.SendMessageW(bap, 0x00F5, 0, 0)          # BM_CLICK = 点「应用」
    time.sleep(0.8)
    print("已启用 · 状态行：", repr(gettext(binfo)))

    time.sleep(13.0)                            # 跨过两轮（5s / 10s）
    p1 = cera()
    print("13 秒后 cera_point = %s（涨了 %d）" % (p1, p1 - P0))
    print("状态行：", repr(gettext(binfo)))

    # 关
    u.SendMessageW(bon, 0x00F5, 0, 0)
    u.SendMessageW(bap, 0x00F5, 0, 0)
    time.sleep(0.8)
    pa = cera()
    time.sleep(9.0)                             # 关掉之后再等近两轮
    pb = cera()
    print("关闭后 %s → 9 秒后 %s（应相等）" % (pa, pb))
    print("状态行：", repr(gettext(binfo)))

    ok1 = p1 - P0 >= 2
    ok2 = pa == pb
    print("\n1) 开启期间按间隔加点: %s（涨 %d，至少应为 2）" % ("OK" if ok1 else "FAIL", p1 - P0))
    print("2) 关闭后不再加点: %s" % ("OK" if ok2 else "FAIL"))
    print("结果：%s" % ("通过" if ok1 and ok2 else "失败"))
finally:
    if p:
        p.kill()
    set_cera(P0)
    set_online(LOGIN0 if LOGIN0 is not None else 0)
    print("已复原：cera_point=%s login_status=%s" % (P0, LOGIN0))
