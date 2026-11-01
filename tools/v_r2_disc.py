# -*- coding: utf-8 -*-
"""R2-003 定向验证：断开时把 g_pq 里排队的动作丢掉。

场景：连接被一个跑得很慢的作业占着（用另一条库连接 LOCK TABLES 把扫描的 SELECT
拖住，模拟「写 6 个容器要几百毫秒」那种窗口）→ 此时点几个 GM 一键动作，它们会进
g_pq 排队 → 立刻点「断开」。

断言：
  · 状态栏第一段是「已断开数据库」
  · 日志里没有新增「xxx 失败：未连接」刷屏
  · 进程不崩，断开后「连接」可用、「断开」置灰
  · 重新连上以后，那些排队的动作没有被拿到新连接上执行（满疲劳没生效）

    python tools/v_r2_disc.py
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, start, switch, ctl, txt, click_at, enabled, db as open_db

BM_CLICK = 0x00F5
IDC_CONN, IDC_DISC, IDC_CHLIST = 104, 105, 113
G_SCANBAD, G_FATIGUE = 1827, 1819
SB_GETTEXTW = 0x040D          # SB_GETTEXTW = WM_USER+13（0x40B 是 SB_SETTEXTW）
SCAN_TITLE = "全服清错误物品"

k32 = ctypes.windll.kernel32
k32.OpenProcess.restype = wt.HANDLE
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


# ---------------- 跨进程读状态栏 ----------------
# SB_GETTEXTW 是 WM_USER 以上的消息，系统不做封送，直接传本进程指针会让对方写到
# 自己的地址空间里去。所以在目标进程里开一块内存，让它写那儿，再读回来。
def status_text(proc, main, part):
    h = k32.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400, False, proc.pid)
    if not h:
        return None
    try:
        addr = k32.VirtualAllocEx(h, None, 4096, 0x3000, 0x04)
        if not addr:
            return None
        hwnd = u.GetDlgItem(main, 191)
        n = u.SendMessageW(hwnd, SB_GETTEXTW, part, addr) & 0xFFFF
        buf = ctypes.create_string_buffer(4096)
        got = ctypes.c_size_t(0)
        k32.ReadProcessMemory(h, addr, buf, 4096, ctypes.byref(got))
        k32.VirtualFreeEx(h, addr, 0, 0x8000)
        return buf.raw[: n * 2].decode("utf-16-le", "replace")
    finally:
        k32.CloseHandle(h)


def main_hwnd(proc):
    return u.FindWindowW("DnfGmMain", None)


# ---------------- 弹窗 / 日志 ----------------
def boxes():
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value == "#32770" and u.IsWindowVisible(h):
            out.append(h)
        return True

    u.EnumWindows(cb(each), 0)
    return out


def box_text(h):
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


def click_box(cid):
    b = boxes()
    if not b:
        return False
    btn = u.GetDlgItem(b[0], cid)
    if btn:
        u.PostMessageW(btn, BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 8:
        if not boxes():
            return True
        time.sleep(0.05)
    return False


def wait_box(timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if boxes():
            time.sleep(0.2)
            return True, box_text(boxes()[0])
        time.sleep(0.05)
    return False, ""


def log_path():
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    return os.path.join(d, sorted(os.listdir(d))[-1])


def log_lines():
    with open(log_path(), encoding="utf-8", errors="replace") as f:
        return f.readlines()


def main():
    lk = open_db()
    lc = lk.cursor()
    dbc = open_db()
    cur = dbc.cursor()
    p = None
    try:
        cur.execute("SELECT fatigue,max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=1")
        fat0 = cur.fetchone()
        print("改前 fatigue/max_fatigue =", fat0)

        p, main = start(page=8)
        click_at(u.GetDlgItem(main, IDC_CHLIST), 40, 12, 4.0)
        switch(main, 8)
        time.sleep(1.5)
        ok("连接可用、断开置灰（起点已连库）",
           (not enabled(u.GetDlgItem(main, IDC_CONN))) and enabled(u.GetDlgItem(main, IDC_DISC)),
           "conn=%s disc=%s" % (enabled(u.GetDlgItem(main, IDC_CONN)), enabled(u.GetDlgItem(main, IDC_DISC))))
        st0 = status_text(p, main, 0)
        print("状态栏[0] =", repr(st0))

        base_lines = log_lines()
        n_fail0 = sum(1 for l in base_lines if "失败：" in l)
        n_unconn0 = sum(1 for l in base_lines if "未连接" in l)

        # 1) 把库拖住，让扫描作业堵在 SELECT 上（模拟一次耗时写库）
        lc.execute("LOCK TABLES taiwan_cain_2nd.inventory WRITE")
        u.PostMessageW(ctl(main, G_SCANBAD), BM_CLICK, 0, 0)
        got, t = wait_box(8.0)
        ok("扫描入口弹出确认框", got, t[:40])
        click_box(1)
        time.sleep(1.0)                          # 作业已提交，正堵在 SELECT 上

        # 2) 此时点两个「一键」动作 —— db_busy 为真，它们只能排队
        for cid, nm in ((G_FATIGUE, "满疲劳"), (1815, "无限负重")):
            u.SendMessageW(ctl(main, cid), BM_CLICK, 0, 0)
            time.sleep(0.15)
        print("已点 满疲劳 / 无限负重（连接被占，应进 g_pq 排队）")
        st_queue = status_text(p, main, 0)
        print("排队后状态栏[0] =", repr(st_queue))

        # 3) 立刻断开：先把消息投出去，再放开库锁，让 job_stop 里的等待能返回
        u.PostMessageW(u.GetDlgItem(main, IDC_DISC), BM_CLICK, 0, 0)
        time.sleep(0.35)
        lc.execute("UNLOCK TABLES")

        # 4) 等断开完成（连接按钮重新可用）
        t0 = time.time()
        while time.time() - t0 < 25:
            if enabled(u.GetDlgItem(main, IDC_CONN)):
                break
            time.sleep(0.1)
        time.sleep(1.5)

        ok("断开后「连接」可用、「断开」置灰",
           enabled(u.GetDlgItem(main, IDC_CONN)) and not enabled(u.GetDlgItem(main, IDC_DISC)),
           "conn=%s disc=%s" % (enabled(u.GetDlgItem(main, IDC_CONN)), enabled(u.GetDlgItem(main, IDC_DISC))))
        st1 = status_text(p, main, 0)
        print("断开后状态栏[0] =", repr(st1))
        ok("状态栏是「已断开数据库」", bool(st1) and st1.endswith("已断开数据库"), "%r" % (st1,))
        st1b = status_text(p, main, 1)
        print("断开后状态栏[1] =", repr(st1b))
        ok("状态栏第二段写着「库未连接」", st1b and st1b.startswith("库未连接"), "%r" % (st1b,))

        # 5) 日志里不能冒出「失败：未连接」刷屏
        time.sleep(1.5)
        now_lines = log_lines()
        new = now_lines[len(base_lines):]
        spam = [l.strip() for l in new if "未连接" in l]
        allfail = [l.strip() for l in new if "失败：" in l]
        print("断开后新增日志 %d 行，其中「未连接」%d 行，「失败：」%d 行"
              % (len(new), len(spam), len(allfail)))
        for l in new[-8:]:
            print("   log:", l.rstrip())
        ok("没有「xxx 失败：未连接」刷屏", not spam, str(spam[:5]))
        ok("断开过程中没有失败日志", not allfail, str(allfail[:5]))
        ok("进程没有崩", p.poll() is None, "poll=%s" % (p.poll(),))

        # 6) 重新连上：排队的动作不能拿到新连接上偷偷执行
        u.PostMessageW(u.GetDlgItem(main, IDC_CONN), BM_CLICK, 0, 0)
        t0 = time.time()
        while time.time() - t0 < 25:
            if enabled(u.GetDlgItem(main, IDC_DISC)):
                break
            time.sleep(0.2)
        time.sleep(4.0)
        cur.execute("SELECT fatigue,max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=1")
        fat1 = cur.fetchone()
        print("重连后 fatigue/max_fatigue =", fat1)
        ok("排队期间点的「满疲劳」没有被补执行", fat1 == fat0, "%s → %s" % (fat0, fat1))
        ok("重连后进程仍活着", p.poll() is None)

    finally:
        try:
            lc.execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            lk.close()
            dbc.close()
        except Exception:
            pass
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
