"""日志页倒序验证：编辑框第一行必须是文件最后一行（最新的在最顶）。"""
import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import wait_main  # noqa: E402
from pages import txt

u = ctypes.windll.user32
u.GetDlgCtrlID.restype = ctypes.c_int
u.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
u.GetDlgItem.restype = ctypes.c_void_p
u.SendMessageW.restype = ctypes.c_void_p
u.PostMessageW.restype = ctypes.c_bool
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]

WM_GETTEXT, WM_GETTEXTLENGTH = 0x000D, 0x000E
L_TEXT = 1000 + 11 * 100 + 1  # PAGE_ID(PAGE_LOG, 1)


class NMHDR(ctypes.Structure):
    _fields_ = [("hwndFrom", ctypes.c_void_p), ("idFrom", ctypes.c_size_t), ("code", ctypes.c_uint)]


def index_pages(hwnd, out):
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


def switch(main, idx):
    tab = u.GetDlgItem(main, 190)
    u.SendMessageW(tab, 0x130C, idx, 0)
    nh = NMHDR(tab, 190, 0xFFFFFDD9)
    u.SendMessageW(main, 0x004E, 190, ctypes.cast(ctypes.byref(nh), ctypes.c_void_p).value)
    time.sleep(1.2)


def main():
    # 先造几条带序号的日志，倒序一眼就能看出来对不对
    p = subprocess.Popen([os.path.join(ROOT, "DNF_GM.exe")], cwd=ROOT)
    mw = ctypes.c_void_p(wait_main(p))
    time.sleep(3.0)
    h = {}
    index_pages(mw, h)
    switch(mw, 11)          # 日志页
    time.sleep(1.0)
    index_pages(mw, h)

    log = os.path.join(ROOT, "config", "logs", time.strftime("log-%Y%m%d.txt"))
    raw = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    flines = [l for l in raw.replace("\r\n", "\n").split("\n") if l.strip()]
    box = txt(h[L_TEXT]).replace("\r\n", "\n").split("\n")
    box = [l for l in box if l.strip()]

    print("文件行数：%d   页面行数：%d" % (len(flines), len(box)))
    print("文件最后一行：", flines[-1][:70] if flines else "(空)")
    print("页面第一行  ：", box[0][:70] if box else "(空)")
    print("文件第一行  ：", flines[0][:70] if flines else "(空)")
    print("页面最后一行：", box[-1][:70] if box else "(空)")
    ok = bool(flines and box and flines[-1][:40] == box[0][:40] and flines[0][:40] == box[-1][:40])
    print("倒序一致：", "是" if ok else "否")
    p.terminate()


main()
