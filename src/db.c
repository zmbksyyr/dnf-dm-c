// db 实现。// port of dnfpkgtool/sqlManager2.py
#include "db.h"

// ---------------- 基础设施 ----------------
// 影响行数顺手带出来：写操作靠它区分「改了」和「一条都没碰到」。
// MyResult 里本来就有 affected，原来 run() 直接扔了。
static int run_aff(Db *d, Str *sql, u64 *aff) {
    MyResult r = {0};
    int ok = mysql_query(&d->conn, str_c(sql), &r);
    if (!ok) snprintf(d->err, ERRBUF, "%s", d->conn.err);
    else if (aff) *aff = r.affected;
    my_free(&r);
    return ok;
}

static int run(Db *d, Str *sql) { return run_aff(d, sql, NULL); }
static int select1(Db *d, Str *sql, MyResult *r);   // 实现见下

// ---------------- 一条写语句 ----------------
// 18 个 db_set_* 原本各自复制同一段「拼 SQL → run → 写 err → str_free」。
// 收敛到这里，以后加一个字段只要一行，不用再抄一遍样板。
//
// 三件原本没人管的事在这里一次做完：
//   1. 错误带上下文——「Unknown column 'pay_coin'」看不出是谁在改什么，
//      加上「改复活币」才知道去哪儿找；
//   2. UPDATE 命中 0 行当失败——表是空的、角色已被删、键列拼错，
//      这三种情况 MySQL 都返回「成功」，界面上就表现为「改了又弹回去」；
//   3. 0 行时再探一次记录是否存在，把「值本来就一样」和「压根没这条」分开，
//      免得正常操作被误报成失败。
static int db_exec(Db *d, const char *what, const char *table, const char *keycol, u32 key,
                   Err err, const char *fmt, ...) {
    char sql[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sql, sizeof(sql), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(sql)) {
        snprintf(err, ERRBUF, "%s：语句过长", what);
        return 0;
    }

    Str s = {0};
    str_cat(&s, sql);
    u64 aff = 0;
    int ok = run_aff(d, &s, &aff);
    str_free(&s);
    if (!ok) {
        snprintf(err, ERRBUF, "%s：%s", what, d->err);
        return 0;
    }
    if (aff > 0) return 1;

    // 0 行：先确认这条记录在不在。不在就是真失败（表空了 / 角色不存在 / 键列写错）
    str_catf(&s, "SELECT 1 FROM %s WHERE %s=%u LIMIT 1", table, keycol, key);
    MyResult r = {0};
    int exists = select1(d, &s, &r) && r.nrow > 0;
    my_free(&r);
    str_free(&s);
    if (!exists) {
        snprintf(err, ERRBUF, "%s：%s 里没有 %s=%u 的记录（表为空、角色已删或键列不对）",
                 what, table, keycol, key);
        return 0;
    }
    return 1;      // 记录在，值本来就一样：这是正常结果，不是失败
}

// ---------------- 事务 ----------------
// 一个动作写多张表时（改等级要动 charac_info + charac_stat + 清惩罚，
// 改点券要动 cash_cera + cash_cera_point），中途失败会留下半新半旧：
// 点券扣了没加，或者等级变了经验没变。这类不一致写后校验只能「发现」，
// 没法「抵消」，所以交给库自己回滚。
int db_tx_begin(Db *d, Err err) {
    Str s = {0};
    str_cat(&s, "START TRANSACTION");
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "开事务失败：%s", d->err);
    return ok;
}

int db_tx_commit(Db *d, Err err) {
    Str s = {0};
    str_cat(&s, "COMMIT");
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "提交失败：%s", d->err);
    return ok;
}

// 回滚本身失败没有补救手段（连接多半已经断了），不往上抛
void db_tx_rollback(Db *d) {
    Str s = {0};
    str_cat(&s, "ROLLBACK");
    run(d, &s);
    str_free(&s);
}

static int select1(Db *d, Str *sql, MyResult *r) {
    if (!mysql_query(&d->conn, str_c(sql), r)) {
        snprintf(d->err, ERRBUF, "%s", d->conn.err);
        return 0;
    }
    return 1;
}

static int set_names(Db *d, const char *cs) {
    Str s = {0};
    str_catf(&s, "SET NAMES %s", cs);
    int ok = run(d, &s);
    str_free(&s);
    return ok;
}

static void cp_str(char *dst, int cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, (size_t)cap, "%s", src);
}

static void reset_tables(void);   // 表存在性缓存（实现见下）
static int  have_table(Db *d, const char *full);

// 把 column 的 UTF-8 文本拷进定长缓冲
static void cp_text(char *dst, int cap, const MyResult *r, int row, int col) {
    const char *s = my_text(r, row, col);
    if (!s) { dst[0] = 0; return; }
    snprintf(dst, (size_t)cap, "%s", s);
}

int db_open(Db *d, const char *host, int port, const char *user, const char *pwd, int charset, Err err) {
    memset(d, 0, sizeof(*d));
    reset_tables();                 // 换库/重连后表结构可能不同，从头探
    cp_str(d->host, sizeof(d->host), host);
    cp_str(d->user, sizeof(d->user), user);
    cp_str(d->pwd, sizeof(d->pwd), pwd);
    d->port = port;
    d->charset = charset;
    if (!mysql_open(&d->conn, host, port, user, pwd, "", charset, 800, 2000)) {
        snprintf(err, ERRBUF, "%s", d->conn.err);
        return 0;
    }
    return 1;
}

void db_close(Db *d) { mysql_close(&d->conn); }
int  db_alive(const Db *d) { return mysql_alive(&d->conn); }

// ---------------- 角色 ----------------
// 搜索关键字同时匹配 角色名 / 账号名 / 角色编号
int db_list_chars(Db *d, Vec *out, int limit, const char *kw, Err err) {
    Str s = {0};
    str_catf(&s,
             "SELECT c.charac_no,c.charac_name,c.job,c.grow_type,c.lev,c.m_id,"
             "(SELECT MAX(l.login_status) FROM taiwan_login.login_account_3 l WHERE l.m_id=c.m_id),"
             "a.accountname"
             " FROM taiwan_cain.charac_info c"
             " LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id"
             " WHERE c.delete_flag=0");
    if (kw && *kw) {
        char *e = mysql_escape(kw);
        str_catf(&s, " AND (c.charac_name LIKE '%%%s%%' OR a.accountname LIKE '%%%s%%'"
                     " OR c.charac_no='%s')", e, e, e);
        free(e);
    }
    str_catf(&s, " ORDER BY c.charac_no DESC LIMIT %d", limit);

    MyResult r = {0};
    if (!select1(d, &s, &r)) {
        snprintf(err, ERRBUF, "%s", d->err);
        str_free(&s);
        return 0;
    }
    str_free(&s);

    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        Charac *c = VEC_ADD(out, Charac);
        c->no = (u32)my_u64(&r, i, 0, 0);
        cp_text(c->name, sizeof(c->name), &r, i, 1);
        c->job = (int)my_i64(&r, i, 2, 0);
        c->grow = (int)my_i64(&r, i, 3, 0);
        c->lev = (int)my_i64(&r, i, 4, 0);
        c->mid = (u32)my_u64(&r, i, 5, 0);
        c->online = my_i64(&r, i, 6, 0) == 1;
        cp_text(c->account, sizeof(c->account), &r, i, 7);
    }
    my_free(&r);
    return 1;
}

u32 db_count_chars(Db *d, const char *kw) {
    Str s = {0};
    str_cat(&s, "SELECT COUNT(*) FROM taiwan_cain.charac_info c"
                " LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id WHERE c.delete_flag=0");
    if (kw && *kw) {
        char *e = mysql_escape(kw);
        str_catf(&s, " AND (c.charac_name LIKE '%%%s%%' OR a.accountname LIKE '%%%s%%'"
                     " OR c.charac_no='%s')", e, e, e);
        free(e);
    }
    MyResult r = {0};
    u32 n = 0;
    if (select1(d, &s, &r)) n = (u32)my_u64(&r, 0, 0, 0);
    my_free(&r);
    str_free(&s);
    return n;
}

