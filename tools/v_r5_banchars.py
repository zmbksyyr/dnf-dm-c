# -*- coding: utf-8 -*-
"""R5-002 定向验证：K_BANCHARS 查询失败时，角色清单不许被当成「已加载」。

修复点（src/act.c）：删掉 char_do 里 K_BANCHARS case 开头那句
`InterlockedOr(&s_lists_dirty, L_BANCHARS)`，只留 char_work 成功分支那一处。
旧版在「还没查」就把位标上了；db_list_ban_chars 失败时是在 vec_clear 之前 return 0，
于是空暂存照样被 act_apply_lists() 换进 g.ban_chars —— 而「加载全服」按钮已经在
点下去的那一刻把 g.ban_chars.n 清成 0 了。结果：一次查询失败封停页就显示
「全服 · 共 0 个角色」，看着像这台服务器上一个人都没有。

**失败怎么造**：第二条连接 `LOCK TABLES taiwan_login.login_account_3 WRITE`，再用
`SHOW PROCESSLIST` 把被测程序那一条连接上正在等的 SELECT 用 `KILL QUERY` 打掉，
直到日志出现「读取角色列表 失败」（1317 Query execution was interrupted）。
这张表里只有 db_list_ban_chars / db_poll_chars 会读，db_list_bans 那份封停清单不碰它，
所以「封停记录读得好好的、角色清单读不出来」这个局面是能精确造出来的。
为什么要杀而不只是等：卡在 metadata lock 上只能证明「慢」，要证明的是「查失败」。
为什么杀的目标是「它那条连接上的任何 Query」而不是某一句 SQL：轮询那条查询跟它
读同一批表，谁先占着这条连接是不可控的；日志里那句「读取角色列表 失败」才是判据。
刻意不用 RENAME TABLE：那会因为 have_table() 的兜底降级变成「查成功、少一列」，
反而测不到失败路径。

**断言**（违反一条就 sys.exit(1)）
  1. 正常：点「加载全服」后，列表行数 == 库里未删角色数，角色下拉也是同一个数
  2. 失败期间：行数不塌成 0、下拉不清空、提示停在「正在读取数据…」，
     不出现「共 0 个角色」（这一条旧版必红）
  3. 放锁之后自己恢复：行数与下拉都回到库里的数

跑法：
    python tools/v_r5_banchars.py
    python tools/v_r5_banchars.py --old     # 指向 95745cf 另编的旧 exe，期望失败
"""
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

from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, db as open_db  # noqa: E402

LVM_GETITEMCOUNT, BM_CLICK, CB_GETCOUNT = 0x1004, 0x00F5, 0x0146
IDC_CONN, IDC_CHLIST = 104, 113
PAGE_BAN = 10
B_LIST, B_CHAR, B_LOADALL, B_INFO = 2001, 2002, 2012, 2009
LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))
INFIX_SELECT = "SELECT c.charac_no"      # db_list_ban_chars 的语句前缀
FAIL_LOG = "读取角色列表 失败"            # app.c:1844 "%s 失败：%s"，act_what 就是它

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


def rows_(m):
    h = ctl(m, B_LIST)
    return u.SendMessageW(h, LVM_GETITEMCOUNT, 0, 0) if h else -1


def combo_n(m):
    h = ctl(m, B_CHAR)
    return u.SendMessageW(h, CB_GETCOUNT, 0, 0) if h else -1


def main():
    print("EXE =", EXE)
    db = open_db()
    cur = db.cursor()
    db2 = open_db()
    cur2 = db2.cursor()

    cur.execute("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE delete_flag=0")
    n = min(cur.fetchone()[0], 5000)
    print("库里未删角色数 =", n)

    before = logs()
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(4.5)
        switch(m, PAGE_BAN)
        time.sleep(1.0)

        # 1) 正常加载
        t0 = time.time()
        while time.time() - t0 < 15 and (rows_(m) != n or combo_n(m) != n):
            time.sleep(0.3)
        ok("1) 列表行数 == 库里角色数", rows_(m), n)
        ok("1) 角色下拉项数 == 库里角色数", combo_n(m), n)
        ok_true("1) 提示是「共 N 个角色」", ("共 %d 个角色" % n) in txt(ctl(m, B_INFO), 256),
                txt(ctl(m, B_INFO), 256))

        # 2) K_BANCHARS 失败：卡在 metadata lock 上的，一概打掉，直到日志认账
        cur2.execute("SELECT CONNECTION_ID()")
        mine = {cur2.fetchone()[0]}
        cur.execute("SELECT CONNECTION_ID()")
        mine.add(cur.fetchone()[0])
        cur2.execute("LOCK TABLES taiwan_login.login_account_3 WRITE")
        u.PostMessageW(ctl(m, B_LOADALL), BM_CLICK, 0, 0)
        killed, killed_what, seen_fail, line = 0, [], False, ""
        t0 = time.time()
        while time.time() - t0 < 30 and not seen_fail:
            cur2.execute("SHOW PROCESSLIST")
            hit = [(r[0], str(r[7])[:46]) for r in cur2.fetchall()
                   if r[0] not in mine and r[4] == "Query"]
            for pid, what in hit:
                cur2.execute("KILL QUERY %d" % pid)
                killed += 1
                killed_what.append(what)
            time.sleep(0.4)
            tail = logs()[len(before):]
            for l in tail.split("\n"):
                if FAIL_LOG in l:
                    seen_fail, line = True, l.strip()[:110]
                    break
        print("打掉的语句条数 =", killed)
        for w in killed_what[:4]:
            print("  被杀：", w)
        if line:
            print("  应用侧：", line)
        ok_true("2) 应用侧确实报了「读取角色列表 失败」", seen_fail)
        time.sleep(1.5)
        ok("2) 失败后列表没塌成 0 行", rows_(m) == n, True)
        ok("2) 失败后角色下拉没被清空", combo_n(m) == n, True)
        info = txt(ctl(m, B_INFO), 256)
        ok_true("2) 提示停在「正在读取数据…」", "正在读取数据" in info, info)
        ok_true("2) 提示没有变成「共 0 个角色」", "共 0 个角色" not in info, info)

        # 3) 放锁后恢复
        cur2.execute("UNLOCK TABLES")
        t0 = time.time()
        while time.time() - t0 < 25 and (rows_(m) != n or combo_n(m) != n):
            time.sleep(0.3)
        ok("3) 放锁后列表行数回到库里角色数", rows_(m), n)
        ok("3) 放锁后角色下拉项数回到库里角色数", combo_n(m), n)
        ok_true("3) 全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
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
