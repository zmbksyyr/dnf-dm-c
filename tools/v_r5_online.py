# -*- coding: utf-8 -*-
"""R5-005 定向验证：物品页的四条写路径共用一道在线闸，而且闸要在弹框之前。

修复点（src/ui_item.c）：抽出 `item_writable()` —— 先查 g.char_ok / g.cur.online，
再逐页问 app_page_busy(sub)（0..5，清错误物品一次会动到多个容器，只判当前页挡不住它），
四条写路径共用它；并且检查放在 item_edit 弹框 / app_confirm 确认框**之前**，
别让人填完十几个字段才告诉他白填。旧版 item_edit_sel / item_clear_sel 压根没有这道闸。

**判据**（违反一条就 sys.exit(1)）
  A. 在线时点「编辑」：先出来的必须是一句「在线」的提示，不是物品编辑框
  B. 在线时点「清空所选」：先出来的必须是同一句提示，不是「清空第 N 格？」的确认框
  C. 这两下都把对话框用 WM_CLOSE 关掉（不走确定），inventory 那一行一个字节都没变
  D. 还原在线状态后，login_account_3 那一行与改前逐字节一致

**在线怎么造**：这一端的「在线」是「账号在线 **且** 正在玩这个角色」两个条件一起
（db.c:278 `acct==1 && playing==1`），所以要两张表配合：

    UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=1          -- 账号在线
    往 taiwan_game_event.event_1306_account_reward 插一行 (m_id,charac_no)       -- 正在玩

少做第二步的话 playing 会是 NULL → online=0，界面照样弹物品编辑框 —— 那时得出的
「没有被拦下」是自己的构造没做完，不是被测程序的行为。两张表都留快照：账号那行按
原值写回；event_1306 这张原本是 0 行，插的那行删掉、行数回去。
两张表都留快照：账号那行按原值写回；event_1306 这张原本是 0 行，插的删掉行数回去。

跑法：
    python tools/v_r5_online.py
    python tools/v_r5_online.py --old
"""
import hashlib
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_OLD = ("--old" in sys.argv)
if _OLD:
    os.environ["DNF_GM_BUILD"] = os.path.abspath(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "_r5_old", "_build", "DNF_GM_C"))

import ctypes                                                             # noqa: E402
from ctypes import wintypes as wt                                         # noqa: E402
from pages import u, EXE, wait_main, switch, ctl, txt, db as open_db      # noqa: E402

BM_CLICK, LVM_GETITEMCOUNT, LVM_GETHEADER = 0x00F5, 0x1004, 0x101F
WM_CLOSE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0010, 0x0201, 0x0202
IDC_CONN, IDC_CHLIST = 104, 113
PAGE_ITEM = 0
I_SUB, I_LIST, I_EDIT, I_CLR, I_CLRALL = 1001, 1002, 1003, 1004, 1005

fails = []


def ok(tag, got, want):
    good = got == want
    print("%s %s（得到 %r，期望 %r）" % ("OK  " if good else "FAIL", tag, got, want))
    if not good:
        fails.append(tag)
    return good


