# -*- coding: utf-8 -*-
"""R5-001 补修（fix2）定向验证：邮件查询还在飞的时候切角色，回来的那份不许换进列表。

补修点在 src/act.c：邮件查询按 `kc.cno` 记下 `s_mails_cno`，`act_apply_lists()` 交换
暂存前先比对 `s_mails_cno != g.cur.no`（外加 `!g.char_ok`）—— 对不上就 `vec_clear`
作废暂存，**并且不置 L_MAILS**。没有这段的话：

  1. 用户在 laoxx 的邮件页 → mail_reload 提交 K_MAILLIST（cno=1）
  2. 作业线程开跑 db_list_mails，SELECT 卡在库上
  3. 查询飞行中用户切到 B
  4. 作业回来，UI 线程无条件交换 → g.mails 装的是 laoxx 的信，界面挂在 B 名下
  5. 此时点「删除邮件」→ 按 postal_id 删掉 laoxx 的信（不可回滚）

**为什么现有的 v_r5_mail.py 打不到这段**：它全程不切角色，新旧行为一致（含补修的
二进制一次都没被这段时序测过）。本脚本补的就是「查询在飞 → 切角色」这个序列。

**怎么让查询「真的在飞」**（照 v_r1_scan / v_r3_jobstop 的规矩，不靠 sleep 抢时序）
  第二条连接 `LOCK TABLES taiwan_cain_2nd.postal WRITE` → db_list_mails 的 SELECT 停在
  metadata lock 上 → 断言 `SHOW PROCESSLIST` 里真有这么一条：
    * STATE 含 "metadata lock"  → 它不是「已经跑完了」，是真的在等
    * INFO 前缀 "SELECT postal_id" 且含 "receive_charac_no=1"
      → 这条在飞的语句查的正是 laoxx（后面切走的也是它）
  放锁的时机由脚本掌握：切角色完成（状态栏出现「已载入 <临时角色>」）之后才 UNLOCK，
  「切角色发生在查询返回之前」因此是断言出来的，不是赌出来的。

**怎么切角色**
  左栏角色列表（IDC_CHLIST）按 charac_no DESC 排，所以临时角色（9918）在第 0 行、
  laoxx（1）在第 1 行。行高不写死：点完读状态栏确认到底选中了谁，没中就换下一个 y
  （探针实测 y=12 命中第 0 行、y=55 命中第 1 行，脚本保留这两个为首选）。

**断言**（违反一条就 sys.exit(1)）
  1. 起点：库里只有 laoxx、临时号不存在、laoxx 有邮件、临时号没有邮件
  2. 选中 laoxx 进邮件页 → 行数 == 库行数 N，提示「laoxx · 共 N 封邮件」
  3. 查询在飞：PROCESSLIST 里那条 SELECT 卡在 metadata lock 上，且查的是 cno=1
  4. 切角色发生在查询返回之前：点完临时角色那一行，状态栏已经指向它（载入排在
     这条连接后面，见 app.c:269 的 db_busy），而那条查 laoxx 的 SELECT 还在锁上
  5. 放锁后收敛：临时角色载入完成，邮件列表**不是 laoxx 的** —— 行数 != N、
     提示不含「共 N 封邮件」、提示是「<临时角色> · 共 0 封邮件」
     旧版（95745cf / 9c6c36f^）在这里必红：行数 == N，别人的信挂在新角色名下
  6. 反向对照：切回 laoxx 刷新 → 行数回到 N（补修没有把列表做成「永久空」）
  7. 全程进程没崩；laoxx 的邮件在库里一封没少
  8. 清理：删掉临时角色，charac_info 回到 1 行、inventory 行数还原、charac_view.info
     字节还原（md5）、AUTO_INCREMENT 还原、laoxx 名字字节还原、postal 行数还原

数据纪律：只 INSERT 两行（charac_info / inventory 各一行，编号 9918 取在自增计数器
之下，插入不推 AUTO_INCREMENT），finally 里删除并断言还原。

**旧版必红（三份二进制的对照结论，实测）**
  * `--nofix2`（9c6c36f 把这段补修原样退回后另编）：**红** —— 列表出现 laoxx 的 5 封，
    提示「切号验证号 · 共 5 封邮件」，且日志里没有第二次「读取邮件 完成」（说明它
    没有为新角色重拉，那份错角色的数据一直挂着）。这条才是补修真正挡住的那一帧。
  * `--old`（`git archive 9c6c36f^`，即 95745cf）：**这份也是绿**，但它绿得有道理、
    不能拿来当「补修前必红」的证据 —— 那个版本在作业线程里 `db_list_mails(&g.db,
    &g.cur, &g.mails, err)` 直接写 g.mails，并且在**作业一开始就**置 L_MAILS；而
    app_select_char 会把 `g.lists_ready` 清零（app.c:893），于是切完角色 L_MAILS 是
    0，新角色自己的查询随后重跑一遍、把 g.mails 覆写成自己的 0 封 —— 终态看着是对的，
    代价是作业线程直接写 UI 线程正在遍历的 g.mails（R5-001 的 use-after-free）。
    换句话说：9c6c36f^ 没有「双缓冲 + job_poll 后交换」这个结构，也就没有「交换时
    该不该收下这份数据」这个问题；补修要挡的那一帧是 R5 引入双缓冲之后才存在的。

跑法：
    python tools/v_r5_mailswap.py            # 打 _build/DNF_GM_C 里那份（HEAD，含补修）
    python tools/v_r5_mailswap.py --nofix2   # 打「退回补修」那份（必须红）
    python tools/v_r5_mailswap.py --old      # 打 9c6c36f^ 那份（会绿，理由见上）
"""
import hashlib
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_OLD = ("--old" in sys.argv)
_NOFIX2 = ("--nofix2" in sys.argv)
_UP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..")
_OLD_BUILD = os.path.join(_UP, "_r5swap_old", "_build", "DNF_GM_C")          # 9c6c36f^
_NOFIX2_BUILD = os.path.join(_UP, "_r5swap_nofix2", "_build", "DNF_GM_C")    # 9c6c36f 退回补修
if _OLD:
    os.environ["DNF_GM_BUILD"] = os.path.abspath(_OLD_BUILD)
