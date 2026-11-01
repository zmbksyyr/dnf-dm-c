# -*- coding: utf-8 -*-
"""探针：改名被拒（emoji）之后，界面上的名字有没有退回库里的值。

只打印不判定。用来判断「拒绝路径有没有留下一个只在内存里的假名字」。
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS = 0x00F5, 0x0008
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_NAME = 1801
EMOJI = "测试\U0001F600"


def status(m):
    return txt(u.GetDlgItem(m, IDC_STATUS), 512)


c = db()
cur = c.cursor()
p = None
try:
    cur.execute("SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=1")
    before = cur.fetchone()[0]
    import subprocess
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    m = wait_main(p)
    u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 30 and not u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), 0x1004, 0, 0):
        time.sleep(0.2)
    click_at(u.GetDlgItem(m, IDC_CHLIST), 40, 12, 3.5)
    switch(m, 8)
    time.sleep(1.0)
    print("起点：库里 %r，名字框 %r" % (before, txt(ctl(m, G_NAME), 64)))

    set_text(ctl(m, G_NAME), EMOJI)
    u.SendMessageW(ctl(m, G_NAME), WM_KILLFOCUS, 0, 0)
    for i in range(10):
        time.sleep(0.4)
        cur.execute("SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=1")
        print("  t=%.1fs 状态栏=%r 名字框=%r 库里=%r"
              % ((i + 1) * 0.4, status(m), txt(ctl(m, G_NAME), 64), cur.fetchone()[0]))

    print("切页（GM→宠物→GM）强制一次 reload 之后：")
    switch(m, 4)
    time.sleep(0.8)
    switch(m, 8)
    time.sleep(1.0)
    print("  名字框=%r 状态栏=%r" % (txt(ctl(m, G_NAME), 64), status(m)))
    print("  左栏角色列表第 0 行的车牌（角色卡）看不出来，看状态栏与名字框即可")
finally:
    try:
        if p and p.poll() is None:
            p.kill()
    except Exception:
        pass
    c.close()
