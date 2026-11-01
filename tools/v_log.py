"""日志页倒序验证：编辑框第一行必须是文件最后一行（最新的在最顶）。"""
import os, sys, time, subprocess, ctypes, ctypes.wintypes as wt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from pages import wait_main, BUILD  # noqa: E402
from pages import txt, EXE

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
    p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    mw = ctypes.c_void_p(wait_main(p))
    time.sleep(3.0)
    h = {}
    index_pages(mw, h)
    switch(mw, 11)          # 日志页
    time.sleep(1.0)
    index_pages(mw, h)

    # 日志按 exe 目录落（art_dir 取 exe 旁边），仓库根那份是老 exe 留下的
    log = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))
    raw = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    flines = [l for l in raw.replace("\r\n", "\n").split("\n") if l.strip()]
    # 日志文件可能上千行，默认 256 字符的读长会把编辑框内容截断，
    # 页面最后一行就永远是第 7 行而不是文件第一行
    box = txt(h[L_TEXT], 1 << 20).replace("\r\n", "\n").split("\n")
    box = [l for l in box if l.strip()]

    print("文件行数：%d   页面行数：%d" % (len(flines), len(box)))
    print("文件最后一行：", flines[-1][:70] if flines else "(空)")
    print("页面第一行  ：", box[0][:70] if box else "(空)")
    print("文件第一行  ：", flines[0][:70] if flines else "(空)")
    print("页面最后一行：", box[-1][:70] if box else "(空)")
    # 倒序的判据只有一条：文件最后一行（最新）必须出现在页面第一行。
    # 原来还要求「文件第一行 == 页面最后一行」（即页面显示得全），但日志页只取文件
    # 末尾 100000 字节（src/ui_data.c:794），连跑十几轮定向脚本之后日志轻松破 100KB，
    # 这条就恒不成立 —— 它是「显示上限」，不是「倒序错了」，混在一起会让这条脚本
    # 在日志变大后永久假红。显示上限单独报，不算倒序的账。
    ok = bool(flines and box and flines[-1][:40] == box[0][:40])
    print("倒序一致：", "是" if ok else "否")
    if len(flines) != len(box):
        print("注：页面只显示文件末尾约 100000 字节（src/ui_data.c:794 的截断），"
              "文件 %d 行 / 页面 %d 行，差的那部分是显示上限所致，不影响倒序" %
              (len(flines), len(box)))
    p.terminate()


main()
