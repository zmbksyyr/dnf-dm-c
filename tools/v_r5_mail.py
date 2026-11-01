# -*- coding: utf-8 -*-
"""R5-001 定向验证：邮件列表的查询失败了，就不能被当成「已加载」。

修复点（src/ui_mail.c + src/act.c）：查询从 ui_mail.c 自己的 mail_work 迁进 act.c
的 K_MAILLIST，走和 K_BANLIST / K_EVLIST 一样的双缓冲 —— 作业写 s_mails_next，
成功后由 char_work 标脏，act_apply_lists() 在 job_poll 之后搬到 g.mails 才置 L_MAILS。

**怎么造「查询失败」而不是「查询很慢」**
  第二条连接 `LOCK TABLES taiwan_cain_2nd.postal WRITE` → db_list_mails 的 SELECT
  停在 metadata lock 上（可控、可观测：PROCESSLIST 里 State 写着 waiting）→
  `SHOW PROCESSLIST` 找到这条语句，`KILL QUERY` 把它打掉 → select1 收到
  1317 Query execution was interrupted → db_list_mails return 0。
  选它不选 RENAME TABLE：故意把用户表藏掉，一旦脚本中途出异常就是真实的数据事故，
  而 LOCK+KILL 最多让这条语句失败。

**断言**（违反一条就 sys.exit(1)）
  1. 正常：进邮件页，列表行数 == 库里 receive_charac_no=1 且 delete_flag=0 的行数
  2. 失败期间：行数不塌成 0，提示停在「正在读取邮件…」而不是变成「共 0 封邮件」
     —— 旧版在 mail_work 的第一句就 `g.lists_ready |= L_MAILS`，失败之后 UI 会拿
     空列表当「已加载」
  3. 放锁之后自己恢复：行数回到库行数（“重试分支下次 reload 会再拉”）
  4. 并发：读取卡住期间连切 10 次页，进程不崩、行数不掉到 0（这是双缓冲要防的
     边读边画的 use-after-free）

跑法（默认打新版；`--old` 只是把 exe 指到 git archive 95745cf 另编的那份）：
    python tools/v_r5_mail.py
    python tools/v_r5_mail.py --old
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_OLD = ("--old" in sys.argv)
_OLD_BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "..", "_r5_old", "_build", "DNF_GM_C")
if _OLD:
    os.environ["DNF_GM_BUILD"] = os.path.abspath(_OLD_BUILD)

from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, db as open_db  # noqa: E402

LVM_GETITEMCOUNT, BM_CLICK, CB_GETCOUNT = 0x1004, 0x00F5, 0x0146
IDC_CONN, IDC_CHLIST = 104, 113
PAGE_MAIL = 3
M_LIST, M_INFO, M_DEL, M_REFRESH = 1301, 1305, 1302, 1320
LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))
INFIX_SELECT = "SELECT postal_id"          # db_list_mails 的语句前缀，PROCESSLIST 里认它
FAIL_LOG = "读取邮件 失败"                  # app.c:1844 "%s 失败：%s"

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


def logpos():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def lognew(pos):
    """从上次的字节位置往后读日志。"""
    if not os.path.exists(LOG):
        return "", pos
    with open(LOG, encoding="utf-8", errors="replace") as f:
        f.seek(pos)
        t = f.read()
        return t, f.tell()


def mrows(m):
    h = ctl(m, M_LIST)
    return u.SendMessageW(h, LVM_GETITEMCOUNT, 0, 0) if h else -1


def minfo(m):
    return txt(ctl(m, M_INFO), 256)


def kill_blocked(cur2, prefix, deadline=12.0):
    """把 app 那条卡在 metadata lock 上的目标语句打掉。返回杀掉了几条。"""
    killed = 0
    t0 = time.time()
    while time.time() - t0 < deadline:
        cur2.execute("SHOW PROCESSLIST")
        hit = [r[0] for r in cur2.fetchall()
               if r[7] and str(r[7]).startswith(prefix) and r[4] == "Query"]
        for pid in hit:
            cur2.execute("KILL QUERY %d" % pid)
            killed += 1
        if killed:
            break
        time.sleep(0.2)
    return killed


def main():
    print("EXE =", EXE)
    db = open_db()
    cur = db.cursor()
    db2 = open_db()                      # 专门用来上锁/查进程的连接
    cur2 = db2.cursor()

    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                "WHERE receive_charac_no=1 AND delete_flag=0")
    n = cur.fetchone()[0]
    print("库里 laoxx 的邮件行数 =", n)
    if n == 0:
        print("FAIL 这份库里没有邮件，用例判据（行数不塌成 0）无从谈起")
        return 1

    p = None
    pos0 = logpos()
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(4.5)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        u.PostMessageW(lv, 0x0201, 1, (12 << 16) | 40)
        u.PostMessageW(lv, 0x0202, 0, (12 << 16) | 40)
        time.sleep(3.5)

        switch(m, PAGE_MAIL)
        t0, got = time.time(), -1
        while time.time() - t0 < 15:
            got = mrows(m)
            if got == n:
                break
            time.sleep(0.2)
        ok("1) 进邮件页列表行数 == 库行数", got, n)
        ok_true("1) 列表非空", got > 0, "rows=%s" % got)
        ok_true("1) 提示是「共 N 封邮件」", ("共 %d 封邮件" % n) in minfo(m), minfo(m))

        # ── 2) 查询失败：列表不许被当成「已加载」 ────────────────────
        cur2.execute("LOCK TABLES taiwan_cain_2nd.postal WRITE")
        pos1 = logpos()
        u.PostMessageW(ctl(m, M_REFRESH), BM_CLICK, 0, 0)   # 刷新 → 重新发起查询
        killed = kill_blocked(cur2, INFIX_SELECT)
        print("打掉的语句条数 =", killed)
        t0, seen_fail = time.time(), False
        while time.time() - t0 < 15:
            nt, pos1 = lognew(pos1)
            if FAIL_LOG in nt:
                seen_fail = True
                line = [l for l in nt.split("\n") if FAIL_LOG in l][-1]
                print("  应用侧：", line.strip()[:110])
                break
            time.sleep(0.3)
        ok_true("2) 应用侧确实报了「读取邮件 失败」", seen_fail)
        time.sleep(1.5)                       # 让重试再转两圈
        ok("2) 失败后列表没塌成 0 行", mrows(m) == n, True)
        ok_true("2) 提示停在「正在读取邮件…」", "正在读取邮件" in minfo(m), minfo(m))
        ok_true("2) 提示没有变成「共 0 封邮件」", "共 0 封邮件" not in minfo(m), minfo(m))

        # ── 3) 并发：读作业卡在库上时来回切页 ───────────────────────
        seen = []
        for r in range(10):
            switch(m, PAGE_MAIL)
            seen.append(mrows(m))
            switch(m, 4)                      # 宠物页
            time.sleep(0.05)
        switch(m, PAGE_MAIL)
        time.sleep(0.5)
        seen.append(mrows(m))
        ok_true("3) 读取期间反复切页进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
        ok_true("3) 切页期间列表没掉到 0 行", min(seen) == n,
                "min=%s max=%s 样本 %d" % (min(seen), max(seen), len(seen)))

        # ── 4) 放锁后自己恢复 ───────────────────────────────────────
        cur2.execute("UNLOCK TABLES")
        t0, got = time.time(), -1
        while time.time() - t0 < 25:
            got = mrows(m)
            if got == n and "共 %d 封邮件" % n in minfo(m):
                break
            time.sleep(0.3)
        ok("4) 放锁后行数回到库行数", got, n)
        ok_true("4) 放锁后提示恢复成「共 N 封邮件」", ("共 %d 封邮件" % n) in minfo(m), minfo(m))
        ok_true("4) 全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            cur2.execute("UNLOCK TABLES")
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
