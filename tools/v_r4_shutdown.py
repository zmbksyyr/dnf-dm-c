# -*- coding: utf-8 -*-
"""R4-02 定向验证：退出时有作业没停下，就整段跳过释放 —— 不许再「一边 free 一边 recv」。

旧版 app_shutdown（4df0a0c）连着 8 次 job_stop，返回值一个都不看，接下来
db_undo_flush(1) → db_free_data(&g.data) → db_close(&g.db)：前一句要在这条连接上
再跑一次查询，后一句收的正是作业线程手里那 6 个 blob 和它的收发包缓冲。作业 30 秒
没退出的情况下走这一段，就是 use-after-free。

新版：stop_db_jobs()（conn/load/act/poll/bubble）+ job_stop(pvf/grab/npk)，
8 个里有任何一个没等到就 `if (stopped)` 整段跳过，只记一行日志；cfg_save 在这道闸门
**之前**（配置必须存下来），app_log/app_log_flush 和 DeleteCriticalSection 在闸门
**之后**（与连接无关，照做）。

构造（照 v_r3_jobstop.py 的成熟做法）：第二条连接 LOCK TABLES
taiwan_cain_2nd.inventory WRITE → 点「全服清错误物品」并确认 → 扫描作业（job_act）
卡在 metadata lock 上 → 发 WM_CLOSE（main.c:927 → app_shutdown）→ 等它把 30 秒
超时走完（脚本会慢，正常）。

断言：
  1) 日志里有「退出：有任务线程 30 秒未退出，跳过资源释放（连接与容器交给系统回收）」
  2) 日志里「退出」那行仍然在（与连接无关的收尾没被闸门吃掉）
  3) 进程**正常**退出：returncode == 0，不是 0xC0000005 / 0xC0000374
  4) cfg_save 仍然执行了 —— 外部把 settings.ini 的 [ui] poll_sec 改成 9，
     退出后必须被 g.cfg 里的 3 覆盖回去（cfg_save 在闸门之前，app.c:1565）
  5) 进程在 150 秒内退出（没卡死）

旧版对照：同一脚本在 4df0a0c 的 exe 上应该崩（非零返回码）或拿不到那行日志。

    python tools/v_r4_shutdown.py                                  # 新版（应 0 失败）
    DNF_GM_BUILD=<旧版目录> python tools/v_r4_shutdown.py          # 4df0a0c（应红）
"""
import ctypes
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt, db, click_at

BM_CLICK, WM_CLOSE = 0x00F5, 0x0010
IDC_CONN, IDC_CHLIST, IDC_STATUS = 104, 113, 191
G_SCANBAD = 1000 + 8 * 100 + 27        # 1827
CONFIRM = "扫描全服所有角色"

SKIP_LOG = "退出：有任务线程 30 秒未退出，跳过资源释放"
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


# ---------------- 弹窗 ----------------
def top_windows(main, cls="#32770"):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if c.value != cls:
            return True
        if u.GetWindow(h, 4) != main:
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


