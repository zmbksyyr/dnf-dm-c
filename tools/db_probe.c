// Integration-test entry point: exercise the production data layer without a GUI.
// Connection parameters come from the test runner's environment, never argv.
#include "db.h"
#include "item.h"
#include "pvf.h"
#include <shellapi.h>

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

static int cleanup_has(u32 id, void *ctx) { return id != *(const u32 *)ctx; }

static int run_probe(int argc, char **argv) {
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
    if ((!strcmp(argv[1], "set-billing") || !strcmp(argv[1], "add-billing")) && (argc == 6 || argc == 7)) {
        ok = db_change_cera(&d, &c, (u64)strtoull(argv[4], NULL, 10), (u64)strtoull(argv[5], NULL, 10),
                            argc == 7 ? (unsigned)strtoul(argv[6], NULL, 10) : 3u, !strcmp(argv[1], "add-billing"), err);
    } else if (!strcmp(argv[1], "account-money") && (argc == 5 || argc == 6)) {
        ok = db_set_account_money(&d, &c, (u64)strtoull(argv[4], NULL, 10), argc == 6 ? atoi(argv[5]) : 0, err);
    } else if ((!strcmp(argv[1], "orphan-delete") || !strcmp(argv[1], "purge-orphan")) && argc == 8) {
        u32 invalid = (u32)strtoul(argv[7], NULL, 10);
        ItemHasName has = invalid ? cleanup_has : NULL;
        if (!strcmp(argv[1], "orphan-delete")) {
            TabOrphan r = {0};r.avatar = atoi(argv[2]);r.cno = (u32)strtoul(argv[3], NULL, 10);
            r.mid = (u32)strtoul(argv[4], NULL, 10);r.ui_id = (u64)strtoull(argv[5], NULL, 10);r.it_id = (u32)strtoul(argv[6], NULL, 10);
            ok = db_del_tab_orphan(&d, &r, has, &invalid, err);
        }
        else {
            Vec rows; vec_init(&rows, sizeof(ScanRow));
            ScanRow *r = VEC_ADD(&rows, ScanRow);
            r->kind = atoi(argv[2]) ? 1 : 2;
            r->cno = (u32)strtoul(argv[3], NULL, 10);r->mid = (u32)strtoul(argv[4], NULL, 10);
            r->key = (u64)strtoull(argv[5], NULL, 10);r->id = (u32)strtoul(argv[6], NULL, 10);
            ok = db_purge_bad(&d, &rows, &nt, has, NULL, &invalid, err);vec_free(&rows);printf("done=%d\n", nt);
        }
    } else if (!strcmp(argv[1], "cleanup-scan") && argc == 6) {
        u32 invalid = (u32)strtoul(argv[5], NULL, 10);
        Vec rows;vec_init(&rows, sizeof(ScanRow));
        ok = db_scan_bad(&d, &rows, &nt, invalid ? cleanup_has : NULL, NULL, &invalid, err);
        Vec selected;vec_init(&selected, sizeof(ScanRow));
        if (ok) for (int i = 0; i < rows.n; i++) {
            ScanRow *r = VEC_AT(&rows, ScanRow, i);
            if (r->cno == c.no && r->mid == c.mid) *VEC_ADD(&selected, ScanRow) = *r;
        }
        if (ok) ok = file_write(argv[4], selected.p, (size_t)selected.n * sizeof(ScanRow));
        printf("scanned=%d selected=%d\n", nt, selected.n);vec_free(&selected);vec_free(&rows);
    } else if (!strcmp(argv[1], "cleanup-purge") && argc == 4) {
        u32 invalid = (u32)strtoul(argv[3], NULL, 10);
        Buf data = {0};
        if (file_read(argv[2], &data) && !(data.n % sizeof(ScanRow))) {
            Vec rows;vec_init(&rows, sizeof(ScanRow));
            for (size_t i = 0; i < data.n; i += sizeof(ScanRow)) memcpy(vec_add(&rows), data.p + i, sizeof(ScanRow));
            ok = db_purge_bad(&d, &rows, &nt, invalid ? cleanup_has : NULL, NULL, &invalid, err);
            printf("done=%d\n", nt);vec_free(&rows);
        }
        buf_free(&data);
    } else if (!strcmp(argv[1], "pet-name") && argc == 6) {
        ok = db_set_pet_name(&d, &c, (u32)strtoul(argv[4], NULL, 10), argv[5], err);
    } else if (!strcmp(argv[1], "pet-full") && argc == 5) {
        ok = db_set_pet_full(&d, &c, (u32)strtoul(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "pet-expire") && argc == 5) {
        ok = db_set_pet_expire(&d, &c, (u32)strtoul(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "pet-delete") && argc == 5) {
        ok = db_delete_pet(&d, &c, (u32)strtoul(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "avatar-hidden") && argc == 6) {
        ok = db_set_avatar_hidden(&d, &c, (u32)strtoul(argv[4], NULL, 10), atoi(argv[5]), err);
    } else if (!strcmp(argv[1], "avatar-expire") && argc == 5) {
        ok = db_set_avatar_expire(&d, &c, (u32)strtoul(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "avatar-delete") && argc == 5) {
        ok = db_delete_avatar(&d, &c, (u32)strtoul(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "dump") && argc == 3) {
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
    } else if ((!strcmp(argv[1], "job") || !strcmp(argv[1], "job-checked")) && argc == 6) {
        ok = strcmp(argv[1], "job-checked") || db_require_offline(&d, c.mid, err);
        if (ok) ok = db_set_job(&d, &c, atoi(argv[4]), atoi(argv[5]), err);
    } else if ((!strcmp(argv[1], "pvp") || !strcmp(argv[1], "pvp-checked")) && argc == 7) {
        ok = strcmp(argv[1], "pvp-checked") || db_require_offline(&d, c.mid, err);
        if (ok) ok = db_set_pvp(&d, &c, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), err);
    } else if (!strcmp(argv[1], "quest-init") && argc == 4) {
        ok = db_quest_init(&d, &c, err);
    } else if (!strcmp(argv[1], "quest-clear") && argc == 4) {
        ok = db_clear_quests(&d, &c, err);
    } else if (!strcmp(argv[1], "quest-slot") && argc >= 8) {
        u32 purge[4]; int n = argc - 8;
        if (n <= 4) {
            for (int i = 0; i < n; i++) purge[i] = (u32)strtoul(argv[8+i], NULL, 10);
            ok = db_set_quest_slot(&d, &c, atoi(argv[4]), (u32)strtoul(argv[5], NULL, 10),
                                  (u32)strtoul(argv[6], NULL, 10), (u32)strtoul(argv[7], NULL, 10), purge, n, err);
        }
    } else if (!strcmp(argv[1], "quest-export") && argc == 5) {
        ok = db_export_quest(&d, &c, argv[4], err);
    } else if (!strcmp(argv[1], "quest-import") && argc == 5) {
        ok = db_import_quest(&d, &c, argv[4], &nt, err);
    } else if (!strcmp(argv[1], "mail-delete") && argc == 3) {
        ok = db_delete_mail(&d, (u64)strtoull(argv[2], NULL, 10), err);
    } else if (!strcmp(argv[1], "mail-delete-all") && argc == 3) {
        ok = db_delete_all_mails(&d, &c, err);
    } else if (!strcmp(argv[1], "capabilities") && argc == 4) {
        Vec found;vec_init(&found,sizeof(Charac));
        ok=db_list_chars(&d,&found,200,argv[2],err);
        if(ok) for(int i=0;i<found.n;i++) {
            const Charac *row=VEC_AT(&found,Charac,i);
            if(row->no==c.no && row->mid==c.mid)
                printf("online=%d\ncapabilities=%u\n",row->online,row->online_caps);
        }
        vec_free(&found);
    } else if (!strcmp(argv[1], "read-name") && argc == 4) {
        ok = db_read_charac(&d, c.no, &c, err);
        if (ok) printf("name=%s\n", c.name);
    } else if (!strcmp(argv[1], "read-job") && argc == 4) {
        ok = db_read_charac(&d, c.no, &c, err);
        if (ok) printf("read_job=%d grow=%d\n", c.job, c.grow);
        Vec found; vec_init(&found, sizeof(Charac));
        if (ok) ok = db_list_chars(&d, &found, 100, argv[2], err);
        for (int i = 0; ok && i < found.n; i++) {
            const Charac *row = VEC_AT(&found, Charac, i);
            if (row->no == c.no) printf("list_job=%d grow=%d\n", row->job, row->grow);
        }
        vec_free(&found);
    } else if (!strcmp(argv[1], "search-name") && argc == 3) {
        Vec found;
        vec_init(&found, sizeof(Charac));
        ok = db_list_chars(&d, &found, 100, argv[2], err);
        for (int i = 0; ok && i < found.n; i++) {
            const Charac *row = VEC_AT(&found, Charac, i);
            printf("%u %s\n", row->no, row->name);
        }
        vec_free(&found);
    } else if (!strcmp(argv[1], "rename") && argc == 5) {
        ok = db_set_name(&d, &c, argv[4], err);
    } else if (!strcmp(argv[1], "level") && argc == 5) {
        ok = db_set_lev(&d, &c, atoi(argv[4]), 0, err);
    } else if (!strcmp(argv[1], "level-exp") && argc == 6) {
        ok = db_set_lev(&d, &c, atoi(argv[4]), (i64)strtoll(argv[5], NULL, 10), err);
    } else if (!strcmp(argv[1], "level-pvf") && argc == 6) {
        Pvf p = {0};
        PvfProg prog = {0};
        u32 exp = 0;
        ok = pvf_open(&p, argv[5], prog, err) &&
             pvf_level_min_exp(&p, atoi(argv[4]), &exp, err) &&
             db_set_lev(&d, &c, atoi(argv[4]), exp, err);
        if (ok) printf("level=%d accumulated_exp=%u\n", atoi(argv[4]), exp);
        pvf_close(&p);
    } else if (!strcmp(argv[1], "scalars") && argc == 4) {
        ok = db_set_money(&d, &c, 1234567, err) && db_set_coin(&d, &c, 17, err) &&
             db_set_fatigue(&d, &c, err) && db_set_sp(&d, &c, 1234, 1234, 12, 12, err) &&
             db_set_qp(&d, &c, 34, err) && db_change_cera(&d, &c, 4567, 5678, 3u, 0, err);
    } else if (!strcmp(argv[1], "cera") && argc == 6) {
        ok = db_change_cera(&d, &c, strtoull(argv[4], NULL, 10), strtoull(argv[5], NULL, 10), 3u, 0, err);
    } else if (!strcmp(argv[1], "skill-points") && argc == 8) {
        ok = db_set_sp(&d, &c, atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), err);
    } else if (!strcmp(argv[1], "quest-points") && argc == 5) {
        ok = db_set_qp(&d, &c, atoi(argv[4]), err);
    } else if ((!strcmp(argv[1], "expert") || !strcmp(argv[1], "expert-checked")) && argc == 4) {
        const u32 targets[5] = {0, 2054, 2054, 200, 2054}; // Synthetic data-test PVF snapshot.
        ok = (strcmp(argv[1], "expert-checked") || db_require_offline(&d, c.mid, err)) &&
             db_max_expert(&d, &c, targets, err);
    } else if (!strcmp(argv[1], "expert-pvf") && argc == 5) {
        Pvf p = {0}; PvfProg prog = {0}; u32 targets[5];
        ok = pvf_open(&p, argv[4], prog, err) && pvf_expert_max_exp(&p, targets, err) &&
             db_max_expert(&d, &c, targets, err);
        pvf_close(&p);
    } else if (!strcmp(argv[1], "fatigue") && argc == 4) {
        ok = db_set_fatigue(&d, &c, err);
    } else if (!strcmp(argv[1], "money") && argc == 5) {
        ok = db_set_money(&d, &c, (u64)strtoull(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "add-money") && argc == 5) {
        ok = db_add_money(&d, &c, (u64)strtoull(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "coin") && argc == 5) {
        ok = db_set_coin(&d, &c, (u64)strtoull(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "add-coin") && argc == 5) {
        ok = db_add_coin(&d, &c, (u64)strtoull(argv[4], NULL, 10), err);
    } else if (!strcmp(argv[1], "online") && argc == 4) {
        printf("online=%d\n", db_is_online(&d, c.mid));
        ok = 1;
    } else if (!strcmp(argv[1], "offline") && argc == 4) {
        ok = db_require_offline(&d, c.mid, err);
    } else if (!strcmp(argv[1], "equipment-slots") && argc == 4) {
        ok = db_open_equip_slot(&d, &c, err);
    } else if (!strcmp(argv[1], "no-weight") && argc == 4) {
        ok = db_no_weight(&d, &c, err);
    } else if (!strcmp(argv[1], "equipment-level") && argc == 5) {
        ok = db_unlock_lev_equip(&d, &c, atoi(argv[4]), err);
    } else if (!strcmp(argv[1], "container-save") && argc == 11) {
        Buf packed[6] = {0};
        u32 mask = (u32)strtoul(argv[4], NULL, 10);
        ok = 1;
        for (int p = 0; p < 6; p++) if (mask & (1u << p)) {
            if (!file_read(argv[5 + p], &packed[p])) {
                snprintf(err, ERRBUF, "cannot read container snapshot %d", p); ok = 0; break;
            }
        }
        if (ok) ok = db_save_containers(&d, &c, packed, mask, err);
        for (int p = 0; p < 6; p++) buf_free(&packed[p]);
    } else if (!strcmp(argv[1], "item-slot") && argc == 8) {
        CharData data = {0};
        Buf packed[6] = {0};
        int slot = atoi(argv[4]);
        ok = db_load_data(&d, &c, &data, err);
        if (ok && (slot < 0 || slot >= data.inv.n || !data.inv.ok)) {
            snprintf(err, ERRBUF, "invalid inventory slot");
            ok = 0;
        }
        if (ok) {
            Item *it = &data.inv.items[slot];
            item_set_id(it, (u32)strtoul(argv[5], NULL, 10));
            item_set_type(it, (u8)atoi(argv[6]));
            item_set_num(it, (u32)strtoul(argv[7], NULL, 10));
            ok = blob_pack(&data.inv, &packed[0]) && db_save_containers(&d, &c, packed, 1u, err);
        }
        buf_free(&packed[0]);
        db_free_data(&data);
    } else if (!strcmp(argv[1], "text-mail") && (argc == 5 || argc == 6)) {
        ok = db_send_text_mail(&d, &c, argc == 6 ? argv[5] : "GM", argv[4], err);
    } else if (!strcmp(argv[1], "item-mail") && (argc == 6 || argc == 8 || argc == 9)) {
        GrantItem g = {0};
        g.id = (u32)strtoul(argv[4], NULL, 10);
        g.num = (u32)strtoul(argv[5], NULL, 10);
        g.type = IT_MATERIAL;
        ok = db_send_item_mail_ex(&d, &c, "GM", argc == 9 ? argv[8] : "GM host comparison", &g,
                                 argc >= 8 ? atoi(argv[6]) : 0, argc >= 8 ? atoi(argv[7]) : 0, err);
    } else if (!strcmp(argv[1], "bubble") && (argc == 9 || argc == 11)) {
        BubbleCfg cfg = {0};
        cfg.target = atoi(argv[2]); cfg.tno = (u32)strtoul(argv[3], NULL, 10);
        cfg.coin = atoi(argv[4]); cfg.dpoint = atoi(argv[5]); cfg.dcoin = atoi(argv[6]);
        cfg.item = (u32)strtoul(argv[7], NULL, 10); cfg.itemnum = atoi(argv[8]);
        if (argc == 11) { cfg.avatar = atoi(argv[9]); cfg.creature = atoi(argv[10]); }
        BubblePaid paid;
        ok = db_bubble_tick(&d, &cfg, &paid, err);
        printf("paid_chars=%u paid_accounts=%u coin_rejected=%u coin_unknown=%u billing_unconfirmed=%u committed=%d\n",
               paid.chars,paid.accounts,paid.coin_rejected,paid.coin_unknown,paid.billing_unconfirmed,paid.committed);
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

// The narrow CRT argv uses the Windows ANSI page. Tests must send exactly the
// same UTF-8 strings as the Win32 UI, including names and Unicode file paths.
int main(void) {
    int argc = 0;
    WCHAR **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) return 2;
    char **args = (char **)xalloc((size_t)argc * sizeof(char *));
    for (int i = 0; i < argc; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        args[i] = (char *)xalloc((size_t)bytes);
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], bytes, NULL, NULL);
    }
    int status = run_probe(argc, args);
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    LocalFree(wide);
    return status;
}