elif _NOFIX2:
    os.environ["DNF_GM_BUILD"] = os.path.abspath(_NOFIX2_BUILD)

from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, db as open_db  # noqa: E402

LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))
DONE_LOG = "读取邮件 完成"          # app.c:1857 "%s 完成"，邮件查询作业成功时记一条

BM_CLICK = 0x00F5
LVM_GETITEMCOUNT = 0x1004
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
PAGE_MAIL, PAGE_GM = 3, 8
M_LIST, M_INFO, M_REFRESH = 1301, 1305, 1320
G_NAME = 1000 + PAGE_GM * 100 + 1      # 1801：GM 页名字框（g.cur.name 的投影）

SRC_NO = 1                     # laoxx
TMP_NO = 9918                  # 临时角色号：取在自增计数器之下，插入不推计数器
TMP_NAME = "切号验证号"
# 左栏行坐标：探针实测 y=12 命中第 0 行、y=55 命中第 1 行；行高随 DPI 变，
# 所以点完必须读状态栏确认选中了谁，没中就接着试下一个。
Y_ROW0 = (12, 33)
Y_ROW1 = (55, 76, 33)

INFIX_SELECT = "SELECT postal_id"        # db_list_mails 的语句前缀
INFIX_CNO1 = "receive_charac_no=1"       # 这条在飞的语句查的正是 laoxx

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


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


# ---------------- 库 ----------------
def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def cols(cur, dbn, tb):
    cur.execute("SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s ORDER BY ORDINAL_POSITION", (dbn, tb))
    return [r[0] for r in cur.fetchall()]


def copy_row(cur, dbn, tb, key, src_no, new_no, name=None):
    """按列名整行复制（主键换成 new_no，可选换名字）。"""
    cs = cols(cur, dbn, tb)
    sel = []
    for c in cs:
        if c == key:
            sel.append(str(new_no))
        elif name is not None and c == "charac_name":
            sel.append("'%s'" % name)
        else:
            sel.append("`%s`" % c)
    cur.execute("INSERT INTO `%s`.`%s` (%s) SELECT %s FROM `%s`.`%s` WHERE `%s`=%%s"
                % (dbn, tb, ",".join("`%s`" % c for c in cs), ",".join(sel), dbn, tb, key),
                (src_no,))


def cv_md5(cur):
    """charac_view.info 是客户端画角色列表用的那份字节，改名/建号都不该动它。"""
    r = None
    cur.execute("SELECT `info` FROM taiwan_cain.charac_view WHERE `m_id`=1")
    row = cur.fetchone()
    if row:
        r = row[0]
    return hashlib.md5(r if r else b"").hexdigest()[:12]


def snapshot(cur):
    return {
        "info_n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info"),
        "inv_n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.inventory"),
        "ai": q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                      "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'"),
        "src": None,
        "tmp_n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                    (TMP_NO,)),
        "postal_src": q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                              "WHERE receive_charac_no=%s AND delete_flag=0", (SRC_NO,)),
        "cv": cv_md5(cur),
    }


