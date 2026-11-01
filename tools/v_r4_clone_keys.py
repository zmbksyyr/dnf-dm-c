# -*- coding: utf-8 -*-
"""R4-04 / R4-05 定向验证：无主键 / 复合主键含自增列 这两支的克隆键策略。

R4-04（Major）：无主键这一支原来是「整表跳过，复制过去只会多出一份重复行」。
  实际两张无主键表 taiwan_cain_2nd.user_items_work（ui_id,charac_no,slot,…）和
  taiwan_cain.test_sooya（m_id,charac_no,lev）都是按 charac_no 逐行归属的：换号
  插入得到的是新角色自己的行，不是重复行；没有主键反而撞不了任何键。旧策略会静默
  丢数据。现在改成：含 charac_no → CK_RECODE（换号复制），不含 → 才 CK_SKIP_TABLE。

R4-05（Minor）：复合主键含自增列这一支同样按 has_keycol 分。当前两库 0 张表命中，
  行为不变，只能验它的判定没有误伤别的分支 —— 本脚本用「判定分布」覆盖：
  CK_SKIP_COL 5 / CK_RECODE 65 / CK_SKIP_TABLE 2，与 keycheck.py 复算一致。

数据：两张表现在都是 0 行，复制 0 行看不出差别，所以**先插测试数据**：
  user_items_work 插 2 行（ui_id 88001/88002，charac_no=1）
  test_sooya      插 2 行（m_id=1，charac_no=1，lev 7/8）
克隆后断言新角色下有对应的 4 行，且 charac_no 被换成新号；然后删掉测试数据并断言还原。

断言清单：
  0) 起点：charac_info 1 行 / AI=9919 / laoxx 名字字节 / delivery 17 行全挂 charac_no=1
  1) 测试数据已插（每张 2 行，源角色的行）
  2) 克隆成功（新 charac_no）
  3) user_items_work：新角色 2 行，ui_id/it_id/slot 与源一致，charac_no 是新号
  4) test_sooya：新角色 2 行，m_id 沿用、lev 一致、charac_no 是新号
  5) 源角色的 4 行仍在，ui_id 没被改
  6) 日志判定分布：复制（换号）65 / 复制（整列跳过自增）5 / 跳过整表 2
  7) user_items_work 与 test_sooya 在日志里是「复制 N 行」，不是「跳过」
  8) 跳过的表里仍有 cera_shop_delivery_applied；delivery 仍 17 行、新角色 0 行
  9) 清理：删测试数据 + 删新角色 + 还原 charac_view 与自增计数器，
     断言 77 张表行数全还原、charac_view 字节一致、AI=9919、laoxx 字节一致

    python tools/v_r4_clone_keys.py
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS = 0x00F5, 0x0008
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_CLONE = 1000 + 8 * 100 + 28        # 1828

SRC_NO = 1
WORK = "taiwan_cain_2nd.user_items_work"
SOOYA = "taiwan_cain.test_sooya"
DELIV = "taiwan_cain_2nd.cera_shop_delivery_applied"
UI_IDS = (88001, 88002)
SOOYA_LEV = (7, 8)

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


# ---------------- 弹窗 ----------------
def top_windows(cls="#32770"):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if cls and c.value != cls:
            return True
        out.append(h)
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


def wait_box(timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = top_windows()
        if w:
            time.sleep(0.2)
            return w[0], dlg_text(w[0])
        time.sleep(0.05)
    return None, ""


def close_box(cid):
    w = top_windows()
    if not w:
        return False
    b = u.GetDlgItem(w[0], cid)
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 8:
        if not top_windows():
            return True
        time.sleep(0.05)
    return False


# ---------------- 库 ----------------
def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def char_tables(cur):
    cur.execute("SELECT table_schema,table_name FROM information_schema.columns "
                "WHERE column_name='charac_no' AND table_schema IN ('taiwan_cain','taiwan_cain_2nd')")
    return ["%s.%s" % (a, b) for a, b in cur.fetchall()]


def counts(cur, tabs):
    return {t: q1(cur, "SELECT COUNT(*) FROM %s" % t) for t in tabs}


def view_row(cur):
    cur.execute("SELECT m_id,charac_count,LENGTH(info),HEX(info) FROM taiwan_cain.charac_view "
                "WHERE m_id=1")
    return cur.fetchone()


# 本轮动过的 6 张表：改前 → 改后（克隆后）→ 还原，逐张留指纹。
# CHECKSUM TABLE 是整表逐行的字节级校验和（含 blob），比只数行数强得多 ——
# 「行数一样但某一行的 blob 被换过」这类事故只有它能查出来。
RT = ["taiwan_cain.charac_info", "taiwan_cain.charac_view", "taiwan_cain_2nd.inventory",
      WORK, SOOYA, DELIV]


def fingerprint(cur):
    fp = {}
    for t in RT:
        cur.execute("CHECKSUM TABLE %s" % t)
        fp[t] = cur.fetchone()[1]
    # CHECKSUM 不含 AUTO_INCREMENT，单独记
    fp["@AUTO_INCREMENT"] = q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                                    "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
    return fp


def seed(cur):
    """插 4 行测试数据（两张无主键表各 2 行，全挂 charac_no=1）。"""
    for i, uid in enumerate(UI_IDS):
        cur.execute(
            "INSERT INTO %s (ui_id,charac_no,slot,it_id,expire_date,obtain_from,reg_date,"
            "ipg_agency_no,ability_no,stat,clear_avatar_id,jewel_socket,item_lock_key) "
            "VALUES (%%s,%%s,%%s,%%s,'2030-01-01 00:00:00',0,NOW(),'R4TEST',0,0,0,'',0)"
            % WORK, (uid, SRC_NO, i + 1, 1001 + i))
    for i, lev in enumerate(SOOYA_LEV):
        cur.execute("INSERT INTO %s (m_id,charac_no,lev) VALUES (%%s,%%s,%%s)" % SOOYA,
                    (1, SRC_NO, lev))


def unseed(cur):
    cur.execute("DELETE FROM %s WHERE charac_no=%%s AND ui_id IN %%s" % WORK,
                (SRC_NO, UI_IDS))
    cur.execute("DELETE FROM %s WHERE charac_no=%%s AND lev IN %%s" % SOOYA,
                (SRC_NO, SOOYA_LEV))


def work_rows(cur, no):
    cur.execute("SELECT ui_id,charac_no,slot,it_id FROM %s WHERE charac_no=%%s "
                "ORDER BY ui_id" % WORK, (no,))
    return cur.fetchall()


def sooya_rows(cur, no):
    cur.execute("SELECT m_id,charac_no,lev FROM %s WHERE charac_no=%%s ORDER BY lev" % SOOYA,
                (no,))
    return cur.fetchall()


def cleanup(cur, tabs, new_no, view_before, ai_before):
    if new_no:
        for t in tabs:
            try:
                cur.execute("DELETE FROM %s WHERE charac_no=%%s" % t, (new_no,))
            except Exception as e:
                print("   [清理] %s 删除失败：%s" % (t, e))
    cur.execute("UPDATE taiwan_cain.charac_view SET info=UNHEX(%s), charac_count=%s WHERE m_id=1",
                (view_before[3], view_before[1]))
    if ai_before:
        try:
            cur.execute("ALTER TABLE taiwan_cain.charac_info AUTO_INCREMENT=%s", (ai_before,))
        except Exception as e:
            print("   [清理] 还原自增计数器失败：%s" % e)


# ---------------- 日志 ----------------
def logfile():
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def log_since(path, off):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def main():
    print("EXE =", EXE)
    c = db()
    cur = c.cursor()
    tabs = char_tables(cur)
    p = None
    new_no = 0
    seeded = False
    fp_after = None
    try:
        # ================= 0) 起点 =================
        view_before = view_row(cur)
        fp_before = fingerprint(cur)
        ai_before = q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                            "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
        cnt_before = counts(cur, tabs)
        cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                    "WHERE charac_no=1")
        name_before = cur.fetchone()
        print("起点：charac_info %s 行 / AI=%s / laoxx=%r / charac_no 表 %d 张"
              % (cnt_before["taiwan_cain.charac_info"], ai_before, name_before, len(tabs)))
        # 不认死 AI=9919：回归集跑完一轮之后它会被别的一次性脚本顶上去
        # （v_r1_name 临时建角色），认死一个数等于让本脚本在第二次跑时永久假红。
        # 真正要钉的是「跑完回到跑之前的样子」，那由下面第 10 步的 ai_before 比对负责。
        ok("0 起点：charac_info 恰好 1 行、laoxx 名字字节正确",
           cnt_before["taiwan_cain.charac_info"] == 1
           and name_before == ("laoxx", "6C616F7878"),
           "%s 行 / AI=%s / %r" % (cnt_before["taiwan_cain.charac_info"], ai_before, name_before))
        ok("0 起点：delivery 17 行且全挂 charac_no=1（克隆验收的条件，别清）",
           q1(cur, "SELECT COUNT(*) FROM %s" % DELIV) == 17
           and q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % DELIV) == 17,
           "%s / %s" % (q1(cur, "SELECT COUNT(*) FROM %s" % DELIV),
                        q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % DELIV)))
        ok("0 起点：两张无主键表原本是 0 行（所以要先插测试数据才看得出差别）",
           q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % WORK) == 0
           and q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % SOOYA) == 0,
           "%s=%s / %s=%s" % (WORK, q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % WORK),
                              SOOYA, q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % SOOYA)))
        if fails:
            print("\n起点不满足，后续无意义")
            return 1

        # ================= 1) 插测试数据 =================
        seed(cur)
        seeded = True
        print("   源角色 user_items_work =", work_rows(cur, SRC_NO))
        print("   源角色 test_sooya      =", sooya_rows(cur, SRC_NO))
        ok("1 测试数据已插：user_items_work 2 行 / test_sooya 2 行（charac_no=1）",
           len(work_rows(cur, SRC_NO)) == 2 and len(sooya_rows(cur, SRC_NO)) == 2,
           "%s / %s" % (len(work_rows(cur, SRC_NO)), len(sooya_rows(cur, SRC_NO))))

        # ================= 2) 启动 + 选角色 + 克隆 =================
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
        click_at(lv, 40, 12, 3.5)
        switch(m, 8)
        time.sleep(1.0)

        logp = logfile()
        off = os.path.getsize(logp) if logp else 0
        ok("2 找到「克隆角色」按钮", bool(ctl(m, G_CLONE)), "控件 %d" % G_CLONE)
        u.PostMessageW(ctl(m, G_CLONE), BM_CLICK, 0, 0)
        b, t = wait_box(8.0)
        ok("2 弹出克隆确认框", b is not None and "复制一份" in t, (t or "")[:60])
        if b:
            close_box(1)
        t0 = time.time()
        while time.time() - t0 < 180:
            cur.execute("SELECT charac_no,charac_name FROM taiwan_cain.charac_info "
                        "WHERE charac_no<>1 ORDER BY charac_no DESC LIMIT 1")
            r = cur.fetchone()
            if r:
                new_no, clone_name = r[0], r[1]
                break
            if top_windows():
                print("   [克隆期间弹窗] %r" % dlg_text(top_windows()[0]))
                close_box(1)
            time.sleep(0.5)
        ok("2 克隆成功（真的建出了新角色）", bool(new_no),
           "charac_no=%s name=%r" % (new_no, clone_name if new_no else None))
        if not new_no:
            print("---- 克隆失败，日志原文 ----")
            print(log_since(logp, off)[-3000:])
            return 1
        time.sleep(1.5)                # 等日志落盘（1s 一批）

        # ================= 3) 换号复制：user_items_work =================
        wn = work_rows(cur, new_no)
        ws = work_rows(cur, SRC_NO)
        print("   新角色 user_items_work =", wn)
        print("   源角色 user_items_work =", ws)
        ok("3 user_items_work：新角色有 2 行（旧策略会整表跳过 → 0 行）", len(wn) == 2,
           "%s 行" % len(wn))
        ok("3 user_items_work：charac_no 已换成新号 %s" % new_no,
           all(r[1] == new_no for r in wn), "%r" % (wn,))
        ok("3 user_items_work：ui_id / slot / it_id 原样带过来",
           [(r[0], r[2], r[3]) for r in wn] == [(r[0], r[2], r[3]) for r in ws],
           "%r vs %r" % ([(r[0], r[2], r[3]) for r in wn], [(r[0], r[2], r[3]) for r in ws]))

        # ================= 4) 换号复制：test_sooya =================
        sn = sooya_rows(cur, new_no)
        ss = sooya_rows(cur, SRC_NO)
        print("   新角色 test_sooya =", sn)
        print("   源角色 test_sooya =", ss)
        ok("4 test_sooya：新角色有 2 行（旧策略会整表跳过 → 0 行）", len(sn) == 2,
           "%s 行" % len(sn))
        ok("4 test_sooya：charac_no 换成新号、m_id 沿用、lev 一致",
           [(r[1], r[0], r[2]) for r in sn] == [(new_no, r[0], r[2]) for r in ss],
           "%r vs %r" % (sn, ss))

        # ================= 5) 源数据没被动过 =================
        ok("5 源角色的 4 行仍在（克隆不是搬家）",
           len(work_rows(cur, SRC_NO)) == 2 and len(sooya_rows(cur, SRC_NO)) == 2,
           "%s / %s" % (len(work_rows(cur, SRC_NO)), len(sooya_rows(cur, SRC_NO))))
        ok("5 源行 ui_id 没被改（ui_id 不是自增列，原样复制）",
           [r[0] for r in work_rows(cur, SRC_NO)] == list(UI_IDS),
           "%r" % ([r[0] for r in work_rows(cur, SRC_NO)],))

        # ================= 6) 日志判定分布 =================
        text = log_since(logp, off)
        per = [l for l in text.splitlines()
               if l.count("复制") and ("——（" in l)]
        skip = [l for l in text.splitlines() if " 跳过 —— " in l]
        recode = [l for l in per if "自增" not in l]
        skipcol = [l for l in per if "自增" in l]
        print("   复制（换号）= %d 张 / 复制（自增列交给服务端发号）= %d 张 / 整表跳过 = %d 张"
              % (len(recode), len(skipcol), len(skip)))
        ok("6 判定分布：换号复制 65 张", len(recode) == 65, "%d 张" % len(recode))
        ok("6 判定分布：单列自增主键跳列 5 张", len(skipcol) == 5, "%d 张" % len(skipcol))
        ok("6 判定分布：整表跳过 2 张（符合 keycheck 复算）", len(skip) == 2, "%d 张" % len(skip))
        ok("6 分布合计 = 在册 72 张", len(recode) + len(skipcol) + len(skip) == 72,
           "%d 张" % (len(recode) + len(skipcol) + len(skip)))

        # ================= 7) 两张无主键表在日志里是「复制」而不是「跳过」 =================
        lw = [l for l in text.splitlines() if "user_items_work" in l]
        ls = [l for l in text.splitlines() if "test_sooya" in l]
        print("   %s" % (lw[0] if lw else "(没有)"))
        print("   %s" % (ls[0] if ls else "(没有)"))
        ok("7 user_items_work 是「复制 2 行」，不是「跳过」",
           bool(lw) and "复制 2 行" in lw[0] and "跳过" not in lw[0], lw[0] if lw else "(没有)")
        ok("7 test_sooya 是「复制 2 行」，不是「跳过」",
           bool(ls) and "复制 2 行" in ls[0] and "跳过" not in ls[0], ls[0] if ls else "(没有)")
        ok("7 理由是「无主键（按 charac_no 归属）」",
           bool(lw) and "无主键（按 charac_no 归属）" in lw[0]
           and bool(ls) and "无主键（按 charac_no 归属）" in ls[0],
           (lw[0] if lw else "")[-40:])

        # ================= 8) 不透明主键仍然跳过 =================
        ld = [l for l in text.splitlines() if "cera_shop_delivery_applied" in l]
        print("   %s" % (ld[0] if ld else "(没有)"))
        ok("8 跳过的两张表里仍有 cera_shop_delivery_applied（不透明主键）",
           any("跳过" in l for l in ld), ld[0] if ld else "(没有)")
        dn = q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=%%s" % DELIV, (new_no,))
        ok("8 delivery 仍然整表跳过：新角色 0 行 / 全表 17 行",
           dn == 0 and q1(cur, "SELECT COUNT(*) FROM %s" % DELIV) == 17,
           "新角色 %s 行 / 全表 %s 行" % (dn, q1(cur, "SELECT COUNT(*) FROM %s" % DELIV)))

        # ================= 9) 数据库往返：改前 / 改后 / 还原 三张指纹 =================
        fp_after = fingerprint(cur)
        print("   改前指纹：%s" % fp_before)
        print("   改后指纹：%s" % fp_after)
        moved = [t for t in RT if fp_before[t] != fp_after[t]]
        info("9 克隆真的改动了 %d 张表（指纹变化）：%s" % (len(moved), moved))

        ok("9 进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        # ================= 清理 =================
        try:
            if new_no:
                cleanup(cur, tabs, new_no, view_before, ai_before)
        except Exception as e:
            print("!! 清理新角色失败：%s" % e)
        try:
            if seeded:
                unseed(cur)
        except Exception as e:
            print("!! 清理测试数据失败：%s" % e)
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass

        cnt_after = counts(cur, tabs)
        diff = {t: (cnt_before[t], cnt_after[t]) for t in tabs if cnt_before[t] != cnt_after[t]}
        print("清理后与改前行数不同的表: %s" % (diff or "无"))
        ok("10 清理后 77 张 charac_no 表行数全部还原", not diff, str(diff)[:300])
        ok("10 清理后 charac_info 回到 1 行",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info") == 1,
           "%s 行" % q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info"))
        ai_now = q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                         "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
        ok("10 清理后 AUTO_INCREMENT 回到改前（%s）" % ai_before, ai_now == ai_before,
           "%s → %s" % (ai_before, ai_now))
        ok("10 charac_view 字节与改前一致", view_row(cur) == view_before,
           "改前 %r 现在 %r" % (view_before, view_row(cur)))
        cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                    "WHERE charac_no=1")
        nm = cur.fetchone()
        ok("10 laoxx 的名字字节与改前一致", nm == ("laoxx", "6C616F7878"), "%r" % (nm,))
        ok("10 测试数据已删净：两张无主键表回到 0 行",
           q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % WORK) == 0
           and q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % SOOYA) == 0,
           "%s / %s" % (q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % WORK),
                        q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % SOOYA)))
        ok("10 delivery 仍是 17 行全挂 charac_no=1",
           q1(cur, "SELECT COUNT(*) FROM %s" % DELIV) == 17
           and q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=1" % DELIV) == 17)

        # ================= 11) 六张表的字节级还原 =================
        fp_end = fingerprint(cur)
        print("   还原后指纹：%s" % fp_end)
        bad = {t: (fp_before[t], fp_end[t]) for t in fp_before if fp_before[t] != fp_end[t]}
        ok("11 六张表 + 自增计数器：还原后字节级指纹与改前完全一致", not bad, str(bad)[:300])
        c.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
