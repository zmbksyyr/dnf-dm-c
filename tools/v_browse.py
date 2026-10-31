# 验证查询页五个「浏览」按钮 → 独立窗口 → 虚拟列表 → 筛选 → 加入队列。
#
# 这一版脚本把前一版踩过的坑全绕开了，逐个留注：
#   · 按钮的父窗口是「页」子窗口，WM_COMMAND 是按钮发给它的 —— 往主窗口 Post
#     只会掉进 main 自己的分支被吞掉。所以一律对按钮本身发 BM_CLICK，由它
#     反过来向自己的父窗口报 WM_COMMAND，这才是真实点击的那条路。
#   · 列表是 LVS_OWNERDATA，文本并不存在控件里，而是画某行时回头问应用要。
#     跨进程发 LVM_GETITEMTEXT 等于让对方进程往我们的缓冲区写
#     （0x1000 以上消息不做跨进程封送），实测直接把 DNF_GM.exe 写崩。
#     所以只读行数（LVM_GETITEMCOUNT，不带指针）和 uint16 安全的 WM_GETTEXT。
#   · 选行同理：LVM_SETITEMSTATE 要传 LVITEM 指针，不能跨进程用了；
#     改点「全选」按钮，让它自己在进程里选。
#   · 浏览窗口是模态的（自带消息循环），对它的控件一律 SendMessage/BM_CLICK。
import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import wait_main, grab, OUT as SHOTS  # noqa: E402
from pages import enabled, rows, set_text as set_txt, txt

u = ctypes.windll.user32
u.GetDlgCtrlID.restype = ctypes.c_int
u.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
u.GetDlgItem.restype = ctypes.c_void_p
u.SendMessageW.restype = ctypes.c_void_p
u.PostMessageW.restype = ctypes.c_bool
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]

WM_SETTEXT, WM_GETTEXT = 0x000C, 0x000D
WM_GETTEXTLENGTH, WM_CLOSE, BM_CLICK = 0x000E, 0x0010, 0x00F5
LVM_GETITEMCOUNT = 0x1004
IDC_CONN, TAB = 104, 190
PAGE_QUERY, PAGE_GRANT = 1, 2
GWL_STYLE = -16
LVS_OWNERDATA = 0x1000

# 浏览按钮在查询页（PAGE_QUERY=1 → 1100 起）
Q_BMAT, Q_BSTK, Q_BEQU, Q_BPET, Q_BAVA = 1111, 1112, 1113, 1114, 1115
G_LIST = 1212                  # 发放页的队列列表（安全：只取行数，不取文本）

B_KW, B_LIST, B_CNT, B_MSG = 3001, 3002, 3003, 3004
B_ADD, B_ALL, B_SEL, B_CLR, B_CLOSE = 3005, 3006, 3007, 3008, 3009


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", wt.HWND), ("idFrom", ctypes.c_size_t), ("code", ctypes.c_uint)]


def P(h):
    """句柄可能是 wt.HWND 也可能是 int，统一成 void* 传进去。"""
    return ctypes.c_void_p(h.value if hasattr(h, "value") else int(h))


def index_pages(hwnd, out):
    """递归枚举所有控件句柄，按控件 ID 存起来（页控件挂在页子窗口下）。"""
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def walk(h):
        def each(c, lp):
            cid = u.GetDlgCtrlID(P(c))
            if cid and cid not in out:
                out[cid] = wt.HWND(c)
            walk(P(c))
            return True

        u.EnumChildWindows(h, cb(each), 0)

    walk(hwnd)


def switch(main, idx):
    tab = u.GetDlgItem(P(main), TAB)
    u.SendMessageW(P(tab), 0x130C, idx, 0)     # TCM_SETCURSEL
    nh = NMHDR(tab, TAB, 0xFFFFFDD9)                         # TCN_SELCHANGE
    u.SendMessageW(P(main), 0x004E, TAB,
                   ctypes.cast(ctypes.byref(nh), ctypes.c_void_p))
    time.sleep(1.2)


def click(h, wait=0.0):
    """用 PostMessage 投递 BM_CLICK：SendMessage 会同步等到对方处理完，
    而打开浏览窗口这次处理是「跑一个模态循环」，窗口不关 SendMessage 就永不返回
    （前一版脚本卡死 12 分钟就是这个）。投递式发出去就回来，之后再轮询窗口。"""
    if h:
        u.PostMessageW(P(h), BM_CLICK, 0, 0)
        if wait:
            time.sleep(wait)


