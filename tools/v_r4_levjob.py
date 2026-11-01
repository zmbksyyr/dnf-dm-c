# -*- coding: utf-8 -*-
"""R4-01 的另外半边：改等级 / 改职业被拒后，快照是不是也退回库里的真值。

R4-01 的修复点是 `w_load` 里那句 `db_read_charac(&g.db, c->c.no, &c->c)`，它一次
带回 name / job / grow / lev 四个字段（src/db.c:730-751），而 `g.cur = g_load_ctx.c`
（src/app.c:1810）是整体赋值。所以按代码推，「改等级/改职业失败后停在假值」应该和
改名是同一个病 —— 走的是同一条分支：act.c:127 `if (mine && sync_after(c->kind))
app_reload_later();`，sync_after 对 K_LEV / K_JOB 同样返回 1（act.c:55）。

审查者实测只验了 name，说 lev/job/grow「查得回来」。本脚本用真实运行判这件事：
db_load_data（db.c:655）的 SELECT 里既没有 charac_name 也没有 lev/job/grow_type，
整个载入过程不碰 `c->c` —— 所以旧版这四个字段应该一起回不来。跑旧 exe 对照即可。

怎么让「改等级」必然失败：
  lev/job 都是 NOT NULL 的 tinyint，而 sql_mode 只有 NO_ENGINE_SUBSTITUTION（无
  STRICT），越界会被静默截断而不是报错；UI 又夹在 1..lev_max(=85)。所以造不出
  「写不进去」的输入。改用 BEFORE UPDATE 触发器 SIGNAL：不动一行数据，只让
  UPDATE charac_info SET lev=77 / job=21 以错误 1644 结束。触发器在 finally 里
  无条件 DROP。

三个判定：
  A) 改职业失败 → GM 页职业下拉必须退回库里的真值（CB_GETCURSEL == 6）。
     下拉是 gm_reload 里 `ui_combo_set(c_job, g.cur.job)` 填的（ui_char.c:226），
     直接读得到，是最硬的观测量。
  B) 改等级失败 → 库里 lev 仍是 85。
  C) 改等级失败 → 再用同一个值试第二次：
       回滚成功 → g.cur.lev 已经是库里的 85 ≠ 77，`if (v == g.cur.lev) return;`
                  不挡，第二次真的会再发一次 UPDATE 并被拒 → 日志里再出现一行
                  「设置等级 失败」
       回滚是死的 → g.cur.lev 还是假的 77，第二次被上面那句直接 return，
                  一条日志都不会有
     等级框在 UI 上恒填 0（ui_char.c:224 那句注释），读不出当前值，所以走这条
     探针。它判的是 g.cur.lev 有没有被纠正，正是修复点。

旧版必红：A 与 C 两条。
数据纪律：只建/删两个触发器，不动任何行；finally 里断言 charac_info 1 行、
AUTO_INCREMENT=9919、laoxx 的 (name,HEX) 与 lev/job/grow 与改前一致。

    python tools/v_r4_levjob.py                      # 新版（应 0 失败）
    DNF_GM_BUILD=<旧版目录> python tools/v_r4_levjob.py   # 4df0a0c（应红）
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS, WM_COMMAND = 0x00F5, 0x0008, 0x0111
CB_GETCOUNT, CB_SETCURSEL, CB_GETCURSEL = 0x0146, 0x014E, 0x0147
CBN_SELENDOK = 9
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_LEV = 1000 + 8 * 100 + 2        # 1802
G_JOB = 1000 + 8 * 100 + 3        # 1803

# 哨兵值必须落在合法区间里，否则 UI 那一层就先挡掉了，根本到不了库：
#   lev 夹在 1..lev_max(85)，当前是 85 → 取 77
#   job 下拉只有 11 项（kJobList，ui_char.c:50），当前是 6「刺客」→ 取 9「黑暗武士」
BAD_LEV, BAD_JOB = 77, 9

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def snap(cur):
    cur.execute("SELECT charac_name,HEX(charac_name),lev,job,grow_type "
                "FROM taiwan_cain.charac_info WHERE charac_no=1")
    return {
        "row": cur.fetchone(),
        "n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info"),
        "ai": q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                      "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'"),
    }


TRG = [
    ("r4_blk_lev", "lev", str(BAD_LEV), "R4 拒绝改等级"),
    ("r4_blk_job", "job", str(BAD_JOB), "R4 拒绝改职业"),
]


def add_triggers(cur):
    for nm, col, val, msg in TRG:
        cur.execute("DROP TRIGGER IF EXISTS taiwan_cain.%s" % nm)
        cur.execute("CREATE TRIGGER taiwan_cain.%s BEFORE UPDATE ON taiwan_cain.charac_info "
                    "FOR EACH ROW BEGIN IF NEW.%s = %s THEN SIGNAL SQLSTATE '45000' "
                    "SET MESSAGE_TEXT = '%s'; END IF; END" % (nm, col, val, msg))


def del_triggers(cur):
    for nm, _, _, _ in TRG:
        cur.execute("DROP TRIGGER IF EXISTS taiwan_cain.%s" % nm)


def left_triggers(cur):
    cur.execute("SELECT TRIGGER_NAME FROM information_schema.TRIGGERS "
                "WHERE TRIGGER_SCHEMA='taiwan_cain'")
    return [r[0] for r in cur.fetchall()]


# ---------------- 日志 ----------------
def logfile():
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def log_off():
    p = logfile()
    return (p, os.path.getsize(p)) if p else (None, 0)


def log_since(off):
    p = _LOGP[0]
    if not p or not os.path.exists(p):
        return ""
    with open(p, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


_LOGP = [None]


def hit_log(off, needle):
    return [l for l in log_since(off).splitlines() if all(k in l for k in needle)]


def wait_log(off, needle, timeout=25.0):
    """等一行同时含 needle 里所有关键字的新日志，返回该行。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = hit_log(off, needle)
        if h:
            return h[0]
        time.sleep(0.3)
    return ""