// 轮询专用：一条查询取回「角色列表 + 账号在线 + 当前在玩的角色」。
// 原来这套要三条往返（列表 / 计数 / 在线刷新），每 poll_sec 跑一次太亏。
// 在线判定与 db_refresh_online 一致：账号在线 且 正在玩该角色；
// 拿不到「当前角色」的映射时退回账号级判定，宁可多标也不漏标。
int db_poll_chars(Db *d, Vec *out, int limit, Err err) {
    static const char *kCurTables[] = {"taiwan_game_event.event_1306_account_reward",
                                       "taiwan_siroco.member_play_info"};
    const char *cur_tab = NULL;
    for (int i = 0; i < COUNT_OF(kCurTables) && !cur_tab; i++)
        if (have_table(d, kCurTables[i])) cur_tab = kCurTables[i];
    int have_login = have_table(d, "taiwan_login.login_account_3");

    Str s = {0};
    str_cat(&s, "SELECT c.charac_no,c.charac_name,c.job,c.grow_type,c.lev,c.m_id,a.accountname,");
    if (have_login)
        str_cat(&s, "(SELECT MAX(l.login_status) FROM taiwan_login.login_account_3 l WHERE l.m_id=c.m_id),");
    else
        str_cat(&s, "NULL,");
    if (cur_tab)
        str_catf(&s,
                 "(SELECT MAX(1) FROM %s t WHERE t.m_id=c.m_id AND t.charac_no=c.charac_no)",
                 cur_tab);
    else
        str_cat(&s, "NULL");
    str_catf(&s,
             " FROM taiwan_cain.charac_info c LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id"
             " WHERE c.delete_flag=0 ORDER BY c.charac_no DESC LIMIT %d", limit);

    MyResult r = {0};
    if (!select1(d, &s, &r)) {
        snprintf(err, ERRBUF, "%s", d->err);
        str_free(&s);
        return 0;
    }
    str_free(&s);

    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        Charac *c = VEC_ADD(out, Charac);
        c->no = (u32)my_u64(&r, i, 0, 0);
        cp_text(c->name, sizeof(c->name), &r, i, 1);
        c->job = (int)my_i64(&r, i, 2, 0);
        c->grow = (int)my_i64(&r, i, 3, 0);
        c->lev = (int)my_i64(&r, i, 4, 0);
        c->mid = (u32)my_u64(&r, i, 5, 0);
        cp_text(c->account, sizeof(c->account), &r, i, 6);   // 列序：6=accountname
        int acct = (int)my_i64(&r, i, 7, 0);                 // 7=login_status
        int playing = cur_tab ? (int)my_i64(&r, i, 8, 0) : 1;
        c->online = acct == 1 && playing == 1;
    }
    my_free(&r);
    return 1;
}

int db_is_online(Db *d, u32 mid) {
    Str s = {0};
    str_catf(&s, "SELECT MAX(login_status) FROM taiwan_login.login_account_3 WHERE m_id=%u", mid);
    MyResult r = {0};
    int on = 1;                       // 查不到保守当作在线，避免误写
    if (select1(d, &s, &r)) on = my_i64(&r, 0, 0, 1) == 1;
    my_free(&r);
    str_free(&s);
    return on;
}

// ---------------- 数据容器 ----------------
void db_free_data(CharData *cd) {
    blob_free(&cd->inv);
    blob_free(&cd->equip);
    blob_free(&cd->creature);
    blob_free(&cd->cargo);
    blob_free(&cd->jewel);
    blob_free(&cd->account_cargo);
    cd->loaded = 0;
}

static void take_blob(Db *d, const MyResult *r, int row, int col, Blob *b, const char *label, Err err) {
    if (my_text(r, row, col) == NULL) { b->ok = 1; return; }
    Err e = {0};
    if (!blob_unpack(my_text(r, row, col), (size_t)my_len(r, row, col), b, e)) {
        snprintf(b->err, sizeof(b->err), "%s", label);
        snprintf(err, ERRBUF, "%s: %s", label, e);
        (void)d;
    }
}

// ---------------- 可选表探测 ----------------
// 私服的表不一定齐（有的没有 billing、有的没有 pvp_result）。第一次用到时探一下
// 并缓存，缺表就把这组列写成 NULL：列序不变、其它数据照常读，也不会每次重试。
static struct {
    char name[64];
    int  ok;
} s_tab[16];
static int s_tabn;

static int have_table(Db *d, const char *full) {
    for (int i = 0; i < s_tabn; i++)
        if (str_ieq(s_tab[i].name, full)) return s_tab[i].ok;
    Str s = {0};
    str_catf(&s, "SELECT 1 FROM %s LIMIT 0", full);
    MyResult r = {0};
    int ok = select1(d, &s, &r);
    my_free(&r);
    str_free(&s);
    if (s_tabn < COUNT_OF(s_tab)) {
        snprintf(s_tab[s_tabn].name, sizeof(s_tab[s_tabn].name), "%s", full);
        s_tab[s_tabn].ok = ok;
        s_tabn++;
    }
    return ok;
}

static void reset_tables(void) { s_tabn = 0; }

// ---------------- 表/列自检 ----------------
// 私服的表结构千差万别：有的少了 pvp_result，有的把 inventory 建成 BLOB 而不是
// LONGBLOB，有的账号金库表干脆是空的。以前这些差异全都表现为同一种症状——
// 改完报「完成」，值却弹回去，用户除了「改不了」说不出别的。
//
// 这份清单是「本工具需要什么」的唯一出处。连上库时对一遍 information_schema，
// 缺表、缺列、列装不下，一次说清楚。optional=1 的缺了只降级对应功能，不影响主流程。
typedef struct {
    const char *table;
    const char *cols;      // 逗号分隔，本工具真正读写的列
    int         optional;  // 1 = 缺了只影响某一块功能
} DbNeed;

static const DbNeed kNeed[] = {
    // 角色主表：左栏、GM 表单、等级/职业/改名全在这
    {"taiwan_cain.charac_info",
     "charac_no,charac_name,lev,job,grow_type,delete_flag,inven_weight,fatigue,max_fatigue", 0},
    {"taiwan_cain.charac_stat", "charac_no,exp,add_slot_flag,expert_job_exp,used_fatigue", 0},
    // 容器：六个 blob 里四个在这两张表
    {"taiwan_cain_2nd.inventory",
     "charac_no,inventory,equipslot,creature,money,coin,inventory_capacity", 0},
    {"taiwan_cain_2nd.charac_inven_expand", "charac_no,cargo,jewel,cargo_capacity", 0},
    // 标量状态
    {"taiwan_cain_2nd.skill", "charac_no,remain_sp,remain_sp_2nd,remain_sfp_1st,remain_sfp_2nd", 0},
    {"taiwan_cain.charac_quest_shop", "charac_no,qp", 0},
    {"taiwan_cain.pvp_result", "charac_no,pvp_grade,win,pvp_point", 0},
    // 账号侧
    {"taiwan_login.login_account_3", "m_id,login_status,login_ip", 0},
    {"d_taiwan.accounts", "UID,accountname,password", 0},
    // 以下缺了只影响局部功能
    {"taiwan_cain.account_cargo", "m_id,cargo,money", 1},
    {"taiwan_billing.cash_cera", "account,cera", 1},
    {"taiwan_billing.cash_cera_point", "account,cera_point", 1},
    {"taiwan_cain.charac_manage_info", "charac_no,max_equip_level", 1},
    {"taiwan_cain.member_dungeon", "m_id,dungeon", 1},
    {"taiwan_cain.new_charac_quest", "charac_no,play_1,play_1_trigger", 1},
    {"d_taiwan.member_punish_info", "m_id", 1},
    {"d_taiwan.limit_create_character", "m_id,count", 1},
    {"d_taiwan.dnf_event_log", "log_id,event_type,parameter1,parameter2", 1},
    {"d_taiwan.dnf_event_info", "event_id,event_name,event_explain", 1},
    {"taiwan_cain_2nd.postal",
     "postal_id,receive_charac_no,delete_flag,item_id,avata_flag,creature_flag,add_info,gold,"
     "occ_time", 1},
    {"taiwan_cain_2nd.letter", "letter_id,charac_no", 1},
    {"taiwan_cain_2nd.user_items", "charac_no,ui_id,it_id,hidden_option,expire_date", 1},
    {"taiwan_cain_2nd.creature_items",
     "charac_no,ui_id,it_id,creature_type,stomach,name,expire_date", 1},
};

