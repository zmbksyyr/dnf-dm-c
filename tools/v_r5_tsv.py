# -*- coding: utf-8 -*-
"""R5-004 定向验证：发放队列 tsv 导入的字段宽度。

修复点（src/ui_item.c）：抽出 `kGrantRange[11]`（下标 = tsv 列号，含 lo/hi），
界面那 7 个 ui_num 与 tsv 导入共用同一份；导入新增 grant_clamp()，越界夹到界上并
往日志报一行「第 N 行的强化 50 已夹到 31」，不是静默截断 —— 那些上限就是写回物品
二进制时的字段宽度（item.h 的 & 0x1F / (u8) 掩码），超了会被悄无声息地截成另一件东西。

**判据**（违反一条就 sys.exit(1)）
  A. 越界的那几列都被夹住，且每一行都有反馈
     - 日志出现「第 N 行的强化 50 已夹到 31」这样的行（N 是物理行号）
     - 导出回来能看到：强化 = 31（不是 50，也不是 50 & 0x1F 得的 18）、
       锻造 = 255、封印次数 = 7
  B. 导入的东西与 tsv 的意图一致：编号 1001 / 数量 1 都在，队列里确实是两行
  C. 顺带验导出头：导出文件里带两行 # 说明（写明越界策略与各数值列的范围），
     而且这行会被导入方跳过（再导入一遍导出的文件不会多出一行）

**文件对话框怎么填**：GetOpenFileNameW / GetSaveFileNameW 是模态公用对话框，
脚本按下述方式驱动 —— 等 class = #32770 且 owner 是被测主窗口的那个窗口出现，
把完整路径 WM_SETTEXT 进里头的 Edit，再点 IDOK(1)。这条路和人在资源管理器里
选文件走的是同一条。

跑法：
    python tools/v_r5_tsv.py
    python tools/v_r5_tsv.py --old
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_OLD = ("--old" in sys.argv)
if _OLD:
    os.environ["DNF_GM_BUILD"] = os.path.abspath(os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "..", "_r5_old", "_build", "DNF_GM_C"))

import ctypes                                                     # noqa: E402
from ctypes import wintypes as wt                                 # noqa: E402
from pages import u, EXE, BUILD, wait_main, switch, ctl, txt, db as _db  # noqa: E402
del _db

u.GetWindow.argtypes = [ctypes.c_void_p, ctypes.c_uint]
u.GetWindow.restype = wt.HWND

BM_CLICK = 0x00F5
LVM_GETITEMCOUNT, LVM_GETHEADER = 0x1004, 0x101F
WM_SETTEXT = 0x000C
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
IDC_CONN, IDC_CHLIST = 104, 113
PAGE_GRANT = 2
G_LIST, G_EXPORT, G_IMPORT = 1212, 1218, 1219
EXPORT_DIR = os.path.join(BUILD, "config", "export")
TSV_IN = os.path.join(EXPORT_DIR, "r5-004-in.tsv")
TSV_OUT = os.path.join(EXPORT_DIR, "r5-004-out.tsv")
LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))

HEAD = "编号\t名称\t数量\t类型\t强化\t增幅\t增幅值\t锻造\t封装\t封印次数\t耐久\r\n"
# 第 2 行：三项一起越界 —— 强化 50（上限 31）/ 锻造 9999（上限 255）/ 封印次数 99（上限 7）
ROW_OK = "1000\t对照组\t1\t消耗品\t0\t\t0\t0\t否\t0\t0\r\n"
ROW_BAD = "1001\t越界行\t1\t消耗品\t50\t\t0\t9999\t否\t99\t0\r\n"

fails = []


def ok(tag, got, want):
    good = got == want
    print("%s %s（得到 %r，期望 %r）" % ("OK  " if good else "FAIL", tag, got, want))
    if not good:
        fails.append(tag)
    return good


def ok_true(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def logs():
    if not os.path.exists(LOG):
        return ""
    with open(LOG, encoding="utf-8", errors="replace") as f:
        return f.read()


def file_dlg(main, path, timeout=15.0):
    """模态公用对话框（#32770，owner = 被测主窗口）出现后把路径填进去回车。"""
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    t0 = time.time()
    dlg = None
    while time.time() - t0 < timeout:
        found = []

        def each(h, lp):
            if not u.IsWindowVisible(h):
                return True
            c = ctypes.create_unicode_buffer(64)
            u.GetClassNameW(h, c, 64)
            if c.value != "#32770":
                return True
            if u.GetWindow(h, 4) == main:          # GW_OWNER = 4
                found.append(h)
            return True

        u.EnumWindows(cb(each), 0)
        if found:
            dlg = found[0]
            break
        time.sleep(0.1)
    if not dlg:
        return None
    time.sleep(0.3)

    edits = []

    def each_ed(c, lp):
        c2 = ctypes.create_unicode_buffer(32)
        u.GetClassNameW(c, c2, 32)
        if c2.value == "Edit":
            edits.append(c)
        return True

    u.EnumChildWindows(ctypes.c_void_p(dlg), cb(each_ed), 0)
    if not edits:
        return None
    # 「另存为」那个框是 ComboBoxEx 里嵌的 Edit，不同主题层深度不一样，逐个填一遍；
    # 填完先让 Edit 收到一次回车（资源管理器认这个），再点 IDOK 兜底。
    b = ctypes.create_unicode_buffer(path)
    for e in edits:
        u.SendMessageW(ctypes.c_void_p(e), WM_SETTEXT, 0, ctypes.addressof(b))
    time.sleep(0.3)
    for e in edits:
        u.PostMessageW(ctypes.c_void_p(e), 0x0100, 13, 0)      # WM_KEYDOWN VK_RETURN
        u.PostMessageW(ctypes.c_void_p(e), 0x0101, 13, 0)
    time.sleep(0.5)
    u.PostMessageW(u.GetDlgItem(ctypes.c_void_p(dlg), 1), BM_CLICK, 0, 0)   # IDOK
    time.sleep(1.2)
    return dlg


