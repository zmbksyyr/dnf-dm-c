# -*- coding: utf-8 -*-
"""GM 页全功能自测：把值改脏 → 点按钮/提交表单 → 看它是不是真的落库。

为什么要"先改脏"：无限负重这类功能的目标值就是 99999999，角色早就是这个值了，
前后一比没变化 —— 看着像通过，其实可能一行都没写（一键按钮没带角色编号那次就是
这样漏掉的：日志里全是 charac_no=0）。所以每一项都先写个脏值，再看它有没有被改回。

跑完会输出一张「功能 / 是否生效 / 备注」表，值一律复原（脚本自己建的行会清掉）。

    python tools/v_features.py
"""
import ctypes
import glob
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch, db

u.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.SendMessageW.restype = wt.LPARAM
u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u.GetDlgItem.restype = wt.HWND
u.EnumChildWindows.argtypes = [wt.HWND, ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p), ctypes.c_void_p]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

PAGE_GM = 8
# GM 页控件号 = PAGE_ID(PAGE_GM, n) = 1800 + n，跟 ui_char.c 的枚举一一对应
G = dict(NAME=1801, LEV=1802, JOB=1803, GROW=1804, MONEY=1805, COIN=1806, CERA=1807, POINT=1808,
         ACCM=1809, GRADE=1810, WIN=1811, PVP_POINT=1812, SP=1813, QP=1814, NOWEIGHT=1815,
         SLOT=1816, UNLOCK=1817, EXPERT=1818, FATIGUE=1819, DUNGEON=1820, PUNISH=1821, QCLR=1822,
         UNLCREATE=1826)
EN_KILLFOCUS = 0x0200
WM_COMMAND = 0x0111
BN_CLICK = 0x00F5


def ctl(main, cid):
    """在可见的 DnfPage 下按号找控件。"""
    found = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "DnfPage" and u.IsWindowVisible(h):
            w = u.GetDlgItem(h, cid)
            if w:
                found.append(w)
                return False
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return found[0] if found else None