// blob 列的容量：写成 BLOB（64KB）而不是 LONGBLOB 的话，背包一满就写不下，
// 而 MySQL 非严格模式下是**静默截断**——写完长度校验能发现，但那时数据已经坏了。
static const char *kBlobCols[] = {
    "taiwan_cain_2nd.inventory.inventory",
    "taiwan_cain_2nd.inventory.equipslot",
    "taiwan_cain_2nd.inventory.creature",
    "taiwan_cain_2nd.charac_inven_expand.cargo",
    "taiwan_cain_2nd.charac_inven_expand.jewel",
    "taiwan_cain.account_cargo.cargo",
};

static u64 blob_type_cap(const char *dt) {
    if (str_ieq(dt, "tinyblob")) return 255;
    if (str_ieq(dt, "blob")) return 65535;
    if (str_ieq(dt, "mediumblob")) return 16777215;
    if (str_ieq(dt, "longblob")) return 4294967295u;   // 够用，不再查数据
    return 0;                                          // 不是 blob 类型
}

static void lower_str(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

// 把 schema.table 拆开，非法直接跳过
static int split_tab(const char *full, char *schema, int scap, char *tab, int tcap) {
    const char *dot = strchr(full, '.');
    if (!dot || dot == full) return 0;
    snprintf(schema, (size_t)scap, "%.*s", (int)(dot - full), full);
    snprintf(tab, (size_t)tcap, "%s", dot + 1);
    return tab[0] != 0;
}

int db_selfcheck(Db *d, char *out, int cap, Err err) {
    (void)err;      // 自检失败不该让人连不上库：错误只写进报告，不往外抛
    Str rep = {0};
    int bad = 0, ntab = 0, ncol = 0;

    for (int i = 0; i < (int)COUNT_OF(kNeed); i++) {
        const DbNeed *n = &kNeed[i];
        char schema[64], tab[64];
        if (!split_tab(n->table, schema, sizeof(schema), tab, sizeof(tab))) continue;

        Str s = {0};
        str_catf(&s, "SELECT COLUMN_NAME,DATA_TYPE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s'", schema, tab);
        MyResult r = {0};
        if (!select1(d, &s, &r)) {
            my_free(&r);
            str_free(&s);
            str_catf(&rep, "· 查不到 %s 的结构：%s\r\n", n->table, d->err);
            bad++;
            continue;
        }
        str_free(&s);

        if (r.nrow == 0) {                     // 整张表不在
            str_catf(&rep, "· 缺%s：%s（%s）\r\n", n->optional ? "表" : "表（影响主流程）",
                     n->table, n->optional ? "相关功能暂不可用" : "必须先修");
            bad++;
            my_free(&r);
            continue;
        }
        ntab++;

        // 把库里的列名拼成 ",a,b,c," 便于整词查找
        Str have = {0};
        for (int k = 0; k < r.nrow; k++) {
            str_catc(&have, ',');
            const char *c = my_text(&r, k, 0);
            if (c) str_cat(&have, c);
        }
        str_catc(&have, ',');
        if (have.p) lower_str(have.p);

        char cols[512];
        snprintf(cols, sizeof(cols), "%s", n->cols);
        for (char *p = strtok(cols, ","); p; p = strtok(NULL, ",")) {
            char key[96];
            snprintf(key, sizeof(key), ",%s,", p);
            lower_str(key);
            ncol++;
            if (!strstr(have.p ? have.p : "", key)) {
                str_catf(&rep, "· 缺列：%s.%s（%s）\r\n", n->table, p,
                         n->optional ? "相关功能不可用" : "必须先修");
                bad++;
            }
        }
        str_free(&have);
        my_free(&r);
    }

    // blob 容量：只有类型上限 < 4GB 时才值得去查实际长度
    for (int i = 0; i < (int)COUNT_OF(kBlobCols); i++) {
        char schema[64], tab[64], col[64];
        char full[160];
        snprintf(full, sizeof(full), "%s", kBlobCols[i]);
        char *dot = strchr(full, '.');            // schema.table.col
        if (!dot) continue;
        *dot = 0;
        char *dot2 = strchr(dot + 1, '.');
        if (!dot2) continue;
        *dot2 = 0;
        snprintf(schema, sizeof(schema), "%s", full);
        snprintf(tab, sizeof(tab), "%s", dot + 1);
        snprintf(col, sizeof(col), "%s", dot2 + 1);

        Str s = {0};
        str_catf(&s, "SELECT DATA_TYPE FROM information_schema.COLUMNS "
                     "WHERE TABLE_SCHEMA='%s' AND TABLE_NAME='%s' AND COLUMN_NAME='%s'",
                 schema, tab, col);
        MyResult r = {0};
        if (!select1(d, &s, &r) || r.nrow == 0) {
            my_free(&r);
            str_free(&s);
            continue;                              // 表或列缺失上面已经报过
        }
        char dt[48];
        snprintf(dt, sizeof(dt), "%s", my_text(&r, 0, 0) ? my_text(&r, 0, 0) : "");
        my_free(&r);

        u64 lim = blob_type_cap(dt);
        str_clear(&s);
        if (lim && lim < 4294967295u) {
            str_catf(&s, "SELECT MAX(LENGTH(%s)) FROM %s.%s", col, schema, tab);
            MyResult r2 = {0};
            if (select1(d, &s, &r2) && r2.nrow > 0) {
                u64 mx = my_u64(&r2, 0, 0, 0);
                if (mx > lim * 9 / 10) {
                    str_catf(&rep, "· %s.%s.%s 是 %s（上限 %llu 字节），当前已有 %llu 字节，"
                                   "再写就得截断\r\n",
                             schema, tab, col, dt, (unsigned long long)lim,
                             (unsigned long long)mx);
                    bad++;
                }
            }
            my_free(&r2);
        }
        str_free(&s);
    }

    if (bad == 0)
        snprintf(out, (size_t)cap, "表/列自检通过：%d 张表、%d 个列全部就位", ntab, ncol);
    else {
        // 报告可能比 cap 长，截断时保留开头（缺表/缺列最要紧）
        snprintf(out, (size_t)cap, "表/列自检发现 %d 处问题：\r\n%s", bad, str_c(&rep));
    }
    str_free(&rep);
    return bad;
}

// 一个「补充表」的列组：缺表时用同样多的 NULL 顶位，读取方索引不变
typedef struct {
    const char *table;
    const char *alias;
    const char *key;
    const char *cols;    // 已带别名的列清单
    int         ncols;
    int         by_mid;  // 1 = 按账号（m_id）关联，0 = 按角色（charac_no）
} TJoin;

static const TJoin kJoinExpand = {"taiwan_cain_2nd.charac_inven_expand", "e", "charac_no",
                                  "e.cargo,e.jewel,e.cargo_capacity", 3, 0};
static const TJoin kJoinExpandCap = {"taiwan_cain_2nd.charac_inven_expand", "e", "charac_no",
                                     "e.cargo_capacity", 1, 0};
static const TJoin kJoinAccount = {"taiwan_cain.account_cargo", "a", "m_id",
                                   "a.cargo,a.money", 2, 1};
static const TJoin kJoinAccountMoney = {"taiwan_cain.account_cargo", "a", "m_id", "a.money", 1, 1};
static const TJoin kJoinSkill = {"taiwan_cain_2nd.skill", "s", "charac_no",
                                 "s.remain_sp,s.remain_sp_2nd,s.remain_sfp_1st,s.remain_sfp_2nd", 4, 0};
static const TJoin kJoinQuest = {"taiwan_cain.charac_quest_shop", "q", "charac_no", "q.qp", 1, 0};
static const TJoin kJoinPvp = {"taiwan_cain.pvp_result", "p", "charac_no",
                               "p.pvp_grade,p.win,p.pvp_point", 3, 0};
static const TJoin kJoinCera = {"taiwan_billing.cash_cera", "c", "account", "c.cera", 1, 1};
static const TJoin kJoinCeraPoint = {"taiwan_billing.cash_cera_point", "cp", "account",
                                     "cp.cera_point", 1, 1};

// 载入容器时用：含 blob
static const TJoin kJLoad[] = {kJoinExpand,   kJoinAccount, kJoinSkill, kJoinQuest,
                               kJoinPvp,      kJoinCera,    kJoinCeraPoint};
// 只读标量时用：不带 blob，结果集体积小一个量级
static const TJoin kJScalar[] = {kJoinExpandCap, kJoinAccountMoney, kJoinSkill, kJoinQuest,
                                 kJoinPvp,       kJoinCera,         kJoinCeraPoint};

static void add_cols(Str *s, int *first, const TJoin *j, int ok) {
    if (ok) {
        if (!*first) str_catc(s, ',');
        str_cat(s, j->cols);
        *first = 0;
        return;
    }
    for (int i = 0; i < j->ncols; i++) {
        if (!*first) str_catc(s, ',');
        str_cat(s, "NULL");
        *first = 0;
    }
}

static void add_join(Str *s, const TJoin *j, int ok, u32 mid) {
    if (!ok) return;
    if (j->by_mid)
        str_catf(s, " LEFT JOIN %s %s ON %s.%s=%u", j->table, j->alias, j->alias, j->key, mid);
    else
        str_catf(s, " LEFT JOIN %s %s ON %s.%s=i.charac_no", j->table, j->alias, j->alias, j->key);
}

int db_load_data(Db *d, const Charac *c, CharData *out, Err err) {
    db_free_data(out);

    // 一次往返取回全部容器与标量：8 条独立查询 → 1 条 JOIN
    Str s = {0};
    str_cat(&s, "SELECT i.inventory,i.equipslot,i.creature,i.inventory_capacity,i.money,i.coin");
    int first = 0;
    for (int k = 0; k < COUNT_OF(kJLoad); k++)
        add_cols(&s, &first, &kJLoad[k], have_table(d, kJLoad[k].table));
    str_cat(&s, " FROM taiwan_cain_2nd.inventory i");
    for (int k = 0; k < COUNT_OF(kJLoad); k++)
        add_join(&s, &kJLoad[k], have_table(d, kJLoad[k].table), c->mid);
    str_catf(&s, " WHERE i.charac_no=%u", c->no);

    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    if (r.nrow == 0) {
        my_free(&r);
        snprintf(err, ERRBUF, "该角色没有背包记录");
        return 0;
    }
    take_blob(d, &r, 0, 0, &out->inv, "物品栏", err);
    take_blob(d, &r, 0, 1, &out->equip, "穿戴栏", err);
    take_blob(d, &r, 0, 2, &out->creature, "宠物栏", err);
    out->inv_cap = (u32)my_u64(&r, 0, 3, 249);
    out->money = my_u64(&r, 0, 4, 0);
    out->coin = my_u64(&r, 0, 5, 0);

    // 6 起按 kJLoad 的列序取：仓库3 / 账号2 / 技能4 / QP1 / PVP3 / 点券1 / 赠送1
    if (have_table(d, kJoinExpand.table)) {
        take_blob(d, &r, 0, 6, &out->cargo, "仓库", err);
        take_blob(d, &r, 0, 7, &out->jewel, "珠宝", err);
        out->cargo_cap = (u32)my_u64(&r, 0, 8, 0);
    } else {
        out->cargo.ok = 1;
        out->jewel.ok = 1;
    }
    if (have_table(d, kJoinAccount.table)) {
        take_blob(d, &r, 0, 9, &out->account_cargo, "账号金库", err);
        out->account_money = my_u64(&r, 0, 10, 0);
    } else {
        out->account_cargo.ok = 1;
    }
    if (have_table(d, kJoinSkill.table)) {
        out->sp = (int)my_i64(&r, 0, 11, 0);
        out->sp2 = (int)my_i64(&r, 0, 12, 0);
        out->tp = (int)my_i64(&r, 0, 13, 0);
        out->tp2 = (int)my_i64(&r, 0, 14, 0);
    }
    if (have_table(d, kJoinQuest.table)) out->qp = (int)my_i64(&r, 0, 15, 0);
    if (have_table(d, kJoinPvp.table)) {
        out->pvp_grade = (int)my_i64(&r, 0, 16, 0);
        out->pvp_win = (int)my_i64(&r, 0, 17, 0);
        out->pvp_point = (int)my_i64(&r, 0, 18, 0);
    }
    if (have_table(d, kJoinCera.table)) out->cera = my_u64(&r, 0, 19, 0);
    if (have_table(d, kJoinCeraPoint.table)) out->cera_point = my_u64(&r, 0, 20, 0);

    my_free(&r);
    out->loaded = 1;
    return out->inv.ok && out->equip.ok && out->creature.ok;
}

// ---------------- 写后回读 ----------------
// GM 写操作成功后把库里最新值读回内存，界面据此刷新成实时值。
// 只取标量字段、不碰 6 个 blob（那是 db_load_data 的活，太重）。
int db_read_charac(Db *d, u32 no, Charac *out, Err err) {
    Str s = {0};
    str_catf(&s, "SELECT charac_name,job,grow_type,lev FROM taiwan_cain.charac_info WHERE charac_no=%u", no);
    MyResult r = {0};
    if (!select1(d, &s, &r)) {
        snprintf(err, ERRBUF, "%s", d->err);
        str_free(&s);
        return 0;
    }
    str_free(&s);
    if (r.nrow == 0) {
        my_free(&r);
        snprintf(err, ERRBUF, "角色 %u 不存在", no);
        return 0;
    }
    cp_text(out->name, sizeof(out->name), &r, 0, 0);
    out->job = (int)my_i64(&r, 0, 1, 0);
    out->grow = (int)my_i64(&r, 0, 2, 0);
    out->lev = (int)my_i64(&r, 0, 3, 0);
    my_free(&r);
    return 1;
}

// 7 条独立查询 → 1 条 JOIN，且不带 blob
int db_read_state(Db *d, const Charac *c, CharData *out, Err err) {
    Str s = {0};
    str_cat(&s, "SELECT i.money,i.coin");
    int first = 0;
    for (int k = 0; k < COUNT_OF(kJScalar); k++)
        add_cols(&s, &first, &kJScalar[k], have_table(d, kJScalar[k].table));
    str_cat(&s, " FROM taiwan_cain_2nd.inventory i");
    for (int k = 0; k < COUNT_OF(kJScalar); k++)
        add_join(&s, &kJScalar[k], have_table(d, kJScalar[k].table), c->mid);
    str_catf(&s, " WHERE i.charac_no=%u", c->no);

    MyResult r = {0};
    int ok = select1(d, &s, &r);
    str_free(&s);
    if (!ok) {
        my_free(&r);
        snprintf(err, ERRBUF, "%s", d->err);
        return 0;
    }
    if (r.nrow == 0) {          // 没有背包行的角色：库里查不到就保留旧值
        my_free(&r);
        (void)err;
        return 1;
    }
    out->money = my_u64(&r, 0, 0, out->money);
    out->coin = my_u64(&r, 0, 1, out->coin);
    // 2 起按 kJScalar 的列序：仓库容量1 / 账号金币1 / 技能4 / QP1 / PVP3 / 点券1 / 赠送1
    if (have_table(d, kJoinExpand.table)) out->cargo_cap = (u32)my_u64(&r, 0, 2, out->cargo_cap);
    if (have_table(d, kJoinAccount.table)) out->account_money = my_u64(&r, 0, 3, out->account_money);
    if (have_table(d, kJoinSkill.table)) {
        out->sp = (int)my_i64(&r, 0, 4, out->sp);
        out->sp2 = (int)my_i64(&r, 0, 5, out->sp2);
        out->tp = (int)my_i64(&r, 0, 6, out->tp);
        out->tp2 = (int)my_i64(&r, 0, 7, out->tp2);
    }
    if (have_table(d, kJoinQuest.table)) out->qp = (int)my_i64(&r, 0, 8, out->qp);
    if (have_table(d, kJoinPvp.table)) {
        out->pvp_grade = (int)my_i64(&r, 0, 9, out->pvp_grade);
        out->pvp_win = (int)my_i64(&r, 0, 10, out->pvp_win);
        out->pvp_point = (int)my_i64(&r, 0, 11, out->pvp_point);
    }
    if (have_table(d, kJoinCera.table)) out->cera = my_u64(&r, 0, 12, out->cera);
    if (have_table(d, kJoinCeraPoint.table)) out->cera_point = my_u64(&r, 0, 13, out->cera_point);
    my_free(&r);
    (void)err;
    return 1;
}

// ---------------- 写前快照（会话内一份，可还原） ----------------
// 以前每写一个容器就落一个 .bin，一次会话能堆出几百个文件。
// 现在只记「本会话第一次写之前」的原值，攒够时间或退出时写成一份 undo-<时间>.sql，
// 直接用「还原备份」就能整体回滚。
typedef struct {
    char table[48];
    char field[24];
    u32  key;
    Str  raw;
} UndoEnt;

static Vec   s_undo;
static int   s_undo_dirty;
static u64   s_undo_ms;

static void undo_note(const char *table, const char *field, u32 key, const void *raw, size_t n) {
    if (!s_undo.cap) vec_init(&s_undo, sizeof(UndoEnt));
    for (int i = 0; i < s_undo.n; i++) {
        UndoEnt *e = VEC_AT(&s_undo, UndoEnt, i);
        if (e->key == key && str_ieq(e->table, table) && str_ieq(e->field, field)) return;
    }
    UndoEnt *e = VEC_ADD(&s_undo, UndoEnt);
    snprintf(e->table, sizeof(e->table), "%s", table);
    snprintf(e->field, sizeof(e->field), "%s", field);
    e->key = key;
    str_setn(&e->raw, (const char *)raw, (int)n);
    s_undo_dirty = 1;
}

void db_undo_flush(int force) {
    if (!s_undo_dirty || s_undo.n == 0) return;
    if (!force && now_ms() - s_undo_ms < 15000) return;
    s_undo_ms = now_ms();

    char dir[MAX_PATH], ts[32], path[MAX_PATH];
    art_dir("backup", dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    snprintf(path, sizeof(path), "%s\\undo-%s.sql", dir, ts);

    Str o = {0};
    char hdr[160];
    now_str(hdr, sizeof(hdr), "%Y-%m-%d %H:%M:%S");
    str_catf(&o,
             "-- DNF GM 写库前快照 · %s\n"
             "-- 本会话第一次写入前的原值，共 %d 处。可直接用「还原备份」整体回滚。\n"
             "SET NAMES utf8mb4;\n\n",
             hdr, s_undo.n);
    for (int i = 0; i < s_undo.n; i++) {
        UndoEnt *e = VEC_AT(&s_undo, UndoEnt, i);
        int by_mid = str_ieq(e->table, "taiwan_cain.account_cargo");
        const char *key_col = by_mid ? "m_id" : "charac_no";
        str_catf(&o, "-- %s.%s  %s=%u\nUPDATE %s SET %s=0x", e->table, e->field, key_col, e->key,
                 e->table, e->field);
        hex_str(e->raw.p, (size_t)e->raw.n, &o);
        str_catf(&o, " WHERE %s=%u;\n\n", key_col, e->key);
    }
    if (file_write(path, o.p, (size_t)o.n)) {
        s_undo_dirty = 0;
        art_prune(dir, "undo-", 5);
    }
    str_free(&o);
}

int db_save_blob(Db *d, const Charac *c, const char *table, const char *field,
                 const void *raw, size_t n, Err err) {
    const int by_mid = strcmp(table, "taiwan_cain.account_cargo") == 0;
    const char *key_col = by_mid ? "m_id" : "charac_no";
    u32 key = by_mid ? c->mid : c->no;

    // 1) 取原值（先记录到本会话快照，不落单文件）
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT %s FROM %s WHERE %s=%u", field, table, key_col, key);
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    if (r.nrow && my_text(&r, 0, 0))
        undo_note(table, field, key, my_text(&r, 0, 0), (size_t)my_len(&r, 0, 0));
    my_free(&r);

    // 2) 用十六进制字面量写入，绕开一切字符集/转义
    str_clear(&s);
    str_catf(&s, "UPDATE %s SET %s=0x", table, field);
    hex_str(raw, n, &s);
    str_catf(&s, " WHERE %s=%u", key_col, key);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) { snprintf(err, ERRBUF, "%s", d->err); return 0; }

    // 3) 只查长度做校验：整块回读一遍纯属浪费
    str_catf(&s, "SELECT LENGTH(%s) FROM %s WHERE %s=%u", field, table, key_col, key);
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    u64 got = my_u64(&r, 0, 0, 0);
    my_free(&r);
    str_free(&s);
    if (got != (u64)n) {
        snprintf(err, ERRBUF, "回读校验失败（写入 %llu 字节，库里 %llu）", (unsigned long long)n,
                 (unsigned long long)got);
        return 0;
    }
    db_undo_flush(0);
    return 1;
}

// ---------------- 基础信息 ----------------
int db_name_taken(Db *d, const char *name, u32 except_no) {
    int taken = 0;
    set_names(d, "latin1");
    char *e = mysql_escape(name);
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT charac_no FROM taiwan_cain.charac_info WHERE charac_name='%s'", e);
    free(e);
    if (select1(d, &s, &r)) {
        for (int i = 0; i < r.nrow; i++)
            if ((u32)my_u64(&r, i, 0, 0) != except_no) taken = 1;
    }
    my_free(&r);
    str_free(&s);
    set_names(d, "utf8");
    return taken;
}

int db_set_name(Db *d, const Charac *c, const char *name, Err err) {
    // 必须 latin1 连接写 UTF-8 字节流，否则中文名变乱码
    set_names(d, "latin1");
    char *e = mysql_escape(name);
    int ok = db_exec(d, "改角色名", "taiwan_cain.charac_info", "charac_no", c->no, err,
                     "UPDATE taiwan_cain.charac_info SET charac_name='%s' WHERE charac_no=%u",
                     e ? e : "", c->no);
    free(e);
    set_names(d, "utf8");
    return ok;
}

int db_set_job(Db *d, const Charac *c, int job, int grow, Err err) {
    return db_exec(d, "改职业/觉醒", "taiwan_cain.charac_info", "charac_no", c->no, err,
                   "UPDATE taiwan_cain.charac_info SET job=%d,grow_type=%d WHERE charac_no=%u",
                   job, grow, c->no);
}

// exp < 0 表示「不知道该写多少」，此时只动等级、不动经验，
// 免得 PVF 没加载时把人家的经验清零。
// 三句写在一个事务里：等级改了经验没改，是最难解释的一种中间态。
int db_set_lev(Db *d, const Charac *c, int lev, i64 exp, Err err) {
    if (!db_tx_begin(d, err)) return 0;
    if (!db_exec(d, "改等级", "taiwan_cain.charac_info", "charac_no", c->no, err,
                 "UPDATE taiwan_cain.charac_info SET lev=%d WHERE charac_no=%u", lev, c->no))
        goto fail;
    if (exp >= 0 && !db_exec(d, "改经验", "taiwan_cain.charac_stat", "charac_no", c->no, err,
                             "UPDATE taiwan_cain.charac_stat SET exp=%llu WHERE charac_no=%u",
                             (unsigned long long)exp, c->no))
        goto fail;
    // 惩罚记录可能本来就没有（DELETE 影响 0 行是正常的），所以不走 db_exec
    Str s = {0};
    str_catf(&s, "DELETE FROM d_taiwan.member_punish_info WHERE m_id=%u", c->mid);
    run(d, &s);
    str_free(&s);
    return db_tx_commit(d, err);
fail:
    db_tx_rollback(d);
    return 0;
}

int db_set_money(Db *d, const Charac *c, u64 money, Err err) {
    return db_exec(d, "改金币", "taiwan_cain_2nd.inventory", "charac_no", c->no, err,
                   "UPDATE taiwan_cain_2nd.inventory SET money=%llu WHERE charac_no=%u",
                   (unsigned long long)money, c->no);
}

int db_set_coin(Db *d, const Charac *c, u64 coin, Err err) {
    // 复活币在 inventory.coin（读也是这一列）。原来这里写的是 pay_coin —— 那是另一种
    // 计数，写完回读还是旧值，界面上就表现为"改了又弹回去"。
    return db_exec(d, "改复活币", "taiwan_cain_2nd.inventory", "charac_no", c->no, err,
                   "UPDATE taiwan_cain_2nd.inventory SET coin=%llu WHERE charac_no=%u",
                   (unsigned long long)coin, c->no);
}

int db_set_account_money(Db *d, const Charac *c, u64 money, Err err) {
    // 账号金库是懒建的：玩家没在游戏里打开过账号金库，account_cargo 里就没有这行，
    // 光 UPDATE 影响 0 行 —— 写过就弹回去，界面上永远显示 0。这里改成有行则改、
    // 无行则建（cargo/容量留默认，只写钱）。
    Str s = {0};
    str_catf(&s,
             "INSERT INTO taiwan_cain.account_cargo (m_id,money,occ_time) "
             "VALUES (%u,%llu,NOW()) ON DUPLICATE KEY UPDATE money=VALUES(money),occ_time=NOW()",
             c->mid, (unsigned long long)money);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "改账号金库：%s", d->err);
    return ok;
}