def wait_msgbox(main, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = top_windows(main)
        if w:
            return w[0], dlg_text(w[0])
        time.sleep(0.05)
    return None, ""


def close_box(main, btn=1, timeout=8.0):
    w = top_windows(main)
    if not w:
        return False
    u.PostMessageW(u.GetDlgItem(w[0], btn), BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not top_windows(main):
            return True
        time.sleep(0.05)
    return False


# ---------------- 库 / 日志 / 配置 ----------------
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

INI = os.path.join(os.path.dirname(EXE), "config", "settings.ini")


def ini_get(key, section="ui"):
    import configparser
    cp = configparser.RawConfigParser()
    cp.read(INI, encoding="utf-8")
    return cp.get(section, key, fallback=None)


def ini_set_raw(key, val, section="ui"):
    """按字节改一行，不动其它内容（cfg_save 落盘后要能比对同一个键）。"""
    with open(INI, "r", encoding="utf-8") as f:
        txt_all = f.read()
    lines = txt_all.splitlines()
    out, sec, hit = [], None, False
    for ln in lines:
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1]
        if sec == section and s.split("=")[0].strip() == key:
            out.append("%s =%s" % (key, val))
            hit = True
        else:
            out.append(ln)
    if not hit:
        raise RuntimeError("settings.ini 里没有 [%s] %s" % (section, key))
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def main():
    print("EXE =", EXE)
    p = None
    lock = None
    orig_ini = None
    try:
        with open(INI, "rb") as f:
            orig_ini = f.read()
    except FileNotFoundError:
        # 干净建出来的目录里没有 settings.ini（cfg_save 只在退出/改配置时才写它）。
        # 手工补一份最小配置，cfg_save 才有东西可覆盖；finally 里再删回「本来没有」。
        orig_ini = None
        with open(INI, "w", encoding="utf-8") as f:
            f.write("[db]\nhost =127.0.0.1\nport =3306\nuser =game\npwd =uu5!^%jg\n"
                    "charset =45\n\n[ui]\nfont =9\npoll_sec =3\nhotkey =Tab\n")
        print("   （settings.ini 原本不存在，已补一份最小配置，退出后删回原状）")
        ps0 = ini_get("poll_sec")
        ok("0 起点：settings.ini 读得到 [ui] poll_sec = %s（后面用它当 cfg_save 的证据）"
           % ps0, ps0 is not None, "%r" % ps0)
        if ps0 is None:
            return 1

        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        lv = u.GetDlgItem(m, IDC_CHLIST)
        t0 = time.time()
        n = 0
        while time.time() - t0 < 30 and n != 1:
            n = u.SendMessageW(lv, 0x1004, 0, 0)
            time.sleep(0.3)
        ok("1 已连接，角色列表 1 行", n == 1, "%s 行" % n)
        click_at(lv, 40, 12, 1.0)
        switch(m, 8)
        time.sleep(1.0)
        ok("1 扫描入口存在", bool(ctl(m, G_SCANBAD)), "控件 %d" % G_SCANBAD)

        # 先把外部痕迹埋好：cfg_save 一旦执行就会把它改回 g.cfg 里那份
        ini_set_raw("poll_sec", "9")
        ok("2 已把 settings.ini 的 [ui] poll_sec 外部改成 9",
           ini_get("poll_sec").strip() == "9", "%r" % ini_get("poll_sec"))

        # 用第二条连接把扫描拖住
        lock = db()
        lock.cursor().execute("LOCK TABLES taiwan_cain_2nd.inventory WRITE")
        u.PostMessageW(ctl(m, G_SCANBAD), BM_CLICK, 0, 0)
        b, t = wait_msgbox(m, 8.0)
        ok("3 弹出扫描确认框", CONFIRM in t, t[:50])
        close_box(m, 1)
        blocked, st = wait_blocked("taiwan_cain_2nd.inventory")
        ok("3 扫描作业真的卡在库上（job_act 停不下来）", blocked, st)

        _LOGP[0] = logfile()
        off = log_off()[1]
        print("   已发 WM_CLOSE，等 job_stop 的 30 秒超时…（这条要等满，正常）")
        t0 = time.time()
        u.PostMessageW(m, WM_CLOSE, 0, 0)
        rc = None
        while time.time() - t0 < 150:
            rc = p.poll()
            if rc is not None:
                break
            time.sleep(0.5)
        sec = time.time() - t0
        print("   等了 %.1f 秒，returncode = %s" % (sec, rc))

        ok("4 进程退出了（没有卡死）", rc is not None, "%.1f 秒" % sec)
        # main.c:938 那句提前 job_stop(&g.job_act) 已删：旧版会在 main.c 等一次 30 秒、
        # 进 app_shutdown 再等一次 30 秒（实测 60.0 秒）。新版只等一次。
        ok("4 只等了一次 30 秒（main.c 那句重复的 job_stop 已删）", rc is not None and sec <= 45,
           "%.1f 秒" % sec)
        if rc is None:
            p.kill()
            p.wait(5)
            print("\n进程 150 秒没退出，后面几条无法判定")
            return 1

        print("   退出码：0x%08X（%s）" % (rc & 0xFFFFFFFF,
                                      CRASH.get(rc & 0xFFFFFFFF, "正常")))
        ok("5 进程**正常**退出：returncode == 0（不是崩）", rc == 0,
           "rc=%s / 0x%08X" % (rc, rc & 0xFFFFFFFF))
        ok("5 不是 use-after-free 那几个崩溃码（0xC0000005 / 0xC0000374）",
           (rc & 0xFFFFFFFF) not in CRASH, "0x%08X" % (rc & 0xFFFFFFFF))

        text = log_since(off)
        skip = [l for l in text.splitlines() if SKIP_LOG in l]
        print("---- 这次运行新写的日志 ----")
        print("\n".join(text.splitlines()[-8:]))
        ok("6 日志里有「退出：有任务线程 30 秒未退出，跳过资源释放…」",
           bool(skip), skip[0] if skip else "(日志里没有)")
        tail = [l for l in text.splitlines() if "退出" in l]
        ok("6 日志里的「退出」收尾行仍然在（闸门之后的收尾照做了）",
           any(l.rstrip().endswith("退出") for l in text.splitlines()),
           repr(tail[-1:]))

        ps1 = ini_get("poll_sec")
        ok("7 cfg_save 仍然执行了（外部改的 poll_sec=9 被 g.cfg 的 %s 覆盖回去）" % ps0,
           ps1 is not None and ps1.strip() == ps0.strip(), "%r" % ps1)
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
        try:
            if orig_ini is None:
                if os.path.exists(INI):
                    os.remove(INI)
                    print("   已删掉测试补建的 settings.ini（原本不存在）")
            else:
                with open(INI, "wb") as f:
                    f.write(orig_ini)
                print("   已把 settings.ini 还原成原始字节（%d 字节）" % len(orig_ini))
        except Exception as e:
            print("!! settings.ini 还原失败：%s" % e)

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
