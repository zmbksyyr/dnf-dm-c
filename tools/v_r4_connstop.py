# -*- coding: utf-8 -*-
"""R4-03 定向验证：断开时要等 job_conn —— 连接作业正在查库时点「断开」不许踩缓冲。

旧版 app_disconnect 只停 load / act / poll / bubble 四个，漏了 job_conn。
「点连接后立刻点断开」这个窗口里 db_close 收的正是连接作业手里那条 socket 的
收发包缓冲（db_open 的 handshake 和紧随其后的自检/角色列表查询都在上面）。

新版把停止集合抽成 stop_db_jobs()（conn/load/act/poll/bubble），断开和退出共用一份，
于是 job_conn 被补上。

怎么把 job_conn 拖住 30 秒以上（否则 job_stop 一下就等到，看不出差别）：
  SO_RCVTIMEO 在握手结束后被设成 60 秒（src/mysql.c:411），而握手阶段只有 2000ms，
  所以「连得上但永不回包」这类构造反而会让它 2 秒就失败。改成用第二条连接
  LOCK TABLES taiwan_cain.charac_info WRITE：握手和 information_schema 自检都不碰
  这张表，w_conn 会一路走到 db_list_chars（src/app.c:566 `FROM taiwan_cain.charac_info`）
  卡在 metadata lock 上，最长 60 秒 —— 足够 job_stop 等满 30 秒。

断言（新版）：
  1) job_conn 真的卡在 charac_info 的 metadata lock 上（PROCESSLIST 可见）
  2) 点「断开」后日志里有「断开取消：后台任务 30 秒内没有退出，连接与队列都保留」
  3) 状态栏是拒绝文案「后台任务还在跑，稍后再断开」，**不是**「已断开数据库」
  4) 进程还活着，退出码不是崩溃码
  5) 放锁后连接作业收尾，再点一次「断开」能成功（拒绝是可重试的）

旧版（4df0a0c）同一脚本：app_disconnect 根本不等 job_conn，第 2、3 条必红。

    python tools/v_r4_connstop.py                                  # 新版（应 0 失败）
    DNF_GM_BUILD=<旧版目录> python tools/v_r4_connstop.py          # 4df0a0c（应红）
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, db

BM_CLICK, WM_CLOSE = 0x00F5, 0x0010
IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191

CANCEL_LOG = "断开取消：后台任务 30 秒内没有退出"
REFUSE_TXT = "后台任务还在跑，稍后再断开"
CRASH = {0xC0000005: "STATUS_ACCESS_VIOLATION", 0xC0000374: "STATUS_HEAP_CORRUPTION",
         0xC0000006: "STATUS_IN_PAGE_ERROR", 0xC0000409: "STATUS_STACK_BUFFER_OVERRUN"}

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def info(tag, detail=""):
    print("--   %s%s" % (tag, (" | " + detail) if detail else ""))


def status(main):
    return txt_w(u.GetDlgItem(main, IDC_STATUS), 512)


def txt_w(h, cap=256):
    b = ctypes.create_unicode_buffer(cap)
    u.SendMessageW(h, 0x000D, cap, ctypes.addressof(b))
    return b.value


def processlist():
    c = db()
    try:
        cur = c.cursor()
        cur.execute("SELECT ID,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                    "WHERE COMMAND='Query'")
        return cur.fetchall()
    finally:
        c.close()


def wait_blocked(marker, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, st, inf in processlist():
            if marker in inf and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, ""


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


def main():
    print("EXE =", EXE)
    p = None
    lock = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        time.sleep(1.0)

        # 先把 charac_info 锁住，再点连接 → w_conn 会卡在 db_list_chars 上
        lock = db()
        lock.cursor().execute("LOCK TABLES taiwan_cain.charac_info WRITE")
        info("已 LOCK TABLES taiwan_cain.charac_info WRITE（第二条连接）")

        _LOGP[0] = logfile()
        off = log_off()[1]
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        blocked, st = wait_blocked("taiwan_cain.charac_info")
        ok("1 连接作业真的卡在库上（job_conn 停在 db_list_chars 里）", blocked, st)
        if not blocked:
            print("\n没能拖住连接作业，后面的断言无意义")
            return 1

        # 连接作业还在跑 —— 这时候点「断开」
        u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)
        print("   已点断开，等 job_stop 的 30 秒超时…（这条要等满，正常）")
        time.sleep(34.0)
        st_txt = status(m)
        print("   状态栏 = %r" % st_txt)
        print("   进程 poll = %s" % (p.poll(),))

        text = log_since(off)
        cancel = [l for l in text.splitlines() if CANCEL_LOG in l]
        print("---- 这次运行新写的日志（最后 6 行） ----")
        print("\n".join(text.splitlines()[-6:]))

        ok("2 日志里有「断开取消：后台任务 30 秒内没有退出，连接与队列都保留」",
           bool(cancel), cancel[0] if cancel else "(日志里没有)")
        ok("3 状态栏是拒绝文案「%s」，不是「已断开数据库」" % REFUSE_TXT,
           REFUSE_TXT in st_txt and "已断开数据库" not in st_txt, repr(st_txt))
        rc = p.poll()
        ok("4 进程还活着（旧版在这里会 db_close 掉连接作业正用的那份缓冲）", rc is None,
           "poll=%s" % (rc,))
        if rc is not None:
            print("   退出码：0x%08X（%s）" % (rc & 0xFFFFFFFF,
                                          CRASH.get(rc & 0xFFFFFFFF, "正常")))
            ok("4 不是崩溃码", (rc & 0xFFFFFFFF) not in CRASH, "0x%08X" % (rc & 0xFFFFFFFF))

        # 放锁：连接作业收尾 → 再点一次断开必须能成功（拒绝是可重试的）
        try:
            lock.cursor().execute("UNLOCK TABLES")
            lock.close()
            lock = None
        except Exception:
            pass
        t0 = time.time()
        disc = ""
        u.PostMessageW(u.GetDlgItem(m, IDC_DISC), BM_CLICK, 0, 0)
        while time.time() - t0 < 45:
            rc = p.poll()
            if rc is not None:
                break
            disc = status(m)
            if "已断开数据库" in disc:
                break
            time.sleep(0.3)
        print("   放锁后状态栏 = %r / poll = %s" % (disc, p.poll()))
        ok("5 放锁后「再点一次断开」能成功（拒绝是可重试的）",
           "已断开数据库" in disc, repr(disc))
        ok("5 进程还活着且没崩", p.poll() is None,
           "poll=%s" % (p.poll(),))
    finally:
        try:
            if lock:
                try:
                    lock.cursor().execute("UNLOCK TABLES")
                except Exception:
                    pass
                lock.close()
        except Exception:
            pass
        try:
            if p and p.poll() is None:
                p.kill()
                p.wait(5)
        except Exception:
            pass

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
