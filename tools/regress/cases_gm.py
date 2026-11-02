# -*- coding: utf-8 -*-
"""GM 页用例：失焦写库、写队列串行、轮询缓存、一键与表单字段、数值框输入、断开丢弃。"""
import configparser
import ctypes
import ctypes.wintypes as wt
import os
import time
from contextlib import contextmanager

from . import core

u = core.u

# ---------------------------------------------------------------- 窗口与控件
IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191
PAGE_ITEM, PAGE_PET, PAGE_GM = 0, 4, 8
I_LIST = core.PID(PAGE_ITEM, 2)          # 物品页列表
P_BAD = core.PID(PAGE_PET, 9)            # 宠物页「表内错误记录」

# GM 页控件编号，与 ui_char.c 的 G_* 枚举一一对应
G = dict(NAME=1801, LEV=1802, JOB=1803, GROW=1804, MONEY=1805, COIN=1806, CERA=1807, POINT=1808,
         ACCM=1809, GRADE=1810, WIN=1811, PVP_POINT=1812, SP=1813, QP=1814, NOWEIGHT=1815,
         SLOT=1816, UNLOCK=1817, EXPERT=1818, FATIGUE=1819, DUNGEON=1820, PUNISH=1821, QCLR=1822,
         UNLCREATE=1826)
G_QADD = core.PID(PAGE_GM, 70)           # 8 个「+数量」按钮的起点，顺序见 ui_char.c 的 kQAdd

WM_TIMER, WM_CHAR, WM_CLOSE = 0x0113, 0x0102, 0x0010
BM_CLICK = 0x00F5
LVM_GETNEXTITEM, LVNI_SELECTED = 0x100C, 0x0002
TAB_TITLE = "宠物/时装表内错误记录"

# 真实前台输入要用到的入口：不声明 argtypes 的话 HWND 会被当成 32 位 int 截断
u.GetForegroundWindow.restype = wt.HWND
u.SetForegroundWindow.argtypes = [wt.HWND]
u.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u.mouse_event.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
u.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
u.SetFocus.argtypes = [wt.HWND]
u.GetFocus.restype = wt.HWND
u.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u.GetWindowThreadProcessId.restype = wt.DWORD
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindow.argtypes = [wt.HWND, wt.UINT]
u.GetWindow.restype = wt.HWND
u.GetParent.argtypes = [wt.HWND]
u.GetParent.restype = wt.HWND


# ---------------------------------------------------------------- 小工具
def _lev_max():
    """等级上限取配置 [gm] lev_max，缺省 85（与程序自己的默认值一致）。"""
    v = 85
    ini = os.path.join(core.BUILD, "config", "settings.ini")
    if os.path.exists(ini):
        cfg = configparser.RawConfigParser()
        cfg.read(ini, encoding="utf-8")
        try:
            v = cfg.getint("gm", "lev_max", fallback=85)
        except Exception:
            v = 85
    return v if 1 <= v <= 255 else 85


def _until(fn, want, timeout=10.0, step=0.3):
    """等取值函数返回期望值：写库是后台作业，提交不等于落库。"""
    got = fn()
    t0 = time.time()
    while got != want and time.time() - t0 < timeout:
        time.sleep(step)
        got = fn()
    return got


def _text(h, want, timeout=5.0):
    """等控件文本变成期望值（提交后框清回 0 也一样有延迟）。"""
    got = core.txt(h)
    t0 = time.time()
    while got != want and time.time() - t0 < timeout:
        time.sleep(0.2)
        got = core.txt(h)
    return got


def _commit(h, wait=1.2):
    """失焦即提交：界面上「点到别处」才写库，这里直接发 WM_KILLFOCUS。"""
    u.SendMessageW(h, core.WM_KILLFOCUS, 0, 0)
    if wait:
        time.sleep(wait)


def _title(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, b, 256)
    return b.value


def _static_texts(main):
    """主窗口下全部 STATIC 的文本：左栏角色卡是一堆无 ID 的 STATIC。"""
    return [core.txt(h, 256) for h in core.kids(main) if core.cls_of(h) == "Static"]


