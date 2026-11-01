# 密码控件的约定（把「明文是设计选择」这件事钉住，免得以后被"顺手"改掉）：
#   1) 控件是可编辑下拉（CBS_DROPDOWN），不是只能选；
#   2) 预置里必须有 uu5!^%jg 与 123456 两项；
#   3) 当前显示的就是 settings.ini 里的口令原文 —— **明文**，不掩码、不加密；
#   4) 能自己敲进去（SETTEXT 后读回一致）。
#
# 关于第 3 条：这是定好的取舍，不是漏做。它是本机连库用的口令，藏成圆点之后
# 看不见、改不动、连不上时也核对不了；而掩码/本地加密都挡不住能碰到这台机器的人。
# 谁把这里改成 ES_PASSWORD（或把 ini 加密），这条用例就该红 —— 那是在改需求，
# 不是在修 bug，先改这个文件里的约定，再动代码。
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, txt, set_text, start  # noqa: E402

IDC_PWD = 103

CB_GETCOUNT, CB_GETLBTEXT, CB_SETCURSEL = 0x0146, 0x0148, 0x014E
GWL_STYLE = -16
CBS_DROPDOWN = 2          # 可编辑
ES_PASSWORD = 0x0020

u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u.GetWindowLongW.restype = ctypes.c_long

ok_all = True


def check(tag, got, want):
    global ok_all
    good = (got == want)
    print("%-16s %s  (got=%r want=%r)" % (tag, "OK" if good else "FAIL", got, want))
    if not good:
        ok_all = False


def combo_items(h):
    out = []
    n = int(u.SendMessageW(h, CB_GETCOUNT, 0, 0))
    for i in range(max(0, n)):
        buf = ctypes.create_unicode_buffer(128)
        u.SendMessageW(h, CB_GETLBTEXT, i, ctypes.addressof(buf))
        out.append(buf.value)
    return out


def cfg_pwd():
    """直接读 settings.ini，不用 configparser —— 口令里可能有 %，会被插值吃掉。"""
    path = os.path.join(BUILD, "config", "settings.ini")
    if not os.path.exists(path):
        return None
    for line in open(path, encoding="utf-8-sig", errors="replace"):
        if line.strip().startswith("pwd") and "=" in line:
            return line.split("=", 1)[1].strip()
    return None


if __name__ == "__main__":
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    main = wait_main(p)
    try:
        h = u.GetDlgItem(main, IDC_PWD)
        if not h:
            print("找不到密码控件 IDC_PWD")
            sys.exit(1)

        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        check("控件类型", cls.value, "ComboBox")

        style = u.GetWindowLongW(h, GWL_STYLE) & 0xF
        check("可编辑样式", style, CBS_DROPDOWN)

        items = combo_items(h)
        print("下拉项         %r" % (items,))
        check("含 uu5!^%jg", "uu5!^%jg" in items, True)
        check("含 123456", "123456" in items, True)

        # 明文：控件上不能带密码样式（EDIT 子控件同样不能带）
        edit = u.GetDlgItem(h, 1001)
        masked = bool(u.GetWindowLongW(h, GWL_STYLE) & ES_PASSWORD)
        if edit:
            masked = masked or bool(u.GetWindowLongW(edit, GWL_STYLE) & ES_PASSWORD)
        check("未做掩码", masked, False)

        want = cfg_pwd()
        if want is not None:
            check("显示 ini 原值", txt(h), want)
        else:
            print("显示 ini 原值   跳过（没有 settings.ini）")

        # 能自己敲：写进去再读回来
        old = txt(h)
        set_text(h, "hand-typed-pwd")
        time.sleep(0.2)
        check("可手填", txt(h), "hand-typed-pwd")

        # 能从下拉选：选中 123456 后编辑区跟着变
        if "123456" in items:
            u.SendMessageW(h, CB_SETCURSEL, items.index("123456"), 0)
            time.sleep(0.2)
            check("可选 123456", txt(h), "123456")

        set_text(h, old)                 # 复原，别把测试值留在界面上
        print("\n约定：密码明文显示、明文保存（设计如此，不是缺陷）")
        print("结果：" + ("全部通过" if ok_all else "有失败项"))
        sys.exit(0 if ok_all else 1)
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
