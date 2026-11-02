# -*- coding: utf-8 -*-
"""邮件与封停回归用例：列表加载与失败保留、读库期间切页、发送/删除的写路径闸门、
邮件列表的跨角色归属、在线角色的物品写库拦截。

控件编号与表名沿用旧脚本常量；写库用例先快照、finally 还原，用例之间不共享状态。
"""
import ctypes
import ctypes.wintypes as wt
import glob
import hashlib
import os
import re
import time

from . import core
from .core import u

# ---------------------------------------------------------------- 常量
IDC_CHLIST, IDC_STATUS = 113, 191

PAGE_ITEM, PAGE_MAIL, PAGE_PET, PAGE_GM, PAGE_BAN = 0, 3, 4, 8, 10

# 邮件页
M_LIST, M_DEL, M_INFO = 1301, 1302, 1305
M_TARGET, M_AID, M_TEXT, M_SEND, M_REFRESH = 1306, 1308, 1310, 1311, 1320
# 封停页
B_LIST, B_CHAR, B_INFO, B_LOADALL = 2001, 2002, 2009, 2012
# 物品页 / GM 页
I_LIST, I_EDIT, I_CLR = 1002, 1003, 1004
G_NAME = 1801

CHARAC_INFO = "taiwan_cain.charac_info"
CHARAC_VIEW = "taiwan_cain.charac_view"
POSTAL = "taiwan_cain_2nd.postal"
LETTER = "taiwan_cain_2nd.letter"
INVENTORY = "taiwan_cain_2nd.inventory"
LOGIN = "taiwan_login.login_account_3"
PLAY_EVENT = "taiwan_game_event.event_1306_account_reward"

MAIL_SELECT = "SELECT postal_id"          # db_list_mails 的语句前缀
MAIL_FAIL = "读取邮件 失败"
MAIL_DONE = "读取邮件 完成"
MAIL_SENT = "发送文本邮件 完成"
BAN_FAIL = "读取角色列表 失败"

LVM_GETHEADER = 0x101F
WM_CLOSE = 0x0010

# 左栏行坐标候选：行高随 DPI 变，点完按状态栏认出到底选中了谁
CHAR_Y = (12, 33, 43, 55, 76, 97, 118)

TMP_NO = 9918
TMP_NAME = "切号验证号"
SEND_TEXT = "回归：纯文本邮件"


# ---------------------------------------------------------------- 通用工具
def _rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def _status(ctx):
    return core.txt(u.GetDlgItem(ctx.main, IDC_STATUS), 512)


def _sha(rows):
    """整表/整行指纹：按 repr 排序后哈希，用来做逐字节还原比对。"""
    return hashlib.sha256(repr(sorted(map(repr, rows))).encode("utf-8", "replace")).hexdigest()[:16]


def _poll_sec():
    """左栏轮询间隔：取程序自己写的 settings.ini，读不到按默认 3 秒。"""
    import configparser

    ini = os.path.join(core.BUILD, "config", "settings.ini")
    cp = configparser.RawConfigParser()
    try:
        cp.read(ini, encoding="utf-8")
        return max(1, int(cp.get("ui", "poll_sec", fallback=3)))
    except Exception:
        return 3


def _log_file():
    d = os.path.join(core.BUILD, "config", "logs")
    fs = sorted(glob.glob(os.path.join(d, "log-*.txt")))
    return fs[-1] if fs else None


def _log_mark():
    p = _log_file()
    if p:
        return p, os.path.getsize(p)
    d = os.path.join(core.BUILD, "config", "logs")
    return os.path.join(d, time.strftime("log-%Y%m%d.txt")), 0


