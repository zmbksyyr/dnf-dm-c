# -*- coding: utf-8 -*-
"""物品域回归用例：发放/邮件页的类型由 PVF 判定、任务材料带数量入队、分类浏览窗口的
计数与入队、发放队列 tsv 的夹界与导出头、全服扫描闸、账号金库无记录时的写入、
任务槽位的动作参数隔离。

控件编号、表名沿用旧脚本；写库用例先取快照、finally 按原值写回；角色编号一律取
ctx.char_no()，不假设库里原本有几行。队列只在进程内存里，进程退出即清空。
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import time
from contextlib import contextmanager

from . import core
from .core import u

# ---------------------------------------------------------------- 常量
# 主窗口
IDC_CONN, IDC_DISC, IDC_CHLIST, IDC_STATUS = 104, 105, 113, 191

PAGE_QUERY, PAGE_GRANT, PAGE_MAIL, PAGE_QUEST, PAGE_GM = 1, 2, 3, 6, 8

# 查询页
Q_LIST, Q_INFO, Q_ALL, Q_TASK = 1102, 1103, 1106, 1110
Q_BMAT, Q_BSTK, Q_BEQU, Q_BPET, Q_BAVA = 1111, 1112, 1113, 1114, 1115

# 发放页
G_ID, G_NUM, G_TYPE, G_ENH, G_FORGE, G_SEALC = 1201, 1202, 1203, 1204, 1207, 1209
G_DUR, G_LIST, G_INFO, G_EXPORT, G_IMPORT = 1210, 1212, 1217, 1218, 1219

# 邮件页
M_ATYPE, M_AID, M_HINT = 1307, 1308, 1312

# 任务页
QUEST_LIST, QUEST_REFRESH, QUEST_INFO = 1601, 1602, 1604
QUEST_DROP, QUEST_DONE = 1605, 1606

# 分类浏览窗口（弹窗自己的号段）
B_KW, B_LIST, B_CNT, B_MSG = 3001, 3002, 3003, 3004
B_ADD, B_SEL, B_CLR = 3005, 3007, 3008

# GM 页
G_SCANBAD, G_ACCM = 1827, 1809
S_LIST, S_RESCAN = 6000, 6003
SCAN_TITLE, SCAN_STUCK, SCAN_CONFIRM = "全服清错误物品", "上一轮还没跑完", "扫描全服所有角色"

# 表
CHARAC_INFO = "taiwan_cain.charac_info"
LOGIN_ACCOUNT = "taiwan_login.login_account_3"
QUEST_TABLE = "taiwan_cain.new_charac_quest"
CARGO_TABLE = "taiwan_cain.account_cargo"
CREATURE_TABLE = "taiwan_cain_web.dnf_creature_info"
INVENTORY = "taiwan_cain_2nd.inventory"

WM_KILLFOCUS, WM_CLOSE, WM_KEYDOWN, WM_KEYUP = 0x0008, 0x0010, 0x0100, 0x0101
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK = 0x0201, 0x0202, 0x0203
BM_CLICK, VK_RETURN, VK_DOWN = 0x00F5, 13, 0x28
GW_OWNER = 4
GWL_STYLE = -16
ES_READONLY = 0x0800
LVS_OWNERDATA = 0x1000
LVM_GETHEADER, LVM_GETNEXTITEM, LVNI_SELECTED = 0x101F, 0x100C, 0x0002
IDOK, IDCANCEL = 1, 2

# 类型文字（与 app_item_type_text 同一套叫法）
TYPE_EMPTY, TYPE_UNKNOWN = "消耗品(默认)", "消耗品(未知编号)"
# 这两类的类型字节是装备（时装同样占装备格），数量位不是堆叠数，所以看耐久
TYPE_GEAR = ("装备", "时装")

# 旧脚本的编号样本：期望类型取自 app_item_type_text。换一份 PVF 可能没有这些编号，
# 那时按 ctx.skip 处理，不当失败。
SAMPLE_TYPES = (("10000", "装备"), ("1232", "消耗品"), ("3012", "材料"),
                ("4066", "任务材料"), ("11907", "时装"))
UNKNOWN_ID = "123456789"
AVATAR_ID = "11907"

# tsv 导入文件：第 1 行表头，第 2 行对照组，第 3 行三项一起越界
TSV_HEAD = "编号\t名称\t数量\t类型\t强化\t增幅\t增幅值\t锻造\t封装\t封印次数\t耐久\r\n"
TSV_OK = "1000\t对照组\t1\t消耗品\t0\t\t0\t0\t否\t0\t0\r\n"
TSV_BAD = "1001\t越界行\t1\t消耗品\t50\t\t0\t9999\t否\t99\t0\r\n"
EXPORT_DIR = os.path.join(core.BUILD, "config", "export")
TSV_IN = os.path.join(EXPORT_DIR, "regress-grant-in.tsv")
TSV_OUT = os.path.join(EXPORT_DIR, "regress-grant-out.tsv")

# 任务槽：20 个槽位 + 各自的触发器
QCOLS = tuple(["play_%d" % i for i in range(1, 21)]
              + ["play_%d_trigger" % i for i in range(1, 21)])
QUEST_ROUNDS = 8

# ---------------------------------------------------------------- Win32 声明
# 句柄按 64 位取，否则句柄一大就截断
u.GetWindowLongW.restype = ctypes.c_long
u.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u.GetWindowLongPtrW.restype = ctypes.c_longlong
u.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindow.restype = wt.HWND
u.GetWindow.argtypes = [wt.HWND, wt.UINT]
u.GetParent.restype = wt.HWND
u.GetParent.argtypes = [wt.HWND]
u.IsWindowEnabled.argtypes = [wt.HWND]
u.IsWindow.argtypes = [wt.HWND]


# ---------------------------------------------------------------- 界面小工具
def _rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def _row_y(lv, idx=0, rowh=20):
    """列表第 idx 行的客户区 y：从列头底边往下算，LVS_REPORT 的表头占住顶部。"""
    y = 12
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    if hd:
        y = (_rect(hd).bottom - _rect(lv).top) + 8
    return max(2, y + idx * rowh)


def _status(ctx, cap=512):
    return core.txt(u.GetDlgItem(ctx.main, IDC_STATUS), cap)


def _enabled(h):
    return bool(h) and bool(u.IsWindowEnabled(h))


def _readonly(h):
    return bool(h) and bool(u.GetWindowLongW(h, GWL_STYLE) & ES_READONLY)


def _title(h):
    b = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, b, 256)
    return b.value


def _tops(cls, pid=None):
    """顶层可见窗口，按类名与进程过滤（其它实例弹的框不能算进来）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h) or core.cls_of(h) != cls:
            return True
        if pid is not None:
            w = wt.DWORD(0)
            u.GetWindowThreadProcessId(h, ctypes.byref(w))
            if w.value != pid:
                return True
        out.append(h)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _owned_tops(main, cls=None, title=None):
    """主窗口名下的顶层窗口：确认框（#32770）与结果窗（DnfDlg）。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h):
            return True
        if cls and core.cls_of(h) != cls:
            return True
        if title and _title(h) != title:
            return True
        # 只认主窗口名下的：别人的对话框不能当成工具弹的框
        if u.GetWindow(h, GW_OWNER) not in (main, 0) and u.GetParent(h) != main:
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
    """等本进程弹的提示/确认框，返回 (hwnd, 全部控件文字)。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        hs = _tops("#32770", ctx.proc.pid)
        if hs:
            time.sleep(0.2)
            return hs[0], _dlg_text(hs[0])
        time.sleep(0.05)
    return None, ""


