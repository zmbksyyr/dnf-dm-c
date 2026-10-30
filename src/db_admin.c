// db_admin.c：活动 / 封停 / 账号 / 备份 / 扫描表操作。从 db.c 拆出，行为不变。
#include "db_priv.h"
#include "db.h"
#include "util.h"
#include "mysql.h"
#include "item.h"
#include <time.h>

// ---------------- 活动 ----------------
int db_list_events(Db *d, Vec *out, Err err) {
    Str s = {0};
    str_cat(&s,
            "SELECT g.log_id,g.event_type,g.parameter1,g.parameter2,i.event_name,i.event_explain "
            "FROM d_taiwan.dnf_event_log g LEFT JOIN d_taiwan.dnf_event_info i "
            "ON i.event_id=g.event_type ORDER BY g.log_id DESC");
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        EventRow *e = VEC_ADD(out, EventRow);
        memset(e, 0, sizeof(*e));
        e->log_id = my_u64(&r, i, 0, 0);
        e->type = (u32)my_u64(&r, i, 1, 0);
        e->p1 = my_i64(&r, i, 2, 0);
        e->p2 = my_i64(&r, i, 3, 0);
        cp_text(e->name, sizeof(e->name), &r, i, 4);
        cp_text(e->explain, sizeof(e->explain), &r, i, 5);
    }
    my_free(&r);
    return 1;
}

int db_list_event_info(Db *d, Vec *out, Err err) {
    Str s = {0};
    str_cat(&s, "SELECT event_id,event_name,event_explain FROM d_taiwan.dnf_event_info ORDER BY event_id");
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        EventInfo *e = VEC_ADD(out, EventInfo);
        memset(e, 0, sizeof(*e));
        e->id = (u32)my_u64(&r, i, 0, 0);
        cp_text(e->name, sizeof(e->name), &r, i, 1);
        cp_text(e->explain, sizeof(e->explain), &r, i, 2);
    }
    my_free(&r);
    return 1;
}

