# -*- coding: utf-8 -*-
"""R1-003 定向验证：db_unlock_lev_equip 用 ON DUPLICATE KEY UPDATE，不再整行重建。

步骤：
  1) 事务内复现旧写法（DELETE + INSERT 只写两列）：(4242,7,50) → (0,0,85)，ROLLBACK
  2) 直连库造行 (charac_no=1, 4242, 7, 50)
  3) GUI 点 GM 页「解锁装备等级」（控件 1817）
  4) 断言三列 = 4242 / 7 / 85，且仍只有 1 行（不是删了重建多一行）
  5) 删行还原（原本 0 行），并核对 charac_info / charac_view 行数没被动过

    python tools/v_r1_unlock.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, db as open_db

CNO = 1
G_UNLOCK = 1000 + 8 * 100 + 17          # 1817
fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def row(cur):
    cur.execute("SELECT charac_no,tag_charac_no,striker_skill_index,max_equip_level "
                "FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (CNO,))
    return cur.fetchone()


def counts(cur):
    out = {}
    for t in ("charac_info", "charac_manage_info", "charac_view"):
        cur.execute("SELECT COUNT(*) FROM taiwan_cain.%s" % t)
        out[t] = cur.fetchone()[0]
    return out


def main():
    db = open_db()
    cur = db.cursor()

    # ---- 1. 旧写法复现（整段在事务里，最后 ROLLBACK）
    tx = open_db()
    tc = tx.cursor()
    tc.execute("START TRANSACTION")
    tc.execute("INSERT INTO taiwan_cain.charac_manage_info "
               "(charac_no,tag_charac_no,striker_skill_index,max_equip_level) VALUES (9914,4242,7,50)")
    tc.execute("SELECT charac_no,tag_charac_no,striker_skill_index,max_equip_level "
               "FROM taiwan_cain.charac_manage_info WHERE charac_no=9914")
    print("旧写法前：", tc.fetchone())
    tc.execute("DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=9914")
    tc.execute("INSERT INTO taiwan_cain.charac_manage_info (charac_no,max_equip_level) VALUES (9914,85)")
    tc.execute("SELECT charac_no,tag_charac_no,striker_skill_index,max_equip_level "
               "FROM taiwan_cain.charac_manage_info WHERE charac_no=9914")
    old = tc.fetchone()
    print("旧写法后：", old)
    tx.rollback()
    tx.close()
    ok("旧写法确实清零 tag/striker", old == (9914, 0, 0, 85), str(old))

    before_counts = counts(cur)
    print("改前行数：", before_counts)
    cur.execute("DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (CNO,))
    cur.execute("SELECT COUNT(*) FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (CNO,))
    plain = cur.fetchone()[0]
    ok("起始时 charac_no=1 在 charac_manage_info 无行", plain == 0, "rows=%s" % (plain,))
    cur.execute("INSERT INTO taiwan_cain.charac_manage_info "
                "(charac_no,tag_charac_no,striker_skill_index,max_equip_level) VALUES (%s,4242,7,50)",
                (CNO,))
    print("造行后：", row(cur))

    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)          # 连接
        time.sleep(4.5)
        lv = u.GetDlgItem(m, 113)
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        time.sleep(3.5)
        switch(m, 8)
        time.sleep(1.0)
        h = ctl(m, G_UNLOCK)
        if not ok("「解锁装备等级」按钮存在", bool(h), hex(h) if h else "None"):
            return 1
        u.PostMessageW(h, 0x00F5, 0, 0)                            # BM_CLICK
        got = None
        t0 = time.time()
        while time.time() - t0 < 12:
            got = row(cur)
            if got and got[3] == 85:
                break
            time.sleep(0.3)
        time.sleep(0.8)
        got = row(cur)
        print("点击后：", got)
        ok("max_equip_level 写成 85", got and got[3] == 85, str(got))
        ok("tag_charac_no 保留 4242（未被清 0）", got and got[1] == 4242, str(got))
        ok("striker_skill_index 保留 7（未被清 0）", got and got[2] == 7, str(got))
        cur.execute("SELECT COUNT(*) FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (CNO,))
        ok("没有多出行（仍是 1 行）", cur.fetchone()[0] == 1)
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        cur.execute("DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no IN (%s,9914)", (CNO,))
        cur.execute("SELECT COUNT(*) FROM taiwan_cain.charac_manage_info")
        left = cur.fetchone()[0]
        after_counts = counts(cur)
        print("还原本：charac_no=%d 行数=%d（全表 %d 行）" % (CNO, 0, left))
        print("还原后行数：", after_counts)
        ok("charac_manage_info 回到 0 行", left == 0, "rows=%d" % left)
        ok("charac_info 行数不变", before_counts["charac_info"] == after_counts["charac_info"],
           "%s → %s" % (before_counts["charac_info"], after_counts["charac_info"]))
        ok("charac_view 行数不变", before_counts["charac_view"] == after_counts["charac_view"],
           "%s → %s" % (before_counts["charac_view"], after_counts["charac_view"]))
        db.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
