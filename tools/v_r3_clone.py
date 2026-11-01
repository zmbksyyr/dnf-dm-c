# -*- coding: utf-8 -*-
"""R3-001 定向验证：克隆角色（db_admin.c 的 clone_key_policy）。

以前：clone_rows 只跳过 auto_increment 列，cera_shop_delivery_applied 的
delivery_id 是 binary(16) 主键（非自增、也不含 charac_no），INSERT..SELECT 把源行
主键原样搬过去 → 撞 PRIMARY(1062) → 整个克隆事务回滚 → 用户只看到「点了没反应」。

这一轮按「键策略」逐表判定。本脚本全程走真实 GUI，直连库断言：

  0) 起点：charac_info 只有 1 行 laoxx；delivery 17 行全挂在 charac_no=1 上
  1) 通过 GM 页把源角色改名成中文（克隆名由原名派生，这样新名字才是中文）
  2) 点「克隆角色」→ 确认 → 断言真的建出来了（charac_info / charac_view 同步）
  3) 新 charac_no 与源不同；delivery 那 17 行没有被复制到新角色（整表跳过）
  4) 日志里有「跳过 N 张非派生主键表」和逐表的跳过原因
  5) 清理：删掉新角色在所有 charac_no 表里的行 + 还原 charac_view 与自增计数器，
     断言行数 / 字节回到改前；名字改回 laoxx
失败（含克隆中途失败）都会走同一段清理，并打印原始错误。

    python tools/v_r3_clone.py
"""
import ctypes
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, db as open_db, start, switch, ctl, txt, set_text, click_at, wait_main, EXE

BM_CLICK, WM_CLOSE, WM_KILLFOCUS = 0x00F5, 0x0010, 0x0008
IDC_CONN, IDC_CHLIST = 104, 113
G_NAME = 1000 + 8 * 100 + 1        # 1801
G_CLONE = 1000 + 8 * 100 + 28      # 1828
CV_SLOT = 296

SRC_NO = 1
CN_NAME = "克隆三号"                # 4 个汉字，克隆名 = 克隆三号2（5 字 / 13 字节）

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


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


def slot_of(info, no):
    """把 charac_view.info 解成槽位，返回 no 所在槽的 (索引, charac_no, 名字字节)。"""
    plain = zlib.decompress(info[4:])
    for k in range(len(plain) // CV_SLOT):
        q = plain[k * CV_SLOT:(k + 1) * CV_SLOT]
        cno = int.from_bytes(q[0:4], "little")
        if cno == no:
            return k, cno, q[4:24].split(b"\0")[0]
    return None, None, None


def logfile():
    import glob
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def log_since(path, off):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


# ---------------- GUI ----------------
def click_btn(main, cid, wait=0.6):
    h = ctl(main, cid)
    if not h:
        return False
    u.PostMessageW(h, BM_CLICK, 0, 0)
    time.sleep(wait)
    return True


def rename_char(main, name, cur, cno=SRC_NO):
    h = ctl(main, G_NAME)
    if not h:
        return None
    set_text(h, name)
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 10:
        n = q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
        if n == name:
            return n
        if top_windows():
            print("   [改名弹窗] %r" % dlg_text(top_windows()[0]))
            close_box(1)
        time.sleep(0.3)
    return q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))


def cleanup(cur, tabs, new_no, view_before, ai_before):
    """把新角色和它派生出来的一切删干净；调用点不依赖它能成功，失败会打印。"""
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


