# -*- coding: utf-8 -*-
"""数据库用例：写库路径（改名/克隆/邮件/宠物/解锁/泡点）与连接生命周期。

每个用例只做自己的操作：写库前快照，finally 里还原；用例之间不依赖执行顺序。
"""

import ctypes
import ctypes.wintypes as wt
import glob
import os
import time

from . import core

u = core.u
k32 = ctypes.windll.kernel32

# 状态栏第二段的文本要读进目标进程的缓冲，句柄与指针宽度必须显式声明
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]

u.IsWindowEnabled.argtypes = [wt.HWND]
u.IsWindowEnabled.restype = wt.BOOL
u.GetParent.argtypes = [wt.HWND]
u.GetParent.restype = wt.HWND
u.GetDlgCtrlID.argtypes = [wt.HWND]
u.GetDlgCtrlID.restype = ctypes.c_int
u.GetWindow.argtypes = [wt.HWND, wt.UINT]
u.GetWindow.restype = wt.HWND

SB_GETTEXTW = 0x040D
WM_CLOSE = 0x0010

IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191
PAGE_MAIL, PAGE_PET, PAGE_GM, PAGE_BUBBLE = 3, 4, 8, 9

G_NAME, G_UNLOCK, G_NO_WEIGHT, G_FATIGUE = 1801, 1817, 1815, 1819
G_SCANBAD, G_CLONE = 1827, 1828

M_TEXT, M_AID, M_SEND = 1310, 1308, 1311
P_LIST, P_NAME, P_RENAME = 1401, 1402, 1403

# 页号、列表控件、刷新按钮
LIST_PAGES = {"pet": (PAGE_PET, 1401, 1407),
              "avatar": (5, 1501, 1505),
              "quest": (6, 1601, 1602)}

BUB_IDS = {"on": core.PID(PAGE_BUBBLE, 4), "sec": core.PID(PAGE_BUBBLE, 5),
           "target": core.PID(PAGE_BUBBLE, 6), "tno": core.PID(PAGE_BUBBLE, 7),
           "hours": core.PID(PAGE_BUBBLE, 8), "coin": core.PID(PAGE_BUBBLE, 9),
           "dp": core.PID(PAGE_BUBBLE, 10), "dc": core.PID(PAGE_BUBBLE, 11),
           "item": core.PID(PAGE_BUBBLE, 12), "num": core.PID(PAGE_BUBBLE, 13),
           "apply": core.PID(PAGE_BUBBLE, 14), "info": core.PID(PAGE_BUBBLE, 16)}

LETTER_TABLE = "taiwan_cain_2nd.letter"
LETTER_BAK = "taiwan_cain_2nd.letter_selfcheck_bak"
CONFIRM_SCAN = "扫描全服所有角色"
CANCEL_LOG = "断开取消：后台任务 30 秒内没有退出"
REFUSE_TXT = "后台任务还在跑，稍后再断开"
SKIP_LOG = "退出：有任务线程 30 秒未退出，跳过资源释放"
CRASH = {0xC0000005: "STATUS_ACCESS_VIOLATION", 0xC0000374: "STATUS_HEAP_CORRUPTION",
         0xC0000006: "STATUS_IN_PAGE_ERROR", 0xC0000409: "STATUS_STACK_BUFFER_OVERRUN"}

CN_NAME = "中文测试"
MAIL_TEXT = "中文正文测试：闭环第二轮"
PET_NAME = "中文宠物"

INI = os.path.join(os.path.dirname(core.EXE), "config", "settings.ini")


# ---------------------------------------------------------------- 界面读写
def _enabled(h):
    return bool(h) and bool(u.IsWindowEnabled(h))


def _status(ctx, cap=512):
    """状态栏第一段（WM_GETTEXT 会返回第一段）。"""
    return core.txt(u.GetDlgItem(ctx.main, IDC_STATUS), cap)


def _status_part(ctx, part):
    """读状态栏指定段：缓冲分配在目标进程里，不把本进程指针传过去。"""
    h = k32.OpenProcess(0x0008 | 0x0010 | 0x0020 | 0x0400, False, ctx.proc.pid)
    if not h:
        return ""
    addr = None
    try:
        addr = k32.VirtualAllocEx(h, None, 4096, 0x3000, 0x04)
        if not addr:
            return ""
        hwnd = u.GetDlgItem(ctx.main, IDC_STATUS)
        n = u.SendMessageW(hwnd, SB_GETTEXTW, part, addr) & 0xFFFF
        if not n:
            return ""
        buf = ctypes.create_string_buffer(4096)
        got = ctypes.c_size_t(0)
        k32.ReadProcessMemory(h, addr, ctypes.addressof(buf), 4096, ctypes.byref(got))
        return buf.raw[:n * 2].decode("utf-16-le", "replace")
    finally:
        if addr:
            k32.VirtualFreeEx(h, addr, 0, 0x8000)
        k32.CloseHandle(h)


def _boxes(main):
    """本程序拥有的可见 #32770 弹窗。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        if core.cls_of(h) != "#32770":
            return True
        if u.GetWindow(h, 4) not in (main, 0):
            return True
        out.append(h)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _box_text(h):
    parts = []

    def each(c, lp):
        cls = core.cls_of(c)
        if cls in ("Static", "Button"):
            t = core.txt(c, 2048)
            if t:
                parts.append(t)
        return True

    u.EnumChildWindows(h, ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                             ctypes.c_void_p)(each), 0)
    return " / ".join(parts)


def _wait_box(main, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        bs = _boxes(main)
        if bs:
            time.sleep(0.2)
            return bs[0], _box_text(bs[0])
        time.sleep(0.05)
    return None, ""


def _close_box(main, h, btn=1, timeout=8.0):
    b = u.GetDlgItem(h, btn)
    if not b and btn != 1:
        b = u.GetDlgItem(h, 1)          # 提示框只有 OK
    if b:
        core.press(b, 0.0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if h not in _boxes(main):
            return True
        time.sleep(0.05)
    return False


def _dismiss_boxes(ctx, btn=1):
    for h in _boxes(ctx.main):
        _close_box(ctx.main, h, btn)


def _press_cid(ctx, cid, wait=0.6):
    h = core.ctl(ctx.main, cid)
    if not h:
        return None
    core.press(h, wait)
    return h


def _set_combo(h, idx):
    u.SendMessageW(h, core.CB_SETCURSEL, idx, 0)
    u.SendMessageW(u.GetParent(h), core.WM_COMMAND,
                   (0x0001 << 16) | u.GetDlgCtrlID(h), h)


def _wait_status(ctx, needle, timeout=25.0):
    return core.wait_until(lambda: needle in _status(ctx), timeout)


def _need_connected(ctx, timeout=25.0):
    """连接已就绪（断开按钮可用）。"""
    return core.wait_until(lambda: _enabled(u.GetDlgItem(ctx.main, IDC_DISC)), timeout)


def _select_char(ctx, timeout=25.0):
    """点角色列表第一行，等载入完成（g.cur 就位）。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not core.wait_until(lambda: core.rows(lv) > 0, 20.0):
        return False
    core.click_at(lv, 40, 12, 0.4)
    return core.wait_until(lambda: "已载入" in _status(ctx), timeout)