def _click_box(ctx, h, cid=IDOK, timeout=8.0):
    """点掉提示框（只有「确定」的框也能关掉），并等它真的消失。"""
    b = u.GetDlgItem(h, cid) or u.GetDlgItem(h, IDOK)
    if b:
        core.press(b, 0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not _tops("#32770", ctx.proc.pid):
            return True
        time.sleep(0.05)
    return False


def _confirm_ok(ctx, timeout=8.0):
    """等确认框出现并点「确定」，返回框里的文字（没框返回空串）。"""
    h, t = _wait_box(ctx, timeout)
    if not h:
        return ""
    _click_box(ctx, h, IDOK)
    return t


def _select_char(ctx, timeout=25.0):
    """选中左栏第一行（列表按 charac_no 降序，第一行就是编号最大的角色）。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not core.wait_until(lambda: core.rows(lv) > 0, timeout, 0.2):
        return False
    core.click_at(lv, 40, _row_y(lv, 0, 20), 1.0)
    return core.wait_until(lambda: "已载入" in _status(ctx), 15.0, 0.3)


def _page(ctx, idx, wait=0.8):
    ctx.page(idx)
    time.sleep(wait)


def _until(fn, want, timeout=10.0, step=0.2):
    """等取值函数返回 want（写库/读库都是后台作业，提交不等于落库）。"""
    got = fn()
    t0 = time.time()
    while got != want and time.time() - t0 < timeout:
        time.sleep(step)
        got = fn()
    return got


def _text_until(h, want, timeout=5.0):
    got = core.txt(h, 64)
    t0 = time.time()
    while got != want and time.time() - t0 < timeout:
        time.sleep(0.15)
        got = core.txt(h, 64)
    return got


def _wait_rows(ctx, cid, want, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = ctx.ctl(cid)
        if h and core.rows(h) == want:
            return True
        time.sleep(0.3)
    return False


def _pvf_ready(ctx, cid, timeout=60.0):
    """查询页的分类按钮可用 = PVF 已加载（页面 lock 跟着 pvf_ok 变）。"""
    return core.wait_until(lambda: _enabled(ctx.ctl(cid)), timeout, 0.5)


def _offline(ctx, mid):
    """GM 写入口在角色在线时按设计拒绝写入，那样验不到写库。"""
    try:
        st = ctx.q1("SELECT MAX(login_status) FROM %s WHERE m_id=%%s" % LOGIN_ACCOUNT, (mid,))
    except Exception:
        return True                     # 没有登录表就当离线，交给用例本身去判
    if st:
        ctx.skip("前置不满足：角色在线（login_status=%s），GM 写库入口拒绝写入" % st)
        return False
    return True


# ---------------------------------------------------------------- 日志与文件
def _log_file():
    return os.path.join(core.BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))


def _log_mark():
    p = _log_file()
    return (p, os.path.getsize(p)) if os.path.exists(p) else (p, 0)


def _log_since(mark):
    p, off = mark
    if not os.path.exists(p):
        return ""
    with open(p, "rb") as f:
        f.seek(off)
        return f.read().decode("utf-8", "replace")


def _wait_log(mark, needles, timeout=20.0):
    """等一行同时含 needles 里全部关键字的新日志（日志每秒落一次盘）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for line in _log_since(mark).splitlines():
            if all(k in line for k in needles):
                return line.strip()
        time.sleep(0.3)
    return ""


def _edits(h):
    """对话框里所有 Edit，含「另存为」那个 ComboBoxEx 里内嵌的（深度不定）。"""
    return [c for c in core.kids(h) if core.cls_of(c) == "Edit"]


def _file_dialog(ctx, path, timeout=20.0):
    """公用打开/另存为对话框出现后，把完整路径填进去并确定。"""
    d = None
    t0 = time.time()
    while time.time() - t0 < timeout:
        for h in _tops("#32770", ctx.proc.pid):
            d = h
            break
        if d:
            break
        time.sleep(0.1)
    if not d:
        return None
    time.sleep(0.3)
    edits = _edits(d)
    if not edits:
        return None
    for e in edits:
        core.set_text(e, path)
    time.sleep(0.3)
    for e in edits:                     # 回车确认，再用 IDOK 兜底
        u.PostMessageW(e, WM_KEYDOWN, VK_RETURN, 0)
        u.PostMessageW(e, WM_KEYUP, VK_RETURN, 0)
    time.sleep(0.5)
    ok = u.GetDlgItem(d, IDOK)
    if ok and u.IsWindow(d):
        core.press(ok, 0)
    time.sleep(1.2)
    return d


def _wait_file(path, timeout=15.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return True
        time.sleep(0.2)
    return False


def _wait_export(timeout=15.0):
    """导出目录里最新的一份 tsv：对话框可能用默认名落盘。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            files = [os.path.join(EXPORT_DIR, f) for f in os.listdir(EXPORT_DIR)
                     if f.endswith(".tsv") and os.path.getsize(os.path.join(EXPORT_DIR, f)) > 0]
        except OSError:
            files = []
        if files:
            return max(files, key=os.path.getmtime)
        time.sleep(0.2)
    return None


def _head_lines(path, n=2):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.rstrip("\r\n") for l in f.read().split("\n")[:n] if l.strip()]


def _prep_tsv():
    """造导入文件，并清掉上次留下的导出文件。"""
    os.makedirs(EXPORT_DIR, exist_ok=True)
    _drop_tsv()
    with open(TSV_IN, "w", encoding="utf-8", newline="") as f:
        f.write(TSV_HEAD + TSV_OK + TSV_BAD)


def _drop_tsv():
    for p in (TSV_IN, TSV_OUT):
        try:
            if os.path.exists(p):
                os.remove(p)
        except Exception:
            pass


# ---------------------------------------------------------------- 公用：连库/选角
def _connected(ctx, timeout=25.0):
    """连库完成：左栏角色列表已经有行。"""
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    return core.wait_until(lambda: core.rows(lv) > 0, timeout, 0.2)


# ================================================================ 类型由 PVF 判定
@core.case("item.type_grant", needs=("gui", "db", "pvf"), group="manual",
           desc="发放页类型格只读、改编号就按 PVF 重判，编号 0/查不到时兜成消耗品")
def type_grant(ctx):
    """验：类型格不可手改；判定结果与「数量/耐久」的可用状态一致（装备看耐久）。"""
    if not ctx.ok("选中第一个角色", _select_char(ctx)):
        return
    _page(ctx, PAGE_GRANT)
    e_id, e_type = ctx.ctl(G_ID), ctx.ctl(G_TYPE)
    e_num, e_dur = ctx.ctl(G_NUM), ctx.ctl(G_DUR)
    if not ctx.ok("编号/类型/数量/耐久四个框都在", all([e_id, e_type, e_num, e_dur])):
        return
    ctx.check("类型格只读（能改的只有编号）", _readonly(e_type), True)

    # 编号 0：没有可判的东西，兜成「消耗品(默认)」，按堆叠物处理
    core.set_text(e_id, "0")
    ctx.check("编号 0 兜成消耗品(默认)", _text_until(e_type, TYPE_EMPTY), TYPE_EMPTY)
    ctx.check("兜底时数量可用、耐久不可用",
              (_enabled(e_num), _enabled(e_dur)), (True, False))

    # PVF 里查不到的编号：仍是消耗品兜底，但文案点明「未知编号」
    core.set_text(e_id, UNKNOWN_ID)
    ctx.check("查不到的编号兜成消耗品(未知编号)",
              _text_until(e_type, TYPE_UNKNOWN), TYPE_UNKNOWN)

    # 样本编号：类型必须与数量/耐久的可用状态一致
    for val, want in SAMPLE_TYPES:
        core.set_text(e_id, val)
        got = core.txt(e_type, 64)
        if got in (TYPE_EMPTY, TYPE_UNKNOWN):
            ctx.skip("这份 PVF 里没有编号 %s（类型兜成 %s），跳过该样本" % (val, got))
            continue
        ctx.check("编号 %s → %s" % (val, want), got, want)
        gear = got in TYPE_GEAR
        ctx.check("类型 %r 与数量/耐久联动" % got,
                  (_enabled(e_num), _enabled(e_dur)), (not gear, gear))


@core.case("item.type_mail", needs=("gui", "db", "pvf"), group="manual",
           desc="邮件页附件类型只读：留空/查不到算普通物品，时装与库宠物编号各换类型与标签")
def type_mail(ctx):
    """验：附件类型格跟着附件编号走，左边那个叫法标签一起变（物品/时装/宠物 ID）。"""
    if not ctx.ok("选中第一个角色", _select_char(ctx)):
        return
    _page(ctx, PAGE_MAIL)
    e_id, e_type, e_hint = ctx.ctl(M_AID), ctx.ctl(M_ATYPE), ctx.ctl(M_HINT)
    if not ctx.ok("附件编号/类型/标签三个控件都在", all([e_id, e_type, e_hint])):
        return
    ctx.check("附件类型格只读", _readonly(e_type), True)

    core.set_text(e_id, "")
    ctx.check("附件编号留空 → 普通物品", _text_until(e_type, "普通物品"), "普通物品")
    ctx.check("留空时标签是「物品 ID」", core.txt(e_hint, 64), "物品 ID")

    core.set_text(e_id, UNKNOWN_ID)
    ctx.check("查不到的编号 → 普通物品", _text_until(e_type, "普通物品"), "普通物品")
    ctx.check("查不到的编号标签仍是「物品 ID」", core.txt(e_hint, 64), "物品 ID")

    core.set_text(e_id, AVATAR_ID)
    got = core.txt(e_type, 64)
    if got == "时装":
        ctx.check("时装编号 → 时装", got, "时装")
        ctx.check("时装编号的标签换成「时装 ID」", core.txt(e_hint, 64), "时装 ID")
    else:
        ctx.skip("这份 PVF 里 %s 不是时装编号（类型 %r），跳过时装分支" % (AVATAR_ID, got))

    try:
        pet = ctx.q1("SELECT it_id FROM %s WHERE it_id>0 LIMIT 1" % CREATURE_TABLE)
    except Exception as e:
        ctx.skip("宠物清单表不可用（%s），跳过宠物分支" % e)
        pet = None
    if pet:
        core.set_text(e_id, str(pet))
        ctx.check("宠物表编号 → 宠物", _text_until(e_type, "宠物"), "宠物")
        ctx.check("宠物编号的标签换成「宠物 ID」", core.txt(e_hint, 64), "宠物 ID")
    elif pet is not None:
        ctx.skip("库里没有宠物清单（dnf_creature_info 空），跳过宠物分支")


# ================================================================ 任务材料的数量随行进队列
def _load_task_mats(ctx, timeout=40.0):
    """点「加载任务材料」，等列表出来，返回行数。"""
    _page(ctx, PAGE_QUERY)
    if not ctx.ok("查询页「加载任务材料」按钮可用（PVF 已就绪）", _pvf_ready(ctx, Q_TASK)):
        return -1
    lv = ctx.ctl(Q_LIST)
    if not ctx.ok("查询页结果列表存在", bool(lv), "控件 %d" % Q_LIST):
        return -1
    core.press(ctx.ctl(Q_TASK), 0)
    core.wait_until(lambda: core.rows(lv) > 0, timeout, 0.5)
    return core.rows(lv)


@core.case("item.tasknum_load", needs=("gui", "db", "pvf"), group="always",
           desc="加载任务材料：列表条数与说明文案、状态栏三处对得上，数量列写明要几个")
def tasknum_load(ctx):
    """验：材料条数是真实的数，说明与状态栏都引用同一个数。"""
    n = _load_task_mats(ctx)
    if n < 0:
        return
    if n == 0:
        ctx.skip("这份 PVF 里没有收集类任务要的材料（列表 0 行）")
        return
    ctx.check("说明文案的条数与列表行数一致",
              core.txt(ctx.ctl(Q_INFO), 128), "任务材料 %d 条 · 数量随行进队列" % n)
    ctx.ok("状态栏写出任务的材料种数",
           core.wait_until(lambda: ("任务材料：%d 种" % n) in _status(ctx), 10.0, 0.3),
           _status(ctx))


@core.case("item.tasknum_queue", needs=("gui", "db", "pvf"), group="manual",
           desc="双击任务材料首行入队时带上该行数量，全部加入后队列行数等于材料条数")
def tasknum_queue(ctx):
    """验：入队数量来自「这道工序要几个」，不是清一色 1；重复加入不重复加行。"""
    if not ctx.ok("选中第一个角色", _select_char(ctx)):   # 「全部加入」要先有目标角色
        return
    n = _load_task_mats(ctx)
    if n < 0:
        return
    if n == 0:
        ctx.skip("这份 PVF 里没有收集类任务要的材料（列表 0 行）")
        return
    lv = ctx.ctl(Q_LIST)
    y = _row_y(lv, 0)
    _dblclick(lv, 40, y)

    core.wait_until(lambda: "已加入队列" in _status(ctx), 10.0, 0.2)
    m = re.search(r"×(\d+)\s*$", _status(ctx))
    num = int(m.group(1)) if m else None
    ctx.ok("双击首行让状态栏报出入队数量", num is not None, _status(ctx))
    if num is None:
        return
    if num <= 1:
        ctx.skip("这份 PVF 里首个任务材料只要 1 个，看不出「数量随行」")
        return

    _page(ctx, PAGE_GRANT)
    gl = ctx.ctl(G_LIST)
    if not ctx.ok("发放页队列列表存在", bool(gl), "控件 %d" % G_LIST):
        return
    ctx.check("双击只入队 1 行", core.rows(gl), 1)
    if not ctx.ok("点队列首行把该项载入表单", _pick_grant_row(ctx, 0)):
        return
    ctx.check("队列里的数量就是状态栏报的那个数", core.txt(ctx.ctl(G_NUM), 32), str(num))

    # 全部加入：队列里已有的只补数量，不重复加行
    _page(ctx, PAGE_QUERY)
    core.press(ctx.ctl(Q_ALL), 0)
    time.sleep(1.5)
    _page(ctx, PAGE_GRANT)
    gl = ctx.ctl(G_LIST)
    ctx.check("全部加入后队列行数 = 材料条数", core.rows(gl), n)
    core.click_at(gl, 40, _row_y(gl, 0), 0.8)
    ctx.check("全部加入没有压小已有行的数量", core.txt(ctx.ctl(G_NUM), 32), str(num))


# ================================================================ 分类浏览窗口
def _dblclick(lv, x, y):
    """双击列表的 (x,y)：NM_DBLCLK 只认 WM_LBUTTONDBLCLK。"""
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    u.PostMessageW(lv, WM_LBUTTONDOWN, 1, lp)
    u.PostMessageW(lv, WM_LBUTTONUP, 0, lp)
    time.sleep(0.05)
    u.PostMessageW(lv, WM_LBUTTONDBLCLK, 1, lp)
    u.PostMessageW(lv, WM_LBUTTONUP, 0, lp)


def _browse_dlg(pid):
    for h in _tops("DnfDlg", pid):
        if _title(h).startswith("浏览"):
            return h
    return None


def _open_browse(ctx, cid, timeout=30.0):
    """点查询页的分类按钮，等模态浏览窗出现（标题以「浏览」开头）。"""
    h = ctx.ctl(cid)
    if not h:
        return None
    core.press(h, 0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        d = _browse_dlg(ctx.proc.pid)
        if d:
            time.sleep(0.8)
            return d
        time.sleep(0.2)
    return None


def _close_dlg(ctx, d, timeout=10.0):
    u.PostMessageW(d, WM_CLOSE, 0, 0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not u.IsWindow(d) or d not in _tops("DnfDlg", ctx.proc.pid):
            return True
        time.sleep(0.1)
    return False


def _count_of(d):
    """浏览窗计数文本里的 (总数, 命中数, 原文)；没有「共 N 条」时总数为 None。"""
    t = core.txt(u.GetDlgItem(d, B_CNT), 256)
    mt = re.search(r"共 (\d+) 条", t)
    mh = re.search(r"命中 (\d+) 条", t)
    return (int(mt.group(1)) if mt else None,
            int(mh.group(1)) if mh else None, t)


@core.case("item.browse_open", needs=("gui", "db", "pvf"), group="always",
           desc="五个分类浏览窗各自开得出来：标题、虚拟列表、计数文本与行数一致")
def browse_open(ctx):
    """验：浏览窗按分类开、列表是虚拟列表、行数与「共 N 条」是同一个数。"""
    _page(ctx, PAGE_QUERY)
    if not ctx.ok("查询页分类按钮可用（PVF 已就绪）", _pvf_ready(ctx, Q_BEQU)):
        return
    for label, cid in (("材料", Q_BMAT), ("消耗品", Q_BSTK), ("装备", Q_BEQU),
                       ("宠物", Q_BPET), ("时装", Q_BAVA)):
        if not ctx.ok("%s按钮存在" % label, bool(ctx.ctl(cid)), "控件 %d" % cid):
            continue
        d = _open_browse(ctx, cid)
        if not ctx.ok("%s：浏览窗开出来了" % label, d is not None):
            continue
        try:
            ctx.ok("%s：标题点明分类" % label, _title(d).startswith("浏览" + label), _title(d))
            lv = u.GetDlgItem(d, B_LIST)
            if not ctx.ok("%s：列表存在" % label, bool(lv), "控件 %d" % B_LIST):
                continue
            style = u.GetWindowLongPtrW(lv, GWL_STYLE) & 0xFFFFFFFF
            ctx.ok("%s：列表是虚拟列表（LVS_OWNERDATA）" % label,
                   bool(style & LVS_OWNERDATA), "style=0x%08x" % style)
            n = core.rows(lv)
            total, _hit, text = _count_of(d)
            if total is None or n <= 0:
                ctx.skip("%s：这一分类当前没有可列条目（%r）" % (label, text))
                continue
            ctx.check("%s：行数 = 计数里的总数" % label, n, total)
            ctx.ok("%s：计数文本点名分类" % label, text.startswith(label), text)
        finally:
            ctx.ok("%s：浏览窗关得掉" % label, _close_dlg(ctx, d))


@core.case("item.browse_filter", needs=("gui", "db", "pvf"), group="always",
           desc="浏览窗边打边筛：命中数与列表行数一致，筛不中明确提示，清空回到全量")
def browse_filter(ctx):
    """验：筛选只是收窄视野，行数与计数文本始终一致；清空关键词能回到全量。"""
    _page(ctx, PAGE_QUERY)
    if not ctx.ok("查询页分类按钮可用（PVF 已就绪）", _pvf_ready(ctx, Q_BEQU)):
        return
    for label, cid, kw in (("材料", Q_BMAT, "硬化"), ("装备", Q_BEQU, "剑"),
                           ("时装", Q_BAVA, "套装")):
        d = _open_browse(ctx, cid)
        if not ctx.ok("%s：浏览窗开出来了" % label, d is not None):
            continue
        try:
            lv = u.GetDlgItem(d, B_LIST)
            e_kw = u.GetDlgItem(d, B_KW)
            if not ctx.ok("%s：列表与筛选框都在" % label, bool(lv) and bool(e_kw)):
                continue
            total, _hit, text = _count_of(d)
            if not total:
                ctx.skip("%s：这一分类当前没有可列条目（%r）" % (label, text))
                continue

            core.set_text(e_kw, kw)
            core.wait_until(lambda: "命中" in core.txt(u.GetDlgItem(d, B_CNT), 256),
                            10.0, 0.2)
            rows = core.rows(lv)
            total2, hit, text = _count_of(d)
            msg = core.txt(u.GetDlgItem(d, B_MSG), 256)
            ctx.check("%s：筛选不改变总数" % label, total2, total)
            if hit == 0:
                ctx.ok("%s：筛「%s」没命中时列表为空并提示换关键词" % (label, kw),
                       rows == 0 and "没有命中" in msg, "行数 %d · %s" % (rows, msg))
            else:
                ctx.check("%s：筛「%s」命中数与列表行数一致" % (label, kw), rows, hit)
                ctx.ok("%s：命中数不超过总数" % label, 0 < hit <= total,
                       "命中 %d / 共 %d" % (hit, total))

            core.press(u.GetDlgItem(d, B_CLR), 0)
            core.wait_until(lambda: "命中" not in core.txt(u.GetDlgItem(d, B_CNT), 256),
                            10.0, 0.2)
            ctx.check("%s：清空关键词后行数回到全量" % label, core.rows(lv), total)
        finally:
            ctx.ok("%s：浏览窗关得掉" % label, _close_dlg(ctx, d))


@core.case("item.browse_add", needs=("gui", "db", "pvf"), group="manual",
           desc="装备浏览窗筛「剑」→ 全选 → 加入所选：入队项数与选中行数一致，队列跟着涨")
def browse_add(ctx):
    """验：全选真的选中全部可见行；「加入所选」把每一项都加到队列里，一项不丢。"""
    if not ctx.ok("选中第一个角色", _select_char(ctx)):   # 发放页要有目标角色才填说明
        return
    _page(ctx, PAGE_GRANT)
    gl = ctx.ctl(G_LIST)
    if not ctx.ok("发放页队列列表存在", bool(gl), "控件 %d" % G_LIST):
        return
    ctx.check("起点：队列是空的", core.rows(gl), 0)

    _page(ctx, PAGE_QUERY)
    if not ctx.ok("装备浏览按钮可用（PVF 已就绪）", _pvf_ready(ctx, Q_BEQU)):
        return
    d = _open_browse(ctx, Q_BEQU)
    if not ctx.ok("装备浏览窗开出来了", d is not None):
        return
    try:
        lv = u.GetDlgItem(d, B_LIST)
        e_kw = u.GetDlgItem(d, B_KW)
        if not ctx.ok("列表与筛选框都在", bool(lv) and bool(e_kw)):
            return
        core.set_text(e_kw, "剑")
        core.wait_until(lambda: "命中" in core.txt(u.GetDlgItem(d, B_CNT), 256), 10.0, 0.2)
        n = core.rows(lv)
        _total, _hit, text = _count_of(d)
        if n <= 0:
            ctx.skip("这份 PVF 里「剑」没有命中（%s），无从验证入队" % text)
            return
        if n > 3000:
            ctx.skip("「剑」命中 %d 行，超过一次加入上限 3000，先缩小关键词再验" % n)
            return

        core.press(u.GetDlgItem(d, B_SEL), 0.8)
        msg = core.txt(u.GetDlgItem(d, B_MSG), 256)
        ctx.ok("全选选中了全部 %d 行" % n, ("已选中 %d 行" % n) in msg, msg)
        core.press(u.GetDlgItem(d, B_ADD), 0)
        if not ctx.ok("加入所选后给出结果",
                      core.wait_until(lambda: "已加入队列" in core.txt(
                          u.GetDlgItem(d, B_MSG), 256), 20.0, 0.2),
                      core.txt(u.GetDlgItem(d, B_MSG), 256)):
            return
        msg = core.txt(u.GetDlgItem(d, B_MSG), 256)
        m = re.search(r"新增 (\d+) 项 · 数量补大 (\d+) 项 · 队列里已有 (\d+) 项", msg)
        if not ctx.ok("结果文案报出新增/补大/已有三项", m is not None, msg):
            return
        add, upd, dup = (int(x) for x in m.groups())
        ctx.check("新增项数 = 选中行数", add, n)
        ctx.check("三项之和 = 选中行数", add + upd + dup, n)
        ctx.ok("状态栏报出从浏览窗口加入的项数",
               ("从浏览窗口加入 %d 项" % n) in _status(ctx), _status(ctx))
    finally:
        ctx.ok("浏览窗关得掉", _close_dlg(ctx, d))

    _page(ctx, PAGE_GRANT)
    gl = ctx.ctl(G_LIST)
    ctx.check("发放页队列行数 = 加入的项数", core.rows(gl), n)
    info = core.txt(ctx.ctl(G_INFO), 256)
    ctx.ok("发放页说明里的队列项数与列表一致", ("队列 %d 项" % n) in info, info)


# ================================================================ 发放队列 tsv
def _pick_grant_row(ctx, idx, want_id=None, timeout=10.0):
    """点队列第 idx 行，确认表单里载入的编号是 want_id（行高随 DPI 变，按结果校正）。

    want_id 为 None 时只要求表单载入了一个非 0 的编号。
    """
    lv = ctx.ctl(G_LIST)
    if not lv:
        return False
    base = _row_y(lv, 0)
    t0 = time.time()
    for dy in (0, 4, 8, 12, -4):
        core.click_at(lv, 40, max(2, base + idx * 20 + dy), 0.6)
        got = core.txt(ctx.ctl(G_ID), 32)
        if (want_id is None and got not in ("", "0")) or got == str(want_id):
            return True
        if time.time() - t0 > timeout:
            break
    return False


@core.case("item.tsv_clamp", needs=("gui", "db"), group="always",
           desc="导入越界的队列 tsv：强化/锻造/封印次数都夹到界上，日志逐行报出、队列里读回夹后值")
def tsv_clamp(ctx):
    """验：越界值不是静默截断（50&0x1F=18 那种），而是夹到字段宽度上限并留一行日志。"""
    _prep_tsv()
    mark = _log_mark()
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx)):
            return
        _page(ctx, PAGE_GRANT)
        gl = ctx.ctl(G_LIST)
        if not ctx.ok("发放页队列列表存在", bool(gl), "控件 %d" % G_LIST):
            return
        ctx.check("起点：队列是空的", core.rows(gl), 0)
        h = ctx.ctl(G_IMPORT)
        if not ctx.ok("「导入」按钮可用", _enabled(h), "控件 %d" % G_IMPORT):
            return

        core.press(h, 0)
        if not ctx.ok("导入：路径已填进公用文件对话框",
                      _file_dialog(ctx, TSV_IN) is not None):
            return
        ctx.check("导入后队列 2 行", _until(lambda: core.rows(ctx.ctl(G_LIST)), 2, 20.0), 2)

        # 日志的物理行号：第 1 行表头、第 2 行对照组、第 3 行越界行
        for want in ("强化 50 已夹到 31", "锻造 9999 已夹到 255", "封印次数 99 已夹到 7"):
            line = _wait_log(mark, ["第 3 行的" + want], 20.0)
            ctx.ok("日志有反馈：%s" % want, bool(line),
                   line or _log_since(mark)[-200:])

        # 选中越界行，从表单读回夹后的值：表单就是 Persistent 下来要写物品二进制的字段
        if not ctx.ok("能选中队列第 2 行", _pick_grant_row(ctx, 1, 1001)):
            return
        for name, cid, want in (("编号", G_ID, "1001"), ("数量", G_NUM, "1"),
                                ("强化", G_ENH, "31"), ("锻造", G_FORGE, "255"),
                                ("封印次数", G_SEALC, "7")):
            ctx.check("表单里的%s = %s" % (name, want), core.txt(ctx.ctl(cid), 32), want)
    finally:
        _drop_tsv()                      # 队列只在内存里，落盘的只有这两个文件


@core.case("item.tsv_head", needs=("gui", "db"), group="always",
           desc="导出的队列 tsv 头两行是 # 说明（写明各数值列范围），且再导入不会多出行")
def tsv_head(ctx):
    """验：导出头把越界策略与各列范围写在文件里，导入方跳过 # 与表头。"""
    _prep_tsv()
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx)):
            return
        _page(ctx, PAGE_GRANT)
        if not ctx.ok("队列列表存在", bool(ctx.ctl(G_LIST)), "控件 %d" % G_LIST):
            return
        # 先导入，让队列里有内容（导出空队列只弹提示、不出文件）
        core.press(ctx.ctl(G_IMPORT), 0)
        if not ctx.ok("导入：路径已填进对话框", _file_dialog(ctx, TSV_IN) is not None):
            return
        if not ctx.check("导入后队列 2 行",
                         _until(lambda: core.rows(ctx.ctl(G_LIST)), 2, 20.0), 2):
            return

        core.press(ctx.ctl(G_EXPORT), 0)
        if not ctx.ok("导出：另存为对话框已出现", _file_dialog(ctx, TSV_OUT) is not None):
            return
        # 对话框会把默认名（grant-<时间戳>.tsv）留在 lpstrFile 里，脚本填的路径
        # 未必被采纳；以「导出目录里最新的一份」为准。
        got_file = _wait_export(15.0)
        if not ctx.ok("导出文件已生成", got_file is not None, TSV_OUT):
            return
        head = _head_lines(got_file, 2)
        ctx.ok("导出头两行都是 # 说明",
               len(head) == 2 and all(l.startswith("#") for l in head),
               head[0][:60] if head else "(空)")
        ctx.ok("说明行写明各数值列的上下限",
               len(head) > 1 and all(x in head[1] for x in
                                     ("强化 0~31", "锻造 0~255", "封印次数 0~7")),
               head[1][:140] if len(head) > 1 else "(没有第二行)")

        # 再导入导出的文件：# 与表头都被跳过，只多出 2 行
        core.press(ctx.ctl(G_IMPORT), 0)
        if not ctx.ok("再导入：路径已填进对话框", _file_dialog(ctx, got_file) is not None):
            return
        ctx.check("导出的文件再导入只多出 2 行（#/表头被跳过）",
                  _until(lambda: core.rows(ctx.ctl(G_LIST)), 4, 20.0), 4)
    finally:
        _drop_tsv()


