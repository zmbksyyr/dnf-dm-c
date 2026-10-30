"""GM 页失焦即写回归：选中角色 → 改金币 → 触发 EN_KILLFOCUS → 等写库 → 改回原值。
页面上的控件挂在子窗口 "DnfPage" 下，必须从那里找控件 ID。"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, grab, EXE, OUT, switch, NAMES, wait_main

WM_KILLFOCUS = 0x0008
WM_SETTEXT = 0x000C
BM_CLICK = 0x00F5
PAGE_ID = lambda page, n: 1000 + page * 100 + n
PAGE_GM = 8
G_MONEY = PAGE_ID(PAGE_GM, 5)


def settext(h, s):
    buf = ctypes.create_unicode_buffer(s)
    u.SendMessageW(h, WM_SETTEXT, 0, ctypes.addressof(buf))


def gettext(h):
    buf = ctypes.create_unicode_buffer(128)
    u.SendMessageW(h, 0x000D, 128, ctypes.addressof(buf))
    return buf.value


def click(h, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    u.PostMessageW(h, 0x0201, 1, lp)
    u.PostMessageW(h, 0x0202, 0, lp)


def pages_of(main):
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "DnfPage":
            out.append(h)
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return out


def logtail(n=20):
    d = os.path.join(os.path.dirname(EXE), "config", "logs")
    if not os.path.isdir(d):
        return ""
    fs = sorted(os.path.join(d, f) for f in os.listdir(d))
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return "".join(f.readlines()[-n:])


p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
main = wait_main(p)
u.SendMessageW(u.GetDlgItem(main, 104), BM_CLICK, 0, 0)
time.sleep(4.5)
lv = u.GetDlgItem(main, 113)
click(lv, 40, 12)
time.sleep(3.5)
print("list selection =", u.SendMessageW(lv, 0x100C, -1, 0x0002))

switch(main, PAGE_GM)
time.sleep(0.5)
ps = pages_of(main)
print("pages:", [hex(x) for x in ps])
money = None
for w in ps:
    money = u.GetDlgItem(w, G_MONEY)
    if money:
        break
if not money:
    print("找不到金币输入框")
    p.kill()
    sys.exit(1)

before = gettext(money)
print("before money =", repr(before))
settext(money, "12345")
u.SendMessageW(money, WM_KILLFOCUS, 0, 0)     # EDIT 会自己发 EN_KILLFOCUS 给父窗口
time.sleep(2.5)
grab(main, os.path.join(OUT, "v10_gm_money.png"))
print("after write =", repr(gettext(money)))

settext(money, before if before else "0")
u.SendMessageW(money, WM_KILLFOCUS, 0, 0)
time.sleep(2.5)
print("restored =", repr(gettext(money)))
grab(main, os.path.join(OUT, "v11_gm_restore.png"))
print("---- log tail ----")
print(logtail(16))
p.kill()
