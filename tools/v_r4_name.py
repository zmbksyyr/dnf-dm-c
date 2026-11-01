# -*- coding: utf-8 -*-
"""R4-01 定向验证：改名被拒（重名）后，界面上的名字框必须退回库里的真值。

链条：ui_char.c:701 gm_push(K_RENAME) → gm_apply 乐观地把 g.cur.name 改成新名字
→ 任务线程 act.c:74 db_name_taken 命中（重名）→ act.c:129 app_reload_later()
→ UI 线程 app_reload() 重读。

旧版（4df0a0c）的 app_reload 用 `g_load_ctx.c = g.cur` —— 那份正是被乐观改过的
内存值，而 db_load_data 的 SELECT 里没有 charac_name，重读一遍也读不回名字。
于是改名被拒后界面一直停在那个没写进库的重名上（看着像改成功了），只有状态栏
一句「已被占用」。本轮改成载入前先 db_read_charac 回读库里的真值（见 w_load）。

构造：库里本来就有 laoxx（charac_no=1）。临时建一个可载入的角色（复制 1 号的
charac_info / inventory 行，编号取在自增计数器之下，不改 AUTO_INCREMENT），
在 GM 页选中它，把它的名字改成 laoxx —— 重名，必然被拒。断言：名字框显示的是
它自己的名字（库里的真值），而不是 laoxx。

旧版必红：名字框停在 laoxx。

数据纪律：只 INSERT 两行（charac_info / inventory 各一行），finally 里删除，
并断言 charac_info 行数、AUTO_INCREMENT、laoxx 的名字字节都回到改前。

    python tools/v_r4_name.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, set_text, click_at

BM_CLICK, WM_KILLFOCUS = 0x00F5, 0x0008
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_NAME = 1000 + 8 * 100 + 1        # 1801：GM 页名字框

SRC_NO = 1                          # laoxx
TMP_NO = 9918                       # 临时角色号（在 AUTO_INCREMENT=9919 之下，插入不推计数器）
TMP_NAME = "重名测试号"
DUP_NAME = "laoxx"                  # 库里 1 号角色就叫这个 —— 改它必然被拒

fails = []
MAIN = 0


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


# ---------------- 弹窗 ----------------
def top_windows(main=None, cls="#32770"):
    main = main or MAIN
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if cls and c.value != cls:
            return True
        owner = u.GetWindow(h, 4)
        if owner and owner != main and not u.IsChild(main, owner):
            return True
        out.append(h)
        return True

    u.EnumWindows(cb(each), 0)
    return out


def close_box():
    w = top_windows()
    if not w:
        return False
    b = u.GetDlgItem(w[0], 1)
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


def src_row(cur):
    cur.execute("SELECT charac_name,HEX(charac_name) FROM taiwan_cain.charac_info "
                "WHERE charac_no=%s", (SRC_NO,))
    return cur.fetchone()


def cols(cur, dbn, tb):
    cur.execute("SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s ORDER BY ORDINAL_POSITION", (dbn, tb))
    return [r[0] for r in cur.fetchall()]


def copy_row(cur, dbn, tb, key, src_no, new_no, name=None):
    """复制一行（主键换成 new_no，可选把 charac_name 换成 name）。"""
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


def snapshot(cur):
    return {
        "info_n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info"),
        "inv_n": q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.inventory"),
        "ai": q1(cur, "SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                      "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'"),
        # 名字 + HEX 一起留档：还原只比对字节（latin1/utf8 双重编码只看名字看不出来）
        "src": src_row(cur),
        "tmp": q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                  (TMP_NO,)),
    }


def name_of(cur, no):
    return q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (no,))


def log_off():
    import glob
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return (fs[-1], os.path.getsize(fs[-1])) if fs else (None, 0)


def read_since(path, off):
    if not path or not os.path.exists(path):
        return ""
    with open(path, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


# ---------------- GUI ----------------
def box_text(main):
    return txt(ctl(main, G_NAME), 64)


def set_box(main, s):
    h = ctl(main, G_NAME)
    if not h:
        return False
    set_text(h, s)
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)      # 失焦 = 提交
    return True


def main():
    global MAIN
    print("EXE =", EXE)
    c = db()
    cur = c.cursor()
    p = None
    before = snapshot(cur)
    print("改前：charac_info %s 行 / inventory %s 行 / AUTO_INCREMENT=%s / 1 号名字=%r"
          % (before["info_n"], before["inv_n"], before["ai"], before["src"][0]))
    try:
        ok("0 起点：库里只有 laoxx 一个角色，AUTO_INCREMENT=9919",
           before["info_n"] == 1 and before["tmp"] == 0 and before["ai"] > TMP_NO,
           "%s 行 / AI=%s" % (before["info_n"], before["ai"]))
        if fails:
            print("\n起点不满足（临时号 %d 会顶到自增计数器，或库里已有别的角色），停止" % TMP_NO)
            return 1
        src_name = before["src"][0]
        ok("0 起点：1 号角色就叫「%s」" % DUP_NAME, src_name == DUP_NAME, "%r" % src_name)

        # ---------------- 1) 建一个可载入的临时角色 ----------------
        copy_row(cur, "taiwan_cain", "charac_info", "charac_no", SRC_NO, TMP_NO, name=TMP_NAME)
        copy_row(cur, "taiwan_cain_2nd", "inventory", "charac_no", SRC_NO, TMP_NO)
        ok("1 临时角色 %d「%s」已建（charac_info + inventory 各 1 行）"
           % (TMP_NO, TMP_NAME),
           name_of(cur, TMP_NO) == TMP_NAME
           and q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.inventory WHERE charac_no=%s",
                  (TMP_NO,)) == 1,
           "库里名字=%r" % name_of(cur, TMP_NO))

        # ---------------- 2) 启动 + 选中临时角色 ----------------
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = MAIN = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        t0 = time.time()
        n = 0
        while time.time() - t0 < 30 and n != 2:
            n = u.SendMessageW(lv, 0x1004, 0, 0)
            time.sleep(0.3)
        ok("2 角色列表有 2 行（laoxx + 临时角色）", n == 2, "%s 行" % n)
        click_at(lv, 40, 12, 1.0)               # 列表按 charac_no DESC：第一行是临时角色
        switch(m, 8)                            # GM 页
        time.sleep(1.0)
        t0 = time.time()
        while time.time() - t0 < 15 and box_text(m) != TMP_NAME:
            time.sleep(0.3)
        ok("2 GM 页选中了临时角色（名字框 = %r）" % TMP_NAME, box_text(m) == TMP_NAME,
           "名字框=%r" % box_text(m))
        if box_text(m) != TMP_NAME:
            print("   选中的不是临时角色，后续断言无意义")
            return 1

        # ---------------- 3) 改成重名 → 必须被拒 ----------------
        path, off = log_off()
        ok("3 把名字框填成「%s」并失焦提交" % DUP_NAME, set_box(m, DUP_NAME))
        hit = ""
        t0 = time.time()
        while time.time() - t0 < 20:
            for line in read_since(path, off).splitlines():
                if "改名" in line and "失败" in line:
                    hit = line
            if hit:
                break
            if top_windows():
                info("改名弹窗", repr(top_windows()[0]))
                close_box()
            time.sleep(0.3)
        print("   日志：%r" % (hit,))
        print("   状态栏：%r" % txt(u.GetDlgItem(m, IDC_STATUS), 512))
        ok("3 改名被拒（日志里有「改名…失败」）", bool(hit), hit or read_since(path, off)[-200:])
        ok("3 拒绝理由是重名（已被占用）", "已被占用" in hit, hit)

        # ---------------- 4) 判定：名字框必须退回库里的真值 ----------------
        # 旧版：g.cur.name 是被乐观改过的 laoxx，重读又读不回 charac_name → 停在 laoxx
        # 新版：载入前 db_read_charac 回读库里的名字 → 重填成临时角色自己的名字
        t0 = time.time()
        seen = []
        while time.time() - t0 < 25:
            t = box_text(m)
            if t not in seen:
                seen.append(t)
            if t == TMP_NAME:
                break
            time.sleep(0.4)
        print("   名字框变化轨迹：%r" % (seen,))
        ok("4 改名被拒后名字框退回库里的真值「%s」" % TMP_NAME, box_text(m) == TMP_NAME,
           "名字框=%r" % box_text(m))
        ok("4 名字框没有停在那个没写进库的重名「%s」上" % DUP_NAME, box_text(m) != DUP_NAME,
           "名字框=%r" % box_text(m))
        ok("4 库里临时角色的名字没被改", name_of(cur, TMP_NO) == TMP_NAME,
           "%r" % name_of(cur, TMP_NO))
        ok("4 库里 laoxx 的名字也没被改", name_of(cur, SRC_NO) == src_name,
           "%r" % name_of(cur, SRC_NO))

        # ---------------- 5) 正对照：改成不重名的名字要能写进去 ----------------
        switch(m, 8)
        time.sleep(0.6)
        ok("5 填「%s」并失焦" % "改名成功号", set_box(m, "改名成功号"))
        t0 = time.time()
        while time.time() - t0 < 15 and name_of(cur, TMP_NO) != "改名成功号":
            if top_windows():
                close_box()
            time.sleep(0.3)
        ok("5 不重名的名字能写进库", name_of(cur, TMP_NO) == "改名成功号",
           "%r" % name_of(cur, TMP_NO))
        t0 = time.time()
        while time.time() - t0 < 15 and box_text(m) != "改名成功号":
            time.sleep(0.4)
        ok("5 成功后名字框显示新名字", box_text(m) == "改名成功号", "名字框=%r" % box_text(m))

        ok("6 进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
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
        print("改后：charac_info %s 行 / inventory %s 行 / AUTO_INCREMENT=%s / 1 号名字=%r"
              % (after["info_n"], after["inv_n"], after["ai"], after["src"][0]))
        ok("7 清理后 charac_info 回到 %s 行" % before["info_n"],
           after["info_n"] == before["info_n"], "%s 行" % after["info_n"])
        ok("7 清理后 inventory 回到 %s 行" % before["inv_n"],
           after["inv_n"] == before["inv_n"], "%s 行" % after["inv_n"])
        ok("7 清理后 AUTO_INCREMENT 未变", after["ai"] == before["ai"],
           "%s → %s" % (before["ai"], after["ai"]))
        ok("7 清理后 laoxx 的名字字节与改前一致", after["src"] == before["src"],
           "%r vs %r" % (after["src"], before["src"]))
        c.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
