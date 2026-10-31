"""验证「只读一次并缓存」：轮询只刷新左栏（列表 + 卡片），不碰 GM 表单快照。

做法：用另一条连接把 lev +1（模拟玩家在游戏里升级），等轮询拉回来，然后验三件事：
1) 左栏角色卡跟着变成新等级（轮询的职责）
2) GM 页的等级框还是旧值（表单是快照，只在换代时重填，不跟着库实时跳）
3) 物品页的选中行没被打断（轮询不触发 data_ver，页面列表不重排）

注意：ListView 的 LVM_GETITEMTEXT 不能跨进程用（指针不通，会把对方打崩），
只能读 STATIC 的文本。
"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, grab, EXE, OUT, switch, wait_main, db as open_db, PID as PAGE_ID, start
from pages import click_at as click, ctl, txt as gettext


PAGE_ITEM, PAGE_GM = 0, 8
I_LIST = PAGE_ID(PAGE_ITEM, 2)
G_LEV = PAGE_ID(PAGE_GM, 2)
LVM_GETNEXTITEM, LVNI_SELECTED = 0x100C, 0x0002


def statics(main):
    """左栏角色卡是一堆无 ID 的 STATIC，全部文本捞回来找 'Lv.x'"""
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def each(h, lp):
        cls = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, cls, 64)
        if cls.value == "Static":
            out.append(gettext(h))
        return True

    u.EnumChildWindows(main, cb(each), 0)
    return out


db = open_db()
cur = db.cursor()
cur.execute("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=1")
lev0 = int(cur.fetchone()[0])
print("db lev =", lev0)

p, main = start()
lv = u.GetDlgItem(main, 113)                             # 左栏角色列表
click(lv, 40, 12)
time.sleep(3.0)

# 先在物品页选一行，作为「被打断」的观察点
switch(main, PAGE_ITEM)
time.sleep(0.8)
il = ctl(main, I_LIST)
click(il, 40, 56)
time.sleep(0.3)
sel0 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
print("item selected =", sel0)

switch(main, PAGE_GM)
time.sleep(0.6)
levw = ctl(main, G_LEV)
print("gm lev before =", gettext(levw))
print("card before   =", [s for s in statics(main) if s.startswith("Lv.")])

cur.execute(f"UPDATE taiwan_cain.charac_info SET lev={lev0 + 1} WHERE charac_no=1")
print("db lev ->", lev0 + 1, "，等轮询…")
time.sleep(9.0)

cards = [s for s in statics(main) if s.startswith("Lv.")]
print("gm lev after  =", gettext(levw))
print("card after    =", cards)
ok1 = f"Lv.{lev0 + 1}" in cards                    # 左栏跟着库变了
ok2 = gettext(levw) == str(lev0)                   # 表单快照没被冲掉
switch(main, PAGE_ITEM)
time.sleep(0.8)
sel1 = u.SendMessageW(il, LVM_GETNEXTITEM, -1, LVNI_SELECTED)
print("item selected after =", sel1)
print("1) 轮询刷新了左栏角色卡:", "OK" if ok1 else "FAIL")
print("2) GM 表单快照没被刷新:", "OK" if ok2 else "FAIL")
print("3) 物品页选中没丢:", "OK" if sel1 == sel0 else "FAIL")
switch(main, PAGE_GM)
time.sleep(0.5)
grab(main, os.path.join(OUT, "v14_pollcard.png"))

cur.execute(f"UPDATE taiwan_cain.charac_info SET lev={int(lev0)} WHERE charac_no=1")
print("lev 恢复 =", lev0)
p.kill()
db.close()
