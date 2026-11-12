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
import unittest
import zlib

import pymysql

ROOT = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("DNF_GM_BUILD", ROOT.parent / "_build" / "DNF_GM_C"))
OUT = ROOT.parent / "_build" / "audit"
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
    sources = ["db", "db_mail", "db_admin", "db_bubble", "mysql", "util", "zlib", "item"]
    cmd = [zig, "cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2",
           "-DUNICODE", "-D_UNICODE", "-Wall", "-Wextra", "-Wshadow", "-Wconversion",
           "-Wpedantic", "-Werror", "-Isrc", "tools/db_probe.c"]
    cmd += ["src/" + s + ".c" for s in sources]
    cmd += ["-lws2_32", "-luser32", "-ladvapi32", "-o", str(PROBE)]
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

    def test_mail_does_not_delete_foreign_owner_attachment(self):
        with self.attachment() as (ui, postal):
            self.cur.execute("UPDATE taiwan_cain_2nd.user_items SET charac_no=%s WHERE ui_id=%s", (TEST_NO - 1, ui))
            r = self.probe("mail-delete", postal)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)

    def test_mail_bulk_failure_rolls_back(self):
        with self.attachment() as (ui, postal):
            with self.trigger("taiwan_cain_2nd.postal", "UPDATE",
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked postal'"):
                r = self.probe("mail-delete-all", TEST_NO)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.one("SELECT COUNT(*) FROM taiwan_cain_2nd.user_items WHERE ui_id=%s", (ui,)), 1)

    def test_quest_init_nested_transaction(self):
        table = "taiwan_cain.new_charac_quest"
        self.assertEqual(self.one("SELECT COUNT(*) FROM " + table + " WHERE charac_no=%s", (TEST_NO,)), 0)
        self.cur.execute("SELECT * FROM " + table + " WHERE charac_no=%s", (self.cno,))
        row = list(self.cur.fetchone())
        cols = [d[0] for d in self.cur.description]
        row[cols.index("charac_no")] = TEST_NO
        row[cols.index("clear_quest")] = struct.pack("<I", 30000) + zlib.compress(b'\x01' * 30000)
        row[cols.index("quest_notify")] = b'\x01' * 40
        try:
            self.cur.execute("INSERT INTO " + table + " (" + ",".join(map(ident, cols)) + ") VALUES (" +
                             ",".join(["%s"] * len(row)) + ")", row)
            with self.trigger(table, "UPDATE", "BEGIN IF NEW.charac_no=" + str(TEST_NO) +
                              " AND NEW.quest_notify=UNHEX(REPEAT('00',40)) THEN "
                              "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked quest'; END IF; END"):
                r = self.probe("quest-init", TEST_NO, self.mid)
            self.assertNotEqual(r.returncode, 0)
            self.cur.execute("SELECT * FROM " + table + " WHERE charac_no=%s", (TEST_NO,))
            self.assertEqual(self.cur.fetchone(), tuple(row), "an inner BLOB commit must not escape the outer rollback")
        finally:
            self.cur.execute("DELETE FROM " + table + " WHERE charac_no=%s", (TEST_NO,))

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
        self.cur.execute("SELECT * FROM " + table + " WHERE m_id=%s", (self.mid,))
        rows = self.cur.fetchall()
        self.cur.execute("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,))
        level = self.cur.fetchone()[0]
        exp = self.one("SELECT exp FROM taiwan_cain.charac_stat WHERE charac_no=%s", (self.cno,))
        try:
            if not rows:
                self.cur.execute("INSERT INTO " + table + " (m_id) VALUES (%s)", (self.mid,))
            with self.trigger(table, "DELETE", "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='audit blocked punish'"):
                r = self.probe("level", self.cno, self.mid, 42 if level != 42 else 43)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(self.one("SELECT lev FROM taiwan_cain.charac_info WHERE charac_no=%s", (self.cno,)), level)
            self.assertEqual(self.one("SELECT exp FROM taiwan_cain.charac_stat WHERE charac_no=%s", (self.cno,)), exp)
        finally:
            self.cur.execute("UPDATE taiwan_cain.charac_info SET lev=%s WHERE charac_no=%s", (level, self.cno))
            self.cur.execute("UPDATE taiwan_cain.charac_stat SET exp=%s WHERE charac_no=%s", (exp, self.cno))
            if not rows:
                self.cur.execute("DELETE FROM " + table + " WHERE m_id=%s", (self.mid,))

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
                             'VALUES (%s,1,NOW())', (mid,))
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

    def test_bubble_online_excludes_deleted_and_orphan_accounts(self):
        with self.bubble_fixture() as (ids, mid, _):
            self.cur.execute('UPDATE taiwan_login.login_account_3 SET login_status=2 WHERE m_id=%s', (mid,))
            self.cur.execute('UPDATE taiwan_cain.charac_info SET m_id=%s WHERE charac_no=%s', (mid + 2, ids[1]))
            self.bubble_paid(self.bubble(target=0, coin=1, dp=1), 1, 1)
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

    def test_bubble_online_membership_is_frozen_across_rewards(self):
        with self.bubble_fixture() as (ids, mid, second):
            self.cur.execute('UPDATE taiwan_cain.charac_info SET m_id=%s WHERE charac_no=%s', (second, ids[1]))
            self.cur.execute('INSERT INTO taiwan_login.login_account_3 (m_id,login_status,last_login_date) '
                             'VALUES (%s,1,NOW())', (second,))
            with self.trigger('taiwan_cain_2nd.inventory', 'UPDATE',
                              'UPDATE taiwan_login.login_account_3 SET login_status=0 WHERE m_id IN (' +
                              str(mid) + ',' + str(second) + ')'):
                self.bubble_paid(self.bubble(target=0, coin=1, dp=2, dc=3, item=1001, num=4), 2, 2)
            for account, no in zip((mid, second), ids):
                self.assertEqual(self.one('SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%s', (account,)), 0)
                self.assertEqual(self.one('SELECT cera FROM taiwan_billing.cash_cera WHERE account=%s', (str(account),)), 2)
                self.assertEqual(self.one('SELECT cera_point FROM taiwan_billing.cash_cera_point WHERE account=%s', (str(account),)), 3)
                self.assertEqual(self.one('SELECT add_info FROM taiwan_cain_2nd.postal WHERE receive_charac_no=%s', (no,)), 4)

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
