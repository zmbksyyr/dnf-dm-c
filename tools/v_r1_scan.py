# -*- coding: utf-8 -*-
"""扫描页的闸是否诚实：窗口关掉、连接断开之后，入口还能不能用。

这一版不再靠 sleep 抢时序。旧版有三处靠赌：
  1) 「点 6003 → 马上关窗」—— 6003 的处理函数第一句是 `if (s_scan_busy) return`，
     只要扫描已经跑完，点击就是空操作，B 组照样全绿；
  2) 关窗后 sleep 1 秒 —— 扫描在这台机器上几百毫秒就结束了，断言过是因为作业
     自己跑完了，不是因为关窗复位了什么；库大一点同一个断言就变红（而那时的红
     恰恰是正确行为）；
  3) 判断「作业还活着」用的是时间，而作业活多久完全由库里有多少行决定。

现在把「作业活着」变成可控量：用第二条连接 `LOCK TABLES ... WRITE` 把扫描要读的
表锁住，作业就会停在 live=1，直到我们放锁。三组用例：

  A 对照：正常跑完一次扫描 → 关窗 → 入口可用（证明入口本身是好的）。
  B 作业还活着时关窗：锁 inventory 拖住扫描 → 点 6003 → 关窗 →
    入口必须回「上一轮还没跑完」（闸还立着，因为作业真的没结束）→ 放锁 →
    作业收尾 → 入口恢复。旧的 sleep 写法在这一步只能二选一（假绿/假红）。
  C 进了队列又被泄洪（R3-003）：锁 charac_info 拖住轮询作业（连接变忙）→
    点全服扫描（动作只是排进队列，不会起线程）→ 点断开（队列被丢）→
    重连 → 入口必须可用。这一组是判定组：漏了「泄洪时通知页面」的版本，
    s_scan_live 永远停在 1，重连后入口一律回「上一轮还没跑完」。

所有等待都是「轮询某个可观测状态直到超时」，失败时打印最后观测到的状态。

    python tools/v_r1_scan.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db

G_SCANBAD = 1000 + 8 * 100 + 27        # 1827：全服清错误物品
IDC_CONN = 104
IDC_DISC = 105
IDC_CHLIST = 113
LVM_GETITEMCOUNT = 0x1004
SCAN_TITLE = "全服清错误物品"
STUCK = "上一轮还没跑完"
CONFIRM = "扫描全服所有角色"
# 扫描的第一条 SELECT 的特征片段（db_scan_bad 里那句）。轮询那条是
# 「c.charac_name,c.job,...」，两者的前几列不一样，用这个片段能把它们分开。
SCAN_SQL = "c.charac_name,c.m_id"
fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def top_windows(main, cls=None, title=None):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        owner = u.GetWindow(h, 4)          # GW_OWNER
        if cls and c.value != cls:
            return True
        if title and t.value != title:
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
        for h, c, t in top_windows(main, cls="#32770"):
            return h, dlg_text(h)
        time.sleep(0.05)
    return None, ""


def click_dlg_item(h, cid):
    b = u.GetDlgItem(h, cid)
    if b:
        u.PostMessageW(b, 0x00F5, 0, 0)      # BM_CLICK
    return bool(b)


def wait_msgbox_gone(main, timeout=8.0):
    """MessageBox 是模态的：它还在的时候，主窗口是 disabled 状态，
    往页面按钮 PostMessage 会直接丢掉。所以每次点完确定/取消都要等它真的消失。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not top_windows(main, cls="#32770"):
            return True
        time.sleep(0.05)
    return False


def open_scan_dialog(m):
    """点入口 → 确认 → 等结果窗出现（扫完才弹）。返回 (窗口句柄, 入口提示文本)。"""
    u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
    b, t = wait_msgbox(m, 8.0)
    if CONFIRM not in t:
        if b:
            click_dlg_item(b, 2)
            wait_msgbox_gone(m)
        return None, t
    click_dlg_item(b, 1)                                    # IDOK
    wait_msgbox_gone(m)
    return wait_scan_dialog(m), t


