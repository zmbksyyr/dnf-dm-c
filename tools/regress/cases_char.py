# -*- coding: utf-8 -*-
"""角色回归用例：改名（字符集 / 重名 / 4 字节字符 / 被拒后回填）、克隆（键策略）、
改等级/职业被库拒绝后的快照回滚。

每个用例只做自己那一段操作：动库前先取快照，finally 里按原值写回；临时角色用
自增计数器之下的空号建，收尾删净并核对行数与计数器。表名、控件编号取程序里的固定值。
"""
import configparser
import ctypes
import ctypes.wintypes as wt
import glob
import hashlib
import os
import time
import zlib

from . import core

u = core.u

# ---------------------------------------------------------------- 常量
# 主窗口 / GM 页控件编号（与 ui_char.c 的 G_* 枚举一致）
IDC_CHLIST, IDC_STATUS = 113, 191
PAGE_PET, PAGE_GM = 4, 8
G_NAME, G_LEV, G_JOB, G_CLONE = 1801, 1802, 1803, 1828

WM_KILLFOCUS = 0x0008
CBN_SELENDOK = 9
LVM_GETHEADER = 0x101F
CV_SLOT = 296                       # charac_view.info 的槽位长度

# 库表
CHARAC_INFO = "taiwan_cain.charac_info"
CHARAC_VIEW = "taiwan_cain.charac_view"
INVENTORY = "taiwan_cain_2nd.inventory"
DELIVERY = "taiwan_cain_2nd.cera_shop_delivery_applied"
WORK = "taiwan_cain_2nd.user_items_work"
SOOYA = "taiwan_cain.test_sooya"

# 克隆不参与的表（与 db_admin.c 的 tab_excluded 同后缀）
EXCL_SUFFIX = ("_log", "_del", "_history", "_backup", "_bak", "_old", "_mod")

# 触发器名固定便于收尾清理；哨兵值要落在程序自己夹取后的合法区间里
TRG_LEV, TRG_JOB = "r4_blk_lev", "r4_blk_job"
BAD_JOB_DEFAULT = 9                 # 职业下拉只有 11 项，9 在范围内
EMOJI = "测试\U0001F600"            # 4 字节字符
UI_IDS = (88001, 88002)             # 两张无主键表的测试行标识
SOOYA_LEV = (7, 8)

# 本轮动过的 6 张表：改前 / 改后 / 还原逐张留字节级指纹
RT = [CHARAC_INFO, CHARAC_VIEW, INVENTORY, WORK, SOOYA, DELIVERY]


# ---------------------------------------------------------------- Win32 声明
# 框架没声明的几个：句柄返回值必须按 64 位取，否则句柄一大就截断
u.GetParent.restype = wt.HWND
u.GetParent.argtypes = [wt.HWND]
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]


# ---------------------------------------------------------------- 界面小工具
def _rect(h):
    r = wt.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r


def _first_row_xy(lv):
    """列表第一行的客户区坐标：表头下方 2px，必落在第 0 行。"""
    r = _rect(lv)
    hd = u.SendMessageW(lv, LVM_GETHEADER, 0, 0)
    y = 26
    if hd:
        hr = wt.RECT()
        u.GetWindowRect(hd, ctypes.byref(hr))
        y = hr.bottom - r.top + 2
    return 40, max(2, y)


def _status(ctx, cap=512):
    return core.txt(u.GetDlgItem(ctx.main, IDC_STATUS), cap)


def _wait_loaded(ctx, timeout=25.0):
    return core.wait_until(lambda: "已载入" in _status(ctx), timeout, 0.3)


def _wait_eq(fn, want, timeout=10.0, step=0.2):
    """等取值函数返回 want（写库是后台作业，提交不等于落库）。"""
    got = fn()
    t0 = time.time()
    while got != want and time.time() - t0 < timeout:
        time.sleep(step)
        got = fn()
    return got


def _select_char(ctx, want_name, timeout=25.0):
    """点左栏第一行并切到 GM 页，直到角色名框显示 want_name。

    列表按 charac_no 降序，第一行就是编号最大的角色。判据用名字框而不是状态栏：
    「已载入」只是状态栏里的一瞬，会被 PVF/扫描之类的提示盖掉。
    """
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    if not core.wait_until(lambda: core.rows(lv) > 0, 20.0, 0.2):
        return False
    ctx.page(PAGE_GM)
    t0 = time.time()
    last = 0.0
    while time.time() - t0 < timeout:
        if _name_box(ctx) == want_name:
            return True
        if time.time() - last > 3.0:          # 列表可能正被轮询重填，点空就再点一次
            core.click_at(lv, *_first_row_xy(lv), 0.4)
            last = time.time()
        _dismiss(ctx)
        time.sleep(0.2)
    return _name_box(ctx) == want_name


def _check_list_rows(ctx, timeout=20.0):
    """左栏角色列表的行数应等于库里在场角色总数（列表最多显示 500 个）。

    用例里新造的角色要等一轮轮询才会进列表，所以按库里实时算出的行数等它追上来。
    """
    lv = u.GetDlgItem(ctx.main, IDC_CHLIST)
    total = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE delete_flag=0")
    want = min(total, 500)
    got = _wait_eq(lambda: core.rows(lv), want, timeout, 0.3)
    if not ctx.ok("左栏角色列表读出来了", got > 0, "行数 %s" % (got,)):
        return False
    return ctx.check("左栏角色列表行数 = 库里在场角色总数", got, want)


def _gm_page(ctx):
    """切到 GM 页，确认角色名控件在场。"""
    ctx.page(PAGE_GM)
    time.sleep(0.8)
    return ctx.ok("GM 页角色名框存在", bool(ctx.ctl(G_NAME)), "控件 %d" % G_NAME)


def _name_box(ctx):
    return core.txt(ctx.ctl(G_NAME), 64)


def _set_name(ctx, name):
    h = ctx.ctl(G_NAME)
    if not h:
        return False
    core.set_text(h, name)
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
    return True


def _set_lev(ctx, lev):
    h = ctx.ctl(G_LEV)
    if not h:
        return False
    core.set_text(h, str(lev))
    u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
    return True


def _combo_sel(ctx, cid):
    h = ctx.ctl(cid)
    return int(u.SendMessageW(h, core.CB_GETCURSEL, 0, 0)) if h else -1


def _combo_pick(ctx, cid, idx):
    """选中下拉第 idx 项并按 CBN_SELENDOK 通知页面 = 提交。"""
    h = ctx.ctl(cid)
    if not h:
        return False
    parent = u.GetParent(h)
    u.SendMessageW(h, core.CB_SETCURSEL, idx, 0)
    u.PostMessageW(parent, core.WM_COMMAND, (CBN_SELENDOK << 16) | (cid & 0xFFFF), h)
    return True


# ---------------------------------------------------------------- 弹窗
def _pid_of(h):
    pid = wt.DWORD(0)
    u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def _boxes(ctx):
    """本进程弹的可见 #32770：确认框、提示框。"""
    out = []

    def each(h, lp):
        if not u.IsWindowVisible(h) or core.cls_of(h) != "#32770":
            return True
        if _pid_of(h) != ctx.proc.pid:
            return True
        out.append(h)
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(each), 0)
    return out


def _box_text(h):
    parts = []
    for c in core.kids(h):
        if core.cls_of(c) in ("Static", "Button"):
            t = core.txt(c, 2048)
            if t:
                parts.append(t)
    return " / ".join(parts)


