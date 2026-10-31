# -*- coding: utf-8 -*-
"""自检的负路径：临时把一张可选表改名，看它能不能说出来；测完必须改回。"""
import ctypes, subprocess, time, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, wait_main, db as open_db, start

db = open_db()
cur = db.cursor()
TAB = "taiwan_cain_2nd.letter"
try:
    cur.execute("RENAME TABLE %s TO taiwan_cain_2nd.letter_selfcheck_bak" % TAB)
    print("已临时改名 letter → letter_selfcheck_bak")

    p, main = start(wait=5.0)
    p.kill()
    print("程序已退出，看日志")
finally:
    cur.execute("RENAME TABLE taiwan_cain_2nd.letter_selfcheck_bak TO %s" % TAB)
    cur.execute("SELECT COUNT(*) FROM %s" % TAB)
    print("已改回 letter，行数 =", cur.fetchone()[0])
    db.close()