def _rename_char(ctx, cno, name, timeout=10.0):
    """GM 页名字框：填名 + 失焦即提交，返回库里最终的名字。"""
    h = core.ctl(ctx.main, G_NAME)
    if not h:
        return None
    core.set_text(h, name)
    u.SendMessageW(h, core.WM_KILLFOCUS, 0, 0)
    t0 = time.time()
    got = _char_name(ctx, cno)
    while time.time() - t0 < timeout:
        got = _char_name(ctx, cno)
        if got == name:
            return got
        _dismiss_boxes(ctx)
        time.sleep(0.3)
    return _char_name(ctx, cno)


# ---------------------------------------------------------------- 数据库快照
def _char_snapshot(ctx, cno):
    r = ctx.q("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),LENGTH(charac_name) "
              "FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    return r[0] if r else None


def _char_name(ctx, cno):
    snap = _char_snapshot(ctx, cno)
    return snap[0] if snap else None


def _view_snapshot(ctx, mid):
    r = ctx.q("SELECT charac_count,LENGTH(info),HEX(info) FROM taiwan_cain.charac_view "
              "WHERE m_id=%s", (mid,))
    return r[0] if r else None


def _view_restore(ctx, mid, snap):
    ctx.cur.execute("UPDATE taiwan_cain.charac_view SET info=UNHEX(%s), charac_count=%s "
                    "WHERE m_id=%s", (snap[2], snap[0], mid))


def _charac_tables(ctx):
    rows = ctx.q("SELECT table_schema,table_name FROM information_schema.columns "
                 "WHERE column_name='charac_no' AND table_schema IN ('taiwan_cain','taiwan_cain_2nd')")
    return ["%s.%s" % (a, b) for a, b in rows]


def _counts(ctx, tabs):
    return {t: ctx.q1("SELECT COUNT(*) FROM %s" % t) for t in tabs}


def _drop_char(ctx, no, tabs):
    for t in tabs:
        ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s" % t, (no,))
    ctx.cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE charac_no=%s", (no,))


class _TableLock:
    """第二条连接上的 LOCK TABLES：把「作业卡在库上」变成可控量。"""

    def __init__(self, tables):
        self.tables = tables
        self.c = None

    def lock(self):
        self.c = core.db()
        self.c.cursor().execute("LOCK TABLES " + ",".join(t + " WRITE" for t in self.tables))

    def unlock(self):
        if self.c is None:
            return
        c, self.c = self.c, None
        try:
            c.cursor().execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass


def _wait_blocked(ctx, marker, timeout=25.0):
    """PROCESSLIST 里出现该表的 metadata lock 等待。"""
    st = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, state, inf in ctx.q("SELECT ID,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                                   "WHERE COMMAND='Query'"):
            st = state or ""
            if marker in (inf or "") and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, st


def _app_dialogs(main):
    """本程序的 DnfDlg 窗口（扫描结果等）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        if core.cls_of(h) != "DnfDlg":
            return True
        if u.GetWindow(h, 4) != main:
            return True
        out.append(h)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _wait_scan_idle(ctx, timeout=45.0):
    """等全服扫描收尾并关掉结果窗，免得模态窗挡住后面的断开。"""
    t0 = time.time()
    quiet = 0
    while time.time() - t0 < timeout:
        for h in _app_dialogs(ctx.main):
            u.PostMessageW(h, WM_CLOSE, 0, 0)
        busy = any("inventory" in (inf or "")
                   for _, _, inf in ctx.q("SELECT ID,STATE,IFNULL(INFO,'') FROM "
                                          "information_schema.PROCESSLIST WHERE COMMAND='Query'"))
        quiet = 0 if busy else quiet + 1
        if quiet >= 5 and not _app_dialogs(ctx.main):
            return True
        time.sleep(0.2)
    return False


def _list_rows(ctx, name):
    h = core.ctl(ctx.main, LIST_PAGES[name][1])
    return core.rows(h) if h else -1


def _wait_list_rows(ctx, name, want, timeout=12.0):
    t0 = time.time()
    got = -1
    while time.time() - t0 < timeout:
        got = _list_rows(ctx, name)
        if got == want:
            return got
        time.sleep(0.2)
    return got


def _list_want(ctx, cno):
    """三页首屏应有的行数，按库实时算。"""
    n_pet = ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.creature_items WHERE charac_no=%s", (cno,))
    n_ava = ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE charac_no=%s", (cno,))
    rows = ctx.q("SELECT " + ",".join("play_%d" % i for i in range(1, 21)) +
                 " FROM taiwan_cain.new_charac_quest WHERE charac_no=%s", (cno,))
    n_quest = sum(1 for x in rows[0] if x) if rows else 0
    return {"pet": n_pet, "avatar": n_ava, "quest": n_quest}


# ---------------------------------------------------------------- 日志 / 配置
def _log_file():
    d = os.path.join(os.path.dirname(core.EXE), "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def _log_mark():
    p = _log_file()
    return (p, os.path.getsize(p)) if p else (None, 0)


def _log_since(mark):
    p, off = mark
    if not p or not os.path.exists(p):
        return ""
    with open(p, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def _log_since_lines(mark):
    return _log_since(mark).splitlines()


def _ini_snapshot():
    if not os.path.exists(INI):
        return None
    with open(INI, "rb") as f:
        return f.read()


def _ini_restore(data):
    if data is None:
        if os.path.exists(INI):
            os.remove(INI)
        return
    with open(INI, "wb") as f:
        f.write(data)


def _ini_get(key, section="ui"):
    import configparser
    cp = configparser.RawConfigParser()
    cp.read(INI, encoding="utf-8")
    return cp.get(section, key, fallback=None)


def _lev_max():
    """「解锁装备等级」写的是配置 [gm] lev_max，缺省 85。"""
    v = 85
    try:
        v = int(_ini_get("lev_max", "gm") or 85)
    except (TypeError, ValueError):
        v = 85
    return v if 1 <= v <= 255 else 85


def _ini_set_raw(key, val, section="ui"):
    """按行改一个键，不动其它内容。"""
    with open(INI, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    out, sec, hit = [], None, False
    for ln in lines:
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1]
        if sec == section and s.split("=")[0].strip() == key:
            out.append("%s =%s" % (key, val))
            hit = True
        else:
            out.append(ln)
    if not hit:
        raise RuntimeError("settings.ini 里没有 [%s] %s" % (section, key))
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def _ini_minimal():
    os.makedirs(os.path.dirname(INI), exist_ok=True)
    with open(INI, "w", encoding="utf-8") as f:
        f.write("[db]\nhost =127.0.0.1\nport =3306\nuser =game\npwd =uu5!^%jg\n"
                "charset =45\n\n[ui]\nfont =9\npoll_sec =3\nhotkey =Tab\n")


def _bubble_ini_restore(data):
    """还原泡点配置，并保证开关落回关闭，后续用例启动时定时器不会自动开始。"""
    if data is None:
        if os.path.exists(INI):
            os.remove(INI)
        return
    lines = data.decode("utf-8", "replace").splitlines(True)
    sec = None
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1]
        elif sec == "bubble" and s.replace(" ", "").startswith("on="):
            lines[i] = ln.replace("on=1", "on=0", 1)
            break
    with open(INI, "w", encoding="utf-8", newline="") as f:
        f.write("".join(lines))


# ---------------------------------------------------------------- 自检
@core.case("db.selfcheck_missing", needs=("gui", "db"), group="always",
           desc="缺可选表时连接自检在状态栏与日志里点名报出")
def selfcheck_missing(ctx):
    conn = u.GetDlgItem(ctx.main, IDC_CONN)
    disc = u.GetDlgItem(ctx.main, IDC_DISC)
    if not ctx.ok("已连上测试库", _need_connected(ctx), _status(ctx)):
        return
    core.press(disc, 0.0)
    if not ctx.ok("断开后「连接」可用", core.wait_until(lambda: _enabled(conn), 25), _status(ctx)):
        return

    renamed = False
    try:
        ctx.cur.execute("RENAME TABLE %s TO %s" % (LETTER_TABLE, LETTER_BAK))
        renamed = True
        mark = _log_mark()
        core.press(conn, 0.0)
        _wait_status(ctx, "自检")
        st = _status(ctx)
        ctx.ok("状态栏报出表/列自检有问题", "表/列自检" in st and "问题" in st, repr(st))
        core.wait_until(lambda: LETTER_TABLE in _log_since(mark), 5.0)
        text = _log_since(mark)
        ctx.ok("日志点名缺 %s" % LETTER_TABLE,
               "缺表" in text and LETTER_TABLE in text, text.splitlines()[-2:] if text else "(空)")
    finally:
        if renamed:
            ctx.cur.execute("RENAME TABLE %s TO %s" % (LETTER_BAK, LETTER_TABLE))
        ctx.check("letter 表已还原（行数可查）",
                  ctx.q1("SELECT COUNT(*) FROM %s" % LETTER_TABLE) is not None, True)


# ---------------------------------------------------------------- 解锁装备等级
@core.case("db.unlock_equip", needs=("gui", "db"), group="always",
           desc="解锁装备等级只写 max_equip_level，保留标签角色与支援兵列")
def unlock_equip(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return

    def row():
        r = ctx.q("SELECT charac_no,tag_charac_no,striker_skill_index,max_equip_level "
                  "FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (cno,))
        return r[0] if r else None

    orig = row()
    lev_max = _lev_max()
    seed_max = 50 if lev_max != 50 else 40
    before_counts = _counts(ctx, ("taiwan_cain.charac_info", "taiwan_cain.charac_manage_info",
                                  "taiwan_cain.charac_view"))
    try:
        ctx.cur.execute("DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (cno,))
        ctx.cur.execute("INSERT INTO taiwan_cain.charac_manage_info "
                        "(charac_no,tag_charac_no,striker_skill_index,max_equip_level) "
                        "VALUES (%s,4242,7,%s)", (cno, seed_max))
        ctx.check("造出待解锁的行（tag=4242 / striker=7 / 上限=%d）" % seed_max,
                  row(), (cno, 4242, 7, seed_max))

        if not ctx.ok("角色可选中", _select_char(ctx)):
            return
        ctx.page(PAGE_GM)
        h = core.ctl(ctx.main, G_UNLOCK)
        if not ctx.ok("「解锁装备等级」按钮存在", bool(h), "控件 %d" % G_UNLOCK):
            return
        core.press(h, 0.0)
        got = None
        t0 = time.time()
        while time.time() - t0 < 12:
            got = row()
            if got and got[3] == lev_max:
                break
            time.sleep(0.3)
        time.sleep(0.8)
        got = row()
        ctx.ok("max_equip_level 写成配置上限 %d" % lev_max, got and got[3] == lev_max, repr(got))
        ctx.ok("tag_charac_no 保留 4242（未被清 0）", got and got[1] == 4242, repr(got))
        ctx.ok("striker_skill_index 保留 7（未被清 0）", got and got[2] == 7, repr(got))
        ctx.check("没有多出行（仍是 1 行）",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_manage_info WHERE charac_no=%s",
                         (cno,)), 1)
    finally:
        ctx.cur.execute("DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=%s", (cno,))
        if orig:
            ctx.cur.execute("INSERT INTO taiwan_cain.charac_manage_info "
                            "(charac_no,tag_charac_no,striker_skill_index,max_equip_level) "
                            "VALUES (%s,%s,%s,%s)", orig)
        ctx.check("还原后 charac_manage_info 与快照一致", row(), orig)
        after = _counts(ctx, ("taiwan_cain.charac_info", "taiwan_cain.charac_manage_info",
                              "taiwan_cain.charac_view"))
        ctx.check("charac_info 行数不变", after["taiwan_cain.charac_info"],
                  before_counts["taiwan_cain.charac_info"])
        ctx.check("charac_view 行数不变", after["taiwan_cain.charac_view"],
                  before_counts["taiwan_cain.charac_view"])


# ---------------------------------------------------------------- 列表
@core.case("db.lists_load", needs=("gui", "db"), group="always",
           desc="宠物/时装/任务三页首屏行数与库一致")
def lists_load(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    if not ctx.ok("角色可选中", _select_char(ctx)):
        return
    want = _list_want(ctx, cno)
    for name in ("pet", "avatar", "quest"):
        ctx.page(LIST_PAGES[name][0])
        got = _wait_list_rows(ctx, name, want[name])
        ctx.check("%s 页行数 == 库里行数" % name, got, want[name])


@core.case("db.lists_refresh", needs=("gui", "db"), group="always",
           desc="刷新与切页交织轰炸后列表仍不空、行数仍与库一致")
def lists_refresh(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    if not ctx.ok("角色可选中", _select_char(ctx)):
        return
    want = _list_want(ctx, cno)
    for name in ("pet", "avatar", "quest"):
        ctx.page(LIST_PAGES[name][0])
        _wait_list_rows(ctx, name, want[name])
        if not ctx.ok("%s 页刷新按钮存在" % name,
                      bool(core.ctl(ctx.main, LIST_PAGES[name][2]))):
            return

    # 12 轮「刷新 → 立刻切走 → 再回来」：交换写错或所有权丢了会留下空列表
    for _r in range(12):
        for name in ("quest", "pet", "avatar"):
            ctx.page(LIST_PAGES[name][0])
            _press_cid(ctx, LIST_PAGES[name][2], 0.0)
            time.sleep(0.05)
        if ctx.proc.poll() is not None:
            break
    time.sleep(2.5)

    ctx.ok("轰炸中进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    for name in ("pet", "avatar", "quest"):
        ctx.page(LIST_PAGES[name][0])
        got = _wait_list_rows(ctx, name, want[name], 15.0)
        ctx.check("轰炸后 %s 页行数仍正确" % name, got, want[name])

    # 刷新期间采样：列表不许先清空再重填（本来就没有行时无从观察）
    ctx.page(6)
    _press_cid(ctx, LIST_PAGES["quest"][2], 0.0)
    seen = []
    t0 = time.time()
    while time.time() - t0 < 6:
        seen.append(_list_rows(ctx, "quest"))
        time.sleep(0.02)
    if want["quest"] > 0:
        ctx.ok("刷新期间任务页列表没有掉到 0 行", bool(seen) and min(seen) > 0,
               "min=%s" % (min(seen) if seen else None))
    else:
        ctx.skip("任务页本来就没有行，跳过「刷新不掉到 0 行」采样")
    ctx.ok("刷新收敛到库里的行数", bool(seen) and seen[-1] == want["quest"],
           "last=%s" % (seen[-1] if seen else None))


# ---------------------------------------------------------------- 连接生命周期
@core.case("db.conn_stress", needs=("gui", "db"), group="always",
           desc="载入中直接断开 12 轮不崩、日志无作业超时、之后仍能载入")
def conn_stress(ctx):
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not ctx.ok("角色列表读出来了", core.wait_until(lambda: core.rows(lv) > 0, 20),
                  "rows=%d" % core.rows(lv)):
        return
    mark = _log_mark()
    alive = []
    for i in range(12):
        core.click_at(lv, 40, 12, 0.0)
        time.sleep(0.10)                       # 让载入刚进 db_load_data
        core.press(u.GetDlgItem(ctx.main, IDC_DISC), 0.0)
        time.sleep(1.2)
        alive.append(ctx.proc.poll() is None)
        if ctx.proc.poll() is not None:
            break
        core.press(u.GetDlgItem(ctx.main, IDC_CONN), 0.0)
        core.wait_until(lambda: core.rows(lv) > 0, 20)

    ctx.ok("12 轮「载入中直接断开」都没崩", all(alive) and len(alive) == 12,
           "%d/12" % sum(alive))
    ctx.ok("进程仍在运行", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    bad = [l for l in _log_since_lines(mark) if "30 秒未退出" in l or "放弃等待" in l]
    ctx.ok("日志里没有作业停不下来的记录", not bad, str(bad[-2:]))

    # 连接状态机没被带坏：还能正常载入一个角色
    core.click_at(lv, 40, 12, 0.0)
    t0 = time.time()
    stat = ""
    while time.time() - t0 < 12:
        stat = _status(ctx)
        if "已载入" in stat:
            break
        time.sleep(0.3)
    ctx.ok("断开 12 轮后仍能载入角色", "已载入" in stat, repr(stat))


@core.case("db.disc_queue", needs=("gui", "db"), group="always",
           desc="断开时丢掉排队动作、状态栏与按钮状态正确、重连不补执行")
def disc_queue(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    fat0 = ctx.q("SELECT fatigue,max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    fat0 = fat0[0] if fat0 else None
    w0 = ctx.q1("SELECT inven_weight FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    if not ctx.ok("已连上测试库", _need_connected(ctx), _status(ctx)):
        return
    if not ctx.ok("角色可选中", _select_char(ctx)):
        return
    ctx.page(PAGE_GM)

    conn = u.GetDlgItem(ctx.main, IDC_CONN)
    disc = u.GetDlgItem(ctx.main, IDC_DISC)
    ctx.ok("起点已连库（连接不可用、断开可用）", (not _enabled(conn)) and _enabled(disc),
           "conn=%s disc=%s" % (_enabled(conn), _enabled(disc)))
    mark = _log_mark()

    lock = _TableLock(["taiwan_cain_2nd.inventory"])
    try:
        # 扫描作业先堵在库上，让后面的「一键」动作只能排队
        lock.lock()
        _press_cid(ctx, G_SCANBAD, 0.0)
        b, t = _wait_box(ctx.main, 8.0)
        ctx.ok("扫描入口弹出确认框", b is not None, (t or "")[:40])
        if b:
            _close_box(ctx.main, b, 1)
        time.sleep(1.0)

        _press_cid(ctx, G_FATIGUE, 0.15)
        _press_cid(ctx, G_NO_WEIGHT, 0.15)

        core.press(disc, 0.0)
        time.sleep(0.35)
        lock.unlock()

        core.wait_until(lambda: _enabled(conn), 25)
        time.sleep(1.5)
        ctx.ok("断开后「连接」可用、「断开」置灰", _enabled(conn) and not _enabled(disc),
               "conn=%s disc=%s" % (_enabled(conn), _enabled(disc)))
        st1 = _status(ctx)
        ctx.ok("状态栏是「已断开数据库」", bool(st1) and st1.endswith("已断开数据库"), repr(st1))
        st1b = _status_part(ctx, 1)
        ctx.ok("状态栏第二段写着「库未连接」", bool(st1b) and st1b.startswith("库未连接"), repr(st1b))

        new = _log_since_lines(mark)
        spam = [l.strip() for l in new if "未连接" in l]
        allfail = [l.strip() for l in new if "失败：" in l]
        ctx.ok("没有「xxx 失败：未连接」刷屏", not spam, str(spam[:5]))
        ctx.ok("断开过程中没有失败日志", not allfail, str(allfail[:5]))
        ctx.ok("进程没有崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))

        # 重连后，排队期间点的「满疲劳」不能被补执行
        core.press(conn, 0.0)
        core.wait_until(lambda: _enabled(disc), 25)
        time.sleep(4.0)
        fat1 = ctx.q("SELECT fatigue,max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s",
                     (cno,))
        fat1 = fat1[0] if fat1 else None
        ctx.check("排队期间点的「满疲劳」没有被补执行", fat1, fat0)
        ctx.ok("重连后进程仍活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.unlock()
        if fat0:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET fatigue=%s, max_fatigue=%s, "
                            "inven_weight=%s WHERE charac_no=%s",
                            (fat0[0], fat0[1], w0, cno))


@core.case("db.jobstop_cancel", needs=("gui", "db"), group="always",
           desc="作业停不下来时拒绝断开、连接与队列保留、放锁后可重试")
def jobstop_cancel(ctx):
    if not ctx.ok("已连上测试库", _need_connected(ctx), _status(ctx)):
        return
    if not ctx.ok("角色可选中", _select_char(ctx)):
        return
    ctx.page(PAGE_GM)
    h = core.ctl(ctx.main, G_SCANBAD)
    if not ctx.ok("扫描入口存在", bool(h), "控件 %d" % G_SCANBAD):
        return

    disc = u.GetDlgItem(ctx.main, IDC_DISC)
    lock = _TableLock(["taiwan_cain_2nd.inventory"])
    try:
        lock.lock()
        core.press(h, 0.0)
        b, t = _wait_box(ctx.main, 8.0)
        ctx.ok("弹出扫描确认框（文案含「%s」）" % CONFIRM_SCAN,
               b is not None and CONFIRM_SCAN in (t or ""), (t or "")[:60])
        if b:
            _close_box(ctx.main, b, 1)
        blocked, st = _wait_blocked(ctx, "taiwan_cain_2nd.inventory")
        ctx.ok("扫描作业真的卡在库上", blocked, st)

        mark = _log_mark()
        core.press(disc, 0.0)
        time.sleep(33.0)                      # 等 job_stop 的 30 秒超时走完
        hit = [l for l in _log_since_lines(mark) if "断开取消" in l]
        ctx.ok("日志里有「断开取消：后台任务 30 秒内没有退出」的拒绝分支",
               bool(hit), hit[0] if hit else "(日志里没有)")
        ctx.ok("进程还活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))

        t = _probe_scan_entry(ctx)
        ctx.ok("连接没有被关（入口回「上一轮还没跑完」而不是「请先连接数据库」）",
               ctx.proc.poll() is None and "上一轮还没跑完" in (t or ""),
               "%r / poll=%s" % (t, ctx.proc.poll()))
    finally:
        lock.unlock()

    # 放锁后作业收尾，再点一次断开必须成功
    _wait_scan_idle(ctx)
    core.press(disc, 0.0)
    disc_txt = ""
    t0 = time.time()
    while time.time() - t0 < 30:
        disc_txt = _status(ctx)
        if "已断开数据库" in disc_txt:
            break
        if ctx.proc.poll() is not None:
            break
        time.sleep(0.3)
    ctx.ok("放锁后「再点一次断开」能成功（拒绝是可重试的）",
           "已断开数据库" in disc_txt, repr(disc_txt))
    ctx.ok("放锁后进程还活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))


def _probe_scan_entry(ctx, timeout=8.0):
    """点全服扫描入口，返回第一个弹框的文案，取消掉不开扫。"""
    _press_cid(ctx, G_SCANBAD, 0.0)
    b, t = _wait_box(ctx.main, timeout)
    if b:
        _close_box(ctx.main, b, 2)
    return t


@core.case("db.connstop_wait", needs=("gui", "db"), group="always",
           desc="连接作业在查库时断开要等它、拒绝文案正确、放锁后可重试")
def connstop_wait(ctx):
    conn = u.GetDlgItem(ctx.main, IDC_CONN)
    disc = u.GetDlgItem(ctx.main, IDC_DISC)
    if not ctx.ok("已连上测试库", _need_connected(ctx), _status(ctx)):
        return
    core.press(disc, 0.0)
    if not ctx.ok("起点已断开（连接按钮可用）", core.wait_until(lambda: _enabled(conn), 25),
                  _status(ctx)):
        return

    lock = _TableLock(["taiwan_cain.charac_info"])
    try:
        lock.lock()
        mark = _log_mark()
        core.press(conn, 0.0)
        blocked, st = _wait_blocked(ctx, "taiwan_cain.charac_info")
        ctx.ok("连接作业真的卡在库上（job_conn 停在 db_list_chars 里）", blocked, st)
        if not blocked:
            return

        core.press(disc, 0.0)
        time.sleep(34.0)                      # 等 job_stop 的 30 秒超时走完
        st_txt = _status(ctx)
        cancel = [l for l in _log_since_lines(mark) if CANCEL_LOG in l]
        ctx.ok("日志里有「断开取消：后台任务 30 秒内没有退出，连接与队列都保留」",
               bool(cancel), cancel[0] if cancel else "(日志里没有)")
        ctx.ok("状态栏是拒绝文案「%s」，不是「已断开数据库」" % REFUSE_TXT,
               REFUSE_TXT in st_txt and "已断开数据库" not in st_txt, repr(st_txt))
        rc = ctx.proc.poll()
        ctx.ok("进程还活着（没有一边 free 一边 recv）", rc is None, "poll=%s" % (rc,))
        if rc is not None:
            ctx.ok("不是崩溃码", (rc & 0xFFFFFFFF) not in CRASH, "0x%08X" % (rc & 0xFFFFFFFF))
            return
    finally:
        lock.unlock()

    # 放锁后连接作业收尾，再点一次断开必须成功
    core.press(disc, 0.0)
    disc_txt = ""
    t0 = time.time()
    while time.time() - t0 < 45:
        if ctx.proc.poll() is not None:
            break
        disc_txt = _status(ctx)
        if "已断开数据库" in disc_txt:
            break
        time.sleep(0.3)
    ctx.ok("放锁后「再点一次断开」能成功（拒绝是可重试的）",
           "已断开数据库" in disc_txt, repr(disc_txt))
    ctx.ok("放锁后进程还活着且没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))


@core.case("db.shutdown_skip", needs=("gui", "db"), group="always",
           desc="退出时有作业没停就跳过释放、进程正常退出、cfg_save 仍落盘")
def shutdown_skip(ctx):
    ini = _ini_snapshot()
    try:
        if ini is None:
            _ini_minimal()
        ps0 = _ini_get("poll_sec")
        if not ctx.ok("settings.ini 读得到 [ui] poll_sec", ps0 is not None, repr(ps0)):
            return
        if not ctx.ok("已连上测试库", _need_connected(ctx), _status(ctx)):
            return
        if not ctx.ok("角色可选中", _select_char(ctx)):
            return
        ctx.page(PAGE_GM)
        h = core.ctl(ctx.main, G_SCANBAD)
        if not ctx.ok("扫描入口存在", bool(h), "控件 %d" % G_SCANBAD):
            return

        # 外部把 poll_sec 改脏：cfg_save 执行就必须被 g.cfg 覆盖回去
        _ini_set_raw("poll_sec", "9")
        ctx.check("已把 settings.ini 的 [ui] poll_sec 外部改成 9",
                  (_ini_get("poll_sec") or "").strip(), "9")

        lock = _TableLock(["taiwan_cain_2nd.inventory"])
        try:
            lock.lock()
            core.press(h, 0.0)
            b, t = _wait_box(ctx.main, 8.0)
            ctx.ok("弹出扫描确认框（文案含「%s」）" % CONFIRM_SCAN,
                   b is not None and CONFIRM_SCAN in (t or ""), (t or "")[:60])
            if b:
                _close_box(ctx.main, b, 1)
            blocked, st = _wait_blocked(ctx, "taiwan_cain_2nd.inventory")
            ctx.ok("扫描作业真的卡在库上（job_act 停不下来）", blocked, st)

            mark = _log_mark()
            t0 = time.time()
            u.PostMessageW(ctx.main, WM_CLOSE, 0, 0)
            rc = None
            while time.time() - t0 < 150:
                rc = ctx.proc.poll()
                if rc is not None:
                    break
                time.sleep(0.5)
            sec = time.time() - t0
            ctx.ok("进程退出了（没有卡死）", rc is not None, "%.1f 秒" % sec)
            if rc is None:
                return
            ctx.ok("只等了一次 30 秒（没有重复等待）", sec <= 45, "%.1f 秒" % sec)
            ctx.ok("进程正常退出：returncode == 0", rc == 0,
                   "rc=%s / 0x%08X" % (rc, rc & 0xFFFFFFFF))
            ctx.ok("不是那几个 use-after-free 崩溃码", (rc & 0xFFFFFFFF) not in CRASH,
                   "0x%08X" % (rc & 0xFFFFFFFF))

            text = _log_since(mark)
            skip = [l for l in text.splitlines() if SKIP_LOG in l]
            ctx.ok("日志里有「退出：有任务线程 30 秒未退出，跳过资源释放…」",
                   bool(skip), skip[0] if skip else "(日志里没有)")
            ctx.ok("日志里的「退出」收尾行仍然在",
                   any(l.rstrip().endswith("退出") for l in text.splitlines()),
                   repr(text.splitlines()[-3:]))

            ps1 = _ini_get("poll_sec")
            ctx.check("cfg_save 仍然执行了（外部改的 9 被 %s 覆盖回去）" % ps0,
                      (ps1 or "").strip(), ps0.strip())
        finally:
            lock.unlock()
    finally:
        _ini_restore(ini)


# ---------------------------------------------------------------- 字符集：改名
@core.case("db.charset_char", needs=("gui", "db"), group="always",
           desc="中文角色名按字符数落库且可逐字节还原")
def charset_char(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    orig = _char_snapshot(ctx, cno)
    if not ctx.ok("读到角色快照", orig is not None, repr(orig)):
        return
    target = CN_NAME if orig[0] != CN_NAME else CN_NAME + "改"

    try:
        if not ctx.ok("角色可选中", _select_char(ctx)):
            return
        ctx.page(PAGE_GM)
        got = _rename_char(ctx, cno, target)
        ctx.check("改名成中文后库里就是「%s」" % target, got, target)
        row = _char_snapshot(ctx, cno)
        ctx.check("CHAR_LENGTH=%d（汉字按字符算，双重编码会是 3 倍）" % len(target),
                  row[2], len(target))
        ctx.check("LENGTH=%d（每个汉字 3 字节）" % (3 * len(target)), row[3], 3 * len(target))
        ctx.ok("utf8mb4 读回无乱码", row[0] == target, repr(row[0]))
    finally:
        got = _rename_char(ctx, cno, orig[0])
        if got != orig[0]:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s",
                            (orig[0], cno))
        row = _char_snapshot(ctx, cno)
        ctx.check("名字改回原值", row[0], orig[0])
        ctx.check("还原后字节与原值一致", row[1], orig[1])
        ctx.check("还原后 CHAR_LENGTH/LENGTH 一致", (row[2], row[3]), (orig[2], orig[3]))
    ctx.ok("进程仍然活着（整轮没有崩溃）", ctx.proc.poll() is None,
           "poll=%s" % (ctx.proc.poll(),))


# ---------------------------------------------------------------- 字符集：克隆
@core.case("db.charset_clone", needs=("gui", "db"), group="always",
           desc="克隆角色经服务端复制中文名、槽位与各表行数可还原")
def charset_clone(ctx):
    base = ctx.char_no()
    if not ctx.ok("库里有角色", base is not None):
        return
    orig = _char_snapshot(ctx, base)
    if not ctx.ok("读到源角色快照", orig is not None, repr(orig)):
        return
    mid = ctx.q1("SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=%s", (base,))
    view_before = _view_snapshot(ctx, mid)
    if not ctx.ok("charac_view 有该账号的行", view_before is not None):
        return
    tabs = _charac_tables(ctx)
    counts_before = _counts(ctx, tabs)
    max_before = ctx.q1("SELECT IFNULL(MAX(charac_no),0) FROM taiwan_cain.charac_info") or 0
    target = CN_NAME if orig[0] != CN_NAME else CN_NAME + "改"
    clone_no = None

    try:
        if not ctx.ok("角色可选中", _select_char(ctx)):
            return
        ctx.page(PAGE_GM)
        if not ctx.check("源角色先改成中文名「%s」" % target, _rename_char(ctx, base, target), target):
            return
        clone_btn = core.ctl(ctx.main, G_CLONE)
        if not ctx.ok("「克隆角色」按钮存在", bool(clone_btn), "控件 %d" % G_CLONE):
            return
        core.press(clone_btn, 0.8)
        b, t = _wait_box(ctx.main, 8.0)
        ctx.ok("弹出克隆确认框", b is not None, (t or "")[:60])
        if b:
            _close_box(ctx.main, b, 1)

        clone = None
        t0 = time.time()
        while time.time() - t0 < 120:
            r = ctx.q("SELECT charac_no,charac_name,HEX(charac_name),"
                      "CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                      "FROM taiwan_cain.charac_info WHERE charac_no>%s "
                      "ORDER BY charac_no DESC LIMIT 1", (max_before,))
            if r:
                clone = r[0]
                clone_no = clone[0]
                break
            if _boxes(ctx.main):
                for h in _boxes(ctx.main):
                    _close_box(ctx.main, h, 1)
            time.sleep(0.5)
        if not ctx.ok("克隆出了新角色", clone_no is not None,
                      "charac_no=%s" % (clone_no,)):
            return

        ctx.check("克隆角色名是中文（原名+数字后缀）", clone[1], target + "2")
        ctx.check("克隆名 CHAR_LENGTH=%d" % (len(target) + 1), clone[3], len(target) + 1)
        ctx.check("克隆名 LENGTH=%d（汉字 3 字节 + 后缀 1 字节）" % (3 * len(target) + 1),
                  clone[4], 3 * len(target) + 1)
        ctx.ok("克隆名无双重编码", clone[1] == target + "2", "hex=%s" % clone[2][:80])
        ctx.check("克隆角色在 charac_info 里恰好 1 行",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                         (clone_no,)), 1)
        # 视图计数以「实际在场角色数」为准：快照里的值可能本来就是过期的
        want_cnt = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info "
                          "WHERE m_id=%s AND delete_flag=0", (mid,))
        ctx.check("charac_count 已同步为 %d" % want_cnt,
                  ctx.q1("SELECT charac_count FROM taiwan_cain.charac_view WHERE m_id=%s", (mid,)),
                  want_cnt)
    finally:
        if clone_no is not None:
            _drop_char(ctx, clone_no, tabs)
        _view_restore(ctx, mid, view_before)
        got = _rename_char(ctx, base, orig[0])
        if got != orig[0]:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s",
                            (orig[0], base))

        after = _counts(ctx, tabs)
        diff = {t: (counts_before[t], after[t])
                for t in tabs if counts_before[t] != after[t]}
        ctx.ok("删克隆后 %d 张表行数与改前一致" % len(tabs), not diff, str(diff)[:300])
        ctx.check("charac_view 槽位字节还原一致", _view_snapshot(ctx, mid), view_before)
        row = _char_snapshot(ctx, base)
        ctx.check("源角色名还原后字节与原值一致",
                  (row[1], row[2], row[3]), (orig[1], orig[2], orig[3]))
    ctx.ok("进程仍然活着（整轮没有崩溃）", ctx.proc.poll() is None,
           "poll=%s" % (ctx.proc.poll(),))


# ---------------------------------------------------------------- 字符集：邮件 / 宠物
@core.case("db.charset_mail_pet", needs=("gui", "db"), group="always",
           desc="中文邮件正文与宠物名按字符数落库，邮件删净、宠物名还原")
def charset_mail_pet(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    max_letter = ctx.q1("SELECT IFNULL(MAX(letter_id),0) FROM taiwan_cain_2nd.letter") or 0
    mail_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.letter")
    sent_id = None
    pet = None

    try:
        if not ctx.ok("角色可选中", _select_char(ctx)):
            return

        ctx.page(PAGE_MAIL)
        tedit = core.ctl(ctx.main, M_TEXT)
        aid = core.ctl(ctx.main, M_AID)
        snd = core.ctl(ctx.main, M_SEND)
        ctx.ok("邮件页正文框/附件框存在", bool(tedit) and bool(aid), "%s/%s" % (tedit, aid))
        if not (tedit and aid and snd):
            return
        core.set_text(aid, "")
        core.set_text(tedit, MAIL_TEXT)
        time.sleep(0.3)
        core.press(snd, 0.8)

        row = None
        t0 = time.time()
        while time.time() - t0 < 20:
            r = ctx.q("SELECT letter_id,charac_no,send_charac_name,letter_text,HEX(letter_text),"
                      "CHAR_LENGTH(letter_text),LENGTH(letter_text) "
                      "FROM taiwan_cain_2nd.letter WHERE letter_id>%s "
                      "ORDER BY letter_id DESC LIMIT 1", (max_letter,))
            if r:
                row = r[0]
                break
            if _boxes(ctx.main):
                _dismiss_boxes(ctx)
            time.sleep(0.5)
        ctx.ok("库里多了一封邮件", row is not None,
               "letter 行数 %s → %s" % (mail_before,
                                        ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.letter")))
        if row:
            sent_id = row[0]
            ctx.check("letter_text 读回与发送内容一致", row[3], MAIL_TEXT)
            ctx.check("letter_text CHAR_LENGTH=%d（字符数）" % len(MAIL_TEXT),
                      row[5], len(MAIL_TEXT))
            ctx.ok("letter_text 不是双重编码", "æ" not in (row[3] or ""),
                   "hex=%s" % (row[4] or "")[:80])
            ctx.check("send_charac_name=GM 未乱码", row[2], "GM")
            ctx.cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id=%s", (row[0],))
            sent_id = None
            ctx.check("删掉这封邮件后行数还原",
                      ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.letter"), mail_before)

        # 宠物改名
        ctx.page(PAGE_PET)
        time.sleep(1.0)
        rows = ctx.q("SELECT ui_id,name,HEX(name),CHAR_LENGTH(name),LENGTH(name) "
                     "FROM taiwan_cain_2nd.creature_items WHERE charac_no=%s "
                     "ORDER BY ui_id LIMIT 1", (cno,))
        pet = rows[0] if rows else None
        lv = core.ctl(ctx.main, P_LIST)
        nm = core.ctl(ctx.main, P_NAME)
        rb = core.ctl(ctx.main, P_RENAME)
        ctx.ok("宠物列表/改名框存在", bool(lv) and bool(nm) and bool(rb),
               "%s/%s/%s" % (lv, nm, rb))
        if not pet:
            ctx.skip("当前角色没有宠物行，跳过宠物改名")
        elif lv and nm and rb:
            core.click_at(lv, 40, 12, 0.8)
            core.set_text(nm, PET_NAME)
            time.sleep(0.3)
            core.press(rb, 1.0)
            got = None
            t0 = time.time()
            while time.time() - t0 < 15:
                got = ctx.q1("SELECT name FROM taiwan_cain_2nd.creature_items WHERE ui_id=%s",
                             (pet[0],))
                if got == PET_NAME:
                    break
                time.sleep(0.4)
            ctx.check("宠物名读回 = 「%s」" % PET_NAME, got, PET_NAME)
            r = ctx.q("SELECT name,HEX(name),CHAR_LENGTH(name),LENGTH(name) "
                      "FROM taiwan_cain_2nd.creature_items WHERE ui_id=%s", (pet[0],))[0]
            ctx.check("宠物名 CHAR_LENGTH=4", r[2], 4)
            ctx.check("宠物名 LENGTH=12", r[3], 12)
            ctx.ok("宠物名无双重编码", r[0] == PET_NAME, "hex=%s" % r[1][:80])
    finally:
        if sent_id is not None:
            ctx.cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id=%s", (sent_id,))
        else:
            ctx.cur.execute("DELETE FROM taiwan_cain_2nd.letter WHERE letter_id>%s AND charac_no=%s",
                            (max_letter, cno))
        if pet:
            ctx.cur.execute("UPDATE taiwan_cain_2nd.creature_items SET name=%s WHERE ui_id=%s",
                            (pet[1], pet[0]))
            back = ctx.q("SELECT HEX(name),CHAR_LENGTH(name) FROM taiwan_cain_2nd.creature_items "
                         "WHERE ui_id=%s", (pet[0],))[0]
            ctx.check("宠物名还原后字节与原值一致", (back[0], back[1]), (pet[2], pet[3]))
        ctx.check("邮件行数还原", ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.letter"), mail_before)
    ctx.ok("进程仍然活着（整轮没有崩溃）", ctx.proc.poll() is None,
           "poll=%s" % (ctx.proc.poll(),))


# ---------------------------------------------------------------- 泡点
def _bubble_controls(ctx):
    hs = {k: core.ctl(ctx.main, cid) for k, cid in BUB_IDS.items()}
    return hs if all(hs.values()) else None


def _bubble_apply(ctx, hs, target, tno, coin, dp, dc, item, num, on=True, sec=5):
    if core.checked(hs["on"]):
        core.press(hs["on"], 0.0)
        _set_combo(hs["target"], 0)       # 先让「关闭」这一次能过校验
        core.press(hs["apply"], 0.6)
    _set_combo(hs["target"], target)
    core.set_text(hs["tno"], str(tno))
    core.set_text(hs["sec"], str(sec))
    core.set_text(hs["dp"], str(dp))
    core.set_text(hs["dc"], str(dc))
    core.set_text(hs["coin"], str(coin))
    core.set_text(hs["item"], str(item))
    core.set_text(hs["num"], str(num))
    if on and not core.checked(hs["on"]):
        core.press(hs["on"], 0.2)
    core.press(hs["apply"], 0.8)


def _bubble_stop(ctx, hs):
    """关掉定时器，读到「已停用 / 未启用」才算停住。"""
    _set_combo(hs["target"], 0)       # 目标 ID 为空时 apply 会被校验拦下
    for _ in range(3):
        _dismiss_boxes(ctx)
        if core.checked(hs["on"]):
            core.press(hs["on"], 0.3)
        core.press(hs["apply"], 1.0)
        info = core.txt(hs["info"], 512)
        if "已停用" in info or "未启用" in info:
            return True
    return False


def _bubble_snapshot(ctx, cno, mid):
    acc = str(mid)
    return {
        "dp": ctx.q1("SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s", (acc,)),
        "dc": ctx.q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s",
                     (acc,)),
        "coin": ctx.q1("SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (cno,)),
        "mails": ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                        "WHERE receive_charac_no=%s AND send_charac_name='泡点'", (cno,)),
        "max_pid": ctx.q1("SELECT IFNULL(MAX(postal_id),0) FROM taiwan_cain_2nd.postal"),
    }


def _cash_restore(ctx, table, col, acc, val):
    if val is None:
        ctx.cur.execute("DELETE FROM %s WHERE account=%%s" % table, (acc,))
        return
    ctx.cur.execute("INSERT INTO %s (account,%s) VALUES (%%s,%%s) "
                    "ON DUPLICATE KEY UPDATE %s=VALUES(%s)" % (table, col, col, col),
                    (acc, val))


def _bubble_restore(ctx, snap, cno, mid):
    acc = str(mid)
    _cash_restore(ctx, "taiwan_billing.cash_cera", "cera", acc, snap["dp"])
    _cash_restore(ctx, "taiwan_billing.cash_cera_point", "cera_point", acc, snap["dc"])
    if snap["coin"] is not None:
        ctx.cur.execute("UPDATE taiwan_cain_2nd.inventory SET coin=%s WHERE charac_no=%s",
                        (snap["coin"], cno))
    ctx.cur.execute("DELETE FROM taiwan_cain_2nd.postal "
                    "WHERE postal_id>%s AND receive_charac_no=%s AND send_charac_name='泡点'",
                    (snap["max_pid"], cno))


def _bubble_prepare(ctx):
    """先停掉遗留定时器，再选角色、切泡点页、拿到控件与账号；返回 (cno, mid, hs)。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return None
    ctx.page(PAGE_BUBBLE)
    hs = _bubble_controls(ctx)
    if not ctx.ok("泡点控件齐全", hs is not None,
                  "控件 %d..%d" % (BUB_IDS["on"], BUB_IDS["info"])):
        return None
    _bubble_stop(ctx, hs)          # 快照前先停掉上次遗留的定时器
    if not ctx.ok("角色可选中", _select_char(ctx)):
        return None
    mid = ctx.q1("SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    if not ctx.ok("角色有账号", mid is not None):
        return None
    return (cno, mid, hs)


@core.case("db.bubble_rewards", needs=("gui", "db"), group="manual",
           desc="泡点按间隔给指定账号加 D点/D币、给指定角色加点券与物品邮件")
def bubble_rewards(ctx):
    ini = _ini_snapshot()
    cno = mid = hs = snap = None
    try:
        prep = _bubble_prepare(ctx)
        if not prep:
            return
        cno, mid, hs = prep
        snap = _bubble_snapshot(ctx, cno, mid)

        # 指定账号：账号级 D点与 D币各加 1 / 5 秒
        _bubble_apply(ctx, hs, target=3, tno=mid, coin=0, dp=1, dc=1, item=0, num=0)
        time.sleep(14.0)
        dp1 = ctx.q1("SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s", (str(mid),)) or 0
        dc1 = ctx.q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s",
                     (str(mid),)) or 0
        dp0 = snap["dp"] or 0
        dc0 = snap["dc"] or 0
        ctx.ok("指定账号：D点按间隔增加（14 秒内至少 2 次）", dp1 - dp0 >= 2,
               "D点 %s → %s" % (dp0, dp1))
        ctx.ok("指定账号：D币按间隔增加（14 秒内至少 2 次）", dc1 - dc0 >= 2,
               "D币 %s → %s" % (dc0, dc1))

        # 指定角色：角色级赠送点券 + 物品随邮件发放
        _bubble_apply(ctx, hs, target=2, tno=cno, coin=1, dp=0, dc=0, item=1001, num=1)
        time.sleep(14.0)
        cn1 = ctx.q1("SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (cno,)) or 0
        ml1 = ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
                     "WHERE receive_charac_no=%s AND send_charac_name='泡点'", (cno,)) or 0
        ctx.ok("指定角色：赠送点券按间隔增加（至少 2 次）", cn1 - (snap["coin"] or 0) >= 2,
               "点券 %s → %s" % (snap["coin"] or 0, cn1))
        ctx.ok("指定角色：泡点物品邮件按间隔插入（至少 2 封）", ml1 - (snap["mails"] or 0) >= 2,
               "邮件 %s → %s" % (snap["mails"] or 0, ml1))
    finally:
        if hs and snap is not None:
            ctx.page(PAGE_BUBBLE)
            _bubble_stop(ctx, hs)
            time.sleep(1.5)               # 等在飞的结算收尾，再回写快照
            _bubble_restore(ctx, snap, cno, mid)
        _bubble_ini_restore(ini)


@core.case("db.bubble_stop", needs=("gui", "db"), group="manual",
           desc="泡点停用后不再按间隔加点")
def bubble_stop(ctx):
    ini = _ini_snapshot()
    cno = mid = hs = snap = None
    try:
        prep = _bubble_prepare(ctx)
        if not prep:
            return
        cno, mid, hs = prep
        snap = _bubble_snapshot(ctx, cno, mid)

        _bubble_apply(ctx, hs, target=3, tno=mid, coin=0, dp=0, dc=1, item=0, num=0)
        time.sleep(11.0)
        during = ctx.q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s",
                        (str(mid),)) or 0
        ctx.ok("启用后 D币确实在增加", during > (snap["dc"] or 0),
               "D币 %s → %s" % (snap["dc"] or 0, during))

        stopped = _bubble_stop(ctx, hs)
        ctx.ok("关闭后状态行显示已停用", stopped, repr(core.txt(hs["info"], 512)))
        time.sleep(1.5)                       # 等在飞的结算收尾，再取基准
        pa = ctx.q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s",
                    (str(mid),)) or 0
        time.sleep(9.0)
        pb = ctx.q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s",
                    (str(mid),)) or 0
        ctx.check("关闭后不再加点（9 秒内 D币不变）", pb, pa)
    finally:
        if hs and snap is not None:
            ctx.page(PAGE_BUBBLE)
            _bubble_stop(ctx, hs)
            time.sleep(1.5)               # 等在飞的结算收尾，再回写快照
            _bubble_restore(ctx, snap, cno, mid)
        _bubble_ini_restore(ini)