def _wait_box(ctx, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        bs = _boxes(ctx)
        if bs:
            time.sleep(0.2)
            return bs[0], _box_text(bs[0])
        time.sleep(0.05)
    return None, ""


def _close_box(ctx, h, btn=1, timeout=8.0):
    b = u.GetDlgItem(h, btn) or u.GetDlgItem(h, 1)     # 只有「确定」的提示框也能关掉
    if b:
        core.press(b, 0.0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if h not in _boxes(ctx):
            return True
        time.sleep(0.05)
    return False


def _dismiss(ctx):
    """点掉意外弹出来的提示框：模态循环会卡住后面的操作。"""
    for h in _boxes(ctx):
        _close_box(ctx, h, 1)


# ---------------------------------------------------------------- 库小工具
def _mid(ctx, cno):
    return ctx.q1("SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))


def _live_chars(ctx, mid):
    return ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info "
                  "WHERE m_id=%s AND delete_flag=0", (mid,))


def _char_row(ctx, cno):
    """(名字, HEX, CHAR_LENGTH, LENGTH)：字节级比较够看出双重编码/截断。"""
    r = ctx.q("SELECT charac_name,HEX(charac_name),CHAR_LENGTH(charac_name),"
              "LENGTH(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    return r[0] if r else None


def _char_name(ctx, cno):
    r = _char_row(ctx, cno)
    return r[0] if r else None


def _char_fields(ctx, cno):
    """(名字, HEX, lev, job, grow_type)：改名/改等级/改职业共用一份快照。"""
    r = ctx.q("SELECT charac_name,HEX(charac_name),lev,job,grow_type "
              "FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))
    return r[0] if r else None


def _lev(ctx, cno):
    return ctx.q1("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=%s", (cno,))


def _auto_inc(ctx):
    return ctx.q1("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                  "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")


def _restore_ai(ctx, ai):
    """自增计数器不会随删行回退，得显式改回去。"""
    if not ai:
        return
    try:
        ctx.cur.execute("ALTER TABLE taiwan_cain.charac_info AUTO_INCREMENT=%s", (ai,))
    except Exception as e:
        print("!! 还原自增计数器失败：%s" % e)


def _view_row(ctx, mid):
    """(charac_count, LENGTH, HEX, info)；还原只认字节。"""
    r = ctx.q("SELECT charac_count,LENGTH(info),HEX(info),info FROM taiwan_cain.charac_view "
              "WHERE m_id=%s", (mid,))
    return r[0] if r else None


def _view_restore(ctx, mid, view):
    ctx.cur.execute("UPDATE taiwan_cain.charac_view SET info=UNHEX(%s), charac_count=%s "
                    "WHERE m_id=%s", (view[2], view[0], mid))


def _slot_of(raw, no):
    """把 charac_view.info 解成 296 字节槽位，返回 no 所在槽的 (索引, 名字字节)。"""
    plain = zlib.decompress(bytes(raw)[4:])
    for k in range(len(plain) // CV_SLOT):
        q = plain[k * CV_SLOT:(k + 1) * CV_SLOT]
        if int.from_bytes(q[0:4], "little") == no:
            return k, q[4:24].split(b"\0")[0]
    return None, None


def _charac_pairs(ctx):
    rows = ctx.q("SELECT table_schema,table_name FROM information_schema.columns "
                 "WHERE column_name='charac_no' AND table_schema IN "
                 "('taiwan_cain','taiwan_cain_2nd') ORDER BY table_schema,table_name")
    return [(a, b) for a, b in rows]


def _charac_tables(ctx):
    return ["%s.%s" % (a, b) for a, b in _charac_pairs(ctx)]


def _counts(ctx, tabs):
    return {t: ctx.q1("SELECT COUNT(*) FROM %s" % t) for t in tabs}


def _drop_char(ctx, no, tabs):
    """删掉一个角色的全部行，返回删不掉的表（清理失败要能被断言抓住）。"""
    errs = []
    for t in tabs:
        try:
            ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s" % t, (no,))
        except Exception as e:
            errs.append("%s: %s" % (t, e))
    return errs


def _new_chars_above(ctx, above):
    """克隆结束后 charac_info 里编号大于 above 的角色：正常只有一个。"""
    return [r[0] for r in ctx.q("SELECT charac_no FROM taiwan_cain.charac_info "
                                "WHERE charac_no>%s", (above,))]


def _free_char_no(ctx):
    """取一个空号：落在自增计数器之下，插入不会把计数器顶上去。"""
    ai = _auto_inc(ctx)
    if not ai:
        return None
    for no in range(int(ai) - 1, max(0, int(ai) - 200), -1):
        if no <= 0:
            break
        if not ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s", (no,)):
            return no
    return None


def _free_name(ctx, base):
    """库里没被唯一键占用的名字（重名要用它，否则撞的是唯一键而不是重名检查）。"""
    for i in range(1, 20):
        cand = base if i == 1 else base + str(i)
        if not ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_name=%s",
                      (cand,)):
            return cand
    return None


def _copy_char_row(ctx, src, dst, name, delete_flag):
    """按列名整行复制源角色（唯一键 charac_name 用给定名字避开）。"""
    rows = ctx.q("SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s", (src,))
    if not rows:
        return False
    cols = [d[0] for d in ctx.cur.description]
    d = dict(zip(cols, rows[0]))
    d["charac_no"] = dst
    d["charac_name"] = name
    d["delete_flag"] = delete_flag
    ctx.cur.execute("INSERT INTO taiwan_cain.charac_info (%s) VALUES (%s)"
                    % (",".join(cols), ",".join(["%s"] * len(cols))),
                    [d[c] for c in cols])
    return True


def _copy_row(ctx, dbn, tb, keycol, src, dst):
    """复制一行，主键列换成 dst（其余列原样）。"""
    cs = [r[0] for r in ctx.q("SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                              "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s "
                              "ORDER BY ORDINAL_POSITION", (dbn, tb))]
    sel = [str(dst) if c == keycol else "`%s`" % c for c in cs]
    ctx.cur.execute("INSERT INTO `%s`.`%s` (%s) SELECT %s FROM `%s`.`%s` WHERE `%s`=%%s"
                    % (dbn, tb, ",".join("`%s`" % c for c in cs), ",".join(sel),
                       dbn, tb, keycol), (src,))


def _drop_temp(ctx, no):
    """临时角色只建了两行，就只删这两行。"""
    ctx.cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no=%s", (no,))
    ctx.cur.execute("DELETE FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (no,))


def _fingerprint(ctx):
    """六张表的字节级校验和 + 自增计数器：还原后要逐项一致。"""
    fp = {}
    for t in RT:
        ctx.cur.execute("CHECKSUM TABLE %s" % t)
        fp[t] = ctx.cur.fetchone()[1]
    # CHECKSUM 不含 AUTO_INCREMENT，单独记
    fp["@AUTO_INCREMENT"] = _auto_inc(ctx)
    return fp


# 投递表：binary(16) 主键，克隆时整表跳过。造几行才看得出「没被复制」。
def _delivery_total(ctx):
    return ctx.q1("SELECT COUNT(*) FROM %s" % DELIVERY)


def _delivery_rows(ctx, cno):
    return ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s" % DELIVERY, (cno,))


def _delivery_ids():
    return [hashlib.md5(("dnf-dm-c-char-regress-%d" % i).encode()).hexdigest() for i in (1, 2, 3)]


def _delivery_clean(ctx, ids):
    if not ids:
        return
    marks = ",".join(["UNHEX(%s)"] * len(ids))
    ctx.cur.execute("DELETE FROM %s WHERE delivery_id IN (%s)" % (DELIVERY, marks), tuple(ids))


def _delivery_seed(ctx, cno, ids):
    _delivery_clean(ctx, ids)
    for i, h in enumerate(ids, 1):
        ctx.cur.execute("INSERT INTO %s (delivery_id,charac_no,request_hash,result_blob,applied_at) "
                        "VALUES (UNHEX(%%s),%%s,UNHEX(SHA2(%%s,256)),%%s,'2030-01-01 00:00:00')"
                        % DELIVERY, (h, cno, "dnf-dm-c-char-regress-%d" % i, b""))


# ---------------------------------------------------------------- 日志
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


def _log_lines(mark, needles):
    return [l for l in _log_since(mark).splitlines() if all(k in l for k in needles)]


def _wait_log(mark, needles, timeout=25.0):
    """等一行同时含 needles 里所有关键字的新日志，返回该行。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = _log_lines(mark, needles)
        if h:
            return h[0]
        time.sleep(0.3)
    return ""


# ---------------------------------------------------------------- 等级上限
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


def _offline(ctx, mid):
    """GM 表单的写入口在角色在线时按设计拒绝写入，那样什么都验不到。"""
    try:
        st = ctx.q1("SELECT MAX(login_status) FROM taiwan_login.login_account_3 WHERE m_id=%s",
                    (mid,))
    except Exception:
        return True                     # 没有登录表就当离线，交给用例本身去判
    if st:
        ctx.skip("前置不满足：角色在线（login_status=%s），GM 写库入口按设计拒绝写入" % st)
        return False
    return True


# ---------------------------------------------------------------- 改名
def _rename_via_gui(ctx, cno, name, timeout=12.0):
    """填名字框并失焦提交，等库里变成 name；返回库里的最终值。

    失焦即提交，界面自己可能因为重载把框重填一次，所以按「库里的值」收敛。
    值没写进去就隔一会儿再提交一次：队列里的意图可能被丢掉，框里留着字不算数。
    """
    t0 = time.time()
    last = 0.0
    while time.time() - t0 < timeout:
        if _char_name(ctx, cno) == name:
            return name
        h = ctx.ctl(G_NAME)
        if h and (time.time() - last > 1.5 or _name_box(ctx) != name):
            core.set_text(h, name)
            u.SendMessageW(h, WM_KILLFOCUS, 0, 0)
            last = time.time()
        _dismiss(ctx)
        time.sleep(0.4)
    return _char_name(ctx, cno)


@core.case("char.rename_cn", needs=("gui", "db"), group="always",
           desc="改成中文名按字符落库、界面回填与库一致、逐字节可还原")
def rename_cn(ctx):
    """验：中文名按字符数写进 charac_info，HEX 与字面量一致，界面回填同一个值。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    orig = _char_row(ctx, cno)
    if not ctx.ok("读到角色名字快照", orig is not None, repr(orig)) or not _offline(ctx, mid):
        return
    target = _free_name(ctx, "测试角色")
    if target is None:
        ctx.skip("前置不满足：造不出一个没被占用的中文名")
        return
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx, orig[0])):
            return
        if not _gm_page(ctx):
            return
        mark = _log_mark()
        ctx.check("改名落库 =「%s」" % target, _rename_via_gui(ctx, cno, target), target)
        row = _char_row(ctx, cno)
        if not ctx.ok("改名后读得到这一行", row is not None):
            return
        ctx.check("CHAR_LENGTH=%d（按字符计，双重编码会是 3 倍）" % len(target),
                  row[2], len(target))
        ctx.check("LENGTH=%d（utf8 字节数）" % len(target.encode("utf-8")),
                  row[3], len(target.encode("utf-8")))
        ctx.check("HEX 与字面量的 utf8 字节一致", row[1], target.encode("utf-8").hex().upper())
        ctx.check("界面回填的名字与库一致",
                  _wait_eq(lambda: _name_box(ctx), target, 8.0), target)
        ctx.ok("日志有「改名 完成」", bool(_wait_log(mark, ["改名", "完成"], 8.0)),
               _log_since(mark).splitlines()[-2:])
    finally:
        got = _rename_via_gui(ctx, cno, orig[0], 8.0)
        if got != orig[0]:               # GUI 路径失败就按原字节直接写回
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s "
                            "WHERE charac_no=%s", (orig[0], cno))
        ctx.check("名字还原后字节与改前一致", _char_row(ctx, cno), orig)


@core.case("char.rename_latin1", needs=("gui", "db"), group="manual",
           desc="latin1 会话把中文名存成双重编码，utf8mb4 会话逐字节正确（临时行对照）")
def rename_latin1(ctx):
    """验：同一份 UTF-8 字节，latin1 会话存成双重编码，utf8mb4 会话逐字节正确。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    orig = _char_row(ctx, cno)
    if not ctx.ok("读到源角色名字快照", orig is not None):
        return
    temp_no = _free_char_no(ctx)
    if temp_no is None:
        ctx.skip("前置不满足：取不到自增计数器之下的空号来建临时行")
        return
    temp_name = _free_name(ctx, "latin1临时行")
    if temp_name is None:
        ctx.skip("前置不满足：造不出没被占用的临时行名字")
        return
    n_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info")
    ai0 = _auto_inc(ctx)
    target = _free_name(ctx, "测试角色")
    if target is None:
        ctx.skip("前置不满足：造不出一个没被占用的中文名")
        return
    try:
        _copy_char_row(ctx, cno, temp_no, temp_name, 1)
        ctx.check("临时行已建（delete_flag=1，不进角色列表）",
                  (ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                          (temp_no,)),
                   ctx.q1("SELECT delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%s",
                          (temp_no,))), (1, 1))
        # 把整条 SQL 按字节发进 latin1 会话：服务端会把 UTF-8 字节逐字节当 latin1 转存
        lat = core.db()
        try:
            lc = lat.cursor()
            lc.execute(b"SET NAMES latin1")
            raw = target.encode("utf-8")
            lc.execute(b"UPDATE taiwan_cain.charac_info SET charac_name='" + raw +
                       b"' WHERE charac_no=" + str(temp_no).encode())
        finally:
            lat.close()
        got = _char_row(ctx, temp_no)
        ctx.ok("latin1 会话确实产生双重编码（名字已不是原文）", got[0] != target, repr(got[0]))
        ctx.ok("latin1 会话的字符数翻倍（每个 UTF-8 字节变成一个字符）",
               got[2] != len(target), "CHAR_LENGTH=%s" % (got[2],))

        # 同一份内容走 utf8mb4 会话：逐字节正确
        ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s",
                        (target, temp_no))
        got = _char_row(ctx, temp_no)
        ctx.check("utf8mb4 会话写同样内容读回原文", got[0], target)
        ctx.check("CHAR_LENGTH=%d" % len(target), got[2], len(target))
        ctx.check("LENGTH=%d" % len(target.encode("utf-8")), got[3],
                  len(target.encode("utf-8")))
        ctx.check("HEX 与字面量一致", got[1], target.encode("utf-8").hex().upper())
    finally:
        _drop_temp(ctx, temp_no)
        ctx.check("临时行已删净", ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info "
                                        "WHERE charac_no=%s", (temp_no,)), 0)
        ctx.check("charac_info 行数回到改前",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info"), n_before)
        ctx.check("自增计数器未变", _auto_inc(ctx), ai0)
        ctx.check("源角色名字字节未变", _char_row(ctx, cno), orig)


@core.case("char.rename_dup", needs=("gui", "db"), group="always",
           desc="改成已被占用的名字被拦住、拒绝理由点名占用而不是唯一键报错")
def rename_dup(ctx):
    """验：重名检查在写库那一步生效，库里的名字一个字节没动，也不是靠 1062 兜的。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    orig = _char_row(ctx, cno)
    if not ctx.ok("读到角色名字快照", orig is not None, repr(orig)) or not _offline(ctx, mid):
        return
    occ = _free_name(ctx, "占用名")
    if occ is None:
        ctx.skip("前置不满足：造不出一个当前没被占用的名字")
        return
    temp_no = _free_char_no(ctx)
    if temp_no is None:
        ctx.skip("前置不满足：取不到自增计数器之下的空号来建占名行")
        return
    n_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info")
    ai0 = _auto_inc(ctx)
    try:
        # delete_flag=1：不进左栏列表，只把名字占住
        _copy_char_row(ctx, cno, temp_no, occ, 1)
        ctx.check("占名行已建（delete_flag=1）",
                  ctx.q1("SELECT delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%s",
                         (temp_no,)), 1)
        if not ctx.ok("选中第一个角色", _select_char(ctx, orig[0])):
            return
        if not _gm_page(ctx):
            return
        ctx.check("名字框显示角色当前名字",
                  _wait_eq(lambda: _name_box(ctx), orig[0], 8.0), orig[0])

        mark = _log_mark()
        ctx.ok("名字框填成被占用的名字「%s」并失焦" % occ, _set_name(ctx, occ))
        hit = _wait_log(mark, ["改名", "失败"], 20.0)
        ctx.ok("改名被拒（日志有「改名 失败」）", bool(hit), hit or _log_since(mark)[-200:])
        ctx.ok("拒绝理由是重名（「已被占用」）", "已被占用" in hit, hit)
        ctx.ok("不是靠唯一键报错兜的（日志无 Duplicate entry）",
               "Duplicate entry" not in _log_since(mark), _log_since(mark)[-200:])
        ctx.check("库里名字没变", _char_row(ctx, cno), orig)
    finally:
        _drop_temp(ctx, temp_no)
        ctx.check("占名行已删净",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                         (temp_no,)), 0)
        ctx.check("charac_info 行数回到改前",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info"), n_before)
        ctx.check("自增计数器未变", _auto_inc(ctx), ai0)
        ctx.check("角色名字字节未变", _char_row(ctx, cno), orig)


@core.case("char.rename_reject", needs=("gui", "db"), group="always",
           desc="改名被拒后名字框退回库里的真值，正对照：不重名的新名字能写进去")
def rename_reject(ctx):
    """验：被拒后界面不留只在内存里的假名字；改名成功时库与界面一起变。"""
    src = ctx.char_no()
    if not ctx.ok("库里有角色", src is not None):
        return
    src_row = _char_row(ctx, src)
    mid = _mid(ctx, src)
    if not ctx.ok("读到源角色名字快照", src_row is not None, repr(src_row)) or not _offline(ctx, mid):
        return
    temp_no = _free_char_no(ctx)
    if temp_no is None:
        ctx.skip("前置不满足：取不到自增计数器之下的空号来建临时角色")
        return
    if temp_no <= src:
        ctx.skip("前置不满足：空号 %d 不大于现有最大角色号 %d，临时角色不会排在列表第一行"
                 % (temp_no, src))
        return
    temp_name = _free_name(ctx, "重名测试号")
    if temp_name is None:
        ctx.skip("前置不满足：造不出没被占用的临时角色名")
        return
    if ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (src,)) != 1:
        ctx.skip("前置不满足：源角色没有 inventory 行，临时角色载入不了")
        return
    n_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info")
    inv_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory")
    ai0 = _auto_inc(ctx)
    try:
        _copy_char_row(ctx, src, temp_no, temp_name, 0)
        _copy_row(ctx, "taiwan_cain_2nd", "inventory", "charac_no", src, temp_no)
        ctx.check("临时角色已建（charac_info + inventory 各 1 行）",
                  (ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                          (temp_no,)),
                   ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory WHERE charac_no=%s",
                          (temp_no,))), (1, 1))
        _check_list_rows(ctx)
        if not ctx.ok("选中第一个角色（临时角色）", _select_char(ctx, temp_name)):
            return
        if not _gm_page(ctx):
            return
        ctx.check("名字框显示临时角色自己的名字",
                  _wait_eq(lambda: _name_box(ctx), temp_name, 8.0), temp_name)

        # 1) 改成源角色的名字：重名，必然被拒
        mark = _log_mark()
        ctx.ok("名字框填成源角色名「%s」并失焦" % src_row[0], _set_name(ctx, src_row[0]))
        hit = _wait_log(mark, ["改名", "失败"], 20.0)
        ctx.ok("改名被拒（日志有「改名 失败」）", bool(hit), hit or _log_since(mark)[-200:])
        ctx.ok("拒绝理由是「已被占用」", "已被占用" in hit, hit)
        ctx.check("临时角色的名字没被改", _char_name(ctx, temp_no), temp_name)
        ctx.check("源角色的名字没被改", _char_row(ctx, src), src_row)

        # 2) 名字框必须退回库里的真值，而不是停在那个没写进库的重名上
        ctx.check("名字框退回临时角色自己的名字",
                  _wait_eq(lambda: _name_box(ctx), temp_name, 20.0), temp_name)
        ctx.ok("名字框没有停在没写进库的重名上", _name_box(ctx) != src_row[0],
               repr(_name_box(ctx)))

        # 3) 正对照：不重名的名字要能写进去
        new_name = _free_name(ctx, "改名成功号")
        if not ctx.ok("能造出一个没被占用的新名字", new_name is not None):
            return
        ctx.check("不重名的新名字能写进库", _rename_via_gui(ctx, temp_no, new_name), new_name)
        ctx.check("成功后名字框显示新名字",
                  _wait_eq(lambda: _name_box(ctx), new_name, 10.0), new_name)
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        _drop_temp(ctx, temp_no)
        ctx.check("清理后 charac_info / inventory 行数回到改前",
                  (ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info"),
                   ctx.q1("SELECT COUNT(*) FROM taiwan_cain_2nd.inventory")),
                  (n_before, inv_before))
        ctx.check("清理后自增计数器未变", _auto_inc(ctx), ai0)
        ctx.check("源角色名字字节未变", _char_row(ctx, src), src_row)