// 点券分两张表存，必须同生同死：扣了没加比不扣更糟
int db_set_cera(Db *d, const Charac *c, u64 cera, u64 point, Err err) {
    // cash_cera / cash_cera_point 也是懒建的（账号没充过点券就没有行），
    // 和 account_cargo / charac_quest_shop 一个毛病：光 UPDATE 会 0 行报失败。
    // 改成有行则改、无行则建。用事务包住两张表，要么都改要么都不改。
    if (!db_tx_begin(d, err)) return 0;
    Str s = {0};
    str_catf(&s,
             "INSERT INTO taiwan_billing.cash_cera (account,cera) VALUES (%u,%llu) "
             "ON DUPLICATE KEY UPDATE cera=VALUES(cera)",
             c->mid, (unsigned long long)cera);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) {
        snprintf(err, ERRBUF, "改点券：%s", d->err);
        db_tx_rollback(d);
        return 0;
    }
    str_catf(&s,
             "INSERT INTO taiwan_billing.cash_cera_point (account,cera_point) VALUES (%u,%llu) "
             "ON DUPLICATE KEY UPDATE cera_point=VALUES(cera_point)",
             c->mid, (unsigned long long)point);
    ok = run(d, &s);
    str_free(&s);
    if (!ok) {
        snprintf(err, ERRBUF, "改赠送点券：%s", d->err);
        db_tx_rollback(d);
        return 0;
    }
    return db_tx_commit(d, err);
}