def main():
    dbc = open_db()
    cur = dbc.cursor()
    tabs = char_tables(cur)
    p = None
    new_no = 0
    cloned = False
    try:
        # ================= 0) 起点快照 =================
        view_before = view_row(cur)
        ai_before = q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                            "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
        cnt_before = counts(cur, tabs)
        cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=1")
        name_before = cur.fetchone()
        dlv_total = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied")
        dlv_src = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                          "WHERE charac_no=1")
        print("起点：charac_info 行数=%d name=%r hex=%s"
              % (cnt_before["taiwan_cain.charac_info"], name_before[0], name_before[1]))
        print("起点：charac_view count=%s len=%s；delivery 全表 %s 行、charac_no=1 的 %s 行"
              % (view_before[1], view_before[2], dlv_total, dlv_src))
        print("起点：charac_info AUTO_INCREMENT=%s；charac_no 表 %d 张" % (ai_before, len(tabs)))

        ok("0 起点：charac_info 恰好 1 行", cnt_before["taiwan_cain.charac_info"] == 1,
           "%d 行" % cnt_before["taiwan_cain.charac_info"])
        ok("0 起点：charac_view 恰好 1 行", view_before is not None)
        ok("0 起点：delivery 17 行且全挂 charac_no=1（旧版必撞 1062 的条件）",
           dlv_total == 17 and dlv_src == 17, "%s / %s" % (dlv_total, dlv_src))
        if fails:
            print("\n起点不满足，后续无意义")
            return 1

        # ================= 1) 启动 + 选角色 + 改名成中文 =================
        p, main = start(page=8)
        lv = u.GetDlgItem(main, IDC_CHLIST)
        click_at(lv, 40, 12, 3.5)
        print("   角色列表选中行 =", u.SendMessageW(lv, 0x100C, -1, 0x0002))
        got = rename_char(main, CN_NAME, cur)
        ok("1 源角色改名成中文「%s」" % CN_NAME, got == CN_NAME, "%r" % (got,))
        cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name) "
                    "FROM taiwan_cain.charac_info WHERE charac_no=1")
        r = cur.fetchone()
        print("   落库 name=%r hex=%s CL=%s" % r)
        ok("1 中文名按 utf8 落库（hex 与字面量一致）", r[1] == CN_NAME.encode("utf-8").hex().upper(),
           "hex=%s" % (r[1],))

        # ================= 2) 点克隆 =================
        logp = logfile()
        off = os.path.getsize(logp) if logp else 0
        ok("2 找到「克隆角色」按钮", bool(ctl(main, G_CLONE)), "控件 %d" % G_CLONE)
        u.PostMessageW(ctl(main, G_CLONE), BM_CLICK, 0, 0)
        b, t = wait_box(8.0)
        ok("2 弹出克隆确认框", b is not None and "复制一份" in t, (t or "")[:80])
        if b:
            close_box(1)

        t0 = time.time()
        clone_name = None
        while time.time() - t0 < 120:
            cur.execute("SELECT charac_no,charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                        "WHERE charac_no<>1 ORDER BY charac_no DESC LIMIT 1")
            r = cur.fetchone()
            if r:
                new_no, clone_name = r[0], r[1]
                break
            if top_windows():
                print("   [克隆期间弹窗] %r" % dlg_text(top_windows()[0]))
                close_box(1)
            time.sleep(0.5)
        time.sleep(1.5)                     # 等 app_log 落盘（1s 一批）
        clone_ok = bool(new_no)
        cloned = clone_ok
        ok("2 克隆成功（真的建出了新角色，不是点了没反应）", clone_ok,
           "charac_no=%s name=%r" % (new_no, clone_name))
        if not clone_ok:
            print("---- 克隆失败，日志原文 ----")
            print(log_since(logp, off)[-3000:])
            return 1

        # ================= 3) 新角色本身 =================
        ok("3 新 charac_no 与源不同", new_no != SRC_NO, "%s vs %s" % (new_no, SRC_NO))
        expect = CN_NAME + "2"
        ok("3 新名字是中文派生名「%s」" % expect, clone_name == expect, "%r" % (clone_name,))
        cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name),"
                    "m_id,delete_flag,lev FROM taiwan_cain.charac_info WHERE charac_no=%s", (new_no,))
        c = cur.fetchone()
        print("   克隆行: name=%r hex=%s CL=%s L=%s m_id=%s del=%s lev=%s" % c)
        ok("3 克隆行在 charac_info 恰好 1 行",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s", (new_no,)) == 1)
        ok("3 克隆名无双重编码（hex 与字面量一致）",
           c[1] == expect.encode("utf-8").hex().upper(), "hex=%s" % (c[1],))
        ok("3 克隆名 CHAR_LENGTH=5 / LENGTH=13", (c[2], c[3]) == (5, 13), "%s/%s" % (c[2], c[3]))
        ok("3 等级跟着源角色（85）", c[6] == 85, "lev=%s" % (c[6],))

        # 不是空壳：几张关键表都要有行
        for t, want in [("taiwan_cain_2nd.inventory", 1), ("taiwan_cain_2nd.user_items", None),
                        ("taiwan_cain.charac_stat", 1), ("taiwan_cain.new_charac_quest", 1)]:
            n = q1(cur, "SELECT COUNT(*) FROM %s WHERE charac_no=%%s" % t, (new_no,))
            print("   %s 新角色行数 = %s" % (t, n))
            ok("3 %s 已复制" % t, (n == want) if want is not None else (n >= 1), "%s 行" % n)

        # ================= 4) charac_view 同步 =================
        cur.execute("SELECT charac_count,LENGTH(info),info FROM taiwan_cain.charac_view WHERE m_id=1")
        cv = cur.fetchone()
        k, cno_slot, nm_slot = slot_of(bytes(cv[2]), new_no)
        print("   charac_count=%s slot 索引=%s slot 内 charac_no=%s slot 内名字=%r"
              % (cv[0], k, cno_slot, nm_slot))
        ok("4 charac_view 的 charac_count 同步为 2", cv[0] == 2, "读到 %s（改前 %s）" % (cv[0], view_before[1]))
        ok("4 charac_view 里有新角色的槽位", k is not None, "槽索引 %s" % (k,))
        ok("4 槽位里的名字是新名字", nm_slot == expect.encode("utf-8"), "%r" % (nm_slot,))
        ok("4 charac_view 仍然只有 1 行（m_id=1）",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_view") == 1)

        # ================= 5) delivery 17 行没有被复制 =================
        dlv_total2 = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied")
        dlv_new = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                          "WHERE charac_no=%s", (new_no,))
        dlv_src2 = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                           "WHERE charac_no=1")
        print("   delivery: 全表 %s 行、新角色 %s 行、charac_no=1 的 %s 行"
              % (dlv_total2, dlv_new, dlv_src2))
        ok("5 delivery 全表仍是 17 行（没被删、没被改）", dlv_total2 == 17, "%s 行" % (dlv_total2,))
        ok("5 delivery 没有被复制到新角色（整表跳过）", dlv_new == 0, "新角色 %s 行" % (dlv_new,))
        ok("5 delivery 的 17 行仍全挂在源角色上", dlv_src2 == 17, "%s 行" % (dlv_src2,))

        # ================= 6) 日志里的跳过记录 =================
        text = log_since(logp, off)
        lines = [l for l in text.splitlines() if "跳过" in l]
        print("   日志里与本轮相关的跳过行 %d 条：" % len(lines))
        for l in lines[:8]:
            print("     " + l)
        ok("6 日志有「跳过 N 张非派生主键表」的汇总行",
           any("张非派生主键表" in l for l in lines),
           next((l for l in lines if "张非派生主键表" in l), "(没有)"))
        ok("6 日志点名跳过了 cera_shop_delivery_applied",
           any("cera_shop_delivery_applied" in l for l in lines),
           next((l for l in lines if "cera_shop_delivery_applied" in l), "(没有)"))
        ok("6 日志里有逐表的「复制 N 行」明细（不是只报一次结果）",
           text.count("复制") >= 10, "含「复制」的行 %d 条" % text.count("复制"))

        # ================= 7) 清理 =================
        cleanup(cur, tabs, new_no, view_before, ai_before)
        new_no_gone = new_no
        new_no = 0
        cnt_after = counts(cur, tabs)
        diff = {t: (cnt_before[t], cnt_after[t]) for t in tabs if cnt_before[t] != cnt_after[t]}
        print("   清理后与改前行数不同的表: %s" % (diff or "无"))
        ok("7 清理后 charac_info 回到 1 行", cnt_after["taiwan_cain.charac_info"] == 1,
           "%d 行" % cnt_after["taiwan_cain.charac_info"])
        ok("7 清理后 77 张 charac_no 表行数全部还原", not diff, str(diff)[:300])
        ok("7 清理后 charac_view 仍是 1 行且字节还原一致", view_row(cur) == view_before,
           "改前 len=%s 现在 len=%s" % (view_before[2], view_row(cur)[2]))
        ok("7 清理后 delivery 仍是 17 行、全挂 charac_no=1",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied") == 17
           and q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                       "WHERE charac_no=1") == 17)
        ok("7 新角色的行已从 charac_info 删净",
           q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
               (new_no_gone,)) == 0)
        ai_now = q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                         "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
        print("   自增计数器：改前 %s → 现在 %s" % (ai_before, ai_now))
        ok("7 自增计数器已还原", ai_now == ai_before, "%s → %s" % (ai_before, ai_now))

        # ================= 8) 名字改回 laoxx =================
        switch(main, 8)
        time.sleep(0.8)
        got = rename_char(main, name_before[0], cur)
        cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                    "FROM taiwan_cain.charac_info WHERE charac_no=1")
        r = cur.fetchone()
        print("   还原后 name=%r hex=%s CL=%s L=%s" % r)
        ok("8 名字改回原值", r[0] == name_before[0], "%r" % (r[0],))
        ok("8 还原后字节与原值一致", r[1] == name_before[1],
           "改前 %s 现在 %s" % (name_before[1], r[1]))

        ok("9 进程仍然活着（整轮没有崩溃）", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        # 走到这里说明中途出了意外（断言失败也会继续清）：宁可多清一次
        try:
            if new_no:
                print("!! 中途退出，执行清理")
                cleanup(cur, tabs, new_no, view_before, ai_before)
        except Exception as e:
            print("!! 清理失败：%s" % e)
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        dbc.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