@core.case("char.rename_emoji", needs=("gui", "db"), group="always",
           desc="4 字节字符改名被拒并点明原因，库里一个字节没动；正对照中文名能写")
def rename_emoji(ctx):
    """验：emoji 不静默截断、不假装成功；普通三字节中文名照常落库。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    before = _char_row(ctx, cno)
    if not ctx.ok("读到角色名字快照", before is not None, repr(before)) or not _offline(ctx, mid):
        return
    good = _free_name(ctx, "测试中")
    if good is None:
        ctx.skip("前置不满足：造不出一个没被占用的中文名")
        return
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx, before[0])):
            return
        if not _gm_page(ctx):
            return
        mark = _log_mark()
        ctx.ok("名字框填「测试😀」并失焦", _set_name(ctx, EMOJI))
        hit = _wait_log(mark, ["改名", "失败"], 15.0)
        ctx.ok("改名被明确拒绝（日志有「改名 失败」）", bool(hit),
               hit or _log_since(mark)[-200:])
        ctx.ok("拒绝理由点明「4 字节」字符", "4 字节" in hit, hit)
        ctx.check("库里名字一个字节都没动", _char_row(ctx, cno), before)
        ctx.ok("库里没出现 '?' 之类的替字符", "?" not in (_char_name(ctx, cno) or ""),
               repr(_char_name(ctx, cno)))

        # 正对照：普通中文名（每个字 3 字节）必须能写。先等拒绝后的回滚重载把名字框
        # 退回库里的值（状态栏那一下会被别的提示盖掉，名字框不会）。
        ctx.check("名字框退回库里的真值",
                  _wait_eq(lambda: _name_box(ctx), before[0], 20.0), before[0])
        ctx.check("普通中文名能写进库（「%s」）" % good,
                  _rename_via_gui(ctx, cno, good), good)
        row = _char_row(ctx, cno)
        if not ctx.ok("改名后读得到这一行", row is not None):
            return
        ctx.check("CHAR_LENGTH=%d / LENGTH=%d" % (len(good), len(good.encode("utf-8"))),
                  (row[2], row[3]), (len(good), len(good.encode("utf-8"))))
        ctx.check("HEX 与字面量一致（没有双重编码）", row[1],
                  good.encode("utf-8").hex().upper())
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        got = _rename_via_gui(ctx, cno, before[0], 8.0)
        if got != before[0]:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s "
                            "WHERE charac_no=%s", (before[0], cno))
        ctx.check("还原后字节与原值一致", _char_row(ctx, cno), before)


@core.case("char.rename_stale", needs=("gui", "db"), group="always",
           desc="改名被拒后名字框不留只在内存里的假名字，切页强制重载后仍与库一致")
def rename_stale(ctx):
    """验：拒绝路径没有留下「界面上是新的、库里是旧的」这种假名字。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    before = _char_row(ctx, cno)
    if not ctx.ok("读到角色名字快照", before is not None, repr(before)) or not _offline(ctx, mid):
        return
    try:
        if not ctx.ok("选中第一个角色", _select_char(ctx, before[0])):
            return
        if not _gm_page(ctx):
            return
        mark = _log_mark()
        ctx.ok("名字框填「测试😀」并失焦", _set_name(ctx, EMOJI))
        hit = _wait_log(mark, ["改名", "失败"], 15.0)
        if not ctx.ok("emoji 改名被拒", bool(hit), hit or _log_since(mark)[-200:]):
            return
        ctx.check("名字框退回库里的真值",
                  _wait_eq(lambda: _name_box(ctx), before[0], 20.0), before[0])
        ctx.ok("名字框没有停在没写进库的 emoji 上", EMOJI not in _name_box(ctx),
               repr(_name_box(ctx)))
        # 切页强制一次重载：假名字若只活在控件里，这一下一定会被冲掉
        ctx.page(PAGE_PET)
        time.sleep(0.8)
        ctx.page(PAGE_GM)
        time.sleep(1.0)
        ctx.check("切页重载后名字框仍与库一致",
                  _wait_eq(lambda: _name_box(ctx), before[0], 10.0), before[0])
        ctx.check("库里名字一个字节都没动", _char_row(ctx, cno), before)
    finally:
        # 万一这条拒绝路径真的写进了被截断的名字，也要按原字节兜回
        if _char_name(ctx, cno) != before[0]:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s "
                            "WHERE charac_no=%s", (before[0], cno))
        ctx.check("收尾时名字与库仍一致", _char_row(ctx, cno), before)


