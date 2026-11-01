# -*- coding: utf-8 -*-
"""R3-004 / R3-007 定向验证：断开时把「还没开始的页面级意图」一起丢掉。

两条粘滞路径，症状同型：页面自己攒着一份「排队中的改动」，而 app_disconnect 只清了
app 层的 g_pq。本轮把它们收进 app_drop_queued() 的钩子（gm_drop / tab_drop）。

B 组（R3-004，GM 页写队列 s_gmq）
  对照：连接被占住（轮询卡在表锁上）→ 改金币 → 断言库里没立刻变 → 放锁 →
        断言改值补写进库（证明「编辑确实排进了 s_gmq，只等连接空闲」）。
  判定：连接被占住 → 改金币 → 断开 → 放锁 → 断言断开后状态栏不是
        「请先连接数据库」（旧版会把队列里的改值对着已关闭的连接再提交一遍）→
        重连 → 断言那个改值没有被写进库。
  旧版在这一步：s_gmq 留着 → gm_kick 弹出条目 → app_act 失败（db_ok=0）→
  状态栏「请先连接数据库」，条目被静默丢弃，界面那一格却已显示改过。

C 组（R3-007，宠物/时装表内扫描 g_tab_pending）
  连接被占住 → 点「清错误记录」（只在队列里，tab_work 不会跑）→ 断开 → 重连 →
  断言不会再弹一个「发现 0 条错误记录」的空窗。
  旧版在这一步：g_tab_pending 留着，重连后 pet_reload 拿一份 n=0 的旧结果弹窗。

    python tools/v_r3_sticky.py            # 新版
    DNF_GM_BUILD=<旧版目录> python tools/v_r3_sticky.py   # 旧版（应当红）
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS = 0x00F5, 0x0008
IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191
PAGE_GM, PAGE_PET = 8, 4
G_MONEY = 1000 + PAGE_GM * 100 + 5      # 1805
P_BAD = 1000 + PAGE_PET * 100 + 9       # 1409
TAB_TITLE = "宠物/时装表内错误记录"
LVM_GETITEMCOUNT = 0x1004
CONFIRM_SCAN = "扫描全服所有角色"

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


# ---------------- 弹窗 ----------------
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


def click_dlg_item(h, cid):
    b = u.GetDlgItem(h, cid)
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
    return bool(b)


def wait_msgbox_gone(main, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not top_windows(main, "#32770"):
            return True
        time.sleep(0.05)
    return False


def find_tab_dlg(main):
    for h, c, t in top_windows(main, "DnfDlg"):
        if t == TAB_TITLE:
            return h
    return None


def status_text(main):
    """状态栏（IDC_STATUS）第 0 段的文本。WM_GETTEXT 在 WM_USER 以下由系统编组，
    跨进程读得到真文本；SB_GETTEXT 是 WM_USER 开头，指针过不去。"""
    h = u.GetDlgItem(main, IDC_STATUS)
    return txt(h, 512) if h else ""


# ---------------- 库 ----------------
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


def wait_blocked(marker, timeout=15.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, st, inf in processlist():
            if marker in inf and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, ""


def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def money(cur):
    return q1(cur, "SELECT money FROM taiwan_cain_2nd.inventory WHERE charac_no=1")


def wait_connected(m, timeout=30.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), LVM_GETITEMCOUNT, 0, 0) > 0:
            return True
        time.sleep(0.2)
    return False


def wait_status(m, want, timeout=25.0):
    """等状态栏出现某段文字（断开/连接都是后台作业收尾时才改状态）。"""
    t0 = time.time()
    s = ""
    while time.time() - t0 < timeout:
        s = status_text(m)
        if want in s:
            return True, s
        time.sleep(0.2)
    return False, s


def click(main, cid, wait=0.6):
    h = ctl(main, cid)
    if not h:
        return False
    u.PostMessageW(h, BM_CLICK, 0, 0)
    time.sleep(wait)
    return bool(h)


def close_stray_box(main):
    """把意外的模态提示框点掉。重连之后 g.char_ok 还是 0，
    这时候去点页面上的动作会弹「请先选择角色。」—— 它是模态的，
    留着会让后面所有 SendMessage 排在同一条消息队列后面。"""
    w = top_windows(main, "#32770")
    if not w:
        return None
    t = dlg_text(w[0])
    click_dlg_item(w[0], 1)
    wait_msgbox_gone(main)
    return t


def select_char(main, cur, timeout=15.0):
    """点左栏第一行选角色（重连之后 g.char_ok 归零，必须重新选一次）。

    判据不用状态栏：选完角色后紧接着可能被「读取宠物 完成」这类状态盖掉。
    用 GM 页的名字框 —— 只有 g.char_ok 为真时 gm_reload 才会把 g.cur.name 填进去。
    """
    close_stray_box(main)
    lv = u.GetDlgItem(main, IDC_CHLIST)
    click_at(lv, 40, 12, 1.0)
    want = q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=1")
    # 必须站在 GM 页上读：pages.ctl 只在「可见的 DnfPage」里找控件
    switch(main, PAGE_GM)
    t0 = time.time()
    while time.time() - t0 < timeout:
        got = txt(ctl(main, 1801), 64)
        if got == want:
            return True
        close_stray_box(main)
        time.sleep(0.3)
    return txt(ctl(main, 1801), 64) == want


def set_money(main, v, cur, timeout=12.0):
    """GM 页金币框：填值 + 失焦 = 提交。返回 (库里最终值, 状态栏)。"""
    h = ctl(main, G_MONEY)
    if not h:
        return None, ""
    set_text(h, str(v))
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
    t0 = time.time()
    got = money(cur)
    while time.time() - t0 < timeout:
        got = money(cur)
        if got == v:
            break
        time.sleep(0.3)
    return got, status_text(main)


# ---------------- 用例 ----------------
def case_b(p, m, cur, m0):
    print("\n================ B 组：GM 页写队列 s_gmq（R3-004） ================")
    switch(m, PAGE_GM)
    time.sleep(0.6)
    ok("B0 金币框存在", bool(ctl(m, G_MONEY)), "控件 %d" % G_MONEY)

    # ---- 对照：忙的时候改，放锁后必须补写 ----
    v1 = 22222 if m0 != 22222 else 33333
    with TableLock(["taiwan_cain.charac_info"]):
        busy, st = wait_blocked("taiwan_cain.charac_info")
        ok("B1 连接被占住（轮询卡在表锁上）", busy, st)
        set_text(ctl(m, G_MONEY), str(v1))
        u.SendMessageW(ctl(m, G_MONEY), WM_KILLFOCUS, 0, 0)
        time.sleep(1.5)
        ok("B1 忙的时候改值不会立刻落库（只排队）", money(cur) == m0,
           "库里 %s（改前 %s）" % (money(cur), m0))
        info("状态栏", status_text(m))
    got = None
    t0 = time.time()
    while time.time() - t0 < 20:
        got = money(cur)
        if got == v1:
            break
        time.sleep(0.3)
    ok("B1 放锁后这份改值补写进库（证明它真的排在 s_gmq 里）", got == v1,
       "%s → %s" % (m0, got))

    # ---- 判定：忙的时候改，然后断开 ----
    v2 = 33333 if v1 != 33333 else 44444
    with TableLock(["taiwan_cain.charac_info"]):
        busy, st = wait_blocked("taiwan_cain.charac_info")
        ok("B2 连接再次被占住", busy, st)
        set_text(ctl(m, G_MONEY), str(v2))
        u.SendMessageW(ctl(m, G_MONEY), WM_KILLFOCUS, 0, 0)
        time.sleep(1.0)
        ok("B2 改值 %d 只是排队（库里仍是 %s）" % (v2, v1), money(cur) == v1,
           "库里 %s" % (money(cur),))
        u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)   # 断开
        time.sleep(0.6)                                            # 让 UI 线程进到 job_stop
    off, s = wait_status(m, "已断开数据库", 30.0)
    ok("B3 断开完成", off, s)
    time.sleep(1.5)                    # 留几个 tick：旧版会在这几拍里把队列发给死连接
    s = status_text(m)
    print("   断开后的状态栏 = %r" % (s,))
    ok("B4 断开后没有「对着已关闭的连接再提交一次」的痕迹", "请先连接数据库" not in s, s)

    u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
    ok("B5 重连成功", wait_connected(m))
    ok("B5 重连后重新选角色（新会话的 g.char_ok 归零）", select_char(m, cur), repr(status_text(m)))
    switch(m, PAGE_GM)
    time.sleep(6.0)                    # 给旧版足够时间把残留队列打进新连接
    now = money(cur)
    print("   重连 6 秒后库里金币 = %s（改前 %s，被丢弃的改值 %s）" % (now, m0, v2))
    ok("B6 排队的改值没有被写进库（跨会话泄漏）", now in (m0, v1),
       "库里 %s" % (now,))
    ok("B6 具体来说不是被丢弃的那个 %s" % v2, now != v2, "库里 %s" % (now,))
    return now


def case_c(p, m, cur):
    print("\n================ C 组：表内扫描 g_tab_pending（R3-007） ================")
    switch(m, PAGE_PET)
    time.sleep(1.5)
    ok("C0 宠物页「清错误记录」按钮存在", bool(ctl(m, P_BAD)), "控件 %d" % P_BAD)

    with TableLock(["taiwan_cain.charac_info"]):
        busy, st = wait_blocked("taiwan_cain.charac_info")
        ok("C1 连接被占住（扫描动作只能进队列）", busy, st)
        click(m, P_BAD, 0.0)
        time.sleep(1.2)
        early = find_tab_dlg(m)
        info("点击后立刻出现的扫描窗", "有（待查）" if early else "无（符合预期：动作还没跑）")
        if early:
            u.SendMessageW(early, 0x0010, 0, 0)          # 有就先关掉，继续测重连路径
            time.sleep(0.4)
        u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)
        time.sleep(0.6)
    off, s = wait_status(m, "已断开数据库", 30.0)
    ok("C2 断开完成", off, s)
    ok("C2 断开时扫描动作被丢弃（没有起线程、没结果窗）", find_tab_dlg(m) is None)

    u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
    ok("C3 重连成功", wait_connected(m))
    ok("C3 重连后重新选角色", select_char(m, cur), repr(status_text(m)))
    # 切回来触发一次 reload：残留的 pending 会让它在这里弹「发现 0 条错误记录」
    switch(m, PAGE_PET)
    time.sleep(1.0)
    switch(m, PAGE_GM)
    time.sleep(0.5)
    switch(m, PAGE_PET)
    popped = None
    t0 = time.time()
    while time.time() - t0 < 10:
        d = find_tab_dlg(m)
        if d:
            popped = dlg_text(d)
            break
        time.sleep(0.3)
    ok("C4 重连后没有弹「发现 0 条错误记录」的空窗", popped is None,
       (popped or "")[:100])
    if popped:
        d = find_tab_dlg(m)
        if d:
            u.SendMessageW(d, 0x0010, 0, 0)
        time.sleep(0.5)

    # 正对照：现在连接是空闲的，点一次必须真的能扫出结果窗（别把「不弹」当成功能没了）
    switch(m, PAGE_PET)
    time.sleep(0.5)
    click(m, P_BAD, 0.5)
    d = None
    t0 = time.time()
    while time.time() - t0 < 20:
        d = find_tab_dlg(m)
        if d:
            break
        time.sleep(0.3)
    ok("C5 正对照：连接空闲时点扫描能弹出结果窗（扫描路径本身没坏）", d is not None)
    if d:
        print("   结果窗文本 = %r" % (dlg_text(d)[:120],))
        u.SendMessageW(d, 0x0010, 0, 0)
        time.sleep(0.5)


def main():
    print("EXE =", EXE)
    c = db()
    cur = c.cursor()
    p = None
    m0 = None
    try:
        m0 = money(cur)
        print("起点：金币 =", m0)
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        if not ok("A0 连接数据库", wait_connected(m)):
            return 1
        click_at(u.GetDlgItem(m, IDC_CHLIST), 40, 12, 3.5)
        print("   角色列表选中行 =", u.SendMessageW(u.GetDlgItem(m, IDC_CHLIST), 0x100C, -1, 0x0002))
        print("   状态栏 =", repr(status_text(m)))
        ok("A0 状态栏读得到（后面几条断言靠它）", bool(status_text(m)), repr(status_text(m)))

        final = case_b(p, m, cur, m0)
        case_c(p, m, cur)

        # 还原金币（必须先切回 GM 页：隐藏页上的编辑框没拿过焦点，
        #  WM_KILLFOCUS 不会转成 EN_KILLFOCUS，那条路等于没提交）
        if final != m0 and final is not None:
            print("\n还原金币 %s → %s" % (final, m0))
            switch(m, PAGE_GM)
            time.sleep(0.8)
            select_char(m, cur)
            close_stray_box(m)
            h = ctl(m, G_MONEY)
            if h:
                set_text(h, str(m0))
                u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
                t0 = time.time()
                while time.time() - t0 < 15 and money(cur) != m0:
                    time.sleep(0.3)
        ok("Z 金币已还原", money(cur) == m0, "改前 %s 现在 %s" % (m0, money(cur)))
        ok("Z 进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        try:
            if m0 is not None and money(cur) != m0:
                cur.execute("UPDATE taiwan_cain_2nd.inventory SET money=%s WHERE charac_no=1", (m0,))
                print("!! finally 兜底把金币改回 %s（现在 %s）" % (m0, money(cur)))
        except Exception as e:
            print("!! finally 兜底失败：%s" % e)
        c.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