int db_add_event(Db *d, u32 type, i64 p1, i64 p2, int days, Err err) {
    long st_t = 0, et_t = 0;
    if (days > 0) {
        // 持续时间：start_time=现在(Unix 时间戳)，end_time=现在+days 天。
        // dnf_event_log.start_time/end_time 是 int(11)，服务端按 time_t 解析；
        // 0/0 表示无时限（工具侧「永久」语义，与 GMTOOL set_event 插入 0,0 一致）。
        time_t now = time(NULL);
        st_t = (long)now;
        et_t = (long)now + (long)days * 86400L;
    }
    Str s = {0};
    str_catf(&s,
             "INSERT INTO d_taiwan.dnf_event_log "
             "(occ_time,event_type,parameter1,parameter2,server_id,event_flag,start_time,end_time) "
             "VALUES (0,%u,%lld,%lld,0,0,%ld,%ld)", type, (long long)p1, (long long)p2, st_t, et_t);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// 往活动字典（dnf_event_info）插一条：下拉里没有的活动，GM 自己补定义。
// 表是 utf8，连接默认 utf8mb4 是其超集，直接插即可，不必切 charset。
int db_add_event_info(Db *d, u32 id, const char *name, const char *explain, Err err) {
    char *en = mysql_escape(name ? name : "");
    char *ee = mysql_escape(explain ? explain : "");
    Str s = {0};
    str_catf(&s,
             "INSERT INTO d_taiwan.dnf_event_info "
             "(event_id,event_name,event_explain,apply_type,start_date,end_date) "
             "VALUES (%u,'%s','%s',0,'0000-00-00','0000-00-00')", id, en, ee);
    free(en);
    free(ee);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_clear_events(Db *d, Err err) {
    Str s = {0};
    str_cat(&s, "DELETE FROM d_taiwan.dnf_event_log");
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_del_event(Db *d, u64 log_id, Err err) {
    Str s = {0};
    str_catf(&s, "DELETE FROM d_taiwan.dnf_event_log WHERE log_id=%llu", (unsigned long long)log_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// ---------------- 封停 ----------------
int db_list_bans(Db *d, Vec *out, Err err) {
    Str s = {0};
    str_cat(&s,
            "SELECT p.m_id,p.punish_type,p.punish_value,p.start_time,p.end_time,p.reason,"
            "a.accountname,"
            "IFNULL(c.charac_name,'') "
            "FROM d_taiwan.member_punish_info p "
            "LEFT JOIN d_taiwan.accounts a ON a.UID=p.m_id "
            "LEFT JOIN taiwan_cain.charac_info c ON c.charac_no=p.punish_value AND p.punish_type=2 "
            // 到期的封禁已经失效，摆出来只会让人误判
            "WHERE p.apply_flag<>0 AND p.end_time>NOW() ORDER BY p.m_id");
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);

    // 附 IP
    Str ip = {0};
    for (int i = 0; i < r.nrow; i++) {
        BanRow *b = VEC_ADD(out, BanRow);
        b->mid = (u32)my_u64(&r, i, 0, 0);
        b->type = (int)my_i64(&r, i, 1, 0);
        b->value = (int)my_i64(&r, i, 2, 0);
        b->charac_no = b->type == 2 ? (u32)b->value : 0;
        cp_text(b->start, sizeof(b->start), &r, i, 3);
        cp_text(b->end, sizeof(b->end), &r, i, 4);
        cp_text(b->reason, sizeof(b->reason), &r, i, 5);
        cp_text(b->account, sizeof(b->account), &r, i, 6);
        cp_text(b->char_name, sizeof(b->char_name), &r, i, 7);
    }
    my_free(&r);
    str_free(&s);

    str_cat(&ip, "SELECT m_id,login_ip FROM taiwan_login.login_account_3");
    MyResult r2 = {0};
    if (select1(d, &ip, &r2)) {
        for (int i = 0; i < out->n; i++) {
            BanRow *b = VEC_AT(out, BanRow, i);
            for (int k = 0; k < r2.nrow; k++) {
                if ((u32)my_u64(&r2, k, 0, 0) == b->mid) {
                    cp_text(b->ip, sizeof(b->ip), &r2, k, 1);
                    break;
                }
            }
        }
    }
    my_free(&r2);
    str_free(&ip);
    return 1;
}

int db_ban(Db *d, u32 mid, int days, int type, int value, const char *reason, Err err) {
    char now[32], end[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(now, sizeof(now), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    if (days <= 0) {
        // 永久封禁：结束时间推到很远
        snprintf(end, sizeof(end), "2099-12-31 23:59:59");
    } else {
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart += (u64)days * 864000000000ull;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        FileTimeToSystemTime(&ft, &st);
        snprintf(end, sizeof(end), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
    }

    set_names(d, "latin1");
    char *e = mysql_escape(reason ? reason : "");
    Str s = {0};
    str_catf(&s,
             "REPLACE INTO d_taiwan.member_punish_info "
             "(m_id,punish_type,occ_time,punish_value,apply_flag,start_time,end_time,reason) "
             "VALUES (%u,%d,'%s',%d,2,'%s','%s','%s')",
             mid, type, now, value, now, end, e);
    free(e);
    int ok = run(d, &s);
    str_free(&s);
    set_names(d, "utf8");
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// type=0 全部清除，type=1 只清账号封禁，type=2 只清角色封禁
int db_unban(Db *d, u32 mid, int type, Err err) {
    if (type == 0) return db_reset_punish(d, mid, err);
    Str s = {0};
    str_catf(&s, "DELETE FROM d_taiwan.member_punish_info WHERE m_id=%u AND punish_type=%d", mid, type);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "解封：%s", d->err);
    return ok;
}

int db_find_mid(Db *d, const char *name, u32 *mid, Err err) {
    set_names(d, "latin1");
    char *e = mysql_escape(name);
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT m_id FROM taiwan_cain.charac_info WHERE charac_name='%s' LIMIT 1", e);
    free(e);
    int ok = select1(d, &s, &r);
    if (ok && r.nrow) *mid = (u32)my_u64(&r, 0, 0, 0);
    else if (ok) { ok = 0; snprintf(err, ERRBUF, "未找到角色"); }
    else snprintf(err, ERRBUF, "%s", d->err);
    my_free(&r);
    str_free(&s);
    set_names(d, "utf8");
    return ok;
}

// ---------------- 账号 ----------------
// 服务端存的是无盐 md5（与官方 accounts.password 一致），改完立即生效
int db_reset_pwd(Db *d, u32 mid, const char *pwd, Err err) {
    char hex[33];
    hash_md5(pwd, strlen(pwd ? pwd : ""), hex);
    Str s = {0};
    str_catf(&s, "UPDATE d_taiwan.accounts SET password='%s' WHERE UID=%u", hex, mid);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// 建角次数打满后账号就建不了新角色，清零即解除
int db_unlock_create(Db *d, u32 mid, Err err) {
    Str s = {0};
    str_catf(&s, "UPDATE d_taiwan.limit_create_character SET count=0 WHERE m_id=%u", mid);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_online_accounts(Db *d, Vec *out) {
    Str s = {0};
    str_cat(&s, "SELECT m_id FROM taiwan_login.login_account_3 WHERE login_status=1");
    MyResult r = {0};
    int ret = select1(d, &s, &r);
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        u32 *p = VEC_ADD(out, u32);
        *p = (u32)my_u64(&r, i, 0, 0);
    }
    my_free(&r);
    return ret;
}

// ---------------- 整库备份用：列出所有表 ----------------
static int db_list_tables(Db *d, Vec *out, Err err) {
    Str s = {0};
    str_cat(&s,
            "SELECT CONCAT(table_schema,'.',table_name) FROM information_schema.tables "
            "WHERE table_schema IN ('taiwan_cain','taiwan_cain_2nd','d_taiwan','taiwan_login',"
            "'taiwan_billing','taiwan_game_event') ORDER BY table_schema,table_name");
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        Str *t = VEC_ADD(out, Str);
        memset(t, 0, sizeof(*t));
        const char *v = my_text(&r, i, 0);
        str_set(t, v ? v : "");
    }
    my_free(&r);
    return 1;
}

// ---------------- 整库备份 / 还原 ----------------
int is_bin_type(const char *t) {
    return str_istr(t, "blob") || str_istr(t, "binary") || strcmp(t, "geometry") == 0;
}

int is_num_type(const char *t) {
    static const char *k[] = {"tinyint", "smallint", "mediumint", "int", "bigint",
                              "decimal", "float", "double", "bit"};
    for (int i = 0; i < COUNT_OF(k); i++)
        if (!strcmp(t, k[i])) return 1;
    return 0;
}

// 写一个 SQL 字面量；binary 走 X'..'，文本逐字节转义（保证不含裸换行）
void sql_lit(Str *o, const char *p, int n, int binary) {
    static const char *hd = "0123456789ABCDEF";
    if (n < 0) n = 0;
    if (binary) {
        str_cat(o, "X'");
        for (int i = 0; i < n; i++) {
            u8 c = (u8)p[i];
            str_catc(o, hd[c >> 4]);
            str_catc(o, hd[c & 15]);
        }
        str_catc(o, '\'');
        return;
    }
    str_catc(o, '\'');
    for (int i = 0; i < n; i++) {
        u8 c = (u8)p[i];
        switch (c) {
            case '\'': str_cat(o, "''"); break;
            case '\\': str_cat(o, "\\\\"); break;
            case '\n': str_cat(o, "\\n"); break;
            case '\r': str_cat(o, "\\r"); break;
            case 0x1a: str_cat(o, "\\Z"); break;
            case 0: str_cat(o, "\\0"); break;
            default: str_catc(o, (char)c);
        }
    }
    str_catc(o, '\'');
}

int split_tbl(const char *full, char *sc, int sccap, char *tb, int tbcap) {
    const char *dot = strchr(full, '.');
    if (!dot || dot == full || !dot[1]) return 0;
    snprintf(sc, (size_t)sccap, "%.*s", (int)(dot - full), full);
    snprintf(tb, (size_t)tbcap, "%s", dot + 1);
    return 1;
}

void db_dump_path(char *out, int cap) {
    art_path(out, cap, "backup", "db", "sql");
}

int db_dump_sql(Db *d, const char *path, int *tables, u64 *rows, Err err) {
    Vec tabs;
    vec_init(&tabs, sizeof(Str));
    if (!db_list_tables(d, &tabs, err)) { vec_free(&tabs); return 0; }
    set_names(d, "utf8");

    Str out = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&out, "-- DNF GM 数据库备份 · %s\n-- %s\nSET NAMES utf8mb4;\nSET FOREIGN_KEY_CHECKS=0;\n\n",
             ts, d->host);
    if (!file_write(path, out.p, (size_t)out.n)) {
        snprintf(err, ERRBUF, "无法写入 %s", path);
        str_free(&out);
        vec_free(&tabs);
        return 0;
    }
    str_clear(&out);

    int nt = 0;
    u64 nr = 0;
    for (int ti = 0; ti < tabs.n; ti++) {
        Str *full = VEC_AT(&tabs, Str, ti);
        char sc[80], tb[128];
        if (!split_tbl(str_c(full), sc, sizeof(sc), tb, sizeof(tb))) continue;

        // 1) 建表语句（IF NOT EXISTS，还原时不会砸掉已有数据）
        Str q = {0};
        MyResult r = {0};
        str_catf(&q, "SHOW CREATE TABLE `%s`.`%s`", sc, tb);
        if (!select1(d, &q, &r)) { my_free(&r); str_free(&q); continue; }
        const char *ddl = r.nrow ? my_text(&r, 0, 1) : NULL;
        if (ddl && *ddl) {
            str_catf(&out, "\n-- %s.%s\n", sc, tb);
            if (strstr(ddl, "CREATE TABLE `")) {
                str_cat(&out, "CREATE TABLE IF NOT EXISTS `");
                str_cat(&out, tb);
                str_cat(&out, "`");
                const char *p = strstr(ddl, "CREATE TABLE `");
                p = p ? strchr(p + 14, '`') : NULL;    // 跳过表名，从 ")" 之前开始拼
                str_cat(&out, p ? p + 1 : "");
            } else {
                str_cat(&out, ddl);
            }
            str_cat(&out, ";\n");
        }
        if (out.n) {                    // 建表语句要立刻落盘，不能攒到最后
            file_append(path, out.p, out.n);
            str_clear(&out);
        }
        my_free(&r);
        str_clear(&q);

        // 2) 列名 / 类型
        Vec names, types;
        vec_init(&names, sizeof(Str));
        vec_init(&types, sizeof(Str));
        str_catf(&q, "SELECT COLUMN_NAME,DATA_TYPE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
                 sc, tb);
        if (select1(d, &q, &r)) {
            for (int i = 0; i < r.nrow; i++) {
                Str *a = VEC_ADD(&names, Str); memset(a, 0, sizeof(*a));
                str_set(a, my_text(&r, i, 0) ? my_text(&r, i, 0) : "");
                Str *b = VEC_ADD(&types, Str); memset(b, 0, sizeof(*b));
                str_set(b, my_text(&r, i, 1) ? my_text(&r, i, 1) : "");
            }
        }
        my_free(&r);
        str_clear(&q);
        if (!names.n) {
            for (int i = 0; i < names.n; i++) str_free(VEC_AT(&names, Str, i));
            for (int i = 0; i < types.n; i++) str_free(VEC_AT(&types, Str, i));
            vec_free(&names); vec_free(&types); str_free(&q);
            continue;
        }

        // 3) 数据
        str_catf(&q, "SELECT * FROM `%s`.`%s`", sc, tb);
        if (!select1(d, &q, &r)) {
            my_free(&r); str_free(&q);
            for (int i = 0; i < names.n; i++) str_free(VEC_AT(&names, Str, i));
            for (int i = 0; i < types.n; i++) str_free(VEC_AT(&types, Str, i));
            vec_free(&names); vec_free(&types);
            continue;
        }

        Str ins = {0};
        str_catf(&ins, "INSERT INTO `%s`.`%s` (", sc, tb);
        for (int i = 0; i < names.n; i++) {
            if (i) str_cat(&ins, ",");
            str_catf(&ins, "`%s`", str_c(VEC_AT(&names, Str, i)));
        }
        str_cat(&ins, ") VALUES ");

        int rows_in = 0;
        for (int i = 0; i < r.nrow; i++) {
            if (rows_in) str_catc(&ins, ',');
            str_catc(&ins, '(');
            for (int c = 0; c < names.n; c++) {
                if (c) str_catc(&ins, ',');
                const char *v = my_text(&r, i, c);
                if (!v) { str_cat(&ins, "NULL"); continue; }
                int len = my_len(&r, i, c);
                const char *ty = str_c(VEC_AT(&types, Str, c));
                if (is_num_type(ty) && len > 0) str_catn(&ins, v, len);
                else sql_lit(&ins, v, len, is_bin_type(ty));
            }
            str_catc(&ins, ')');
            rows_in++;
            if (rows_in >= 64 || ins.n > (1 << 20)) {
                str_cat(&ins, ";\n");
                if (!file_append(path, ins.p, ins.n)) {
                    snprintf(err, ERRBUF, "写入失败：%s", path);
                    my_free(&r); str_free(&q); str_free(&ins);
                    nr += (u64)rows_in;
                    for (int k = 0; k < names.n; k++) str_free(VEC_AT(&names, Str, k));
                    for (int k = 0; k < types.n; k++) str_free(VEC_AT(&types, Str, k));
                    vec_free(&names); vec_free(&types); vec_free(&tabs); str_free(&out);
                    return 0;
                }
                nr += (u64)rows_in;
                rows_in = 0;
                str_clear(&ins);
                str_catf(&ins, "INSERT INTO `%s`.`%s` (", sc, tb);
                for (int k = 0; k < names.n; k++) {
                    if (k) str_cat(&ins, ",");
                    str_catf(&ins, "`%s`", str_c(VEC_AT(&names, Str, k)));
                }
                str_cat(&ins, ") VALUES ");
            }
        }
        if (rows_in) {
            str_cat(&ins, ";\n");
            file_append(path, ins.p, ins.n);
            nr += (u64)rows_in;
        }
        str_free(&ins);
        my_free(&r);
        str_free(&q);
        for (int k = 0; k < names.n; k++) str_free(VEC_AT(&names, Str, k));
        for (int k = 0; k < types.n; k++) str_free(VEC_AT(&types, Str, k));
        vec_free(&names);
        vec_free(&types);
        nt++;
    }

    str_clear(&out);
    str_cat(&out, "\nSET FOREIGN_KEY_CHECKS=1;\n");
    file_append(path, out.p, out.n);
    str_free(&out);
    for (int i = 0; i < tabs.n; i++) str_free(VEC_AT(&tabs, Str, i));
    vec_free(&tabs);
    if (tables) *tables = nt;
    if (rows) *rows = nr;
    {   // 只留最近 5 份整库备份
        char dir[MAX_PATH];
        art_dir("backup", dir, sizeof(dir));
        art_prune(dir, "db-", 5);
    }
    return 1;
}

// 逐条执行 SQL 文本（识别引号/注释，按 ';' 切）
int db_restore_sql(Db *d, const char *path, int *stmts, Err err) {
    Buf f = {0};
    if (!file_read(path, &f)) { snprintf(err, ERRBUF, "读不到 %s", path); return 0; }
    set_names(d, "utf8");

    Str cur = {0};
    int quote = 0, n = 0;
    for (size_t i = 0; i < f.n; i++) {
        char c = (char)f.p[i];
        if (quote) {
            str_catc(&cur, c);
            if (c == '\\' && i + 1 < f.n) { str_catc(&cur, (char)f.p[++i]); continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { quote = c; str_catc(&cur, c); continue; }
        if (c == '-' && i + 1 < f.n && f.p[i + 1] == '-') {
            while (i < f.n && f.p[i] != '\n') i++;
            str_catc(&cur, '\n');
            continue;
        }
        if (c == '#') {
            while (i < f.n && f.p[i] != '\n') i++;
            str_catc(&cur, '\n');
            continue;
        }
        if (c == '/' && i + 1 < f.n && f.p[i + 1] == '*') {
            i += 2;
            while (i + 1 < f.n && !(f.p[i] == '*' && f.p[i + 1] == '/')) i++;
            i++;
            continue;
        }
        if (c == ';') {
            if (cur.n) {
                if (!run(d, &cur)) { snprintf(err, ERRBUF, "第 %d 条：%s", n + 1, d->err); goto fail; }
                n++;
                str_clear(&cur);
            }
            continue;
        }
        str_catc(&cur, c);
    }
    if (cur.n) {
        int only_ws = 1;
        for (int i = 0; i < cur.n; i++)
            if (cur.p[i] > ' ') { only_ws = 0; break; }
        if (!only_ws && !run(d, &cur)) { snprintf(err, ERRBUF, "第 %d 条：%s", n + 1, d->err); goto fail; }
        if (!only_ws) n++;
    }
    str_free(&cur);
    buf_free(&f);
    if (stmts) *stmts = n;
    return 1;

fail:
    str_free(&cur);
    buf_free(&f);
    return 0;
}

static void vec_free_strs(Vec *v) {
    for (int i = 0; i < v->n; i++) str_free(VEC_AT(v, Str, i));
    vec_free(v);
}

// ---------------- 角色备份 / 还原 ----------------
// 只导一个角色（外加可选的账号共享数据），误删、误改时能定点回滚。
// 生成的是标准 SQL，还原直接复用 db_restore_sql。
typedef struct {
    const char *db;
    const char *tb;
    int         by_mid;     // 0 = 按 charac_no，1 = 按 m_id
} CharTab;

static const CharTab kCharTabs[] = {
    {"taiwan_cain",     "charac_stat",                  0},
    {"taiwan_cain",     "charac_info",                  0},
    {"taiwan_cain",     "new_charac_quest",             0},
    {"taiwan_cain",     "charac_quest_shop",            0},
    {"taiwan_cain",     "charac_titlebook",             0},
    {"taiwan_cain",     "pvp_result",                   0},
    {"taiwan_cain",     "charac_dimension_inout",       0},
    {"taiwan_cain",     "charac_blood_dungeon_reward",  0},
    {"taiwan_cain_2nd", "inventory",                    0},
    {"taiwan_cain_2nd", "charac_inven_expand",          0},
    {"taiwan_cain_2nd", "skill",                        0},
    {"taiwan_cain_2nd", "user_items",                   0},
    {"taiwan_cain_2nd", "creature_items",               0},
    // 以下按账号归属，账号下其它角色会一起受影响，单独放在文件末尾并注释提示
    {"taiwan_cain",     "account_cargo",                1},
    {"taiwan_cain",     "member_dungeon",               1},
};

void db_backup_char_path(char *out, int cap, const Charac *c) {
    char dir[MAX_PATH], ts[32];
    art_dir("backup", dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    snprintf(out, (size_t)cap, "%s\\char-%u-%s.sql", dir, c->no, ts);
}

int db_backup_char(Db *d, const Charac *c, const char *path, int *tables, u64 *rows, Err err) {
    set_names(d, "utf8");
    Str out = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&out,
             "-- DNF GM 角色备份 · %s\n-- charac_no=%u  %s  (m_id=%u)\n"
             "-- 还原会先删掉该角色在这些表里的现有行，再按备份写回\n"
             "SET NAMES utf8mb4;\nSET FOREIGN_KEY_CHECKS=0;\n\n",
             ts, c->no, c->name, c->mid);
    if (!file_write(path, out.p, (size_t)out.n)) {
        snprintf(err, ERRBUF, "无法写入 %s", path);
        str_free(&out);
        return 0;
    }
    str_clear(&out);

    int nt = 0;
    u64 nr = 0;
    for (int ti = 0; ti < COUNT_OF(kCharTabs); ti++) {
        const CharTab *ct = &kCharTabs[ti];
        Str q = {0};
        MyResult r = {0};

        // 列名与类型，用来决定值是裸数字、字符串还是十六进制
        Vec names, types;
        vec_init(&names, sizeof(Str));
        vec_init(&types, sizeof(Str));
        str_catf(&q, "SELECT COLUMN_NAME,DATA_TYPE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
                 ct->db, ct->tb);
        if (select1(d, &q, &r)) {
            for (int i = 0; i < r.nrow; i++) {
                Str *a = VEC_ADD(&names, Str); memset(a, 0, sizeof(*a));
                str_set(a, my_text(&r, i, 0) ? my_text(&r, i, 0) : "");
                Str *b = VEC_ADD(&types, Str); memset(b, 0, sizeof(*b));
                str_set(b, my_text(&r, i, 1) ? my_text(&r, i, 1) : "");
            }
        }
        my_free(&r);
        str_clear(&q);
        if (!names.n) { vec_free_strs(&names); vec_free_strs(&types); continue; }

        // 主列名可能不叫 charac_no（skill 那类），先看它在不在
        const char *key = ct->by_mid ? "m_id" : "charac_no";
        int have_key = 0;
        for (int i = 0; i < names.n; i++)
            if (!strcmp(str_c(VEC_AT(&names, Str, i)), key)) { have_key = 1; break; }
        if (!have_key) { vec_free_strs(&names); vec_free_strs(&types); continue; }

        u32 kv = ct->by_mid ? c->mid : c->no;
        str_catf(&q, "SELECT * FROM `%s`.`%s` WHERE `%s`=%u", ct->db, ct->tb, key, kv);
        if (!select1(d, &q, &r)) {
            my_free(&r);
            str_free(&q);
            vec_free_strs(&names);
            vec_free_strs(&types);
            continue;
        }
        str_free(&q);

        // 注释和 DELETE 必须先落盘：数据是边查边 append 的，
        // 攒到最后写会让「插入」跑到「删除」前面，还原等于清空。
        str_catf(&out, "-- %s.%s (%d 行)%s\n", ct->db, ct->tb, r.nrow,
                 ct->by_mid ? " · 账号共享" : "");
        if (r.nrow)
            str_catf(&out, "DELETE FROM `%s`.`%s` WHERE `%s`=%u;\n", ct->db, ct->tb, key, kv);
        file_append(path, out.p, (size_t)out.n);
        str_clear(&out);

        Str ins = {0};
        str_catf(&ins, "INSERT INTO `%s`.`%s` (", ct->db, ct->tb);
        for (int i = 0; i < names.n; i++) {
            if (i) str_cat(&ins, ",");
            str_catf(&ins, "`%s`", str_c(VEC_AT(&names, Str, i)));
        }
        str_cat(&ins, ") VALUES ");

        int rows_in = 0;
        for (int i = 0; i < r.nrow; i++) {
            if (rows_in) str_catc(&ins, ',');
            str_catc(&ins, '(');
            for (int k = 0; k < names.n; k++) {
                if (k) str_catc(&ins, ',');
                const char *v = my_text(&r, i, k);
                if (!v) { str_cat(&ins, "NULL"); continue; }
                int len = my_len(&r, i, k);
                const char *ty = str_c(VEC_AT(&types, Str, k));
                if (is_num_type(ty) && len > 0) str_catn(&ins, v, len);
                else sql_lit(&ins, v, len, is_bin_type(ty));
            }
            str_catc(&ins, ')');
            rows_in++;
            nr++;
            if (rows_in >= 64 || ins.n > (1 << 20)) {
                str_cat(&ins, ";\n");
                file_append(path, ins.p, (size_t)ins.n);
                str_clear(&ins);
                str_catf(&ins, "INSERT INTO `%s`.`%s` (", ct->db, ct->tb);
                for (int k = 0; k < names.n; k++) {
                    if (k) str_cat(&ins, ",");
                    str_catf(&ins, "`%s`", str_c(VEC_AT(&names, Str, k)));
                }
                str_cat(&ins, ") VALUES ");
                rows_in = 0;
            }
        }
        if (rows_in) {
            str_cat(&ins, ";\n");
            file_append(path, ins.p, (size_t)ins.n);
        }
        str_free(&ins);
        str_catc(&out, '\n');
        file_append(path, out.p, (size_t)out.n);
        str_clear(&out);

        nt++;
        my_free(&r);
        vec_free_strs(&names);
        vec_free_strs(&types);
    }
    if (tables) *tables = nt;
    if (rows) *rows = nr;
    {   // 角色备份只留最近 6 份
        char dir[MAX_PATH];
        art_dir("backup", dir, sizeof(dir));
        art_prune(dir, "char-", 6);
    }
    return 1;
}

// ---------------- 全服错误物品扫描 ----------------
// 覆盖一切能挂物品编号的地方：容器 blob（背包/穿戴/宠物栏/仓库/珠宝/账号金库）、
// 邮件附件、时装/宠物表行。角色多时较慢，只在后台线程调用。
typedef struct {
    const char *table;    // blob 所在的表
    const char *col;      // blob 列名
    const char *label;    // 界面上显示的容器名
    const char *keycol;   // 定位行的键：charac_no 或 m_id
} ScanCol;

static const ScanCol kScanCols[] = {
    {"taiwan_cain_2nd.inventory",           "inventory", "背包",     "charac_no"},
    {"taiwan_cain_2nd.inventory",           "equipslot", "穿戴栏",   "charac_no"},
    {"taiwan_cain_2nd.inventory",           "creature",  "宠物栏",   "charac_no"},
    {"taiwan_cain_2nd.charac_inven_expand", "cargo",     "仓库",     "charac_no"},
    {"taiwan_cain_2nd.charac_inven_expand", "jewel",     "珠宝",     "charac_no"},
    {"taiwan_cain.account_cargo",           "cargo",     "账号金库", "m_id"},
};

static u32 scan_col_key(const Charac *c, const char *keycol) {
    return strcmp(keycol, "m_id") == 0 ? c->mid : c->no;
}

// 一个容器里的全部异常槽位
static void scan_blob(Db *d, const Charac *c, const ScanCol *sc, Vec *out,
                      ItemHasName has, void *hctx) {
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT `%s` FROM %s WHERE %s=%u", sc->col, sc->table, sc->keycol,
             scan_col_key(c, sc->keycol));
    if (!select1(d, &s, &r) || !r.nrow || !my_text(&r, 0, 0)) {
        my_free(&r);
        str_free(&s);
        return;
    }
    Blob b = {0};
    Err e = {0};
    int ok = blob_unpack(my_text(&r, 0, 0), (size_t)my_len(&r, 0, 0), &b, e) && b.ok;
    my_free(&r);
    str_free(&s);
    if (!ok) return;
    for (int i = 0; i < b.n; i++) {
        char why[48];
        if (!item_bad(&b.items[i], has, hctx, why, sizeof(why))) continue;
        ScanRow *o = VEC_ADD(out, ScanRow);
        o->cno = c->no;
        o->mid = c->mid;
        snprintf(o->name, sizeof(o->name), "%s", c->name);
        snprintf(o->cont, sizeof(o->cont), "%s", sc->label);
        o->slot = i + 1;
        o->id = item_id(&b.items[i]);
        snprintf(o->why, sizeof(o->why), "%s", why);
    }
    blob_free(&b);
}

int db_scan_bad(Db *d, Vec *out, int *nchar, ItemHasName has, void *hctx, Err err) {
    vec_clear(out);
    if (nchar) *nchar = 0;

    Str s = {0};
    MyResult ch = {0};
    str_cat(&s, "SELECT c.charac_no,c.charac_name,c.m_id,a.accountname"
                " FROM taiwan_cain.charac_info c"
                " LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id"
                " WHERE c.delete_flag=0 ORDER BY c.charac_no");
    if (!select1(d, &s, &ch)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_clear(&s);

    Vec mids;                       // 账号金库按账号扫一次就够
    vec_init(&mids, sizeof(u32));
    for (int ci = 0; ci < ch.nrow; ci++) {
        Charac c;
        memset(&c, 0, sizeof(c));
        c.no = (u32)my_u64(&ch, ci, 0, 0);
        c.mid = (u32)my_u64(&ch, ci, 2, 0);
        cp_text(c.name, sizeof(c.name), &ch, ci, 1);
        const char *acc = my_text(&ch, ci, 3);
        if (nchar) (*nchar)++;

        for (int k = 0; k < COUNT_OF(kScanCols); k++) {
            const ScanCol *sc = &kScanCols[k];
            if (!strcmp(sc->keycol, "m_id")) {
                int dup = !c.mid;
                for (int q = 0; q < mids.n && !dup; q++)
                    if (*(u32 *)vec_at(&mids, q) == c.mid) dup = 1;
                if (dup) continue;
                *(u32 *)vec_add(&mids) = c.mid;
            }
            scan_blob(d, &c, sc, out, has, hctx);
        }

        // 邮件附件：没有 PVF 就没法凭编号判断，跳过
        if (has) {
            MyResult m = {0};
            str_clear(&s);
            str_catf(&s, "SELECT postal_id,item_id FROM taiwan_cain_2nd.postal "
                         "WHERE receive_charac_no=%u AND delete_flag=0", c.no);
            if (select1(d, &s, &m)) {
                for (int i = 0; i < m.nrow; i++) {
                    u32 id = (u32)my_u64(&m, i, 1, 0);
                    if (!id || has(id, hctx)) continue;
                    ScanRow *o = VEC_ADD(out, ScanRow);
                    o->cno = c.no;
                    o->mid = c.mid;
                    snprintf(o->name, sizeof(o->name), "%s", c.name);
                    snprintf(o->acc, sizeof(o->acc), "%s", acc ? acc : "");
                    snprintf(o->cont, sizeof(o->cont), "邮件");
                    o->id = id;
                    snprintf(o->why, sizeof(o->why), "PVF 无此编号");
                    o->kind = 3;
                    o->key = my_u64(&m, i, 0, 0);
                }
            }
            my_free(&m);
        }
    }
    // 补上账号名（上面按行拷过了，这里兜底容器行）
    my_free(&ch);
    str_free(&s);
    vec_free(&mids);

    // 时装 / 宠物表行（it_id 查不到、角色已删/不存在）
    Vec tb;
    vec_init(&tb, sizeof(TabOrphan));
    if (db_scan_tab_orphan(d, &tb, has, hctx, err)) {
        for (int i = 0; i < tb.n; i++) {
            TabOrphan *t = VEC_AT(&tb, TabOrphan, i);
            ScanRow *o = VEC_ADD(out, ScanRow);
            o->cno = t->cno;
            snprintf(o->name, sizeof(o->name), "%s", t->cname);
            snprintf(o->cont, sizeof(o->cont), "%s", t->avatar ? "时装" : "宠物");
            o->id = t->it_id;
            snprintf(o->why, sizeof(o->why), "%s", t->why);
            o->kind = t->avatar ? 1 : 2;
            o->key = t->ui_id;
        }
    }
    vec_free(&tb);

    // 账号名统一补齐：容器/时装/宠物行在生成时没带，按角色名回查一遍
    if (out->n) {
        Str q = {0};
        MyResult am = {0};
        str_cat(&q, "SELECT c.charac_no,a.accountname FROM taiwan_cain.charac_info c"
                    " LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id");
        if (select1(d, &q, &am)) {
            for (int i = 0; i < out->n; i++) {
                ScanRow *o = VEC_AT(out, ScanRow, i);
                if (o->acc[0]) continue;
                for (int k = 0; k < am.nrow; k++) {
                    if ((u32)my_u64(&am, k, 0, 0) == o->cno) {
                        const char *an = my_text(&am, k, 1);
                        snprintf(o->acc, sizeof(o->acc), "%s", an ? an : "");
                        break;
                    }
                }
            }
        }
        my_free(&am);
        str_free(&q);
    }
    return 1;
}

// 清掉清单里的错误物品。容器槽位按容器归并（一次读、清多格、一次写），
// 时装/宠物表行直接删，邮件清掉附件并保留金币。返回成功清除的条数。
int db_purge_bad(Db *d, Vec *rows, int *done, Err err) {
    if (done) *done = 0;
    int ok_n = 0;
    u8 *handled = (u8 *)calloc((size_t)(rows->n > 0 ? rows->n : 1), 1);
    if (!handled) return 0;

    for (int i = 0; i < rows->n; i++) {
        ScanRow *r = VEC_AT(rows, ScanRow, i);
        if (handled[i]) continue;

        if (r->kind == 0) {                       // 容器槽位
            const ScanCol *sc = NULL;
            for (int k = 0; k < COUNT_OF(kScanCols); k++)
                if (!strcmp(kScanCols[k].label, r->cont)) { sc = &kScanCols[k]; break; }
            if (!sc) { handled[i] = 1; continue; }

            Charac c;
            memset(&c, 0, sizeof(c));
            c.no = r->cno;
            c.mid = r->mid;
            Str s = {0};
            MyResult q = {0};
            str_catf(&s, "SELECT `%s` FROM %s WHERE %s=%u", sc->col, sc->table, sc->keycol,
                     scan_col_key(&c, sc->keycol));
            int have = select1(d, &s, &q) && q.nrow && my_text(&q, 0, 0);
            str_free(&s);
            if (!have) { my_free(&q); handled[i] = 1; continue; }

            Blob b = {0};
            Err e = {0};
            if (!(blob_unpack(my_text(&q, 0, 0), (size_t)my_len(&q, 0, 0), &b, e) && b.ok)) {
                my_free(&q);
                handled[i] = 1;
                continue;
            }
            my_free(&q);
            int cleared = 0;
            for (int j = i; j < rows->n; j++) {
                ScanRow *t = VEC_AT(rows, ScanRow, j);
                if (handled[j] || t->kind != 0 || t->cno != r->cno || strcmp(t->cont, r->cont))
                    continue;
                if (t->slot >= 1 && t->slot <= b.n) {
                    memset(&b.items[t->slot - 1], 0, sizeof(Item));
                    cleared++;
                }
                handled[j] = 1;
            }
            if (cleared) {
                Buf out;
                if (blob_pack(&b, &out)) {
                    if (db_save_blob(d, &c, sc->table, sc->col, out.p, out.n, err)) ok_n += cleared;
                    buf_free(&out);
                }
            }
            blob_free(&b);
        } else if (r->kind == 1 || r->kind == 2) {  // 时装 / 宠物表行
            Str s = {0};
            str_catf(&s, "DELETE FROM taiwan_cain_2nd.%s WHERE ui_id=%llu",
                     r->kind == 1 ? "user_items" : "creature_items",
                     (unsigned long long)r->key);
            if (run(d, &s)) ok_n++;
            else snprintf(err, ERRBUF, "%s", d->err);
            str_free(&s);
            handled[i] = 1;
        } else if (r->kind == 3) {                  // 邮件：清附件，金币留下
            mail_drop_attach(d, r->key);
            Str s = {0};
            str_catf(&s, "UPDATE taiwan_cain_2nd.postal SET item_id=0,add_info=0,"
                         "avata_flag=0,creature_flag=0 WHERE postal_id=%llu",
                     (unsigned long long)r->key);
            if (run(d, &s)) ok_n++;
            else snprintf(err, ERRBUF, "%s", d->err);
            str_free(&s);
            handled[i] = 1;
        } else {
            handled[i] = 1;
        }
    }
    free(handled);
    if (done) *done = ok_n;
    return 1;
}

// ---------------- 宠物 / 时装表内错误记录 ----------------
// 两张表不在背包 blob 里，直接按行检查：it_id 为 0 是空行（合法）；
// 角色已删 / 不存在，或 PVF 里查不到编号，才算错误行。
int db_scan_tab_orphan(Db *d, Vec *out, ItemHasName has, void *hctx, Err err) {
    vec_clear(out);
    Str s = {0};
    MyResult ch = {0};
    str_cat(&s, "SELECT charac_no,charac_name,delete_flag FROM taiwan_cain.charac_info ORDER BY charac_no");
    if (!select1(d, &s, &ch)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_clear(&s);

    for (int t = 0; t < 2; t++) {
        int avatar = (t == 0);
        MyResult r = {0};
        str_catf(&s, "SELECT ui_id,charac_no,it_id FROM taiwan_cain_2nd.%s ORDER BY ui_id",
                 avatar ? "user_items" : "creature_items");
        if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); my_free(&r); goto fail; }
        str_clear(&s);
        for (int i = 0; i < r.nrow; i++) {
            u64 ui = my_u64(&r, i, 0, 0);
            u32 cno = (u32)my_u64(&r, i, 1, 0);
            u32 it = (u32)my_u64(&r, i, 2, 0);
            if (!it) continue;                       // 空行合法
            int lo = 0, hi = ch.nrow - 1, f = -1;    // 角色表有序，二分
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                u32 v = (u32)my_u64(&ch, mid, 0, 0);
                if (v == cno) { f = mid; break; }
                if (v < cno) lo = mid + 1; else hi = mid - 1;
            }
            const char *why = NULL;
            if (f < 0) why = "角色不存在";
            else if (my_u64(&ch, f, 2, 0)) why = "角色已删除";
            else if (has && !has(it, hctx)) why = "PVF 无此编号";
            if (!why) continue;
            TabOrphan *o = VEC_ADD(out, TabOrphan);
            memset(o, 0, sizeof(*o));
            o->cno = cno;
            if (f >= 0) cp_text(o->cname, sizeof(o->cname), &ch, f, 1);
            o->avatar = avatar;
            o->ui_id = ui;
            o->it_id = it;
            snprintf(o->why, sizeof(o->why), "%s", why);
        }
        my_free(&r);
    }
    my_free(&ch);
    str_free(&s);
    return 1;
fail:
    my_free(&ch);
    str_free(&s);
    return 0;
}

int db_del_tab_orphan(Db *d, int avatar, u64 ui_id, Err err) {
    Str s = {0};
    str_catf(&s, "DELETE FROM taiwan_cain_2nd.%s WHERE ui_id=%llu",
             avatar ? "user_items" : "creature_items", (unsigned long long)ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}