# ---------------------------------------------------------------- 克隆
def _clone_name(ctx, name):
    """复算克隆名：LEFT(原名,18) + 从 2 开始第一个没被占用的后缀。"""
    base = name[:18]
    for k in range(2, 100):
        cand = base + str(k)
        if not ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_name=%s",
                      (cand,)):
            return cand
    return None


def _clone_via_gui(ctx, timeout=10.0):
    """点「克隆角色」→ 确认框点确定。"""
    h = ctx.ctl(G_CLONE)
    if not ctx.ok("「克隆角色」按钮存在", bool(h), "控件 %d" % G_CLONE):
        return False
    core.press(h, 0.4)
    b, t = _wait_box(ctx, timeout)
    if not ctx.ok("弹出克隆确认框", bool(b), (t or "")[:80]):
        return False
    ctx.ok("确认框文案含「复制一份」", "复制一份" in (t or ""), (t or "")[:80])
    _close_box(ctx, b, 1)
    return True


def _wait_new_char(ctx, above, timeout=180.0):
    """等 charac_info 里出现编号大于 above 的新角色：返回 (no, 名字, HEX, 字符数, 字节数)。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = ctx.q("SELECT charac_no,charac_name,HEX(charac_name),"
                  "CHAR_LENGTH(charac_name),LENGTH(charac_name) "
                  "FROM taiwan_cain.charac_info WHERE charac_no>%s "
                  "ORDER BY charac_no DESC LIMIT 1", (above,))
        if r:
            return r[0]
        _dismiss(ctx)                    # 中途弹出来的提示框会挡住作业收尾
        time.sleep(0.5)
    return None


def _policy_pairs(ctx):
    """克隆会逐表判键的范围：排除日志/流水类与 charac_info 本身。"""
    out = []
    for db, tb in _charac_pairs(ctx):
        if tb == "charac_info":
            continue
        if any(tb.endswith(s) and len(tb) > len(s) for s in EXCL_SUFFIX):
            continue
        out.append((db, tb))
    return out


def _policy_counts(ctx):
    """按 clone_key_policy 逐表复算：换号复制 / 跳列自增 / 整表跳过。"""
    recode = skipcol = skiptable = 0
    for db, tb in _policy_pairs(ctx):
        pk = ctx.q("SELECT s.COLUMN_NAME,IFNULL(c.EXTRA,''),IFNULL(c.DATA_TYPE,'') "
                   "FROM information_schema.STATISTICS s "
                   "LEFT JOIN information_schema.COLUMNS c "
                   "ON c.TABLE_SCHEMA=s.TABLE_SCHEMA AND c.TABLE_NAME=s.TABLE_NAME "
                   "AND c.COLUMN_NAME=s.COLUMN_NAME "
                   "WHERE s.TABLE_SCHEMA=%s AND s.TABLE_NAME=%s AND s.INDEX_NAME='PRIMARY' "
                   "ORDER BY s.SEQ_IN_INDEX", (db, tb))
        npk = len(pk)
        # 自增列按 COLUMNS.EXTRA 判：它未必在 PRIMARY 里（可能在 UNIQUE 索引上）
        nai = ctx.q1("SELECT COUNT(*) FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s "
                     "AND EXTRA LIKE '%%auto_increment%%'", (db, tb)) or 0
        have_key = any(c == "charac_no" for c, _e, _d in pk)
        # 与 clone_key_policy 的分支顺序一致
        if npk == 0:
            recode += 1                                   # 无主键：换号复制
        elif nai and npk == 1:
            skipcol += 1                                  # 单列自增主键：交服务端发号
        elif nai:
            recode += 1                                   # 复合主键含自增列：跳列 + 换号
        elif have_key:
            recode += 1                                   # 主键含 charac_no：换号复制
        else:
            skiptable += 1                                # 不透明键或重新发号会造出假记录
    return recode, skipcol, skiptable


def _clone_log_counts(text):
    lines = text.splitlines()
    per = [l for l in lines if " 复制 " in l and " 行 ——（" in l]
    skip = [l for l in lines if " 跳过 —— " in l]
    # 「跳列」只认单列自增主键那一支：复合主键含自增列的日志也提「自增」，
    # 但它的判定是换号复制。
    skipcol = [l for l in per if "单列自增主键" in l]
    recode = [l for l in per if "单列自增主键" not in l]
    return recode, skipcol, skip


@core.case("char.clone", needs=("gui", "db"), group="always",
           desc="克隆出派生中文名的新角色、关键表与 charac_view 同步、不透明主键表整表跳过")
def clone_char(ctx):
    """验：克隆真的建出角色（不是点了没反应），数据复制到新号上，槽位视图同步。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    orig = _char_row(ctx, cno)
    if not ctx.ok("读到源角色名字快照", orig is not None, repr(orig)) or not _offline(ctx, mid):
        return
    view = _view_row(ctx, mid)
    if view is None:
        ctx.skip("前置不满足：charac_view 里没有该账号的行，槽位同步无从观察")
        return
    tabs = _charac_tables(ctx)
    cnt_before = _counts(ctx, tabs)
    ai_before = _auto_inc(ctx)
    live_before = _live_chars(ctx, mid)
    max_before = ctx.q1("SELECT IFNULL(MAX(charac_no),0) FROM taiwan_cain.charac_info") or 0
    src_lev = _lev(ctx, cno)
    dlv_before = _delivery_total(ctx)
    ids = _delivery_ids()
    new_no = None
    target = _free_name(ctx, "克隆三号")
    if target is None:
        ctx.skip("前置不满足：造不出一个没被占用的中文名作为克隆源")
        return
    try:
        _delivery_seed(ctx, cno, ids)
        ctx.check("占位投递记录已建（整表跳过的判据要有行）",
                  _delivery_rows(ctx, cno), len(ids))
        if not ctx.ok("选中第一个角色", _select_char(ctx, orig[0])):
            return
        if not _gm_page(ctx):
            return
        ctx.check("源角色先改成中文名「%s」" % target, _rename_via_gui(ctx, cno, target), target)
        row = _char_row(ctx, cno)
        ctx.check("中文名按 utf8 落库（HEX 与字面量一致）", row[1],
                  target.encode("utf-8").hex().upper())
        # 改名作业收尾后才能点克隆：上一个动作没结束会被入口挡掉（判据在库上，这里只同步）
        core.wait_until(lambda: "改名 完成" in _status(ctx), 10.0, 0.2)
        time.sleep(0.3)
        expect = _clone_name(ctx, target)
        if expect is None:
            ctx.skip("前置不满足：原名加 2~99 的后缀全被占用，程序会拒绝克隆")
            return

        mark = _log_mark()
        if not _clone_via_gui(ctx):
            return
        got = _wait_new_char(ctx, max_before)
        if not ctx.ok("克隆成功（真的建出了新角色）", got is not None, "新角色 %r" % (got,)):
            return
        new_no = got[0]
        time.sleep(1.5)                  # 日志每秒落一次盘

        ctx.ok("新 charac_no 与源不同", new_no != cno, "%s vs %s" % (new_no, cno))
        ctx.check("克隆名 =「%s」" % expect, got[1], expect)
        ctx.check("克隆名 CHAR_LENGTH / LENGTH", (got[3], got[4]),
                  (len(expect), len(expect.encode("utf-8"))))
        ctx.check("克隆名 HEX 与字面量一致（无双重编码）", got[2],
                  expect.encode("utf-8").hex().upper())
        ctx.check("克隆行在 charac_info 恰好 1 行",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s",
                         (new_no,)), 1)
        ctx.check("等级跟着源角色", _lev(ctx, new_no), src_lev)

        # 不是空壳：几张关键表都要有与源角色同样多的行
        for t in (INVENTORY, "taiwan_cain_2nd.user_items", "taiwan_cain.charac_stat",
                  "taiwan_cain.new_charac_quest"):
            n_src = ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s" % t, (cno,))
            n_new = ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s" % t, (new_no,))
            ctx.check("%s 复制行数与源一致" % t, n_new, n_src)

        # charac_view：客户端角色列表照这张表的槽位画
        cv = _view_row(ctx, mid)
        ctx.check("charac_count 同步为账号下在场角色数", cv[0], _live_chars(ctx, mid))
        ctx.check("charac_count 比克隆前多 1", cv[0], live_before + 1)
        k, nm = _slot_of(cv[3], new_no)
        ctx.ok("charac_view 里有新角色的槽位", k is not None, "槽索引 %s" % (k,))
        ctx.check("槽位里的名字是新名字", nm, expect.encode("utf-8"))
        ctx.check("charac_view 仍然只有该账号 1 行",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_view WHERE m_id=%s", (mid,)), 1)

        # 不透明主键的表整表跳过：行照搬过去必然撞主键
        ctx.check("投递表：新角色 0 行（整表跳过）", _delivery_rows(ctx, new_no), 0)
        ctx.check("投递表全表行数没变（没被删、没被改）", _delivery_total(ctx),
                  dlv_before + len(ids))
        ctx.ok("投递表的行仍全挂在源角色上", _delivery_rows(ctx, cno) >= len(ids),
               "%s 行" % (_delivery_rows(ctx, cno),))

        text = _log_since(mark)
        lines = text.splitlines()
        ctx.ok("日志有「跳过 N 张非派生主键表」的汇总行",
               any("张非派生主键表" in l for l in lines),
               next((l for l in lines if "张非派生主键表" in l), "(没有)"))
        dl = [l for l in lines if "cera_shop_delivery_applied" in l]
        ctx.ok("日志点名跳过了 cera_shop_delivery_applied",
               any("跳过" in l for l in dl), dl[0] if dl else "(没有)")
        ctx.ok("日志里有逐表的「复制 N 行」明细（不是只报一次结果）",
               text.count("复制") >= 10, "含「复制」的行 %d 条" % text.count("复制"))
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        # 抛异常/超时退出时可能还没拿到新编号：按「编号大于克隆前最大值」兜底清一次
        orphans = [] if new_no else _new_chars_above(ctx, max_before)
        errs = _drop_char(ctx, new_no, tabs) if new_no else []
        for no in orphans:
            errs += _drop_char(ctx, no, tabs)
        _delivery_clean(ctx, ids)
        _view_restore(ctx, mid, view)
        _restore_ai(ctx, ai_before)
        got = _rename_via_gui(ctx, cno, orig[0], 10.0)
        if got != orig[0]:
            ctx.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s "
                            "WHERE charac_no=%s", (orig[0], cno))
        ctx.ok("删克隆时每张表都删得掉", not errs, str(errs)[:200])
        ctx.ok("没有残留的新角色", not _new_chars_above(ctx, max_before),
               str(_new_chars_above(ctx, max_before)))
        after = _counts(ctx, tabs)
        diff = {t: (cnt_before[t], after[t]) for t in tabs if cnt_before[t] != after[t]}
        ctx.ok("删克隆后 %d 张表行数与克隆前一致" % len(tabs), not diff, str(diff)[:300])
        ctx.check("charac_view 槽位字节还原一致", _view_row(ctx, mid), view)
        ctx.check("自增计数器已还原", _auto_inc(ctx), ai_before)
        ctx.check("投递表回到跑前的行数", _delivery_total(ctx), dlv_before)
        ctx.check("源角色名还原后字节一致", _char_row(ctx, cno), orig)


