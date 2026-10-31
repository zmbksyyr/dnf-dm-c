# -*- coding: utf-8 -*-
"""泡点回归：覆盖「目标人群 + 多奖励」四个新维度，跑完复原。

维度：
  1) 指定账号（target=3）：账号级 D点 + D币 两条 UPDATE 都生效
  2) 指定角色（target=2）：角色级 赠送点券 UPDATE 生效 + 物品随邮件发放（postal 插入）
  3) 关闭后不再加点

测试库里没有在线账号，所以这里直接按「指定账号 / 指定角色」显式给 ID，
绕开在线判定，既能测目标人群子查询，又不依赖 login_status。
全程只动 m_id=1 这一条账号 + 其下第一个角色，finally 里逐项复原
（点券/赠送点券/物品邮件各自回滚，login_status 改回原值）。
"""
import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, switch, db, PID, start
from pages import click_at as click, press as click_btn, ctl, txt as gettext, set_text as settext


G_BON = PID(8, 28)        # 启用泡点（复选框）
G_BSEC = PID(8, 29)       # 间隔（秒）
G_BTARGET = PID(8, 30)    # 目标人群（下拉）
G_BTNO = PID(8, 31)       # 指定 ID
G_BCOIN = PID(8, 32)      # 赠送点券
G_BDP = PID(8, 33)        # D点
G_BDC = PID(8, 34)        # D币
G_BITEM = PID(8, 35)      # 物品 ID
G_BINUM = PID(8, 36)      # 数量
G_BAPPLY = PID(8, 39)     # 应用
G_BINFO = PID(8, 41)      # 状态行

M_ID = 1
ACC = "1"
SEC = 5                   # 5 秒一轮
CB_SETCURSEL = 0x14E
BM_CLICK = 0x00F5
WM_SETTEXT = 0x000C


def setcombo(h, idx):
    u.SendMessageW(h, CB_SETCURSEL, idx, 0)


def q1(sql, args=()):
    c = db()
    cur = c.cursor()
    cur.execute(sql, args)
    row = cur.fetchone()
    c.close()
    return row[0] if row else None


def dpoint():
    return q1("SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s", (ACC,))


def dcoin():
    return q1("SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s", (ACC,))


def coin(ch):
    if not ch:
        return None
    return q1("SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s", (ch,))


def mails(ch):
    if not ch:
        return None
    return q1("SELECT COUNT(*) FROM taiwan_cain_2nd.postal "
              "WHERE receive_charac_no=%s AND send_charac_name='泡点'", (ch,))


def set_dpoint(v):
    c = db()
    cur = c.cursor()
    cur.execute("INSERT INTO taiwan_billing.cash_cera (account,cera) VALUES (%s,%s) "
                "ON DUPLICATE KEY UPDATE cera=VALUES(cera)", (ACC, v))
    c.commit(); c.close()


def set_dcoin(v):
    c = db()
    cur = c.cursor()
    cur.execute("INSERT INTO taiwan_billing.cash_cera_point (account,cera_point) VALUES (%s,%s) "
                "ON DUPLICATE KEY UPDATE cera_point=VALUES(cera_point)", (ACC, v))
    c.commit(); c.close()


def set_coin(ch, v):
    if not ch:
        return
    c = db()
    cur = c.cursor()
    cur.execute("UPDATE taiwan_cain_2nd.inventory SET coin=%s WHERE charac_no=%s", (v, ch))
    c.commit(); c.close()


def set_online(v):
    c = db()
    cur = c.cursor()
    cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s", (v, M_ID))
    c.commit(); c.close()


def del_test_mails():
    c = db()
    cur = c.cursor()
    cur.execute("DELETE FROM taiwan_cain_2nd.postal WHERE send_charac_name='泡点'")
    c.commit(); c.close()


# ---- 取测试用的角色（m_id=1 下的第一个角色）----
CH = q1("SELECT charac_no FROM taiwan_cain.charac_info WHERE m_id=%s "
        "AND delete_flag=0 ORDER BY charac_no LIMIT 1", (M_ID,))
LOGIN0 = q1("SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s", (M_ID,))

DP0 = dpoint() or 0
DC0 = dcoin() or 0
CN0 = coin(CH) or 0
ML0 = mails(CH) or 0
print("初始：m_id=%s CH=%s login_status=%s" % (M_ID, CH, LOGIN0))
print("初始：D点=%s D币=%s 赠送点券(CH)=%s 泡点邮件(CH)=%s" % (DP0, DC0, CN0, ML0))
if CH is None:
    print("!! 警告：m_id=%s 下没有可用角色，赠送点券/物品邮件两项将跳过" % M_ID)