def _log_since(mark):
    p, off = mark
    if not p or not os.path.exists(p):
        return ""
    with open(p, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def _rows(ctx, cid):
    h = ctx.ctl(cid)
    return core.rows(h) if h else -1


def _combo_n(ctx, cid=B_CHAR):
    h = ctx.ctl(cid)
    return int(u.SendMessageW(h, core.CB_GETCOUNT, 0, 0)) if h else -1


def _top(cls, pid=None):
    """顶层可见窗口（可按类名、按进程过滤）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h) or core.cls_of(h) != cls:
            return True
        wpid = wt.DWORD(0)
        u.GetWindowThreadProcessId(h, ctypes.byref(wpid))
        if pid is not None and wpid.value != pid:
            return True
        out.append(h)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _dlg_text(h):
    parts = []
    for c in core.kids(h):
        if core.cls_of(c) in ("Static", "Button", "Edit"):
            t = core.txt(c, 2048)
            if t:
                parts.append(t)
    return " / ".join(parts)


def _wait_box(ctx, timeout=8.0):
    """等本程序弹的提示/确认框，返回 (hwnd, 全部控件文字)。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        hs = _top("#32770", ctx.proc.pid)
        if hs:
            time.sleep(0.2)
            return hs[0], _dlg_text(hs[0])
        time.sleep(0.05)
    return None, ""


def _close_box(h):
    u.PostMessageW(h, WM_CLOSE, 0, 0)
    time.sleep(0.8)


def _dismiss_boxes(ctx):
    """关掉残留提示框，免得模态挡住后面的点击。"""
    for h in _top("#32770", ctx.proc.pid):
        b = u.GetDlgItem(h, 1)
        if b:
            core.press(b, 0.0)
        else:
            u.PostMessageW(h, WM_CLOSE, 0, 0)
        time.sleep(0.3)


def _row_xy(lv, idx=0):
    """列表第 idx 行的客户区坐标：从列头底边往下算。"""
    r = _rect(lv)
    y = 12 + idx * 20
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    if hd:
        y = (_rect(hd).bottom - r.top) + 8 + idx * 20
    return 40, max(2, y)


def _loaded_name(ctx):
    """状态栏里「已载入 X」的 X；没有这句就返回 None。"""
    s = _status(ctx)
    if "已载入 " not in s:
        return None
    return s.split("已载入 ", 1)[1].split(" ·", 1)[0]


def _status_has(ctx, needle, timeout=10.0):
    return core.wait_until(lambda: needle in _status(ctx), timeout)


def _click_char(ctx, y):
    core.click_at(u.GetDlgItem(ctx.main, IDC_CHLIST), 40, y, 0.0)


def _select_first(ctx, timeout=20.0):
    """选中左栏第一行（列表按 charac_no 降序，就是编号最大的角色）。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not core.wait_until(lambda: core.rows(lv) > 0, timeout):
        return False
    for y in CHAR_Y:
        _click_char(ctx, y)
        if _status_has(ctx, "已载入", 3.0):
            return True
    return False


def _probe_chars(ctx, names, timeout=8.0):
    """逐候选 y 点左栏，按稳定的状态栏读数记下每个名字命中的 y；返回 {名字: y}。

    点一行会触发一次载入；连接忙时这一下会被排成「待载入」，所以不能只认第一个
    「已载入」——连续 0.25 秒不变才算这一下真正落定的人。
    """
    got = {}
    for y in CHAR_Y:
        _click_char(ctx, y)
        last, stable = None, 0
        t0 = time.time()
        while time.time() - t0 < timeout:
            nm = _loaded_name(ctx)
            if nm is not None and nm == last:
                stable += 1
                if stable >= 5:
                    break
            else:
                last, stable = nm, 0
            time.sleep(0.05)
        if last in names and last not in got:     # 点到空行时状态栏还是上一个人，别覆盖
            got[last] = y
        if len(got) >= len(names):
            break
    return got


def _select_char(ctx, y, name, timeout=10.0):
    _click_char(ctx, y)
    return core.wait_until(lambda: ("已载入 " + name) in _status(ctx), timeout)


def _wait_blocked(ctx, marker, timeout=15.0):
    """PROCESSLIST 里出现该语句的 metadata lock 等待：证明它真的在等，不是已跑完。"""
    state, info = "", ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _id, _cmd, st, inf in ctx.q(
                "SELECT ID,COMMAND,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                "WHERE COMMAND='Query'"):
            state, info = st or "", inf or ""
            if marker in info and "metadata lock" in state:
                return True, state, info
        time.sleep(0.2)
    return False, state, info


def _kill_until(lock, own, fail_text, mark, deadline=30.0, marker=None):
    """把被测连接上在等的语句打掉，直到日志里出现 fail_text；返回 (是否出现, 打掉条数)。

    卡在 metadata lock 上只能证明「慢」；KILL QUERY 才能造出「查失败」。
    """
    killed = 0
    t0 = time.time()
    while time.time() - t0 < deadline:
        if fail_text in _log_since(mark):
            return True, killed
        try:
            pl = lock.q("SHOW PROCESSLIST")
        except Exception:
            pl = []
        for r in pl:
            if r[0] in own or r[4] != "Query":
                continue
            if marker and not str(r[7] or "").startswith(marker):
                continue
            try:
                lock.exec("KILL QUERY %d" % r[0])
                killed += 1
            except Exception:
                pass
        time.sleep(0.4)
    return fail_text in _log_since(mark), killed


class _LockConn:
    """第二条连接：只用来 LOCK TABLES、查 PROCESSLIST、KILL QUERY。"""

    def __init__(self):
        self.conn = None
        self.cur = None

    def open(self):
        self.conn = core.db()
        self.cur = self.conn.cursor()
        return self

    def exec(self, sql):
        self.cur.execute(sql)
        return self.cur

    def q(self, sql):
        self.cur.execute(sql)
        return self.cur.fetchall()

    def lock(self, table):
        self.exec("LOCK TABLES %s WRITE" % table)

    def ids(self):
        return {r[0] for r in self.q("SELECT CONNECTION_ID()")}

    def close(self):
        if self.conn is None:
            return
        try:
            self.cur.execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            self.conn.close()
        except Exception:
            pass
        self.conn = self.cur = None


def _own_ids(ctx, lock):
    return lock.ids() | {ctx.q1("SELECT CONNECTION_ID()")}


# ---------------------------------------------------------------- 邮件列表
def _mail_state(ctx):
    h = ctx.ctl(M_LIST)
    return (core.rows(h) if h else -1, core.txt(ctx.ctl(M_INFO), 512))


def _wait_mail(ctx, want, name=None, timeout=25.0):
    t0 = time.time()
    st = (-2, "")
    while time.time() - t0 < timeout:
        st = _mail_state(ctx)
        if (st[0] == want and ("共 %d 封邮件" % want) in st[1]
                and (name is None or name in st[1])):
            return st
        time.sleep(0.2)
    return st


def _postal_n(ctx, cno):
    return ctx.q1("SELECT COUNT(*) FROM " + POSTAL +
                  " WHERE receive_charac_no=%s AND delete_flag=0", (cno,))


@core.case("mail.list_load", needs=("gui", "db"), group="always",
           desc="进邮件页后列表行数、非空与提示文案都与库中该角色的邮件一致")
def list_load(ctx):
    """验：正常进入邮件页，列表取自库里该角色的未删邮件。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    n = _postal_n(ctx, cno)
    if not n:
        ctx.skip("该角色没有邮件，行数判据无从判断")
        return
    if not ctx.ok("角色可选中", _select_first(ctx)):
        return
    ctx.page(PAGE_MAIL)
    rows, info = _wait_mail(ctx, n)
    ctx.check("列表行数 == 库里邮件数", rows, n)
    ctx.ok("列表非空", rows > 0, "rows=%s" % rows)
    ctx.ok("提示是「共 %d 封邮件」" % n, ("共 %d 封邮件" % n) in info, info)


@core.case("mail.list_fail_keeps", needs=("gui", "db"), group="always",
           desc="邮件查询失败时列表不塌成空、提示停在读取中，放锁后自行恢复")
def list_fail_keeps(ctx):
    """验：查询失败只作废这次拉取，不把旧列表当空数据，也不显示「共 0 封邮件」。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    n = _postal_n(ctx, cno)
    if not n:
        ctx.skip("该角色没有邮件，行数判据无从判断")
        return
    lock = _LockConn()
    try:
        if not ctx.ok("角色可选中", _select_first(ctx)):
            return
        ctx.page(PAGE_MAIL)
        rows, _info = _wait_mail(ctx, n)
        ctx.check("起点：列表行数 == 库行数", rows, n)
        if not ctx.ok("刷新按钮存在", bool(ctx.ctl(M_REFRESH)), "控件 %d" % M_REFRESH):
            return

        lock.open()
        own = _own_ids(ctx, lock)
        mark = _log_mark()
        lock.lock(POSTAL)
        core.press(ctx.ctl(M_REFRESH), 0.0)
        blocked, state, _inf = _wait_blocked(ctx, MAIL_SELECT, 15.0)
        ctx.ok("邮件查询真的卡在库上（%s）" % state, blocked, state)
        seen, killed = _kill_until(lock, own, MAIL_FAIL, mark, deadline=20.0,
                                   marker=MAIL_SELECT)
        ctx.ok("应用侧报了「%s」" % MAIL_FAIL, seen, "打掉 %d 条" % killed)

        time.sleep(1.5)                       # 让重试再转两圈
        rows, info = _mail_state(ctx)
        ctx.ok("失败后列表没塌成 0 行", rows == n, "rows=%s" % rows)
        ctx.ok("提示停在「正在读取邮件…」", "正在读取邮件" in info, info)
        ctx.ok("提示没有变成「共 0 封邮件」", "共 0 封邮件" not in info, info)

        lock.close()                          # 放锁，靠下次重试把列表拉回来
        rows, info = _wait_mail(ctx, n)
        ctx.check("放锁后行数回到库行数", rows, n)
        ctx.ok("放锁后提示恢复成「共 %d 封邮件」" % n,
               ("共 %d 封邮件" % n) in info, info)
        ctx.ok("全程进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.close()


@core.case("mail.list_switch", needs=("gui", "db"), group="always",
           desc="邮件查询卡在库上时来回切页 10 轮：进程不崩、列表不掉到 0 行")
def list_switch(ctx):
    """验：读作业还没回来时反复切页，界面不会拿到半截或被释放的列表。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    n = _postal_n(ctx, cno)
    if not n:
        ctx.skip("该角色没有邮件，行数判据无从判断")
        return
    lock = _LockConn()
    try:
        if not ctx.ok("角色可选中", _select_first(ctx)):
            return
        ctx.page(PAGE_MAIL)
        rows, _info = _wait_mail(ctx, n)
        ctx.check("起点：列表行数 == 库行数", rows, n)
        if not ctx.ok("刷新按钮存在", bool(ctx.ctl(M_REFRESH)), "控件 %d" % M_REFRESH):
            return

        lock.open()
        own = _own_ids(ctx, lock)
        mark = _log_mark()
        lock.lock(POSTAL)
        core.press(ctx.ctl(M_REFRESH), 0.0)
        blocked, state, _inf = _wait_blocked(ctx, MAIL_SELECT, 15.0)
        ctx.ok("邮件查询真的卡在库上（%s）" % state, blocked, state)
        _kill_until(lock, own, MAIL_FAIL, mark, deadline=20.0, marker=MAIL_SELECT)

        seen = []
        for _r in range(10):
            ctx.page(PAGE_MAIL)
            seen.append(_mail_state(ctx)[0])
            ctx.page(PAGE_PET)
            time.sleep(0.05)
        ctx.page(PAGE_MAIL)
        time.sleep(0.5)
        seen.append(_mail_state(ctx)[0])
        ctx.ok("读取期间反复切页进程没崩", ctx.proc.poll() is None,
               "poll=%s" % (ctx.proc.poll(),))
        ctx.ok("切页期间列表没掉到 0 行", min(seen) == n,
               "min=%s max=%s 样本 %d" % (min(seen), max(seen), len(seen)))
    finally:
        lock.close()


# ---------------------------------------------------------------- 发送与删除
def _letter_state(ctx):
    rows = ctx.q("SELECT * FROM " + LETTER + " ORDER BY letter_id")
    return len(rows), _sha(rows)


@core.case("mail.send_text", needs=("gui", "db"), group="always",
           desc="发纯文本邮件：letter 落一行、发送后重拉列表、删除后整表还原")
def send_text(ctx):
    """验：发送真的写进 letter，且写完把列表置回未加载；删掉新行后整表指纹还原。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    n_mail = _postal_n(ctx, cno)
    before_n, before_sha = _letter_state(ctx)
    max_id = ctx.q1("SELECT IFNULL(MAX(letter_id),0) FROM " + LETTER) or 0
    try:
        if not ctx.ok("角色可选中", _select_first(ctx)):
            return
        ctx.page(PAGE_MAIL)
        rows, info = _wait_mail(ctx, n_mail)
        ctx.check("起点：邮件列表行数 == 库行数", rows, n_mail)
        ctx.ok("起点：列表已加载（提示「共 %d 封邮件」）" % n_mail,
               ("共 %d 封邮件" % n_mail) in info, info)

        target = ctx.ctl(M_TARGET)
        aid = ctx.ctl(M_AID)
        tedit = ctx.ctl(M_TEXT)
        snd = ctx.ctl(M_SEND)
        if not ctx.ok("邮件页控件齐全", all([target, aid, tedit, snd])):
            return
        u.SendMessageW(target, core.CB_SETCURSEL, 0, 0)      # 目标 = 当前角色
        core.set_text(aid, "")                                # 留空 = 纯文本
        core.set_text(tedit, SEND_TEXT)
        time.sleep(1.3)                       # 先让本次发送之前的日志落盘，再取位置
        mark = _log_mark()
        core.press(snd, 0.0)

        got = None
        t0 = time.time()
        while time.time() - t0 < 20:
            rows = ctx.q("SELECT send_charac_name,letter_text FROM " + LETTER +
                         " WHERE letter_id>%s ORDER BY letter_id DESC LIMIT 1", (max_id,))
            if rows:
                got = rows[0]
                break
            _dismiss_boxes(ctx)
            time.sleep(0.3)
        ctx.check("发送后 letter 多出一行",
                  ctx.q1("SELECT COUNT(*) FROM " + LETTER), before_n + 1)
        ctx.check("落库内容对得上", got, ("GM", SEND_TEXT))

        # 日志的先后顺序就是「列表被置回未加载、随后又拉了一次」的落地证据
        core.wait_until(lambda: (MAIL_SENT in _log_since(mark)
                                 and MAIL_DONE in _log_since(mark)), 15.0)
        lines = _log_since(mark).split("\n")
        i_send = max([i for i, l in enumerate(lines) if MAIL_SENT in l] or [-1])
        i_read = max([i for i, l in enumerate(lines) if MAIL_DONE in l] or [-1])
        ctx.ok("发送之后列表被置回未加载（随后又拉了一次）",
               i_send >= 0 and i_read > i_send, "i_send=%s i_read=%s" % (i_send, i_read))
    finally:
        ctx.cur.execute("DELETE FROM " + LETTER + " WHERE letter_id>%s", (max_id,))
        n, sha = _letter_state(ctx)
        ctx.check("还原后 letter 行数回到改前", n, before_n)
        ctx.check("还原后 letter 整表指纹与改前一致", sha, before_sha)


@core.case("mail.del_queue_full", needs=("gui", "db"), group="always",
           desc="动作队列满时删除邮件不提交、列表不被清空、目标行 delete_flag 不变")
def del_queue_full(ctx):
    """验：app_act 返回 0（队列满）时删除邮件这条写路径不许白清列表。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    n = _postal_n(ctx, cno)
    if not n:
        ctx.skip("该角色没有邮件，删除判据无从判断")
        return
    target = ctx.q1("SELECT postal_id FROM " + POSTAL +
                    " WHERE receive_charac_no=%s AND delete_flag=0 "
                    "ORDER BY postal_id DESC LIMIT 1", (cno,))
    dflag0 = ctx.q1("SELECT delete_flag FROM " + POSTAL + " WHERE postal_id=%s", (target,))
    coin0 = ctx.q("SELECT money,coin FROM " + INVENTORY + " WHERE charac_no=%s", (cno,))
    lock = _LockConn()
    try:
        if not ctx.ok("角色可选中", _select_first(ctx)):
            return
        ctx.page(PAGE_MAIL)
        rows, info = _wait_mail(ctx, n)
        ctx.check("起点：列表行数 == 库行数", rows, n)
        ctx.ok("起点：列表已加载（提示「共 %d 封邮件」）" % n,
               ("共 %d 封邮件" % n) in info, info)
        if not ctx.ok("邮件页删除按钮存在", bool(ctx.ctl(M_DEL)), "控件 %d" % M_DEL):
            return

        # 先把连接占死：背景查询卡在登录表上，后面的读动作只能排队
        lock.open()
        own = _own_ids(ctx, lock)
        lock.lock(LOGIN)
        blocked, state, _inf = _wait_blocked(ctx, "login_account_3", 20.0)
        ctx.ok("连接被背景查询占住（%s）" % state, blocked, state)

        ctx.page(PAGE_BAN)
        b = ctx.ctl(B_LOADALL)
        if not ctx.ok("「加载全服」按钮存在", bool(b), "控件 %d" % B_LOADALL):
            return
        for _i in range(8):                   # 排满 4 槽；多出来的会被拒
            core.press(b, 0.12)
        _dismiss_boxes(ctx)
        ctx.ok("动作队列已排满（状态栏报排队满）",
               "个操作在排队" in _status(ctx), _status(ctx))
        busy = [r[0] for r in lock.q("SHOW PROCESSLIST")
                if r[0] not in own and r[4] == "Query"]
        ctx.ok("连接确实还被占着", bool(busy), str(busy[:4]))

        ctx.page(PAGE_MAIL)
        _dismiss_boxes(ctx)
        lv = ctx.ctl(M_LIST)
        if not ctx.ok("邮件列表存在", bool(lv), "控件 %d" % M_LIST):
            return
        core.click_at(lv, *_row_xy(lv, 0), wait=0.4)
        core.press(ctx.ctl(M_DEL), 0.0)
        box, text = _wait_box(ctx, 8.0)
        ctx.ok("弹出了删除确认框（选中行是真的）",
               box is not None and "删除邮件" in text, text[:80])
        hit = re.search(r"删除邮件\s*(\d+)", text or "")
        ctx.check("确认框指向列表第 0 行（postal_id）",
                  int(hit.group(1)) if hit else None, target)
        if box:
            btn = u.GetDlgItem(box, 1)
            if btn:
                core.press(btn, 0.0)

        time.sleep(0.5)
        ctx.ok("确认框已关闭（确定这一下真的被处理了）",
               core.wait_until(lambda: not _top("#32770", ctx.proc.pid), 5.0),
               str(_top("#32770", ctx.proc.pid)))
        ctx.page(PAGE_GM)
        ctx.page(PAGE_MAIL)
        time.sleep(0.8)
        _dismiss_boxes(ctx)
        rows, info = _mail_state(ctx)
        ctx.ok("队列满时不白清列表（仍停在「共 %d 封邮件」）" % n,
               ("共 %d 封邮件" % n) in info, info)
        ctx.check("列表行数也没变", rows, n)

        lock.close()
        time.sleep(4.0)                       # 让排队的读动作真正执行完
        ctx.check("这次删除没有被提交（队列确实是满的）",
                  ctx.q1("SELECT delete_flag FROM " + POSTAL + " WHERE postal_id=%s",
                         (target,)), 0)
        ctx.ok("全程进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.close()
        try:
            ctx.cur.execute("UPDATE " + POSTAL + " SET delete_flag=0 WHERE postal_id=%s",
                            (target,))
        except Exception:
            pass
        if coin0:
            ctx.cur.execute("UPDATE " + INVENTORY +
                            " SET money=%s,coin=%s WHERE charac_no=%s",
                            (coin0[0][0], coin0[0][1], cno))
        ctx.check("还原后目标邮件 delete_flag 回到改前",
                  ctx.q1("SELECT delete_flag FROM " + POSTAL + " WHERE postal_id=%s",
                         (target,)), dflag0)
        ctx.check("还原后 inventory 的 money/coin 与改前一致",
                  ctx.q("SELECT money,coin FROM " + INVENTORY + " WHERE charac_no=%s",
                        (cno,)), coin0)


# ---------------------------------------------------------------- 跨角色归属
def _cv_md5(ctx, mid):
    if mid is None:
        return None
    r = ctx.q("SELECT `info` FROM " + CHARAC_VIEW + " WHERE `m_id`=%s", (mid,))
    return hashlib.md5(r[0][0] if r and r[0][0] else b"").hexdigest()[:12]


def _swap_snapshot(ctx, cno, mid):
    return {
        "info_n": ctx.q1("SELECT COUNT(*) FROM " + CHARAC_INFO),
        "inv_n": ctx.q1("SELECT COUNT(*) FROM " + INVENTORY),
        "ai": ctx.q1("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                     "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'"),
        "tmp_n": ctx.q1("SELECT COUNT(*) FROM " + CHARAC_INFO + " WHERE charac_no=%s",
                        (TMP_NO,)),
        "name_n": ctx.q1("SELECT COUNT(*) FROM " + CHARAC_INFO + " WHERE charac_name=%s",
                         (TMP_NAME,)),
        "postal_n": ctx.q1("SELECT COUNT(*) FROM " + POSTAL +
                           " WHERE receive_charac_no=%s AND delete_flag=0", (cno,)),
        "tmp_postal": ctx.q1("SELECT COUNT(*) FROM " + POSTAL +
                             " WHERE receive_charac_no=%s", (TMP_NO,)),
        "cv": _cv_md5(ctx, mid),
        "src": (ctx.q("SELECT charac_name,HEX(charac_name) FROM " + CHARAC_INFO +
                      " WHERE charac_no=%s", (cno,)) or [None])[0],
    }


def _cols(ctx, dbn, tb):
    return [r[0] for r in ctx.q(
        "SELECT COLUMN_NAME FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s ORDER BY ORDINAL_POSITION", (dbn, tb))]


def _copy_row(ctx, dbn, tb, key, src, new, name=None):
    """按列名整行复制（主键换成 new，可选换角色名）。"""
    cs = _cols(ctx, dbn, tb)
    sel = []
    for c in cs:
        if c == key:
            sel.append(str(new))
        elif name is not None and c == "charac_name":
            sel.append("'%s'" % name)
        else:
            sel.append("`%s`" % c)
    ctx.cur.execute("INSERT INTO `%s`.`%s` (%s) SELECT %s FROM `%s`.`%s` WHERE `%s`=%%s"
                    % (dbn, tb, ",".join("`%s`" % c for c in cs), ",".join(sel),
                       dbn, tb, key), (src,))


def _swap_begin(ctx):
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return None
    mid = ctx.q1("SELECT m_id FROM " + CHARAC_INFO + " WHERE charac_no=%s", (cno,))
    if not ctx.ok("源角色有账号", mid is not None):
        return None
    before = _swap_snapshot(ctx, cno, mid)
    if not ctx.ok("读到源角色快照", before["src"] is not None, repr(before["src"])):
        return None
    return cno, mid, before


def _swap_prepare(ctx, cno, before):
    """建一个没有邮件的临时角色，并校准左栏两个角色的行坐标。"""
    src_name = before["src"][0]
    # 两条是本用例的前提，不是产品行为：不满足就跳过，不判失败
    if not before["postal_n"]:
        ctx.skip("前置不满足：源角色没有邮件")
        return None
    if not (before["ai"] and before["ai"] > TMP_NO):
        ctx.skip("前置不满足：自增计数器 %s 未超过临时号 %d" % (before["ai"], TMP_NO))
        return None
    ctx.ok("临时号 %d 与临时名字未被占用" % TMP_NO,
           not (before["tmp_n"] or before["name_n"]),
           "tmp_n=%s name_n=%s" % (before["tmp_n"], before["name_n"]))
    ctx.ok("临时号名下没有历史邮件", not before["tmp_postal"],
           "%s 封" % before["tmp_postal"])
    if before["tmp_n"] or before["name_n"] or before["tmp_postal"]:
        return None

    _copy_row(ctx, "taiwan_cain", "charac_info", "charac_no", cno, TMP_NO, name=TMP_NAME)
    _copy_row(ctx, "taiwan_cain_2nd", "inventory", "charac_no", cno, TMP_NO)
    if not ctx.ok("临时角色已建（charac_info 与 inventory 各一行）",
                  ctx.q1("SELECT charac_name FROM " + CHARAC_INFO +
                         " WHERE charac_no=%s", (TMP_NO,)) == TMP_NAME
                  and ctx.q1("SELECT COUNT(*) FROM " + INVENTORY +
                             " WHERE charac_no=%s", (TMP_NO,)) == 1):
        return None

    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not ctx.ok("左栏角色列表出现临时角色",
                  core.wait_until(lambda: core.rows(lv) == before["info_n"] + 1, 15.0),
                  "行数 %s" % core.rows(lv)):
        return None
    got = _probe_chars(ctx, (src_name, TMP_NAME))
    if not ctx.ok("能按名字选中两个角色（行坐标已校准）", len(got) == 2, str(got)):
        return None
    if not ctx.ok("源角色可载入", _select_char(ctx, got[src_name], src_name)):
        return None
    return {"src_name": src_name, "tmp_y": got[TMP_NAME], "src_y": got[src_name]}


def _swap_end(ctx, cno, before, mid):
    """删掉临时角色，逐项断言库回到改前。"""
    try:
        if not before["tmp_n"]:
            ctx.cur.execute("DELETE FROM " + CHARAC_INFO + " WHERE charac_no=%s", (TMP_NO,))
            ctx.cur.execute("DELETE FROM " + INVENTORY + " WHERE charac_no=%s", (TMP_NO,))
    except Exception:
        pass
    after = _swap_snapshot(ctx, cno, mid)
    ctx.check("清理后 charac_info 行数回到改前", after["info_n"], before["info_n"])
    ctx.check("清理后 inventory 行数回到改前", after["inv_n"], before["inv_n"])
    ctx.check("清理后 AUTO_INCREMENT 未变", after["ai"], before["ai"])
    ctx.check("清理后源角色名字节与改前一致", after["src"], before["src"])
    ctx.check("清理后 charac_view.info 字节与改前一致（md5 %s）" % before["cv"],
              after["cv"], before["cv"])
    ctx.check("清理后源角色的邮件行数与改前一致", after["postal_n"], before["postal_n"])


def _swap_block_query(ctx, lock, cno):
    """锁住 postal、刷新列表，断言在飞的那条 SELECT 卡的正是源角色。"""
    lock.open()
    lock.lock(POSTAL)
    core.press(ctx.ctl(M_REFRESH), 0.0)
    blocked, state, inf = _wait_blocked(ctx, MAIL_SELECT, 15.0)
    ctx.ok("邮件查询真的卡在库上（%s）" % state, blocked, state)
    ctx.ok("这条在飞的 SELECT 查的正是源角色",
           ("receive_charac_no=%d" % cno) in inf, str(inf)[:140])


@core.case("mail.swap_inflight", needs=("gui", "db"), group="always",
           desc="刷新邮件时查询被拖在库上，切角色的请求在查询返回之前就发出")
def swap_inflight(ctx):
    """验：切角色确实发生在在飞查询返回之前，而不是靠 sleep 赌出来的时序。"""
    got = _swap_begin(ctx)
    if not got:
        return
    cno, mid, before = got
    lock = _LockConn()
    try:
        prep = _swap_prepare(ctx, cno, before)
        if not prep:
            return
        ctx.page(PAGE_MAIL)
        rows, info = _wait_mail(ctx, before["postal_n"], prep["src_name"])
        ctx.check("进邮件页行数 == 库行数", rows, before["postal_n"])
        ctx.ok("提示挂在源角色名下（「%s · 共 %d 封邮件」）"
               % (prep["src_name"], before["postal_n"]),
               prep["src_name"] in info
               and ("共 %d 封邮件" % before["postal_n"]) in info, info)

        _swap_block_query(ctx, lock, cno)
        _click_char(ctx, prep["tmp_y"])
        ctx.ok("查询还在飞时就发起了切角色（状态栏指向临时角色）",
               _status_has(ctx, TMP_NAME, 10.0), _status(ctx))
        still, st2, _inf = _wait_blocked(ctx, MAIL_SELECT, 2.0)
        ctx.ok("那一刻查源角色的 SELECT 还没回来（仍卡在 %s）" % st2, still, st2)
        ctx.ok("此刻当前角色还没换成临时角色（载入排在查询后面）",
               ("已载入 " + TMP_NAME) not in _status(ctx), _status(ctx))
        ctx.ok("进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.close()
        time.sleep(3.0)                       # 先让放锁后的载入收尾，再删临时角色
        ctx.ok("放锁后进程没崩", ctx.proc.poll() is None,
               "poll=%s" % (ctx.proc.poll(),))
        _swap_end(ctx, cno, before, mid)


@core.case("mail.swap_guard", needs=("gui", "db"), group="always",
           desc="查询飞行中切角色：回来的旧角色邮件被作废，并为新角色重拉一份")
def swap_guard(ctx):
    """验：旧角色那份邮件不许挂到新角色名下，作废后新角色仍能拉到自己的空列表。"""
    got = _swap_begin(ctx)
    if not got:
        return
    cno, mid, before = got
    n = before["postal_n"]
    lock = _LockConn()
    pos0 = None
    try:
        prep = _swap_prepare(ctx, cno, before)
        if not prep:
            return
        ctx.page(PAGE_MAIL)
        rows, info = _wait_mail(ctx, n, prep["src_name"])
        ctx.check("进邮件页行数 == 库行数", rows, n)
        ctx.ok("提示挂在源角色名下（「%s · 共 %d 封邮件」）" % (prep["src_name"], n),
               prep["src_name"] in info and ("共 %d 封邮件" % n) in info, info)

        time.sleep(1.3)                       # 先让本次刷新之前的日志落盘，再取位置
        pos0 = _log_mark()
        _swap_block_query(ctx, lock, cno)
        _click_char(ctx, prep["tmp_y"])
        ctx.ok("查询还在飞时就发起了切角色（状态栏指向临时角色）",
               _status_has(ctx, TMP_NAME, 10.0), _status(ctx))
        still, st2, _inf = _wait_blocked(ctx, MAIL_SELECT, 2.0)
        ctx.ok("那一刻查源角色的 SELECT 还没回来（仍卡在 %s）" % st2, still, st2)
        ctx.ok("此刻当前角色还没换成临时角色（载入排在查询后面）",
               ("已载入 " + TMP_NAME) not in _status(ctx), _status(ctx))

        lock.close()                          # 放锁，让这份旧数据去该去的地方
        seen = []
        t0 = time.time()
        while time.time() - t0 < 30:
            pair = _mail_state(ctx)
            if pair not in seen:
                seen.append(pair)
            if pair[0] == 0 and "共 0 封邮件" in pair[1] and TMP_NAME in pair[1]:
                break
            time.sleep(0.05)
        rows_after, info_after = _mail_state(ctx)
        ctx.ok("放锁后进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
        ctx.ok("邮件列表不是源角色的（行数 %s != %s）" % (rows_after, n),
               rows_after != n, "rows=%s 轨迹=%r" % (rows_after, seen[-3:]))
        ctx.ok("提示里没有「共 %d 封邮件」" % n,
               ("共 %d 封邮件" % n) not in info_after, info_after)
        ctx.check("邮件列表是空的（临时角色本来就没信）", rows_after, 0)
        ctx.ok("提示挂在临时角色名下（「%s · 共 0 封邮件」）" % TMP_NAME,
               TMP_NAME in info_after and "共 0 封邮件" in info_after, info_after)
        ctx.ok("提示里没有源角色的名字", prep["src_name"] not in info_after, info_after)

        ctx.page(PAGE_GM)
        time.sleep(0.8)
        ctx.check("当前角色已经是临时角色（GM 页名字框）",
                  core.txt(ctx.ctl(G_NAME), 64), TMP_NAME)

        ctx.page(PAGE_MAIL)
        time.sleep(1.0)
        core.wait_until(lambda: _log_since(pos0).count(MAIL_DONE) >= 2, 10.0)
        n_done = _log_since(pos0).count(MAIL_DONE)
        ctx.ok("应用为当前角色重拉了一次邮件（「%s」%d 条 ≥ 2）" % (MAIL_DONE, n_done),
               n_done >= 2)

        # 反向对照：切回源角色刷新，列表要能正常重拉（别做成永久空）
        _click_char(ctx, prep["src_y"])
        time.sleep(0.5)
        core.press(ctx.ctl(M_REFRESH), 0.0)
        rows_back, info_back = _wait_mail(ctx, n, prep["src_name"], 25.0)
        ctx.check("切回源角色刷新后行数回到 %d" % n, rows_back, n)
        ctx.ok("提示挂在源角色名下（「%s · 共 %d 封邮件」）" % (prep["src_name"], n),
               prep["src_name"] in info_back and ("共 %d 封邮件" % n) in info_back,
               info_back)
        ctx.check("源角色的邮件一封没少（库里）", _postal_n(ctx, cno), n)
        ctx.ok("全程进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.close()
        time.sleep(1.0)
        _swap_end(ctx, cno, before, mid)


# ---------------------------------------------------------------- 封停列表
def _ban_n(ctx):
    n = ctx.q1("SELECT COUNT(*) FROM " + CHARAC_INFO + " WHERE delete_flag=0")
    return min(n or 0, 5000)


@core.case("ban.chars_load", needs=("gui", "db"), group="always",
           desc="进封停页后角色列表行数、角色下拉与库中未删角色数一致")
def ban_chars_load(ctx):
    """验：正常加载时列表、下拉、提示三处都是全服角色数。"""
    n = _ban_n(ctx)
    if not n:
        ctx.skip("库里没有未删角色")
        return
    ctx.page(PAGE_BAN)
    ok = core.wait_until(lambda: _rows(ctx, B_LIST) == n and _combo_n(ctx) == n, 20.0)
    if not ctx.ok("列表与角色下拉都加载到库里的数", ok,
                  "列表 %s / 下拉 %s / 期望 %s" % (_rows(ctx, B_LIST), _combo_n(ctx), n)):
        return
    ctx.check("列表行数 == 库里角色数", _rows(ctx, B_LIST), n)
    ctx.check("角色下拉项数 == 库里角色数", _combo_n(ctx), n)
    info = core.txt(ctx.ctl(B_INFO), 512)
    ctx.ok("提示是「共 %d 个角色」" % n, ("共 %d 个角色" % n) in info, info)


@core.case("ban.chars_fail_keeps", needs=("gui", "db"), group="always",
           desc="角色清单查询失败时列表与下拉不清空、提示停在读取中，放锁后恢复")
def ban_chars_fail_keeps(ctx):
    """验：查询失败不许被当成「全服 0 个角色」，旧清单原样留着。"""
    n = _ban_n(ctx)
    if not n:
        ctx.skip("库里没有未删角色")
        return
    lock = _LockConn()
    try:
        ctx.page(PAGE_BAN)
        if not ctx.ok("起点：列表与下拉都是库里的角色数",
                      core.wait_until(lambda: _rows(ctx, B_LIST) == n
                                      and _combo_n(ctx) == n, 20.0),
                      "列表 %s / 下拉 %s" % (_rows(ctx, B_LIST), _combo_n(ctx))):
            return
        b = ctx.ctl(B_LOADALL)
        if not ctx.ok("「加载全服」按钮存在", bool(b), "控件 %d" % B_LOADALL):
            return

        lock.open()
        own = _own_ids(ctx, lock)
        mark = _log_mark()
        lock.lock(LOGIN)
        core.press(b, 0.0)
        # 封停清单本身也读登录表（取 IP），所以这里不按语句前缀挑，卡住的哪条都打掉
        seen, killed = _kill_until(lock, own, BAN_FAIL, mark, deadline=30.0)
        ctx.ok("应用侧报了「%s」" % BAN_FAIL, seen, "打掉 %d 条" % killed)

        time.sleep(1.5)
        ctx.check("失败后列表没塌成 0 行", _rows(ctx, B_LIST), n)
        ctx.check("失败后角色下拉没被清空", _combo_n(ctx), n)
        info = core.txt(ctx.ctl(B_INFO), 512)
        ctx.ok("提示停在「正在读取数据…」", "正在读取数据" in info, info)
        ctx.ok("提示没有变成「共 0 个角色」", "共 0 个角色" not in info, info)

        lock.close()
        ctx.ok("放锁后列表与下拉都回到库里的数",
               core.wait_until(lambda: _rows(ctx, B_LIST) == n and _combo_n(ctx) == n, 25.0),
               "列表 %s / 下拉 %s" % (_rows(ctx, B_LIST), _combo_n(ctx)))
        ctx.check("放锁后列表行数回到库里角色数", _rows(ctx, B_LIST), n)
        ctx.check("放锁后角色下拉项数回到库里角色数", _combo_n(ctx), n)
        ctx.ok("全程进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        lock.close()


# ---------------------------------------------------------------- 在线闸门
@core.case("ban.online_gate", needs=("gui", "db"), group="always",
           desc="角色在线时物品页的编辑/清空在弹框之前被拦下，库存一字节不动")
def online_gate(ctx):
    """验：在线状态由账号在线 + 正在玩该角色造出，两条写路径都先提示后放弃。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = ctx.q1("SELECT m_id FROM " + CHARAC_INFO + " WHERE charac_no=%s", (cno,))
    rows_before = ctx.q("SELECT * FROM " + LOGIN + " WHERE m_id=%s", (mid,))
    if not ctx.ok("账号表里有这一行（在线状态才造得出来）", bool(rows_before),
                  "m_id=%s" % mid):
        return
    login_sha = _sha(rows_before)
    cols = [r[0] for r in ctx.q("SHOW COLUMNS FROM " + LOGIN)]
    if not ctx.ok("login_account_3 有 login_status 列", "login_status" in cols):
        return
    idx = cols.index("login_status")
    play_before = ctx.q1("SELECT COUNT(*) FROM " + PLAY_EVENT)
    pair_before = ctx.q1("SELECT COUNT(*) FROM " + PLAY_EVENT +
                         " WHERE m_id=%s AND charac_no=%s", (mid, cno))
    inv_rows = ctx.q("SELECT * FROM " + INVENTORY + " WHERE charac_no=%s", (cno,))
    inv_n = len(inv_rows)
    inserted = False
    try:
        ctx.cur.execute("UPDATE " + LOGIN + " SET login_status=1 WHERE m_id=%s", (mid,))
        ctx.ok("账号已是在线（login_status=1）",
               rows_before[0][idx] == 1
               or _sha(ctx.q("SELECT * FROM " + LOGIN + " WHERE m_id=%s", (mid,))) != login_sha)
        if not pair_before:
            ctx.cur.execute("INSERT INTO " + PLAY_EVENT +
                            " (m_id,charac_no,occ_date) VALUES (%s,%s,NOW())", (mid, cno))
            inserted = True
        ctx.check("已标记「正在玩这个角色」",
                  ctx.q1("SELECT COUNT(*) FROM " + PLAY_EVENT +
                         " WHERE m_id=%s AND charac_no=%s", (mid, cno)),
                  max(1, pair_before))

        time.sleep(_poll_sec() + 1.0)         # 等轮询把在线状态带进左栏
        if not ctx.ok("角色可选中", _select_first(ctx)):
            return
        time.sleep(_poll_sec() + 1.0)         # 载入后再等一轮轮询把 g.cur.online 带上来
        ctx.page(PAGE_ITEM)
        time.sleep(1.5)
        lv = ctx.ctl(I_LIST)
        nrows = core.rows(lv) if lv else -1
        if not ctx.ok("物品页已经载入（列表有行）", nrows > 1, "rows=%s" % nrows):
            return
        core.click_at(lv, *_row_xy(lv, 0), wait=0.4)

        edit = ctx.ctl(I_EDIT)
        clr = ctx.ctl(I_CLR)
        if not ctx.ok("物品页编辑/清空按钮存在", bool(edit) and bool(clr)):
            return
        core.press(edit, 0.0)
        box, text = _wait_box(ctx)
        ctx.ok("在线时点「编辑」被拦下（提示里说明在线）",
               box is not None and "在线" in text, text[:80])
        ctx.ok("出来的不是物品编辑框", "耐久" not in text and "增幅值" not in text,
               text[:80])
        if box:
            _close_box(box)
        ctx.ok("编辑提示框已关闭（不留模态挡住下一次点击）",
               core.wait_until(lambda: not _top("#32770", ctx.proc.pid), 5.0),
               str(_top("#32770", ctx.proc.pid)))
        time.sleep(0.4)

        core.press(clr, 0.0)
        box2, text2 = _wait_box(ctx)
        ctx.ok("在线时点「清空所选」被拦下（提示里说明在线）",
               box2 is not None and "在线" in text2, text2[:80])
        ctx.ok("出来的不是「清空第 N 格」的确认框", "清空第" not in text2, text2[:80])
        if box2:
            _close_box(box2)

        time.sleep(1.5)
        after = ctx.q("SELECT * FROM " + INVENTORY + " WHERE charac_no=%s", (cno,))
        ctx.check("inventory 行数没变", len(after), inv_n)
        ctx.ok("inventory 整行字节与试之前一致", after == inv_rows)
        ctx.ok("全程进程没崩", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        try:
            ctx.cur.execute("UPDATE " + LOGIN + " SET login_status=%s WHERE m_id=%s",
                            (rows_before[0][idx], mid))
        except Exception:
            pass
        if inserted:
            try:
                ctx.cur.execute("DELETE FROM " + PLAY_EVENT +
                                " WHERE m_id=%s AND charac_no=%s", (mid, cno))
            except Exception:
                pass
        after = ctx.q("SELECT * FROM " + LOGIN + " WHERE m_id=%s", (mid,))
        ctx.check("还原后 login_account_3 那一行与改前逐字节一致",
                  _sha(after), login_sha)
        ctx.check("event_1306_account_reward 行数回到改前",
                  ctx.q1("SELECT COUNT(*) FROM " + PLAY_EVENT), play_before)
