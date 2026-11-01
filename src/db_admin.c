// db_admin.c：活动 / 封停 / 账号 / 备份 / 扫描表操作。从 db.c 拆出，行为不变。
#include "db_priv.h"
#include "db.h"
#include "util.h"
#include "mysql.h"
#include "item.h"
#include "zlib.h"
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

    // reason 列是 utf8（整表 DEFAULT CHARSET=utf8），这里必须走正常的 utf8 连接。
    // 早先照搬 db_set_name 用了 latin1 连接，MySQL 会把送进去的 UTF-8 字节当成
    // latin1 再转一次码：库里存的是 C3A8C2BF… 这种二次编码，界面回读就是一坨乱码。
    // 角色名要 latin1 是因为服务端那条链路认的是原始字节，reason 没有这个包袱。
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

// VIP 标记在 d_taiwan.accounts.VIP（varchar，本库是 utf8）。连接默认 utf8mb4 是其超集，
// 直接读/转义写即可，不必切 charset。读不到行就留空，不让整次加载失败。
int db_get_vip(Db *d, u32 mid, char *out, int cap, Err err) {
    Str s = {0};
    str_catf(&s, "SELECT VIP FROM d_taiwan.accounts WHERE UID=%u", mid);
    MyResult r = {0};
    int ok = select1(d, &s, &r);
    if (ok && r.nrow) cp_text(out, cap, &r, 0, 0);
    else if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    my_free(&r);
    str_free(&s);
    return ok;
}

