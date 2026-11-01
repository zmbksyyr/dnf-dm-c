# -*- coding: utf-8 -*-
"""R2-002 定向验证：写库字符集（db_admin.c / db_mail.c 由 latin1 改成 db_cs_name）。

三条写路径各走一次真实 GUI，全部用中文输入，直连库按 utf8mb4 读回比对：

  1) 建号/克隆  db_clone_char      → charac_info.charac_name（+ charac_view 槽位）
  2) 发文本邮件 db_send_text_mail  → letter.send_charac_name / letter.letter_text
  3) 宠物改名   db_set_pet_name    → creature_items.name

判据：读回来的字符串 == 写进去的字符串，且 CHAR_LENGTH 按字符数（双重编码会让
CHAR_LENGTH 变成 3 倍）。每一步都记录改前值 → 触发 → 改后值 → 还原 → 断言还原后
与原值逐字节一致。失败 sys.exit(1)。

    python tools/v_r2_charset.py
"""
import ctypes
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, db as open_db, start, switch, ctl, txt, set_text, click_at, wait_main

BM_CLICK = 0x00F5
WM_CLOSE = 0x0010

PAGE_MAIL, PAGE_PET = 3, 4
G_NAME = 1801
G_CLONE = 1828
M_TEXT, M_AID, M_SEND = 1310, 1308, 1311
P_LIST, P_NAME, P_RENAME = 1401, 1402, 1403
IDC_CHLIST = 113

NEW_NAME = "克隆测试"
MAIL_TEXT = "中文正文测试：闭环第二轮"
PET_NAME = "中文宠物"

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


# ---------------- 弹窗（确认框 / 提示框） ----------------
def top_windows(main=None, cls="#32770"):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value != cls:
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
            time.sleep(0.25)
            return w[0], dlg_text(w[0])
        time.sleep(0.05)
    return None, ""


def close_box(cid):
    """点掉当前弹窗（1=IDOK 2=IDCANCEL）。"""
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


# ---------------- 库访问 ----------------
def q1(cur, sql, args=None):
    cur.execute(sql, args)
    r = cur.fetchone()
    return r[0] if r else None


def snapshot_char(cur):
    cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                "FROM taiwan_cain.charac_info WHERE charac_no=1")
    return cur.fetchone()


def snapshot_view(cur):
    cur.execute("SELECT charac_count,LENGTH(info),HEX(info) FROM taiwan_cain.charac_view WHERE m_id=1")
    return cur.fetchone()


def char_tables(cur):
    cur.execute("SELECT table_schema,table_name FROM information_schema.columns "
                "WHERE column_name='charac_no' AND table_schema IN ('taiwan_cain','taiwan_cain_2nd')")
    return ["%s.%s" % (a, b) for a, b in cur.fetchall()]


def counts(cur, tabs):
    out = {}
    for t in tabs:
        out[t] = q1(cur, "SELECT COUNT(*) FROM %s" % t)
    return out


def drop_char(cur, no, tabs):
    for t in tabs:
        cur.execute("DELETE FROM %s WHERE charac_no=%%s" % t, (no,))
    cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE charac_no=%s", (no,))


# ---------------- GUI 动作 ----------------
def click_btn(main, cid, wait=0.6):
    h = ctl(main, cid)
    if not h:
        return False
    u.PostMessageW(h, BM_CLICK, 0, 0)
    time.sleep(wait)
    return True


def select_char(main):
    lv = u.GetDlgItem(main, IDC_CHLIST)
    click_at(lv, 40, 12, 3.5)


def rename_char(main, name, cur):
    """GM 页 G_NAME 框：填名 + 失焦 = 提交（K_RENAME）。"""
    h = ctl(main, G_NAME)
    if not h:
        return None
    set_text(h, name)
    u.SendMessageW(h, 0x0008, 0, 0)      # WM_KILLFOCUS → 子类转 EN_KILLFOCUS
    t0 = time.time()
    while time.time() - t0 < 8:
        n = q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=1")
        if n == name:
            return n
        if top_windows():
            print("   [改名弹窗] %r" % dlg_text(top_windows()[0]))
            close_box(1)
        time.sleep(0.3)
    return q1(cur, "SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=1")