# ---------------------------------------------------------------- 库
def _mid(ctx, cno):
    return ctx.q1("SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))


def _money(ctx, cno):
    return ctx.q1("SELECT money FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (cno,))


def _coin(ctx, cno):
    return ctx.q1("SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (cno,))


def _lev(ctx, cno):
    return ctx.q1("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))


def _cera(ctx, mid):
    return ctx.q1("SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s", (mid,))


def _offline(ctx, mid):
    """GM 表单与「+」按钮在角色在线时按设计拒绝写入，先确认角色离线。"""
    try:
        st = ctx.q1("SELECT MAX(login_status) FROM taiwan_login.login_account_3 WHERE m_id=%s",
                    (mid,))
    except Exception:
        return True                      # 没有登录表就当离线，交给用例本身去判
    if st:
        ctx.skip("角色在线（login_status=%s），GM 写库入口按设计拒绝写入" % st)
        return False
    return True


def _set_dirty(ctx, table, keycol, key, col, dirty):
    """把值改成脏值再点按钮，否则「目标值本来就是它」等于什么都没验。

    行不存在就先建一行：懒建的表（QP 商店、装备等级管理）只 UPDATE 会 0 行。
    """
    cur = ctx.cur
    cur.execute("UPDATE %s SET %s=%%s WHERE %s=%%s" % (table, col, keycol), (dirty, key))
    if not cur.rowcount and not ctx.q1(
            "SELECT 1 FROM %s WHERE %s=%%s LIMIT 1" % (table, keycol), (key,)):
        try:
            cur.execute("INSERT INTO %s (%s,%s) VALUES (%%s,%%s)" % (table, keycol, col),
                        (key, dirty))
        except Exception:
            return False
    return True


class Snap:
    """写库前的行快照：finally 里按原值写回，原本没有的行走删除。

    用例之间不共享状态，失败也不会把库留在脏值上。
    """

    def __init__(self, ctx):
        self.ctx = ctx
        self.items = []

    def take(self, table, keycol, key, cols):
        """记下一行（不存在记 None），返回该行元组或 None。"""
        cols = tuple(cols)
        row = self.ctx.q("SELECT %s FROM %s WHERE %s=%%s"
                         % (",".join(cols), table, keycol), (key,))
        row = row[0] if row else None
        self.items.append((table, keycol, key, cols, row))
        return row

    def restore(self):
        for table, keycol, key, cols, row in self.items:
            cur = self.ctx.cur
            if row is None:
                cur.execute("DELETE FROM %s WHERE %s=%%s" % (table, keycol), (key,))
            else:
                sets = ",".join("%s=%%s" % c for c in cols)
                cur.execute("UPDATE %s SET %s WHERE %s=%%s" % (table, sets, keycol),
                            tuple(row) + (key,))
        self.items = []          # 还原过的就销账：重复调用是空操作


def _take_soft(ctx, snap, table, keycol, key, cols):
    """记一行快照；表/列不可用时记一次跳过，别让整条用例崩在取快照上。"""
    try:
        return snap.take(table, keycol, key, cols)
    except Exception as e:
        ctx.skip("快照失败：%s（%s）" % (table, e))
        return None


@contextmanager
def _lock(tables):
    """用另一条连接 LOCK TABLES：把「连接忙」变成可控状态，而不是靠 sleep 抢时序。"""
    c = core.db()
    try:
        c.cursor().execute("LOCK TABLES " + ",".join(t + " WRITE" for t in tables))
        yield
    finally:
        try:
            c.cursor().execute("UNLOCK TABLES")
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass


def _blocked(ctx, table, timeout=15.0):
    """等到有查询卡在这张表的元数据锁上：此时程序的连接确实被占住了。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for _, st, info in ctx.q("SELECT ID,STATE,IFNULL(INFO,'') FROM information_schema.PROCESSLIST "
                                 "WHERE COMMAND='Query'"):
            if table in info and "metadata lock" in st:
                return True, st
        time.sleep(0.2)
    return False, ""


# ---------------------------------------------------------------- 窗口前置
def _conn_ready(ctx, timeout=25.0):
    """连库完成：左栏角色列表已经有行。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    return core.wait_until(lambda: core.rows(lv) > 0, timeout, 0.2)


def _prep(ctx):
    """用例公共前置：连库、取左栏第一行对应的角色编号。"""
    if not ctx.ok("连库后左栏角色列表有行", _conn_ready(ctx)):
        return None
    cno = ctx.char_no()          # 列表按 charac_no 降序，第一行就是编号最大的角色
    if not ctx.ok("库里有角色", bool(cno)):
        return None
    return cno


def _top_windows(main, cls=None):
    """主窗口名下的可见顶层窗口：确认框（#32770）与结果窗（DnfDlg）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        c = core.cls_of(h)
        if cls and c != cls:
            return True
        # 只认主窗口名下的：别人的对话框不能被当成工具弹的框
        if u.GetWindow(h, 4) != main and u.GetParent(h) != main:
            return True
        out.append((h, c, _title(h)))
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _dlg_text(h):
    """窗口里所有 Static/Button 的文本拼起来：结果窗的正文从这里读。"""
    parts = []
    for c in core.kids(h):
        if core.cls_of(c) in ("Static", "Button"):
            t = core.txt(c, 2048)
            if t:
                parts.append(t)
    return " / ".join(parts)


def _close_box(ctx):
    """点掉意外的模态提示框（例如「请先选择角色。」）—— 模态循环会卡住后面的消息。"""
    w = _top_windows(ctx.main, "#32770")
    if not w:
        return None
    t = _dlg_text(w[0][0])
    b = u.GetDlgItem(w[0][0], 1)                 # IDOK
    if b:
        u.PostMessageW(b, BM_CLICK, 0, 0)
        core.wait_until(lambda: not _top_windows(ctx.main, "#32770"), 8.0, 0.05)
    return t


def _confirm(ctx, timeout=5.0):
    """确认框出现并点掉（IDOK），返回点掉了几个。"""
    n = 0
    t0 = time.time()
    while time.time() - t0 < timeout:
        for h, _, _ in _top_windows(ctx.main, "#32770"):
            b = u.GetDlgItem(h, 1)
            if b:
                u.PostMessageW(b, BM_CLICK, 0, 0)
                n += 1
        if n:
            time.sleep(0.3)
            return n
        time.sleep(0.2)
    return n


def _tab_dlg(main):
    """宠物/时装表内错误记录的结果窗。"""
    for h, _, t in _top_windows(main, "DnfDlg"):
        if t == TAB_TITLE:
            return h
    return None


def _status(ctx):
    """状态栏文本。WM_GETTEXT 在 WM_USER 以下由系统编组，跨进程读得到。"""
    h = u.GetDlgItem(ctx.main, IDC_STATUS)
    return core.txt(h, 512) if h else ""


def _wait_status(ctx, want, timeout=25.0):
    """等状态栏出现某段文字（断开/连接都是后台作业收尾时才改状态）。"""
    s = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = _status(ctx)
        if want in s:
            return True, s
        time.sleep(0.2)
    return False, s


def _connected(ctx, timeout=30.0):
    """重连完成：左栏列表又填出来了。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    return core.wait_until(lambda: core.rows(lv) > 0, timeout, 0.2)


def _select_char(ctx, cno, timeout=15.0):
    """点左栏第一行选角色，结束后停在 GM 页。

    判据用 GM 页的名字框：只有选中角色成功（char_ok 为真）时页面才把角色名填进去，
    重连之后 char_ok 会归零，必须重新选一次。
    """
    _close_box(ctx)
    core.click_at(u.GetDlgItem(ctx.main, IDC_CHLIST), 40, 12, 1.0)
    want = ctx.q1("SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    ctx.page(PAGE_GM)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if core.txt(ctx.ctl(G["NAME"]), 64) == want:
            return True
        _close_box(ctx)
        time.sleep(0.3)
    return core.txt(ctx.ctl(G["NAME"]), 64) == want


def _press(ctx, cid, wait=0.0, label=""):
    """点按钮。必须异步投递：按钮的处理里可能弹模态确认框，同步发送会卡死。"""
    h = ctx.ctl(cid)
    if not h:
        ctx.ok("%s按钮存在" % (label or "控件 %d " % cid), False, "控件 %d" % cid)
        return False
    core.press(h, wait)
    return True


# ================================================================ 失焦写库
@core.case("gm.write.money", needs=("gui", "db"), group="always",
           desc="GM 页金币框失焦提交后写进库，框随即清回 0")
def gm_write_money(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        if not _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money",)):
            ctx.skip("inventory 没有这个角色的行")
            return
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        money = ctx.ctl(G["MONEY"])
        if not ctx.ok("金币输入框存在", bool(money), "控件 %d" % G["MONEY"]):
            return
        core.set_text(money, "12345")
        _commit(money, 0)
        ctx.check("失焦后金币写进库", _until(lambda: _money(ctx, cno), 12345), 12345)
        ctx.check("提交后框清回 0", _text(money, "0"), "0")
    finally:
        snap.restore()


@core.case("gm.write.restore", needs=("gui", "db"), group="always",
           desc="把金币填回原值同样走写库路径，库里回到原值")
def gm_write_restore(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        row = _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money",))
        if not row:
            ctx.skip("inventory 没有这个角色的行")
            return
        m0 = row[0]
        if not m0:
            ctx.skip("原值为 0：数值框按设计不接受「设为 0」，没法用填原值的方式还原")
            return
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        money = ctx.ctl(G["MONEY"])
        if not ctx.ok("金币输入框存在", bool(money), "控件 %d" % G["MONEY"]):
            return
        core.set_text(money, "12345")
        _commit(money, 0)
        if not ctx.check("先改成 12345", _until(lambda: _money(ctx, cno), 12345), 12345):
            return
        core.set_text(money, str(m0))
        _commit(money, 0)
        ctx.check("填回原值后库里是原值", _until(lambda: _money(ctx, cno), m0), m0)
        ctx.check("填回原值后框也清回 0", _text(money, "0"), "0")
    finally:
        snap.restore()


@core.case("gm.queue.four_fields", needs=("gui", "db"), group="always",
           desc="连改金币/复活币/点券/等级四个字段（间隔 80ms），四个都落到同一个角色")
def gm_queue_four(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money", "coin"))
        _take_soft(ctx, snap, "taiwan_billing.cash_cera", "account", mid, ("cera",))
        _take_soft(ctx, snap, "taiwan_billing.cash_cera_point", "account", mid, ("cera_point",))
        _take_soft(ctx, snap, "taiwan_cain.charac_info", "charac_no", cno, ("lev",))
        _take_soft(ctx, snap, "taiwan_cain.charac_stat", "charac_no", cno, ("exp",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        # 连着改四个字段，中间只留 80ms：模拟按 Tab 快速跳着改
        for cid, val in ((G["MONEY"], "111111"), (G["COIN"], "2222"),
                         (G["CERA"], "3333"), (G["LEV"], "60")):
            h = ctx.ctl(cid)
            if not ctx.ok("控件 %d 存在" % cid, bool(h)):
                return
            core.set_text(h, val)
            _commit(h, 0.08)
        ctx.check("金币落库", _until(lambda: _money(ctx, cno), 111111), 111111)
        ctx.check("复活币落库", _until(lambda: _coin(ctx, cno), 2222), 2222)
        ctx.check("点券落库", _until(lambda: _cera(ctx, mid), 3333), 3333)
        ctx.check("等级落库", _until(lambda: _lev(ctx, cno), 60), 60)
    finally:
        snap.restore()


# ================================================================ 刷新与缓存
@core.case("gm.stable.item_sel", needs=("gui", "db"), group="always",
           desc="物品页选中行扛过一次 reload：切到 GM 再切回来选中不丢")
def gm_stable_item_sel(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
        return
    ctx.page(PAGE_ITEM)
    il = ctx.ctl(I_LIST)
    if not ctx.ok("物品列表存在", bool(il), "控件 %d" % I_LIST):
        return
    n = core.rows(il)
    if n < 4:
        ctx.skip("物品只有 %d 行，选不出第 3 行" % n)
        return
    for y in (12, 34, 56):
        core.click_at(il, 40, y, 0.15)
    sel = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
    if not ctx.ok("点中了第 3 行", sel >= 2, "选中行 %s" % sel):
        return
    ctx.page(PAGE_GM)                 # 切走再切回：两次 reload
    time.sleep(0.4)
    ctx.page(PAGE_ITEM)
    time.sleep(0.8)
    sel2 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
    ctx.check("reload 后选中行不变", sel2, sel)


@core.case("gm.stable.two_fields", needs=("gui", "db"), group="always",
           desc="GM 页连改金币/复活币，写库期间连灌 6 次 tick，两个字段都落库")
def gm_stable_two_fields(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money", "coin"))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        money, coin = ctx.ctl(G["MONEY"]), ctx.ctl(G["COIN"])
        if not ctx.ok("金币/复活币输入框存在", bool(money) and bool(coin)):
            return
        for h, val in ((money, "54321"), (coin, "7")):
            core.set_text(h, val)
            _commit(h, 0)
            # 写库还没落地时连灌 tick：字段值不该被快照里的旧值顶掉
            for _ in range(6):
                u.PostMessageW(ctx.main, WM_TIMER, 1, 0)
                time.sleep(0.03)
        ctx.check("金币落库", _until(lambda: _money(ctx, cno), 54321), 54321)
        ctx.check("复活币落库", _until(lambda: _coin(ctx, cno), 7), 7)
    finally:
        snap.restore()


@core.case("gm.poll.card", needs=("gui", "db"), group="always",
           desc="轮询把左栏角色卡刷成新等级，GM 表单快照保持不动")
def gm_poll_card(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    lev0 = _lev(ctx, cno)
    if not lev0 or lev0 >= 250:
        ctx.skip("等级 %r 不适合做 +1 观察" % (lev0,))
        return
    snap = Snap(ctx)
    try:
        _take_soft(ctx, snap, "taiwan_cain.charac_info", "charac_no", cno, ("lev",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        levw = ctx.ctl(G["LEV"])
        if not ctx.ok("等级输入框存在", bool(levw), "控件 %d" % G["LEV"]):
            return
        ctx.check("进页面时等级框是 0（框只收本次要设的值）", _text(levw, "0", 3.0), "0")
        # 模拟玩家在游戏里升级：直接改库，等轮询把左栏拉回来
        ctx.cur.execute("UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=%s",
                        (lev0 + 1, cno))
        want = "Lv.%d" % (lev0 + 1)
        seen = core.wait_until(lambda: want in _static_texts(ctx.main), 25.0, 0.5)
        cards = [s for s in _static_texts(ctx.main) if s.startswith("Lv.")]
        ctx.ok("轮询把左栏角色卡刷成新等级", seen, "找 %s，卡上是 %s" % (want, cards[:3]))
        ctx.check("轮询不动 GM 表单快照（等级框仍是 0）", _text(levw, "0", 3.0), "0")
    finally:
        snap.restore()


@core.case("gm.poll.item_sel", needs=("gui", "db"), group="always",
           desc="轮询只刷左栏，不触发页面重排：物品页选中行在轮询后仍在")
def gm_poll_item_sel(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    lev0 = _lev(ctx, cno)
    if not lev0 or lev0 >= 250:
        ctx.skip("等级 %r 不适合做 +1 观察" % (lev0,))
        return
    snap = Snap(ctx)
    try:
        _take_soft(ctx, snap, "taiwan_cain.charac_info", "charac_no", cno, ("lev",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        ctx.page(PAGE_ITEM)
        il = ctx.ctl(I_LIST)
        if not ctx.ok("物品列表存在", bool(il), "控件 %d" % I_LIST):
            return
        if core.rows(il) < 4:
            ctx.skip("物品不足 4 行，选不出可观察的行")
            return
        core.click_at(il, 40, 56, 0.3)
        sel0 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
        if not ctx.ok("先选中一行作为观察点", sel0 >= 0, "选中行 %s" % sel0):
            return
        ctx.page(PAGE_GM)                     # 站在 GM 页等轮询
        time.sleep(0.6)
        ctx.cur.execute("UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=%s",
                        (lev0 + 1, cno))
        want = "Lv.%d" % (lev0 + 1)
        if not ctx.ok("轮询已经跑过一轮", core.wait_until(
                lambda: want in _static_texts(ctx.main), 25.0, 0.5), "找 %s" % want):
            return
        ctx.page(PAGE_ITEM)
        time.sleep(0.8)
        sel1 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
        ctx.check("轮询后物品页选中行没丢", sel1, sel0)
    finally:
        snap.restore()


# ================================================================ 一键按钮
def _one_key(ctx, snap, cno, label, cid, table, col, target, dirty):
    """点一个一键按钮：先把值改成脏值，再看它是否被改成目标值。"""
    try:
        snap.take(table, "charac_no", cno, (col,))
    except Exception as e:
        ctx.skip("%s：表/列不可用（%s）" % (label, e))
        return
    h = ctx.ctl(cid)
    if not ctx.ok("%s：按钮存在" % label, bool(h), "控件 %d" % cid):
        return
    if not _set_dirty(ctx, table, "charac_no", cno, col, dirty):
        ctx.skip("%s：脏值 %s 写不进去" % (label, dirty))
        return
    sel = "SELECT %s FROM %s WHERE charac_no=%%s" % (col, table)
    if _until(lambda: ctx.q1(sel, (cno,)), dirty, 5.0) != dirty:
        ctx.skip("%s：脏值 %s 没写进去，无从判断按钮有没有生效" % (label, dirty))
        return
    want = target
    if want is None:                     # 补满疲劳：目标是当前的 max_fatigue
        want = ctx.q1("SELECT max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s",
                      (cno,))
    core.press(h, 0)
    ctx.check("%s：脏值 %s 被改成 %s" % (label, dirty, want),
              _until(lambda: ctx.q1(sel, (cno,)), want, 10.0), want)


@core.case("gm.btn.one_key", needs=("gui", "db"), group="always",
           desc="一键按钮逐个先改脏值再点，看它是否落到目标值（SP/QP/负重/槽位/装备等级/专家/疲劳）")
def gm_btn_one_key(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        # 同一动作会连带写同表的其它列，一并快照还原
        _take_soft(ctx, snap, "taiwan_cain_2nd.skill", "charac_no", cno,
                   ("remain_sp_2nd", "remain_sfp_1st", "remain_sfp_2nd"))
        _take_soft(ctx, snap, "taiwan_cain.charac_stat", "charac_no", cno, ("used_fatigue",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        for label, cid, table, col, target, dirty in (
                ("满SP/TP", G["SP"], "taiwan_cain_2nd.skill", "remain_sp", 90000, 7),
                ("满QP", G["QP"], "taiwan_cain.charac_quest_shop", "qp", 90000, 7),
                ("无限负重", G["NOWEIGHT"], "taiwan_cain.charac_info", "inven_weight",
                 99999999, 100),
                ("开装备槽", G["SLOT"], "taiwan_cain.charac_stat", "add_slot_flag", 3, 0),
                # 目标值取端允许的最高等级（settings.ini [gm] lev_max）
                ("解锁装备等级", G["UNLOCK"], "taiwan_cain.charac_manage_info",
                 "max_equip_level", _lev_max(), 10),
                ("满专家", G["EXPERT"], "taiwan_cain.charac_stat", "expert_job_exp", 2054, 7),
                ("补满疲劳", G["FATIGUE"], "taiwan_cain.charac_info", "fatigue", None, 7)):
            _one_key(ctx, snap, cno, label, cid, table, col, target, dirty)
    finally:
        snap.restore()


@core.case("gm.btn.cleanup", needs=("gui", "db"), group="always",
           desc="清理类按钮：全图全难度被写满、惩罚清空、任务进度归零、建角次数限制归零")
def gm_btn_cleanup(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return

        # ---- 解锁全图：先把 dungeon 串清空，点完验它被写成全图全难度
        try:
            snap.take("taiwan_cain.member_dungeon", "m_id", mid, ("dungeon",))
            ctx.cur.execute("UPDATE taiwan_cain.member_dungeon SET dungeon='' WHERE m_id=%s",
                            (mid,))
        except Exception as e:
            ctx.skip("解锁全图：表/列不可用（%s）" % e)
        else:
            if _press(ctx, G["DUNGEON"], 0, "解锁全图："):
                dun = ""
                t0 = time.time()
                while time.time() - t0 < 10:
                    dun = ctx.q1("SELECT dungeon FROM taiwan_cain.member_dungeon "
                                 "WHERE m_id=%s", (mid,)) or ""
                    if "1|3" in dun:
                        break
                    time.sleep(0.3)
                ctx.ok("解锁全图：dungeon 写满全图全难度", "1|3" in dun, dun[:40])

        # ---- 清惩罚：先塞一条，清完验 0；账号原本就有记录时不测（清了无法逐条还原）
        try:
            cnt0 = ctx.q1("SELECT COUNT(*) FROM d_taiwan.member_punish_info WHERE m_id=%s",
                          (mid,))
        except Exception as e:
            cnt0, bad = None, e
        else:
            bad = None
        if bad is not None:
            ctx.skip("清惩罚：表不可用（%s）" % bad)
        elif cnt0:
            ctx.skip("清惩罚：账号已有 %d 条惩罚记录，清了无法逐条还原" % cnt0)
        else:
            try:
                ctx.cur.execute("INSERT INTO d_taiwan.member_punish_info (m_id,occ_time) "
                                "VALUES (%s,NOW())", (mid,))
            except Exception as e:
                ctx.skip("清惩罚：表不可用（%s）" % e)
            else:
                try:
                    ctx.check("清惩罚：造的惩罚记录已就位",
                              ctx.q1("SELECT COUNT(*) FROM d_taiwan.member_punish_info "
                                     "WHERE m_id=%s", (mid,)), 1)
                    if _press(ctx, G["PUNISH"], 0, "清惩罚："):
                        ctx.check("清惩罚：记录清空",
                                  _until(lambda: ctx.q1("SELECT COUNT(*) FROM "
                                                        "d_taiwan.member_punish_info "
                                                        "WHERE m_id=%s", (mid,)), 0), 0)
                finally:
                    ctx.cur.execute("DELETE FROM d_taiwan.member_punish_info WHERE m_id=%s",
                                    (mid,))

        # ---- 清任务：先把 play_1 写成 555，点完验归零（整行原值写回）
        cols = tuple(["play_%d" % i for i in range(1, 21)]
                     + ["play_%d_trigger" % i for i in range(1, 21)])
        row, bad = None, None
        try:
            row = snap.take("taiwan_cain.new_charac_quest", "charac_no", cno, cols)
        except Exception as e:
            bad = e
        if bad is not None:
            ctx.skip("清任务：表/列不可用（%s）" % bad)
        elif row is None:
            ctx.skip("清任务：角色没有任务记录行")
        else:
            ctx.cur.execute("UPDATE taiwan_cain.new_charac_quest SET play_1=555 "
                            "WHERE charac_no=%s", (cno,))
            if _press(ctx, G["QCLR"], 0, "清任务："):
                ctx.check("清任务：play_1 归零", _until(
                    lambda: ctx.q1("SELECT play_1 FROM taiwan_cain.new_charac_quest "
                                   "WHERE charac_no=%s", (cno,)), 0), 0)

        # ---- 解除建角限制：先写 count=9，点按钮并在确认框上点确定，验归零
        try:
            row = snap.take("d_taiwan.limit_create_character", "m_id", mid, ("count",))
        except Exception as e:
            ctx.skip("解除建角限制：表不可用（%s）" % e)
        else:
            if row is None:
                ctx.skip("解除建角限制：该账号没有建角限制记录（本来就能建角）")
            else:
                ctx.cur.execute("UPDATE d_taiwan.limit_create_character SET count=9 "
                                "WHERE m_id=%s", (mid,))
                if _press(ctx, G["UNLCREATE"], 0, "解除建角限制："):
                    n = _confirm(ctx)
                    ctx.ok("解除建角限制：确认框出现并已点掉", n > 0, "点掉 %d 个" % n)
                    ctx.check("解除建角限制：count 归零", _until(
                        lambda: ctx.q1("SELECT count FROM d_taiwan.limit_create_character "
                                       "WHERE m_id=%s", (mid,)), 0), 0)
    finally:
        snap.restore()


# ================================================================ 表单字段
@core.case("gm.form.fields", needs=("gui", "db"), group="always",
           desc="GM 表单五个字段失焦即写：金币/复活币/点券/赠送点券/账号金币")
def gm_form_fields(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money", "coin"))
        _take_soft(ctx, snap, "taiwan_billing.cash_cera", "account", mid, ("cera",))
        _take_soft(ctx, snap, "taiwan_billing.cash_cera_point", "account", mid, ("cera_point",))
        _take_soft(ctx, snap, "taiwan_cain.account_cargo", "m_id", mid, ("money",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        for label, cid, table, keycol, col, val in (
                ("金币", G["MONEY"], "taiwan_cain_2nd.inventory", "charac_no", "money", 123456),
                ("复活币", G["COIN"], "taiwan_cain_2nd.inventory", "charac_no", "coin", 77),
                ("点券", G["CERA"], "taiwan_billing.cash_cera", "account", "cera", 4321),
                ("赠送点券", G["POINT"], "taiwan_billing.cash_cera_point", "account",
                 "cera_point", 321),
                ("账号金币", G["ACCM"], "taiwan_cain.account_cargo", "m_id", "money", 888888)):
            key = cno if keycol == "charac_no" else mid      # 点券表按 account=m_id 归属
            h = ctx.ctl(cid)
            if not ctx.ok("%s：输入框存在" % label, bool(h), "控件 %d" % cid):
                continue
            core.set_text(h, str(val))
            _commit(h, 0)
            sel = "SELECT %s FROM %s WHERE %s=%%s" % (col, table, keycol)
            ctx.check("%s：失焦后落库" % label,
                      _until(lambda: ctx.q1(sel, (key,)), val), val)
            time.sleep(0.6)      # 让快照跟上：点券和赠送点券是同一条 UPDATE
    finally:
        snap.restore()


# ================================================================ 数值框「设成多少」与「+多少」
@core.case("gm.qadd.boxes", needs=("gui", "db", "pvf"), group="manual",
           desc="数值框只收「这次设成多少」：进页面为 0，+ 按钮在当前值上累加")
def gm_qadd_boxes(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    lim = _lev_max()
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        inv = _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno,
                         ("money", "coin"))
        if not inv:
            ctx.skip("inventory 没有这个角色的行")
            return
        m0, c0 = inv
        row = _take_soft(ctx, snap, "taiwan_cain.charac_info", "charac_no", cno, ("lev",))
        lev0 = row[0] if row else None
        _take_soft(ctx, snap, "taiwan_cain.charac_stat", "charac_no", cno, ("exp",))
        # 起点等级压在 [1, 上限) 内：撞上限时 +10 走的是另一条分支
        l0 = lev0 if (lev0 and 1 <= lev0 < lim) else max(1, min(70, lim - 1))
        if l0 != lev0:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=%s",
                            (l0, cno))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        money, coin, levw = ctx.ctl(G["MONEY"]), ctx.ctl(G["COIN"]), ctx.ctl(G["LEV"])
        if not ctx.ok("金币/复活币/等级三个框存在", bool(money) and bool(coin) and bool(levw)):
            return
        ctx.check("金币框进页面是 0", _text(money, "0", 3.0), "0")
        ctx.check("复活币框进页面是 0", _text(coin, "0", 3.0), "0")
        ctx.check("等级框进页面是 0", _text(levw, "0", 3.0), "0")

        if not _press(ctx, G_QADD + 1, 1.6, "金币 +1000万"):
            return
        ctx.check("金币 +1000万 落在当前值上",
                  _until(lambda: _money(ctx, cno), m0 + 10000000), m0 + 10000000)
        ctx.check("点完金币框仍是 0", _text(money, "0", 3.0), "0")
        if not _press(ctx, G_QADD + 2, 1.6, "复活币 +100"):
            return
        ctx.check("复活币 +100 落在当前值上",
                  _until(lambda: _coin(ctx, cno), c0 + 100), c0 + 100)
        if not _press(ctx, G_QADD + 0, 1.6, "等级 +10"):
            return
        want_lev = min(l0 + 10, lim)
        ctx.check("等级 +10 落在当前值上（封顶到 %d）" % lim,
                  _until(lambda: _lev(ctx, cno), want_lev), want_lev)
        ctx.check("升级后经验清零",
                  ctx.q1("SELECT exp FROM taiwan_cain.charac_stat WHERE charac_no=%s",
                         (cno,)), 0)

        # 连点三次金币 +1000万：每次都在上一次的结果上加
        for i in range(1, 4):
            want = m0 + (1 + i) * 10000000
            if not _press(ctx, G_QADD + 1, 1.2, "第 %d 次金币 +1000万" % i):
                return
            ctx.check("第 %d 次连点金币 +1000万" % i,
                      _until(lambda: _money(ctx, cno), want), want)
            time.sleep(0.4)     # 「+」的起点是快照里的当前值，让快照先跟上
    finally:
        snap.restore()


@core.case("gm.qadd.input", needs=("gui", "db", "pvf"), group="manual",
           desc="数值框手输：绝对值写入、填 0/空框不改、字母规范成 0、超上限夹住")
def gm_qadd_input(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    lim = _lev_max()
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money", "coin"))
        _take_soft(ctx, snap, "taiwan_cain.charac_info", "charac_no", cno, ("lev",))
        _take_soft(ctx, snap, "taiwan_cain.charac_stat", "charac_no", cno, ("exp",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        money, levw = ctx.ctl(G["MONEY"]), ctx.ctl(G["LEV"])
        if not ctx.ok("金币/等级输入框存在", bool(money) and bool(levw)):
            return

        # 手输一个绝对数：失焦按这个数写库，框随即回 0
        core.set_text(money, "30000")
        _commit(money, 0)
        ctx.check("手输 30000 设为库值", _until(lambda: _money(ctx, cno), 30000), 30000)
        ctx.check("交完框回 0", _text(money, "0"), "0")

        # 填 0 = 这次没有要设的值，不能把金币清零
        core.set_text(money, "0")
        _commit(money, 1.2)
        ctx.check("填 0 不动库", _money(ctx, cno), 30000)
        ctx.check("填 0 后框仍是 0", _text(money, "0", 3.0), "0")

        # 非法输入：字母串被规范成 0，等于没填，库里不动
        core.set_text(money, "abc")
        _commit(money, 1.2)
        ctx.check("字母串规范成 0", _text(money, "0", 3.0), "0")
        ctx.check("库里没被字母串污染", _money(ctx, cno), 30000)

        # 超上限：先夹到 4294967295 再写库
        core.set_text(money, "99999999999")
        _commit(money, 0)
        ctx.check("超上限夹到 4294967295 并写入",
                  _until(lambda: _money(ctx, cno), 4294967295), 4294967295)
        ctx.check("写完框回 0", _text(money, "0"), "0")

        # 空框失焦：兜回上一次的值，等于没填，库里不动
        core.set_text(money, "")
        _commit(money, 1.2)
        ctx.check("空框不动库", _money(ctx, cno), 4294967295)

        # 等级填 0 = 没填，不能把角色打成 0 级 / 1 级
        lev_now = _lev(ctx, cno)
        core.set_text(levw, "0")
        _commit(levw, 1.2)
        ctx.check("等级填 0 不动库", _lev(ctx, cno), lev_now)

        core.set_text(levw, "60")
        _commit(levw, 0)
        ctx.check("等级设为 60", _until(lambda: _lev(ctx, cno), 60), 60)
        ctx.check("等级框回 0", _text(levw, "0"), "0")

        # 越上限：夹到上限再写库
        core.set_text(levw, str(lim + 1))
        _commit(levw, 0)
        ctx.check("等级填 %d 夹到上限 %d" % (lim + 1, lim),
                  _until(lambda: _lev(ctx, cno), lim), lim)
        ctx.check("被夹后框回 0", _text(levw, "0"), "0")

        core.set_text(levw, str(lim))
        _commit(levw, 1.2)
        ctx.check("等级填上限 %d 可写入" % lim, _lev(ctx, cno), lim)

        if not _press(ctx, G_QADD + 0, 1.6, "等级 +10（已在上限）"):
            return
        ctx.check("已在上限时 +10 不动库", _lev(ctx, cno), lim)
    finally:
        snap.restore()


# ================================================================ 真实键盘输入
@core.case("gm.numkey.input", needs=("gui", "db", "desktop"), group="manual",
           desc="数值框走真实前台输入时只收数字，超上限的那一位敲不进去")
def gm_numkey(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    snap = Snap(ctx)
    try:
        _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money",))
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        e = ctx.ctl(G["MONEY"])
        if not ctx.ok("金币输入框存在", bool(e), "控件 %d" % G["MONEY"]):
            return
        # 前台 + 焦点：EDIT 对没有焦点的 WM_CHAR 直接丢弃，没这一段就测不到那两道过滤
        k32 = ctypes.WinDLL("kernel32")
        k32.GetCurrentThreadId.restype = wt.DWORD
        pid = wt.DWORD()
        tid = u.GetWindowThreadProcessId(ctx.main, ctypes.byref(pid))
        u.AttachThreadInput(k32.GetCurrentThreadId(), tid, True)
        u.ShowWindow(ctx.main, 9)                     # SW_RESTORE
        u.SetForegroundWindow(ctx.main)
        time.sleep(0.5)
        ctx.ok("前台窗口是主窗口", u.GetForegroundWindow() == ctx.main,
               "前台 %r，主窗口 %r" % (u.GetForegroundWindow(), ctx.main))
        rc = wt.RECT()
        u.GetWindowRect(e, ctypes.byref(rc))
        u.SetCursorPos((rc.left + rc.right) // 2, (rc.top + rc.bottom) // 2)
        time.sleep(0.15)
        u.mouse_event(0x0002, 0, 0, 0, 0)             # LEFTDOWN
        u.mouse_event(0x0004, 0, 0, 0, 0)             # LEFTUP
        time.sleep(0.3)
        u.SetFocus(e)
        time.sleep(0.2)
        if not ctx.ok("焦点在金币框", u.GetFocus() == e, "焦点 %r" % (u.GetFocus(),)):
            return
        # 清空后依次敲 A（字母）、5（数字）、B（字母）
        core.set_text(e, "")
        time.sleep(0.2)
        for ch in "A5B":
            u.SendMessageW(e, WM_CHAR, ord(ch), 0)
            time.sleep(0.05)
        ctx.check("字母进不去、数字能进", core.txt(e), "5")
        # 先填满 4294967295，再敲一位：超上限的那一下应被拒
        core.set_text(e, "4294967295")
        time.sleep(0.2)
        u.SendMessageW(e, WM_CHAR, ord("5"), 0)
        time.sleep(0.3)
        ctx.check("超上限敲不进去", core.txt(e), "4294967295")
    finally:
        snap.restore()


# ================================================================ 断开时丢弃未开始的意图
@core.case("gm.sticky.queue_drop", needs=("gui", "db"), group="always",
           desc="连接被占住时改的金币：断开后不落库、重连也不补写，断开后状态栏不带报错")
def gm_sticky_queue(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    mid = _mid(ctx, cno)
    snap = Snap(ctx)
    try:
        if not _offline(ctx, mid):
            return
        row = _take_soft(ctx, snap, "taiwan_cain_2nd.inventory", "charac_no", cno, ("money",))
        if not row:
            ctx.skip("inventory 没有这个角色的行")
            return
        m0 = row[0]
        if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
            return
        ctx.ok("状态栏读得到（后面几条断言靠它）", bool(_status(ctx)), _status(ctx))
        if not ctx.ok("金币输入框存在", bool(ctx.ctl(G["MONEY"])), "控件 %d" % G["MONEY"]):
            return
        v1 = 22222 if m0 != 22222 else 33333
        v2 = 33333 if v1 != 33333 else 44444

        # 对照：忙的时候改，放锁后必须补写（证明编辑确实排进了写队列）
        with _lock(["taiwan_cain.charac_info"]):
            busy, st = _blocked(ctx, "taiwan_cain.charac_info")
            ctx.ok("连接被占住（轮询卡在表锁上）", busy, st)
            core.set_text(ctx.ctl(G["MONEY"]), str(v1))
            _commit(ctx.ctl(G["MONEY"]), 1.5)
            ctx.check("忙的时候改值不立刻落库（只排队）", _money(ctx, cno), m0)
        ctx.check("放锁后这份改值补写进库", _until(lambda: _money(ctx, cno), v1, 20.0), v1)

        # 判定：忙的时候改，然后断开
        with _lock(["taiwan_cain.charac_info"]):
            busy, st = _blocked(ctx, "taiwan_cain.charac_info")
            ctx.ok("连接再次被占住", busy, st)
            core.set_text(ctx.ctl(G["MONEY"]), str(v2))
            _commit(ctx.ctl(G["MONEY"]), 1.0)
            ctx.check("第二个改值也只是排队", _money(ctx, cno), v1)
            u.PostMessageW(u.GetDlgItem(ctx.main, IDC_DISC), BM_CLICK, 0, 0)
            time.sleep(0.6)               # 让 UI 线程进到作业停止那一拍
        off, s = _wait_status(ctx, "已断开数据库", 30.0)
        ctx.ok("断开完成", off, s)
        time.sleep(1.5)                   # 留几拍：队列若没丢，会在这几拍里发给死连接
        s = _status(ctx)
        ctx.ok("断开后状态栏里没有「请先连接数据库」", "请先连接数据库" not in s, s)

        u.PostMessageW(u.GetDlgItem(ctx.main, IDC_CONN), BM_CLICK, 0, 0)
        ctx.ok("重连成功", _connected(ctx))
        ctx.ok("重连后重新选中角色", _select_char(ctx, cno))
        ctx.page(PAGE_GM)
        time.sleep(6.0)                   # 给残留队列足够时间打进新连接
        now = _money(ctx, cno)
        ctx.ok("排队的改值没有跨会话写进库", now in (m0, v1), "库里 %s" % now)
        ctx.ok("具体不是被丢弃的那个 %s" % v2, now != v2, "库里 %s" % now)
        # 收尾：还原写在用例里，末尾再确认一次（finally 里那条兜底是空操作）
        snap.restore()
        ctx.check("金币已还原", _money(ctx, cno), m0)
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % ctx.proc.poll())
    finally:
        snap.restore()


@core.case("gm.sticky.tab_scan", needs=("gui", "db"), group="always",
           desc="连接被占住时点的表内扫描：断开即丢弃，重连后不弹「发现 0 条错误记录」")
def gm_sticky_tab(ctx):
    cno = _prep(ctx)
    if not cno:
        return
    if not ctx.ok("选中第一个角色", _select_char(ctx, cno)):
        return
    ctx.ok("状态栏读得到（后面几条断言靠它）", bool(_status(ctx)), _status(ctx))
    ctx.page(PAGE_PET)
    time.sleep(1.0)
    bad = ctx.ctl(P_BAD)
    if not ctx.ok("宠物页「清错误记录」按钮存在", bool(bad), "控件 %d" % P_BAD):
        return

    with _lock(["taiwan_cain.charac_info"]):
        busy, st = _blocked(ctx, "taiwan_cain.charac_info")
        ctx.ok("连接被占住（扫描动作只能进队列）", busy, st)
        core.press(bad, 0)
        time.sleep(1.2)
        early = _tab_dlg(ctx.main)
        if early:                          # 抢在断开前跑起来就先关掉，继续测重连路径
            u.SendMessageW(early, WM_CLOSE, 0, 0)
            time.sleep(0.4)
        u.PostMessageW(u.GetDlgItem(ctx.main, IDC_DISC), BM_CLICK, 0, 0)
        time.sleep(0.6)
    off, s = _wait_status(ctx, "已断开数据库", 30.0)
    ctx.ok("断开完成", off, s)
    ctx.ok("断开时扫描动作被丢弃（没起线程、没结果窗）", _tab_dlg(ctx.main) is None)

    u.PostMessageW(u.GetDlgItem(ctx.main, IDC_CONN), BM_CLICK, 0, 0)
    ctx.ok("重连成功", _connected(ctx))
    ctx.ok("重连后重新选中角色", _select_char(ctx, cno))
    # 切页往返触发一次 reload：残留的待办会让它在这里弹「发现 0 条错误记录」
    ctx.page(PAGE_PET)
    time.sleep(1.0)
    ctx.page(PAGE_GM)
    time.sleep(0.5)
    ctx.page(PAGE_PET)
    core.wait_until(lambda: _tab_dlg(ctx.main) is not None, 10.0, 0.3)
    d = _tab_dlg(ctx.main)
    ctx.ok("重连后没有弹空窗", d is None, _dlg_text(d)[:100] if d else "")
    if d:
        u.SendMessageW(d, WM_CLOSE, 0, 0)
        time.sleep(0.5)

    # 正对照：连接空闲时点一次必须真能弹出结果窗，别把「不弹」当成功能没了
    ctx.page(PAGE_PET)
    time.sleep(0.5)
    if not _press(ctx, P_BAD, 0.5, "扫描（正对照）"):
        return
    d = None
    t0 = time.time()
    while time.time() - t0 < 20:
        d = _tab_dlg(ctx.main)
        if d:
            break
        time.sleep(0.3)
    ctx.ok("正对照：空闲时点扫描能弹出结果窗（扫描路径本身没坏）", d is not None,
           _dlg_text(d)[:100] if d else "")
    if d:
        u.SendMessageW(d, WM_CLOSE, 0, 0)
        time.sleep(0.5)
