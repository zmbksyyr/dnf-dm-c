# 验证「编号悬浮卡」：鼠标停在列表的编号行上，浮出物品详情 + 图标。
#
# 被测进程是浮层的宿主，所以脚本只做「移动鼠标 → 等 → 数浮层窗口」这三件事：
#   · 浮层是自建的 WS_POPUP（类名 DnfIdTip），跨进程只看得见它的存在/位置/大小，
#     不读它的文本（跟列表文本同理，跨进程 WM_GETTEXT 会写崩对方进程）。
#   · 鼠标位置用 SetCursorPos 直接落坐标，不模拟输入 —— 被测进程靠 GetCursorPos
#     轮询，不需要等输入注入。
#   · 图标是否真的画出来了靠截图：浮层窗口露出来后整屏截一张，肉眼核对。
#
# 关于「一直按住光标」：SetCursorPos 只管到下一次真实鼠标输入为止。本机若有人
# 在用鼠标（或挂着远控），一松手坐标就被抢回去了，浮层永远等不到 400ms 的停留。
# 所以下面一律用 park() 高频重申坐标，并在结束后回读一次，判断到底是我们没等到，
# 还是光标压根没听我们的 —— 后者算「跳过」，不算失败。
import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import wait_main, grab, OUT as SHOTS, EXE  # noqa: E402

u = ctypes.windll.user32
u.SetCursorPos.restype = ctypes.c_bool
u.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u.GetCursorPos.restype = ctypes.c_bool
u.GetWindowRect.restype = ctypes.c_bool
u.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.RECT)]
u.IsWindowVisible.restype = ctypes.c_bool
u.EnumWindows.restype = ctypes.c_bool
u.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]

BM_CLICK = 0x00F5
LVM_GETITEMCOUNT = 0x1004
IDC_CONN = 104
PAGE_QUERY = 1
Q_MAT = 1107          # 查询页「加载常用材料」
Q_LIST = 1102
TCM_SETCURSEL = 0x130C

HOVER_MS = 400        # 与程序里的 IDTIP_DELAY 对齐


def P(h):
    return ctypes.c_void_p(h.value if hasattr(h, "value") else int(h))


def find_tip():
    """找我们的浮层窗口（类名 DnfIdTip），返回 (句柄, 位置尺寸)。"""
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(P(h), cls, 64)
        if cls.value == "DnfIdTip" and u.IsWindowVisible(P(h)):
            r = wt.RECT()
            u.GetWindowRect(P(h), ctypes.byref(r))
            found.append((h, r))
            return False
        return True

    u.EnumWindows(cb(each), 0)
    return found[0] if found else (None, None)


def page_ctl(main, cid):
    """页子窗口下的控件：递归一层就够（控件直接挂在 DnfPage 下）。"""
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    out = []

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(P(h), cls, 64)
        if cls.value == "DnfPage" and u.IsWindowVisible(P(h)):
            w = u.GetDlgItem(P(h), cid)
            if w:
                out.append(w)
                return False
        return True

    u.EnumChildWindows(P(main), cb(each), 0)
    return out[0] if out else None


def cursor():
    p = wt.POINT()
    u.GetCursorPos(ctypes.byref(p))
    return p.x, p.y


def row_xy(lv, row=0):
    """列表第 row 行中间那个点。行高按 20px 估，落在行内即可。"""
    r = wt.RECT()
    u.GetWindowRect(P(lv), ctypes.byref(r))
    return (r.left + r.right) // 2, r.top + 24 + row * 20


def park(x, y, secs, stop=None):
    """把光标按在 (x,y) 上 secs 秒。高频重申，抢回去就再按一次。

    stop 是一个 callable，每次重申后调一次；返回真值就提前收工（浮层已经出来了）。
    返回 (稳不稳, 提前收工否)：稳 = 结束时回读坐标还在目标点附近。
    """
    end = time.time() + secs
    while True:
        u.SetCursorPos(x, y)
        if stop and stop():
            return True, True
        if time.time() >= end:
            break
        time.sleep(0.02)
    cx, cy = cursor()
    steady = abs(cx - x) <= 4 and abs(cy - y) <= 4
    return steady, False