# ---------------- GUI ----------------
def status(m):
    return txt(u.GetDlgItem(m, IDC_STATUS), 512)


def logpos():
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def lognew(pos):
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


def click_row(m, lv, y):
    lp = (int(y) << 16) | 40
    u.PostMessageW(lv, 0x0201, 1, lp)
    u.PostMessageW(lv, 0x0202, 0, lp)


def wait_loaded(m, name, timeout=10.0, step=0.05):
    """载入完成的判据是状态栏那句「已载入 <名字>」：它是 job_load 成功、快照换代之后
    才写的。

    轮询要密（50ms）：状态栏只有一行，紧接着的作业（邮件查询、下一次载入）一完成
    就把这句话盖掉，慢轮询会整段错过——实测那个窗口只有一个 tick。
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        if ("已载入 " + name) in status(m):
            return True
        time.sleep(step)
    return False


def select_char(m, lv, name, ys):
    """点左栏某一行选中角色，点完读状态栏确认。返回命中的 y（没中返回 None）。"""
    for y in ys:
        click_row(m, lv, y)
        if wait_loaded(m, name, timeout=7.0):
            return y
    return None


def wait_blocked(cur2, timeout=15.0):
    """等那条邮件 SELECT 卡在表锁上：STATE 含 metadata lock，INFO 命中前缀。
    返回 (是否卡住, state, info)。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        cur2.execute("SELECT ID,COMMAND,STATE,IFNULL(INFO,'') "
                     "FROM information_schema.PROCESSLIST")
        for _, cmd, st, inf in cur2.fetchall():
            if str(inf).startswith(INFIX_SELECT) and "metadata lock" in str(st or ""):
                return True, st, inf
        time.sleep(0.2)
    return False, "", ""


