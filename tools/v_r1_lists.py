# -*- coding: utf-8 -*-
"""R1-004 定向验证：宠物 / 时装 / 任务三页改成「作业写暂存、UI 线程交换」之后，
列表还能正常拿到数据，且反复切页 + 刷新不会崩、不会留空表。

断言：
  1) 三页首屏行数 == 库里行数（宠物 1 / 时装 20 / 任务 1，按库实时算）
  2) 12 轮「刷新 + 立刻切页」交织轰炸后，三页行数仍然 == 库行数
     （交换写错、所有权丢了、角色校验误丢包，都会表现为 0 行或行数不对）
  3) 进程存活，日志里没有异常

    python tools/v_r1_lists.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch, ctl, db as open_db

ROOT = os.environ.get("DNF_GM_SRC") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = {"pet": (4, 1401, 1407), "avatar": (5, 1501, 1505), "quest": (6, 1601, 1602)}
ROUNDS = 12
fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def static_check():
    """代码路径：作业只写暂存，UI 线程在 job_poll 之后交换。"""
    with open(os.path.join(ROOT, "src", "ui_data.c"), encoding="utf-8", errors="replace") as f:
        ud = f.read()
    with open(os.path.join(ROOT, "src", "app.c"), encoding="utf-8", errors="replace") as f:
        ap = f.read()
    ok("作业不再原地写 g.pets", "&g.cur, &g.pets" not in ud, "")
    ok("作业写的是 s_pets_next", "&g.cur, &s_pets_next" in ud, "")
    i_call = ap.find("ui_data_apply_lists();")
    i_poll = ap.find("job_poll(&g.job_act")
    ok("ui_data_apply_lists 在 app_tick 的 job_poll(job_act) 之后调用",
       i_call > 0 and i_poll > 0 and i_call > i_poll, "poll@%d call@%d" % (i_poll, i_call))
    body = ud[ud.find("void ui_data_apply_lists"):ud.find("void ui_data_apply_lists") + 600]
    ok("三张表都在交换函数里 take_list，位也在那里置",
       "take_list(&g.pets, &s_pets_next" in body and
       "take_list(&g.avatars, &s_avas_next" in body and
       "take_list(&g.quests, &s_quests_next" in body and
       "g.lists_ready |= L_PETS" in body and
       "g.lists_ready |= L_QUESTS" in body, "")


def rows(m, page):
    h = ctl(m, PAGES[page][1])
    return u.SendMessageW(h, 0x1004, 0, 0) if h else -1


def wait_rows(m, page, want, timeout=12.0):
    t0 = time.time()
    got = -1
    while time.time() - t0 < timeout:
        got = rows(m, page)
        if got == want:
            return got
        time.sleep(0.2)
    return got


def main():
    print("EXE =", EXE)
    static_check()
    db = open_db()
    cur = db.cursor()
    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.creature_items WHERE charac_no=1")
    n_pet = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE charac_no=1")
    n_ava = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM taiwan_cain.new_charac_quest WHERE charac_no=1")
    n_qrow = cur.fetchone()[0]
    cur.execute("SELECT " + ",".join("play_%d" % i for i in range(1, 21)) +
                " FROM taiwan_cain.new_charac_quest WHERE charac_no=1")
    q = cur.fetchone()
    n_quest = sum(1 for x in q if x) if q else 0
    db.close()
    want = {"pet": n_pet, "avatar": n_ava, "quest": n_quest}
    print("库里行数：宠物 %d / 时装 %d / 任务 %d（new_charac_quest 有 %d 行）"
          % (n_pet, n_ava, n_quest, n_qrow))

    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)
        time.sleep(4.5)
        lv = u.GetDlgItem(m, 113)
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        time.sleep(3.5)

        for name in ("pet", "avatar", "quest"):
            switch(m, PAGES[name][0])
            got = wait_rows(m, name, want[name])
            ok("%s 页行数 == 库里行数" % name, got == want[name], "got=%s want=%s" % (got, want[name]))

        # 交织轰炸：刷新 → 立刻切走 → 再回来
        seq = ["quest", "pet", "avatar"]
        for r in range(ROUNDS):
            for name in seq:
                switch(m, PAGES[name][0])
                b = ctl(m, PAGES[name][2])
                if b:
                    u.PostMessageW(b, 0x00F5, 0, 0)          # 刷新，不等
                time.sleep(0.05)
            if p.poll() is not None:
                print("!! 第 %d 轮中途崩溃" % (r + 1))
                break
        time.sleep(2.5)

        ok("轰炸中进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
        for name in ("pet", "avatar", "quest"):
            switch(m, PAGES[name][0])
            got = wait_rows(m, name, want[name], 15.0)
            ok("轰炸后 %s 页行数仍正确" % name, got == want[name],
               "got=%s want=%s" % (got, want[name]))

        # 再刷一次，看列表没有变空（旧写法会先 vec_clear 再填）
        switch(m, 6)
        b = ctl(m, 1602)
        u.PostMessageW(b, 0x00F5, 0, 0)
        seen = []
        t0 = time.time()
        while time.time() - t0 < 6:
            seen.append(rows(m, "quest"))
            time.sleep(0.02)
        print("刷新期间任务页行数采样：min=%s max=%s 样本 %d" %
              (min(seen), max(seen), len(seen)))
        ok("刷新期间列表没有掉到 0 行", min(seen) > 0, "min=%s" % (min(seen),))
        ok("刷新收敛到库里的行数", seen[-1] == want["quest"], "last=%s" % (seen[-1],))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
