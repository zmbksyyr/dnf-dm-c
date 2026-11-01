// db 实现。// port of dnfpkgtool/sqlManager2.py
#include "db_priv.h"
#include "db.h"

// ---------------- 基础设施 ----------------
// 影响行数顺手带出来：写操作靠它区分「改了」和「一条都没碰到」。
// MyResult 里本来就有 affected，原来 run() 直接扔了。
int run_aff(Db *d, Str *sql, u64 *aff) {
    MyResult r = {0};
    int ok = mysql_query(&d->conn, str_c(sql), &r);
    if (!ok) snprintf(d->err, ERRBUF, "%s", d->conn.err);
    else if (aff) *aff = r.affected;
    my_free(&r);
    return ok;
}

int run(Db *d, Str *sql) { return run_aff(d, sql, NULL); }
int select1(Db *d, Str *sql, MyResult *r);   // 实现见下

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

int select1(Db *d, Str *sql, MyResult *r) {
    if (!mysql_query(&d->conn, str_c(sql), r)) {
        snprintf(d->err, ERRBUF, "%s", d->conn.err);
        return 0;
    }
    return 1;
}

int set_names(Db *d, const char *cs) {
    Str s = {0};
    str_catf(&s, "SET NAMES %s", cs);
    int ok = run(d, &s);
    str_free(&s);
    return ok;
}

// 连接字符集编号 → SET NAMES 用的名字。33 = utf8（也就是 utf8mb3）、45 = utf8mb4、
// 8 = latin1；认不出的按 utf8 处理。
//
// 备份头必须由它生成，不能图省事写死 utf8mb4：还原时文件里的 SET NAMES 会被逐条
// 执行，声明成一个跟连接不符的字符集，等于把连接偷偷切走 —— 之后所有转义都对不上
// 号，写进去的东西静默变乱码。
const char *db_cs_name(int charset) {
    switch (charset) {
    case 8:  return "latin1";
    case 45: return "utf8mb4";
    default: return "utf8";
    }
}

void cp_str(char *dst, int cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, (size_t)cap, "%s", src);
}

static void reset_tables(void);   // 表存在性缓存（实现见下）
static int  have_table(Db *d, const char *full);

// 把 column 的 UTF-8 文本拷进定长缓冲
void cp_text(char *dst, int cap, const MyResult *r, int row, int col) {
    const char *s = my_text(r, row, col);
    if (!s) { dst[0] = 0; return; }
    snprintf(dst, (size_t)cap, "%s", s);
}

// 连接字符集。db_open 时记下，写备份头时要用 —— 头里声明的必须跟连接实际用的
// 一致，否则还原时文件里的 SET NAMES 会把连接切到另一个字符集上去。
static int s_cs = 45;