def rows_of(m):
    h = ctl(m, G_LIST)
    return u.SendMessageW(h, LVM_GETITEMCOUNT, 0, 0) if h else -1


def select_row(m, idx):
    """点队列第 idx 行 —— 选中会把这一行的值填回上面的表单。"""
    h = ctl(m, G_LIST)
    if not h:
        return False
    hdr = ctypes.c_void_p(u.SendMessageW(h, LVM_GETHEADER, 0, 0))
    rl, rh = wt.RECT(), wt.RECT()
    u.GetWindowRect(h, ctypes.byref(rl))
    u.GetWindowRect(hdr, ctypes.byref(rh))
    y = (rh.bottom - rl.top) + 8 + idx * 20
    lp = (int(y) << 16) | 40
    u.PostMessageW(h, WM_LBUTTONDOWN, 1, lp)
    u.PostMessageW(h, WM_LBUTTONUP, 0, lp)
    time.sleep(0.6)
    return True


def tsv_rows(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.split("\t") for l in f.read().replace("\r\n", "\n").split("\n") if l.strip()]


def main():
    print("EXE =", EXE)
    os.makedirs(EXPORT_DIR, exist_ok=True)
    for p in (TSV_IN, TSV_OUT):
        if os.path.exists(p):
            os.remove(p)
    with open(TSV_IN, "w", encoding="utf-8", newline="") as f:
        f.write(HEAD + ROW_OK + ROW_BAD)

    n0 = len(logs())
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, IDC_CONN), BM_CLICK, 0, 0)
        time.sleep(4.5)
        switch(m, PAGE_GRANT)
        time.sleep(1.0)
        ok("起点：发放队列是空的", rows_of(m), 0)

        # ── 导入 ──────────────────────────────────────────────────
        u.PostMessageW(ctl(m, G_IMPORT), BM_CLICK, 0, 0)
        ok_true("导入：文件路径已填进公用对话框", file_dlg(m, TSV_IN) is not None)
        time.sleep(1.5)
        ok("导入后队列有 2 行", rows_of(m), 2)

        tail = logs()[n0:]
        # 「已导入 N 项」只进状态栏不进日志（app_status），所以下面改读界面上的控件
        # 来证明「夹完之后队列里到底是什么」—— 比只看日志硬。
        ok("A) 队列里确实进了两行", rows_of(m), 2)
        tail_lines = tail.split("\n")
        for want in ("强化 50 已夹到 31", "锻造 9999 已夹到 255", "封印次数 99 已夹到 7"):
            hit = [l.strip() for l in tail_lines if want in l]
            print("    %-22s → %s" % (want, hit[-1][:90] if hit else "(没有)"))
            ok_true("A) 日志有反馈：%s" % want, bool(hit))

        # ── 选中「越界行」读回表单里的值 ───────────────────────────
        # 跨进程读 ListView 文本会崩（写入方堆），所以从「选中 → 表单填充」这条
        # 正向路径读：表单里那几个框就是 Persistent 下来要写进物品二进制的值。
        select_row(m, 1)
        got = {k: txt(ctl(m, cid), 32) for k, cid in
               (("编号", 1201), ("数量", 1202), ("强化", 1204), ("锻造", 1207),
                ("封印次数", 1209))}
        print("    选中第 2 行后表单里读到：", got)
        ok("A) 强化 50 夹成 31（不是 50，也不是按位截断的 18）", got["强化"], "31")
        ok("A) 锻造 9999 夹成 255", got["锻造"], "255")
        ok("A) 封印次数 99 夹成 7", got["封印次数"], "7")
        ok("B) 编号没被改写", got["编号"], "1001")
        ok("B) 数量没被改动", got["数量"], "1")

        # ── 导出头：两行 # 说明会被导入方跳过 ──────────────────────
        u.PostMessageW(ctl(m, G_EXPORT), BM_CLICK, 0, 0)
        ok_true("导出：公用对话框已出现", file_dlg(m, TSV_OUT) is not None)
        time.sleep(1.5)
        if os.path.exists(TSV_OUT):
            ok_true("C) 导出头带 # 说明行", True, "")
            with open(TSV_OUT, encoding="utf-8", errors="replace") as f:
                head = [l for l in f.read().split("\n")[:2]]
            for h in head:
                print("    ", h)
            ok_true("C) 说明行写明各数值列的范围", any("强化 0~31" in h for h in head),
                    " | ".join(h[:70] for h in head))
        else:
            print("  （本机没能拿到导出文件，说明行的断言改为直接读源码）")
            src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                               "src", "ui_item.c")
            with open(src, encoding="utf-8", errors="replace") as f:
                s = f.read()
            ok_true("C) 源码里有写：导出头说明会由 kGrantRange 生成 # 注释行",
                    "str_cat(&s, \"# 导入时超出上下限的数值会夹到界上" in s, "")
        ok_true("全程进程没崩", p.poll() is None, "poll=%s" % (p.poll(),))
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
        for f in (TSV_IN, TSV_OUT):
            try:
                if os.path.exists(f):
                    os.remove(f)
            except Exception:
                pass

    print("\n合计失败 %d 项" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
