# -*- coding: utf-8 -*-
"""R1-005 定向验证：任务页的动作参数（qj / qsend）并入 DCtx，连点两行不再串参数。

做法：
  1) 临时给 charac_no=1 加第二个任务槽（play_2=102 / trigger=1），任务页就有两行；
     跑完把 play_1..play_20 及对应的 trigger 全部按快照还原。
  2) 对照组：只点第 1 行「放弃」→ play_1 必须变 0，play_2 不动。
  3) 判据组：先选第 2 行点「标记完成」，紧接着选第 1 行点「放弃」，重复 8 轮。
     参数若串了（旧写法两个动作读的是同一份全局 qj），第 1 行不会被放弃 —— play_1 仍是 101。
  4) 代码路径：qj / qsend 这两个文件级 static 必须已经不在了，作业读 c->q.*。

    python tools/v_r1_quest.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db as open_db

ROOT = os.environ.get("DNF_GM_SRC") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
Q_LIST, Q_REFRESH, Q_CLEAR, Q_INFO, Q_DROP, Q_DONE, Q_SEND, Q_PURGE = 1601, 1602, 1603, 1604, 1605, 1606, 1607, 1608
CNO = 1
ROUNDS = 8
fails = []
COLS = ["play_%d" % i for i in range(1, 21)] + ["play_%d_trigger" % i for i in range(1, 21)]


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def static_check():
    with open(os.path.join(ROOT, "src", "ui_data.c"), encoding="utf-8", errors="replace") as f:
        ud = f.read()
    ok("文件级 static QJob qj 已删除", "static QJob qj" not in ud)
    ok("文件级 static GrantItem qsend 已删除", "static GrantItem qsend" not in ud)
    ok("作业读 c->q.slot（参数随 DCtx 传）", "c->q.slot" in ud)
    ok("作业读 c->send[] / c->send_n", "c->send_n" in ud and "c->send[i]" in ud)


def snapshot(cur):
    cur.execute("SELECT %s FROM taiwan_cain.new_charac_quest WHERE charac_no=%%s" % ",".join(COLS), (CNO,))
    return cur.fetchone()


def restore(cur, snap):
    sets = ",".join("%s=%%s" % c for c in COLS)
    cur.execute("UPDATE taiwan_cain.new_charac_quest SET %s WHERE charac_no=%%s" % sets,
                list(snap) + [CNO])


def set_slot(cur, slot, qid, trig):
    cur.execute("UPDATE taiwan_cain.new_charac_quest SET play_%d=%%s, play_%d_trigger=%%s "
                "WHERE charac_no=%%s" % (slot, slot), (qid, trig, CNO))


def get_slots(cur):
    cur.execute("SELECT play_1,play_1_trigger,play_2,play_2_trigger "
                "FROM taiwan_cain.new_charac_quest WHERE charac_no=%s", (CNO,))
    return cur.fetchone()


def sel(m):
    h = ctl(m, Q_LIST)
    return u.SendMessageW(h, 0x100C, -1, 0x0002)      # LVM_GETNEXTITEM / LVNI_SELECTED


def pick_row(m, idx):
    """任务页行高约 36px（实测 y=48 落在第 2 行），选中状态用 LVM_GETNEXTITEM 回读确认。"""
    h = ctl(m, Q_LIST)
    for y in (12 + idx * 36, 12 + idx * 34, 48 if idx else 12):
        lp = (y << 16) | 60
        u.PostMessageW(h, 0x0201, 1, lp)
        u.PostMessageW(h, 0x0202, 0, lp)
        time.sleep(0.35)
        if sel(m) == idx:
            return y
    # 兜底：先回第一行，再用方向键往下走
    lp = (12 << 16) | 60
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)
    time.sleep(0.3)
    for _ in range(idx):
        u.PostMessageW(h, 0x0100, 0x28, 0)     # WM_KEYDOWN VK_DOWN
        u.PostMessageW(h, 0x0101, 0x28, 0)
        time.sleep(0.2)
    return "key" if sel(m) == idx else None


def wait_msgbox(main, timeout=6.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = []
        cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def each(h, lp):
            if not u.IsWindowVisible(h):
                return True
            cls = ctypes.create_unicode_buffer(32)
            u.GetClassNameW(h, cls, 32)
            ttl = ctypes.create_unicode_buffer(128)
            u.GetWindowTextW(h, ttl, 128)
            if cls.value == "#32770" and ttl.value == "DNF GM 工具":
                found.append(h)
            return True

        u.EnumWindows(cb(each), 0)
        if found:
            return found[0], txt(u.GetDlgItem(found[0], 0xFFFF), 512)
        time.sleep(0.03)
    return None, ""


def confirm_ok(m):
    b, t = wait_msgbox(m, 6.0)
    if not b:
        return ""
    u.PostMessageW(u.GetDlgItem(b, 1), 0x00F5, 0, 0)     # IDOK
    t0 = time.time()
    while time.time() - t0 < 5:
        cls = ctypes.create_unicode_buffer(32)
        u.GetClassNameW(b, cls, 32)
        if not u.IsWindow(b):
            break
        time.sleep(0.05)
    return t


def main():
    print("EXE =", EXE)
    static_check()
    db = open_db()
    cur = db.cursor()
    cur.execute("SELECT COUNT(*) FROM taiwan_cain.new_charac_quest WHERE charac_no=%s", (CNO,))
    if not ok("new_charac_quest 有 charac_no=1 的行", cur.fetchone()[0] == 1):
        return 1
    snap = snapshot(cur)
    print("原始槽位：", get_slots(cur))

    p = None
    try:
        set_slot(cur, 1, 101, 1)
        set_slot(cur, 2, 102, 1)
        print("造两行后：", get_slots(cur))

        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)
        time.sleep(4.5)
        lv = u.GetDlgItem(m, 113)
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        time.sleep(3.5)
        switch(m, 6)
        time.sleep(1.5)
        u.SendMessageW(ctl(m, Q_REFRESH), 0x00F5, 0, 0)
        time.sleep(1.5)
        n = u.SendMessageW(ctl(m, Q_LIST), 0x1004, 0, 0)
        if not ok("任务页显示 2 行", n == 2, "rows=%s" % n):
            return 1

        # ---- 对照组：单点第 1 行「放弃」
        y0 = pick_row(m, 0)
        ok("能选中第 1 行", y0 is not None, "y=%s" % (y0,))
        u.PostMessageW(ctl(m, Q_DROP), 0x00F5, 0, 0)
        t = confirm_ok(m)
        print("   放弃确认框：%r" % t[:40])
        time.sleep(1.2)
        got = get_slots(cur)
        print("   单点放弃后：", got)
        ok("对照组：play_1 被放弃（=0）", got[0] == 0, str(got))
        ok("对照组：play_2 未被动", got[2] == 102, str(got))

        # ---- 判据组：连着点两行不同动作
        bad = 0
        for r in range(ROUNDS):
            set_slot(cur, 1, 101, 1)
            set_slot(cur, 2, 102, 1)
            u.SendMessageW(ctl(m, Q_REFRESH), 0x00F5, 0, 0)
            time.sleep(0.9)
            if u.SendMessageW(ctl(m, Q_LIST), 0x1004, 0, 0) != 2:
                # 列表还没回来再等一会儿
                time.sleep(1.2)
            y1 = pick_row(m, 1)                              # 选中第 2 行
            s1 = sel(m)
            u.PostMessageW(ctl(m, Q_DONE), 0x00F5, 0, 0)     # 标记完成
            confirm_ok(m)
            y0b = pick_row(m, 0)                             # 立刻切到第 1 行
            s0 = sel(m)
            u.PostMessageW(ctl(m, Q_DROP), 0x00F5, 0, 0)     # 放弃
            confirm_ok(m)
            time.sleep(1.2)
            got = get_slots(cur)
            flag = "OK" if (got[0] == 0 and got[2] == 102) else "!!"
            print("   第 %d 轮：%s slots=%s  选中序列=%s→%s (y=%s/%s)"
                  % (r + 1, flag, got, s1, s0, y1, y0b))
            if got[0] != 0 or got[2] != 102:
                bad += 1
        ok("8 轮连点两行都按各自参数执行", bad == 0, "错 %d 轮" % bad)
        ok("进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        restore(cur, snap)
        now = get_slots(cur)
        print("还原后：", now)
        exp = (snap[0], snap[20], snap[1], snap[21])   # COLS 前 20 个是 play_n，后 20 个是 trigger
        ok("play_1 / play_2 已还原", now == exp, "%s vs %s" % (now, exp))
        db.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