# ---------------- 控件 ----------------
def combo_sel(main, cid):
    h = ctl(main, cid)
    return u.SendMessageW(h, CB_GETCURSEL, 0, 0) if h else -1


def combo_pick(main, cid, idx):
    """选中下拉第 idx 项并按 CBN_SELENDOK 通知页面 = 提交（ui_char.c:780）。"""
    h = ctl(main, cid)
    if not h:
        return False
    parent = u.GetParent(h)
    u.SendMessageW(h, CB_SETCURSEL, idx, 0)
    u.PostMessageW(parent, WM_COMMAND, (CBN_SELENDOK << 16) | (cid & 0xFFFF), h)
    return True


def set_box(main, s):
    h = ctl(main, G_LEV)
    if not h:
        return False
    set_text(h, s)
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)      # 失焦 = 提交
    return True


def status(main):
    return txt(u.GetDlgItem(main, IDC_STATUS), 512)


def wait_loaded(main, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if "已载入" in status(main):
            return True
        time.sleep(0.3)
    return False


def main():
    print("EXE =", EXE)
    c = db()
    cur = c.cursor()
    p = None
    before = snap(cur)
    print("改前：charac_info %s 行 / AUTO_INCREMENT=%s / 1 号 =%r"
          % (before["n"], before["ai"], before["row"]))
    cur_lev, cur_job, cur_grow = before["row"][2], before["row"][3], before["row"][4]
    try:
        # 同上：不认死 AI=9919。它会被别的一次性脚本顶上去，认死等于第二次跑永久假红；
        # 该钉的是「跑完回到跑之前」，那是第 8 步 after["ai"] == before["ai"] 的事。
        ok("0 起点：charac_info 只有 laoxx 一行", before["n"] == 1,
           "%s 行 / AI=%s" % (before["n"], before["ai"]))
        ok("0 起点：laoxx 的 lev/job/grow 已知（%s/%s/%s）" % (cur_lev, cur_job, cur_grow),
           cur_lev is not None and BAD_LEV != cur_lev and BAD_JOB != cur_job,
           "哨兵 lev=%s job=%s" % (BAD_LEV, BAD_JOB))
        ok("0 起点：库里没有遗留触发器", left_triggers(cur) == [],
           "%r" % (left_triggers(cur),))
        if fails:
            print("\n起点不满足，停止")
            return 1

        add_triggers(cur)
        ok("1 两个 BEFORE UPDATE 触发器已建（lev=%s / job=%s 会被 SIGNAL 拒绝）"
           % (BAD_LEV, BAD_JOB),
           sorted(left_triggers(cur)) == sorted([t[0] for t in TRG]),
           "%r" % (left_triggers(cur),))

        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        t0 = time.time()
        n = 0
        while time.time() - t0 < 30 and n != 1:
            n = u.SendMessageW(lv, 0x1004, 0, 0)
            time.sleep(0.3)
        ok("2 角色列表 1 行", n == 1, "%s 行" % n)
        click_at(lv, 40, 12, 1.0)
        switch(m, 8)
        time.sleep(1.0)
        ok("2 已载入 laoxx", wait_loaded(m), status(m))
        njob = u.SendMessageW(ctl(m, G_JOB), CB_GETCOUNT, 0, 0)
        ok("2 职业下拉项数够（%d 项，哨兵 %d 在范围内）" % (njob, BAD_JOB), njob > BAD_JOB,
           "%s 项" % njob)

        # ---------- A) 改职业被拒 → 下拉必须退回库里的真值 ----------
        sel0 = combo_sel(m, G_JOB)
        ok("3 改前职业下拉 = 库里的 job=%s" % cur_job, sel0 == cur_job, "选中 %s" % sel0)
        off = log_off()[1]
        _LOGP[0] = logfile()
        ok("3 把职业下拉选到 %s 提交" % BAD_JOB, combo_pick(m, G_JOB, BAD_JOB))
        hit = wait_log(off, ["设置职业", "失败"])
        print("   日志：%r" % (hit,))
        ok("3 改职业被拒（日志有「设置职业…失败」）", bool(hit),
           hit or log_since(off)[-200:])
        ok("3 拒绝理由来自触发器（1644/R4）", "R4" in hit or "1644" in hit, hit)

        seen = []
        t0 = time.time()
        while time.time() - t0 < 25:
            s = combo_sel(m, G_JOB)
            if not seen or seen[-1] != s:
                seen.append(s)
            if s == cur_job:
                break
            time.sleep(0.4)
        print("   职业下拉变化轨迹：%r" % (seen,))
        ok("4 改职业被拒后下拉退回库里的真值 %s" % cur_job, combo_sel(m, G_JOB) == cur_job,
           "选中 %s" % combo_sel(m, G_JOB))
        ok("4 下拉没有停在没写进库的假值 %s 上" % BAD_JOB, combo_sel(m, G_JOB) != BAD_JOB,
           "选中 %s" % combo_sel(m, G_JOB))
        cur.execute("SELECT lev,job,grow_type FROM taiwan_cain.charac_info WHERE charac_no=1")
        got = cur.fetchone()
        ok("4 库里 job/grow 没被改", got == (cur_lev, cur_job, cur_grow), "%r" % (got,))

        # ---------- B) 改等级被拒 ----------
        ok("5 等级框填 %s 并提交（触发器会拒）" % BAD_LEV, set_box(m, BAD_LEV))
        hit = wait_log(off, ["设置等级", "失败"])
        print("   日志：%r" % (hit,))
        ok("5 改等级被拒（日志有「设置等级…失败」）", bool(hit),
           hit or log_since(off)[-200:])
        ok("5 库里 lev 没被改", q1(cur, "SELECT lev FROM taiwan_cain.charac_info "
                                  "WHERE charac_no=1") == cur_lev,
           "%s" % q1(cur, "SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=1"))
        wait_loaded(m)
        # 日志是攒在内存里每秒刷一次的（app.c:1922），等它落盘再取新起点，
        # 否则第 5 步那行可能还没写进文件，第 6 步会拿旧行当新证据。
        time.sleep(1.8)

        # ---------- C) 第二次同值写入探针：判 g.cur.lev 有没有被纠正 ----------
        off2 = log_off()[1]
        n_before = len(hit_log(off2, ["设置等级", "失败"]))
        info("6 第二次把等级框填成同一个 %s" % BAD_LEV,
             "回滚成功 → 会再发一次 UPDATE 再被拒；回滚是死的 → 一句日志都不会有")
        ok("6 探针起点干净（新起点之后还没有「设置等级 失败」）", n_before == 0,
           "%s 行" % n_before)
        ok("6 等级框填 %s 并提交" % BAD_LEV, set_box(m, BAD_LEV))
        hit2 = wait_log(off2, ["设置等级", "失败"], timeout=14.0)
        print("   日志：%r" % (hit2,))
        ok("6 第二次用同一个值仍然真的发了写库（说明 g.cur.lev 已退回库里的 %s）"
           % cur_lev, bool(hit2), hit2 or "(日志里没有，说明被 v == g.cur.lev 挡住了)")
        ok("6 库里 lev 仍然是 %s" % cur_lev,
           q1(cur, "SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=1") == cur_lev)
        ok("7 进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        try:
            del_triggers(cur)
        except Exception as e:
            print("!! 触发器清理失败：%s" % e)
        after = snap(cur)
        print("改后：charac_info %s 行 / AUTO_INCREMENT=%s / 1 号 =%r"
              % (after["n"], after["ai"], after["row"]))
        ok("8 触发器已全部删除", left_triggers(cur) == [], "%r" % (left_triggers(cur),))
        ok("8 charac_info 回到 1 行", after["n"] == before["n"], "%s 行" % after["n"])
        ok("8 AUTO_INCREMENT 未变", after["ai"] == before["ai"],
           "%s → %s" % (before["ai"], after["ai"]))
        ok("8 laoxx 的 (名字,HEX,lev,job,grow) 与改前一致", after["row"] == before["row"],
           "%r vs %r" % (after["row"], before["row"]))
        c.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