def _work_rows(ctx, cno):
    return ctx.q("SELECT ui_id,charac_no,slot,it_id FROM %s WHERE charac_no=%%s "
                 "ORDER BY ui_id" % WORK, (cno,))


def _sooya_rows(ctx, cno):
    return ctx.q("SELECT m_id,charac_no,lev FROM %s WHERE charac_no=%%s ORDER BY lev" % SOOYA,
                 (cno,))


def _seed_nopk(ctx, cno, mid):
    """插 4 行测试数据（两张无主键表各 2 行，全挂当前角色）。"""
    ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s AND ui_id IN %%s" % WORK,
                    (cno, UI_IDS))
    for i, uid in enumerate(UI_IDS):
        ctx.cur.execute(
            "INSERT INTO %s (ui_id,charac_no,slot,it_id,expire_date,obtain_from,reg_date,"
            "ipg_agency_no,ability_no,stat,clear_avatar_id,jewel_socket,item_lock_key) "
            "VALUES (%%s,%%s,%%s,%%s,'2030-01-01 00:00:00',0,NOW(),'CHARREGRESS',0,0,0,'',0)"
            % WORK, (uid, cno, i + 1, 1001 + i))
    ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s AND lev IN %%s" % SOOYA,
                    (cno, SOOYA_LEV))
    for lev in SOOYA_LEV:
        ctx.cur.execute("INSERT INTO %s (m_id,charac_no,lev) VALUES (%%s,%%s,%%s)" % SOOYA,
                        (mid, cno, lev))
    return (ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s AND ui_id IN %%s"
                   % WORK, (cno, UI_IDS)),
            ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s AND lev IN %%s"
                   % SOOYA, (cno, SOOYA_LEV)))


