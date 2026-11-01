# -*- coding: utf-8 -*-
"""回归总入口：把四轮攒下来的 v_*.py 跑成一套，逐个记 rc + 失败项数，末尾出汇总表。

    python tools/run_regress.py              # 跑「每次都跑」那一组（默认）
    python tools/run_regress.py --group all  # 连「需人工」那组一起跑
    python tools/run_regress.py --group manual
    python tools/run_regress.py --only v_log # 只跑一个（不带 .py 也行）
    python tools/run_regress.py --timeout 600

退出码真实反映结果：任一用例失败 → 1，全绿 → 0。

## 为什么要分两组

不是所有脚本都适合进「每次都跑」。判据只有三条，命中任意一条就划到 manual：

  1. **依赖真实鼠标 / 真实输入桌面** —— 无头会话里跑出来的「跳过」不等于通过，
     把它塞进默认组等于拿跳过冒充绿。
  2. **依赖一份特定的库状态** —— 库被前一个脚本改过就会假红，得有人先看一眼。
  3. **只打印不判定 / 记录型探针** —— 它的价值在于「记录那个缺陷当时的样子」，
     缺陷修完之后再跑一百次也不会变，重复跑只是噪音。

其余的进 ALWAYS：不依赖特定库状态（自己备份、自己还原）、结果可复现、
判定由脚本自己做。

## 各脚本的归类理由

ALWAYS（自还原，可复现）
  v_selfcheck      临时改 letter 表名再改回，还原写在 finally 里
  v_features       16 项全先“改脏”后落库，跑完逐项复原
  v_gmwrite        改金币后改回原值
  v_pwd            只读控件属性 + SETTEXT 读回，不写库
  v_log            只读今天的日志文件与日志页文本比对
  v_pageall        逐个页签进一次再切回，只数控件的颜色种类
  v_queue / v_qadd / v_stable / v_pollcard / v_browse
                   都走 GM 页/发放页写路径，按 pages.first_char_no 认当前角色，
                   原值记下来跑完写回
  v_align / v_geom / v_left / v_top / v_tab / v_pages
                   纯量矩形，改布局后第一件该跑的东西
  v_type / v_tasknum
                   走 PVF 判定，只读发放页/邮件页的只读文本框
  v_r1_conn / v_r1_lists / v_r1_quest / v_r1_scan / v_r1_unlock
                   R1 的定向脚本，都用「第二条连接 LOCK TABLES」把意图变成可控量，
                   不靠 sleep 抢时序；改过的行在 finally 里按快照还原
  v_r1_name / v_r3_emoji
                   临时改名 → 断言 → 改回 laoxx，最后比 HEX
  v_r2_charset / v_r2_dlg / v_r2_disc
                   写路径字符集与对话框框架的定向脚本，自还原
  v_r3_clone / v_r4_clone_keys
                   克隆出来的是临时角色，finally 里删掉；依赖的起点状态
                   （charac_info 1 行 + delivery 17 行挂 charac_no=1）是
                   brief 里写明的常设验收前提，不是随机状态
  v_r3_jobstop / v_r3_sticky / v_r4_connstop / v_r4_levjob / v_r4_name / v_r4_shutdown
                   都用「卡住作业 → 点断开/退出」的构造，锁释放后库回到原样

## 一个已知的先后依赖（排这张表时踩到的）

`v_r1_name` 会临时插一个角色再删掉，MySQL 的 `AUTO_INCREMENT` 因此从 brief 里的
9919 涨到 9920。而 `v_r4_clone_keys` / `v_r4_levjob` 的起点断言写死
`AUTO_INCREMENT=9919` —— 它们排在 `v_r1_name` 后面的话，整套跑下来必假红，
**不是被测程序出问题，是顺序**。所以下面 ALWAYS 的次序是刻意排的：这两个放在最前，
`v_r1_name` 放后面。整套跑完一轮再跑第二轮时，计数器已经停在 9920 了，用
`--reset-ai` 把它拽回 9919（默认不拽：动自增计数器是一次 DDL，不静默替你做）。

MANUAL（命中上面三条之一）
  v_hover          依赖真实鼠标停留 + PVF 旁的 ImagePacks2；本机光标被占时它标“跳过”
  v_grabauto       热键与 --hover 两组要真实输入桌面，RDP 断开时自动跳过
  v_numkey         前台窗口 + keybd_event 真实键盘，同 v_hover 的输入前提
  v_norow          断言前提是“account_cargo 里没有 m_id=1 的行”，这份空表状态
                   会被其它脚本抹掉（v_features 建过再删，但只要中途失败就会留下）
  v_bubble         会把 account=1 的 login_status 置在线并改 cera_point，
                   在线状态正是本轮 R5-005 要校验的那一份；而且它会往 postal 插行，
                   与邮件页行数断言抢同一份数据
  v_r3_probe_stale_name
                   记录型探针：只打印不判定。它记的是“改名被拒后界面停在假名字”，
                   那个缺陷第 4 轮（R4-01）已经修掉，现在该打印「已回滚」而不是
                   「停在假名字」。它是 R4-01 的人工确认证据，不是每轮该跑的回归
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 顺序有讲究：两个克隆脚本认死 AUTO_INCREMENT=9919，必须排在 v_r1_name 之前
# （v_r1_name 会临时建一个角色，把计数器顶到 9920）。详见文件头那段。
ALWAYS = [
    "v_r4_clone_keys", "v_r4_levjob",
    "v_selfcheck", "v_features", "v_gmwrite", "v_pwd", "v_log", "v_pageall",
    "v_align", "v_geom", "v_left", "v_top", "v_tab", "v_pages",
    "v_type", "v_queue", "v_stable", "v_pollcard",
    "v_r1_conn", "v_r1_lists", "v_r1_quest", "v_r1_scan", "v_r1_unlock", "v_r1_name",
    "v_r2_charset", "v_r2_dlg", "v_r2_disc",
    "v_r3_clone", "v_r3_emoji", "v_r3_jobstop", "v_r3_sticky",
    "v_r4_connstop", "v_r4_name", "v_r4_shutdown",
]

MANUAL = [
    "v_hover", "v_grabauto", "v_numkey", "v_norow", "v_bubble", "v_r3_probe_stale_name",
    # 这三个都依赖 PVF 物料（Script.pvf），本机没选 PVF 时它们的失败是环境而不是缺陷：
    #   v_tasknum / v_browse —— 按名字查任务材料，无 PVF 直接报「未加载 PVF」
    #   v_qadd              —— 「经验清零」那项走 db.c 的 exp 写路径，而 db.c:1008-1009
    #                          明写无 PVF 时不动 exp，所以断言缺的是「先选 PVF」这个前提
    # 塞进 ALWAYS 等于拿环境缺失冒充产品回归，先选了 PVF 再单跑它们。
    "v_tasknum", "v_browse", "v_qadd",
]

# 通用坏味道：脚本自己打印的失败标记。这些脚本大多是「只 print 不 exit」，
# 所以 rc=0 一律不作为通过依据，只看输出。
BAD_GENERIC = [
    (re.compile(r"\bFAIL\b"), "FAIL"),
    (re.compile(r"Traceback \(most recent call last\)"), "Traceback"),
    (re.compile(r"(?m)^\s*!!\s"), "!!"),
    (re.compile(r"没生效\s*[1-9]"), "没生效>0"),
    (re.compile(r"失败\s*[1-9]"), "失败>0"),
]
# 个别脚本的失败行不带 FAIL / !!，单独点出来
BAD_EXTRA = {
    "v_log":      [(re.compile(r"倒序一致：\s*否"), "倒序一致：否")],
    "v_r1_scan":  [(re.compile(r"闸不诚实|该拦没拦"), "闸不诚实")],
    "v_norow":    [(re.compile(r"报了「完成」|没有报错"), "没报错")],
    # 这几个依赖 PVF：本机没配就没法给你真的结论，标出来别把它当绿
    "v_tasknum":  [(re.compile(r"未加载 PVF|先选 PVF"), "未加载 PVF")],
    "v_browse":   [(re.compile(r"未加载 PVF|先选 PVF"), "未加载 PVF")],
    "v_grabauto": [(re.compile(r"未加载 PVF|先选 PVF"), "未加载 PVF")],
    "v_pages":    [(re.compile(r"未加载 PVF|先选 PVF"), "未加载 PVF")],
}
# 这些是脚本自己宣告通过的行，只用来在汇总表里多给一句证据
GOOD_LINE = {
    "v_features":  re.compile(r"合计 16 项"),
    "v_pageall":   re.compile(r"可疑控件合计 = "),
    "v_log":       re.compile(r"倒序一致："),
    "v_r3_clone":  re.compile(r"判定分布|CK_"),
    "v_r4_clone_keys": re.compile(r"判定分布|CK_"),
    "v_r1_lists":  re.compile(r"合计失败 \d+ 项"),
    "v_selfcheck": re.compile(r"已改回 letter"),
}


def kill_stale():
    """跑 GUI 之前先清场：占住 exe 会让下一次构建失败，占住库会让脚本假红。"""
    subprocess.run(["taskkill", "/F", "/IM", "DNF_GM.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.6)


def run_one(name, timeout, logdir):
    t0 = time.time()
    kill_stale()
    try:
        cp = subprocess.run([sys.executable, os.path.join(HERE, name + ".py")],
                            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout, encoding="utf-8", errors="replace")
        out, rc = cp.stdout or "", cp.returncode
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or "") + "\n[run_regress] 超时 %ss" % timeout, -9
    elapsed = time.time() - t0

    with open(os.path.join(logdir, name + ".log"), "w", encoding="utf-8") as f:
        f.write(out or "(no output)")

    marks = []
    for pat, tag in BAD_GENERIC + BAD_EXTRA.get(name, []):
        for m in pat.finditer(out or ""):
            line = out[:m.start()].count("\n") + 1
            marks.append("%s@L%d" % (tag, line))
    good = ""
    if name in GOOD_LINE:
        m = GOOD_LINE[name].search(out or "")
        if m:
            good = (out[m.start():].split("\n")[0]).strip()[:78]

    return {"name": name, "rc": rc, "secs": round(elapsed, 1), "marks": marks,
            "good": good, "out": out or ""}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", default="always", choices=["always", "manual", "all"])
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--reset-ai", action="store_true",
                    help="跑之前把 charac_info 的 AUTO_INCREMENT 拽回 9919（见文件头注释）")
    ap.add_argument("--logdir", default=os.path.join(ROOT, "..", "_pipeline",
                                                     "round-05", "regress-logs"))
    a = ap.parse_args()

    if a.only:
        names = [n[:-3] if n.endswith(".py") else n for n in a.only]
    elif a.group == "always":
        names = list(ALWAYS)
    elif a.group == "manual":
        names = list(MANUAL)
    else:
        names = ALWAYS + MANUAL

    logdir = os.path.abspath(a.logdir)
    os.makedirs(logdir, exist_ok=True)
    print("run_regress: 共 %d 个用例，日志落在 %s\n" % (len(names), logdir))

    if a.reset_ai:
        import pymysql
        c = None
        try:
            c = pymysql.connect(host="127.0.0.1", port=3306, user="game",
                                password="uu5!^%jg", charset="utf8mb4", autocommit=True)
            cu = c.cursor()
            cu.execute("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                       "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
            was = cu.fetchone()[0]
            cu.execute("ALTER TABLE taiwan_cain.charac_info AUTO_INCREMENT=9919")
            print("AUTO_INCREMENT: %s → 9919" % was)
        finally:
            if c:
                c.close()

    rows = []
    for n in names:
        r = run_one(n, a.timeout, logdir)
        rows.append(r)
        bad = r["rc"] != 0 or r["marks"]
        print("%-22s rc=%-4s %6.1fs  %s%s" % (
            n, r["rc"], r["secs"],
            "FAIL  " if bad else "pass  ",
            ",".join(r["marks"][:6]) if r["marks"] else (r["good"] or "")))
        if bad:
            tail = [l for l in r["out"].strip().split("\n") if l.strip()][-6:]
            for l in tail:
                print("      | " + l)

    print("\n" + "=" * 96)
    print("%-24s %6s %8s  %s" % ("用例", "rc", "耗时s", "结果"))
    print("-" * 96)
    bads = []
    for r in rows:
        bad = r["rc"] != 0 or bool(r["marks"])
        if bad:
            bads.append(r["name"])
        print("%-24s %6s %8.1f  %s" % (
            r["name"], r["rc"], r["secs"],
            "FAIL  " + ",".join(r["marks"][:6]) if bad else "pass  " + (r["good"] or "")))
    print("-" * 96)
    print("合计 %d 个：pass %d · FAIL %d" % (len(rows), len(rows) - len(bads), len(bads)))
    if bads:
        print("失败清单：" + ", ".join(bads))
    print("=" * 96)
    return 1 if bads else 0


if __name__ == "__main__":
    sys.exit(main())
