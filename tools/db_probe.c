// Integration-test entry point: exercise the production data layer without a GUI.
// Connection parameters come from the test runner's environment, never argv.
#include "db.h"
#include "item.h"

static int item_codec(const char *path) {
    Buf f = {0};
    if (!file_read(path, &f)) return 1;
    if (f.n % ITEM_SIZE) { buf_free(&f); return 1; }
    for (size_t off = 0; off < f.n; off += ITEM_SIZE) {
        Item it;
        memcpy(it.raw, f.p + off, ITEM_SIZE);
        printf("%d %u %u %d %d %u %u %d %u %u %d",
               item_seal(&it), item_type(&it), item_id(&it), item_enhance(&it),
               item_seal_cnt(&it), item_num(&it), item_durability(&it), item_inc_type(&it),
               item_inc_value(&it), item_orb(&it), item_forge(&it));
        for (int j = 0; j < 4; j++)
            printf(" %d %u", item_seal_id(&it, j), item_seal_lv(&it, j));
        item_set_seal(&it, 1); item_set_id(&it, 0x12345678);
        item_set_enhance(&it, 23); item_set_seal_cnt(&it, 5);
        item_set_num(&it, 0x98765432); item_set_durability(&it, 54321);
        item_set_inc_type(&it, 3); item_set_inc_value(&it, 12345);
        item_set_orb(&it, 0xABCDEF01); item_set_forge(&it, 9);
        item_set_otherworld(&it, 0xA5B6);
        for (int j = 0; j < 4; j++) {
            item_set_seal_id(&it, j, j + 21);
            item_set_seal_lv(&it, j, (u16)(j * 1000 + 321));
        }
        printf(" ");
        for (int j = 0; j < ITEM_SIZE; j++) printf("%02x", it.raw[j]);
        printf("\n");
    }
    buf_free(&f);
    return 0;
}

static const char *env_or(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return v && *v ? v : fallback;
}

static void progress(int pct) {
    const char *cancel = getenv("DNF_GM_TEST_CANCEL_PCT");
    if (cancel && pct >= atoi(cancel)) db_cancel_request();
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (argc == 3 && !strcmp(argv[1], "item-codec")) return item_codec(argv[2]);
    Db d;
    Err err = {0};
    if (!db_open(&d, env_or("DNF_GM_TEST_HOST", "127.0.0.1"),
                 atoi(env_or("DNF_GM_TEST_PORT", "3306")),
                 env_or("DNF_GM_TEST_USER", "game"), env_or("DNF_GM_TEST_PWD", ""), 45, err)) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    const char *mode = getenv("DNF_GM_TEST_SQL_MODE");
    if (mode) {
        Str q = {0};
        char *escaped = mysql_escape(&d.conn, mode);
        str_catf(&q, "SET SESSION sql_mode='%s'", escaped);
        free(escaped);
        MyResult r = {0};
        int ok = mysql_query(&d.conn, str_c(&q), &r);
        my_free(&r);
        str_free(&q);
        if (!ok) { fprintf(stderr, "%s\n", d.conn.err); db_close(&d); return 2; }
    }
    int ok = 0, nt = 0;
    db_progress_cb = progress;
    u64 nr = 0;
    Charac c = {0};
    if (argc > 2) c.no = (u32)strtoul(argv[2], NULL, 10);
    if (argc > 3) c.mid = (u32)strtoul(argv[3], NULL, 10);
    if (!strcmp(argv[1], "dump") && argc == 3) {
        ok = db_dump_sql(&d, argv[2], &nt, &nr, err);
    } else if (!strcmp(argv[1], "restore") && argc == 3) {
        ok = db_restore_sql(&d, argv[2], &nt, err);
        MyResult state = {0};
        if (mysql_query(&d.conn, "SELECT @@foreign_key_checks,@@sql_mode", &state))
            printf("session_foreign_keys=%llu\nsession_mode=%s\n",
                   (unsigned long long)my_u64(&state, 0, 0, 0), my_text(&state, 0, 1));
        my_free(&state);
    } else if (!strcmp(argv[1], "char-dump") && argc == 5) {
        ok = db_backup_char(&d, &c, argv[4], &nt, &nr, err);
    } else if (!strcmp(argv[1], "clone") && argc == 4) {
        u32 new_no = 0;
        char name[64];
        Str log = {0};
        ok = db_clone_char(&d, &c, name, sizeof(name), &new_no, &nt, &nr, &log, err);
        str_free(&log);
        if (ok) printf("new_no=%u\n", new_no);
    } else if (!strcmp(argv[1], "quest-init") && argc == 4) {
        ok = db_quest_init(&d, &c, err);
    } else if (!strcmp(argv[1], "mail-delete") && argc == 3) {
        ok = db_delete_mail(&d, (u64)strtoull(argv[2], NULL, 10), err);
    } else if (!strcmp(argv[1], "mail-delete-all") && argc == 3) {
        ok = db_delete_all_mails(&d, &c, err);
    } else if (!strcmp(argv[1], "rename") && argc == 5) {
        ok = db_set_name(&d, &c, argv[4], err);
    } else if (!strcmp(argv[1], "level") && argc == 5) {
        ok = db_set_lev(&d, &c, atoi(argv[4]), 0, err);
    } else if (!strcmp(argv[1], "bubble") && (argc == 9 || argc == 11)) {
        BubbleCfg cfg = {0};
        cfg.target = atoi(argv[2]); cfg.tno = (u32)strtoul(argv[3], NULL, 10);
        cfg.coin = atoi(argv[4]); cfg.dpoint = atoi(argv[5]); cfg.dcoin = atoi(argv[6]);
        cfg.item = (u32)strtoul(argv[7], NULL, 10); cfg.itemnum = atoi(argv[8]);
        if (argc == 11) { cfg.avatar = atoi(argv[9]); cfg.creature = atoi(argv[10]); }
        BubblePaid paid;
        ok = db_bubble_tick(&d, &cfg, &paid, err);
        printf("paid_chars=%u paid_accounts=%u\n", paid.chars, paid.accounts);
    } else if (!strcmp(argv[1], "query") && argc == 3) {
        Buf f = {0};
        if (file_read(argv[2], &f)) {
            buf_putc(&f, 0);
            MyResult r = {0};
            ok = mysql_query(&d.conn, (const char *)f.p, &r);
            if (ok) printf("rows=%d cols=%d value=%llu\n", r.nrow, r.ncol,
                           (unsigned long long)my_u64(&r, 0, 0, 0));
            else snprintf(err, ERRBUF, "%s", d.conn.err);
            my_free(&r);
        }
        buf_free(&f);
    } else {
        snprintf(err, ERRBUF, "unknown command or invalid arguments");
    }
    db_close(&d);
    if (ok) printf("OK tables=%d rows=%llu\n", nt, (unsigned long long)nr);
    else fprintf(stderr, "%s\n", err[0] ? err : "operation failed");
    return ok ? 0 : 1;
}