// 泡点：给每个在线账号的赠送点券加 val。
// 一条 UPDATE ... JOIN 写完，要么全加要么一个不加，不需要事务。
// 不走 db_exec：那里把「命中 0 行」当失败，而这里没人上线是正常结果。
int db_bubble_tick(Db *d, int val, u32 *paid, Err err) {
    if (val <= 0) {
        if (paid) *paid = 0;
        return 1;
    }
    Str s = {0};
    u64 aff = 0;
    // account 是 varchar(30)。拿它跟 int 的 m_id 直接比会把列转成数字、索引失效；
    // 反过来把 m_id 转成字符，两边都是字符串，索引还在。
    str_catf(&s,
             "UPDATE taiwan_billing.cash_cera_point p"
             " JOIN (SELECT DISTINCT m_id FROM taiwan_login.login_account_3"
             " WHERE login_status>=1) l ON p.account=CAST(l.m_id AS CHAR)"
             " SET p.cera_point=p.cera_point+%d, p.mod_date=NOW()", val);
    int ok = run_aff(d, &s, &aff);
    if (!ok) snprintf(err, ERRBUF, "泡点结算：%s", d->err);
    else if (paid) *paid = (u32)aff;
    str_free(&s);
    return ok;
}

int db_set_delete_flag(Db *d, const Charac *c, int deleted, Err err) {
    return db_exec(d, deleted ? "标记删除" : "取消删除", "taiwan_cain.charac_info", "charac_no",
                   c->no, err,
                   "UPDATE taiwan_cain.charac_info SET delete_flag=%d WHERE charac_no=%u",
                   deleted ? 1 : 0, c->no);
}

