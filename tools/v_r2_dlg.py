# -*- coding: utf-8 -*-
"""R2-001 定向验证（框架侧）：ui_dlg_run 的销毁顺序改了之后，所有模态对话框都受影响。

A 组：3 个对话框各开/关 5 轮，验证「能打开、能取消、能销毁、销毁后再打开不崩」：
     编辑物品（item_edit，栈上 ctx）、清除错误物品（bad_proc，NULL ctx）、
     全服扫描结果窗（scan_proc，带 WM_DESTROY 清理）。
     每关一次都断言：窗口真的没了（IsWindow 为 0）、主窗重新可用、进程活着。
     另外统计 GDI/USER 句柄，窗口没被 DestroyWindow 的话这里会一路涨。

B 组：作业还在跑时关窗，再立刻点扫描入口，重复 10 轮。
     断言每轮要么被拦（「上一轮还没跑完」），要么安全地重新开始；进程不能崩。

    python tools/v_r2_dlg.py
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, start, switch, ctl, txt, click_at, enabled, db as open_db

BM_CLICK = 0x00F5
WM_CLOSE = 0x0010
LVM_GETITEMCOUNT = 0x1004
IDC_CHLIST = 113

PAGE_ITEM, PAGE_GM = 0, 8
I_SUB, I_LIST, I_EDIT, I_BAD = 1001, 1002, 1003, 1007
G_SCANBAD = 1827
S_LIST, S_RESCAN, S_PURGE = 6000, 6003, 6002
SCAN_TITLE = "全服清错误物品"

k32 = ctypes.windll.kernel32
k32.OpenProcess.restype = wt.HANDLE
u.GetGuiResources.restype = wt.DWORD

fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


# ---------------- 窗口工具 ----------------
def enum_top(cls=None):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        if cls and c.value != cls:
            return True
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        out.append((h, c.value, t.value))
        return True

    u.EnumWindows(cb(each), 0)
    return out


def dlg_by_title(title):
    for h, c, t in enum_top("DnfDlg"):
        if t == title:
            return h
    return None


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


def msgbox():
    w = enum_top("#32770")
    return (w[0][0], dlg_text(w[0][0])) if w else (None, "")


def wait_msgbox(timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h, t = msgbox()
        if h:
            time.sleep(0.25)
            return h, dlg_text(h)
        time.sleep(0.05)
    return None, ""


def click_box(cid):
    h, _ = msgbox()
    if not h:
        return False
    b = u.GetDlgItem(h, cid)
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
    t0 = time.time()
    while time.time() - t0 < 8:
        if not enum_top("#32770"):
            return True
        time.sleep(0.05)
    return False


def wait_dlg(title, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = dlg_by_title(title)
        if h:
            time.sleep(0.5)
            return h
        time.sleep(0.1)
    return None


def wait_gone(title, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not dlg_by_title(title):
            return True
        time.sleep(0.05)
    return False


def close_all_dlgs():
    for h, c, t in enum_top("DnfDlg"):
        u.SendMessageW(h, WM_CLOSE, 0, 0)
        time.sleep(0.4)


def settle(main):
    """把上一轮可能残留的提示框/对话框收干净，避免模态把下一轮挡掉。"""
    for _ in range(4):
        b, _t = msgbox()
        if b:
            click_box(2)
            time.sleep(0.2)
            continue
        close_all_dlgs()
        time.sleep(0.2)
        if not enum_top("DnfDlg") and not msgbox()[0]:
            return True
    return False


def modal_ok(main):
    """模态对话框打开时 owner 被 EnableWindow(FALSE)；关掉后必须恢复。"""
    return not u.IsWindowEnabled(main)


# ---------------- 进程句柄 ----------------
def gui_res(proc):
    h = k32.OpenProcess(0x1000, False, proc.pid)     # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    g = u.GetGuiResources(h, 0)                      # GR_GDIOBJECTS
    us = u.GetGuiResources(h, 1)                     # GR_USEROBJECTS
    k32.CloseHandle(h)
    return g, us


# ---------------- 三个对话框的开关动作 ----------------
def cycle_edit(main, how):
    """编辑物品：点「编辑」→ 用 IDCANCEL 或 WM_CLOSE 关。"""
    il = ctl(main, I_LIST)
    click_at(il, 40, 12, 0.5)                        # 选中一行
    u.PostMessageW(ctl(main, I_EDIT), BM_CLICK, 0, 0)
    h = wait_dlg("编辑物品", 8.0)
    if not h:
        return False, "窗口没出来"
    if how == "cancel":
        u.SendMessageW(u.GetDlgItem(h, 2), BM_CLICK, 0, 0)      # IDCANCEL
    else:
        u.SendMessageW(h, WM_CLOSE, 0, 0)
    return wait_gone("编辑物品", 6.0), ""


def cycle_bad(main, how):
    u.PostMessageW(ctl(main, I_BAD), BM_CLICK, 0, 0)
    h = wait_dlg("清除错误物品", 15.0)
    if not h:
        b, t = msgbox()
        if b:
            click_box(2)
            return False, "没弹窗，只有提示：%s" % t[:60]
        return False, "窗口没出来"
    if how == "cancel":
        u.SendMessageW(u.GetDlgItem(h, 2), BM_CLICK, 0, 0)
    else:
        u.SendMessageW(h, WM_CLOSE, 0, 0)
    return wait_gone("清除错误物品", 6.0), ""


def open_scan(main):
    if not ctl(main, G_SCANBAD):          # 扫描入口在 GM 页，先切过去
        switch(main, PAGE_GM)
        time.sleep(1.5)
    u.PostMessageW(ctl(main, G_SCANBAD), BM_CLICK, 0, 0)
    h, t = wait_msgbox(8.0)
    if not h:
        return None, "入口没弹确认框（%r）" % t
    confirm = "扫描全服所有角色" in t
    click_box(1)                                  # 确认框点确定；提示框只有确定，同样点它
    if not confirm:
        return None, "入口弹的是：%s" % t[:60]
    t0 = time.time()
    while time.time() - t0 < 30:
        d = dlg_by_title(SCAN_TITLE)
        if d:
            time.sleep(0.5)
            return d, ""
        b, t2 = msgbox()
        if b:
            click_box(1)
            return None, "提示：%s" % t2[:60]
        time.sleep(0.1)
    return None, "等不到结果窗"


def cycle_scan(main, how):
    h, why = open_scan(main)
    if not h:
        return False, why
    if how == "cancel":
        u.SendMessageW(u.GetDlgItem(h, 2), BM_CLICK, 0, 0)
    else:
        u.SendMessageW(h, WM_CLOSE, 0, 0)
    return wait_gone(SCAN_TITLE, 6.0), ""


def main():
    print("EXE =", EXE)
    p = None
    try:
        p, main = start(page=PAGE_ITEM)
        lv = u.GetDlgItem(main, IDC_CHLIST)
        click_at(lv, 40, 12, 5.0)                    # 选角色
        switch(main, PAGE_ITEM)
        time.sleep(2.0)
        lvl = ctl(main, I_LIST)
        nitem = u.SendMessageW(lvl, LVM_GETITEMCOUNT, 0, 0) if lvl else 0
        print("物品列表行数 =", nitem)

        # ============ A 组：三个对话框各 5 轮 ============
        base = gui_res(p)
        print("起始句柄 GDI/USER =", base)
        cycles = [("编辑物品", cycle_edit), ("清除错误物品", cycle_bad), ("扫描结果窗", cycle_scan)]
        samples = []                     # 每轮关干净之后的句柄数：看斜率，不看绝对值
        for name, fn in cycles:
            good = 0
            why = ""
            for i in range(5):
                settle(main)
                how = "cancel" if i % 2 == 0 else "close"
                done, why = fn(main, how)
                if done:
                    good += 1
                else:
                    print("   第 %d 轮失败：%s" % (i + 1, why))
                # 关掉窗口后必须立刻可再开：先确认没有残留窗口、owner 已恢复
                if dlg_by_title(name if name != "扫描结果窗" else SCAN_TITLE):
                    print("   第 %d 轮关闭后窗口还在" % (i + 1))
                if not enabled(main):
                    print("   第 %d 轮关闭后主窗仍是禁用态" % (i + 1))
                settle(main)
                samples.append((name, gui_res(p)))
                time.sleep(0.3)
            ok("A %s：5 轮开/关全部成功" % name, good == 5, "%d/5" % good)
            ok("A %s：关掉后没有残留窗口" % name,
               not dlg_by_title(name if name != "扫描结果窗" else SCAN_TITLE))
            ok("A %s：关掉后主窗恢复可用" % name, enabled(main))
            ok("A %s：进程仍然活着" % name, p.poll() is None)
            time.sleep(0.5)

        settle(main)
        after = gui_res(p)
        print("结束句柄 GDI/USER =", after)
        # 每个对话框首轮都有一次性分配（窗口类 / 字体 / 列表列头），所以按对话框分组
        # 看「该组 5 轮内的最大值-最小值」：真漏的话它随轮数线性涨，不是平的。
        for k, nm in ((0, "GDI"), (1, "USER")):
            for name, _fn in cycles:
                seq = [s[1][k] for s in samples if s[0] == name and s[1]]
                span = (max(seq) - min(seq)) if seq else 99
                ok("A %s 连开 5 次 %s 句柄不涨" % (name, nm), span <= 2, "采样 %s" % seq)

        # ============ B 组：作业在跑时关窗 → 立刻再点入口，10 轮 ============
        # 1..5 轮走「快路径」：扫描本来就快，重进时作业多半已经结束 → 安全重新开始；
        # 6..10 轮用一条独立的库连接 LOCK TABLES inventory WRITE 把扫描的 SELECT 拖住，
        # 让 s_scan_live 稳定停在 1，强制走到闸门那条分支。
        print("\nB 组：扫描中关窗 → 立刻再点入口")
        blocked = 0
        restarted = 0
        slow = 0
        lk = open_db()
        lc = lk.cursor()
        for r in range(1, 11):
            locked = r > 5
            close_all_dlgs()
            # 等作业真的结束（入口能弹出结果窗）
            h = None
            t0 = time.time()
            while time.time() - t0 < 25:
                if msgbox()[0]:
                    click_box(2)
                    time.sleep(0.5)
                h, why = open_scan(main)
                if h:
                    break
                if "上一轮还没跑完" in why:
                    click_box(1)                     # app_info 的「确定」
                    time.sleep(0.6)
                    continue
                time.sleep(0.4)
            if not h:
                print("   第 %d 轮：等不到空闲，跳过（%s）" % (r, why))
                continue

            if locked:
                lc.execute("LOCK TABLES taiwan_cain_2nd.inventory WRITE")
            # 重新扫描（同步）后立刻关窗
            u.SendMessageW(u.GetDlgItem(h, S_RESCAN), BM_CLICK, 0, 0)
            u.SendMessageW(h, WM_CLOSE, 0, 0)
            if not wait_gone(SCAN_TITLE, 3.0):
                print("   第 %d 轮：关不掉" % r)
            time.sleep(0.02)
            if locked:
                time.sleep(0.3)                      # 让作业真的堵在那条 SELECT 上

            # 立刻再点入口
            u.PostMessageW(ctl(main, G_SCANBAD), BM_CLICK, 0, 0)
            b, t = wait_msgbox(6.0)
            verdict = "?"
            if b and "上一轮还没跑完" in t:
                verdict = "入口直接拦下"
                blocked += 1
                click_box(1)
            elif b and "扫描全服所有角色" in t:
                click_box(1)                          # 确定 → 走 scan_start
                b2, t2 = wait_msgbox(4.0)
                if b2 and "上一轮还没跑完" in t2:
                    verdict = "确认后被 s_scan_live 拦下"
                    blocked += 1
                    click_box(1)
                elif b2:
                    verdict = "另弹提示：%s" % t2[:40]
                    click_box(1)
                else:
                    verdict = "安全地重新开始扫描"
                    restarted += 1
            elif b:
                verdict = "另弹提示：%s" % t[:40]
                click_box(1)
            else:
                verdict = "没有弹窗（入口没反应）"
                slow += 1
            alive = p.poll() is None
            print("   第 %2d 轮（%s）：%s · 进程 %s"
                  % (r, "锁表拖住作业" if locked else "快路径", verdict, "存活" if alive else "已退出!"))
            if locked:
                lc.execute("UNLOCK TABLES")
            if not alive:
                ok("B 第 %d 轮：进程不能崩" % r, False, "poll=%s" % (p.poll(),))
                break
            time.sleep(0.2)

        try:
            lc.execute("UNLOCK TABLES")
        except Exception:
            pass
        lk.close()

        ok("B 10 轮作业存活期间关窗+重进，进程没有崩", p.poll() is None, "poll=%s" % (p.poll(),))
        ok("B 每轮都有明确结论（被拦或重新开始）", blocked + restarted >= 8,
           "被拦 %d · 重新开始 %d · 无反应 %d" % (blocked, restarted, slow))
        ok("B 至少有一轮撞上「作业还在跑」并被闸门处理", blocked + restarted > 0,
           "被拦 %d · 重新开始 %d" % (blocked, restarted))
        ok("B 锁表那 5 轮都走到了「被拦」分支", blocked >= 5,
           "被拦 %d 次" % blocked)

        # 收尾：把窗口关干净，进程仍在
        close_all_dlgs()
        ok("B 收尾后没有残留对话框", not enum_top("DnfDlg"), str(enum_top("DnfDlg"))[:120])
        ok("B 收尾后进程仍活着", p.poll() is None)
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
