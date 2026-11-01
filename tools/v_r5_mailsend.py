# -*- coding: utf-8 -*-
"""R5-003 定向验证：写路径改完邮件之后，列表必须回到「未加载」。

修复点（src/ui_mail.c）：M_DEL / M_DELALL / M_DELALLS / M_SEND 四条写路径统一挂到
app_act 的返回值上 —— `if (app_act(...)) { g.mails.n = 0; g.lists_ready &= ~L_MAILS; }`。
旧版前两条不看返回值就清（app_act 返回 0「未连库 / 队列满 / 启动失败」时把还在手里的
列表白清掉），后两条压根不清（发完邮件列表停在旧的那份）。

**三组断言**（违反一条就 sys.exit(1)）
  A. 发一封纯文本邮件给 laoxx
     - 库里 taiwan_cain_2nd.letter 多出一行，正文对得上（INSERT 真的发生了）
     - 日志里「发送文本邮件 完成」之后还能看到一次「读取邮件 完成」
       —— 列表清了标志才会再拉一次；页面每次动作后都会 reload，
       仍然文言文停留在旧列表的话，这一行不会出现（旧版必红）
  B. 删掉这一行还原，断言 letter 的行数与整表字节指纹回到改前
  C. 队列满（app_act 返回 0）时点「删除所选」：列表不许被白清掉
     - 先把连接占死：锁住 taiwan_login.login_account_3，背景查询卡在上面
     - 连点 GM 页「+100 复活币」把 g_pq 排满（4 槽），第五次就会是人见到的
       「已有 4 个操作在排队，这个没提交」
     - 选中邮件第 0 行 → 删除所选 → 弹确认框 → 确定 → 此时 app_act 返回 0
     - 切页再切回来触发一次 reload：新版仍停在「共 N 封邮件」，
       旧版因为列表被清了会停在「正在读取邮件…」
     - 放锁后把复活币写回原值，断言 inventory 那一行逐字节还原

跑法：
    python tools/v_r5_mailsend.py
    python tools/v_r5_mailsend.py --old
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

import ctypes                                                          # noqa: E402
from ctypes import wintypes as wt                                      # noqa: E402
from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, db as open_db  # noqa: E402

LVM_GETITEMCOUNT, LVM_GETHEADER = 0x1004, 0x101F
BM_CLICK, CB_SETCURSEL = 0x00F5, 0x014E
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
IDC_CONN, IDC_CHLIST = 104, 113
PAGE_MAIL, PAGE_GM, PAGE_BAN = 3, 8, 10
B_LOADALL = 2012
M_LIST, M_TARGET, M_TEXT, M_SEND, M_INFO, M_REFRESH = 1301, 1306, 1310, 1311, 1305, 1320
G_QADD_COIN = 1000 + 800 + 72          # 1872，kQAdd[2] = 复活币 +100
LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))
TEXT = "R5-003 定向验证邮件"

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


def logs():
    if not os.path.exists(LOG):
        return ""
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return f.read()


def mrows(m):
    h = ctl(m, M_LIST)
    return u.SendMessageW(h, LVM_GETITEMCOUNT, 0, 0) if h else -1


def minfo(m):
    return txt(ctl(m, M_INFO), 256)


def select_row(m, idx=0):
    """点列表第 idx 行。行区从列头底边往下算（按固定偏移猜会点进列头里）。"""
    h = ctl(m, M_LIST)
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
    time.sleep(0.4)
    return True


def top_windows(cls="#32770"):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value == cls:
            out.append(h)
        return True

    u.EnumWindows(cb(each), 0)
    return out


def letter_state(cur):
    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.letter")
    n = cur.fetchone()[0]
    cur.execute("SELECT * FROM taiwan_cain_2nd.letter ORDER BY letter_id")
    rows = cur.fetchall()
    sha = hashlib.sha256(repr(list(rows)).encode("utf-8", "replace")).hexdigest()[:16]
    return n, sha, [r[0] for r in rows]


def inv_row(cur):
    cur.execute("SELECT money,coin FROM taiwan_cain_2nd.inventory WHERE charac_no=1")
    return cur.fetchone()


def main():
    print("EXE =", EXE)
    db = open_db()
    cur = db.cursor()
    db2 = open_db()
    cur2 = db2.cursor()

    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                "WHERE receive_charac_no=1 AND delete_flag=0")
    nm = cur.fetchone()[0]
    before_n, before_sha, before_ids = letter_state(cur)
    coin_before = inv_row(cur)
    print("邮件列表行数 = %d / letter 行数 = %d / inventory(money,coin) = %r"
          % (nm, before_n, coin_before))

    before_log = len(logs())
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(4.5)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        u.PostMessageW(lv, WM_LBUTTONDOWN, 1, (12 << 16) | 40)
        u.PostMessageW(lv, WM_LBUTTONUP, 0, (12 << 16) | 40)
        time.sleep(3.5)
        switch(m, PAGE_MAIL)
        t0 = time.time()
        while time.time() - t0 < 15 and mrows(m) != nm:
            time.sleep(0.2)
        ok("起点：邮件列表行数", mrows(m), nm)

        # ── A) 发一封纯文本邮件 ────────────────────────────────────
        u.SendMessageW(ctl(m, M_TARGET), CB_SETCURSEL, 0, 0)      # 目标 = 当前角色
        b = ctypes.create_unicode_buffer(TEXT)
        u.SendMessageW(ctl(m, M_TEXT), 0x000C, 0, ctypes.addressof(b))
        time.sleep(0.3)
        u.PostMessageW(ctl(m, M_SEND), BM_CLICK, 0, 0)
        t0 = time.time()
        while time.time() - t0 < 20:
            n2, _, _ = letter_state(cur)
            if n2 == before_n + 1:
                break
            time.sleep(0.3)
        n2, _, _ = letter_state(cur)
        ok("A) letter 多出一行", n2, before_n + 1)
        cur.execute("SELECT send_charac_name,letter_text FROM taiwan_cain_2nd.letter "
                    "ORDER BY letter_id DESC LIMIT 1")
        r = cur.fetchone()
        ok("A) 落库内容对得上", (r[0], r[1]) if r else None, ("GM", TEXT))
        # 日志是攒着落的（1s 一批），发完别急着读那一Logs: 等它沉下来再看重拉有没有发生
        time.sleep(2.5)

        tail = logs()[before_log:]
        lines = tail.split("\n")
        i_send = max([i for i, l in enumerate(lines) if "发送文本邮件 完成" in l] or [-1])
        i_read = max([i for i, l in enumerate(lines) if "读取邮件 完成" in l] or [-1])
        print("  「发送文本邮件 完成」@%d / 「读取邮件 完成」@%d" % (i_send, i_read))
        ok_true("A) 发送之后列表被置回未加载（之后又拉了一次）",
                i_send >= 0 and i_read > i_send,
                "i_send=%s i_read=%s" % (i_send, i_read))

        # ── B) 删掉这一行，还原 ────────────────────────────────────
        cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id>%d"
                    % (max(before_ids) if before_ids else 0))
        rn, rsha, _ = letter_state(cur)
        ok("B) 还原后 letter 行数回到改前", rn, before_n)
        ok("B) 还原后 letter 整表字节指纹与改前一致", rsha, before_sha)

        # ── C) app_act 返回 0 时不许白清列表 ───────────────────────
        switch(m, PAGE_MAIL)
        t0 = time.time()
        while time.time() - t0 < 15 and "共 %d 封邮件" % nm not in minfo(m):
            time.sleep(0.2)
        ok("C) 起点：列表已加载", ("共 %d 封邮件" % nm) in minfo(m), True)
        ok_true("C) 列表第 0 行已选中（下面要有删除对象）", select_row(m, 0))

        # 占满队列要用「读」类动作：GM 页那颗「+100」先落进它自己的页面队列
        # s_gmq，压不到 app 层这个 g_pq 上；封停页的「加载全服」是直接 app_act 的。
        cur.execute("SELECT postal_id FROM taiwan_cain_2nd.postal "
                    "WHERE receive_charac_no=1 AND delete_flag=0 "
                    "ORDER BY postal_id DESC LIMIT 1")
        target_pid = cur.fetchone()[0]        # 列表第 0 行就是它（ORDER BY postal_id DESC）
        cur2.execute("SELECT CONNECTION_ID()")
        mine = {cur2.fetchone()[0]}
        cur.execute("SELECT CONNECTION_ID()")
        mine.add(cur.fetchone()[0])
        cur2.execute("LOCK TABLES taiwan_login.login_account_3 WRITE")
        time.sleep(1.0)                       # 等背景查询占住这条连接
        switch(m, PAGE_BAN)
        for _ in range(8):                    # 排满 4 槽；多出来的会被拒
            u.PostMessageW(ctl(m, B_LOADALL), BM_CLICK, 0, 0)
            time.sleep(0.12)
        cur2.execute("SHOW PROCESSLIST")
        busy = [r[0] for r in cur2.fetchall() if r[0] not in mine and r[4] == "Query"]
        ok_true("C) 连接确实被占住了（后面要靠它让队列排满）", bool(busy), str(busy[:4]))

        switch(m, PAGE_MAIL)
        select_row(m, 0)
        u.PostMessageW(ctl(m, 1302), BM_CLICK, 0, 0)   # M_DEL 删除所选
        box = None
        t0 = time.time()
        while time.time() - t0 < 6:
            w = top_windows()
            if w:
                box = w[0]
                break
            time.sleep(0.1)
        ok_true("C) 弹出了删除确认框（说明选中行是真的）", box is not None)
        if box:
            u.PostMessageW(u.GetDlgItem(box, 1), BM_CLICK, 0, 0)   # 确定
        time.sleep(1.0)
        switch(m, PAGE_GM)
        switch(m, PAGE_MAIL)
        time.sleep(0.8)
        info = minfo(m)
        ok_true("C) 队列满时不白清列表（仍停在「共 N 封邮件」）",
                ("共 %d 封邮件" % nm) in info, info)
        ok("C) 列表行数也没变", mrows(m), nm)

        cur2.execute("UNLOCK TABLES")
        time.sleep(4.0)
        # 队列真排满的话，这次删除根本没被提交 —— 这一行就验这件事
        cur.execute("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%d"
                    % target_pid)
        dflag = cur.fetchone()[0]
        print("  目标邮件 postal_id=%d 的 delete_flag = %s（1 = 删除真的排进去并执行了）"
              % (target_pid, dflag))
        ok("C) 这次删除没有被提交（队列确实是满的，app_act 返回 0）", dflag, 0)
        if dflag != 0:
            cur.execute("UPDATE taiwan_cain_2nd.postal SET delete_flag=0 WHERE postal_id=%d"
                        % target_pid)
            print("  已把 delete_flag 改回 0 还原")
        ok_true("C) 全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            cur2.execute("UNLOCK TABLES")
        except Exception:
            pass
        # 兜底：万一上面没删干净
        if before_ids:
            cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id>%d" % max(before_ids))
        else:
            cur.execute("DELETE FROM taiwan_cain_2nd.letter")
        try:
            cur.execute("UPDATE taiwan_cain_2nd.inventory SET money=%d,coin=%d WHERE charac_no=1"
                        % (coin_before[0], coin_before[1]))
        except Exception:
            pass
        db.close()
        db2.close()
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass

    print("\n合计失败 %d 项" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
