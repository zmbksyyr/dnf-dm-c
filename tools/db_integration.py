"""Data-layer regressions against an explicitly supplied disposable DNF test server.

Reads connection settings from the build's settings.ini or DNF_GM_TEST_* variables.
Each test restores its fixtures in finally/tearDown; run without concurrent writers.
"""
import argparse
import configparser
import contextlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import threading
import time
import unittest
import zlib

import pymysql

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("DNF_GM_BUILD", ROOT.parent / "_build" / "DNF_GM_C"))
OUT = Path(os.environ.get("DNF_GM_AUDIT", ROOT.parent / "_build" / "audit"))
PROBE = OUT / "db_probe.exe"
TEST_NO = 2000000001


def ident(name):
    return ".".join("`" + p.replace("`", "``") + "`" for p in name.split("."))


def settings():
    cfg = configparser.RawConfigParser()
    cfg.read(BUILD / "config" / "settings.ini", encoding="utf-8-sig")
    defaults = {"host": "127.0.0.1", "port": "3306", "user": "game", "pwd": ""}
    return {k: os.environ.get("DNF_GM_TEST_" + k.upper(),
                            cfg.get("db", k, fallback=v)) for k, v in defaults.items()}


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    zig = os.environ.get("ZIG") or shutil.which("zig") or r"C:\zig\zig.exe"
    sources = ["db", "db_mail", "db_admin", "db_bubble", "db_online", "mysql", "util", "zlib", "item", "pvf"]
    cmd = [zig, "cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2",
           "-DUNICODE", "-D_UNICODE", "-Wall", "-Wextra", "-Wshadow", "-Wconversion",
           "-Wpedantic", "-Werror", "-Isrc", "tools/db_probe.c"]
    cmd += ["src/" + s + ".c" for s in sources]
    cmd += ["-lws2_32", "-luser32", "-lshell32", "-ladvapi32", "-o", str(PROBE)]
    subprocess.run(cmd, cwd=ROOT, check=True)


class DataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.params = settings()
        cls.conn = pymysql.connect(host=cls.params["host"], port=int(cls.params["port"]),
                                   user=cls.params["user"], password=cls.params["pwd"],
                                   charset="utf8mb4", autocommit=True, read_timeout=30)
        cls.cur = cls.conn.cursor()
        cls.cur.execute("SELECT charac_no,m_id FROM taiwan_cain.charac_info "
                        "WHERE delete_flag=0 ORDER BY charac_no DESC LIMIT 1")
        cls.cno, cls.mid = cls.cur.fetchone()

    @classmethod
    def tearDownClass(cls):
        cls.conn.close()

    def probe(self, *args, mode=None, timeout=60, extra_env=None):
        env = dict(os.environ, **{"DNF_GM_TEST_" + k.upper(): v for k, v in self.params.items()})
        if mode is not None:
            env["DNF_GM_TEST_SQL_MODE"] = mode
        if extra_env:
            env.update(extra_env)
        return subprocess.run([str(PROBE), *map(str, args)], cwd=OUT, env=env,
                              capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout)

    def one(self, sql, args=None):
        self.cur.execute(sql, args)
        return self.cur.fetchone()[0]

    def test_original_game_name_encoding_and_search(self):
        before = self.one("SELECT HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
        name = "编码测试甲"
        try:
            r = self.probe("rename", self.cno, self.mid, name)
            self.assertEqual(r.returncode, 0, r.stderr)
            wire = self.one("SELECT HEX(CONVERT(charac_name USING latin1)) FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
            self.assertEqual(bytes.fromhex(wire), name.encode())
            r = self.probe("read-name", self.cno, self.mid)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("name=" + name, r.stdout)
            r = self.probe("search-name", name)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(str(self.cno) + " " + name, r.stdout)
        finally:
            self.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=UNHEX(%s) WHERE charac_no=%s", (before, self.cno))
            self.assertEqual(self.one("SELECT HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,)), before)

    @contextlib.contextmanager
    def trigger(self, table, event, body):
        schema = table.split(".")[0]
        name = schema + ".codex_audit_trigger"
        self.assertEqual(self.one("SELECT COUNT(*) FROM information_schema.TRIGGERS "
                                  "WHERE TRIGGER_SCHEMA=%s AND TRIGGER_NAME='codex_audit_trigger'",
                                  (schema,)), 0, "test trigger name is already in use")
        self.cur.execute("CREATE TRIGGER " + ident(name) + " BEFORE " + event + " ON " +
                         ident(table) + " FOR EACH ROW " + body)
        try:
            yield
        finally:
            self.cur.execute("DROP TRIGGER " + ident(name))

    @contextlib.contextmanager
    def attachment(self):
        tables = ("taiwan_cain_2nd.user_items", "taiwan_cain_2nd.postal")
        counters = [self.one("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                             "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s", t.split(".")) for t in tables]
        ui = postal = None
        try:
            self.cur.execute("INSERT INTO taiwan_cain_2nd.user_items "
                             "(charac_no,it_id,expire_date,obtain_from,reg_date,stat) "
                             "VALUES (%s,1001,'9999-12-31 23:59:59',1,NOW(),2)", (TEST_NO,))
            ui = self.cur.lastrowid
            self.cur.execute("INSERT INTO taiwan_cain_2nd.postal "
                             "(occ_time,send_charac_name,receive_charac_no,item_id,add_info,"
                             "avata_flag,creature_flag,letter_id,unlimit_flag) "
                             "VALUES (NOW(),'audit',%s,1001,%s,1,0,0,1)", (TEST_NO, ui))
            postal = self.cur.lastrowid
            yield ui, postal
        finally:
            if postal is not None:
                self.cur.execute("DELETE FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,))
            if ui is not None:
                self.cur.execute("DELETE FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,))
            for table, counter in zip(tables, counters):
                if counter:
                    self.cur.execute("ALTER TABLE " + ident(table) + " AUTO_INCREMENT=" + str(counter))

    def test_mail_postal_failure_rolls_back_attachment(self):
        with self.attachment() as (ui, postal):
            with self.trigger("taiwan_cain_2nd.postal", "UPDATE",
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked postal'"):
                r = self.probe("mail-delete", postal)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)
            self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 0)

    def test_mail_attachment_failure_keeps_postal(self):
        with self.attachment() as (ui, postal):
            with self.trigger("taiwan_cain_2nd.user_items", "DELETE",
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked attachment'"):
                r = self.probe("mail-delete", postal)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 0)

    def test_mail_delete_success(self):
        with self.attachment() as (ui, postal):
            r = self.probe("mail-delete", postal)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 0)
            self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 1)

    @contextlib.contextmanager
    def offline_character(self):
        # Own the live-status rows; never change a real account's login state.
        self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain.charac_info WHERE charac_no=%s OR m_id=%s",
                                  (TEST_NO, TEST_NO)), 0)
        self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_login.login_account_3 WHERE m_id=%s",
                                  (TEST_NO,)), 0)
        counter = self.one("SELECT AUTO_INCREMENT FROM information_schema.TABLES "
                           "WHERE TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info'")
        self.cur.execute("SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
        template = list(self.cur.fetchone())
        cols = [d[0] for d in self.cur.description]
        template[cols.index('charac_no')] = TEST_NO
        template[cols.index('m_id')] = TEST_NO
        template[cols.index('charac_name')] = 'audit_mail'
        try:
            self.cur.execute("INSERT INTO taiwan_cain.charac_info (" + ','.join(map(ident, cols)) +
                             ") VALUES (" + ','.join(['%s'] * len(cols)) + ")", template)
            self.cur.execute("INSERT INTO taiwan_login.login_account_3 (m_id,login_status,last_login_date) "
                             "VALUES (%s,0,NOW())", (TEST_NO,))
            yield
        finally:
            self.cur.execute("DELETE FROM taiwan_login.login_account_3 WHERE m_id=%s", (TEST_NO,))
            self.cur.execute("DELETE FROM taiwan_cain.charac_info WHERE charac_no=%s", (TEST_NO,))
            if counter:
                self.cur.execute("ALTER TABLE taiwan_cain.charac_info AUTO_INCREMENT=" + str(counter))

    def test_mail_delete_rejects_online_and_leaving_account(self):
        with self.offline_character(), self.attachment() as (ui, postal):
            for status in (1, 2):
                self.cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s",
                                 (status, TEST_NO))
                for command, target in (("mail-delete", postal), ("mail-delete-all", TEST_NO)):
                    with self.subTest(status=status, command=command):
                        r = self.probe(command, target)
                        self.assertNotEqual(r.returncode, 0, r.stdout)
                        self.assertIn("在线或正在退出", r.stderr)
                        self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)
                        self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 0)
            self.cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s", (TEST_NO,))
            r = self.probe("mail-delete", postal)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 0)
            self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 1)

    def test_mail_bulk_after_logout_deletes_all_attachments(self):
        with self.offline_character(), self.attachment() as first, self.attachment() as second:
            self.cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s", (TEST_NO,))
            r = self.probe("mail-delete-all", TEST_NO)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertIn("在线或正在退出", r.stderr)
            for ui, postal in (first, second):
                self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)
                self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 0)
            self.cur.execute("UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s", (TEST_NO,))
            r = self.probe("mail-delete-all", TEST_NO)
            self.assertEqual(r.returncode, 0, r.stderr)
            for ui, postal in (first, second):
                self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 0)
                self.assertEqual(self.one("SELECT delete_flag FROM taiwan_cain_2nd.postal WHERE postal_id=%s", (postal,)), 1)

    def test_mail_does_not_delete_foreign_owner_attachment(self):
        with self.attachment() as (ui, postal):
            self.cur.execute("UPDATE taiwan_cain_2nd.user_items SET charac_no=%s WHERE ui_id=%s", (TEST_NO - 1, ui))
            r = self.probe("mail-delete", postal)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)

    @contextlib.contextmanager
    def equipment_fixture(self):
        tables = ('taiwan_cain.charac_stat', 'taiwan_cain.charac_manage_info')
        for table in tables:
            self.assertEqual(self.one('SELECT COUNT(*) FROM ' + table + ' WHERE charac_no=%s', (TEST_NO,)), 0)
        with self.offline_character():
            self.cur.execute('SELECT * FROM taiwan_cain.charac_stat WHERE charac_no=%s', (self.cno,))
            row = list(self.cur.fetchone()); cols = [d[0] for d in self.cur.description]
            row[cols.index('charac_no')] = TEST_NO
            row[cols.index('add_slot_flag')] = 0
            try:
                self.cur.execute('INSERT INTO taiwan_cain.charac_stat (' + ','.join(map(ident, cols)) +
                                 ') VALUES (' + ','.join(['%s'] * len(cols)) + ')', row)
                yield
            finally:
                for table in tables:
                    self.cur.execute('DELETE FROM ' + table + ' WHERE charac_no=%s', (TEST_NO,))

    def test_equipment_slots_offline_ownership_and_missing_record(self):
        with self.equipment_fixture():
            for _ in range(2):
                r = self.probe('equipment-slots', TEST_NO, TEST_NO)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(self.one('SELECT add_slot_flag FROM taiwan_cain.charac_stat WHERE charac_no=%s', (TEST_NO,)), 3)
            r = self.probe('equipment-slots', TEST_NO, TEST_NO + 1)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s', (TEST_NO,))
            r = self.probe('equipment-slots', TEST_NO, TEST_NO)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s', (TEST_NO,))
            self.cur.execute('DELETE FROM taiwan_cain.charac_stat WHERE charac_no=%s', (TEST_NO,))
            r = self.probe('equipment-slots', TEST_NO, TEST_NO)
            self.assertNotEqual(r.returncode, 0, r.stdout)

    def test_weight_offline_ownership_and_login_state(self):
        with self.offline_character():
            self.cur.execute('SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s', (TEST_NO,))
            before = self.cur.fetchone(); columns = [d[0] for d in self.cur.description]
            index = columns.index('inven_weight')
            for _ in range(2):
                r = self.probe('no-weight', TEST_NO, TEST_NO)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.cur.execute('SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s', (TEST_NO,))
                after = self.cur.fetchone()
                self.assertEqual(after[index], 99999999)
                self.assertEqual(after[:index] + after[index + 1:], before[:index] + before[index + 1:])
            r = self.probe('no-weight', TEST_NO, TEST_NO + 1)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s', (TEST_NO,))
            r = self.probe('no-weight', TEST_NO, TEST_NO)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0,inven_weight=35000 WHERE charac_no=%s', (TEST_NO,))
            for status in (1, 2):
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s', (status, TEST_NO))
                r = self.probe('no-weight', TEST_NO, TEST_NO)
                self.assertNotEqual(r.returncode, 0, r.stdout)
                self.assertEqual(self.one('SELECT inven_weight FROM taiwan_cain.charac_info WHERE charac_no=%s', (TEST_NO,)), 35000)

    def test_equipment_level_lazy_row_and_preserved_selection(self):
        with self.equipment_fixture():
            r = self.probe('equipment-level', TEST_NO, TEST_NO + 1, 85)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_cain.charac_manage_info WHERE charac_no=%s', (TEST_NO,)), 0)
            r = self.probe('equipment-level', TEST_NO, TEST_NO, 85)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.cur.execute('UPDATE taiwan_cain.charac_manage_info SET tag_charac_no=17,striker_skill_index=3 WHERE charac_no=%s', (TEST_NO,))
            for _ in range(2):
                r = self.probe('equipment-level', TEST_NO, TEST_NO, 70)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.cur.execute('SELECT tag_charac_no,striker_skill_index,max_equip_level FROM taiwan_cain.charac_manage_info WHERE charac_no=%s', (TEST_NO,))
                self.assertEqual(self.cur.fetchone(), (17, 3, 70))

    def test_equipment_leaving_account_never_falls_back_to_sql(self):
        with self.equipment_fixture():
            self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=2 WHERE m_id=%s', (TEST_NO,))
            for args in [('equipment-slots', TEST_NO, TEST_NO), ('equipment-level', TEST_NO, TEST_NO, 85)]:
                r = self.probe(*args)
                self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertEqual(self.one('SELECT add_slot_flag FROM taiwan_cain.charac_stat WHERE charac_no=%s', (TEST_NO,)), 0)
            self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_cain.charac_manage_info WHERE charac_no=%s', (TEST_NO,)), 0)

    def test_mail_bulk_failure_rolls_back(self):
        with self.attachment() as (ui, postal):
            with self.trigger("taiwan_cain_2nd.postal", "UPDATE",
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked postal'"):
                r = self.probe("mail-delete-all", TEST_NO)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)

    @contextlib.contextmanager
    def quest_fixture(self):
        tables = ('taiwan_cain.new_charac_quest', 'taiwan_cain.charac_quest_shop', 'taiwan_cain_2nd.inventory')
        with self.offline_character():
            for table in tables:
                self.assertEqual(self.one('SELECT COUNT(*) FROM '+table+' WHERE charac_no=%s', (TEST_NO,)), 0)
            try:
                for table in tables:
                    self.cur.execute('SELECT * FROM '+table+' WHERE charac_no=%s', (self.cno,))
                    row = self.cur.fetchone(); columns = [d[0] for d in self.cur.description]
                    if row is None:
                        self.cur.execute('INSERT INTO '+table+' (charac_no) VALUES (%s)', (TEST_NO,))
                    else:
                        row = list(row); row[columns.index('charac_no')] = TEST_NO
                        self.cur.execute('INSERT INTO '+table+' ('+','.join(map(ident, columns))+') VALUES ('+
                                         ','.join(['%s']*len(row))+')', row)
                self.cur.execute('UPDATE taiwan_cain.new_charac_quest SET clear_quest=%s,quest_notify=%s,'+
                                 ','.join('play_%d=%s,play_%d_trigger=%s' % (i, '%s', i, '%s') for i in range(1,21))+
                                 ' WHERE charac_no=%s',
                                 (struct.pack('<I',30000)+zlib.compress(b'\x01'*30000), b'\x01'*40)+
                                 tuple(x for i in range(1,21) for x in (100+i, i))+(TEST_NO,))
                plain = bytearray(61*4)
                for slot, (kind, item) in enumerate(((4,12345),(3,12345),(4,23456),(4,12345))):
                    plain[slot*61+1] = kind
                    struct.pack_into('<I',plain,slot*61+2,item)
                    struct.pack_into('<I',plain,slot*61+7,10)
                self.cur.execute('UPDATE taiwan_cain_2nd.inventory SET inventory=%s WHERE charac_no=%s',
                                 (struct.pack('<I',len(plain))+zlib.compress(plain), TEST_NO))
                yield
            finally:
                for table in tables:
                    self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s', (TEST_NO,))

    def quest_snapshot(self):
        rows = {}
        for table in ('taiwan_cain.new_charac_quest','taiwan_cain.charac_quest_shop','taiwan_cain_2nd.inventory'):
            self.cur.execute('SELECT * FROM '+table+' WHERE charac_no=%s', (TEST_NO,))
            rows[table] = dict(zip((d[0] for d in self.cur.description), self.cur.fetchone()))
        return rows

    def test_quest_init_nested_transaction(self):
        with self.quest_fixture():
            before = self.quest_snapshot()
            with self.trigger('taiwan_cain.new_charac_quest', 'UPDATE', 'BEGIN IF NEW.charac_no='+str(TEST_NO)+
                              " AND NEW.quest_notify=UNHEX(REPEAT('00',40)) THEN "
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked quest'; END IF; END"):
                r = self.probe('quest-init', TEST_NO, TEST_NO)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('audit blocked quest', r.stderr, 'must reach the injected write failure')
            self.assertEqual(self.quest_snapshot(), before, 'nested BLOB writes must roll back with the slots')

    def test_quest_clear_reset_and_exact_readback(self):
        with self.quest_fixture():
            before = self.quest_snapshot(); table = 'taiwan_cain.new_charac_quest'
            for command in ('quest-clear','quest-init'):
                for _ in range(2):
                    r = self.probe(command, TEST_NO, TEST_NO)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    after = self.quest_snapshot()
                    for name in before[table]:
                        if name.startswith('play_'): self.assertEqual(after[table][name], 0)
                        elif name == 'clear_quest' and command == 'quest-init':
                            self.assertEqual(struct.unpack('<I',after[table][name][:4])[0],30000)
                            self.assertEqual(zlib.decompress(after[table][name][4:]), bytes(30000))
                        elif name == 'quest_notify' and command == 'quest-init': self.assertEqual(after[table][name], bytes(40))
                        else: self.assertEqual(after[table][name], before[table][name])
                    self.assertEqual({k:v for k,v in after.items() if k!=table}, {k:v for k,v in before.items() if k!=table})
            # A same-length trigger rewrite must be detected, not merely length-checked.
            with self.trigger(table, 'UPDATE', 'BEGIN IF NEW.charac_no='+str(TEST_NO)+
                              " THEN SET NEW.quest_notify=UNHEX(CONCAT('01',REPEAT('00',39))); END IF; END"):
                before = self.quest_snapshot()
                r = self.probe('quest-init', TEST_NO, TEST_NO)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('回读不一致', r.stderr)
                self.assertEqual(self.quest_snapshot(), before)

    def test_quest_slot_stale_selection_and_atomic_purge(self):
        with self.quest_fixture():
            table='taiwan_cain.new_charac_quest'; inventory='taiwan_cain_2nd.inventory'
            for _ in range(2):
                r=self.probe('quest-slot',TEST_NO,TEST_NO,2,102,102,0)
                self.assertEqual(r.returncode,0,r.stderr)
            before=self.quest_snapshot()
            self.assertEqual(before[table]['play_2'],102);self.assertEqual(before[table]['play_2_trigger'],0)
            for slot,expected in ((0,101),(21,101),(1,999)):
                r=self.probe('quest-slot',TEST_NO,TEST_NO,slot,expected,0,0,12345)
                self.assertNotEqual(r.returncode,0);self.assertEqual(self.quest_snapshot(),before)
            with self.trigger(inventory,'UPDATE',"SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked purge'"):
                r=self.probe('quest-slot',TEST_NO,TEST_NO,1,101,0,0,12345)
            self.assertNotEqual(r.returncode,0);self.assertIn('audit blocked purge',r.stderr)
            self.assertEqual(self.quest_snapshot(),before,'failed purge must restore the abandoned quest')
            r=self.probe('quest-slot',TEST_NO,TEST_NO,1,101,0,0,12345)
            self.assertEqual(r.returncode,0,r.stderr);after=self.quest_snapshot()
            self.assertEqual((after[table]['play_1'],after[table]['play_1_trigger']),(0,0))
            self.assertEqual({k:v for k,v in after[table].items() if k not in ('play_1','play_1_trigger')},
                             {k:v for k,v in before[table].items() if k not in ('play_1','play_1_trigger')})
            raw=zlib.decompress(before[inventory]['inventory'][4:]);want=bytes(61)+raw[61:183]+bytes(61)
            self.assertEqual(zlib.decompress(after[inventory]['inventory'][4:]),want)
            self.assertEqual(after['taiwan_cain.charac_quest_shop'],before['taiwan_cain.charac_quest_shop'])

    def test_quest_guards_cover_all_entry_points(self):
        with self.quest_fixture():
            path=OUT/'quest-guard-template.ini'
            r=self.probe('quest-export',TEST_NO,TEST_NO,path);self.assertEqual(r.returncode,0,r.stderr)
            commands=[('quest-slot',1,101,0,0,12345),('quest-clear',),('quest-init',),('quest-import',path)]
            before=self.quest_snapshot()
            try:
                for command,*args in commands:
                    for cid,mid in ((TEST_NO,TEST_NO+1),(0,TEST_NO),(TEST_NO+1,TEST_NO)):
                        r=self.probe(command,cid,mid,*args);self.assertNotEqual(r.returncode,0,r.stdout)
                        self.assertEqual(self.quest_snapshot(),before)
                    self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s',(TEST_NO,))
                    r=self.probe(command,TEST_NO,TEST_NO,*args);self.assertNotEqual(r.returncode,0)
                    self.assertEqual(self.quest_snapshot(),before)
                    self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s',(TEST_NO,))
                    for status in (1,2,-1):
                        self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s',(status,TEST_NO))
                        r=self.probe(command,TEST_NO,TEST_NO,*args);self.assertNotEqual(r.returncode,0,r.stdout)
                        self.assertEqual(self.quest_snapshot(),before)
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(TEST_NO,))
            finally:
                path.unlink(missing_ok=True)

    def test_quest_import_rollback_and_clamped_value(self):
        with self.quest_fixture():
            path=OUT/'quest-rollback-template.ini';before=self.quest_snapshot()
            try:
                path.write_text('# DNF GM 任务模版 v1\n[taiwan_cain.new_charac_quest]\nplay_1=555\n'
                                '[taiwan_cain.charac_quest_shop]\nqp=777\n',encoding='utf8')
                with self.trigger('taiwan_cain.charac_quest_shop','INSERT',
                                  "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked import'"):
                    r=self.probe('quest-import',TEST_NO,TEST_NO,path)
                self.assertNotEqual(r.returncode,0);self.assertIn('audit blocked import',r.stderr)
                self.assertEqual(self.quest_snapshot(),before,'the second table failure must restore both deleted rows')
                path.write_text('# DNF GM 任务模版 v1\n[taiwan_cain.charac_quest_shop]\nqp=-1\n',encoding='utf8')
                r=self.probe('quest-import',TEST_NO,TEST_NO,path,mode='')
                self.assertNotEqual(r.returncode,0,'silent unsigned clamping must fail readback')
                self.assertEqual(self.quest_snapshot(),before)
                r=self.probe('quest-export',TEST_NO,TEST_NO,path);self.assertEqual(r.returncode,0,r.stderr)
                for _ in range(2):
                    r=self.probe('quest-import',TEST_NO,TEST_NO,path);self.assertEqual(r.returncode,0,r.stderr)
                    self.assertEqual(self.quest_snapshot(),before)
            finally:
                path.unlink(missing_ok=True)

    def test_quest_login_change_while_waiting_for_account_lock(self):
        with self.quest_fixture():
            path=OUT/'quest-race-template.ini'
            r=self.probe('quest-export',TEST_NO,TEST_NO,path);self.assertEqual(r.returncode,0,r.stderr)
            before=self.quest_snapshot()
            commands=[('quest-slot',1,101,0,0,12345),('quest-clear',),('quest-init',),('quest-import',path)]
            lock=pymysql.connect(host=self.params['host'],port=int(self.params['port']),user=self.params['user'],
                                 password=self.params['pwd'],autocommit=False)
            try:
                for command,*args in commands:
                    pending=None
                    try:
                        with lock.cursor() as q:q.execute('SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s FOR UPDATE',(TEST_NO,))
                        env=dict(os.environ,**{'DNF_GM_TEST_'+k.upper():v for k,v in self.params.items()})
                        pending=subprocess.Popen([str(PROBE),command,str(TEST_NO),str(TEST_NO),*map(str,args)],
                                                 cwd=OUT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                        deadline=time.monotonic()+10
                        while time.monotonic()<deadline:
                            if self.one("SELECT COUNT(*) FROM information_schema.PROCESSLIST WHERE INFO LIKE %s",
                                        ('SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id='+str(TEST_NO)+' FOR UPDATE',)):break
                            self.assertIsNone(pending.poll());time.sleep(.05)
                        else:self.fail('quest write never reached the controlled login race')
                        with lock.cursor() as q:q.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s',(TEST_NO,))
                        lock.commit();out,err=pending.communicate(timeout=30)
                        self.assertNotEqual(pending.returncode,0,out)
                        self.assertIn('在线或正在退出',err.decode('utf8'))
                        self.assertEqual(self.quest_snapshot(),before)
                    finally:
                        lock.rollback()
                        if pending is not None and pending.poll() is None:pending.communicate(timeout=30)
                        self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(TEST_NO,))
            finally:
                lock.close();path.unlink(missing_ok=True)

    def test_quest_login_waits_until_edit_commits_even_without_login_row(self):
        with self.quest_fixture():
            for missing in (False, True):
                if missing:self.cur.execute('DELETE FROM taiwan_login.login_account_3 WHERE m_id=%s',(TEST_NO,))
                lock=pymysql.connect(host=self.params['host'],port=int(self.params['port']),user=self.params['user'],
                                     password=self.params['pwd'],autocommit=False)
                pending=None;thread=None;done=threading.Event();errors=[]
                try:
                    with lock.cursor() as q:q.execute('SELECT play_1 FROM taiwan_cain.new_charac_quest WHERE charac_no=%s FOR UPDATE',(TEST_NO,))
                    env=dict(os.environ,**{'DNF_GM_TEST_'+k.upper():v for k,v in self.params.items()})
                    pending=subprocess.Popen([str(PROBE),'quest-slot',str(TEST_NO),str(TEST_NO),'1','101','101','0'],
                                             cwd=OUT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                    query='SELECT play_1 FROM taiwan_cain.new_charac_quest WHERE charac_no='+str(TEST_NO)+' FOR UPDATE'
                    deadline=time.monotonic()+10
                    while time.monotonic()<deadline:
                        if self.one('SELECT COUNT(*) FROM information_schema.PROCESSLIST WHERE INFO=%s',(query,)):break
                        self.assertIsNone(pending.poll());time.sleep(.05)
                    else:self.fail('quest write did not reach the controlled row lock')
                    def login():
                        db=None
                        try:
                            db=pymysql.connect(host=self.params['host'],port=int(self.params['port']),user=self.params['user'],
                                               password=self.params['pwd'],autocommit=True,read_timeout=20)
                            with db.cursor() as q:
                                if missing:q.execute('INSERT INTO taiwan_login.login_account_3 (m_id,login_status,last_login_date) VALUES (%s,1,NOW())',(TEST_NO,))
                                else:q.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s',(TEST_NO,))
                        except Exception as exc:errors.append(str(exc))
                        finally:
                            if db:db.close()
                            done.set()
                    thread=threading.Thread(target=login);thread.start()
                    self.assertFalse(done.wait(.3),'login must not pass a task edit holding the account lock')
                    lock.commit();out,err=pending.communicate(timeout=30)
                    self.assertEqual(pending.returncode,0,err.decode('utf8'))
                    self.assertTrue(done.wait(10));self.assertFalse(errors,errors)
                    self.assertEqual(self.one('SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s',(TEST_NO,)),1)
                finally:
                    lock.rollback();lock.close()
                    if pending is not None and pending.poll() is None:pending.communicate(timeout=30)
                    if thread is not None:thread.join(timeout=25)
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(TEST_NO,))

    def test_truncated_character_backup_is_rejected(self):
        path = OUT / "truncated-character.sql"
        path.write_text("-- DNF GM 角色备份 · audit\nSELECT 1;\n", encoding="utf-8")
        try:
            r = self.probe("restore", path)
            self.assertNotEqual(r.returncode, 0, "character backups also require their completion marker")
        finally:
            path.unlink(missing_ok=True)

    def test_level_punishment_failure_rolls_back(self):
        table = "d_taiwan.member_punish_info"
        with self.level_fixture() as before:
            self.cur.execute("INSERT INTO " + table + " (m_id) VALUES (%s)", (TEST_NO,))
            with self.trigger(table, "DELETE", "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked punish'"):
                r = self.probe("level-exp", TEST_NO, TEST_NO, 60, 150172894)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertIn("清除处罚失败", r.stderr)
            self.assertEqual(self.level_values(), before)
            self.assertEqual(self.one("SELECT COUNT(*) FROM " + table + " WHERE m_id=%s", (TEST_NO,)), 1)

    @contextlib.contextmanager
    def level_fixture(self):
        with self.equipment_fixture():
            self.assertEqual(self.one("SELECT COUNT(*) FROM d_taiwan.member_punish_info WHERE m_id=%s", (TEST_NO,)), 0)
            try:
                yield self.level_values()
            finally:
                self.cur.execute("DELETE FROM d_taiwan.member_punish_info WHERE m_id=%s", (TEST_NO,))

    def level_values(self):
        self.cur.execute("SELECT c.lev,c.exp,s.exp FROM taiwan_cain.charac_info c "
                         "JOIN taiwan_cain.charac_stat s USING(charac_no) WHERE c.charac_no=%s", (TEST_NO,))
        return self.cur.fetchone()

    def test_level_updates_both_experience_columns(self):
        with self.level_fixture():
            before = {}
            for table, changed in [('charac_info', ('lev', 'exp')), ('charac_stat', ('exp',))]:
                self.cur.execute('SELECT * FROM taiwan_cain.' + table + ' WHERE charac_no=%s', (TEST_NO,))
                row = self.cur.fetchone(); cols = [d[0] for d in self.cur.description]
                before[table] = {k: v for k, v in zip(cols, row) if k not in changed}
            for _ in range(2):
                r = self.probe("level-exp", TEST_NO, TEST_NO, 60, 150172894)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(self.level_values(), (60, 150172894, 150172894))
            for table in before:
                self.cur.execute('SELECT * FROM taiwan_cain.' + table + ' WHERE charac_no=%s', (TEST_NO,))
                row = self.cur.fetchone(); cols = [d[0] for d in self.cur.description]
                self.assertEqual({k: v for k, v in zip(cols, row) if k in before[table]}, before[table])

    def test_level_second_table_failure_rolls_back_first(self):
        with self.level_fixture() as before:
            with self.trigger("taiwan_cain.charac_stat", "UPDATE",
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked experience'"):
                r = self.probe("level-exp", TEST_NO, TEST_NO, 60, 150172894)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("audit blocked experience", r.stderr)
            self.assertEqual(self.level_values(), before)

    def test_level_out_of_range_never_clips_or_writes(self):
        with self.level_fixture() as before:
            for level, exp in ((0, 0), (128, 0), (60, -1), (94, 2147483648)):
                r = self.probe("level-exp", TEST_NO, TEST_NO, level, exp)
                self.assertNotEqual(r.returncode, 0, (level, exp, r.stdout))
                self.assertEqual(self.level_values(), before)

    def test_level_rejects_wrong_owner_deleted_and_login_state(self):
        with self.level_fixture() as before:
            self.cur.execute('INSERT INTO d_taiwan.member_punish_info (m_id) VALUES (%s)', (TEST_NO,))
            for owner, deleted, status in [(TEST_NO + 1, 0, 0), (TEST_NO, 1, 0),
                                           (TEST_NO, 0, 1), (TEST_NO, 0, 2)]:
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=%s WHERE charac_no=%s', (deleted, TEST_NO))
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s', (status, TEST_NO))
                r = self.probe('level-exp', TEST_NO, owner, 60, 150172894)
                self.assertNotEqual(r.returncode, 0, (owner, deleted, status, r.stdout))
                self.assertEqual(self.level_values(), before)
                self.assertEqual(self.one('SELECT COUNT(*) FROM d_taiwan.member_punish_info WHERE m_id=%s', (TEST_NO,)), 1)

    def test_level_missing_stat_does_not_change_info_or_penalty(self):
        with self.level_fixture() as before:
            self.cur.execute('DELETE FROM taiwan_cain.charac_stat WHERE charac_no=%s', (TEST_NO,))
            self.cur.execute('INSERT INTO d_taiwan.member_punish_info (m_id) VALUES (%s)', (TEST_NO,))
            r = self.probe('level-exp', TEST_NO, TEST_NO, 60, 150172894)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute('SELECT lev,exp FROM taiwan_cain.charac_info WHERE charac_no=%s', (TEST_NO,))
            self.assertEqual(self.cur.fetchone(), before[:2])
            self.assertEqual(self.one('SELECT COUNT(*) FROM d_taiwan.member_punish_info WHERE m_id=%s', (TEST_NO,)), 1)

    def test_level_readback_mismatch_rolls_back(self):
        with self.level_fixture() as before:
            with self.trigger('taiwan_cain.charac_stat', 'UPDATE', 'SET NEW.exp=NEW.exp+1'):
                r = self.probe('level-exp', TEST_NO, TEST_NO, 60, 150172894)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertIn('等级/经验未确认', r.stderr)
            self.assertEqual(self.level_values(), before)

    def test_offline_gate_rechecks_raw_account_status(self):
        table = "taiwan_login.login_account_3"
        self.assertEqual(self.one("SELECT COUNT(*) FROM " + table + " WHERE m_id=%s", (TEST_NO,)), 0)
        try:
            self.cur.execute("INSERT INTO " + table + " (m_id,login_status) VALUES (%s,0)", (TEST_NO,))
            self.assertEqual(self.probe("offline", TEST_NO, TEST_NO).returncode, 0)
            # A cached UI/offline count must never override a new login or transition.
            for status in (1, 2):
                self.cur.execute("UPDATE " + table + " SET login_status=%s WHERE m_id=%s", (status, TEST_NO))
                r = self.probe("offline", TEST_NO, TEST_NO)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("未写入", r.stderr)
            self.cur.execute("UPDATE " + table + " SET login_status=0 WHERE m_id=%s", (TEST_NO,))
            self.assertEqual(self.probe("offline", TEST_NO, TEST_NO).returncode, 0)
        finally:
            self.cur.execute("DELETE FROM " + table + " WHERE m_id=%s", (TEST_NO,))

    def test_rename_no_backslash_escapes(self):
        name = self.one("SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
        try:
            r = self.probe("rename", self.cno, self.mid, "audit'\\name", mode="NO_BACKSLASH_ESCAPES")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.one("SELECT charac_name FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,)), "audit'\\name")
        finally:
            self.cur.execute("UPDATE taiwan_cain.charac_info SET charac_name=%s WHERE charac_no=%s", (name, self.cno))

    def test_restore_failure_restores_session(self):
        path = OUT / "bad-session.sql"
        path.write_text("SET FOREIGN_KEY_CHECKS=0; SET SQL_MODE=''; INVALID SQL;", encoding="utf-8")
        try:
            r = self.probe("restore", path, mode="NO_BACKSLASH_ESCAPES")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("session_foreign_keys=1", r.stdout)
            self.assertIn("session_mode=NO_BACKSLASH_ESCAPES", r.stdout)
        finally:
            path.unlink(missing_ok=True)

    def test_restore_embedded_nul_is_rejected(self):
        path = OUT / "embedded-nul.sql"
        path.write_bytes(b"SELECT 1\x00;")
        try:
            r = self.probe("restore", path)
            self.assertNotEqual(r.returncode, 0, "embedded NUL must not silently truncate SQL")
            self.assertIn("NUL", r.stderr)
        finally:
            path.unlink(missing_ok=True)

    def test_backup_roundtrip_bit_generated_and_zero_key(self):
        table = "taiwan_game_event.codex_audit_types"
        path = OUT / "audit-full.sql"
        fragment = OUT / "audit-table.sql"
        self.assertEqual(self.one("SELECT COUNT(*) FROM information_schema.TABLES "
                                  "WHERE TABLE_SCHEMA='taiwan_game_event' AND TABLE_NAME='codex_audit_types'"), 0)
        try:
            self.cur.execute("CREATE TABLE " + table + " (id INT AUTO_INCREMENT PRIMARY KEY, "
                             "flags BIT(9),txt VARCHAR(100) CHARACTER SET utf8, "
                             "computed INT GENERATED ALWAYS AS (LENGTH(txt)) STORED) ENGINE=InnoDB")
            self.cur.execute("SET SESSION sql_mode='NO_AUTO_VALUE_ON_ZERO'")
            self.cur.execute("INSERT INTO " + table + " (id,flags,txt) VALUES (0,b'100000001',%s)",
                             ("quote' slash\\ newline\n 中文",))
            self.cur.execute("SELECT * FROM " + table); original = self.cur.fetchall()
            r = self.probe("dump", path)
            self.assertEqual(r.returncode, 0, r.stderr)
            text = path.read_text(encoding="utf-8")
            chunk = text.split("-- " + table + "\n", 1)[1].split("\n-- ", 1)[0]
            header = text.split("\n-- ", 1)[0]
            fragment.write_text(header + "\n" + chunk + "\nSET FOREIGN_KEY_CHECKS=1;\n", encoding="utf-8")
            self.cur.execute("DELETE FROM " + table)
            r = self.probe("restore", fragment, mode="NO_BACKSLASH_ESCAPES")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.cur.execute("SELECT * FROM " + table)
            self.assertEqual(self.cur.fetchall(), original)
        finally:
            self.cur.execute("DROP TABLE IF EXISTS " + table)
            self.cur.execute("SET SESSION sql_mode='NO_ENGINE_SUBSTITUTION'")
            path.unlink(missing_ok=True); fragment.unlink(missing_ok=True)

    def test_cancelled_backup_preserves_previous_file(self):
        path = OUT / "previous-backup.sql"
        original = b"previous verified backup"
        path.write_bytes(original)
        try:
            r = self.probe("dump", path, extra_env={"DNF_GM_TEST_CANCEL_PCT": "0"})
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse(list(OUT.glob(path.name + ".*.part")))
        finally:
            path.unlink(missing_ok=True)

    @contextlib.contextmanager
    def clone_view(self, plain):
        self.cur.execute("SELECT * FROM taiwan_cain.charac_view WHERE m_id=%s", (self.mid,))
        row = self.cur.fetchone()
        cols = [d[0] for d in self.cur.description]
        self.cur.execute("SELECT charac_no FROM taiwan_cain.charac_info")
        original_ids = {r[0] for r in self.cur.fetchall()}
        self.cur.execute("SELECT c.TABLE_SCHEMA,c.TABLE_NAME,t.AUTO_INCREMENT "
                         "FROM information_schema.COLUMNS c JOIN information_schema.TABLES t "
                         "ON t.TABLE_SCHEMA=c.TABLE_SCHEMA AND t.TABLE_NAME=c.TABLE_NAME "
                         "WHERE c.COLUMN_NAME='charac_no' AND t.TABLE_TYPE='BASE TABLE' "
                         "AND c.TABLE_SCHEMA IN ('taiwan_cain','taiwan_cain_2nd')")
        tables = self.cur.fetchall()
        try:
            self.cur.execute("UPDATE taiwan_cain.charac_view SET info=%s WHERE m_id=%s",
                             (struct.pack('<I', len(plain)) + zlib.compress(plain), self.mid))
            yield original_ids
        finally:
            self.cur.execute("SELECT charac_no FROM taiwan_cain.charac_info")
            added = {r[0] for r in self.cur.fetchall()} - original_ids
            for schema, table, counter in tables:
                if added:
                    self.cur.execute("DELETE FROM " + ident(schema + '.' + table) + " WHERE charac_no IN %s", (tuple(added),))
                if counter is not None:
                    self.cur.execute("ALTER TABLE " + ident(schema + '.' + table) + " AUTO_INCREMENT=" + str(counter))
            self.cur.execute("UPDATE taiwan_cain.charac_view SET " +
                             ','.join(ident(c) + '=%s' for c in cols if c != 'm_id') + " WHERE m_id=%s",
                             [v for c, v in zip(cols, row) if c != 'm_id'] + [self.mid])

    def test_full_clone_view_rolls_back(self):
        slot = bytearray(296); struct.pack_into('<I', slot, 0, self.cno)
        with self.clone_view(bytes(slot) * 4) as original_ids:
            r = self.probe("clone", self.cno, self.mid)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.cur.execute("SELECT charac_no FROM taiwan_cain.charac_info")
            self.assertEqual({r[0] for r in self.cur.fetchall()}, original_ids)

    def test_clone_missing_source_slot_rolls_back(self):
        with self.clone_view(bytearray(296 * 3)) as original_ids:
            r = self.probe("clone", self.cno, self.mid)
            self.assertNotEqual(r.returncode, 0, "a missing source slot must not invent unknown template bytes")
            self.cur.execute("SELECT charac_no FROM taiwan_cain.charac_info")
            self.assertEqual({row[0] for row in self.cur.fetchall()}, original_ids)

    def test_clone_uses_selected_source_slot(self):
        plain = bytearray(296 * 4)
        struct.pack_into('<I', plain, 0, TEST_NO); plain[100] = 17
        struct.pack_into('<I', plain, 296 * 3, self.cno); plain[296 * 3 + 100] = 99
        with self.clone_view(plain):
            r = self.probe("clone", self.cno, self.mid)
            self.assertEqual(r.returncode, 0, r.stderr)
            new_no = int(next(s.split('=')[1] for s in r.stdout.splitlines() if s.startswith('new_no=')))
            blob = self.one("SELECT info FROM taiwan_cain.charac_view WHERE m_id=%s", (self.mid,))
            slots = zlib.decompress(blob[4:])
            found = [slots[i:i+296] for i in range(0,len(slots),296) if struct.unpack_from('<I',slots,i)[0] == new_no]
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0][100], 99)

    def test_clone_name_slot_uses_original_game_bytes(self):
        before = self.one("SELECT HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
        plain = bytearray(296 * 4)
        struct.pack_into('<I', plain, 0, self.cno)
        try:
            r = self.probe('rename', self.cno, self.mid, '编码克隆')
            self.assertEqual(r.returncode, 0, r.stderr)
            with self.clone_view(plain):
                r = self.probe('clone', self.cno, self.mid)
                self.assertEqual(r.returncode, 0, r.stderr)
                new_no = int(next(s.split('=')[1] for s in r.stdout.splitlines() if s.startswith('new_no=')))
                name = bytes.fromhex(self.one('SELECT HEX(CONVERT(charac_name USING latin1)) FROM taiwan_cain.charac_info WHERE charac_no=%s', (new_no,)))
                self.assertTrue(name.decode('utf8').startswith('编码克隆'))
                blob = self.one('SELECT info FROM taiwan_cain.charac_view WHERE m_id=%s', (self.mid,))
                slots = zlib.decompress(blob[4:])
                found = [slots[i+4:i+24].split(b'\0')[0] for i in range(0, len(slots), 296)
                         if struct.unpack_from('<I', slots, i)[0] == new_no]
                self.assertEqual(found, [name])
        finally:
            self.cur.execute('UPDATE taiwan_cain.charac_info SET charac_name=UNHEX(%s) WHERE charac_no=%s', (before, self.cno))

    def test_protocol_exact_packet_boundary(self):
        path = OUT / 'packet-boundary.sql'
        head, tail = "SELECT LENGTH('", "')"
        length = 0xffffff - 1 - len(head) - len(tail)
        path.write_text(head + 'a' * length + tail, encoding='ascii')
        try:
            r = self.probe('query', path, timeout=10)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('value=' + str(length), r.stdout)
        finally:
            path.unlink(missing_ok=True)

    def test_character_backup_roundtrip(self):
        path = OUT / 'character-roundtrip.sql'
        self.cur.execute('SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s', (self.cno,))
        char_before = self.cur.fetchone()
        char_cols = [d[0] for d in self.cur.description]
        self.cur.execute('SELECT * FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (self.cno,))
        inv_before = self.cur.fetchone()
        inv_cols = [d[0] for d in self.cur.description]
        plain = bytearray(296 * 4)
        struct.pack_into('<I', plain, 0, self.cno)
        try:
            with self.clone_view(plain):
                r = self.probe('char-dump', self.cno, self.mid, path)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET lev=42 WHERE charac_no=%s', (self.cno,))
                self.cur.execute('UPDATE taiwan_cain_2nd.inventory SET money=12345 WHERE charac_no=%s', (self.cno,))
                r = self.probe('restore', path)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.cur.execute('SELECT * FROM taiwan_cain.charac_info WHERE charac_no=%s', (self.cno,))
                self.assertEqual(self.cur.fetchone(), char_before)
                self.cur.execute('SELECT * FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (self.cno,))
                self.assertEqual(self.cur.fetchone(), inv_before)
        finally:
            self.cur.execute('UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=%s',
                             (char_before[char_cols.index('lev')], self.cno))
            self.cur.execute('UPDATE taiwan_cain_2nd.inventory SET money=%s WHERE charac_no=%s',
                             (inv_before[inv_cols.index('money')], self.cno))
            path.unlink(missing_ok=True)

    @contextlib.contextmanager
    def bubble_fixture(self, live=2):
        mid, empty = TEST_NO, TEST_NO + 1
        ids = list(range(TEST_NO + 10, TEST_NO + 11 + live))
        tables = ['d_taiwan.accounts', 'taiwan_cain.charac_info',
                  'taiwan_cain_2nd.postal', 'taiwan_cain_2nd.letter',
                  'taiwan_cain_2nd.user_items', 'taiwan_cain_2nd.creature_items']
        counters = {t: self.one('SELECT AUTO_INCREMENT FROM information_schema.TABLES '
                               'WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s', t.split('.')) for t in tables}
        self.cur.execute('SELECT charac_no,coin FROM taiwan_cain_2nd.inventory')
        coins = self.cur.fetchall()
        self.cur.execute('SELECT DISTINCT m_id FROM taiwan_cain.charac_info')
        mids = [str(r[0]) for r in self.cur.fetchall()] + [str(mid), str(empty)]
        placeholders = ','.join(['%s'] * len(mids))
        cash = {}
        for table in ('cash_cera', 'cash_cera_point'):
            self.cur.execute('SELECT * FROM taiwan_billing.' + table + ' WHERE account IN (' + placeholders + ')', mids)
            cash[table] = ([d[0] for d in self.cur.description], self.cur.fetchall())
        for table, col, nums in [('d_taiwan.accounts', 'UID', [mid, empty]),
                                 ('taiwan_login.login_account_3', 'm_id', [mid, empty]),
                                 ('taiwan_cain.charac_info', 'charac_no', ids),
                                 ('taiwan_cain_2nd.inventory', 'charac_no', ids),
                                 ('taiwan_cain_2nd.postal', 'receive_charac_no', ids),
                                 ('taiwan_cain_2nd.letter', 'charac_no', ids),
                                 ('taiwan_cain_2nd.user_items', 'charac_no', ids),
                                 ('taiwan_cain_2nd.creature_items', 'charac_no', ids)]:
            self.assertEqual(self.one('SELECT COUNT(*) FROM ' + table + ' WHERE ' + col +
                                     ' IN (' + ','.join(map(str, nums)) + ')'), 0, 'reserved fixture IDs already used')
        for table in cash:
            self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_billing.' + table +
                                     ' WHERE account IN (%s,%s)', (str(mid), str(empty))), 0)
        try:
            self.cur.executemany('INSERT INTO d_taiwan.accounts (UID,accountname) VALUES (%s,%s)',
                                 [(mid, 'audit_bubble'), (empty, 'audit_bubble_empty')])
            self.cur.execute('INSERT INTO taiwan_login.login_account_3 (m_id,login_status,last_login_date) '
                             'VALUES (%s,0,NOW())', (mid,))
            for table, key in [('taiwan_cain.charac_info', 'charac_no'),
                               ('taiwan_cain_2nd.inventory', 'charac_no')]:
                self.cur.execute('SELECT * FROM ' + table + ' WHERE charac_no=%s', (self.cno,))
                template = list(self.cur.fetchone())
                cols = [d[0] for d in self.cur.description]
                rows = []
                for i, no in enumerate(ids):
                    row = template.copy(); row[cols.index(key)] = no
                    if table.endswith('charac_info'):
                        row[cols.index('m_id')] = mid
                        row[cols.index('charac_name')] = 'audit_bub_' + str(i)
                        row[cols.index('delete_flag')] = int(i == live)
                    else:
                        row[cols.index('coin')] = 10
                    rows.append(row)
                self.cur.executemany('INSERT INTO ' + table + ' (' + ','.join(map(ident, cols)) +
                                     ') VALUES (' + ','.join(['%s'] * len(cols)) + ')', rows)
            yield ids, mid, empty
        finally:
            for table, col in [('taiwan_cain_2nd.postal', 'receive_charac_no'),
                               ('taiwan_cain_2nd.letter', 'charac_no'),
                               ('taiwan_cain_2nd.user_items', 'charac_no'),
                               ('taiwan_cain_2nd.creature_items', 'charac_no'),
                               ('taiwan_cain_2nd.inventory', 'charac_no'),
                               ('taiwan_cain.charac_info', 'charac_no')]:
                self.cur.execute('DELETE FROM ' + table + ' WHERE ' + col + ' IN (' + ','.join(map(str, ids)) + ')')
            self.cur.execute('DELETE FROM taiwan_login.login_account_3 WHERE m_id IN (%s,%s)', (mid, empty))
            self.cur.execute('DELETE FROM d_taiwan.accounts WHERE UID IN (%s,%s)', (mid, empty))
            self.cur.executemany('UPDATE taiwan_cain_2nd.inventory SET coin=%s WHERE charac_no=%s',
                                 [(coin, no) for no, coin in coins])
            for table, (cols, rows) in cash.items():
                full = 'taiwan_billing.' + table
                self.cur.execute('DELETE FROM ' + full + ' WHERE account IN (' + placeholders + ')', mids)
                if rows:
                    self.cur.executemany('INSERT INTO ' + full + ' (' + ','.join(map(ident, cols)) +
                                         ') VALUES (' + ','.join(['%s'] * len(cols)) + ')', rows)
            for table, counter in counters.items():
                if counter:
                    self.cur.execute('ALTER TABLE ' + ident(table) + ' AUTO_INCREMENT=' + str(counter))

    def bubble(self, target=3, tno=TEST_NO, coin=0, dp=0, dc=0, item=0, num=1,
               avatar=0, creature=0, **kwargs):
        return self.probe('bubble', target, tno, coin, dp, dc, item, num, avatar, creature, **kwargs)

    def bubble_paid(self, result, chars, accounts):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('paid_chars=%d paid_accounts=%d' % (chars, accounts), result.stdout)

    def bubble_state(self):
        state = []
        for sql in ['SELECT charac_no,coin FROM taiwan_cain_2nd.inventory ORDER BY charac_no',
                    'SELECT * FROM taiwan_billing.cash_cera ORDER BY account',
                    'SELECT * FROM taiwan_billing.cash_cera_point ORDER BY account',
                    *['SELECT * FROM taiwan_cain_2nd.' + t + ' ORDER BY 1'
                      for t in ('postal', 'letter', 'user_items', 'creature_items')]]:
            self.cur.execute(sql); state.append(self.cur.fetchall())
        return state

    def test_bubble_missing_cash_strict_repeat_and_counts(self):
        with self.bubble_fixture() as (ids, mid, _):
            for _ in range(2):
                self.bubble_paid(self.bubble(coin=3, dp=5, dc=7, mode='STRICT_TRANS_TABLES'), 2, 1)
            self.assertEqual(self.one('SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s', (str(mid),)), 10)
            self.assertEqual(self.one('SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s', (str(mid),)), 14)
            for no in ids[:2]:
                self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (no,)), 16)
            self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[-1],)), 10)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_billing.cash_cera WHERE account=%s "
                                     "AND reg_date>'2000-01-01' AND mod_date>'2000-01-01'", (str(mid),)), 1)

    def test_bubble_account_excludes_deleted_and_orphan_accounts(self):
        with self.bubble_fixture() as (ids, mid, _):
            self.cur.execute('UPDATE taiwan_cain.charac_info SET m_id=%s WHERE charac_no=%s', (mid + 2, ids[1]))
            self.bubble_paid(self.bubble(target=3, coin=1, dp=1), 1, 1)
            self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[0],)), 11)
            self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[1],)), 10)

    def test_bubble_all_targets_match_live_roles_and_unique_accounts(self):
        with self.bubble_fixture() as (ids, mid, _):
            self.cur.execute('SELECT c.charac_no,c.m_id,i.coin FROM taiwan_cain.charac_info c '
                             'JOIN d_taiwan.accounts a ON a.UID=c.m_id '
                             'JOIN taiwan_cain_2nd.inventory i ON i.charac_no=c.charac_no WHERE c.delete_flag=0')
            rows = self.cur.fetchall()
            self.bubble_paid(self.bubble(target=1, coin=2, dp=3), len(rows), len({r[1] for r in rows}))
            for no, _, coin in rows:
                self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (no,)), coin + 2)
            self.assertEqual(self.one('SELECT coin FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[-1],)), 10)

    def test_bubble_sql_online_flags_are_not_active_game_characters(self):
        with self.bubble_fixture() as (ids, mid, second):
            self.cur.execute('UPDATE taiwan_cain.charac_info SET m_id=%s WHERE charac_no=%s', (second, ids[1]))
            self.cur.execute('INSERT INTO taiwan_login.login_account_3 (m_id,login_status,last_login_date) '
                             'VALUES (%s,1,NOW())', (second,))
            self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s', (mid,))
            state=self.bubble_state()
            r=self.bubble(target=0,coin=1,dp=2,dc=3,item=1001,num=4)
            self.assertNotEqual(r.returncode,0)
            self.assertEqual(self.bubble_state(),state)

    @unittest.skipUnless(os.environ.get('DNF_GM_TEST_LIVE_BRIDGE'),
                         'requires an isolated live original-game bridge and no other online accounts')
    def test_live_bubble_skips_inactive_accounts_without_rewarding_or_mutating_them(self):
        self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_login.login_account_3 WHERE login_status<>0'), 0)
        channel=self.one('SELECT channel_no FROM d_taiwan.dnf_gm_hosts_v1 '
                         'WHERE seen>DATE_SUB(NOW(6),INTERVAL 5 SECOND) ORDER BY channel_no LIMIT 1')
        with self.bubble_fixture() as (ids,mid,empty):
            self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1,m_channel_no=%s WHERE m_id=%s',
                             (channel,mid))
            self.cur.execute('INSERT INTO taiwan_login.login_account_3 (m_id,login_status,m_channel_no,last_login_date) '
                             'VALUES (%s,1,%s,NOW())',(empty,channel))
            state=self.bubble_state()
            self.bubble_paid(self.bubble(target=0,coin=1,dp=2,dc=3,item=1001,num=4),0,0)
            self.assertEqual(self.bubble_state(),state)
            result=self.probe('add-coin',ids[0],mid,3)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('wrong_active_character',result.stderr)
            self.assertEqual(self.bubble_state(),state)

    def test_bubble_deleted_or_missing_explicit_target_rejected(self):
        with self.bubble_fixture() as (ids, mid, _):
            state = self.bubble_state()
            for target, no in [(2, ids[-1]), (2, mid + 9), (3, mid + 2)]:
                r = self.bubble(target=target, tno=no, coin=1, dp=1, dc=1)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('paid_chars=0 paid_accounts=0', r.stdout)
                self.assertEqual(self.bubble_state(), state)

    def test_bubble_valid_empty_account_can_receive_account_reward(self):
        with self.bubble_fixture() as (_, _, empty):
            self.bubble_paid(self.bubble(tno=empty, coin=1, dp=2, dc=3), 0, 1)
            self.assertEqual(self.one('SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s', (str(empty),)), 2)

    def test_bubble_empty_online_is_success_without_orphan_payment(self):
        with self.bubble_fixture() as (_, mid, _):
            self.cur.execute('DELETE FROM taiwan_cain.charac_info WHERE m_id=%s', (mid,))
            state = self.bubble_state()
            self.bubble_paid(self.bubble(target=0, dp=1), 0, 0)
            self.assertEqual(self.bubble_state(), state)

    def test_bubble_coin_overflow_rolls_back_nonstrict(self):
        with self.bubble_fixture() as (ids, _, _):
            self.cur.execute('UPDATE taiwan_cain_2nd.inventory SET coin=4294967295 WHERE charac_no=%s', (ids[1],))
            state = self.bubble_state()
            r = self.bubble(coin=1, dp=1, dc=1, mode='')
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.bubble_state(), state)

    def test_balance_increment_zero_overflow_and_owner_guard(self):
        with self.bubble_fixture() as (ids, mid, _):
            for field, maximum in [('coin', 4294967295), ('money', 2147483647)]:
                for mode in ('', 'STRICT_TRANS_TABLES'):
                    with self.subTest(field=field, mode=mode):
                        def balance():
                            return self.one('SELECT '+field+' FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[0],))
                        for args, expected in [((field, ids[0], mid, 10), 10),
                                               (('add-'+field, ids[0], mid, 0), 10),
                                               (('add-'+field, ids[0], mid, 7), 17),
                                               ((field, ids[0], mid, 17), 17),
                                               ((field, ids[0], mid, maximum), maximum)]:
                            r=self.probe(*args, mode=mode)
                            self.assertEqual(r.returncode, 0, r.stderr)
                            self.assertEqual(balance(), expected)
                        for args in [('add-'+field, ids[0], mid, 1),
                                     (field, ids[0], mid, maximum+1),
                                     (field, ids[0], mid+1, 20)]:
                            r=self.probe(*args, mode=mode)
                            self.assertNotEqual(r.returncode, 0)
                            self.assertEqual(balance(), maximum)
                        for args in [(field, ids[0], mid, 0), ('add-'+field, ids[0], mid, 0)]:
                            r=self.probe(*args, mode=mode)
                            self.assertEqual(r.returncode, 0, r.stderr)
                            self.assertEqual(balance(), 0)

    def test_quest_points_range_lazy_row_and_offline_guards(self):
        table='taiwan_cain.charac_quest_shop'
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            self.assertEqual(self.one('SELECT COUNT(*) FROM '+table+' WHERE charac_no IN ('+
                                     ','.join(map(str,ids))+')'),0)
            def row():
                self.cur.execute('SELECT * FROM '+table+' WHERE charac_no=%s',(cid,))
                return self.cur.fetchone()
            try:
                for mode in ('','STRICT_ALL_TABLES'):
                    for owner in (mid+1,mid):
                        r=self.probe('quest-points',cid,owner,-1,mode=mode)
                        self.assertNotEqual(r.returncode,0);self.assertIsNone(row())
                    r=self.probe('quest-points',cid,mid+1,90000,mode=mode)
                    self.assertNotEqual(r.returncode,0);self.assertIsNone(row())
                    for value in (0,90000,2147483647,0):
                        for _ in range(2):
                            r=self.probe('quest-points',cid,mid,value,mode=mode)
                            self.assertEqual(r.returncode,0,r.stderr)
                            self.assertEqual(self.one('SELECT qp FROM '+table+' WHERE charac_no=%s',(cid,)),value)
                    self.cur.execute('UPDATE '+table+' SET max_hp=5,init_count=7,quest_piece=9 WHERE charac_no=%s',(cid,))
                    before=row();self.cur.execute('SELECT * FROM '+table+' LIMIT 0');columns=[x[0] for x in self.cur.description]
                    r=self.probe('quest-points',cid,mid,90000,mode=mode)
                    self.assertEqual(r.returncode,0,r.stderr);after=row()
                    self.assertEqual([v for k,v in zip(columns,before) if k!='qp'],
                                     [v for k,v in zip(columns,after) if k!='qp'])
                    self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s',(cid,))
                    r=self.probe('quest-points',cid,mid,1,mode=mode)
                    self.assertNotEqual(r.returncode,0);self.assertEqual(row(),after)
                    self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s',(cid,))
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1,m_channel_no=0 WHERE m_id=%s',(mid,))
                    r=self.probe('quest-points',cid,mid,1,mode=mode)
                    self.assertNotEqual(r.returncode,0);self.assertEqual(row(),after)
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(mid,))
                    self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s',(cid,))
                    r=self.probe('quest-points',ids[-1],mid,90000,mode=mode)
                    self.assertNotEqual(r.returncode,0)
                    self.assertEqual(self.one('SELECT COUNT(*) FROM '+table+' WHERE charac_no=%s',(ids[-1],)),0)
            finally:
                self.cur.execute('DELETE FROM '+table+' WHERE charac_no IN ('+','.join(map(str,ids))+')')

    @contextlib.contextmanager
    def pvp_fixture(self):
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_cain.pvp_result WHERE charac_no=%s',(cid,)),0)
            try:
                self.cur.execute('INSERT INTO taiwan_cain.pvp_result (charac_no,lose) VALUES (%s,123)',(cid,))
                yield ids,mid
            finally:
                self.cur.execute('DELETE FROM taiwan_cain.pvp_result WHERE charac_no=%s',(cid,))

    def test_pvp_offline_guard_ranges_and_idempotence(self):
        with self.pvp_fixture() as (ids,mid):
            cid=ids[0]
            def values():
                self.cur.execute('SELECT pvp_grade,win,pvp_point,lose FROM taiwan_cain.pvp_result WHERE charac_no=%s',(cid,))
                return self.cur.fetchone()
            for mode in ('','STRICT_ALL_TABLES'):
                for target in ((37,2147483647,2147483647),(0,0,0)):
                    for _ in range(2):
                        result=self.probe('pvp',cid,mid,*target,mode=mode)
                        self.assertEqual(result.returncode,0,result.stderr)
                        self.assertEqual(values(),(*target,123))
                before=values()
                for target in ((-1,1,2),(38,1,2),(1,-1,2),(1,2,-1)):
                    self.assertNotEqual(self.probe('pvp',cid,mid,*target,mode=mode).returncode,0)
                    self.assertEqual(values(),before)
                self.assertNotEqual(self.probe('pvp',cid,mid+1,0,0,0,mode=mode).returncode,0)
                self.assertNotEqual(self.probe('pvp',ids[1],mid,0,0,0,mode=mode).returncode,0)  # Missing PVP row.
                self.assertNotEqual(self.probe('pvp',0,mid,0,0,0,mode=mode).returncode,0)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s',(cid,))
                self.assertNotEqual(self.probe('pvp',cid,mid,0,0,0,mode=mode).returncode,0)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s',(cid,))
                try:
                    for status in (1,2):
                        self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=%s,m_channel_no=0 WHERE m_id=%s',(status,mid))
                        self.assertNotEqual(self.probe('pvp',cid,mid,1,2,3,mode=mode).returncode,0)
                        self.assertEqual(values(),before)
                finally:
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(mid,))

    def test_pvp_login_changes_while_update_waits_for_table_lock(self):
        with self.pvp_fixture() as (ids,mid):
            cid=ids[0]
            self.cur.execute('SELECT * FROM taiwan_cain.pvp_result WHERE charac_no=%s',(cid,));before=self.cur.fetchone()
            lock=pymysql.connect(host=self.params['host'],port=int(self.params['port']),
                                 user=self.params['user'],password=self.params['pwd'],autocommit=True)
            pending=None
            try:
                with lock.cursor() as q:q.execute('LOCK TABLES taiwan_cain.pvp_result WRITE')
                env=dict(os.environ,**{'DNF_GM_TEST_'+k.upper():v for k,v in self.params.items()})
                pending=subprocess.Popen([str(PROBE),'pvp-checked',str(cid),str(mid),'17','123','456'],
                                         cwd=OUT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                end=time.monotonic()+15
                while True:
                    self.cur.execute('SHOW FULL PROCESSLIST')
                    blocked=any(row[7] and row[7].startswith('UPDATE taiwan_cain.pvp_result') and
                                ('charac_no='+str(cid)) in row[7] and row[6] and 'lock' in row[6].lower()
                                for row in self.cur.fetchall())
                    if blocked:break
                    if pending.poll() is not None or time.monotonic()>=end:
                        self.fail('PVP UPDATE did not reach the expected table-lock wait')
                    time.sleep(.05)
                # A controlled SQL race; original-game login is tested separately.
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s',(mid,))
                with lock.cursor() as q:q.execute('UNLOCK TABLES')
                out,err=pending.communicate(timeout=30)
                self.assertEqual(pending.returncode,1,(out,err))
                self.cur.execute('SELECT * FROM taiwan_cain.pvp_result WHERE charac_no=%s',(cid,))
                self.assertEqual(self.cur.fetchone(),before)
            finally:
                with lock.cursor() as q:q.execute('UNLOCK TABLES')
                lock.close()
                if pending is not None and pending.poll() is None:
                    try:pending.communicate(timeout=30)
                    except subprocess.TimeoutExpired:
                        pending.kill();pending.communicate()
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(mid,))

    def test_job_offline_ownership_deletion_and_idempotence(self):
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            def values():
                self.cur.execute('SELECT job,grow_type FROM taiwan_cain.charac_info WHERE charac_no=%s',(cid,))
                return self.cur.fetchone()
            for mode in ('','STRICT_ALL_TABLES'):
                for want in ((0,0),(1,2)):
                    for _ in range(2):
                        r=self.probe('job',cid,mid,*want,mode=mode)
                        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(values(),want)
                before=values()
                for want in (before,(0,0)):
                    r=self.probe('job',cid,mid+1,*want,mode=mode)
                    self.assertNotEqual(r.returncode,0);self.assertEqual(values(),before)
                    r=self.probe('job',ids[-1],mid,*want,mode=mode)
                    self.assertNotEqual(r.returncode,0)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s',(cid,))
                for want in (before,(0,0)):
                    r=self.probe('job',cid,mid,*want,mode=mode)
                    self.assertNotEqual(r.returncode,0);self.assertEqual(values(),before)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s',(cid,))
                try:
                    for status in (1,2):
                        self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=%s WHERE m_id=%s',(status,mid))
                        for want in (before,(0,0)):
                            r=self.probe('job',cid,mid,*want,mode=mode)
                            self.assertNotEqual(r.returncode,0);self.assertEqual(values(),before)
                finally:
                    self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(mid,))

    def test_job_packed_byte_storage_and_readback(self):
        column=self.one("SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='taiwan_cain' "
                        "AND TABLE_NAME='charac_info' AND COLUMN_NAME='grow_type'")
        signed_byte='tinyint' in column and 'unsigned' not in column
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            def values():
                self.cur.execute('SELECT job,grow_type FROM taiwan_cain.charac_info WHERE charac_no=%s',(cid,))
                return self.cur.fetchone()
            for mode in ('','STRICT_ALL_TABLES'):
                for raw in (0,17,33,127,128,145,162,255):
                    with self.subTest(mode=mode,raw=raw):
                        stored=raw-256 if signed_byte and raw>127 else raw
                        for _ in range(2):
                            r=self.probe('job',cid,mid,7,raw,mode=mode)
                            self.assertEqual(r.returncode,0,r.stderr)
                            self.assertEqual(values(),(7,stored))
                        r=self.probe('read-job',cid,mid,mode=mode)
                        self.assertEqual(r.returncode,0,r.stderr)
                        self.assertIn('read_job=7 grow='+str(raw),r.stdout)
                        self.assertIn('list_job=7 grow='+str(raw),r.stdout)
                before=values()
                for raw in (-1,256):
                    r=self.probe('job',cid,mid,7,raw,mode=mode)
                    self.assertNotEqual(r.returncode,0)
                    self.assertEqual(values(),before)

    def test_job_login_changes_while_update_waits_for_table_lock(self):
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            self.cur.execute('SELECT job,grow_type FROM taiwan_cain.charac_info WHERE charac_no=%s',(cid,))
            before=self.cur.fetchone();target=1 if before[1]!=1 else 2
            lock=pymysql.connect(host=self.params['host'],port=int(self.params['port']),
                                 user=self.params['user'],password=self.params['pwd'],autocommit=True)
            pending=None
            try:
                with lock.cursor() as q:q.execute('LOCK TABLES taiwan_cain.charac_info WRITE')
                env=dict(os.environ,**{'DNF_GM_TEST_'+k.upper():v for k,v in self.params.items()})
                pending=subprocess.Popen([str(PROBE),'job-checked',str(cid),str(mid),str(before[0]),str(target)],
                                         cwd=OUT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                end=time.monotonic()+15
                while True:
                    self.cur.execute('SHOW FULL PROCESSLIST')
                    blocked=any(r[7] and r[7].startswith('UPDATE taiwan_cain.charac_info') and
                                ('charac_no='+str(cid)) in r[7] and r[6] and 'lock' in r[6].lower()
                                for r in self.cur.fetchall())
                    if blocked:break
                    if pending.poll() is not None or time.monotonic()>=end:
                        self.fail('job UPDATE did not reach the expected table-lock wait')
                    time.sleep(.05)
                # Controlled SQL race, not a replacement for a real game login test.
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s',(mid,))
                with lock.cursor() as q:q.execute('UNLOCK TABLES')
                out,err=pending.communicate(timeout=30)
                self.assertEqual(pending.returncode,1,(out,err))
                self.cur.execute('SELECT job,grow_type FROM taiwan_cain.charac_info WHERE charac_no=%s',(cid,))
                self.assertEqual(self.cur.fetchone(),before)
            finally:
                with lock.cursor() as q:q.execute('UNLOCK TABLES')
                lock.close()
                if pending is not None and pending.poll() is None:
                    try:pending.communicate(timeout=30)
                    except subprocess.TimeoutExpired:
                        pending.kill();pending.communicate()
                self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id=%s',(mid,))

    def test_skill_points_wire_range_and_offline_guards(self):
        table='taiwan_cain_2nd.skill'
        fields=('remain_sp','remain_sp_2nd','remain_sfp_1st','remain_sfp_2nd')
        with self.bubble_fixture() as (ids,mid,_):
            cid=ids[0]
            self.assertEqual(self.one('SELECT COUNT(*) FROM '+table+' WHERE charac_no=%s',(cid,)),0)
            self.cur.execute('SELECT s.* FROM '+table+' s JOIN taiwan_cain.charac_info c '
                             'ON c.charac_no=s.charac_no WHERE c.delete_flag=0 LIMIT 1')
            row=list(self.cur.fetchone());columns=[x[0] for x in self.cur.description]
            row[columns.index('charac_no')]=cid
            for i,name in enumerate(fields):row[columns.index(name)]=7+i
            self.cur.execute('INSERT INTO '+table+' ('+','.join(map(ident,columns))+') VALUES ('+
                             ','.join(['%s']*len(columns))+')',row)
            def values():
                self.cur.execute('SELECT '+','.join(fields)+' FROM '+table+' WHERE charac_no=%s',(cid,))
                return self.cur.fetchone()
            try:
                before=values()
                r=self.probe('skill-points',cid,mid+1,1,2,3,4)
                self.assertNotEqual(r.returncode,0);self.assertEqual(values(),before)
                for mode in ('','STRICT_ALL_TABLES'):
                    for want in ((0,0,0,0),(12345,23456,34567,45678),(65535,)*4):
                        for _ in range(2):
                            r=self.probe('skill-points',cid,mid,*want,mode=mode)
                            self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(values(),want)
                    for index in range(4):
                        for invalid in (-1,65536):
                            bad=[1,2,3,4];bad[index]=invalid;before=values()
                            r=self.probe('skill-points',cid,mid,*bad,mode=mode)
                            self.assertNotEqual(r.returncode,0);self.assertEqual(values(),before)
                self.cur.execute('SELECT * FROM '+table+' WHERE charac_no=%s',(cid,))
                saved=self.cur.fetchone()
                self.assertEqual([v for k,v in zip(columns,saved) if k not in fields],
                                 [v for k,v in zip(columns,row) if k not in fields])
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=1 WHERE charac_no=%s',(cid,))
                r=self.probe('skill-points',cid,mid,1,2,3,4)
                self.assertNotEqual(r.returncode,0);self.assertEqual(values(),(65535,)*4)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET delete_flag=0 WHERE charac_no=%s',(cid,))
                self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s',(cid,))
                r=self.probe('skill-points',cid,mid,1,2,3,4)
                self.assertNotEqual(r.returncode,0)
            finally:
                self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s',(cid,))

    def test_fatigue_remaining_value_and_offline_guards(self):
        with self.bubble_fixture() as (ids, mid, _):
            cid=ids[0]
            table='taiwan_cain.charac_stat'
            self.assertEqual(self.one('SELECT COUNT(*) FROM '+table+' WHERE charac_no=%s', (cid,)), 0)
            self.cur.execute('INSERT INTO '+table+' (charac_no,fatigue,used_fatigue,premium_fatigue) VALUES (%s,13,13,7)', (cid,))
            try:
                self.cur.execute('UPDATE taiwan_cain.charac_info SET fatigue=9,max_fatigue=156 WHERE charac_no=%s', (cid,))
                def values():
                    self.cur.execute('SELECT c.fatigue,c.max_fatigue,s.fatigue,s.used_fatigue,s.premium_fatigue '
                                     'FROM taiwan_cain.charac_info c JOIN '+table+' s ON s.charac_no=c.charac_no WHERE c.charac_no=%s', (cid,))
                    return self.cur.fetchone()
                before=values()
                r=self.probe('fatigue',cid,mid+1)
                self.assertNotEqual(r.returncode,0)
                self.assertEqual(values(),before)
                r=self.probe('fatigue',cid,mid)
                self.assertEqual(r.returncode,0,r.stderr)
                self.assertEqual(values(),(156,156,156,0,7))
                r=self.probe('fatigue',cid,mid)
                self.assertEqual(r.returncode,0,r.stderr)
                # max_fatigue is signed SMALLINT; non-strict MySQL clamps 32768.
                for invalid in (-1,0):
                    self.cur.execute('UPDATE taiwan_cain.charac_info SET max_fatigue=%s WHERE charac_no=%s', (invalid,cid))
                    before=values()
                    r=self.probe('fatigue',cid,mid)
                    self.assertNotEqual(r.returncode,0)
                    self.assertEqual(values(),before)
                self.cur.execute('UPDATE taiwan_cain.charac_info SET max_fatigue=156,fatigue=9 WHERE charac_no=%s', (cid,))
                self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s',(cid,))
                r=self.probe('fatigue',cid,mid)
                self.assertNotEqual(r.returncode,0)
                self.assertEqual(self.one('SELECT fatigue FROM taiwan_cain.charac_info WHERE charac_no=%s',(cid,)),9)
            finally:
                self.cur.execute('DELETE FROM '+table+' WHERE charac_no=%s',(cid,))

    def test_bubble_online_siblings_rejected_before_account_rewards(self):
        with self.bubble_fixture() as (_, mid, _):
            self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=1 WHERE m_id=%s',(mid,))
            state=self.bubble_state()
            r=self.bubble(coin=1,dp=1,dc=1)
            self.assertNotEqual(r.returncode,0)
            self.assertIn('包含多个角色',r.stderr)
            self.assertIn('committed=0',r.stdout)
            self.assertEqual(self.bubble_state(),state)

    def test_bubble_billing_overflow_rolls_back_prior_rewards_and_missing_row(self):
        with self.bubble_fixture() as (_, mid, _):
            self.cur.execute('INSERT INTO taiwan_billing.cash_cera_point '
                             '(account,cera_point,reg_date,mod_date) VALUES (%s,4294967295,NOW(),NOW())', (str(mid),))
            state = self.bubble_state()
            r = self.bubble(coin=1, dp=1, dc=1, mode='')
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.bubble_state(), state)

    def test_bubble_missing_inventory_rejected_without_partial_payment(self):
        with self.bubble_fixture() as (ids, _, _):
            self.cur.execute('DELETE FROM taiwan_cain_2nd.inventory WHERE charac_no=%s', (ids[1],))
            state = self.bubble_state()
            r = self.bubble(coin=1, dp=1)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.bubble_state(), state)

    def test_bubble_cash_failure_rolls_back_all_rewards(self):
        with self.bubble_fixture():
            state = self.bubble_state()
            with self.trigger('taiwan_billing.cash_cera_point', 'INSERT',
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked cash'"):
                r = self.bubble(coin=1, dp=1, dc=1)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.bubble_state(), state)

    def test_bubble_item_mail_quantity_body_and_independent_counts(self):
        with self.bubble_fixture() as (ids, _, _):
            self.bubble_paid(self.bubble(target=2, tno=ids[0], item=1001, num=7,
                                         mode='STRICT_TRANS_TABLES'), 1, 0)
            self.cur.execute('SELECT p.item_id,p.add_info,p.avata_flag,p.creature_flag,l.charac_no '
                             'FROM taiwan_cain_2nd.postal p JOIN taiwan_cain_2nd.letter l '
                             'ON l.letter_id=p.letter_id WHERE p.receive_charac_no=%s', (ids[0],))
            self.assertEqual(self.cur.fetchall(), ((1001, 7, 0, 0, ids[0]),))

    def test_bubble_avatar_and_creature_attachment_ownership_strict(self):
        with self.bubble_fixture() as (ids, _, _):
            for avatar, creature, table, flag in [(1, 0, 'user_items', 'avata_flag'),
                                                 (0, 1, 'creature_items', 'creature_flag')]:
                self.bubble_paid(self.bubble(target=2, tno=ids[0], item=1001, num=2,
                                             avatar=avatar, creature=creature, mode='STRICT_TRANS_TABLES'), 1, 0)
                self.assertEqual(self.one('SELECT COUNT(*) FROM taiwan_cain_2nd.postal p '
                                         'JOIN taiwan_cain_2nd.' + table + ' a ON a.ui_id=p.add_info '
                                         'AND a.it_id=p.item_id AND a.charac_no=p.receive_charac_no '
                                         'WHERE p.' + flag + '=1 AND p.receive_charac_no=%s', (ids[0],)), 2)

    def test_bubble_later_postal_failure_rolls_back_every_recipient(self):
        with self.bubble_fixture() as (ids, _, _):
            state = self.bubble_state()
            for avatar in (0, 1):
                with self.trigger('taiwan_cain_2nd.postal', 'INSERT', 'BEGIN IF NEW.receive_charac_no=' +
                                  str(ids[1]) + " THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked mail'; END IF; END"):
                    r = self.bubble(coin=1, dp=1, dc=1, item=1001, num=2, avatar=avatar)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn('paid_chars=0 paid_accounts=0', r.stdout)
                self.assertEqual(self.bubble_state(), state)

    def test_bubble_invalid_configuration_writes_nothing(self):
        with self.bubble_fixture():
            state = self.bubble_state()
            for kwargs in [dict(target=-1), dict(target=4), dict(tno=0), dict(coin=-1),
                           dict(item=1001, num=0), dict(item=1001, num=201),
                           dict(item=1001, avatar=1, creature=1), dict(target=1, item=1001)]:
                r = self.bubble(dp=1, **kwargs)
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(self.bubble_state(), state)

    def test_bubble_later_batch_failure_rolls_back_first_batch(self):
        with self.bubble_fixture(live=260) as (ids, _, _):
            state = self.bubble_state()
            with self.trigger('taiwan_cain_2nd.inventory', 'UPDATE', 'BEGIN IF NEW.charac_no=' +
                              str(ids[-2]) + " THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked batch'; END IF; END"):
                r = self.bubble(coin=1, dp=1)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.bubble_state(), state)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    args, rest = parser.parse_known_args()
    if args.build:
        build()
    unittest.main(argv=[__file__, *rest], verbosity=2)