# ================================================================ 全服扫描闸
def _wait_scan_dlg(ctx, timeout=60.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        w = _owned_tops(ctx.main, "DnfDlg", SCAN_TITLE)
        if w:
            return w[0]
        time.sleep(0.1)
    return None


def _close_scan(ctx, d, timeout=6.0):
    u.PostMessageW(d, WM_CLOSE, 0, 0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not _owned_tops(ctx.main, "DnfDlg", SCAN_TITLE):
            return True
        time.sleep(0.1)
    return False


def _open_scan(ctx, timeout=60.0):
    """点入口 → 确定 → 等结果窗（扫完才弹）。返回 (窗口, 入口提示文本)。"""
    b = ctx.ctl(G_SCANBAD)
    if b:
        core.press(b, 0)
    h, t = _wait_box(ctx, 8.0)
    if not h or SCAN_CONFIRM not in t:
        if h:
            _click_box(ctx, h, IDCANCEL)
        return None, t
    _click_box(ctx, h, IDOK)
    core.scan_bg(ctx.proc.pid)              # 进度窗先转后台，测试才能继续驱动界面
    return _wait_scan_dlg(ctx, timeout), t


def _probe_entry(ctx, timeout=8.0):
    """点入口读第一屏提示（点取消，不真的开扫）：用来看闸的答复。"""
    b = ctx.ctl(G_SCANBAD)
    if b:
        core.press(b, 0)
    h, t = _wait_box(ctx, timeout)
    if h:
        _click_box(ctx, h, IDCANCEL)
    return t


def _try_scan(ctx):
    """真的走一遍「点入口 → 确定」，返回 (是否扫起来, 被拒原因, 结果窗)。"""
    b = ctx.ctl(G_SCANBAD)
    if b:
        core.press(b, 0)
    h, t = _wait_box(ctx, 8.0)
    if not h:
        return False, t, None
    if SCAN_CONFIRM not in t:               # 入口自己就拒了
        _click_box(ctx, h, IDCANCEL)
        return False, t, None
    _click_box(ctx, h, IDOK)
    core.scan_bg(ctx.proc.pid)              # 进度窗先转后台
    h2, t2 = _wait_box(ctx, 2.0)            # scan_start 拒绝时会紧接着再弹一个
    if h2:
        _click_box(ctx, h2, IDCANCEL)
        return False, t2, None
    return True, "", _wait_scan_dlg(ctx)


def _wait_online(ctx, timeout=20.0):
    """等入口不再回「请先连接数据库」：g.db_ok 比左栏列表晚一拍落地。"""
    t = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = _probe_entry(ctx)
        if "请先连接数据库" not in t:
            return True, t
        time.sleep(0.3)
    return False, t


@contextmanager
def _lock(tables):
    """用另一条连接 LOCK TABLES：作业是否还停在库上由用例控制。"""
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


def _wait_blocked(ctx, marker, timeout=15.0):
    """PROCESSLIST 里出现卡在这张表的 metadata lock 上的语句：作业真的停住了。"""
    state = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for st, info in ctx.q("SELECT STATE,IFNULL(INFO,'') FROM "
                              "information_schema.PROCESSLIST WHERE COMMAND='Query'"):
            state = st or ""
            if marker in (info or "") and "metadata lock" in state:
                return True, state
        time.sleep(0.2)
    return False, state


@core.case("item.scan_gate", needs=("gui", "db"), group="always",
           desc="扫描窗关掉后入口能再用；作业还卡在库上时关窗，入口必须拒「上一轮还没跑完」")
def scan_gate(ctx):
    """验：关窗不等于作业结束——闸看的是作业是否真的收尾。"""
    if not ctx.ok("连库后左栏角色列表有行", _connected(ctx)):
        return
    _page(ctx, PAGE_GM)
    if not ctx.ok("连接就绪（入口不再回「请先连接数据库」）", _wait_online(ctx)[0]):
        return
    if not ctx.ok("「全服清错误物品」按钮存在", bool(ctx.ctl(G_SCANBAD)),
                  "控件 %d" % G_SCANBAD):
        return

    # A 对照：正常扫完一次 → 关窗 → 还能再扫（证明入口本身是好的）
    d, _t = _open_scan(ctx)
    if not ctx.ok("A：扫描结果窗出现", d is not None):
        return
    rows0 = core.rows(u.GetDlgItem(d, S_LIST))
    ctx.ok("A：扫描窗已关闭", _close_scan(ctx, d), "列表 %d 行" % rows0)
    started, why, d2 = _try_scan(ctx)
    ctx.ok("A：关窗后还能再扫（对照组）", started and d2 is not None, why)
    if d2:
        _close_scan(ctx, d2)

    # B 作业还活着：锁住 inventory 拖住扫描，关窗后入口必须拒
    d, _t = _open_scan(ctx)
    if not ctx.ok("B：扫描结果窗再次出现", d is not None):
        return
    with _lock([INVENTORY]):
        u.PostMessageW(u.GetDlgItem(d, S_RESCAN), BM_CLICK, 0, 0)   # 重新扫描（同步）
        blocked, st = _wait_blocked(ctx, INVENTORY)
        ctx.ok("B：扫描作业真的卡在库上（不是靠 sleep 猜的）", blocked, st)
        ctx.ok("B：扫描窗已关闭", _close_scan(ctx, d))
        started, why, _d = _try_scan(ctx)
        ctx.ok("B：作业没结束 → 扫描被拒，提示「上一轮还没跑完」",
               (not started) and SCAN_STUCK in why, why)
    # 放锁：作业收尾，闸由作业自己放下
    started, why, d = _try_scan(ctx)
    ctx.ok("B：作业收尾后能正常再扫", started and d is not None, why)
    if d:
        _close_scan(ctx, d)
    ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))