p = None
try:
    p, main = start()
    click(u.GetDlgItem(main, 113), 40, 12)                     # 选中第一个角色（仅为了连库就绪）
    time.sleep(3.0)
    switch(main, 8)                                            # 切到 GM 页
    time.sleep(1.0)

    bon = ctl(main, G_BON)
    bsec = ctl(main, G_BSEC)
    btarget = ctl(main, G_BTARGET)
    btno = ctl(main, G_BTNO)
    bcoin = ctl(main, G_BCOIN)
    bdp = ctl(main, G_BDP)
    bdc = ctl(main, G_BDC)
    bitem = ctl(main, G_BITEM)
    binum = ctl(main, G_BINUM)
    bap = ctl(main, G_BAPPLY)
    binfo = ctl(main, G_BINFO)
    if not all([bon, bsec, btarget, btno, bcoin, bdp, bdc, bitem, binum, bap, binfo]):
        print("!! 找不到泡点控件：", bon, bsec, btarget, btno, bcoin, bdp, bdc, bitem, binum, bap, binfo)
        sys.exit(1)

    def apply_bubble(target, tno, dc, dp, cn, item, inum):
        # 先关掉，避免上一次设置还在跑
        if u.SendMessageW(bon, 0x00F1, 0, 0) & 0x0001:   # BM_GETCHECK
            click_btn(bon)
            click_btn(bap)
            time.sleep(0.6)
        setcombo(btarget, target)
        settext(btno, str(tno))
        settext(bsec, str(SEC))
        settext(bdp, str(dp))
        settext(bdc, str(dc))
        settext(bcoin, str(cn))
        settext(bitem, str(item))
        settext(binum, str(inum))
        if target != 0:
            click_btn(bon)          # 勾上启用
        click_btn(bap)
        time.sleep(0.8)
        print("  [应用] target=%d tno=%s D币+%d D点+%d 点券+%d 物品%d×%d · 状态行=%r"
              % (target, tno, dc, dp, cn, item, inum, gettext(binfo)))

    # ---- 子测试 1：指定账号 → D点 + D币 同时涨 ----
    apply_bubble(target=3, tno=M_ID, dc=1, dp=1, cn=0, item=0, inum=0)
    time.sleep(14.0)               # 跨过两轮（5s / 10s）
    dp1 = dpoint() or 0
    dc1 = dcoin() or 0
    print("子测试1 后：D点=%s(+%d) D币=%s(+%d)" % (dp1, dp1 - DP0, dc1, dc1 - DC0))

    # ---- 子测试 2：指定角色 → 赠送点券涨 + 物品邮件插入 ----
    if CH is not None:
        apply_bubble(target=2, tno=CH, dc=0, dp=0, cn=1, item=1001, inum=1)
        time.sleep(14.0)
        cn1 = coin(CH) or 0
        ml1 = mails(CH) or 0
        print("子测试2 后：赠送点券(CH)=%s(+%d) 泡点邮件(CH)=%s(+%d)" % (cn1, cn1 - CN0, ml1, ml1 - ML0))
    else:
        cn1 = CN0; ml1 = ML0

    # ---- 关闭 ----
    if u.SendMessageW(bon, 0x00F1, 0, 0) & 0x0001:
        click_btn(bon)
        click_btn(bap)
        time.sleep(0.8)
    pa = dcoin() or 0
    time.sleep(9.0)
    pb = dcoin() or 0
    print("关闭后 D币 %s → 9 秒后 %s（应相等）" % (pa, pb))
    print("状态行：", repr(gettext(binfo)))

    ok1 = (dp1 - DP0) >= 2 and (dc1 - DC0) >= 2
    ok2 = (cn1 - CN0) >= 2 and (ml1 - ML0) >= 2 if CH is not None else True
    ok3 = pa == pb
    print("\n1) 指定账号：D点+D币各按间隔加: %s（D点+%d，D币+%d，均≥2）"
          % ("OK" if ok1 else "FAIL", dp1 - DP0, dc1 - DC0))
    print("2) 指定角色：赠送点券+物品邮件: %s（点券+%d，邮件+%d，均≥2）"
          % ("OK" if ok2 else "FAIL", cn1 - CN0, ml1 - ML0))
    print("3) 关闭后不再加点: %s" % ("OK" if ok3 else "FAIL"))
    print("结果：%s" % ("通过" if ok1 and ok2 and ok3 else "失败"))
finally:
    if p:
        p.kill()
    # 逐项复原
    set_dcoin(DC0)
    set_dpoint(DP0)
    set_coin(CH, CN0)
    del_test_mails()
    set_online(LOGIN0 if LOGIN0 is not None else 0)
    print("已复原：D点=%s D币=%s 点券(CH)=%s 泡点邮件已删 login_status=%s"
          % (DP0, DC0, CN0, LOGIN0))
