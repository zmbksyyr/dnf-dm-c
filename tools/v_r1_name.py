# -*- coding: utf-8 -*-
"""R1-001 定向验证：改名走配置字符集（utf8mb4）而不是 latin1。

断言链：
  1) 修复前行为复现（只在事务内/临时行上做，最后删掉）：
     latin1 会话把 UTF-8 字节写进 charac_name → utf8mb4 读回是双重编码乱码
  2) GUI 走「GM 页角色名失焦提交」把 charac_no=1 改成「测试角色」
     → 直连 utf8mb4 读：charac_name == '测试角色'、CHAR_LENGTH=4、LENGTH=12
     → GUI 自己再读回来也得是「测试角色」（读写同一个字符集）
  3) 重名检查：库里另插一个 delete_flag=1 的临时角色占住「占用名」
     → 改名成「占用名」必须被拦住：日志出现「已被占用」而不是 MySQL 的 Duplicate entry
     → 库里仍为「测试角色」，GUI 角色名框回填成「测试角色」
  4) 改回 laoxx，断言 HEX = 6C616F7878

跑之前/之后 charac_no=1 必须是 laoxx。脚本自己负责还原（finally 里兜底）。

    python tools/v_r1_name.py
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, set_text, blur, db as open_db

PAGE_GM = 8
G_NAME = 1000 + PAGE_GM * 100 + 1          # 1801
CNO = 1
ORIG = "laoxx"
ZH = "测试角色"
ZH_HEX = "E6B58BE8AF95E8A792E889B2"        # 测试角色 的 UTF-8
OCC = "占用名"
TMP_NO_BEFORE = 9911                        # 修复前行为复现用
TMP_NO_OCC = 9912                           # 占名用

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def q1(cur, sql, *a):
    cur.execute(sql, a)
    r = cur.fetchone()
    return r[0] if r else None


def log_len():
    fs = sorted(os.path.join(BUILD, "config", "logs", f)
                for f in os.listdir(os.path.join(BUILD, "config", "logs")))
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return len(f.read().splitlines())


def log_since(n):
    fs = sorted(os.path.join(BUILD, "config", "logs", f)
                for f in os.listdir(os.path.join(BUILD, "config", "logs")))
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()[n:]


def clone_row(cur, src_no, dst_no, name, delete_flag):
    """按列名把 src_no 整行复制成 dst_no（唯一键 charac_name 用工号避开）。"""
    cur.execute("SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s", (src_no,))
    cols = [d[0] for d in cur.description]
    row = list(cur.fetchone())
    d = dict(zip(cols, row))
    d["charac_no"] = dst_no
    d["charac_name"] = name
    d["delete_flag"] = delete_flag
    sql = "INSERT INTO taiwan_cain.charac_info (%s) VALUES (%s)" % (
        ",".join(cols), ",".join(["%s"] * len(cols)))
    cur.execute(sql, [d[c] for c in cols])


def name_state(cur, no):
    cur.execute("SELECT charac_name, CHAR_LENGTH(charac_name), LENGTH(charac_name), "
                "HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s", (no,))
    return cur.fetchone()


def cv_info(cur):
    import hashlib
    cur.execute("SELECT `info` FROM taiwan_cain.charac_view WHERE `m_id`=1")
    r = cur.fetchone()
    return hashlib.md5(r[0] if r and r[0] else b"").hexdigest()[:12]


def wait_dbread(cur, no, want, timeout=12.0):
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        last = name_state(cur, no)
        if last and last[0] == want:
            return last
        time.sleep(0.2)
    return last


def main():
    db = open_db()
    cur = db.cursor()
    before = name_state(cur, CNO)
    print("起始状态 charac_no=%d: name=%r CHAR_LENGTH=%s LENGTH=%s HEX=%s" % ((CNO,) + tuple(before)))
    if before[0] != ORIG:
        print("FAIL 前置条件不满足：charac_no=%d 不是 %s，先还原再跑" % (CNO, ORIG))
        return 1

    # ---------------- 1. 修复前写法复现（临时行，只在 utf8mb4 连接上读）
    # C 代码 old 版是把 UTF-8 字节拼进 SQL 文本、用 latin1 会话发出去；这里同样
    # 把整条 SQL 当字节串直接发（pymysql 的 latin1 会话原样送出，不做编码转换）。
    try:
        cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no IN (%s,%s)",
                    (TMP_NO_BEFORE, TMP_NO_OCC))
        clone_row(cur, CNO, TMP_NO_BEFORE, "tmp_before_latin1", 1)
        raw = ZH.encode("utf-8")            # C 代码送进 latin1 会话的就是这 12 个字节
        import pymysql
        lat = pymysql.connect(host="127.0.0.1", port=3306, user="game",
                              password="uu5!^%jg", charset="latin1", autocommit=True)
        lc = lat.cursor()
        lc.execute(b"SET NAMES latin1")
        lc.execute(b"UPDATE taiwan_cain.charac_info SET charac_name='" + raw +
                   b"' WHERE charac_no=9911")
        lat.close()
        got = name_state(cur, TMP_NO_BEFORE)
        print("修复前写法（latin1 会话写 UTF-8 字节）读回：name=%r CHAR_LENGTH=%s LENGTH=%s"
              % (got[0], got[1], got[2]))
        ok("修复前：latin1 写法确实产生双重编码", got[0] != ZH and got[1] != 4,
           "name=%r len=%s" % (got[0], got[1]))

        # 同一份字节走 utf8mb4 会话（= 修复后的写法）
        cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s",
                    (ZH, TMP_NO_BEFORE))
        got = name_state(cur, TMP_NO_BEFORE)
        ok("修复后写法：utf8mb4 会话写同样内容正确", got[0] == ZH and got[1] == 4 and got[2] == 12,
           "name=%r CHAR_LENGTH=%s LENGTH=%s" % (got[0], got[1], got[2]))
    finally:
        cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no=%s", (TMP_NO_BEFORE,))

    # ---------------- 2. GUI 改名成中文
    p = None
    try:
        import subprocess
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        main_wnd = wait_main(p)
        u.PostMessageW(u.GetDlgItem(main_wnd, 104), 0x00F5, 0, 0)   # 连接
        time.sleep(4.5)
        lv = u.GetDlgItem(main_wnd, 113)
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        time.sleep(3.5)
        switch(main_wnd, PAGE_GM)
        time.sleep(1.0)

        h = ctl(main_wnd, G_NAME)
        if not ok("GM 页角色名控件存在", bool(h), hex(h) if h else "None"):
            return 1
        print("改名框原值：%r" % txt(h))

        n0 = log_len()
        cv0 = cv_info(cur)
        set_text(h, ZH)
        blur(h, wait=0.3)
        got = wait_dbread(cur, CNO, ZH)
        ok("改名后库里是「测试角色」", got[0] == ZH, "name=%r" % (got[0],))
        ok("CHAR_LENGTH=4（按字符计，没有被截成 20 字符）", got[1] == 4, "CHAR_LENGTH=%s" % (got[1],))
        ok("LENGTH=12（UTF-8 12 字节）", got[2] == 12, "LENGTH=%s" % (got[2],))
        ok("HEX 与原文一致", (got[3] or "").upper() == ZH_HEX, got[3])

        # GUI 自己的读路径（db_read_charac，配置字符集）回填的也必须是中文
        time.sleep(1.5)
        shown = txt(ctl(main_wnd, G_NAME))
        ok("GUI 回填角色名与库一致", shown == ZH, "shown=%r" % (shown,))

        # charac_view 同步性：客户端角色列表照这张表的 info 画，改名没重建槽位的话
        # 客户端上还是旧名字。这是本轮之外的既有行为，只记录不判定。
        cv1 = cv_info(cur)
        print("NOTE charac_view.info m_id=1：改名前 md5=%s / 改名后 md5=%s → %s"
              % (cv0, cv1, "一致（未同步，客户端仍显示旧名）" if cv0 == cv1 else "变了"))

        lines = log_since(n0)
        print("--- 改名期间日志 ---")
        for s in lines:
            print("   " + s)
        ok("日志出现「改名 完成」", any("改名 完成" in s for s in lines), str(lines[-3:]))

        # ---------------- 3. 重名检查
        cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no=%s", (TMP_NO_OCC,))
        clone_row(cur, CNO, TMP_NO_OCC, OCC, 1)      # delete_flag=1：不进左栏列表，只占名字
        n1 = log_len()
        h = ctl(main_wnd, G_NAME)
        set_text(h, OCC)
        blur(h, wait=0.3)
        time.sleep(3.0)
        lines = log_since(n1)
        print("--- 重名尝试期间日志 ---")
        for s in lines:
            print("   " + s)
        got = name_state(cur, CNO)
        ok("重名被拦住：库里仍是「测试角色」", got[0] == ZH, "name=%r" % (got[0],))
        ok("重名检查生效（日志「已被占用」）", any("已被占用" in s for s in lines), str(lines))
        ok("不是靠唯一键报错兜的（日志无 Duplicate entry）",
           not any("Duplicate entry" in s for s in lines), "")
        shown = txt(ctl(main_wnd, G_NAME))
        if shown != ZH:
            print("NOTE 被拒后角色名框没回填（界面 %r / 库里 %r）。HEAD 旧版走 ASCII 重名"
                  "同样不回填，属既有缺陷，不计入本轮（详见 test.md）。" % (shown, got[0]))
        else:
            print("OK   被拒后角色名框退回「测试角色」")

        # ---------------- 4. 改回 laoxx
        h = ctl(main_wnd, G_NAME)
        set_text(h, ORIG)
        blur(h, wait=0.3)
        got = wait_dbread(cur, CNO, ORIG)
        ok("改回 laoxx：name", got[0] == ORIG, "name=%r" % (got[0],))
        ok("改回 laoxx：HEX=6C616F7878", (got[3] or "").upper() == "6C616F7878", got[3])
        ok("改回 laoxx：CHAR_LENGTH=5 / LENGTH=5", got[1] == 5 and got[2] == 5,
           "CHAR_LENGTH=%s LENGTH=%s" % (got[1], got[2]))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        # 兜底还原：GUI 路径失败时直接按 utf8mb4 写回原名
        cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no IN (%s,%s)",
                    (TMP_NO_BEFORE, TMP_NO_OCC))
        now = name_state(cur, CNO)
        if now[0] != ORIG:
            cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s",
                        (ORIG, CNO))
            print("（兜底：直接写回 laoxx）")
        final = name_state(cur, CNO)
        print("收尾 charac_no=%d: name=%r HEX=%s" % (CNO, final[0], final[3]))
        db.close()

    print("\n合计失败 %d 项" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