@core.case("item.scan_flush", needs=("gui", "db"), group="always",
           desc="连接被占住时点的全服扫描只排队：断开即泄洪，重连后入口恢复且没有遗留结果窗")
def scan_flush(ctx):
    """验：排队的动作属于旧连接，断开时被丢掉；重连后闸跟着放下，不会回「上一轮还没跑完」。"""
    if not ctx.ok("连库后左栏角色列表有行", _connected(ctx)):
        return
    _page(ctx, PAGE_GM)
    if not ctx.ok("连接就绪（入口不再回「请先连接数据库」）", _wait_online(ctx)[0]):
        return
    scan_sql = "c.charac_name,c.m_id"      # 扫描第一条 SELECT 的特征片段

    with _lock(["taiwan_cain.charac_info"]):
        busy, st = _wait_blocked(ctx, "taiwan_cain.charac_info")
        if not ctx.ok("连接被占住（轮询卡在表锁上）", busy, st):
            return
        b = ctx.ctl(G_SCANBAD)
        if not ctx.ok("「全服清错误物品」按钮存在", bool(b), "控件 %d" % G_SCANBAD):
            return
        core.press(b, 0)
        h, t = _wait_box(ctx, 8.0)
        if not ctx.ok("入口弹出确认框", SCAN_CONFIRM in t, t):
            if h:
                _click_box(ctx, h, IDCANCEL)
            return
        _click_box(ctx, h, IDOK)
        time.sleep(1.0)
        # 扫起来的话它和轮询一样会卡在 charac_info 上；库里看不到这条语句 = 只是排队
        queued = not any(scan_sql in (info or "") for _s, info in
                         ctx.q("SELECT STATE,IFNULL(INFO,'') FROM "
                               "information_schema.PROCESSLIST WHERE COMMAND='Query'"))
        ctx.ok("扫描只是排进队列，没有起线程", queued)
        ctx.ok("排队期间不弹结果窗", not _owned_tops(ctx.main, "DnfDlg", SCAN_TITLE))
        ctx.ok("进程还活着（无上下文的动作排队不该崩）", ctx.proc.poll() is None,
               "poll=%s" % (ctx.proc.poll(),))
        # 断开要先等作业退出，所以让 UI 线程先进到 stop 那一拍，再由 with 结束放锁
        u.PostMessageW(u.GetDlgItem(ctx.main, IDC_DISC), BM_CLICK, 0, 0)
        time.sleep(0.6)

    off = ""
    t0 = time.time()
    while time.time() - t0 < 20:
        off = _probe_entry(ctx)
        if "请先连接数据库" in off:
            break
        time.sleep(0.3)
    ctx.ok("断开后入口回「请先连接数据库」", "请先连接数据库" in off, off)

    u.PostMessageW(u.GetDlgItem(ctx.main, IDC_CONN), BM_CLICK, 0, 0)
    ctx.ok("重连成功", _connected(ctx))
    _page(ctx, PAGE_GM)
    stray = _owned_tops(ctx.main, "DnfDlg", SCAN_TITLE)
    ctx.ok("重连后没有遗留的扫描结果窗", not stray, str(stray)[:80])
    for h in stray:
        _close_scan(ctx, h)
    ctx.ok("重连后连接就绪", _wait_online(ctx)[0])
    started, why, d = _try_scan(ctx)
    ctx.ok("泄洪后闸已放下（扫描能正常开始）", started and d is not None, why)
    if d:
        _close_scan(ctx, d)
    ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))