def ok_true(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def sha(rows):
    return hashlib.sha256(repr(sorted(map(repr, rows))).encode("utf-8", "replace")).hexdigest()[:16]


def wait_box(timeout=8.0):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value == "#32770":
            out.append(h)
        return True

    t0 = time.time()
    while time.time() - t0 < timeout:
        out = []
        u.EnumWindows(cb(each), 0)
        if out:
            time.sleep(0.25)
            return out[0]
        time.sleep(0.1)
    return None


def box_text(h):
    parts = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(c, lp):
        cls = ctypes.create_unicode_buffer(32)
        u.GetClassNameW(c, cls, 32)
        if cls.value in ("Static", "Button", "Edit"):
            t = txt(c, 512)
            if t:
                parts.append(t)
        return True

    u.EnumChildWindows(ctypes.c_void_p(h), cb(each), 0)
    return " / ".join(parts)


def close_box(h):
    u.PostMessageW(ctypes.c_void_p(h), WM_CLOSE, 0, 0)
    time.sleep(0.8)


def select_row(m, idx=1):
    h = ctl(m, I_LIST)
    if not h:
        return False
    hdr = ctypes.c_void_p(u.SendMessageW(h, LVM_GETHEADER, 0, 0))
    rl, rh = wt.RECT(), wt.RECT()
    u.GetWindowRect(h, ctypes.byref(rl))
    u.GetWindowRect(hdr, ctypes.byref(rh))
    y = (rh.bottom - rl.top) + 8 + idx * 20
    lp = (int(y) << 16) | 40
    u.PostMessageW(h, WM_LBUTTONDOWN, 1, lp)
    u.PostMessageW(h, WM_LBUTTONUP, 0, lp)
    time.sleep(0.6)
    return True


def main():
    print("EXE =", EXE)
    db = open_db()
    cur = db.cursor()
    cur.execute("SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=1")
    mid = cur.fetchone()[0]
    cur.execute("SELECT * FROM taiwan_login.login_account_3 WHERE m_id=%d" % mid)
    row_before = cur.fetchall()
    before_sha = sha(row_before)
    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
    inv_n = cur.fetchone()[0]
    cur.execute("SELECT * FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
    inv_before = sha(cur.fetchall())
    print("角色 charac_no=1 的 m_id = %s / login_account_3 改前 %d 行 sha=%s"
          % (mid, len(row_before), before_sha))
    if not row_before:
        print("FAIL 账号表里没有这一行，在线状态无从制造")
        return 1

    cols = None
    cur.execute("SHOW COLUMNS FROM taiwan_login.login_account_3")
    for i, c in enumerate([r[0] for r in cur.fetchall()]):
        if c == "login_status":
            cols = i
    ok("在 login_account_3 里找到了 login_status 列", cols is not None, True)

    cur.execute("SELECT COUNT(*) FROM taiwan_game_event.event_1306_account_reward")
    play_n_before = cur.fetchone()[0]

    p = None
    try:
        cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%d" % mid)
        cur.execute("SELECT * FROM taiwan_login.login_account_3 WHERE m_id=%d" % mid)
        ok("已把账号置为在线（login_status=1）", sha(cur.fetchall()) != before_sha, True)
        cur.execute("INSERT INTO taiwan_game_event.event_1306_account_reward "
                    "(m_id,charac_no,occ_date) VALUES (%d,1,NOW())" % mid)
        cur.execute("SELECT COUNT(*) FROM taiwan_game_event.event_1306_account_reward")
        play_n_after = cur.fetchone()[0]
        ok("已标记「正在玩这个角色」", play_n_after, play_n_before + 1)

        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(4.5)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        u.PostMessageW(lv, WM_LBUTTONDOWN, 1, (12 << 16) | 40)
        u.PostMessageW(lv, WM_LBUTTONUP, 0, (12 << 16) | 40)
        time.sleep(4.0)
        switch(m, PAGE_ITEM)
        time.sleep(2.0)
        nrows = u.SendMessageW(ctl(m, I_LIST), LVM_GETITEMCOUNT, 0, 0)
        print("  物品列表行数 =", nrows)
        ok_true("物品页已经载入", nrows > 1, "rows=%s" % nrows)
        ok_true("已选中一行（下面两个按钮要有作用对象）", select_row(m, 1))

        # A) 编辑
        u.PostMessageW(ctl(m, I_EDIT), BM_CLICK, 0, 0)
        box = wait_box()
        t = box_text(box) if box else ""
        print("  A) 弹出来的窗口：", t[:110])
        ok_true("A) 在线时点「编辑」被拦下（提示里说明在线）", box is not None and "在线" in t, t[:80])
        ok_true("A) 出来的不是物品编辑框", "耐久" not in t and "增幅值" not in t, t[:80])
        if box:
            close_box(box)

        # B) 清空所选
        select_row(m, 1)
        u.PostMessageW(ctl(m, I_CLR), BM_CLICK, 0, 0)
        box2 = wait_box()
        t2 = box_text(box2) if box2 else ""
        print("  B) 弹出来的窗口：", t2[:110])
        ok_true("B) 在线时点「清空所选」被拦下（提示里说明在线）",
                box2 is not None and "在线" in t2, t2[:80])
        ok_true("B) 出来的不是「清空第 N 格」的确认框", "清空第" not in t2, t2[:80])
        if box2:
            close_box(box2)

        time.sleep(1.5)
        cur.execute("SELECT * FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
        inv_after = sha(cur.fetchall())
        cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
        ok("C) inventory 行数没变", cur.fetchone()[0], inv_n)
        ok("C) inventory 整行字节与试之前一致", inv_after, inv_before)
        ok_true("C) 全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        try:
            cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=%d WHERE m_id=%d"
                        % (row_before[0][cols], mid))
        except Exception:
            pass
        try:
            cur.execute("DELETE FROM taiwan_game_event.event_1306_account_reward "
                        "WHERE m_id=%d AND charac_no=1" % mid)
        except Exception:
            pass
        db.close()

    db = open_db()
    cur = db.cursor()
    cur.execute("SELECT * FROM taiwan_login.login_account_3 WHERE m_id=%d" % mid)
    after = cur.fetchall()
    ok("D) 还原后 login_account_3 那一行与改前逐字节一致", sha(after), before_sha)
    print("  改后：", after)
    cur.execute("SELECT COUNT(*) FROM taiwan_game_event.event_1306_account_reward")
    ok("D) event_1306_account_reward 行数回到改前", cur.fetchone()[0], play_n_before)
    db.close()
    print("\n合计失败 %d 项" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