// ---------------- GM 一键 ----------------
int db_set_sp(Db *d, const Charac *c, int sp, int sp2, int tp, int tp2, Err err) {
    return db_exec(d, "改 SP/TP", "taiwan_cain_2nd.skill", "charac_no", c->no, err,
                   "UPDATE taiwan_cain_2nd.skill SET remain_sp=%d,remain_sp_2nd=%d,"
                   "remain_sfp_1st=%d,remain_sfp_2nd=%d WHERE charac_no=%u",
                   sp, sp2, tp, tp2, c->no);
}

int db_set_qp(Db *d, const Charac *c, int qp, Err err) {
    // 同账号金库：charac_quest_shop 也是懒建的（角色没碰过 QP 商店就没有这行），
    // 只 UPDATE 会 0 行。改成有行则改、无行则建，其余加成列走表默认值 0。
    Str s = {0};
    str_catf(&s,
             "INSERT INTO taiwan_cain.charac_quest_shop (charac_no,qp) VALUES (%u,%d) "
             "ON DUPLICATE KEY UPDATE qp=VALUES(qp)",
             c->no, qp);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "改 QP：%s", d->err);
    return ok;
}

int db_set_pvp(Db *d, const Charac *c, int grade, int win, int point, Err err) {
    return db_exec(d, "改 PVP", "taiwan_cain.pvp_result", "charac_no", c->no, err,
                   "UPDATE taiwan_cain.pvp_result SET pvp_grade=%d,win=%d,pvp_point=%d "
                   "WHERE charac_no=%u", grade, win, point, c->no);
}

int db_no_weight(Db *d, const Charac *c, Err err) {
    return db_exec(d, "取消负重", "taiwan_cain.charac_info", "charac_no", c->no, err,
                   "UPDATE taiwan_cain.charac_info SET inven_weight=99999999 WHERE charac_no=%u",
                   c->no);
}

int db_open_equip_slot(Db *d, const Charac *c, Err err) {
    return db_exec(d, "开装备槽", "taiwan_cain.charac_stat", "charac_no", c->no, err,
                   "UPDATE taiwan_cain.charac_stat SET add_slot_flag=3 WHERE charac_no=%u",
                   c->no);
}

// 先删后插，中途断掉会连原来的上限也一起没了，所以包事务
int db_unlock_lev_equip(Db *d, const Charac *c, Err err) {
    if (!db_tx_begin(d, err)) return 0;
    Str s = {0};
    str_catf(&s, "DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=%u", c->no);
    run(d, &s);
    str_clear(&s);
    str_catf(&s, "INSERT INTO taiwan_cain.charac_manage_info (charac_no,max_equip_level) "
                 "VALUES (%u,999)", c->no);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) {
        snprintf(err, ERRBUF, "解锁装备等级：%s", d->err);
        db_tx_rollback(d);
        return 0;
    }
    return db_tx_commit(d, err);
}

int db_max_expert(Db *d, const Charac *c, Err err) {
    return db_exec(d, "满专家", "taiwan_cain.charac_stat", "charac_no", c->no, err,
                   "UPDATE taiwan_cain.charac_stat SET expert_job_exp=2054 WHERE charac_no=%u",
                   c->no);
}

// 补满疲劳：当前疲劳拉到上限，同时把已消耗计数清零（两表同生同死）
int db_set_fatigue(Db *d, const Charac *c, Err err) {
    if (!db_tx_begin(d, err)) return 0;
    if (!db_exec(d, "补满疲劳", "taiwan_cain.charac_info", "charac_no", c->no, err,
                 "UPDATE taiwan_cain.charac_info SET fatigue=max_fatigue WHERE charac_no=%u",
                 c->no))
        goto fail;
    if (!db_exec(d, "清疲劳消耗", "taiwan_cain.charac_stat", "charac_no", c->no, err,
                 "UPDATE taiwan_cain.charac_stat SET used_fatigue=0 WHERE charac_no=%u", c->no))
        goto fail;
    return db_tx_commit(d, err);
fail:
    db_tx_rollback(d);
    return 0;
}