# ================================================================ 账号金库无记录
@core.case("item.norow_accm", needs=("gui", "db"), group="always",
           desc="账号金库没有该账号的行时写入：值真的落库（不是报完成却没写）")
def norow_accm(ctx):
    """验：懒建表里写一个不存在的行，界面说完成时库里必须真有这份值。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = ctx.q1("SELECT m_id FROM %s WHERE charac_no=%%s" % CHARAC_INFO, (cno,))
    if not mid:
        ctx.skip("前置不满足：读不到角色归属的 m_id")
        return
    if ctx.q("SELECT money FROM %s WHERE m_id=%%s" % CARGO_TABLE, (mid,)):
        ctx.skip("前置不满足：account_cargo 已有 m_id=%s 的行，构造不出「无记录」" % mid)
        return
    try:
        if not _offline(ctx, mid):
            return
        if not ctx.ok("选中第一个角色", _select_char(ctx)):
            return
        _page(ctx, PAGE_GM)
        box = ctx.ctl(G_ACCM)
        if not ctx.ok("账号金币框存在", bool(box), "控件 %d" % G_ACCM):
            return
        ctx.check("起点：库里没有这个账号的账号金库行",
                  ctx.q1("SELECT COUNT(*) FROM %s WHERE m_id=%%s" % CARGO_TABLE, (mid,)), 0)

        core.set_text(box, "777")
        u.SendMessageW(box, WM_KILLFOCUS, 0, 0)
        got = _until(lambda: ctx.q1("SELECT money FROM %s WHERE m_id=%%s" % CARGO_TABLE,
                                    (mid,)), 777, 20.0)
        ctx.check("无记录时写入会建行并落值（不是报完成却没写）", got, 777)
        ctx.check("提交后框清回 0", _text_until(box, "0"), "0")
    finally:
        # 原来的状态就是「没有行」：把用例可能建出来的那行删掉
        ctx.cur.execute("DELETE FROM %s WHERE m_id=%%s" % CARGO_TABLE, (mid,))
        ctx.check("收尾后该账号仍没有账号金库行",
                  ctx.q1("SELECT COUNT(*) FROM %s WHERE m_id=%%s" % CARGO_TABLE, (mid,)), 0)


# ================================================================ 任务槽位的动作参数
def _quest_row(ctx, cno):
    r = ctx.q("SELECT %s FROM %s WHERE charac_no=%%s" % (",".join(QCOLS), QUEST_TABLE),
              (cno,))
    return r[0] if r else None


def _quest_slots(ctx, cno):
    r = ctx.q("SELECT play_1,play_1_trigger,play_2,play_2_trigger FROM %s "
              "WHERE charac_no=%%s" % QUEST_TABLE, (cno,))
    return r[0] if r else None


def _quest_seed(ctx, cno):
    """清空 20 个任务槽再给 1、2 号槽各放一个：列表恰好 2 行，不依赖库里原本几条。"""
    ctx.cur.execute("UPDATE %s SET %s WHERE charac_no=%%s"
                    % (QUEST_TABLE, ",".join("%s=0" % c for c in QCOLS)), (cno,))
    for slot, qid in ((1, 101), (2, 102)):
        ctx.cur.execute("UPDATE %s SET play_%d=%%s, play_%d_trigger=1 WHERE charac_no=%%s"
                        % (QUEST_TABLE, slot, slot), (qid, cno))


def _quest_restore(ctx, cno, snap):
    sets = ",".join("%s=%%s" % c for c in QCOLS)
    ctx.cur.execute("UPDATE %s SET %s WHERE charac_no=%%s" % (QUEST_TABLE, sets),
                    tuple(snap) + (cno,))


def _sel_row(lv):
    return u.SendMessageW(lv, LVM_GETNEXTITEM, -1, LVNI_SELECTED)


def _pick_row(lv, idx):
    """点中任务列表第 idx 行：行高随 DPI 变，按回读的选中行校正，不行再用方向键走。"""
    if idx == 0 and _sel_row(lv) == 0:
        return True
    base = _row_y(lv, 0)
    for step in (36, 32, 30):
        core.click_at(lv, 60, max(2, base + idx * step), 0.35)
        if _sel_row(lv) == idx:
            return True
    core.click_at(lv, 60, base, 0.3)
    for _ in range(idx):
        u.PostMessageW(lv, WM_KEYDOWN, VK_DOWN, 0)
        u.PostMessageW(lv, WM_KEYUP, VK_DOWN, 0)
        time.sleep(0.2)
    return _sel_row(lv) == idx


def _refresh_quests(ctx, want, timeout=20.0):
    """点「刷新」逼页面重读任务，直到列表恰好 want 行。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if _wait_rows(ctx, QUEST_LIST, want, 1.0):
            return True
        core.press(ctx.ctl(QUEST_REFRESH), 0)
        time.sleep(1.0)
    return _wait_rows(ctx, QUEST_LIST, want, 1.0)


