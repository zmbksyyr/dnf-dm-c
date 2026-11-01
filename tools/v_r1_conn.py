# -*- coding: utf-8 -*-
"""R1-002 定向验证：断开连接时先停掉共用 g.db 的四个作业，再 db_close / db_free_data。

两条证据：
  1) 代码路径（静态读 src/app.c）：app_disconnect 必须先停掉共用 g.db 的那几个作业，
     再 db_close / db_free_data。修复前这里是 db_close 在最前面。
     （第 4 轮起这段收进了 stop_db_jobs()，断言跟着改成看它 —— 意图没变。）
  2) 实测：连库 → 点角色（起 job_load，载入 6 个容器要几百毫秒）→ 立刻点「断开」。
     这一下正好把 db_close / db_free_data 压在载入作业身上。
     重复 12 轮，断言进程不崩、日志里没有「任务线程 30 秒未退出」，断开后还能再连上。

    python tools/v_r1_conn.py
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, txt

ROOT = os.environ.get("DNF_GM_SRC") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDC_CONN, IDC_DISC, IDC_CHLIST = 104, 105, 113
CYCLES = 12
fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def app_c_src():
    with open(os.path.join(ROOT, "src", "app.c"), encoding="utf-8", errors="replace") as f:
        return f.read()


def static_check():
    src = app_c_src()
    m = re.search(r"void app_disconnect\(void\) \{(.*?)\n\}", src, re.S)
    if not m:
        ok("找到 app_disconnect 函数体", False)
        return
    body = m.group(1)
    i_close = body.find("db_close(&g.db)")
    # 第 4 轮把「断开前先停哪些作业」抽成了 stop_db_jobs()（断开和退出共用一份，
    # 顺便补上了以前漏掉的 job_conn）。断言的意图没变 —— db_close / db_free_data
    # 之前必须先把共用这条连接的作业停下来 —— 只是四个 job_stop 不再写在
    # app_disconnect 体内，所以这里改成：体内必须调 stop_db_jobs 且在其之前不关连接，
    # 同时进 stop_db_jobs 体内确认那 5 个作业一个不少。
    i_stop = body.find("stop_db_jobs()")
    ok("app_disconnect 调了 stop_db_jobs()", i_stop >= 0, "stop_db_jobs@%d" % i_stop)
    ok("db_close 在 stop_db_jobs 之后",
       i_close > 0 and i_stop >= 0 and i_stop < i_close, "db_close@%d" % i_close)
    i_free = body.find("db_free_data(&g.data)")
    ok("db_free_data 也在 stop_db_jobs 之后",
       i_free > 0 and i_stop >= 0 and i_stop < i_free, "db_free_data@%d" % i_free)

    m2 = re.search(r"static int stop_db_jobs\(void\) \{(.*?)\n\}", src, re.S)
    if not m2:
        ok("找到 stop_db_jobs 函数体", False)
        return
    body2 = m2.group(1)
    # job_conn 是第 4 轮补的：它正在 db_open/select 时点断开，db_close 就撞上它的
    # 收发包缓冲。断言里必须有它，否则「补上又漏掉」这类回归测不出来。
    jobs = ["job_stop(&g.job_conn)", "job_stop(&g.job_load)", "job_stop(&g.job_act)",
            "job_stop(&g.job_poll)", "job_stop(&g.job_bubble)"]
    pos = [(j, body2.find(j)) for j in jobs]
    print("stop_db_jobs 体内位置：%s" % pos)
    ok("stop_db_jobs 停了全部 5 个共用连接的作业（含 job_conn）",
       all(p >= 0 for _, p in pos), "缺少 %s" % [j for j, p in pos if p < 0])


def log_path():
    d = os.path.join(BUILD, "config", "logs")
    if not os.path.isdir(d):
        return None
    fs = sorted(os.path.join(d, f) for f in os.listdir(d))
    return fs[-1] if fs else None


def log_nlines():
    """当前日志行数 —— 用来在程序启动前记一个水位。

    日志文件是**按天追加**的，同一天跑十几个脚本全部往里写。以前直接读尾部
    400 行去断言「没有作业停不下来的记录」，于是把前一个脚本（v_r3_jobstop /
    v_r4_shutdown 会故意把作业卡 30 秒）留下的历史行也算进来了 —— 本轮明明干净
    却一直红。改成只从「本次启动时的行数」之后开始看。
    """
    p = log_path()
    if not p:
        return 0
    with open(p, encoding="utf-8", errors="replace") as f:
        return len(f.read().splitlines())


def log_since(mark, n=400):
    p = log_path()
    if not p:
        return []
    with open(p, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    return lines[mark:][-n:]


def rows(h):
    return u.SendMessageW(h, 0x1004, 0, 0)


def main():
    print("EXE =", EXE)
    static_check()

    log_mark = log_nlines()          # 本次启动前的水位：只看这之后新增的行
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        conn = u.GetDlgItem(m, IDC_CONN)
        disc = u.GetDlgItem(m, IDC_DISC)
        lv = u.GetDlgItem(m, IDC_CHLIST)

        ok("连上第一轮", True)
        u.PostMessageW(conn, 0x00F5, 0, 0)
        t0 = time.time()
        while time.time() - t0 < 20 and rows(lv) == 0:
            time.sleep(0.2)
        ok("角色列表读出来了", rows(lv) > 0, "rows=%d" % rows(lv))

        alive = []
        for i in range(CYCLES):
            lp = (12 << 16) | 40
            u.PostMessageW(lv, 0x0201, 1, lp)               # 点角色 → job_load 起
            u.PostMessageW(lv, 0x0202, 0, lp)
            time.sleep(0.10)                                # 让它刚进 db_load_data
            u.PostMessageW(disc, 0x00F5, 0, 0)              # 立刻断开
            time.sleep(1.2)
            alive.append(p.poll() is None)
            if p.poll() is not None:
                print("!! 第 %d 轮断开后进程已退出，returncode=%s" % (i + 1, p.poll()))
                break
            # 再连回来，下一轮继续
            u.PostMessageW(conn, 0x00F5, 0, 0)
            t0 = time.time()
            while time.time() - t0 < 20 and rows(lv) == 0:
                time.sleep(0.2)
            print("   第 %2d 轮：断开→重连 行数=%d 进程存活=%s" % (i + 1, rows(lv), alive[-1]))

        ok("%d 轮「载入中直接断开」都没崩" % CYCLES, all(alive) and len(alive) == CYCLES,
           "%d/%d" % (sum(alive), CYCLES))
        ok("进程仍在运行", p.poll() is None, "poll=%s" % (p.poll(),))

        logs = log_since(log_mark)
        bad = [s for s in logs if "30 秒未退出" in s or "放弃等待" in s]
        ok("日志里没有作业停不下来的记录", not bad, str(bad[-2:]))

        # 断开后还能正常载入一个角色（连接状态机没被带坏）
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        stat = ""
        t0 = time.time()
        while time.time() - t0 < 12:
            stat = txt(u.GetDlgItem(m, 191))          # IDC_STATUS
            if "已载入" in stat:
                break
            time.sleep(0.3)
        print("   状态栏：%r" % stat)
        ok("断开 12 轮后仍能载入角色", "已载入" in stat, stat)
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