def main():
    print("EXE =", EXE)
    db = open_db()
    cur = db.cursor()
    db2 = open_db()                      # 专门上锁 / 查 PROCESSLIST 的连接
    cur2 = db2.cursor()
    p = None

    cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                "WHERE charac_no=%s", (SRC_NO,))
    src = cur.fetchone()
    before = snapshot(cur)
    before["src"] = src
    print("起点：charac_info %s 行 / inventory %s 行 / AUTO_INCREMENT=%s / laoxx=%r(%s) / "
          "laoxx 邮件 %s 封 / charac_view md5=%s"
          % (before["info_n"], before["inv_n"], before["ai"], src[0], src[1],
             before["postal_src"], before["cv"]))
    N = before["postal_src"]

    try:
        ok("0 起点：库里只有 laoxx 一个角色", before["info_n"] == 1, 1)
        ok("0 起点：临时号 %d 不存在" % TMP_NO, before["tmp_n"], 0)
        ok_true("0 起点：laoxx 叫「laoxx」且字节是 6C616F7878",
                src[0] == "laoxx" and (src[1] or "").upper() == "6C616F7878", "%r" % (src,))
        ok_true("0 起点：临时号 %d 在自增计数器 %s 之下（插入不推计数器）"
                % (TMP_NO, before["ai"]), before["ai"] > TMP_NO, "AI=%s" % before["ai"])
        ok_true("0 起点：laoxx 有邮件（否则「列表是不是他的」无从判断）", N > 0, "%s 封" % N)
        ok("0 起点：临时角色本来没有邮件",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.postal WHERE receive_charac_no=%s",
              (TMP_NO,)), 0)
        if fails:
            print("\n起点不满足，停止（库不是 brief 里那份，或临时号会顶到自增计数器）")
            return 1

        # ---------------- 1) 临时角色：可载入、但一封邮件都没有 ----------------
        copy_row(cur, "taiwan_cain", "charac_info", "charac_no", SRC_NO, TMP_NO, name=TMP_NAME)
        copy_row(cur, "taiwan_cain_2nd", "inventory", "charac_no", SRC_NO, TMP_NO)
        ok_true("1 临时角色 %d「%s」已建（charac_info + inventory 各 1 行）"
                % (TMP_NO, TMP_NAME),
                q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s",
                   (TMP_NO,)) == TMP_NAME
                and q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.inventory "
                            "WHERE charac_no=%s", (TMP_NO,)) == 1,
                "库里名字=%r"
                % q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s",
                     (TMP_NO,)))

        # ---------------- 2) 启动 + 选中 laoxx + 进邮件页 ----------------
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        t0 = time.time()
        n = 0
        while time.time() - t0 < 30 and n != 2:
            n = u.SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0)
            time.sleep(0.3)
        ok("2 左栏角色列表 2 行（临时角色 + laoxx）", n, 2)

        y1 = select_char(m, lv, "laoxx", Y_ROW1)     # 列表按 charac_no DESC：laoxx 在第 1 行
        ok_true("2 选中 laoxx（状态栏「已载入 laoxx」，点中的 y=%s）" % y1, y1 is not None,
                "状态栏=%r" % status(m))
        if y1 is None:
            print("   没能选中 laoxx，后续断言无意义，停止")
            return 1

        switch(m, PAGE_MAIL)
        t0, got = time.time(), -1
        while time.time() - t0 < 20:
            got = mrows(m)
            if got == N and ("共 %d 封邮件" % N) in minfo(m):
                break
            time.sleep(0.3)
        ok("2 进邮件页列表行数 == 库行数", got, N)
        ok_true("2 提示是「laoxx · 共 %d 封邮件」" % N,
                ("共 %d 封邮件" % N) in minfo(m) and "laoxx" in minfo(m), minfo(m))

        # ---------------- 3) 把查询拖住：让它真的在飞 ----------------
        cur2.execute("LOCK TABLES taiwan_cain_2nd.postal WRITE")
        pos0 = logpos()                                     # 从这往后数「读取邮件 完成」
        u.PostMessageW(ctl(m, M_REFRESH), BM_CLICK, 0, 0)   # 刷新 → 重新发起查询
        blocked, st, inf = wait_blocked(cur2)
        ok_true("3 邮件查询真的卡在库上（PROCESSLIST：%s）" % st, blocked, st)
        ok_true("3 这条在飞的 SELECT 查的正是 laoxx（INFO 含 %s）" % INFIX_CNO1,
                INFIX_CNO1 in str(inf), str(inf)[:120])

        # ---------------- 4) 查询在飞时切到临时角色 ----------------
        # 注意：切角色的载入作业要等这条连接空出来（db_busy() 把 job_act 算在内，
        # 见 app.c:269），所以「切完成」不可能发生在放锁之前 —— 能发生在放锁之前的
        # 是「切的请求」。这里断言的正是后者：状态栏已经指向临时角色（载入排队中），
        # 而 PROCESSLIST 里那条查 laoxx 的 SELECT 还在锁上等着。放锁之后两者才依次
        # 落地 —— 先 job_poll(job_act) 交换列表，再 job_load 换 g.cur —— 这正是补修
        # 要挡的那一帧顺序。
        click_row(m, lv, Y_ROW0[0])
        t0 = time.time()
        aimed = False
        while time.time() - t0 < 10:
            if TMP_NAME in status(m):
                aimed = True
                break
            time.sleep(0.2)
        ok_true("4 查询还在飞时就发起了切角色（状态栏指向「%s」）" % TMP_NAME, aimed,
                "状态栏=%r" % status(m))
        still, st2, _ = wait_blocked(cur2, timeout=2.0)
        ok_true("4 那一刻那条查 laoxx 的 SELECT 还没回来（仍卡在 %s）" % st2, still, st2)
        ok_true("4 此刻当前角色还没换成临时角色（载入排在查询后面）",
                ("已载入 " + TMP_NAME) not in status(m), "状态栏=%r" % status(m))

        # ---------------- 5) 放锁，看这份「上一个角色的数据」去哪了 ----------------
        cur2.execute("UNLOCK TABLES")
        seen = []
        t0 = time.time()
        while time.time() - t0 < 30:
            pair = (mrows(m), minfo(m))
            if pair not in seen:
                seen.append(pair)
            if pair[0] == 0 and ("共 0 封邮件" in pair[1]) and TMP_NAME in pair[1]:
                break
            time.sleep(0.05)
        print("   放锁后 (行数, 提示) 轨迹：%r" % (seen,))
        rows_after, info_after = mrows(m), minfo(m)
        ok_true("5 放锁后进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
        ok_true("5 邮件列表不是 laoxx 的（行数 %s != %s）" % (rows_after, N), rows_after != N,
                "rows=%s" % rows_after)
        ok_true("5 提示里没有「共 %d 封邮件」" % N, ("共 %d 封邮件" % N) not in info_after,
                info_after)
        ok("5 邮件列表是空的（临时角色本来就没信）", rows_after, 0)
        ok_true("5 提示挂在临时角色名下（「%s · 共 0 封邮件」）" % TMP_NAME,
                (TMP_NAME in info_after) and ("共 0 封邮件" in info_after), info_after)
        ok_true("5 提示里没有 laoxx 的名字", "laoxx" not in info_after, info_after)
        # 「当前角色确实换成了临时角色」另找一个稳的观测点：状态栏那句「已载入 X」
        # 只活一个 tick（紧跟着的邮件查询一完成就把它盖掉，实测 50ms 轮询也捞不到），
        # 而 GM 页的名字框是 g.cur 的直接投影，不会自己变。
        switch(m, 8)
        time.sleep(0.8)
        ok("5 当前角色已经是临时角色（GM 页名字框）", txt(ctl(m, G_NAME), 64), TMP_NAME)
        switch(m, PAGE_MAIL)
        time.sleep(1.0)
        # 补修的注释说「不置 L_MAILS，下次 reload 会再拉一份当前角色的」。这条就是
        # 那个「再拉一份」的落地证据：日志里应出现两条「读取邮件 完成」——
        # ① laoxx 那份（被作废的那次）② 临时角色自己那份。
        # 没有归属校验的版本只会有一条：它把 ① 换进了 g.mails，界面因此认为自己
        # 已就绪，再也不会为临时角色重拉。
        tail, _ = lognew(pos0)
        n_done = tail.count(DONE_LOG)
        info("5 日志里「%s」%d 条" % (DONE_LOG, n_done),
             repr([l.strip() for l in tail.split("\n") if DONE_LOG in l][:4]))
        ok_true("5 应用为当前角色重拉了一次邮件（「%s」%d 条 >= 2）" % (DONE_LOG, n_done),
                n_done >= 2)

        # ---------------- 6) 反向对照：切回 laoxx，列表要能正常重拉 ----------------
        # 补修的写法是「作废暂存 + 不置 L_MAILS」，靠下次 reload 重拉。要防的是它
        # 把列表做成永久空 —— 所以这里切回 laoxx 再刷一次，行数必须回到 N。
        # 「切回」同样只认状态栏那句「已载入 laoxx」，但它同样会被后面的作业盖掉，
        # 所以点完就刷，用「列表回到 laoxx 的 5 封」当切回成功的判据（这句提示里的
        # 角色名取自 g.cur，比状态栏那一行稳）。
        click_row(m, lv, Y_ROW1[0])
        time.sleep(0.5)
        u.PostMessageW(ctl(m, M_REFRESH), BM_CLICK, 0, 0)
        t0, got = time.time(), -1
        while time.time() - t0 < 25:
            got = mrows(m)
            if got == N and ("共 %d 封邮件" % N) in minfo(m):
                break
            time.sleep(0.3)
        ok("6 切回 laoxx 刷新后行数回到 %s（补修没把列表做成永久空）" % N, got, N)
        ok_true("6 提示挂在 laoxx 名下（「laoxx · 共 %d 封邮件」）" % N,
                ("laoxx" in minfo(m)) and ("共 %d 封邮件" % N) in minfo(m), minfo(m))

        # ---------------- 7) 数据没被动过 ----------------
        ok("7 laoxx 的邮件一封没少",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                   "WHERE receive_charac_no=%s AND delete_flag=0", (SRC_NO,)), N)
        ok_true("7 全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            cur2.execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        # ---------------- 清理：删掉临时角色，断言库回到改前 ----------------
        try:
            cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no=%s", (TMP_NO,))
            cur.execute("DELETE FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (TMP_NO,))
        except Exception as e:
            print("!! 清理失败：%s" % e)
        after = snapshot(cur)
        after["src"] = None
        cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                    "WHERE charac_no=%s", (SRC_NO,))
        after["src"] = cur.fetchone()
        print("清理后：charac_info %s 行 / inventory %s 行 / AUTO_INCREMENT=%s / laoxx=%r(%s) / "
              "laoxx 邮件 %s 封 / charac_view md5=%s"
              % (after["info_n"], after["inv_n"], after["ai"], after["src"][0],
                 after["src"][1], after["postal_src"], after["cv"]))
        ok("8 清理后 charac_info 回到 1 行", after["info_n"], 1)
        ok("8 清理后 inventory 回到 %s 行" % before["inv_n"], after["inv_n"], before["inv_n"])
        ok("8 清理后 AUTO_INCREMENT 未变（%s）" % before["ai"], after["ai"], before["ai"])
        ok("8 清理后 laoxx 的名字字节与改前一致", after["src"], before["src"])
        ok("8 清理后 charac_view.info 字节与改前一致（md5 %s）" % before["cv"],
           after["cv"], before["cv"])
        ok("8 清理后 laoxx 的邮件行数与改前一致", after["postal_src"], before["postal_src"])
        db.close()
        db2.close()

    print("\n合计失败 %d 项" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