// member_dungeon.dungeon 是「副本ID|难度」的逗号串，难度 3 为最高。
// 写满即全图全难度开启（原来这里写空串，等于把人家的图全锁回去）。
static const char *kAllDungeon =
    "1|3,2|3,3|3,4|3,5|3,6|3,7|3,8|3,9|3,11|3,12|3,13|3,14|3,15|3,16|1,17|3,21|3,22|3,23|3,"
    "24|3,25|3,26|3,27|3,31|3,32|3,33|3,34|3,35|3,36|3,37|3,40|3,41|2,42|3,43|3,44|3,45|3,"
    "50|3,51|3,52|3,53|3,60|3,61|3,62|2,63|3,64|3,65|3,67|3,70|3,71|3,72|3,73|3,74|3,75|3,"
    "76|3,77|3,80|3,81|3,82|3,83|3,84|3,85|3,86|3,87|3,88|3,89|3,90|3,91|2,92|3,93|3,100|3,"
    "101|3,102|3,103|3,104|3,110|3,111|3,112|3,140|3,141|3,502|3,511|3,515|1,518|1,521|3,"
    "1000|3,1500|3,1501|3,1502|3,1507|1,3506|3,10000|3";

// upsert 影响 0 行也是正常（值没变），所以不用 db_exec 的 0 行判定
int db_unlock_dungeon(Db *d, const Charac *c, Err err) {
    Str s = {0};
    str_catf(&s,
             "INSERT INTO taiwan_cain.member_dungeon (m_id,dungeon) VALUES (%u,'%s') "
             "ON DUPLICATE KEY UPDATE dungeon=VALUES(dungeon)",
             c->mid, kAllDungeon);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "解锁全图：%s", d->err);
    return ok;
}

int db_reset_punish(Db *d, u32 mid, Err err) {
    Str s = {0};
    str_catf(&s, "DELETE FROM d_taiwan.member_punish_info WHERE m_id=%u", mid);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "清惩罚：%s", d->err);
    return ok;
}

// ---------------- 邮件 ----------------
int db_list_mails(Db *d, const Charac *c, Vec *out, Err err) {
    Str s = {0};
    str_catf(&s,
             "SELECT postal_id,item_id,avata_flag,creature_flag,add_info,gold,occ_time "
             "FROM taiwan_cain_2nd.postal WHERE receive_charac_no=%u AND delete_flag=0 "
             "ORDER BY postal_id DESC", c->no);
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        MailRow *m = VEC_ADD(out, MailRow);
        m->postal_id = my_u64(&r, i, 0, 0);
        m->item_id = (u32)my_u64(&r, i, 1, 0);
        m->avata = (int)my_i64(&r, i, 2, 0);
        m->creature = (int)my_i64(&r, i, 3, 0);
        m->add_info = (u32)my_u64(&r, i, 4, 0);
        m->gold = my_u64(&r, i, 5, 0);
        cp_text(m->time, sizeof(m->time), &r, i, 6);
    }
    my_free(&r);
    return 1;
}

// 邮件本身只是软删（delete_flag=1）；但它附带的时装/宠物是真实行，
// 不跟着清掉就成了占格的孤儿记录。add_info 就是那行的 ui_id。
static void mail_drop_attach(Db *d, u64 postal_id) {
    Str s = {0};
    MyResult r = {0};
    str_catf(&s,
             "SELECT avata_flag,creature_flag,add_info FROM taiwan_cain_2nd.postal "
             "WHERE postal_id=%llu",
             (unsigned long long)postal_id);
    if (!select1(d, &s, &r) || !r.nrow) { str_free(&s); return; }
    int av = (int)my_i64(&r, 0, 0, 0);
    int cr = (int)my_i64(&r, 0, 1, 0);
    u64 add = my_u64(&r, 0, 2, 0);
    my_free(&r);
    if ((!av && !cr) || !add) { str_free(&s); return; }
    str_clear(&s);
    str_catf(&s, "DELETE FROM taiwan_cain_2nd.%s WHERE ui_id=%llu", av ? "user_items" : "creature_items",
             (unsigned long long)add);
    run(d, &s);
    str_free(&s);
}

