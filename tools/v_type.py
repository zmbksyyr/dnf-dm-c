"""验证「类型由 PVF 判定」在发放页与邮件页的表现。

发 WM_SETTEXT 给编号框，再读回只读类型框的文字：
  - 发放页：装备编号 → 装备；材料 → 材料；任务材料 → 任务材料；查不到的编号 → 消耗品(默认)
  - 邮件页：时装编号 → 时装（左边标签跟着变成「时装 ID」）；普通编号 → 普通物品
"""
import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import wait_main, db  # noqa: E402
from pages import txt

u = ctypes.windll.user32
u.GetDlgCtrlID.restype = ctypes.c_int
u.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
u.GetDlgItem.restype = ctypes.c_void_p
u.SendMessageW.restype = ctypes.c_void_p
u.PostMessageW.restype = ctypes.c_bool
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]

WM_SETTEXT, WM_GETTEXT = 0x000C, 0x000D
WM_GETTEXTLENGTH = 0x000E
GWL_STYLE = -16
ES_READONLY = 0x0800

IDC_CONN = 104
G_ID, G_TYPE = 1201, 1203
M_ATYPE, M_AID, M_HINT = 1307, 1308, 1312


def index_pages(hwnd, out):
    """页内控件挂在 Page 子窗口下，GetDlgItem(main) 找不到，只能递归枚举。"""
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def walk(h):
        def each(c, lp):
            cid = u.GetDlgCtrlID(ctypes.c_void_p(c))
            if cid and cid not in out:
                out[cid] = wt.HWND(c)
            walk(ctypes.c_void_p(c))
            return True

        u.EnumChildWindows(h, cb(each), 0)

    walk(hwnd)


def set_txt(h, cid, s):
    hw = h.get(cid)
    if not hw:
        return
    b = ctypes.create_unicode_buffer(s)
    u.SendMessageW(ctypes.c_void_p(hw.value), WM_SETTEXT, 0, b)
    time.sleep(0.3)


def readonly(h, cid):
    """只读框验证：ES_READONLY 应当在位。"""
    hw = h.get(cid)
    if not hw:
        return False
    u.GetWindowLongPtrW.restype = ctypes.c_longlong
    u.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    st = u.GetWindowLongPtrW(ctypes.c_void_p(hw.value), GWL_STYLE)
    return bool(st & ES_READONLY)


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", ctypes.c_void_p), ("idFrom", ctypes.c_size_t), ("code", ctypes.c_uint)]


def switch(main, idx):
    """切签要把 TCN_SELCHANGE 一起补发：只 SETCURSEL 的话页面不会真的建起来/换掉。"""
    tab = u.GetDlgItem(main, 190)
    u.SendMessageW(tab, 0x130C, idx, 0)  # TCM_SETCURSEL
    nh = NMHDR(tab, 190, 0xFFFFFDD9)     # TCN_SELCHANGE = -551
    u.SendMessageW(main, 0x004E, 190, ctypes.cast(ctypes.byref(nh), ctypes.c_void_p).value)
    time.sleep(1.2)


def sample_ids():
    """编号样本：既能验 interfaces（装备/消耗品/材料/任务材料），也能验邮件页的时装与宠物。
    宠物取的是 dnf_creature_info 的 it_id —— PVF 里它藏在装备目录，只能靠这张表认出来。"""
    out = [("10000", "PVF 装备"), ("1232", "PVF 消耗品"), ("3012", "PVF 材料"),
           ("4066", "PVF 任务材料"), ("11907", "PVF 时装")]
    try:
        c = db()
        with c.cursor() as cur:
            cur.execute("SELECT it_id FROM taiwan_cain_web.dnf_creature_info WHERE it_id>0 LIMIT 3")
            out += [(str(r[0]), "宠物表 it_id") for r in cur.fetchall()]
            cur.execute("SELECT item_id FROM taiwan_cain_2nd.postal WHERE item_id>0 LIMIT 3")
            out += [(str(r[0]), "库里邮件附件") for r in cur.fetchall()]
        c.close()
    except Exception as e:
        print("取样本失败：", e)
    return out


def main():
    p = subprocess.Popen([os.path.join(ROOT, "DNF_GM.exe")], cwd=ROOT)
    mw = ctypes.c_void_p(wait_main(p))
    u.PostMessageW(mw, 0x0111, IDC_CONN, 0)  # 连库
    time.sleep(7.0)

    h = {}
    cases = [("0", "空"), ("123456789", "PVF 里查不到")] + sample_ids()

    print("== 发放页 ==")
    switch(mw, 2)
    index_pages(mw, h)          # 页面是用的那一刻才建，切完必须重新索引
    print("  类型框只读：", readonly(h, G_TYPE))
    for v, label in cases:
        set_txt(h, G_ID, v)
        print("  编号 %-12s -> 类型 [%s]   (%s)" % (v, txt(h, G_TYPE), label))

    print("== 邮件页 ==")
    switch(mw, 3)
    index_pages(mw, h)
    print("  类型框只读：", readonly(h, M_ATYPE))
    for v, label in cases:
        set_txt(h, M_AID, v)
        print("  附件 %-12s -> 类型 [%s] · 标签 [%s]   (%s)"
              % (v, txt(h, M_ATYPE), txt(h, M_HINT), label))

    p.terminate()


main()