int db_set_vip(Db *d, u32 mid, const char *vip, Err err) {
    char *e = mysql_escape(vip ? vip : "0");
    Str s = {0};
    str_catf(&s, "UPDATE d_taiwan.accounts SET VIP='%s' WHERE UID=%u", e, mid);
    free(e);
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
    set_names(d, db_cs_name(d->charset));

    Str out = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    // 头里的 SET NAMES 必须跟连接实际用的字符集一致：还原时这条会被真的执行一遍，
    // 写成别的等于把连接切走，后面的转义全对不上号。
    str_catf(&out, "-- DNF GM 数据库备份 · %s\n-- %s\nSET NAMES %s;\nSET FOREIGN_KEY_CHECKS=0;\n\n",
             ts, d->host, db_cs_name(d->charset));
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
        if (!names.n) {                 // 查不到列定义：这张表跳过（没有任何 Str 要逐个释放）
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
//
// 全程包在一个事务里，这是必需的而不是锦上添花：备份是「先 DELETE 再 INSERT」，
// 第 N 条失败时前 N-1 条已经把原数据删干净了。不包事务的话，失败之后角色的行还在、
// 装备技能任务全没了，而且没法回退 —— 那不叫还原失败，那叫数据丢失。
//
// 一个诚实的限制：MySQL 里 CREATE / DROP / ALTER 会隐式提交事务，所以整库备份
// （带建表语句）做不到端到端原子，只能保证两个 DDL 之间的 DML 段是原子的。角色
// 备份是纯 DML，能完整回滚 —— 而它恰恰是最需要回滚的那一种。
int db_restore_sql(Db *d, const char *path, int *stmts, Err err) {
    Buf f = {0};
    if (!file_read(path, &f)) { snprintf(err, ERRBUF, "读不到 %s", path); return 0; }
    set_names(d, db_cs_name(d->charset));

    // 关自动提交，把后面所有语句收进同一个事务。
    // 这一步失败就不继续：下面每一步都假定自己在事务里，事实不是那样的话，
    // 出错时的表现会比不包事务更让人困惑。
    Str t = {0};
    str_cat(&t, "SET autocommit=0");
    if (!run(d, &t)) {
        snprintf(err, ERRBUF, "无法关闭自动提交：%s", d->err);
        str_free(&t);
        buf_free(&f);
        return 0;
    }
    str_clear(&t);
    str_cat(&t, "START TRANSACTION");
    if (!run(d, &t)) {
        snprintf(err, ERRBUF, "无法开启事务：%s", d->err);
        str_clear(&t);
        str_cat(&t, "SET autocommit=1");
        run(d, &t);
        str_free(&t);
        buf_free(&f);
        return 0;
    }
    str_free(&t);

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
    {
        Str s = {0};
        str_cat(&s, "COMMIT");
        int ok = run(d, &s);
        if (!ok) snprintf(err, ERRBUF, "提交失败：%s（前 %d 条已回滚或处于未知状态）", d->err, n);
        str_clear(&s);
        str_cat(&s, "SET autocommit=1");
        run(d, &s);
        str_free(&s);
        // 文件里可能自带 SET NAMES（备份头就有一条），执行时已经把连接切走了，
        // 收尾时切回本连接该用的，免得后续转义对不上号。
        set_names(d, db_cs_name(d->charset));
        if (!ok) return 0;
    }
    if (stmts) *stmts = n;
    return 1;

fail:
    str_free(&cur);
    buf_free(&f);
    {
        // 已经执行的语句全部作废。err 里是原始错误，回滚本身失败也得喊一声，
        // 否则用户会以为没写进去。
        Str s = {0};
        str_cat(&s, "ROLLBACK");
        if (!run(d, &s)) snprintf(err, ERRBUF, "%s（且回滚失败：%s）", err, d->err);
        str_clear(&s);
        str_cat(&s, "SET autocommit=1");
        run(d, &s);
        str_free(&s);
        set_names(d, db_cs_name(d->charset));
    }
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
    char db[64];
    char tb[96];
    int  by_mid;     // 0 = 按 charac_no，1 = 按 m_id
} CharTab;

// charac_view：账号的角色槽位视图，客户端的角色列表照着它画。
// 槽定长 296 字节：+0 charac_no(u32)、+4 名字(20)、+24 job、+25 grow_type、
// +26 lev。布局是从库里现存的原生槽逐字节量出来的，不是猜的。
#define CV_SLOT 296
#define CV_NAME 20

// 按 m_id 归属的表必须手工列，不能自动发现：那一列只说明「属于某个账号」，
// 里面混着封停记录（member_punish_info）、建角限制、计费数据。自动扫出来
// 再复制过去，等于把封号一起克隆 —— 那不是备份，是复制故障。
// 要跟着角色走的只有这两张：账号金库 + 已开图。
static const CharTab kMidTabs[] = {
    {"taiwan_cain", "account_cargo",  1},
    {"taiwan_cain", "member_dungeon", 1},
};

// 日志 / 流水 / 回收站 / 审核镜像表不参与：行数动辄几百万，复制过去既没
// 意义又会把备份文件撑爆、把克隆事务拖到超时。
static int tab_excluded(const char *tb) {
    static const char *kNoSuf[] = {"_log", "_del", "_history", "_backup", "_bak", "_old", "_mod"};
    size_t n = strlen(tb);
    for (int i = 0; i < COUNT_OF(kNoSuf); i++) {
        size_t m = strlen(kNoSuf[i]);
        if (n > m && strcmp(tb + n - m, kNoSuf[i]) == 0) return 1;
    }
    return 0;
}

// 扫 information_schema 把「按 charac_no 归属」的表全捞出来，再接上手工
// 白名单的账号级表。以前是一份写死的清单，漏了 11 张表 —— 克隆出的角色
// 缺这些行，界面上看不出异常，进游戏却被服务端校验踢掉，极难定位。
// 返回 1 = 成功，0 = 失败（err 已填）。
static int char_tabs(Db *d, Vec *out, Err err) {
    vec_clear(out);

    Str s = {0};
    MyResult r = {0};
    str_cat(&s, "SELECT TABLE_SCHEMA,TABLE_NAME FROM information_schema.COLUMNS "
                "WHERE COLUMN_NAME='charac_no' "
                "  AND TABLE_SCHEMA IN ('taiwan_cain','taiwan_cain_2nd') "
                "ORDER BY TABLE_SCHEMA,TABLE_NAME");
    int ok = select1(d, &s, &r);
    str_free(&s);
    if (!ok) {
        snprintf(err, ERRBUF, "扫角色归属表失败：%s", d->err);
        my_free(&r);
        return 0;
    }
    for (int i = 0; i < r.nrow; i++) {
        const char *sc = my_text(&r, i, 0), *tn = my_text(&r, i, 1);
        if (!sc || !tn || tab_excluded(tn)) continue;
        CharTab *e = VEC_ADD(out, CharTab);
        memset(e, 0, sizeof(*e));
        snprintf(e->db, sizeof(e->db), "%s", sc);
        snprintf(e->tb, sizeof(e->tb), "%s", tn);
    }
    my_free(&r);

    for (int i = 0; i < COUNT_OF(kMidTabs); i++) {
        CharTab *e = VEC_ADD(out, CharTab);
        memset(e, 0, sizeof(*e));
        snprintf(e->db, sizeof(e->db), "%s", kMidTabs[i].db);
        snprintf(e->tb, sizeof(e->tb), "%s", kMidTabs[i].tb);
        e->by_mid = 1;
    }
    return 1;
}

void db_backup_char_path(char *out, int cap, const Charac *c) {
    char dir[MAX_PATH], ts[32];
    art_dir("backup", dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    snprintf(out, (size_t)cap, "%s\\char-%u-%s.sql", dir, c->no, ts);
}

int db_backup_char(Db *d, const Charac *c, const char *path, int *tables, u64 *rows, Err err) {
    // 连接字符集和文件头的 SET NAMES 必须是同一个来源。
    // 以前这里一边 set_names("utf8")、一边把文件头写死成 utf8mb4，两者对不上；
    // 默认配置（45）下内容其实是按 mb3 读出来的，头里却声明 mb4。
    const char *cs = db_cs_name(d->charset);
    set_names(d, cs);
    Str out = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&out,
             "-- DNF GM 角色备份 · %s\n-- charac_no=%u  %s  (m_id=%u)\n"
             "-- 还原会先删掉该角色在这些表里的现有行，再按备份写回\n"
             "SET NAMES %s;\nSET FOREIGN_KEY_CHECKS=0;\n\n",
             ts, c->no, c->name, c->mid, cs);
    if (!file_write(path, out.p, (size_t)out.n)) {
        snprintf(err, ERRBUF, "无法写入 %s", path);
        str_free(&out);
        return 0;
    }
    str_clear(&out);

    int nt = 0;
    u64 nr = 0;

    // 表集运行时发现，不写死：库里凡带 charac_no 的表（日志类除外）都要跟着
    // 角色走。以前写死 15 张、实际有 26 张，漏掉的 11 张让克隆出来的角色是
    // 残的 —— 工具里看不出问题，游戏服务端一校验就把人踢了。
    Vec tabs;
    vec_init(&tabs, sizeof(CharTab));
    if (!char_tabs(d, &tabs, err)) { str_free(&out); vec_free(&tabs); return 0; }
    for (int ti = 0; ti < tabs.n; ti++) {
        const CharTab *ct = VEC_AT(&tabs, CharTab, ti);
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
    vec_free(&tabs);
    if (tables) *tables = nt;
    if (rows) *rows = nr;
    {   // 角色备份只留最近 6 份
        char dir[MAX_PATH];
        art_dir("backup", dir, sizeof(dir));
        art_prune(dir, "char-", 6);
    }
    return 1;
}

// ---------------- 角色克隆 ----------------
// 和「备份 → 还原」不是一回事：不生成 SQL 文件，直接在库里把整份数据复制一份。
// 做法是一条 INSERT ... SELECT，值全在服务端内部搬运，不经过客户端转码，
// 顺带避开了 blob（背包 / 穿戴栏）在客户端往返一趟可能出的编码问题。
//
// 三个必须处理的点：
// 1) 自增列要跳过。charac_info.charac_no、user_items.ui_id、creature_items.ui_id
//    都是 AUTO_INCREMENT，照搬原值要么撞主键、要么把新行挂到旧角色名下。
//    跳过后由服务端发号，新 charac_no 用 LAST_INSERT_ID() 取回。
// 2) charac_name 是唯一键，必须改名。截断在服务端用 LEFT 做（按字符数，
//    中文不会被砍半），写回和 db_set_name 一样走 latin1 连接，保持同一约定。
// 3) 账号共享表（account_cargo / member_dungeon）不复制——它们按 m_id 归属，
//    复制等于凭空多出一份账号金库。
typedef struct {
    const char *col;
    const char *expr;    // 该列改用这段 SQL 表达式（已转义的常量），NULL 表示原样搬
} CloneOvr;

// 返回值：1 = 成功（*rows 为复制行数）；2 = 该表没有 keycol 这一列，跳过；
//         0 = 出错（err 已填）
static int clone_rows(Db *d, const char *dbn, const char *tb, const char *keycol,
                      u32 src, const char *key_expr, const CloneOvr *ovr, int novr,
                      u64 *rows, Err err) {
    if (rows) *rows = 0;
    Vec names, ais;
    vec_init(&names, sizeof(Str));
    vec_init(&ais, sizeof(int));
    Str q = {0};
    MyResult r = {0};
    str_catf(&q, "SELECT COLUMN_NAME,EXTRA FROM information_schema.COLUMNS "
                 "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
             dbn, tb);
    int ok = select1(d, &q, &r);
    if (ok) {
        for (int i = 0; i < r.nrow; i++) {
            Str *a = VEC_ADD(&names, Str);
            memset(a, 0, sizeof(*a));
            str_set(a, my_text(&r, i, 0) ? my_text(&r, i, 0) : "");
            const char *x = my_text(&r, i, 1);
            *(int *)vec_add(&ais) = (x && strstr(x, "auto_increment")) ? 1 : 0;
        }
    } else {
        snprintf(err, ERRBUF, "读 %s.%s 的列信息失败：%s", dbn, tb, d->err);
    }
    my_free(&r);
    str_free(&q);
    if (!ok || !names.n) {
        if (ok) snprintf(err, ERRBUF, "表 %s.%s 不存在或没有列", dbn, tb);
        vec_free_strs(&names);
        vec_free(&ais);
        return 0;
    }

    int have_key = 0;
    for (int i = 0; i < names.n; i++)
        if (!strcmp(str_c(VEC_AT(&names, Str, i)), keycol)) { have_key = 1; break; }
    if (!have_key) {                       // 表里没这一列（skill 那类），跳过即可
        vec_free_strs(&names);
        vec_free(&ais);
        return 2;
    }

    Str ins = {0}, sel = {0};
    str_catf(&ins, "INSERT INTO `%s`.`%s` (", dbn, tb);
    int first = 1;
    for (int i = 0; i < names.n; i++) {
        const char *cn = str_c(VEC_AT(&names, Str, i));
        if (*(int *)vec_at(&ais, i)) continue;      // 自增列交给服务端发号
        const char *ex = NULL;
        for (int k = 0; k < novr; k++)
            if (!strcmp(ovr[k].col, cn)) { ex = ovr[k].expr; break; }
        if (!ex && !strcmp(cn, keycol)) ex = key_expr;
        if (first) first = 0;
        else { str_catc(&ins, ','); str_catc(&sel, ','); }
        str_catf(&ins, "`%s`", cn);
        if (ex) str_cat(&sel, ex);
        else str_catf(&sel, "`%s`", cn);
    }
    str_cat(&ins, ") SELECT ");
    str_cat(&ins, sel.p);
    str_catf(&ins, " FROM `%s`.`%s` WHERE `%s`=%u", dbn, tb, keycol, src);
    str_free(&sel);

    u64 aff = 0;
    ok = run_aff(d, &ins, &aff);
    if (!ok) snprintf(err, ERRBUF, "复制 %s.%s 失败：%s", dbn, tb, d->err);
    str_free(&ins);
    vec_free_strs(&names);
    vec_free(&ais);
    if (rows) *rows = aff;
    return ok ? 1 : 0;
}

int db_clone_char(Db *d, const Charac *c, char *out_name, int cap, u32 *out_no,
                  int *tables, u64 *rows, Err err) {
    if (tables) *tables = 0;
    if (rows) *rows = 0;
    if (out_no) *out_no = 0;
    if (out_name && cap > 0) out_name[0] = 0;

    // 源角色必须存在、且没被删
    Str q = {0};
    MyResult r = {0};
    str_catf(&q, "SELECT delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%u", c->no);
    if (!select1(d, &q, &r)) {
        snprintf(err, ERRBUF, "%s", d->err);
        my_free(&r);
        str_free(&q);
        return 0;
    }
    if (!r.nrow) {
        snprintf(err, ERRBUF, "源角色 charac_no=%u 不存在", c->no);
        my_free(&r);
        str_free(&q);
        return 0;
    }
    int deleted = (int)my_u64(&r, 0, 0, 0);
    my_free(&r);
    if (deleted) {
        str_free(&q);
        snprintf(err, ERRBUF, "源角色已删除，不能克隆");
        return 0;
    }

    // 新名字 = 原名截到 18 字符 + 数字后缀。charac_name 是 varchar(20)，
    // 留两位给后缀；LEFT 在服务端按字符截，中文不会被砍成半个字。
    str_clear(&q);
    str_catf(&q, "SELECT LEFT(charac_name,18) FROM taiwan_cain.charac_info WHERE charac_no=%u",
             c->no);
    if (!select1(d, &q, &r) || !r.nrow) {
        snprintf(err, ERRBUF, "读源角色名失败：%s", d->err);
        my_free(&r);
        str_free(&q);
        return 0;
    }
    char base[64];
    cp_text(base, sizeof(base), &r, 0, 0);
    my_free(&r);
    str_free(&q);
    if (!base[0]) snprintf(base, sizeof(base), "Clone");

    char cand[64];
    int found = 0;
    for (int k = 2; k <= 99; k++) {
        snprintf(cand, sizeof(cand), "%s%d", base, k);
        if (!db_name_taken(d, cand, 0)) { found = 1; break; }
    }
    if (!found) {
        snprintf(err, ERRBUF, "「%s」加 2~99 的后缀都被占了，先改个名再克隆", base);
        return 0;
    }

    Str s = {0};
    str_cat(&s, "BEGIN");
    if (!run(d, &s)) {
        snprintf(err, ERRBUF, "开启事务失败：%s", d->err);
        str_free(&s);
        return 0;
    }

    int ok = 1;
    int nt = 0;
    u64 nr = 0;
    u32 new_no = 0;

    // 1) charac_info：charac_no 是自增列会被跳过，唯一要覆盖的就是 charac_name
    {
        set_names(d, "latin1");          // 与 db_set_name 同一约定：latin1 连接写 UTF-8 字节
        char *e = mysql_escape(cand);
        char expr[160];
        snprintf(expr, sizeof(expr), "'%s'", e ? e : "");
        free(e);
        CloneOvr ovr[1] = {{"charac_name", expr}};
        u64 aff = 0;
        int ret = clone_rows(d, "taiwan_cain", "charac_info", "charac_no", c->no, NULL,
                             ovr, 1, &aff, err);
        set_names(d, "utf8");
        if (ret != 1 || aff != 1) {
            if (ret == 1)
                snprintf(err, ERRBUF, "复制 charac_info 影响了 %llu 行（应为 1）",
                         (unsigned long long)aff);
            ok = 0;
        } else {
            nt = 1;
            nr = aff;
        }
    }

    // 2) 新 charac_no：自增列被跳过，只能靠 LAST_INSERT_ID() 拿
    if (ok) {
        MyResult r2 = {0};
        str_clear(&s);
        str_cat(&s, "SELECT LAST_INSERT_ID()");
        if (select1(d, &s, &r2) && r2.nrow) new_no = (u32)my_u64(&r2, 0, 0, 0);
        my_free(&r2);
        if (!new_no) {
            snprintf(err, ERRBUF, "取新 charac_no 失败：%s", d->err);
            ok = 0;
        }
    }

    // 3) 其余按 charac_no 归属的表，键直接换成新号
    if (ok) {
        char kexpr[32];
        snprintf(kexpr, sizeof(kexpr), "%u", new_no);
        // 同 db_backup_char：表集运行时发现，不写死清单
        Vec tabs;
        vec_init(&tabs, sizeof(CharTab));
        if (!char_tabs(d, &tabs, err)) { ok = 0; }
        for (int ti = 0; ok && ti < tabs.n; ti++) {
            const CharTab *ct = VEC_AT(&tabs, CharTab, ti);
            if (ct->by_mid) continue;                        // 账号共享数据不复制
            if (!strcmp(ct->tb, "charac_info")) continue;    // 已经处理过
            u64 n2 = 0;
            int ret = clone_rows(d, ct->db, ct->tb, "charac_no", c->no, kexpr, NULL, 0,
                                 &n2, err);
            if (ret == 0) { ok = 0; break; }
            if (ret == 1 && n2) { nt++; nr += n2; }
        }
        vec_free(&tabs);
    }

    // 4) 账号的角色槽位视图：客户端的角色列表就是照着这张表画的。
    //    克隆出的角色不在这儿占一格，界面上就永远看不见 —— charac_info 里
    //    老老实实写着两行也没用，登录时 charac_count 照旧是 1。
    //    它按 m_id 归属，进不了上面那份 charac_no 清单，所以只能单独处理。
    //    槽定长 296 字节：+0 charac_no(u32)、+4 名字(20)、+24 job、
    //    +25 grow_type、+26 lev。新槽拿源角色那格当模板，改掉这三项即可。
    if (ok) {
        MyResult rv = {0};
        u32 nlev = 0;
        str_clear(&s);
        str_catf(&s, "SELECT `lev` FROM taiwan_cain.charac_info WHERE `charac_no`=%u", new_no);
        if (select1(d, &s, &rv) && rv.nrow) nlev = (u32)my_u64(&rv, 0, 0, 0);
        my_free(&rv);

        str_clear(&s);
        str_catf(&s, "SELECT `info` FROM taiwan_cain.charac_view WHERE `m_id`=%u", c->mid);
        if (select1(d, &s, &rv) && rv.nrow) {
            const char *bp = my_text(&rv, 0, 0);
            int bn = my_len(&rv, 0, 0);
            Buf plain = {0};
            if (bp && bn > 4 && zlib_unpack((const u8 *)bp + 4, (size_t)(bn - 4), &plain)) {
                int nslot = plain.n / CV_SLOT;
                int t = -1;
                for (int k = 1; k < nslot; k++) {          // 0 号槽是源角色，从 1 开始找空位
                    int z = 1;
                    for (int j = 0; j < CV_SLOT; j++)
                        if (plain.p[k * CV_SLOT + j]) { z = 0; break; }
                    if (z) { t = k; break; }
                }
                if (t >= 0) {
                    u8 *q = plain.p + t * CV_SLOT;
                    memcpy(q, plain.p, CV_SLOT);           // 以源角色那一格为模板
                    wr32(q + 0, new_no);
                    memset(q + 4, 0, 20);
                    memcpy(q + 4, cand, strlen(cand) < 20 ? strlen(cand) : 19);
                    q[26] = (u8)nlev;

                    Buf pk = {0};
                    zlib_pack(plain.p, plain.n, &pk);
                    // info 的格式是 [4 字节前缀][zlib 流]，跟容器 blob 一样。读的
                    // 时候剥了前缀，写回去就得原样补上 —— zlib_pack 只产 zlib 流
                    // 本身（0x78 0x01 起），它不知道前缀这回事。漏补的话槽位
                    // 整体错位，服务端读出来是空槽，克隆的角色照样不显示。
                    Buf out = {0};
                    buf_put(&out, bp, 4);
                    buf_put(&out, pk.p, pk.n);
                    Str u = {0};
                    str_cat(&u, "UPDATE taiwan_cain.charac_view SET `info`=");
                    sql_lit(&u, (const char *)out.p, (int)out.n, 1);
                    str_catf(&u, ", `charac_count`=(SELECT COUNT(*) FROM taiwan_cain.charac_info"
                                 " WHERE `m_id`=%u AND `delete_flag`=0) WHERE `m_id`=%u",
                             c->mid, c->mid);
                    if (!run(d, &u)) {
                        snprintf(err, ERRBUF, "更新 charac_view 失败：%s", d->err);
                        ok = 0;
                    }
                    str_free(&u);
                    buf_free(&out);
                    buf_free(&pk);
                }
                buf_free(&plain);
            }
            my_free(&rv);
        }
    }

    str_clear(&s);
    if (ok) {
        str_cat(&s, "COMMIT");
        if (!run(d, &s)) {
            snprintf(err, ERRBUF, "提交事务失败：%s", d->err);
            ok = 0;
        }
    }
    if (!ok) {
        str_clear(&s);
        str_cat(&s, "ROLLBACK");
        run(d, &s);
    }
    str_free(&s);
    if (!ok) return 0;

    if (out_no) *out_no = new_no;
    if (out_name && cap > 0) snprintf(out_name, (size_t)cap, "%s", cand);
    if (tables) *tables = nt;
    if (rows) *rows = nr;
    db_undo_flush(0);
    return 1;
}

// ---------------- 任务模版 导入 / 导出 ----------------
// 任务进度分散在两张表里：new_charac_quest（已完成任务 blob + 20 个进行中任务槽
// 及其触发器）和 charac_quest_shop（任务商店换来的永久属性加成）。
// 模版就是把这两张表的一份快照存成纯 key=value 文本，blob 写成 MySQL 的 X'..'
// 十六进制字面量——导入时直接拼进 INSERT，不用再解一遍码。
// 典型用法：大号把「主线全清」导成模版，小号导入即跳过推图。
static const char *kQuestTabs[] = {"taiwan_cain.new_charac_quest", "taiwan_cain.charac_quest_shop"};

void db_quest_tpl_path(char *out, int cap, const Charac *c) {
    char dir[MAX_PATH], ts[32];
    art_dir("export", dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    snprintf(out, (size_t)cap, "%s\\任务模版-%u-%s.txt", dir, c->no, ts);
}

int db_export_quest(Db *d, const Charac *c, const char *path, Err err) {
    set_names(d, "utf8");
    Str out = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&out,
             "# DNF GM 任务模版 v1\n"
             "# 源角色 %s (charac_no=%u, m_id=%u)\n"
             "# 导出时间 %s\n"
             "#\n"
             "# clear_quest / quest_notify 是十六进制 blob，其余都是数值。\n"
             "# 导入会先删掉目标角色在这两张表里的现有行，再按模版写回。\n\n",
             c->name, c->no, c->mid, ts);
    if (!file_write(path, out.p, (size_t)out.n)) {
        snprintf(err, ERRBUF, "无法写入 %s", path);
        str_free(&out);
        return 0;
    }
    str_clear(&out);

    int nt = 0;
    for (int ti = 0; ti < COUNT_OF(kQuestTabs); ti++) {
        char dbn[64], tb[64];
        if (!split_tbl(kQuestTabs[ti], dbn, sizeof(dbn), tb, sizeof(tb))) continue;

        Vec names, types;
        vec_init(&names, sizeof(Str));
        vec_init(&types, sizeof(Str));
        Str q = {0};
        MyResult r = {0};
        str_catf(&q, "SELECT COLUMN_NAME,DATA_TYPE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
                 dbn, tb);
        if (select1(d, &q, &r)) {
            for (int i = 0; i < r.nrow; i++) {
                Str *a = VEC_ADD(&names, Str);
                memset(a, 0, sizeof(*a));
                str_set(a, my_text(&r, i, 0) ? my_text(&r, i, 0) : "");
                Str *b = VEC_ADD(&types, Str);
                memset(b, 0, sizeof(*b));
                str_set(b, my_text(&r, i, 1) ? my_text(&r, i, 1) : "");
            }
        }
        my_free(&r);
        str_clear(&q);
        if (!names.n) {
            vec_free_strs(&names);
            vec_free_strs(&types);
            continue;
        }

        str_catf(&q, "SELECT * FROM `%s`.`%s` WHERE charac_no=%u", dbn, tb, c->no);
        int ok = select1(d, &q, &r);
        str_free(&q);
        if (!ok) {
            snprintf(err, ERRBUF, "读 %s 失败：%s", kQuestTabs[ti], d->err);
            my_free(&r);
            vec_free_strs(&names);
            vec_free_strs(&types);
            str_free(&out);
            return 0;
        }
        str_catf(&out, "[%s]\n", kQuestTabs[ti]);
        if (!r.nrow) {
            str_cat(&out, "# 该角色在这张表里没有记录\n\n");
        } else {
            for (int k = 0; k < names.n; k++) {
                const char *cn = str_c(VEC_AT(&names, Str, k));
                if (!strcmp(cn, "charac_no")) continue;      // 主键不进模版，导入时按目标角色写
                const char *v = my_text(&r, 0, k);
                str_catf(&out, "%s=", cn);
                if (!v) str_cat(&out, "NULL");
                else if (is_bin_type(str_c(VEC_AT(&types, Str, k))))
                    sql_lit(&out, v, my_len(&r, 0, k), 1);
                else
                    str_cat(&out, v);
                str_catc(&out, '\n');
            }
            str_catc(&out, '\n');
            nt++;
        }
        file_append(path, out.p, (size_t)out.n);
        str_clear(&out);
        my_free(&r);
        vec_free_strs(&names);
        vec_free_strs(&types);
    }
    str_free(&out);
    if (!nt) {
        snprintf(err, ERRBUF, "角色「%s」还没有任何任务记录，没什么可导出的", c->name);
        return 0;
    }
    return 1;
}

// 值只允许数值、NULL 或 X'..' 十六进制字面量，其余一律拒绝：
// 这些字符串要原样拼进 SQL，放行等于把注入口子留在模版文件里。
static int tpl_val_ok(const char *v) {
    if (!v || !v[0]) return 0;
    if (!strcmp(v, "NULL")) return 1;
    if (v[0] == 'X' && v[1] == '\'') {
        size_t n = strlen(v);
        if (n < 3 || v[n - 1] != '\'') return 0;
        for (size_t i = 2; i + 1 < n; i++) {
            char c = v[i];
            int hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
            if (!hex) return 0;
        }
        return 1;
    }
    for (const char *p = v; *p; p++) {
        if (*p >= '0' && *p <= '9') continue;
        if (*p == '-' && p == v) continue;
        return 0;
    }
    return 1;
}

typedef struct {
    Str sec, key, val;
} TplLine;

static void tpl_lines_free(Vec *v) {
    for (int i = 0; i < v->n; i++) {
        TplLine *t = VEC_AT(v, TplLine, i);
        str_free(&t->sec);
        str_free(&t->key);
        str_free(&t->val);
    }
    vec_free(v);
}

// 取某张表在模版里配好的所有列，拼成 (cols, vals)。返回列数；-1 = 值不合法
static int tpl_cols(Vec *lines, const char *full, Vec *names, Str *cols, Str *vals, Err err) {
    int n = 0;
    for (int k = 0; k < names->n; k++) {
        const char *cn = str_c(VEC_AT(names, Str, k));
        if (!strcmp(cn, "charac_no")) continue;
        const char *v = NULL;
        for (int i = 0; i < lines->n; i++) {
            TplLine *t = VEC_AT(lines, TplLine, i);
            if (strcmp(str_c(&t->sec), full)) continue;
            if (strcmp(str_c(&t->key), cn)) continue;
            v = str_c(&t->val);
            break;
        }
        if (!v) continue;                       // 模版里没写这一列就留默认值
        if (!tpl_val_ok(v)) {
            snprintf(err, ERRBUF, "模版里 %s 的值是 %s，不是数值或 X'..' 十六进制", cn, v);
            return -1;
        }
        if (n) {
            str_catc(cols, ',');
            str_catc(vals, ',');
        }
        str_catf(cols, "`%s`", cn);
        str_cat(vals, v);
        n++;
    }
    return n;
}

int db_import_quest(Db *d, const Charac *c, const char *path, int *tables, Err err) {
    if (tables) *tables = 0;
    Buf f = {0};
    if (!file_read(path, &f) || !f.n) {
        snprintf(err, ERRBUF, "读不到模版文件 %s", path);
        buf_free(&f);
        return 0;
    }
    char head[80] = {0};
    size_t hn = f.n < sizeof(head) - 1 ? f.n : sizeof(head) - 1;
    memcpy(head, f.p, hn);
    if (!strstr(head, "DNF GM 任务模版")) {
        snprintf(err, ERRBUF, "这不是任务模版文件（开头缺标识行）");
        buf_free(&f);
        return 0;
    }

    Vec lines;
    vec_init(&lines, sizeof(TplLine));
    Str cur = {0};
    size_t i = 0;
    while (i < f.n) {
        size_t j = i;
        while (j < f.n && f.p[j] != '\n') j++;
        size_t e = j;
        if (e > i && f.p[e - 1] == '\r') e--;
        Str ln = {0};
        str_catn(&ln, (const char *)f.p + i, (int)(e - i));
        i = j + 1;
        const char *s = str_c(&ln);
        if (!s[0] || s[0] == '#') {
            str_free(&ln);
            continue;
        }
        if (s[0] == '[') {
            size_t L = strlen(s);
            if (L > 2 && s[L - 1] == ']') {
                str_clear(&cur);
                str_catn(&cur, s + 1, (int)(L - 2));
            }
            str_free(&ln);
            continue;
        }
        const char *eq = strchr(s, '=');
        if (eq && eq != s) {
            TplLine *t = VEC_ADD(&lines, TplLine);
            memset(t, 0, sizeof(*t));
            str_catn(&t->sec, str_c(&cur), cur.n);
            str_catn(&t->key, s, (int)(eq - s));
            str_cat(&t->val, eq + 1);
        }
        str_free(&ln);
    }
    str_free(&cur);
    buf_free(&f);

    Str s = {0};
    str_cat(&s, "BEGIN");
    if (!run(d, &s)) {
        snprintf(err, ERRBUF, "开启事务失败：%s", d->err);
        str_free(&s);
        tpl_lines_free(&lines);
        return 0;
    }

    int ok = 1;
    int nt = 0;
    for (int ti = 0; ti < COUNT_OF(kQuestTabs) && ok; ti++) {
        const char *full = kQuestTabs[ti];
        char dbn[64], tb[64];
        if (!split_tbl(full, dbn, sizeof(dbn), tb, sizeof(tb))) continue;

        int have_sec = 0;
        for (int i = 0; i < lines.n; i++)
            if (!strcmp(str_c(&VEC_AT(&lines, TplLine, i)->sec), full)) { have_sec = 1; break; }
        if (!have_sec) continue;                // 模版里没这一段就别动这张表

        Vec names;
        vec_init(&names, sizeof(Str));
        Str q = {0};
        MyResult r = {0};
        str_catf(&q, "SELECT COLUMN_NAME FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' ORDER BY ORDINAL_POSITION",
                 dbn, tb);
        if (select1(d, &q, &r)) {
            for (int i = 0; i < r.nrow; i++) {
                Str *a = VEC_ADD(&names, Str);
                memset(a, 0, sizeof(*a));
                str_set(a, my_text(&r, i, 0) ? my_text(&r, i, 0) : "");
            }
        }
        my_free(&r);
        str_free(&q);

        Str cols = {0}, vals = {0};
        int n = names.n ? tpl_cols(&lines, full, &names, &cols, &vals, err) : 0;
        vec_free_strs(&names);
        if (n < 0) {
            ok = 0;
        } else if (n == 0) {
            // 一段里一个字段都没有：多半是拿错了文件，别去删目标角色的现有数据
            snprintf(err, ERRBUF, "模版里 %s 这一段没有任何可用字段", tb);
            ok = 0;
        } else {
            str_clear(&s);
            str_catf(&s, "DELETE FROM `%s`.`%s` WHERE charac_no=%u", dbn, tb, c->no);
            if (!run(d, &s)) {
                snprintf(err, ERRBUF, "清掉 %s 的旧任务失败：%s", tb, d->err);
                ok = 0;
            } else {
                str_clear(&s);
                str_catf(&s, "INSERT INTO `%s`.`%s` (`charac_no`,%s) VALUES (%u,%s)",
                         dbn, tb, str_c(&cols), c->no, str_c(&vals));
                if (!run(d, &s)) {
                    snprintf(err, ERRBUF, "写入 %s 失败：%s", tb, d->err);
                    ok = 0;
                } else {
                    nt++;
                }
            }
        }
        str_free(&cols);
        str_free(&vals);
    }

    str_clear(&s);
    if (ok && !nt) {
        snprintf(err, ERRBUF, "模版里没有可导入的任务数据");
        ok = 0;
    }
    if (ok) {
        str_cat(&s, "COMMIT");
        if (!run(d, &s)) {
            snprintf(err, ERRBUF, "提交事务失败：%s", d->err);
            ok = 0;
        }
    }
    if (!ok) {
        str_clear(&s);
        str_cat(&s, "ROLLBACK");
        run(d, &s);
    }
    str_free(&s);
    tpl_lines_free(&lines);
    if (!ok) return 0;

    if (tables) *tables = nt;
    db_undo_flush(0);
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
                      ItemHasName has, ItemStackLim lim, void *hctx) {
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
        if (!item_bad(&b.items[i], has, lim, hctx, why, sizeof(why))) continue;
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

int db_scan_bad(Db *d, Vec *out, int *nchar, ItemHasName has, ItemStackLim lim, void *hctx, Err err) {
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
            scan_blob(d, &c, sc, out, has, lim, hctx);
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