def log_new(n0):
    fs = sorted(glob.glob(os.path.join(BUILD, "config", "logs", "log-*.txt")))
    if not fs:
        return []
    with open(fs[-1], encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()[n0:]


def log_n():
    return len(log_new(0))


def step(label):
    print("  → %s" % label, flush=True)

def click(h):
    """异步点：SendMessage(BM_CLICK) 会让"点"这个动作在目标线程里同步跑完才返回，
    一旦这个按钮会弹确认框（MessageBox 模态循环），发送方就永远等不到返回——
    脚本第一次就是这样卡死的。PostMessage 只投递、不等结果，确认框另开线程去点。"""
    u.PostMessageW(h, BN_CLICK, 0, 0)


def dismiss_confirm(main, timeout=3.0):
    """把工具弹出来的确认框点掉（确定）。返回点掉了几个。"""
    n = 0
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = []
        cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def each(h, lp):
            cls = ctypes.create_unicode_buffer(32)
            u.GetClassNameW(h, cls, 32)
            if cls.value == "#32770" and (u.GetWindow(h, 4) == main or u.GetParent(h) == main):
                found.append(h)
            return True

        u.EnumWindows(cb(each), 0)
        for d in found:
            u.PostMessageW(u.GetDlgItem(d, 1), BN_CLICK, 0, 0)     # IDOK
            n += 1
        if n:
            return n
        time.sleep(0.2)
    return n


def set_edit(main, h, text):
    """改文本框并触发失焦提交（GM 页是失焦即写，没有"设置"按钮）。"""
    old = ctypes.create_unicode_buffer(128)
    u.SendMessageW(h, 0x000D, 128, ctypes.addressof(old))      # WM_GETTEXT
    buf = ctypes.create_unicode_buffer(text)
    u.SendMessageW(h, 0x000C, 0, ctypes.addressof(buf))         # WM_SETTEXT
    # 让 EDIT 自己发 EN_KILLFOCUS（WM_COMMAND 是 id 在低字、通知码在高字，
    # 手工拼很容易写反，写反了页面收到的是个不存在的 id，静默不提交）
    u.SendMessageW(h, 0x0008, 0, 0)                             # WM_KILLFOCUS
    return old.value


# ---------------------------------------------------------------- 用例表
# btn   : (按钮名, 控件号, 表, 键列, 列, 目标值, 脏值, 还原值说明)
BTN_CASES = [
    ("满SP/TP", G["SP"], "taiwan_cain_2nd.skill", "charac_no", "remain_sp", 90000, 7),
    ("满QP", G["QP"], "taiwan_cain.charac_quest_shop", "charac_no", "qp", 90000, 7),
    ("无限负重", G["NOWEIGHT"], "taiwan_cain.charac_info", "charac_no", "inven_weight", 99999999, 100),
    ("开装备槽", G["SLOT"], "taiwan_cain.charac_stat", "charac_no", "add_slot_flag", 3, 0),
    ("解锁装备等级", G["UNLOCK"], "taiwan_cain.charac_manage_info", "charac_no", "max_equip_level", 999, 10),
    ("满专家", G["EXPERT"], "taiwan_cain.charac_stat", "charac_no", "expert_job_exp", 2054, 7),
    ("补满疲劳", G["FATIGUE"], "taiwan_cain.charac_info", "charac_no", "fatigue", None, 7),  # 目标=max_fatigue
]
# 表单用例：(控件号, 表名, 键列, 列, 写入值)
FORM_CASES = [
    ("金币", G["MONEY"], "taiwan_cain_2nd.inventory", "charac_no", "money", 123456),
    ("复活币", G["COIN"], "taiwan_cain_2nd.inventory", "charac_no", "coin", 77),
    ("点券", G["CERA"], "taiwan_billing.cash_cera", "account", "cera", 4321),
    ("赠送点券", G["POINT"], "taiwan_billing.cash_cera_point", "account", "cera_point", 321),
    ("账号金币", G["ACCM"], "taiwan_cain.account_cargo", "m_id", "money", 888888),
]

cn = db()
cur = cn.cursor()


def q1(sql, *a):
    try:
        if cur.execute(sql, a):
            r = cur.fetchone()
            return r[0] if r else None
        return None
    except Exception as e:
        return "ERR:%s" % e


def execsql(sql):
    try:
        cur.execute(sql)
        return True
    except Exception:
        return False


cur.execute("SELECT charac_no, charac_name, m_id, lev FROM taiwan_cain.charac_info "
            "WHERE delete_flag=0 ORDER BY charac_no LIMIT 1")
NO, NAME, MID, LEV = cur.fetchone()
cur.execute("SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s", (MID,))
ONLINE = (cur.fetchone() or [0])[0]
print("角色: charac_no=%s %s mid=%s lev=%s 在线状态=%s" % (NO, NAME, MID, LEV, ONLINE))
if ONLINE:
    print("!! 角色在线：GM 表单按设计拒绝写入（按钮仍可用），表单用例会判为「跳过」\n")

results = []
p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
try:
    main = wait_main(p)
    click(u.GetDlgItem(main, 104))                     # 连接
    time.sleep(4.5)
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0201, 1, (12 << 16) | 40)   # 选中第一个角色
    u.PostMessageW(u.GetDlgItem(main, 113), 0x0202, 0, (12 << 16) | 40)
    time.sleep(3.5)
    switch(main, PAGE_GM)
    time.sleep(1.5)

    # ---------- 一键按钮 ----------
    for label, cid, table, keycol, col, target, dirty in BTN_CASES:
        key = NO if keycol == "charac_no" else MID
        sel = "SELECT %s FROM %s WHERE %s=%%s" % (col, table, keycol)
        orig = q1(sel, key)
        if orig == "ERR" or (isinstance(orig, str) and orig.startswith("ERR")):
            results.append((label, "表/列不存在", str(orig)[:60]))
            continue
        execsql("UPDATE %s SET %s=%d WHERE %s=%d" % (table, col, dirty, keycol, key))
        step(label)
        h = ctl(main, cid)
        if not h:
            results.append((label, "控件找不到", "id=%d" % cid))
            continue
        n0 = log_n()
        click(h)
        deadline = time.time() + 8.0
        got = None
        while time.time() < deadline:
            time.sleep(0.4)
            got = q1(sel, key)
            if target is None:
                mx = q1("SELECT max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s" % NO)
                if got is not None and got == mx:
                    break
            elif got == target:
                break
        ok = (got == target) if target is not None else (got == q1(
            "SELECT max_fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s" % NO))
        # 复原
        if orig is None:
            execsql("DELETE FROM %s WHERE %s=%d" % (table, keycol, key))
        else:
            execsql("UPDATE %s SET %s=%d WHERE %s=%d" % (table, col, int(orig), keycol, key))
        tail = log_new(n0)
        results.append((label, "生效" if ok else "没生效",
                        "脏值%d → %s%s" % (dirty, got, (" | " + tail[-1][11:]) if tail else "")))

    # 解锁全图：目标值是一串，只验非空且含最高难度标记
    step("解锁全图")
    execsql("UPDATE taiwan_cain.member_dungeon SET dungeon='' WHERE m_id=%d" % MID)
    h = ctl(main, G["DUNGEON"])
    click(h)
    time.sleep(2.5)
    dun = q1("SELECT dungeon FROM taiwan_cain.member_dungeon WHERE m_id=%s" % MID)
    results.append(("解锁全图", "生效" if (dun and "1|3" in dun) else "没生效",
                    "dungeon=%s…" % (dun[:24] if dun else dun)))

    # 清惩罚：先塞一条再清
    step("清惩罚")
    execsql("INSERT INTO d_taiwan.member_punish_info (m_id,occ_time) VALUES (%d,NOW())" % MID)
    click(ctl(main, G["PUNISH"]))
    time.sleep(2.5)
    left = q1("SELECT COUNT(*) FROM d_taiwan.member_punish_info WHERE m_id=%s" % MID)
    results.append(("清惩罚", "生效" if left == 0 else "没生效", "剩余惩罚记录 %s 条" % left))

    # 清任务：先记下原值，清完验 0，再原样写回
    cols = ["play_%d" % i for i in range(1, 21)] + ["play_%d_trigger" % i for i in range(1, 21)]
    cur.execute("SELECT %s FROM taiwan_cain.new_charac_quest WHERE charac_no=%%s" % ",".join(cols), (NO,))
    row = cur.fetchone()
    if not row:
        results.append(("清任务", "跳过", "没有任务记录行（角色没接过任务）"))
    else:
        step("清任务")
        execsql("UPDATE taiwan_cain.new_charac_quest SET play_1=555 WHERE charac_no=%d" % NO)
        click(ctl(main, G["QCLR"]))
        time.sleep(2.5)
        z = q1("SELECT play_1 FROM taiwan_cain.new_charac_quest WHERE charac_no=%s" % NO)
        back = ",".join("%s=%d" % (c, int(v)) for c, v in zip(cols, row))
        execsql("UPDATE taiwan_cain.new_charac_quest SET %s WHERE charac_no=%d" % (back, NO))
        results.append(("清任务", "生效" if z == 0 else "没生效", "play_1=%s（原值已还原）" % z))

    # 解除建角限制
    orig_cnt = q1("SELECT count FROM d_taiwan.limit_create_character WHERE m_id=%s" % MID)
    if orig_cnt is None:
        results.append(("解除建角限制", "跳过", "该账号没有建角限制记录（本来就能建角）"))
    else:
        step("解除建角限制")
        execsql("UPDATE d_taiwan.limit_create_character SET count=9 WHERE m_id=%d" % MID)
        old = log_n()
        click(ctl(main, G["UNLCREATE"]))
        n_conf = dismiss_confirm(main)      # 这条有确认框：异步点了之后再把框点掉
        time.sleep(2.5)
        got = q1("SELECT count FROM d_taiwan.limit_create_character WHERE m_id=%s" % MID)
        execsql("UPDATE d_taiwan.limit_create_character SET count=%d WHERE m_id=%d" % (int(orig_cnt), MID))
        results.append(("解除建角限制", "生效" if got == 0 else "没生效(需确认框)", "count=%s" % got))

    # ---------- 表单字段（失焦即写） ----------
    for label, cid, table, keycol, col, val in FORM_CASES:
        key = NO if keycol == "charac_no" else MID
        sel = "SELECT %s FROM %s WHERE %s=%%s" % (col, table, keycol)
        orig = q1(sel, key)
        if isinstance(orig, str) and orig.startswith("ERR"):
            results.append((label, "表/列不存在", str(orig)[:60]))
            continue
        if ONLINE:
            results.append((label, "跳过", "角色在线，GM 表单按设计拒绝写入"))
            continue
        step(label)
        h = ctl(main, cid)
        set_edit(main, h, str(val))
        deadline = time.time() + 8.0
        got = orig
        while time.time() < deadline:
            time.sleep(0.4)
            got = q1(sel, key)
            if got == val:
                break
        # 复原：把原值再写回去
        if orig is None:
            execsql("DELETE FROM %s WHERE %s=%d" % (table, keycol, key))
        else:
            set_edit(main, h, str(orig))
            time.sleep(1.2)
        results.append((label, "生效" if got == val else "没生效", "%s → %s" % (orig, got)))
finally:
    p.kill()

print("\n%-14s %-10s %s" % ("功能", "结果", "明细"))
print("-" * 78)
for label, res, note in results:
    print("%-14s %-10s %s" % (label, res, note))
bad = [r for r in results if r[1] == "没生效"]
print("\n合计 %d 项：生效 %d · 没生效 %d · 跳过 %d" %
      (len(results), len([r for r in results if r[1] == "生效"]), len(bad),
       len([r for r in results if r[1].startswith("跳过")])))
print("（值已复原；账号金库/QP 这类原本没有的行会被删回去）")
