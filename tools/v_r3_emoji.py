# -*- coding: utf-8 -*-
"""R3-006 定向验证：4 字节字符（emoji）改名必须被明确拦下。

charac_name 是 utf8(mb3) 列，emoji 在非严格模式下会被静默截断/替 '?'，
旧版照样回一句「改名 完成」，用户看到的名字和库里对不上。

本脚本走真实 GUI（GM 页的名字框 + 失焦提交）：

  1) 改成「测试😀」→ 必须弹一个明确说「4 字节」的提示，且库里名字一个字都没动
  2) 正对照：改成普通中文名「测试中」（3 字节/字）→ 必须真的写进去
  3) 收尾改回原名，断言 HEX / CHAR_LENGTH / LENGTH 与改前逐项一致

    python tools/v_r3_emoji.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS = 0x00F5, 0x0008
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_NAME = 1000 + 8 * 100 + 1        # 1801
CNO = 1

EMOJI_NAME = "测试\U0001F600"        # 测试😀 —— 4 字节字符
GOOD_NAME = "测试中"

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


MAIN = 0


def top_windows(main=None, cls="#32770"):
    main = main or MAIN
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        owner = u.GetWindow(h, 4)
        if cls and c.value != cls:
            return True
        # MessageBox 的 owner 是 GetActiveWindow()，可能是页面子窗口而不是主窗口，
        # 所以「主窗口本身或它的子窗口」都算自己的弹窗
        if owner and owner != main and not u.IsChild(main, owner):
            return True
        out.append(h)
        return True

    u.EnumWindows(cb(each), 0)
    return out


def dlg_text(h):
    parts = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(c, lp):
        cls = ctypes.create_unicode_buffer(32)
        u.GetClassNameW(c, cls, 32)
        if cls.value in ("Static", "Button"):
            t = txt(c, 2048)
            if t:
                parts.append(t)
        return True

    u.EnumChildWindows(h, cb(each), 0)
    return " / ".join(parts)


def wait_box(main, timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = top_windows(main)
        if w:
            time.sleep(0.2)
            return w[0], dlg_text(w[0])
        time.sleep(0.05)
    return None, ""


def close_box():
    w = top_windows()
    if not w:
        return False
    b = u.GetDlgItem(w[0], 1)
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 8:
        if not top_windows():
            return True
        time.sleep(0.05)
    return False


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


def status_text(main):
    return txt(u.GetDlgItem(main, IDC_STATUS), 512)


def log_off():
    import glob
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return os.path.getsize(fs[-1]) if fs else 0


def read_since(off):
    import glob
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    if not fs:
        return ""
    with open(fs[-1], "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def name_row(cur):
    cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                "FROM taiwan_cain.charac_info WHERE charac_no=%s", (CNO,))
    return cur.fetchone()


def set_name(main, name):
    h = ctl(main, G_NAME)
    if not h:
        return False
    set_text(h, name)
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
    return True


def main():
    global MAIN
    print("EXE =", EXE)
    c = db()
    cur = c.cursor()
    p = None
    try:
        before = name_row(cur)
        print("改前 name=%r hex=%s CL=%s L=%s" % before)
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = MAIN = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        t0 = time.time()
        while time.time() - t0 < 30 and not u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), 0x1004, 0, 0):
            time.sleep(0.2)
        click_at(u.GetDlgItem(m, IDC_CHLIST), 40, 12, 3.5)
        switch(m, 8)
        time.sleep(1.0)
        ok("0 GM 页名字框存在", bool(ctl(m, G_NAME)), "控件 %d" % G_NAME)

        # ---------------- 1) emoji 必须被拦 ----------------
        # 失败提示走的是状态栏 + 日志（改名在后台任务里判定，UI 线程不能查库），
        # 所以要盯这两处，而不是等一个 MessageBox。
        off = log_off()
        ok("1 填「测试😀」并失焦", set_name(m, EMOJI_NAME))
        hit, st, lg = "", "", ""
        t0 = time.time()
        while time.time() - t0 < 15:
            st = status_text(m)
            lg = read_since(off)
            for line in lg.splitlines():
                if "改名 失败" in line:
                    hit = line
            if hit and "4 字节" in st:
                break
            time.sleep(0.3)
        print("   状态栏 = %r" % (st,))
        print("   日志   = %r" % (hit,))
        ok("1 改名被明确拒绝（不是静默成功）", bool(hit), hit or lg[-200:])
        ok("1 拒绝理由点明「4 字节」字符", "4 字节" in (hit or ""), hit)
        # 状态栏会被随后的「已载入 …」盖掉，只能当参考，不当判据
        info("拒绝瞬间的状态栏", st or "(被后续状态覆盖)")
        info("拒绝后名字框里还留着什么", repr(txt(ctl(m, G_NAME), 64)))
        row1 = name_row(cur)
        print("   emoji 后库里 name=%r hex=%s CL=%s L=%s" % row1)
        ok("1 库里名字没变（一个字节都没动）", row1 == before,
           "%r vs %r" % (row1[0], before[0]))
        ok("1 库里没有出现 '?' 之类的替字符", row1[0] == before[0], "%r" % (row1[0],))

        # ---------------- 2) 正对照：普通中文名要能改 ----------------
        switch(m, 8)
        time.sleep(0.6)
        ok("2 填「%s」并失焦" % GOOD_NAME, set_name(m, GOOD_NAME))
        got = None
        t0 = time.time()
        while time.time() - t0 < 12:
            got = q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (CNO,))
            if got == GOOD_NAME:
                break
            if top_windows():
                print("   [改名弹窗] %r" % dlg_text(top_windows()[0]))
                close_box()
            time.sleep(0.3)
        row2 = name_row(cur)
        print("   中文名落库 name=%r hex=%s CL=%s L=%s" % row2)
        ok("2 普通中文名能改进去", row2[0] == GOOD_NAME, "%r" % (row2[0],))
        ok("2 utf8 三字节：LENGTH=9、CHAR_LENGTH=3", (row2[2], row2[3]) == (3, 9),
           "%s/%s" % (row2[2], row2[3]))
        ok("2 没有双重编码", row2[1] == GOOD_NAME.encode("utf-8").hex().upper(), "hex=%s" % (row2[1],))

        # ---------------- 3) 还原 ----------------
        switch(m, 8)
        time.sleep(0.6)
        ok("3 改回原名", set_name(m, before[0]))
        t0 = time.time()
        while time.time() - t0 < 15:
            if q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s",
                  (CNO,)) == before[0]:
                break
            if top_windows():
                close_box()
            time.sleep(0.3)
        row3 = name_row(cur)
        print("   还原后 name=%r hex=%s CL=%s L=%s" % row3)
        ok("3 还原后字节与原值逐项一致", row3 == before, "%r vs %r" % (row3, before))
        ok("4 进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        c.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