def wait_dlg(timeout=25.0):
    """等模态浏览窗口出现：类名 DnfDlg、标题以「浏览」开头。"""
    end = time.time() + timeout
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def scan(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(P(h), cls, 64)
        if cls.value == "DnfDlg":
            t = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(P(h), t, 256)
            if t.value.startswith("浏览"):
                found.append(wt.HWND(h))
                return False
        return True

    while time.time() < end:
        found.clear()
        u.EnumWindows(cb(scan), 0)
        if found:
            time.sleep(0.8)
            return found[0]
        time.sleep(0.2)
    return None


def open_cat(h, idc):
    click(h.get(idc), 0.3)
    return wait_dlg()


def close_dlg(d):
    u.PostMessageW(P(d), WM_CLOSE, 0, 0)
    time.sleep(1.0)


def main():
    p = subprocess.Popen([os.path.join(ROOT, "DNF_GM.exe")], cwd=ROOT)
    mw = wait_main(p)
    u.PostMessageW(P(mw), 0x0111, IDC_CONN, 0)     # 连库（宠物清单靠它）
    time.sleep(8.0)
    switch(mw, PAGE_QUERY)

    h = {}
    index_pages(P(mw), h)
    waited = 0
    while waited < 60 and not enabled(h.get(Q_BEQU)):
        time.sleep(1.0)
        waited += 1
        index_pages(P(mw), h)
    print("== 等 PVF 就绪 %ds · 装备按钮可用=%d" % (waited, enabled(h.get(Q_BEQU))))
    index_pages(P(mw), h)
    grab(mw, os.path.join(SHOTS, "q_browse_row.png"))

    cases = [("材料", Q_BMAT, "硬化"), ("消耗品", Q_BSTK, "药剂"), ("装备", Q_BEQU, "剑"),
             ("宠物", Q_BPET, "佛"), ("时装", Q_BAVA, "套装")]
    for name, idc, kw in cases:
        print("== 按钮「%s」 可用=%d" % (name, enabled(h.get(idc))))
        if not enabled(h.get(idc)):
            continue
        d = open_cat(h, idc)
        if not d:
            print("   窗口没出来")
            continue
        lv = u.GetDlgItem(P(d), B_LIST)
        if not lv:
            print("   列表句柄取不到")
            close_dlg(d)
            continue
        lv = wt.HWND(lv)
        style = u.GetWindowLongPtrW(P(lv), GWL_STYLE)
        print("   标题：%s" % txt(d))
        print("   虚拟列表(LVS_OWNERDATA)=%s 行数=%d" % (bool(style & LVS_OWNERDATA), rows(lv)))
        print("   计数：%s" % txt(u.GetDlgItem(P(d), B_CNT)))
        grab(d, os.path.join(SHOTS, "br_%s.png" % name))
        set_txt(u.GetDlgItem(P(d), B_KW), kw)
        time.sleep(1.2)
        print("   筛「%s」→ 行数=%d · %s" % (kw, rows(lv), txt(u.GetDlgItem(P(d), B_CNT))))
        close_dlg(d)

    print("== 加入队列：装备 → 筛「剑」→ 全选 → 加入所选 ==")
    d = open_cat(h, Q_BEQU)
    if d:
        lv = wt.HWND(u.GetDlgItem(P(d), B_LIST))
        set_txt(u.GetDlgItem(P(d), B_KW), "剑")
        time.sleep(1.2)
        n = rows(lv)
        click(u.GetDlgItem(P(d), B_SEL), 0.6)      # 全选（自己进程里选，安全）
        click(u.GetDlgItem(P(d), B_ADD), 1.2)      # 加入所选
        print("   列表 %d 行 → %s" % (n, txt(u.GetDlgItem(P(d), B_MSG))))
        close_dlg(d)
    else:
        print("   窗口没出来")

    time.sleep(1.0)
    switch(mw, PAGE_GRANT)                         # 队列在发放页，切过去才数得到行数
    h2 = {}
    index_pages(P(mw), h2)
    print("   发放队列行数 = %d" % rows(h2.get(G_LIST)))
    grab(mw, os.path.join(SHOTS, "g_queue.png"))
    print("（队列只在内存里，进程退出即清空，无需还原）")
    p.terminate()


if __name__ == "__main__":
    main()