def wait_scan_dialog(m, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = top_windows(m, cls="DnfDlg", title=SCAN_TITLE)
        if w:
            return w[0][0]
        time.sleep(0.1)
    return None


def probe_entry(m, timeout=8.0):
    """点入口，返回第一个弹框的文本（不点确定，所以不会真的开扫）。

    注意它只看得见 on_scanbad 那道闸（s_scan_busy）：确认框是在 scan_start
    之前弹的。要看 s_scan_live 那道闸必须真的点确定，见 try_start_scan。"""
    u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
    b, t = wait_msgbox(m, timeout)
    if b:
        click_dlg_item(b, 2)                                # IDCANCEL
        wait_msgbox_gone(m)
    return t


def try_start_scan(m):
    """真的走一遍「点入口 → 确定」，返回 (扫描是否跑起来, 被拒原因, 结果窗)。

    两道闸都要经过：on_scanbad 看 s_scan_busy（拒绝时弹「上一轮还没跑完。」），
    scan_start 看 s_scan_live（拒绝时弹「上一轮还没跑完，等它结束再扫。」）。
    跑起来的话结果窗会在扫完之后弹出来 —— 用它当「真的扫了」的判据。"""
    u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
    b, t = wait_msgbox(m, 8.0)
    if not b:
        return False, t, None
    if CONFIRM not in t:                     # 入口自己就拒了
        click_dlg_item(b, 2)
        wait_msgbox_gone(m)
        return False, t, None
    click_dlg_item(b, 1)                     # 确定
    wait_msgbox_gone(m)
    b2, t2 = wait_msgbox(m, 2.0)             # scan_start 拒绝时会紧接着再弹一个
    if b2:
        click_dlg_item(b2, 2)
        wait_msgbox_gone(m)
        return False, t2, None
    return True, "", wait_scan_dialog(m)


def wait_connected(m, timeout=25.0):
    """连接完成的判据用左栏角色列表的行数：它是连库作业收尾时才填的。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        n = u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), LVM_GETITEMCOUNT, 0, 0)
        if n > 0:
            return True
        time.sleep(0.2)
    return False


class TableLock:
    """第二条连接上的 LOCK TABLES ... WRITE。被锁住的表，另一条连接的读会停在
    「Waiting for table metadata lock」，于是「扫描正在跑」变成我们能控制的状态。"""

    def __init__(self, tables):
        self.tables = tables
        self.c = None

    def __enter__(self):
        self.c = db()
        cur = self.c.cursor()
        cur.execute("LOCK TABLES " + ",".join(t + " WRITE" for t in self.tables))
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
    """(id, state, info)，用来看「谁还卡在库上」。开独立连接，别和锁的会话混。"""
    c = db()
    try:
        cur = c.cursor()
        cur.execute("SELECT ID,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                    "WHERE COMMAND='Query'")
        return cur.fetchall()
    finally:
        c.close()


def wait_online(m, timeout=15.0):
    """等入口不再回「请先连接数据库」：g.db_ok 是连库作业在 app_tick 里落地的，
    比左栏列表晚一拍，不能拿列表行数当「能点了」的证据。"""
    t0 = time.time()
    t = ""
    while time.time() - t0 < timeout:
        t = probe_entry(m)
        if "请先连接数据库" not in t:
            return True, t
        time.sleep(0.3)
    return False, t


def wait_blocked(marker, timeout=15.0):
    """等某条语句卡在表锁上（STATE 含 metadata lock 且 INFO 命中 marker）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, st, info in processlist():
            if marker in info and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, ""


def main():
    print("EXE =", EXE)
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), 0x00F5, 0, 0)     # 连接
        if not ok("连接数据库（左栏出了角色）", wait_connected(m)):
            return 1
        switch(m, 8)
        online, t = wait_online(m)                 # 列表有行了还不等于 g.db_ok 已经落地
        if not ok("连接就绪（入口不再回「请先连接数据库」）", online, t):
            return 1

        if not ok("「全服清错误物品」按钮存在", bool(ctl(m, G_SCANBAD))):
            return 1

        # ---------------- A 组：正常扫描 → 关窗 → 还能再扫（对照）
        dlg, _ = open_scan_dialog(m)
        if not ok("A：扫描结果窗出现", bool(dlg)):
            return 1
        print("   A 列表行数 =", u.SendMessageW(u.GetDlgItem(dlg, 6000), LVM_GETITEMCOUNT, 0, 0))
        u.SendMessageW(dlg, 0x0010, 0, 0)                      # WM_CLOSE
        t0 = time.time()
        while time.time() - t0 < 5 and top_windows(m, cls="DnfDlg", title=SCAN_TITLE):
            time.sleep(0.1)
        ok("A：扫描窗已关闭", not top_windows(m, cls="DnfDlg", title=SCAN_TITLE))
        started, why, dlg2 = try_start_scan(m)
        ok("A：关窗后还能再扫（对照组）", started, why)
        if dlg2:
            u.SendMessageW(dlg2, 0x0010, 0, 0)
            time.sleep(0.5)

        # ---------------- B 组：作业真的还活着时关窗
        dlg, _ = open_scan_dialog(m)
        if not ok("B：扫描结果窗再次出现", bool(dlg)):
            return 1
        with TableLock(["taiwan_cain_2nd.inventory"]):
            u.SendMessageW(u.GetDlgItem(dlg, 6003), 0x00F5, 0, 0)   # 重新扫描（同步）
            # 扫描的第一个容器就是 inventory：锁住它，作业一定停在 live=1，
            # 不再取决于库有多大、机器多快
            blocked, st = wait_blocked("taiwan_cain_2nd.inventory")
            ok("B：扫描作业真的卡在库上（不是靠 sleep 猜的）", blocked, st)
            u.SendMessageW(dlg, 0x0010, 0, 0)                  # WM_CLOSE
            t0 = time.time()
            while time.time() - t0 < 5 and top_windows(m, cls="DnfDlg", title=SCAN_TITLE):
                time.sleep(0.1)
            ok("B：扫描窗已关闭", not top_windows(m, cls="DnfDlg", title=SCAN_TITLE))
            # 真的按一遍「确定」：on_scanbad 的 busy 闸在 WM_DESTROY 里放掉了，
            # 所以确认框会照弹；紧接着 scan_start 的 live 闸必须把它拦下来 ——
            # 因为作业确实还卡在库上，一秒钟都没跑完。
            started, why, _ = try_start_scan(m)
            print("   B 作业还活着时确定扫描：started=%s why=%r" % (started, why))
            ok("B：作业没结束 → 扫描被拒，提示「上一轮还没跑完」", (not started) and STUCK in why, why)
        # 放锁：作业收尾，闸由作业自己放下
        started, why, dlg = try_start_scan(m)
        print("   B 放锁后再确定扫描：started=%s why=%r" % (started, why))
        ok("B：作业收尾后能正常再扫", started and bool(dlg), why)
        if dlg:
            u.SendMessageW(dlg, 0x0010, 0, 0)
            time.sleep(0.5)

        # ---------------- C 组：动作进了队列，随后被断开泄洪（R3-003）
        # 先把连接弄忙：锁住 charac_info，轮询作业（每 3 秒一次）会卡在表锁上，
        # db_busy() 于是恒为真 —— 这时点扫描，app_act 只会把动作排进队列。
        with TableLock(["taiwan_cain.charac_info"]):
            busy, st = wait_blocked("taiwan_cain.charac_info")
            ok("C：连接被占住（轮询卡在表锁上）", busy, st)

            u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
            b, t = wait_msgbox(m, 8.0)
            if not ok("C：入口弹出确认框", CONFIRM in t, t):
                if b:
                    click_dlg_item(b, 2)
                    wait_msgbox_gone(m)
                return 1
            click_dlg_item(b, 1)                               # IDOK
            wait_msgbox_gone(m)
            # 判定「只是排进队列」：扫描那条 SELECT 没有出现在库上。
            # 它一旦起了线程，就会和轮询一样卡在 charac_info 上，一眼就能看到。
            time.sleep(1.0)
            queued = not any(SCAN_SQL in info for _, _, info in processlist())
            ok("C：扫描只是排队，没有起线程", queued)
            ok("C：排队期间不弹结果窗", not top_windows(m, cls="DnfDlg", title=SCAN_TITLE))
            ok("C：进程还活着（无上下文的动作排队不该崩）", p.poll() is None, "poll=%s" % (p.poll(),))

            # 点断开。app_disconnect 会先等作业退出（最长 30 秒），所以要让 UI 线程
            # 先进到 job_stop 里，再放锁 —— 放锁就是 with 块结束，这里用 sleep 把
            # 两个动作的先后固定下来，而不是拿它当同步点。
            u.PostMessageW(u.GetDlgItem(m, IDC_DISC), 0x00F5, 0, 0)
            time.sleep(0.6)
        # 等到真的断开：入口回「请先连接数据库」
        t0 = time.time()
        off = ""
        while time.time() - t0 < 20:
            off = probe_entry(m)
            if "请先连接数据库" in off:
                break
            time.sleep(0.3)
        ok("C：已断开（入口回「请先连接数据库」）", "请先连接数据库" in off, off)

        # 重连：断开时那份「排队中的扫描」被丢掉了，闸必须跟着放下
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), 0x00F5, 0, 0)
        ok("C：重连成功", wait_connected(m))
        switch(m, 8)
        # 队列没被丢掉的话，排着的扫描会在连接关掉之后照样起线程，扫一空跑完再
        # 弹一个结果窗 —— 那就不是「什么都没扫」而是「扫了，0 条」，更误导。
        stray = top_windows(m, cls="DnfDlg", title=SCAN_TITLE)
        ok("C：重连后没有遗留的扫描结果窗", not stray)
        if stray:
            u.SendMessageW(stray[0][0], 0x0010, 0, 0)
            time.sleep(0.5)
        online, t = wait_online(m)
        ok("C：重连后连接就绪", online, t)
        started, why, dlg = try_start_scan(m)
        print("   C 重连后确定扫描：started=%s why=%r" % (started, why))
        ok("C：泄洪后闸已放下（扫描能正常开始）", started and bool(dlg), why)
        if dlg:
            u.SendMessageW(dlg, 0x0010, 0, 0)
            time.sleep(0.5)

        ok("进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
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