def _unseed_nopk(ctx, cno):
    ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s AND ui_id IN %%s" % WORK,
                    (cno, UI_IDS))
    ctx.cur.execute("DELETE FROM %s WHERE charac_no=%%s AND lev IN %%s" % SOOYA,
                    (cno, SOOYA_LEV))


@core.case("char.clone_nopk", needs=("gui", "db"), group="always",
           desc="无主键表按 charac_no 换号复制、逐表判定分布与复算一致、不透明主键仍跳过")
def clone_nopk(ctx):
    """验：无主键的两张表跟着新角色换号复制，其余表的键策略没被误伤。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    view = _view_row(ctx, mid)
    if view is None:
        ctx.skip("前置不满足：charac_view 里没有该账号的行，槽位同步无从观察")
        return
    name_before = _char_row(ctx, cno)
    if not ctx.ok("读到源角色名字快照", name_before is not None, repr(name_before)):
        return
    tabs = _charac_tables(ctx)
    cnt_before = _counts(ctx, tabs)
    fp_before = _fingerprint(ctx)
    max_before = ctx.q1("SELECT IFNULL(MAX(charac_no),0) FROM taiwan_cain.charac_info") or 0
    dlv_before = _delivery_total(ctx)
    ai_before = _auto_inc(ctx)
    src_work0 = _work_rows(ctx, cno)
    src_sooya0 = _sooya_rows(ctx, cno)
    new_no = None
    seeded = False
    try:
        seeded = True                    # 下面无论插成几行，收尾都要按测试标识清一遍
        ctx.check("测试数据已插：user_items_work / test_sooya 各 2 行",
                  _seed_nopk(ctx, cno, mid), (2, 2))
        ws = _work_rows(ctx, cno)
        ss = _sooya_rows(ctx, cno)
        if not ctx.ok("选中第一个角色", _select_char(ctx, name_before[0])):
            return
        if not _gm_page(ctx):
            return

        mark = _log_mark()
        if not _clone_via_gui(ctx):
            return
        got = _wait_new_char(ctx, max_before)
        if not ctx.ok("克隆成功（真的建出了新角色）", got is not None, "新角色 %r" % (got,)):
            return
        new_no = got[0]
        time.sleep(1.5)                  # 日志每秒落一次盘

        # 换号复制：user_items_work
        wn = _work_rows(ctx, new_no)
        ctx.check("user_items_work：新角色行数与源一致（换号复制，不是整表跳过）",
                  len(wn), len(ws))
        ctx.check("user_items_work：charac_no 已换成新号",
                  sorted({r[1] for r in wn}), [new_no])
        ctx.check("user_items_work：ui_id / slot / it_id 原样带过来",
                  sorted((r[0], r[2], r[3]) for r in wn),
                  sorted((r[0], r[2], r[3]) for r in ws))

        # 换号复制：test_sooya
        sn = _sooya_rows(ctx, new_no)
        ctx.check("test_sooya：新角色行数与源一致（换号复制，不是整表跳过）", len(sn), len(ss))
        ctx.check("test_sooya：charac_no 换新号、m_id 沿用、lev 一致",
                  sorted((r[1], r[0], r[2]) for r in sn),
                  sorted((new_no, r[0], r[2]) for r in ss))

        # 源数据没被动过（克隆不是搬家）
        ctx.check("源角色的两张表行数与插数据后一致",
                  (_work_rows(ctx, cno), _sooya_rows(ctx, cno)), (ws, ss))
        ctx.ok("源行 ui_id 没被改", all(uid in [r[0] for r in _work_rows(ctx, cno)]
                                      for uid in UI_IDS),
               "%r" % ([r[0] for r in _work_rows(ctx, cno)],))

        # 逐表判定分布：与按同一套策略独立复算的结果对齐
        text = _log_since(mark)
        lines = text.splitlines()
        recode, skipcol, skip = _clone_log_counts(text)
        exp_r, exp_sc, exp_sk = _policy_counts(ctx)
        # 分桶只做合计：单表归属由下面逐表断言，按理由文本分桶会受措辞影响
        ctx.check("分布合计 = 在册 %d 张" % (exp_r + exp_sc + exp_sk + 1),
                  len(recode) + len(skipcol) + len(skip), exp_r + exp_sc + exp_sk + 1)

        # 两张无主键表在日志里必须是「复制」，不是「跳过」
        lw = [l for l in lines if "user_items_work" in l]
        ls = [l for l in lines if "test_sooya" in l]
        want_w = "复制 %d 行" % len(ws)
        want_s = "复制 %d 行" % len(ss)
        ctx.ok("user_items_work 是「%s」而不是跳过" % want_w,
               bool(lw) and want_w in lw[0] and "跳过" not in lw[0],
               lw[0] if lw else "(没有)")
        ctx.ok("test_sooya 是「%s」而不是跳过" % want_s,
               bool(ls) and want_s in ls[0] and "跳过" not in ls[0],
               ls[0] if ls else "(没有)")
        ctx.ok("两张表的理由是「无主键（按 charac_no 归属）」",
               bool(lw) and "无主键（按 charac_no 归属）" in lw[0] and
               bool(ls) and "无主键（按 charac_no 归属）" in ls[0],
               (lw[0] if lw else "")[-40:])

        # 不透明主键仍然整表跳过
        ld = [l for l in lines if "cera_shop_delivery_applied" in l]
        ctx.ok("跳过的表里仍有 cera_shop_delivery_applied（不透明主键）",
               any("跳过" in l for l in ld), ld[0] if ld else "(没有)")
        ctx.check("投递表仍然整表跳过：新角色 0 行 / 全表行数不变",
                  (_delivery_rows(ctx, new_no), _delivery_total(ctx)),
                  (0, dlv_before))
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        orphans = [] if new_no else _new_chars_above(ctx, max_before)
        errs = _drop_char(ctx, new_no, tabs) if new_no else []
        for no in orphans:
            errs += _drop_char(ctx, no, tabs)
        if seeded:
            _unseed_nopk(ctx, cno)
        _view_restore(ctx, mid, view)
        _restore_ai(ctx, ai_before)
        ctx.ok("删克隆/测试数据时每张表都删得掉", not errs, str(errs)[:200])
        ctx.ok("没有残留的新角色", not _new_chars_above(ctx, max_before),
               str(_new_chars_above(ctx, max_before)))
        after = _counts(ctx, tabs)
        diff = {t: (cnt_before[t], after[t]) for t in tabs if cnt_before[t] != after[t]}
        ctx.ok("清理后 %d 张表行数全部还原" % len(tabs), not diff, str(diff)[:300])
        ctx.check("清理后 charac_view 字节与跑前一致", _view_row(ctx, mid), view)
        ctx.check("清理后自增计数器未变", _auto_inc(ctx), ai_before)
        ctx.check("源角色名字字节未变", _char_row(ctx, cno), name_before)
        ctx.check("测试数据已删净（按测试标识查）",
                  (ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s AND ui_id IN %%s"
                          % WORK, (cno, UI_IDS)),
                   ctx.q1("SELECT COUNT(*) FROM %s WHERE charac_no=%%s AND lev IN %%s"
                          % SOOYA, (cno, SOOYA_LEV))), (0, 0))
        ctx.check("user_items_work / test_sooya 行数回到跑前",
                  (_work_rows(ctx, cno), _sooya_rows(ctx, cno)),
                  (src_work0, src_sooya0))
        fp_end = _fingerprint(ctx)
        bad = {t: (fp_before[t], fp_end[t]) for t in fp_before if fp_before[t] != fp_end[t]}
        ctx.ok("六张表 + 自增计数器：还原后字节级指纹与跑前一致", not bad, str(bad)[:300])


# ---------------------------------------------------------------- 改职业 / 改等级被拒
def _triggers(ctx, table="charac_info"):
    return [r[0] for r in ctx.q("SELECT TRIGGER_NAME FROM information_schema.TRIGGERS "
                                "WHERE TRIGGER_SCHEMA='taiwan_cain' AND EVENT_OBJECT_TABLE=%s",
                                (table,))]


def _add_trigger(ctx, name, col, value, msg):
    """用 BEFORE UPDATE 触发器把某次写入变成错误 1644：不动一行数据，只让写失败。"""
    ctx.cur.execute("DROP TRIGGER IF EXISTS taiwan_cain.%s" % name)
    ctx.cur.execute("CREATE TRIGGER taiwan_cain.%s BEFORE UPDATE ON taiwan_cain.charac_info "
                    "FOR EACH ROW BEGIN IF NEW.%s = %d THEN SIGNAL SQLSTATE '45000' "
                    "SET MESSAGE_TEXT = '%s'; END IF; END" % (name, col, value, msg))


def _del_trigger(ctx, name):
    ctx.cur.execute("DROP TRIGGER IF EXISTS taiwan_cain.%s" % name)


@core.case("char.job_reject", needs=("gui", "db"), group="always",
           desc="改职业被库拒绝后职业下拉退回库里的真值，库里 job/grow 不变")
def job_reject(ctx):
    """验：写失败后界面按库里的值重填，而不是停在那个没写进库的职业上。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    row0 = _char_fields(ctx, cno)
    if not ctx.ok("读到角色快照", row0 is not None, repr(row0)) or not _offline(ctx, mid):
        return
    bad_job = BAD_JOB_DEFAULT if row0[3] != BAD_JOB_DEFAULT else 8
    _del_trigger(ctx, TRG_JOB)
    left = _triggers(ctx)
    if left:
        ctx.skip("前置不满足：charac_info 上已有别的触发器 %s" % left)
        return
    n_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info")
    ai0 = _auto_inc(ctx)
    try:
        _add_trigger(ctx, TRG_JOB, "job", bad_job, "R4 拒绝改职业")
        ctx.ok("BEFORE UPDATE 触发器已建（job=%d 会被 SIGNAL 拒绝）" % bad_job,
               TRG_JOB in _triggers(ctx), str(_triggers(ctx)))
        _check_list_rows(ctx)
        if not ctx.ok("选中第一个角色", _select_char(ctx, row0[0])):
            return
        if not _gm_page(ctx):
            return
        h = ctx.ctl(G_JOB)
        if not ctx.ok("职业下拉存在", bool(h), "控件 %d" % G_JOB):
            return
        n = int(u.SendMessageW(h, core.CB_GETCOUNT, 0, 0))
        if not ctx.ok("职业下拉项数够（哨兵 %d 在范围内）" % bad_job, n > bad_job, "%d 项" % n):
            return

        ctx.check("改前职业下拉 = 库里的 job",
                  _wait_eq(lambda: _combo_sel(ctx, G_JOB), row0[3], 8.0), row0[3])
        mark = _log_mark()
        ctx.ok("把职业下拉选到 %d 提交" % bad_job, _combo_pick(ctx, G_JOB, bad_job))
        hit = _wait_log(mark, ["设置职业", "失败"], 25.0)
        ctx.ok("改职业被拒（日志有「设置职业 失败」）", bool(hit),
               hit or _log_since(mark)[-200:])
        ctx.ok("拒绝理由来自触发器（1644/R4）", "R4" in hit or "1644" in hit, hit)
        ctx.check("职业下拉退回库里的真值",
                  _wait_eq(lambda: _combo_sel(ctx, G_JOB), row0[3], 25.0), row0[3])
        ctx.ok("下拉没有停在没写进库的假值 %d 上" % bad_job,
               _combo_sel(ctx, G_JOB) != bad_job, "选中 %s" % (_combo_sel(ctx, G_JOB),))
        ctx.check("库里名字/等级/职业/觉醒都没被改", _char_fields(ctx, cno), row0)
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        _del_trigger(ctx, TRG_JOB)
        ctx.ok("触发器已删除", TRG_JOB not in _triggers(ctx), str(_triggers(ctx)))
        ctx.check("charac_info 行数不变",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info"), n_before)
        ctx.check("自增计数器未变", _auto_inc(ctx), ai0)
        ctx.check("角色的名字/等级/职业/觉醒与改前一致", _char_fields(ctx, cno), row0)


@core.case("char.lev_reject", needs=("gui", "db"), group="always",
           desc="改等级被拒：库里不变，且第二次同值写入仍真的发写库（快照已退回真值）")
def lev_reject(ctx):
    """验：快照里的等级被纠正成库里的值，否则第二次同值写入会被「值没变」挡掉。"""
    cno = ctx.char_no()
    if not ctx.ok("库里有角色", cno is not None):
        return
    mid = _mid(ctx, cno)
    row0 = _char_fields(ctx, cno)
    if not ctx.ok("读到角色快照", row0 is not None, repr(row0)) or not _offline(ctx, mid):
        return
    lim = _lev_max()
    bad_lev = next((v for v in (77, 78, 76, 70) if v != row0[2] and 1 <= v <= lim), None)
    if bad_lev is None:
        ctx.skip("前置不满足：找不到落在 1..%d 且与当前等级 %s 不同的哨兵值" % (lim, row0[2]))
        return
    _del_trigger(ctx, TRG_LEV)
    left = _triggers(ctx)
    if left:
        ctx.skip("前置不满足：charac_info 上已有别的触发器 %s" % left)
        return
    n_before = ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info")
    ai0 = _auto_inc(ctx)
    try:
        _add_trigger(ctx, TRG_LEV, "lev", bad_lev, "R4 拒绝改等级")
        ctx.ok("BEFORE UPDATE 触发器已建（lev=%d 会被 SIGNAL 拒绝）" % bad_lev,
               TRG_LEV in _triggers(ctx), str(_triggers(ctx)))
        _check_list_rows(ctx)
        if not ctx.ok("选中第一个角色", _select_char(ctx, row0[0])):
            return
        if not _gm_page(ctx):
            return
        h = ctx.ctl(G_LEV)
        if not ctx.ok("等级输入框存在", bool(h), "控件 %d" % G_LEV):
            return
        ctx.check("进页面等级框是 0（框只收本次要设的值）",
                  _wait_eq(lambda: core.txt(h), "0", 3.0), "0")

        mark = _log_mark()
        ctx.ok("等级框填 %d 并失焦" % bad_lev, _set_lev(ctx, bad_lev))
        hit = _wait_log(mark, ["设置等级", "失败"], 25.0)
        ctx.ok("改等级被拒（日志有「设置等级 失败」）", bool(hit),
               hit or _log_since(mark)[-200:])
        ctx.check("库里 lev 没被改", _lev(ctx, cno), row0[2])
        # 让回滚重载跑完再探；状态栏那一下会被别的提示盖掉，这里只当同步用
        _wait_loaded(ctx, 8.0)
        time.sleep(1.8)                  # 日志每秒落一次盘，先让第一次的失败行落定

        mark2 = _log_mark()
        ctx.check("探针起点干净（新起点之后还没有「设置等级 失败」）",
                  len(_log_lines(mark2, ["设置等级", "失败"])), 0)
        ctx.ok("第二次填同一个值 %d 并失焦" % bad_lev, _set_lev(ctx, bad_lev))
        hit2 = _wait_log(mark2, ["设置等级", "失败"], 14.0)
        ctx.ok("第二次仍然真的发了写库（说明快照里的 lev 已退回库里的 %d）" % row0[2],
               bool(hit2), hit2 or "(日志里没有，说明被「值没变就不发」挡住了)")
        ctx.check("库里 lev 仍然是 %d" % row0[2], _lev(ctx, cno), row0[2])
        ctx.ok("进程仍然活着", ctx.proc.poll() is None, "poll=%s" % (ctx.proc.poll(),))
    finally:
        _del_trigger(ctx, TRG_LEV)
        ctx.ok("触发器已删除", TRG_LEV not in _triggers(ctx), str(_triggers(ctx)))
        ctx.check("charac_info 行数不变",
                  ctx.q1("SELECT COUNT(*) FROM taiwan_cain.charac_info"), n_before)
        ctx.check("自增计数器未变", _auto_inc(ctx), ai0)
        ctx.check("角色的名字/等级/职业/觉醒与改前一致", _char_fields(ctx, cno), row0)
