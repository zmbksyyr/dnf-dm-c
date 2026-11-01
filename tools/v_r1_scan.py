# -*- coding: utf-8 -*-
"""R1-006 定向验证：关掉扫描窗后 s_scan_busy 是否复位。

场景（finding 原话）：窗口开着 → 点「重新扫描」把 s_scan_busy 置 1 → 关窗
→ 再点「全服清错误物品」入口。

脚本跑两组，差别只在「关窗时 busy 是几」：

  A 组（对照）：点「重新扫描」后等 0.6 秒再关窗。这 0.6 秒 ≥ 一个 200ms 定时器，
     busy 已经被 WM_TIMER 复位，关窗时 busy=0 —— 用来证明「入口本身是好的」。
  B 组（判据）：点「重新扫描」后立刻关窗（SendMessage 同步，保证点击真的落到
     s_scan_busy=1 上，再发 WM_CLOSE）。这一组才是 finding 描述的触发条件。

A 通过、B 失败 = 关窗那条路没复位 busy，修复没生效。
两组都用「再点入口弹出哪个 MessageBox」判定：确认框=可用，「上一轮还没跑完」=卡死。

    python tools/v_r1_scan.py
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, ctl, txt

G_SCANBAD = 1000 + 8 * 100 + 27        # 1827
SCAN_TITLE = "全服清错误物品"
fails = []


def ok(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def top_windows(main, cls=None, title=None):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, c, 64)
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        owner = u.GetWindow(h, 4)          # GW_OWNER
        if cls and c.value != cls:
            return True
        if title and t.value != title:
            return True
        if owner not in (main, 0):
            return True
        out.append((h, c.value, t.value))
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
        for h, c, t in top_windows(main, cls="#32770"):
            return h, dlg_text(h)
        time.sleep(0.05)
    return None, ""


def click_dlg_item(h, cid):
    b = u.GetDlgItem(h, cid)
    if b:
        u.PostMessageW(b, 0x00F5, 0, 0)      # BM_CLICK
    return bool(b)


def wait_msgbox_gone(main, timeout=8.0):
    """MessageBox 是模态的：它还在的时候，主窗口是 disabled 状态，
    往页面按钮 PostMessage 会直接丢掉。所以每次点完确定/取消都要等它真的消失。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not top_windows(main, cls="#32770"):
            return True
        time.sleep(0.05)
    return False


def open_scan_dialog(m):
    """点入口 → 确认 → 等结果窗，返回窗口句柄（失败返回 None）。"""
    u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
    b, t = wait_msgbox(m, 8.0)
    if "扫描全服所有角色" not in t:
        print("   入口弹出：%r" % t)
        if b:
            click_dlg_item(b, 2)
            wait_msgbox_gone(m)
        return None, t
    click_dlg_item(b, 1)                                    # IDOK
    wait_msgbox_gone(m)
    t0 = time.time()
    while time.time() - t0 < 25:
        w = top_windows(m, cls="DnfDlg", title=SCAN_TITLE)
        if w:
            return w[0][0], t
        time.sleep(0.1)
    return None, t


def probe_entry(m):
    """点入口，返回弹出来的文本框（不真的扫）。"""
    u.PostMessageW(ctl(m, G_SCANBAD), 0x00F5, 0, 0)
    b, t = wait_msgbox(m, 8.0)
    if b:
        click_dlg_item(b, 2)                                # IDCANCEL
        wait_msgbox_gone(m)
    return t


def main():
    print("EXE =", EXE)
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)     # 连接
        time.sleep(4.5)
        lv = u.GetDlgItem(m, 113)
        lp = (12 << 16) | 40
        u.PostMessageW(lv, 0x0201, 1, lp)
        u.PostMessageW(lv, 0x0202, 0, lp)
        time.sleep(3.0)
        switch(m, 8)
        time.sleep(1.0)

        if not ok("「全服清错误物品」按钮存在", bool(ctl(m, G_SCANBAD))):
            return 1

        # ---------------- A 组：关窗时 busy=0（对照组）
        dlg, _ = open_scan_dialog(m)
        if not ok("A：扫描结果窗出现", bool(dlg)):
            return 1
        print("   A 列表行数 =", u.SendMessageW(u.GetDlgItem(dlg, 6000), 0x1004, 0, 0))
        u.SendMessageW(u.GetDlgItem(dlg, 6003), 0x00F5, 0, 0)   # 重新扫描（同步）
        time.sleep(0.6)                                        # 让 WM_TIMER 把 busy 复位
        u.SendMessageW(dlg, 0x0010, 0, 0)                      # WM_CLOSE
        time.sleep(1.0)
        ok("A：扫描窗已关闭", not top_windows(m, cls="DnfDlg", title=SCAN_TITLE))
        t = probe_entry(m)
        print("   A 关窗后再点入口：%r" % t)
        ok("A：busy=0 时入口可用（对照组）", "扫描全服所有角色" in t, t)

        # ---------------- B 组：关窗时 busy=1（finding 的触发条件）
        dlg, _ = open_scan_dialog(m)
        if not ok("B：扫描结果窗再次出现", bool(dlg)):
            return 1
        # SendMessage 是同步的：返回时 6003 一定已经处理完（s_scan_busy=1），
        # 紧接着发 WM_CLOSE，抢在下一个 200ms 定时器之前关窗。
        u.SendMessageW(u.GetDlgItem(dlg, 6003), 0x00F5, 0, 0)
        u.SendMessageW(dlg, 0x0010, 0, 0)
        time.sleep(1.0)
        ok("B：扫描窗已关闭", not top_windows(m, cls="DnfDlg", title=SCAN_TITLE))
        t = probe_entry(m)
        print("   B 带着 busy 关窗后再点入口：%r" % t)
        ok("B：不再提示「上一轮还没跑完」", "上一轮还没跑完" not in t, t)
        ok("B：入口恢复可用（弹出确认框）", "扫描全服所有角色" in t, t)
        ok("进程仍然活着", p.poll() is None, "poll=%s" % (p.poll(),))
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