int db_open(Db *d, const char *host, int port, const char *user, const char *pwd, int charset, Err err) {
    memset(d, 0, sizeof(*d));
    s_cs = charset;
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
        // 前导通配 LIKE 全表扫：角色表量小可接受；大表改前缀匹配/全文索引
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
        // 前导通配 LIKE 全表扫：角色表量小可接受；大表改前缀匹配/全文索引
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

// 封停页的「加载全服 / 加载在线」：和 db_poll_chars 用同一套在线判定
// （账号在线 且 正在玩该角色；拿不到当前角色映射时退回账号级），
// 只是这里不需要分页，但要能在 SQL 之外按 online_only 过滤。
int db_list_ban_chars(Db *d, Vec *out, int online_only, int limit, Err err) {
    static const char *kCurTables[] = {"taiwan_game_event.event_1306_account_reward",
                                       "taiwan_siroco.member_play_info"};
    const char *cur_tab = NULL;
    for (int i = 0; i < COUNT_OF(kCurTables) && !cur_tab; i++)
        if (have_table(d, kCurTables[i])) cur_tab = kCurTables[i];
    int have_login = have_table(d, "taiwan_login.login_account_3");

    Str s = {0};
    str_cat(&s, "SELECT c.charac_no,c.charac_name,c.job,c.grow_type,c.lev,c.m_id,a.accountname,");
    if (have_login)
        str_cat(&s,
                "(SELECT MAX(l.login_status) FROM taiwan_login.login_account_3 l WHERE l.m_id=c.m_id),");
    else
        str_cat(&s, "NULL,");
    if (cur_tab)
        str_catf(&s, "(SELECT MAX(1) FROM %s t WHERE t.m_id=c.m_id AND t.charac_no=c.charac_no)",
                 cur_tab);
    else
        str_cat(&s, "NULL");
    str_cat(&s, " FROM taiwan_cain.charac_info c"
                " LEFT JOIN d_taiwan.accounts a ON a.UID=c.m_id"
                " WHERE c.delete_flag=0 ORDER BY c.charac_no DESC");
    if (limit > 0) str_catf(&s, " LIMIT %d", limit);

    MyResult r = {0};
    if (!select1(d, &s, &r)) {
        snprintf(err, ERRBUF, "%s", d->err);
        str_free(&s);
        return 0;
    }
    str_free(&s);

    vec_clear(out);
    for (int i = 0; i < r.nrow; i++) {
        int acct = (int)my_i64(&r, i, 7, 0);
        int playing = cur_tab ? (int)my_i64(&r, i, 8, 0) : 1;
        int online = acct == 1 && playing == 1;
        if (online_only && !online) continue;       // 过滤在填的时候做，SQL 少一层子查询
        Charac *c = VEC_ADD(out, Charac);
        c->no = (u32)my_u64(&r, i, 0, 0);
        cp_text(c->name, sizeof(c->name), &r, i, 1);
        c->job = (int)my_i64(&r, i, 2, 0);
        c->grow = (int)my_i64(&r, i, 3, 0);
        c->lev = (int)my_i64(&r, i, 4, 0);
        c->mid = (u32)my_u64(&r, i, 5, 0);
        cp_text(c->account, sizeof(c->account), &r, i, 6);
        c->online = online;
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

    // VIP 在另一个库 d_taiwan.accounts：单独一条查询带回来，不进上面的 JOIN。
    // 失败不让整次加载挂掉——VIP 只是个标记字段，因为它读不出来就让整个角色页打不开不值得，
    // 所以错误留在自己的缓冲里，不往上传。
    {
        Err e = {0};
        db_get_vip(d, c->mid, out->vip, sizeof(out->vip), e);
    }

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
    // VIP 同上：单独带回来（账号级，按文本处理），读失败维持原值
    {
        Err e = {0};
        db_get_vip(d, c->mid, out->vip, sizeof(out->vip), e);
    }
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
#define UNDO_MAX_ENT  512           // 条目上限：超出后不再记（undo 文件不完整但会话不爆内存）
#define UNDO_MAX_RAW  (256 * 1024)  // 单条原值上限：超大 blob 只记一条提示

static void undo_note(const char *table, const char *field, u32 key, const void *raw, size_t n) {
    if (!s_undo.cap) vec_init(&s_undo, sizeof(UndoEnt));
    for (int i = 0; i < s_undo.n; i++) {
        UndoEnt *e = VEC_AT(&s_undo, UndoEnt, i);
        if (e->key == key && str_ieq(e->table, table) && str_ieq(e->field, field)) return;
    }
    if (s_undo.n >= UNDO_MAX_ENT) return;       // 条目满了，不再记
    UndoEnt *e = VEC_ADD(&s_undo, UndoEnt);
    snprintf(e->table, sizeof(e->table), "%s", table);
    snprintf(e->field, sizeof(e->field), "%s", field);
    e->key = key;
    if (n > UNDO_MAX_RAW) {
        // 超大 blob（背包 hex 可达数十 KB）：只记提示，不占内存
        str_set(&e->raw, "-- 原值过大，未记录（>256KB），无法自动还原此条");
    } else {
        str_setn(&e->raw, (const char *)raw, (int)n);
    }
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
             "SET NAMES %s;\n\n",
             hdr, s_undo.n, db_cs_name(s_cs));
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
    // charac_stat.exp 是 int(11)：PVF 里 94 级往上的经验就超过 2^31 了，
    // 原样写会被非严格模式静默截成 2147483647、严格模式直接报错。
    // 夹到列上限，至少写进去的是个确定值。
    if (exp > 2147483647LL) exp = 2147483647LL;
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

// 泡点结算：按目标人群 + 多奖励（赠送点券 / D点 / D币 / 物品）给账号或角色加点。
// 每条奖励是一条 UPDATE ... JOIN（或 INSERT ... SELECT），要么全加要么不加。
// 不走 db_exec：那里把「命中 0 行」当失败，而这里没人符合条件本是正常结果。
int db_bubble_tick(Db *d, const BubbleCfg *cfg, u32 *paid, Err err) {
    if (paid) *paid = 0;
    if (cfg->coin <= 0 && cfg->dpoint <= 0 && cfg->dcoin <= 0 && cfg->item == 0) {
        return 1;   // 没设任何奖励
    }
    // 目标集合子查询：chars = 角色号集合，mids = 账号 m_id 集合（账号级奖励用）
    Str chs = {0}, mids = {0};
    if (cfg->target == 0) {          // 仅在线
        str_cat(&chs, "SELECT charac_no FROM taiwan_cain.charac_info WHERE m_id IN "
                      "(SELECT m_id FROM taiwan_login.login_account_3 WHERE login_status>=1)");
        str_cat(&mids, "SELECT DISTINCT m_id FROM taiwan_login.login_account_3 WHERE login_status>=1");
    } else if (cfg->target == 1) {   // 全服（含离线）
        str_cat(&chs, "SELECT charac_no FROM taiwan_cain.charac_info WHERE delete_flag=0");
        str_cat(&mids, "SELECT DISTINCT m_id FROM taiwan_cain.charac_info WHERE delete_flag=0");
    } else if (cfg->target == 2) {   // 指定角色
        str_catf(&chs, "SELECT charac_no FROM taiwan_cain.charac_info WHERE charac_no=%u", cfg->tno);
        str_catf(&mids, "SELECT m_id FROM taiwan_cain.charac_info WHERE charac_no=%u", cfg->tno);
    } else {                         // 指定账号
        str_catf(&chs, "SELECT charac_no FROM taiwan_cain.charac_info WHERE m_id=%u", cfg->tno);
        str_catf(&mids, "SELECT %u AS m_id", cfg->tno);
    }

    int ok = 1;
    u64 aff = 0, paid_rows = 0;
    // 赠送点券（角色级，inventory.coin）
    if (cfg->coin > 0) {
        Str s = {0};
        str_catf(&s, "UPDATE taiwan_cain_2nd.inventory i JOIN (%s) t ON i.charac_no=t.charac_no "
                     "SET i.coin=i.coin+%d", chs.p, cfg->coin);
        if (!run_aff(d, &s, &aff)) { ok = 0; snprintf(err, ERRBUF, "泡点·点券：%s", d->err); }
        else paid_rows = aff;
        str_free(&s);
    }
    // D点（账号级，cash_cera.cera）
    if (cfg->dpoint > 0) {
        Str s = {0};
        str_catf(&s, "UPDATE taiwan_billing.cash_cera c JOIN (%s) t ON c.account=CAST(t.m_id AS CHAR) "
                     "SET c.cera=c.cera+%d", mids.p, cfg->dpoint);
        if (!run_aff(d, &s, &aff)) { ok = 0; snprintf(err, ERRBUF, "泡点·D点：%s", d->err); }
        else if (paid_rows == 0) paid_rows = aff;
        str_free(&s);
    }
    // D币（账号级，cash_cera_point.cera_point）
    if (cfg->dcoin > 0) {
        Str s = {0};
        str_catf(&s, "UPDATE taiwan_billing.cash_cera_point p JOIN (%s) t ON p.account=CAST(t.m_id AS CHAR) "
                     "SET p.cera_point=p.cera_point+%d", mids.p, cfg->dcoin);
        if (!run_aff(d, &s, &aff)) { ok = 0; snprintf(err, ERRBUF, "泡点·D币：%s", d->err); }
        else if (paid_rows == 0) paid_rows = aff;
        str_free(&s);
    }
    // 物品：逐收件人插一封（全服目标跳过，避免海量插入卡死）
    if (cfg->item != 0 && cfg->itemnum > 0 && cfg->target != 1) {
        int n = cfg->itemnum > 0 ? cfg->itemnum : 1;
        if (n > 200) n = 200;
        for (int k = 0; k < n; k++) {
            Str s = {0};
            str_catf(&s, "INSERT INTO taiwan_cain_2nd.postal "
                         "(occ_time,send_charac_name,receive_charac_no,item_id,gold,letter_id,"
                         "avata_flag,creature_flag,unlimit_flag) "
                         "SELECT NOW(),'泡点',charac_no,%u,0,0,0,0,1 FROM (%s) t",
                         cfg->item, chs.p);
            if (!run_aff(d, &s, &aff)) { ok = 0; snprintf(err, ERRBUF, "泡点·物品：%s", d->err); }
            else if (paid_rows == 0) paid_rows = aff;
            str_free(&s);
        }
    }
    str_free(&chs); str_free(&mids);
    if (paid) *paid = (u32)paid_rows;
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
//
// lev_max 传等级的封顶值（settings.ini [gm] lev_max，默认 85），不是 999：
// max_equip_level 是「能穿多少级的装备」，服务端拿它跟角色等级比。写成 999
// 等于告诉服务端这号人物允许穿 999 级装备，在 85 封顶的端上就是个越界值 ——
// 要么判非法，要么让等级校验彻底失效。解除限制到「端允许的最高」就够了。
int db_unlock_lev_equip(Db *d, const Charac *c, int lev_max, Err err) {
    int lv = lev_max;
    if (lv < 1 || lv > 255) lv = 85;          // 调用方没传就退回默认，别把 0 写进去
    if (!db_tx_begin(d, err)) return 0;
    Str s = {0};
    str_catf(&s, "DELETE FROM taiwan_cain.charac_manage_info WHERE charac_no=%u", c->no);
    run(d, &s);
    str_clear(&s);
    str_catf(&s, "INSERT INTO taiwan_cain.charac_manage_info (charac_no,max_equip_level) "
                 "VALUES (%u,%d)", c->no, lv);
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