def wait_tip(secs):
    """等浮层出现，返回 (句柄, rect) 或 (None, None)。"""
    end = time.time() + secs
    while time.time() < end:
        h, r = find_tip()
        if h:
            return h, r
        time.sleep(0.1)
    return None, None


def main():
    ok = True
    skipped = False
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    mw = wait_main(p)
    u.PostMessageW(P(u.GetDlgItem(P(mw), IDC_CONN)), BM_CLICK, 0, 0)
    time.sleep(5.0)

    # 切到查询页并加载一批常用材料（编号 1039 / 1232 之类都在里头）
    u.SendMessageW(P(u.GetDlgItem(P(mw), 190)), TCM_SETCURSEL, PAGE_QUERY, 0)
    time.sleep(0.5)

    btn = page_ctl(mw, Q_MAT)
    waited = 0
    while waited < 90 and (not btn or not u.IsWindowEnabled(P(btn))):
        time.sleep(1.0)
        waited += 1
    print("== 等 PVF 就绪 %ds · 按钮可用=%d" % (waited, bool(btn) and u.IsWindowEnabled(P(btn))))
    u.PostMessageW(P(btn), BM_CLICK, 0, 0)
    time.sleep(1.5)
    # 贴图包扫描是后台活，给它一点时间；晚了顶多第一张卡没图标，不算错
    time.sleep(6.0)

    lv = page_ctl(mw, Q_LIST)
    n = u.SendMessageW(P(lv), LVM_GETITEMCOUNT, 0, 0)
    print("== 查询页列表 %d 行" % n)
    ok &= n > 0

    # 1) 光标挪到列表外 → 不该有浮层
    park(4, 4, 1.5)
    h0, _ = find_tip()
    print("== 移开后浮层：%s（应为无）" % ("有" if h0 else "无"))
    ok &= h0 is None

    # 2) 停在第一行上 → 浮层出现（PVF 已加载，详情文本至少有一行）
    x, y = row_xy(lv, 0)
    steady, early = park(x, y, 12.0, stop=lambda: find_tip()[0] is not None)
    h, r = find_tip()
    print("== 悬停 0 行 %d,%d → 浮层：%s%s" % (
        x, y, "出现" if h else "没出现",
        "" if steady else "（光标被抢，坐标未生效）"))
    if h:
        print("   浮层 %dx%d @(%d,%d)" % (r.right - r.left, r.bottom - r.top, r.left, r.top))
        grab(h, os.path.join(SHOTS, "hover_card.png"))     # 浮层本体
    grab(mw, os.path.join(SHOTS, "hover_page.png"))
    if not h and not steady:
        skipped = True
    ok &= h is not None or not steady

    # 3) 换一行 → 旧浮层先收，新的再出（内容随编号变）
    x2, y2 = row_xy(lv, 3)
    park(x2, y2, 0.2)
    h_quick, _ = find_tip()
    print("== 换到第 3 行后 0.2s：%s（延迟期内应已收起）" % ("还有" if h_quick else "已收"))
    steady3, _ = park(x2, y2, 12.0, stop=lambda: find_tip()[0] is not None)
    h3, r3 = find_tip()
    print("== 第 3 行浮层：%s%s" % ("出现" if h3 else "没出现",
                                 "" if steady3 else "（光标被抢，坐标未生效）"))
    if h3:
        grab(h3, os.path.join(SHOTS, "hover_card3.png"))
    if not h3 and not steady3:
        skipped = True
    ok &= h3 is not None or not steady3

    # 4) 再移开 → 收起
    park(4, 4, 1.2)
    h2, _ = find_tip()
    print("== 移开后浮层：%s（应无）" % ("有" if h2 else "无"))
    ok &= h2 is None

    print("\n结果：%s" % ("通过" if ok else "不通过"))
    if skipped:
        print("（其中至少一步因本机光标被占用没能验证，需在无人操作的环境重跑）")
    p.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
