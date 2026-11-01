# -*- coding: utf-8 -*-
"""R3-002 定向验证：job_stop 超时后不许再 db_close。

旧版 app_disconnect 不看 job_stop 的返回值：30 秒没等到作业退出，照样
db_close（free 收发包缓冲 + 关 socket），而那个作业此刻正在这条连接上 recv。
后果是「一边 free 一边 recv」，进程崩或者连接被踩坏。

构造：用第二条连接 LOCK TABLES taiwan_cain_2nd.inventory WRITE，全服扫描的第一条
SELECT 读的就是它 → 扫描作业停在库上（可观测：PROCESSLIST 里 Waiting for table
metadata lock）→ 点「断开」→ UI 线程进 job_stop，等满 30 秒 → 断言：

  1) 日志里有「断开取消：后台任务 30 秒内没有退出」（说明走了拒绝分支）
  2) 连接没有关：扫描入口回的是「上一轮还没跑完」（作业还在跑），不是「请先连接数据库」
  3) 进程还活着
  4) 放锁后扫描能收尾、能正常断开 —— 拒绝是可重试的，不是把状态机卡死

旧版在这一步：没有那条日志，连接被关掉，进程多半直接没了。

    python tools/v_r3_jobstop.py
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, click_at

BM_CLICK = 0x00F5
IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191
G_SCANBAD = 1000 + 8 * 100 + 27        # 1827
CONFIRM = "扫描全服所有角色"
SCAN_TITLE = "全服清错误物品"

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


def top_windows(main, cls=None):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        owner = u.GetWindow(h, 4)
        if cls and c.value != cls:
            return True
        if owner not in (main, 0):
            return True
        out.append((h, c.value, t.value))
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


def wait_msgbox(main, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for h, c, t in top_windows(main, "#32770"):
            return h, dlg_text(h)
        time.sleep(0.05)
    return None, ""


def status_text(main):
    return txt(u.GetDlgItem(main, IDC_STATUS), 512)


def logfile():
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def log_off():
    p = logfile()
    return os.path.getsize(p) if p else 0


def logtail(n=40):
    p = logfile()
    if not p:
        return ""
    with open(p, encoding="utf-8", errors="replace") as f:
        return "".join(f.readlines()[-n:])


def log_since(off):
    """只读本次运行新增的部分。整份 tail 里可能还躺着上一轮同样的「断开取消」，
    拿它当证据等于用旧日志自证。"""
    p = logfile()
    if not p:
        return ""
    with open(p, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


class TableLock:
    def __init__(self, tables):
        self.tables = tables
        self.c = None

    def __enter__(self):
        self.c = db()
        self.c.cursor().execute("LOCK TABLES " + ",".join(t + " WRITE" for t in self.tables))
        return self

    def __exit__(self, *a):
        try:
            self.c.cursor().execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            self.c.close()
        except Exception:
            pass
        return False


def processlist():
    c = db()
    try:
        cur = c.cursor()
        cur.execute("SELECT ID,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                    "WHERE COMMAND='Query'")
        return cur.fetchall()
    finally:
        c.close()


def wait_blocked(marker, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, st, inf in processlist():
            if marker in inf and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, ""


def wait_connected(m, timeout=30.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), 0x1004, 0, 0) > 0:
            return True
        time.sleep(0.2)
    return False


def probe_scan_entry(m, timeout=8.0):
    """点全服扫描入口，返回第一个弹框的文本（点取消，不开扫）。"""
    u.PostMessageW(ctl(m, G_SCANBAD), BM_CLICK, 0, 0)
    b, t = wait_msgbox(m, timeout)
    if b:
        u.PostMessageW(u.GetDlgItem(b, 2), BM_CLICK, 0, 0)
        t0 = time.time()
        while time.time() - t0 < 8 and top_windows(m, "#32770"):
            time.sleep(0.05)
    return t


def main():
    print("EXE =", EXE)
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        if not ok("0 连接数据库", wait_connected(m)):
            return 1
        click_at(u.GetDlgItem(m, IDC_CHLIST), 40, 12, 3.5)
        switch(m, 8)
        time.sleep(1.0)
        print("   状态栏 =", repr(status_text(m)))
        ok("0 扫描入口存在", bool(ctl(m, G_SCANBAD)), "控件 %d" % G_SCANBAD)

        with TableLock(["taiwan_cain_2nd.inventory"]):
            # 起一次扫描，让它卡在表锁上
            u.PostMessageW(ctl(m, G_SCANBAD), BM_CLICK, 0, 0)
            b, t = wait_msgbox(m, 8.0)
            ok("1 弹出扫描确认框", CONFIRM in t, t[:60])
            if b:
                u.PostMessageW(u.GetDlgItem(b, 1), BM_CLICK, 0, 0)     # 确定
                t0 = time.time()
                while time.time() - t0 < 8 and top_windows(m, "#32770"):
                    time.sleep(0.05)
            blocked, st = wait_blocked("taiwan_cain_2nd.inventory")
            ok("1 扫描作业真的卡在库上", blocked, st)

            off = log_off()                 # 只看这次点完断开之后新写进日志的行
            u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)  # 断开
            # app_disconnect 会逐个 job_stop，每个最多 30 秒。这里等它把 30 秒等完。
            print("   已点断开，等 job_stop 的 30 秒超时…")
            time.sleep(33.0)
            print("   状态栏 =", repr(status_text(m)))
            print("   进程 poll =", p.poll())

        text = log_since(off)
        hit = [l for l in text.splitlines() if "断开取消" in l]
        ok("2 日志里有「断开取消：后台任务 30 秒内没有退出」的拒绝分支",
           bool(hit), hit[0] if hit else "(日志里没有)")
        print("---- 这次运行新写的日志（最后 6 行） ----")
        print("\n".join(text.splitlines()[-6:]))
        ok("3 进程还活着（旧版在这里会一边 free 一边 recv）", p.poll() is None,
           "poll=%s" % (p.poll(),))
        t = probe_scan_entry(m)
        print("   点扫描入口回：%r" % (t,))
        # 进程已经崩了的话所有控件都读不到空串，这条会被「空串不含那六个字」蒙混过关，
        # 所以必须同时要求进程活着
        ok("4 连接没有被关（入口回「上一轮还没跑完」而不是「请先连接数据库」）",
           p.poll() is None and "上一轮还没跑完" in t, "%r / poll=%s" % (t, p.poll()))

        # 放锁：作业收尾 → 闸自己放下 → 这时断开必须能成功
        t0 = time.time()
        disc = ""
        u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)
        while time.time() - t0 < 25:
            disc = status_text(m)
            if "已断开数据库" in disc:
                break
            time.sleep(0.3)
        ok("5 放锁后「再点一次断开」能成功（拒绝是可重试的）", "已断开数据库" in disc, disc)
        ok("5 进程还活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
