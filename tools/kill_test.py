# -*- coding: utf-8 -*-
"""硬杀一致性：写操作卡在库上时直接结束进程，检查数据有没有写一半。

    python tools\\kill_test.py

做法：锁住 charac_stat，让「补满疲劳」的 UPDATE 停在等锁；taskkill /F 杀掉程序；
放锁后检查该列仍是原值（服务端回滚），再重启一次确认还能正常连库读数据。

退出码 0 = 数据未变、重启正常。
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from regress import core      # noqa: E402

u = core.u
TABLE = "taiwan_cain.charac_stat"


def main():
    db = core.db()
    cur = db.cursor()
    lk = core.db()
    lc = lk.cursor()
    try:
        cur.execute("SELECT used_fatigue FROM %s WHERE charac_no=1" % TABLE)
        row = cur.fetchone()
        if not row:
            print("测试库里没有 charac_no=1 的角色")
            return 2
        before = row[0]
        print("used_fatigue 原值:", before)

        core.kill_stale()
        p, m = core.boot()
        try:
            lv = u.GetDlgItem(m, 113)
            if not core.wait_until(lambda: core.rows(lv) > 0, 25.0, 0.2):
                print("启动后没有读到角色列表")
                return 1
            core.click_at(lv, 40, 40, wait=2.0)
            core.switch(m, 8, wait=1.0)
            lc.execute("LOCK TABLES %s WRITE" % TABLE)
            core.press(core.ctl(m, 1819), 1.0)        # 补满疲劳：会卡在 UPDATE
            time.sleep(1.5)
            cur.execute("SELECT COUNT(*) FROM information_schema.PROCESSLIST "
                        "WHERE STATE LIKE '%%metadata lock%%'")
            blocked = cur.fetchone()[0]
            print("等锁的查询数:", blocked)
            if not blocked:
                print("写操作没有被挡住，测试前提不成立")
                return 1
            subprocess.run(["taskkill", "/F", "/PID", str(p.pid)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.0)
            lc.execute("UNLOCK TABLES")
            time.sleep(1.0)
        finally:
            try:
                lc.execute("UNLOCK TABLES")
            except Exception:
                pass
            core.kill_stale()

        cur.execute("SELECT used_fatigue FROM %s WHERE charac_no=1" % TABLE)
        after = cur.fetchone()[0]
        print("杀进程后 used_fatigue:", after)
        ok = after == before
        print("  数据未被写一半:", ok)

        p2, m2 = core.boot()
        try:
            lv = u.GetDlgItem(m2, 113)
            again = core.wait_until(lambda: core.rows(lv) > 0, 25.0, 0.2)
            print("  重启后能连库读角色:", again)
            ok = ok and again
        finally:
            try:
                p2.kill()
            except Exception:
                pass
            core.kill_stale()
        print("结果：%s" % ("通过" if ok else "有异常"))
        return 0 if ok else 1
    finally:
        db.close()
        lk.close()


if __name__ == "__main__":
    sys.exit(main())