int db_delete_mail(Db *d, u64 postal_id, Err err) {
    mail_drop_attach(d, postal_id);
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.postal SET delete_flag=1 WHERE postal_id=%llu",
             (unsigned long long)postal_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_delete_all_mails(Db *d, const Charac *c, Err err) {
    Str s = {0};
    MyResult r = {0};
    // 先把所有带附件的邮件 id 收出来逐个清，再统一软删
    str_catf(&s,
             "SELECT postal_id FROM taiwan_cain_2nd.postal WHERE receive_charac_no=%u "
             "AND delete_flag=0 AND (avata_flag=1 OR creature_flag=1)",
             c->no);
    if (select1(d, &s, &r)) {
        for (int i = 0; i < r.nrow; i++) mail_drop_attach(d, my_u64(&r, i, 0, 0));
        my_free(&r);
    }
    str_clear(&s);
    str_catf(&s, "UPDATE taiwan_cain_2nd.postal SET delete_flag=1 WHERE receive_charac_no=%u", c->no);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_send_text_mail(Db *d, const Charac *c, const char *sender, const char *text, Err err) {
    set_names(d, "latin1");
    char *es = mysql_escape(sender ? sender : "GM");
    char *et = mysql_escape(text ? text : "");
    Str s = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&s,
             "INSERT INTO taiwan_cain_2nd.letter "
             "(charac_no,send_charac_no,send_charac_name,letter_text,reg_date,stat) "
             "VALUES (%u,0,'%s','%s','%s',1)",
             c->no, es, et, ts);
    free(es);
    free(et);
    int ok = run(d, &s);
    str_free(&s);
    set_names(d, "utf8");
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_send_item_mail(Db *d, const Charac *c, const char *sender, const char *text,
                      u32 item_id, u32 num, Err err) {
    GrantItem g;
    memset(&g, 0, sizeof(g));
    g.id = item_id;
    g.num = num;
    g.type = IT_STACK;
    return db_send_item_mail_ex(d, c, sender, text, &g, err);
}

int db_send_item_mail_ex(Db *d, const Charac *c, const char *sender, const char *text,
                         const GrantItem *g, Err err) {
    if (!db_send_text_mail(d, c, sender, text, err)) return 0;
    set_names(d, "latin1");
    char *es = mysql_escape(sender ? sender : "GM");
    Str s = {0};
    MyResult r = {0};
    u64 letter_id = 0;
    str_cat(&s, "SELECT LAST_INSERT_ID()");
    if (select1(d, &s, &r)) letter_id = my_u64(&r, 0, 0, 0);
    my_free(&r);
    if (!letter_id) {
        // 回退：按 charac_no 找最新一封
        str_clear(&s);
        str_catf(&s, "SELECT MAX(letter_id) FROM taiwan_cain_2nd.letter WHERE charac_no=%u", c->no);
        if (select1(d, &s, &r)) letter_id = my_u64(&r, 0, 0, 0);
        my_free(&r);
    }
    str_clear(&s);
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&s,
             "INSERT INTO taiwan_cain_2nd.postal "
             "(occ_time,send_charac_no,send_charac_name,receive_charac_no,amplify_option,amplify_value,"
             "seperate_upgrade,seal_flag,item_id,add_info,upgrade,gold,letter_id,avata_flag,creature_flag,"
             "endurance,unlimit_flag) "
             "VALUES ('%s',0,'%s',%u,%u,%u,%u,%u,%u,%u,%u,0,%llu,0,0,%u,1)",
             ts, es, c->no, g->inc_type, g->inc_val, g->forge, g->seal, g->id, g->num, g->enhance,
             (unsigned long long)letter_id, g->dur);
    free(es);
    int ok = run(d, &s);
    str_free(&s);
    set_names(d, "utf8");
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// ---------------- 宠物 / 时装 ----------------
int db_list_pets(Db *d, const Charac *c, Vec *out, Err err) {
    Str s = {0};
    str_catf(&s,
             "SELECT ui_id,it_id,creature_type,stomach,name,expire_date "
             "FROM taiwan_cain_2nd.creature_items WHERE charac_no=%u ORDER BY ui_id", c->no);
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        PetRow *p = VEC_ADD(out, PetRow);
        p->ui_id = (u32)my_u64(&r, i, 0, 0);
        p->it_id = (u32)my_u64(&r, i, 1, 0);
        p->type = (int)my_i64(&r, i, 2, 0);
        p->stomach = (int)my_i64(&r, i, 3, 0);
        cp_text(p->name, sizeof(p->name), &r, i, 4);
        cp_text(p->expire, sizeof(p->expire), &r, i, 5);
    }
    my_free(&r);
    return 1;
}

int db_set_pet_name(Db *d, u32 ui_id, const char *name, Err err) {
    set_names(d, "latin1");
    char *e = mysql_escape(name);
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.creature_items SET name='%s' WHERE ui_id=%u", e, ui_id);
    free(e);
    int ok = run(d, &s);
    str_free(&s);
    set_names(d, "utf8");
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_set_pet_full(Db *d, u32 ui_id, Err err) {
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.creature_items SET stomach=100 WHERE ui_id=%u", ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_set_pet_expire(Db *d, u32 ui_id, Err err) {
    Str s = {0};
    str_catf(&s,
             "UPDATE taiwan_cain_2nd.creature_items SET expire_date='9999-12-31 23:59:59' WHERE ui_id=%u",
             ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_delete_pet(Db *d, u32 ui_id, Err err) {
    Str s = {0};
    str_catf(&s, "DELETE FROM taiwan_cain_2nd.creature_items WHERE ui_id=%u", ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_list_avatars(Db *d, const Charac *c, Vec *out, Err err) {
    Str s = {0};
    str_catf(&s,
             "SELECT ui_id,it_id,hidden_option,expire_date FROM taiwan_cain_2nd.user_items "
             "WHERE charac_no=%u ORDER BY ui_id", c->no);
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        AvatarRow *a = VEC_ADD(out, AvatarRow);
        a->ui_id = (u32)my_u64(&r, i, 0, 0);
        a->it_id = (u32)my_u64(&r, i, 1, 0);
        a->hidden = (int)my_i64(&r, i, 2, 0);
        cp_text(a->expire, sizeof(a->expire), &r, i, 3);
    }
    my_free(&r);
    return 1;
}

int db_set_avatar_hidden(Db *d, u32 ui_id, int hidden, Err err) {
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.user_items SET hidden_option=%d WHERE ui_id=%u", hidden, ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_set_avatar_expire(Db *d, u32 ui_id, Err err) {
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.user_items SET expire_date='9999-12-31 23:59:59' WHERE ui_id=%u", ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

int db_delete_avatar(Db *d, u32 ui_id, Err err) {
    Str s = {0};
    str_catf(&s, "DELETE FROM taiwan_cain_2nd.user_items WHERE ui_id=%u", ui_id);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// ---------------- 任务 ----------------
int db_list_quests(Db *d, const Charac *c, Vec *out, Err err) {
    Str s = {0};
    str_catf(&s, "SELECT * FROM taiwan_cain.new_charac_quest WHERE charac_no=%u", c->no);
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    vec_clear(out);
    // 列顺序：charac_no, clear_quest, quest_notify, play_1, play_1_trigger, play_2, ...
    for (int i = 3; i + 1 < r.ncol; i += 2) {
        u32 id = (u32)my_u64(&r, 0, i, 0);
        if (!id) continue;
        QuestRow *q = VEC_ADD(out, QuestRow);
        q->slot = (i - 3) / 2 + 1;
        q->id = id;
        q->trigger = (u32)my_u64(&r, 0, i + 1, 0);
    }
    my_free(&r);
    return 1;
}

// 改单个任务槽：slot 从 1 开始。id=0 即放弃该任务。
int db_set_quest_slot(Db *d, const Charac *c, int slot, u32 id, u32 trigger, Err err) {
    if (slot < 1 || slot > 20) { snprintf(err, ERRBUF, "槽位超出范围"); return 0; }
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain.new_charac_quest SET play_%d=%u,play_%d_trigger=%u WHERE charac_no=%u",
             slot, id, slot, trigger, c->no);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

// 放弃任务后，背包里为它攒的任务材料就成了垃圾。
// 只清 type=4（任务材料）且编号命中的格子，普通材料一律不动。
int db_purge_quest_items(Db *d, const Charac *c, const u32 *ids, int n, int *removed, Err err) {
    if (removed) *removed = 0;
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT inventory FROM taiwan_cain_2nd.inventory WHERE charac_no=%u", c->no);
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    if (!r.nrow) { my_free(&r); snprintf(err, ERRBUF, "该角色没有背包记录"); return 0; }

    Blob b = {0};
    Err e = {0};
    if (!blob_unpack(my_text(&r, 0, 0), (size_t)my_len(&r, 0, 0), &b, e)) {
        snprintf(err, ERRBUF, "物品栏: %s", e);
        my_free(&r);
        return 0;
    }
    my_free(&r);
    if (!b.ok) { snprintf(err, ERRBUF, "物品栏: %s", b.err); blob_free(&b); return 0; }

    int hit = 0;
    for (int i = 0; i < b.n; i++) {
        Item *it = &b.items[i];
        if (item_empty(it) || item_type(it) != IT_QUEST) continue;
        u32 id = item_id(it);
        for (int k = 0; k < n; k++) {
            if (ids[k] != id) continue;
            memset(it->raw, 0, ITEM_SIZE);
            hit++;
            break;
        }
    }
    if (!hit) { blob_free(&b); return 1; }        // 没有可清的，不动数据库

    Buf out = {0};
    if (!blob_pack(&b, &out)) { snprintf(err, ERRBUF, "打包失败"); blob_free(&b); return 0; }
    blob_free(&b);
    int ok = db_save_blob(d, c, "taiwan_cain_2nd.inventory", "inventory", out.p, out.n, err);
    buf_free(&out);
    if (ok && removed) *removed = hit;
    return ok;
}

// 放弃全部进行中的任务：把 20 个槽位清零而不是删行——
// 服务器期望这行存在，删掉可能让角色再也接不了任务。
int db_clear_quests(Db *d, const Charac *c, Err err) {
    Str s = {0};
    str_cat(&s, "UPDATE taiwan_cain.new_charac_quest SET ");
    for (int i = 1; i <= 20; i++)
        str_catf(&s, "%splay_%d=0,play_%d_trigger=0", i > 1 ? "," : "", i, i);
    str_catf(&s, " WHERE charac_no=%u", c->no);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok;
}

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

int db_add_event(Db *d, u32 type, i64 p1, i64 p2, Err err) {
    Str s = {0};
    str_catf(&s,
             "INSERT INTO d_taiwan.dnf_event_log "
             "(occ_time,event_type,parameter1,parameter2,server_id,event_flag,start_time,end_time) "
             "VALUES (0,%u,%lld,%lld,0,0,0,0)", type, (long long)p1, (long long)p2);
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
            "a.accountname FROM d_taiwan.member_punish_info p "
            "LEFT JOIN d_taiwan.accounts a ON a.UID=p.m_id "
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
        cp_text(b->start, sizeof(b->start), &r, i, 3);
        cp_text(b->end, sizeof(b->end), &r, i, 4);
        cp_text(b->reason, sizeof(b->reason), &r, i, 5);
        cp_text(b->account, sizeof(b->account), &r, i, 6);
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

int db_unban(Db *d, u32 mid, Err err) { return db_reset_punish(d, mid, err); }

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
static int is_bin_type(const char *t) {
    return str_istr(t, "blob") || str_istr(t, "binary") || strcmp(t, "geometry") == 0;
}

static int is_num_type(const char *t) {
    static const char *k[] = {"tinyint", "smallint", "mediumint", "int", "bigint",
                              "decimal", "float", "double", "bit"};
    for (int i = 0; i < COUNT_OF(k); i++)
        if (!strcmp(t, k[i])) return 1;
    return 0;
}

// 写一个 SQL 字面量；binary 走 X'..'，文本逐字节转义（保证不含裸换行）
static void sql_lit(Str *o, const char *p, int n, int binary) {
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

static int split_tbl(const char *full, char *sc, int sccap, char *tb, int tbcap) {
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