def main():
    dbc = open_db()
    cur = dbc.cursor()
    tabs = char_tables(cur)
    p = None
    try:
        # ================= 0) 改前快照 =================
        before = snapshot_char(cur)
        view_before = snapshot_view(cur)
        cnt_before = counts(cur, tabs)
        print("改前 charac_name=%r hex=%s CHAR_LENGTH=%d LENGTH=%d" % before)
        print("改前 charac_view: count=%s len=%s" % (view_before[0], view_before[1]))
        print("改前 charac_info 行数=%d（charac_no 表共 %d 张）" % (cnt_before["taiwan_cain.charac_info"], len(tabs)))
        ok("0 起点是干净的（角色名 laoxx、只有一个角色）",
           before[0] == "laoxx" and cnt_before["taiwan_cain.charac_info"] == 1,
           "%r / %d 行" % (before[0], cnt_before["taiwan_cain.charac_info"]))

        p, main = start(page=8)
        select_char(main)
        print("角色列表选中行 =", u.SendMessageW(u.GetDlgItem(main, IDC_CHLIST), 0x100C, -1, 0x0002))

        # ================= 1) 改名成中文（为克隆准备中文名） ==============
        got = rename_char(main, NEW_NAME, cur)
        ok("1 改名成中文后库里就是「%s」" % NEW_NAME, got == NEW_NAME, "读到 %r" % (got,))
        row = snapshot_char(cur)
        ok("1 CHAR_LENGTH=4（汉字按字符算，双重编码会是 12）", row[2] == 4, "CHAR_LENGTH=%s" % (row[2],))
        ok("1 LENGTH=12（每个汉字 3 字节）", row[3] == 12, "LENGTH=%s" % (row[3],))
        ok("1 utf8mb4 读回无乱码", row[0] == NEW_NAME, "%r" % (row[0],))

        # ================= 2) 克隆角色 =================
        # 先处理一个与字符集无关的既有缺陷：cera_shop_delivery_applied 的主键
        # delivery_id binary(16) 不是从 charac_no 派生的，db_clone_char 的
        # INSERT ... SELECT 会把源行的主键一起复制，撞 PRIMARY → 整个克隆事务回滚
        # （日志原话：Duplicate entry ...）。这段临时移走源角色的行、克隆完原样插回，
        # 只为让 charac_name 那条写路径真的跑起来；缺陷本身单独记进 regressions。
        cur.execute("SELECT delivery_id,charac_no,request_hash,result_blob,applied_at "
                    "FROM taiwan_cain_2nd.cera_shop_delivery_applied WHERE charac_no=1")
        deliv = cur.fetchall()
        cur.execute("DELETE FROM taiwan_cain_2nd.cera_shop_delivery_applied WHERE charac_no=1")
        print("   临时移走 cera_shop_delivery_applied %d 行（主键冲突会让克隆整体回滚）" % len(deliv))

        time.sleep(1.0)
        ok("2 点「克隆角色」", click_btn(main, G_CLONE, 0.8), "控件 %d" % G_CLONE)
        b, t = wait_box(8.0)
        ok("2 弹出克隆确认框", b is not None, (t or "")[:60])
        if b:
            close_box(1)

        clone_no, clone_name = 0, None
        t0 = time.time()
        while time.time() - t0 < 120:
            cur.execute("SELECT charac_no,charac_name FROM taiwan_cain.charac_info "
                        "WHERE charac_no<>1 ORDER BY charac_no DESC LIMIT 1")
            r = cur.fetchone()
            if r:
                clone_no, clone_name = r[0], r[1]
                break
            if top_windows():
                print("   [克隆期间弹窗] %r" % dlg_text(top_windows()[0]))
                close_box(1)
            time.sleep(0.5)
        ok("2 克隆出了新角色", clone_no > 1, "charac_no=%s name=%r" % (clone_no, clone_name))
        if clone_no > 1:
            cur.execute("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                        "FROM taiwan_cain.charac_info WHERE charac_no=%s", (clone_no,))
            c = cur.fetchone()
            print("   克隆行: name=%r hex=%s CL=%s L=%s" % c)
            ok("2 克隆角色名是中文（原名+数字后缀）", c[0] == NEW_NAME + "2", "%r" % (c[0],))
            ok("2 克隆名 CHAR_LENGTH=5", c[2] == 5, "CHAR_LENGTH=%s" % (c[2],))
            ok("2 克隆名 LENGTH=13（4 汉字 12 字节 + 后缀 1 字节）", c[3] == 13, "LENGTH=%s" % (c[3],))
            ok("2 克隆名无双重编码", c[0] == NEW_NAME + "2", "hex=%s" % (c[1],))
            # 事务完整性：charac_info 1 行、charac_view 槽位指向新号
            cur.execute("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s", (clone_no,))
            ok("2 克隆角色在 charac_info 里恰好 1 行", q1(cur, "SELECT COUNT(*) FROM taiwan_cain.charac_info "
                                                        "WHERE charac_no=%s", (clone_no,)) == 1)
            ok("2 charac_count 已同步为 2", q1(cur, "SELECT charac_count FROM taiwan_cain.charac_view "
                                                  "WHERE m_id=1") == 2,
               "读到 %s（改前 %s）" % (q1(cur, "SELECT charac_count FROM taiwan_cain.charac_view WHERE m_id=1"),
                                    view_before[0]))

        # ================= 3) 删掉克隆 + 还原 charac_view + 还原移走的行 ==========
        if clone_no > 1:
            drop_char(cur, clone_no, tabs)
            cur.execute("UPDATE taiwan_cain.charac_view SET info=UNHEX(%s), charac_count=%s WHERE m_id=1",
                        (view_before[2], view_before[0]))
        for row in deliv:
            cur.execute("INSERT INTO taiwan_cain_2nd.cera_shop_delivery_applied "
                        "(delivery_id,charac_no,request_hash,result_blob,applied_at) "
                        "VALUES (%s,%s,%s,%s,%s)", row)
        cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied WHERE charac_no=1")
        ok("3 cera_shop_delivery_applied 行数还原", q1(cur, "SELECT COUNT(*) FROM "
                                                  "taiwan_cain_2nd.cera_shop_delivery_applied "
                                                  "WHERE charac_no=1") == len(deliv),
           "%d → %d" % (len(deliv), q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                                       "WHERE charac_no=1")))
        cur.execute("SELECT HEX(delivery_id) FROM taiwan_cain_2nd.cera_shop_delivery_applied "
                    "WHERE charac_no=1 ORDER BY 1")
        now_ids = [r[0] for r in cur.fetchall()]
        ok("3 cera_shop_delivery_applied 主键字节还原一致",
           sorted(now_ids) == sorted(r[0].hex().upper() for r in deliv), "%d 个主键" % len(now_ids))
        cnt_after = counts(cur, tabs)
        diff = {t: (cnt_before[t], cnt_after[t]) for t in tabs if cnt_before[t] != cnt_after[t]}
        ok("3 删克隆后 77 张表行数与改前一致", not diff, str(diff)[:300])
        cur.execute("SELECT charac_count,LENGTH(info),HEX(info) FROM taiwan_cain.charac_view WHERE m_id=1")
        v = cur.fetchone()
        ok("3 charac_view 槽位字节还原一致", v == view_before, "改前 %s… 现在 %s…" % (view_before[1], v[1]))

        # ================= 4) 名字改回 laoxx =================
        switch(main, 8)
        time.sleep(0.8)
        got = rename_char(main, before[0], cur)
        row = snapshot_char(cur)
        ok("4 名字改回原值", row[0] == before[0], "%r" % (row[0],))
        ok("4 还原后字节与原值一致", row[1] == before[1], "改前 %s 现在 %s" % (before[1], row[1]))
        ok("4 还原后 CHAR_LENGTH/LENGTH 一致", (row[2], row[3]) == (before[2], before[3]),
           "%s/%s" % (row[2], row[3]))

        # ================= 5) 发中文正文邮件 =================
        switch(main, PAGE_MAIL)
        time.sleep(1.2)
        mail_before = q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.letter")
        print("改前 letter 行数 =", mail_before)
        tedit = ctl(main, M_TEXT)
        aid = ctl(main, M_AID)
        ok("5 邮件页正文框/附件框存在", bool(tedit) and bool(aid), "%s/%s" % (tedit, aid))
        if tedit and aid:
            set_text(aid, "")
            set_text(tedit, MAIL_TEXT)
            time.sleep(0.3)
            print("   框里读回 =", repr(txt(tedit)))
            ok("5 点「发送邮件」", click_btn(main, M_SEND, 0.8))
            t0 = time.time()
            row = None
            while time.time() - t0 < 20:
                cur.execute("SELECT letter_id,charac_no,send_charac_name,letter_text,"
                            "HEX(letter_text),CHAR_LENGTH(letter_text),LENGTH(letter_text) "
                            "FROM taiwan_cain_2nd.letter ORDER BY letter_id DESC LIMIT 1")
                row = cur.fetchone()
                if row and row[0]:
                    break
                row = None
                if top_windows():
                    print("   [发信弹窗] %r" % dlg_text(top_windows()[0]))
                    close_box(1)
                time.sleep(0.5)
            ok("5 库里多了一封邮件", row is not None,
               "letter 行数 %s → %s" % (mail_before, q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.letter")))
            if row:
                print("   落库: id=%s cno=%s sender=%r text=%r CL=%s L=%s"
                      % (row[0], row[1], row[2], row[3], row[5], row[6]))
                ok("5 letter_text 读回与发送内容一致", row[3] == MAIL_TEXT, "%r" % (row[3],))
                ok("5 letter_text CHAR_LENGTH=%d（字符数）" % len(MAIL_TEXT),
                   row[5] == len(MAIL_TEXT), "CHAR_LENGTH=%s" % (row[5],))
                ok("5 letter_text 不是双重编码", "æ" not in (row[3] or "") and "\\x" not in (row[4] or ""),
                   "hex=%s" % (row[4],))
                ok("5 send_charac_name=GM 未乱码", row[2] == "GM", "%r" % (row[2],))
                cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id=%s", (row[0],))
            ok("5 删掉这封邮件后行数还原", q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.letter") == mail_before,
               "现在 %s" % (q1(cur, "SELECT COUNT(*) FROM taiwan_cain_2nd.letter"),))

        # ================= 6) 宠物改名 =================
        switch(main, PAGE_PET)
        time.sleep(1.5)
        cur.execute("SELECT ui_id,name,HEX(name),CHAR_LENGTH(name) FROM taiwan_cain_2nd.creature_items "
                    "WHERE charac_no=1 ORDER BY ui_id LIMIT 1")
        pet_before = cur.fetchone()
        print("改前宠物: ui_id=%s name=%r hex=%s CL=%s" % pet_before)
        lv = ctl(main, P_LIST)
        nm = ctl(main, P_NAME)
        ok("6 宠物列表/改名框存在", bool(lv) and bool(nm), "%s/%s" % (lv, nm))
        if lv and nm and pet_before:
            print("   宠物列表行数 =", u.SendMessageW(lv, 0x1004, 0, 0))
            click_at(lv, 40, 12, 0.8)
            set_text(nm, PET_NAME)
            time.sleep(0.3)
            ok("6 点「改名」", click_btn(main, P_RENAME, 1.0))
            got = None
            t0 = time.time()
            while time.time() - t0 < 15:
                got = q1(cur, "SELECT name FROM taiwan_cain_2nd.creature_items WHERE ui_id=%s",
                         (pet_before[0],))
                if got == PET_NAME:
                    break
                time.sleep(0.4)
            ok("6 宠物名读回 = 「%s」" % PET_NAME, got == PET_NAME, "%r" % (got,))
            cur.execute("SELECT name,HEX(name),CHAR_LENGTH(name),LENGTH(name) "
                        "FROM taiwan_cain_2nd.creature_items WHERE ui_id=%s", (pet_before[0],))
            r = cur.fetchone()
            print("   落库: name=%r hex=%s CL=%s L=%s" % r)
            ok("6 宠物名 CHAR_LENGTH=4", r[2] == 4, "CHAR_LENGTH=%s" % (r[2],))
            ok("6 宠物名 LENGTH=12", r[3] == 12, "LENGTH=%s" % (r[3],))
            ok("6 宠物名无双重编码", r[0] == PET_NAME, "hex=%s" % (r[1],))
            # 原名是空串，UI 的改名路径明确拒绝空名，只能直接还原
            cur.execute("UPDATE taiwan_cain_2nd.creature_items SET name=%s WHERE ui_id=%s",
                        (pet_before[1], pet_before[0]))
            cur.execute("SELECT HEX(name),CHAR_LENGTH(name) FROM taiwan_cain_2nd.creature_items WHERE ui_id=%s",
                        (pet_before[0],))
            back = cur.fetchone()
            ok("6 宠物名还原后字节与原值一致", (back[0], back[1]) == (pet_before[2], pet_before[3]),
               "改前 %s/%s 现在 %s/%s" % (pet_before[2], pet_before[3], back[0], back[1]))

        ok("进程仍然活着（整轮没有崩溃）", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
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