def _quest_prep(ctx):
    """公共前置：快照该角色的 20 个任务槽，返回 (角色号, 快照)；快照为空返回 (None, None)。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return None, None
    snap = _quest_row(ctx, cno)
    if snap is None:
        ctx.skip("前置不满足：new_charac_quest 没有该角色的行")
        return None, None
    return cno, snap


@core.case("item.quest_drop", needs=("gui", "db"), group="always",
           desc="任务页选中第 1 行放弃：只有 play_1 归零，另一行原样不动")
def quest_drop(ctx):
    """验：单行动作只改被选中的那个槽位。"""
    cno, snap = _quest_prep(ctx)
    if cno is None:
        return
    try:
        _quest_seed(ctx, cno)
        if not ctx.ok("选中第一个角色", _select_char(ctx)):
            return
        _page(ctx, PAGE_QUEST, 1.0)
        lv = ctx.ctl(QUEST_LIST)
        if not ctx.ok("任务列表存在", bool(lv), "控件 %d" % QUEST_LIST):
            return
        if not ctx.ok("造两行后任务页显示 2 行", _refresh_quests(ctx, 2)):
            return
        ctx.ok("任务页说明写的是 2 条任务记录",
               core.wait_until(lambda: "共 2 条任务记录" in core.txt(
                   ctx.ctl(QUEST_INFO), 128), 10.0, 0.3),
               core.txt(ctx.ctl(QUEST_INFO), 128))
        if not ctx.ok("能选中第 1 行", _pick_row(lv, 0)):
            return
        core.press(ctx.ctl(QUEST_DROP), 0)
        t = _confirm_ok(ctx)
        ctx.ok("放弃任务弹出确认框", "放弃任务" in t, t[:60])
        got = _until(lambda: _quest_slots(ctx, cno), (0, 0, 102, 1), 20.0)
        ctx.check("被放弃那一行归零", got[:2], (0, 0))
        ctx.check("另一行原样不动", got[2:], (102, 1))
    finally:
        _quest_restore(ctx, cno, snap)
        ctx.check("任务槽已按快照还原", _quest_row(ctx, cno), snap)


@core.case("item.quest_two_rows", needs=("gui", "db"), group="always",
           desc="8 轮连点两行不同动作：标记完成与放弃各按自己选中的那一行执行")
def quest_two_rows(ctx):
    """验：动作参数跟着选中的行走，不会串成同一份（串了第 1 行就不会被放弃）。"""
    cno, snap = _quest_prep(ctx)
    if cno is None:
        return
    bad = 0
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx)):
            return
        _page(ctx, PAGE_QUEST, 1.0)
        if not ctx.ok("任务列表存在", bool(ctx.ctl(QUEST_LIST)),
                      "控件 %d" % QUEST_LIST):
            return
        for r in range(QUEST_ROUNDS):
            _quest_seed(ctx, cno)
            if not _refresh_quests(ctx, 2):
                ctx.skip("第 %d 轮：任务页没刷出 2 行" % (r + 1))
                return
            lv = ctx.ctl(QUEST_LIST)
            # 先选第 2 行标记完成，紧接着选第 1 行放弃
            if not _pick_row(lv, 1):
                ctx.skip("第 %d 轮：选不中第 2 行" % (r + 1))
                return
            core.press(ctx.ctl(QUEST_DONE), 0)
            _confirm_ok(ctx)
            if not _wait_rows(ctx, QUEST_LIST, 2, 15.0):
                ctx.skip("第 %d 轮：标记完成后列表没回到 2 行" % (r + 1))
                return
            if not _pick_row(lv, 0):
                ctx.skip("第 %d 轮：选不中第 1 行" % (r + 1))
                return
            core.press(ctx.ctl(QUEST_DROP), 0)
            _confirm_ok(ctx)
            got = _until(lambda: _quest_slots(ctx, cno), (0, 0, 102, 0), 20.0)
            if got[:2] != (0, 0) or got[2:] != (102, 0):
                bad += 1
        ctx.ok("%d 轮里每一轮都按各自选中的行执行" % QUEST_ROUNDS, bad == 0,
               "错 %d 轮" % bad)
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        _quest_restore(ctx, cno, snap)
        ctx.check("任务槽已按快照还原", _quest_row(ctx, cno), snap)
