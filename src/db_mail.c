// db_mail.c：邮件 / 宠物 / 时装 / 任务表操作。从 db.c 拆出，行为不变。
#include "db_priv.h"
#include "zlib.h"
#include "db.h"
#include "util.h"
#include "mysql.h"

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

// 宠物编号清单：dnf_creature_info 是「it_id ↔ creature_id」的对照表，
// 也就是「哪些编号是宠物」的唯一权威来源 —— PVF 的 creature/ 目录按键值是
// creature_id（佛拉斯=1），跟道具编号根本不在一个空间里，照它判会把编号 1
// （复活币）当成宠物。所以这里按 it_id 收一份清单给界面用。
// 有的私服没有这两个 web 库，查不到就当清单为空，不影响其它功能。
int db_creature_ids(Db *d, Vec *out, Err err) {
    vec_clear(out);
    static const char *kd[] = {"taiwan_cain_web.dnf_creature_info",
                               "taiwan_main_web.dnf_creature_info"};
    for (int i = 0; i < 2; i++) {
        Str s = {0};
        MyResult r = {0};
        str_catf(&s, "SELECT it_id FROM %s WHERE it_id>0", kd[i]);
        int ok = select1(d, &s, &r);
        str_free(&s);
        if (!ok) { err[0] = 0; d->err[0] = 0; continue; }
        for (int k = 0; k < r.nrow; k++) {
            u32 id = (u32)my_u64(&r, k, 0, 0);
            if (id) *VEC_ADD(out, u32) = id;
        }
        my_free(&r);
        return out->n;
    }
    return out->n;
}

// 邮件本身只是软删（delete_flag=1）；但它附带的时装/宠物是真实行，
// 不跟着清掉就成了占格的孤儿记录。add_info 就是那行的 ui_id。
static int mail_drop_where(Db *d, const char *where, Err err) {
    Str s = {0};
    MyResult r = {0};
    // SQL deletion cannot revoke an attachment already cached by the game.
    // Lock the recipient's live login row together with the selected mails;
    // this check belongs in the transaction, including bulk/attachment cleanup.
    str_catf(&s, "SELECT p.postal_id,COALESCE(l.login_status,0) "
                 "FROM taiwan_cain_2nd.postal p "
                 "LEFT JOIN taiwan_cain.charac_info c ON c.charac_no=p.receive_charac_no "
                 "LEFT JOIN taiwan_login.login_account_3 l ON l.m_id=c.m_id "
                 "WHERE p.delete_flag=0 AND %s FOR UPDATE", where);
    int ok = select1(d, &s, &r);
    for (int i = 0; ok && i < r.nrow; i++) {
        if (my_i64(&r, i, 1, 1) != 0) {
            snprintf(err, ERRBUF, "邮件所属账号在线或正在退出，未删除；请退出游戏后重试，避免游戏缓存继续领取附件");
            ok = 0;
        }
    }
    my_free(&r);
    static const char *tables[] = {"user_items", "creature_items"};
    static const char *flags[] = {"avata_flag", "creature_flag"};
    for (int i = 0; ok && i < 2; i++) {
        if (db_cancelled()) { snprintf(err, ERRBUF, "已取消"); ok = 0; break; }
        str_clear(&s);
        // Check ownership and item ID as well: a stale/malformed mail must not
        // delete another character's item merely because add_info matches.
        str_catf(&s, "DELETE taiwan_cain_2nd.%s FROM taiwan_cain_2nd.%s "
                     "JOIN taiwan_cain_2nd.postal p ON taiwan_cain_2nd.%s.ui_id=p.add_info "
                     "AND taiwan_cain_2nd.%s.charac_no=p.receive_charac_no "
                     "AND taiwan_cain_2nd.%s.it_id=p.item_id "
                     "WHERE p.delete_flag=0 AND p.%s=1 AND %s",
                 tables[i], tables[i], tables[i], tables[i], tables[i], flags[i], where);
        ok = run(d, &s);
    }
    if (!ok && !err[0]) snprintf(err, ERRBUF, "%s", d->err);
    str_free(&s);
    return ok;
}

int mail_drop_attach(Db *d, u64 postal_id, Err err) {
    if (!db_tx_begin(d, err)) return 0;
    char where[80];
    snprintf(where, sizeof(where), "p.postal_id=%llu", (unsigned long long)postal_id);
    if (!mail_drop_where(d, where, err)) { db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}

static int delete_mail_where(Db *d, const char *where, Err err) {
    if (!db_tx_begin(d, err)) return 0;
    if (!mail_drop_where(d, where, err)) { db_tx_rollback(d); return 0; }
    Str s = {0};
    str_catf(&s, "UPDATE taiwan_cain_2nd.postal p SET p.delete_flag=1 "
                 "WHERE p.delete_flag=0 AND %s", where);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) { snprintf(err, ERRBUF, "%s", d->err); db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}

int db_delete_mail(Db *d, u64 postal_id, Err err) {
    char where[80];
    snprintf(where, sizeof(where), "p.postal_id=%llu", (unsigned long long)postal_id);
    return delete_mail_where(d, where, err);
}

int db_delete_all_mails(Db *d, const Charac *c, Err err) {
    char where[80];
    snprintf(where, sizeof(where), "p.receive_charac_no=%u", c->no);
    return delete_mail_where(d, where, err);
}

static int mail_insert_id(Db *d, u64 *id, Err err);

// A recipient can be online: the original game loads new mail from these
// tables. Hold the character row so deletion/transfer cannot race the insert.
static int mail_send_begin(Db *d, const Charac *c, int attachment, const char *item_table, Err err) {
    if (!c->no) { snprintf(err, ERRBUF, "邮件收件角色无效，未发放"); return 0; }
    Str s = {0}; MyResult r = {0};
    str_cat(&s, "SELECT COUNT(*) FROM information_schema.TABLES WHERE ENGINE='InnoDB' AND "
        "CONCAT(TABLE_SCHEMA,'.',TABLE_NAME) IN ('taiwan_cain.charac_info','taiwan_cain_2nd.letter'");
    if (attachment) str_cat(&s, ",'taiwan_cain_2nd.postal'");
    if (item_table) str_catf(&s, ",'taiwan_cain_2nd.%s'", item_table);
    str_cat(&s, ")");
    int ok = select1(d, &s, &r) && my_i64(&r, 0, 0, 0) == 2 + attachment + (item_table != NULL);
    my_free(&r); str_clear(&s);
    if (!ok) { snprintf(err, ERRBUF, "邮件发放需要 InnoDB 角色、正文及相关附件表，未发放"); goto finish; }
    if (!db_tx_begin(d, err)) { ok = 0; goto finish; }
    str_catf(&s, "SELECT m_id,delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%u FOR UPDATE", c->no);
    ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, 0) != 0 &&
        (!c->mid || my_u64(&r, 0, 0, 0) == c->mid) && my_i64(&r, 0, 1, 1) == 0;
    if (!ok) { snprintf(err, ERRBUF, "邮件未发放：收件角色不存在、已删除或账号归属变化"); db_tx_rollback(d); }
finish:
    my_free(&r); str_free(&s); return ok;
}

static int mail_letter_matches(Db *d, const Charac *c, u64 id, const char *sender, const char *text, Err err) {
    Str s = {0}, from = {0}, body = {0}; MyResult r = {0};
    game_text_literal(&from, sender ? sender : "GM");
    game_text_literal(&body, text ? text : "");
    str_catf(&s, "SELECT charac_no=%u AND send_charac_no=0 AND "
        "BINARY send_charac_name=BINARY %s AND BINARY letter_text=BINARY %s AND stat=1 "
        "FROM taiwan_cain_2nd.letter WHERE letter_id=%llu FOR UPDATE", c->no,
        str_c(&from), str_c(&body), (unsigned long long)id);
    int ok = select1(d, &s, &r) && r.nrow == 1 && my_i64(&r, 0, 0, 0) == 1;
    if (!ok) snprintf(err, ERRBUF, "邮件正文回读不一致，已回滚");
    my_free(&r); str_free(&s); str_free(&from); str_free(&body); return ok;
}

static int mail_insert_letter(Db *d, const Charac *c, const char *sender, const char *text, u64 *id, Err err) {
    const char *body = text ? text : "", *from = sender ? sender : "GM";
    if (strlen(body) > 255 || strlen(from) > 20 ||
        !utf8_valid((const u8 *)body, (int)strlen(body)) || !utf8_valid((const u8 *)from, (int)strlen(from))) {
        snprintf(err, ERRBUF, "邮件正文最多 255 字节、发件人最多 20 字节，须为 UTF-8 文字");
        return 0;
    }
    if (!set_names(d, db_cs_name(d->charset))) {
        snprintf(err, ERRBUF, "邮件字符集：%s", d->err);
        return 0;
    }
    Str es = {0}, et = {0};
    game_text_literal(&es, sender ? sender : "GM");
    game_text_literal(&et, text ? text : "");
    Str s = {0};
    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&s,
             "INSERT INTO taiwan_cain_2nd.letter "
             "(charac_no,send_charac_no,send_charac_name,letter_text,reg_date,stat) "
             "VALUES (%u,0,%s,%s,'%s',1)",
             c->no, str_c(&es), str_c(&et), ts);
    str_free(&es);
    str_free(&et);
    int ok = run(d, &s);
    str_free(&s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    return ok && mail_insert_id(d, id, err);
}

static int mail_insert_id(Db *d, u64 *id, Err err) {
    Str s = {0};
    MyResult r = {0};
    str_cat(&s, "SELECT LAST_INSERT_ID()");
    int ok = select1(d, &s, &r);
    *id = ok ? my_u64(&r, 0, 0, 0) : 0;
    if (!ok) snprintf(err, ERRBUF, "邮件插入编号：%s", d->err);
    else if (!*id || *id > INT32_MAX) {
        snprintf(err, ERRBUF, "邮件插入编号超出有效范围");
        ok = 0;
    }
    my_free(&r);
    str_free(&s);
    return ok;
}

int db_send_text_mail(Db *d, const Charac *c, const char *sender, const char *text, Err err) {
    if (!mail_send_begin(d, c, 0, NULL, err)) return 0;
    u64 id = 0;
    if (!mail_insert_letter(d, c, sender, text, &id, err) ||
        !mail_letter_matches(d, c, id, sender, text, err)) { db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}

int db_send_item_mail_ex(Db *d, const Charac *c, const char *sender, const char *text,
                         const GrantItem *g, int avata_flag, int creature_flag, Err err) {
    if (!g || !g->id || !g->num || (avata_flag != 0 && avata_flag != 1) ||
        (creature_flag != 0 && creature_flag != 1) || (avata_flag && creature_flag) ||
        ((avata_flag || creature_flag) && g->num != 1)) {
        snprintf(err, ERRBUF, "邮件附件编号、数量或类型无效，未发放"); return 0;
    }
    // 正文、附件行、postal 必须同生共死：附件先落库而 postal 失败会留下占格的孤儿行
    const char *item_table = avata_flag ? "user_items" : creature_flag ? "creature_items" : NULL;
    if (!mail_send_begin(d, c, 1, item_table, err)) return 0;
    u64 letter_id = 0;
    if (!mail_insert_letter(d, c, sender, text, &letter_id, err)) goto fail;
    // 正文已经设置会话字符集；使用本连接的插入编号，不猜测“最新一封”。
    Str es = {0};
    game_text_literal(&es, sender ? sender : "GM");
    Str s = {0};

    // 附件落点：普通物品 add_info 存数量；时装/宠物要先建一行再取 ui_id 作为 add_info
    u64 add_info = (u64)g->num;
    if (avata_flag) {
        char ts0[32];
        now_str(ts0, sizeof(ts0), "%Y-%m-%d %H:%M:%S");
        str_catf(&s,
                 "INSERT INTO taiwan_cain_2nd.user_items "
                 "(charac_no,it_id,expire_date,obtain_from,reg_date,stat,jewel_socket) "
                 "VALUES (%u,%u,'9999-12-31 23:59:59',1,'%s',2,X'')",
                 c->no, g->id, ts0);
        if (!run(d, &s)) { snprintf(err, ERRBUF, "建时装行：%s", d->err); str_free(&s); str_free(&es); goto fail; }
        if (!mail_insert_id(d, &add_info, err)) { str_free(&s); str_free(&es); goto fail; }
        str_clear(&s);
    } else if (creature_flag) {
        char ts0[32];
        now_str(ts0, sizeof(ts0), "%Y-%m-%d %H:%M:%S");
        str_catf(&s,
                 "INSERT INTO taiwan_cain_2nd.creature_items "
                 "(charac_no,it_id,expire_date,reg_date,stat,item_lock_key,creature_type,stomach) "
                 "VALUES (%u,%u,'9999-12-31 23:59:59','%s',0,0,0,100)",
                 c->no, g->id, ts0);
        if (!run(d, &s)) { snprintf(err, ERRBUF, "建宠物行：%s", d->err); str_free(&s); str_free(&es); goto fail; }
        if (!mail_insert_id(d, &add_info, err)) { str_free(&s); str_free(&es); goto fail; }
        str_clear(&s);
    }

    char ts[32];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    str_catf(&s,
             "INSERT INTO taiwan_cain_2nd.postal "
             "(occ_time,send_charac_no,send_charac_name,receive_charac_no,amplify_option,amplify_value,"
             "seperate_upgrade,seal_flag,item_id,add_info,upgrade,gold,letter_id,avata_flag,creature_flag,"
             "endurance,unlimit_flag) "
             "VALUES ('%s',0,%s,%u,%u,%u,%u,%u,%u,%llu,%u,0,%llu,%d,%d,%u,1)",
             ts, str_c(&es), c->no, g->inc_type, g->inc_val, g->forge, g->seal, g->id,
             (unsigned long long)add_info, g->enhance,
             (unsigned long long)letter_id, avata_flag, creature_flag, g->dur);
    int ok = run(d, &s);
    if (!ok) {
        snprintf(err, ERRBUF, "%s", d->err);
    } else {
        u64 postal_id = 0;
        ok = mail_insert_id(d, &postal_id, err);
        if (ok) {
            str_clear(&s);
            str_catf(&s, "SELECT receive_charac_no=%u AND send_charac_no=0 AND "
                "BINARY send_charac_name=BINARY %s AND amplify_option=%u AND amplify_value=%u "
                "AND seperate_upgrade=%u AND seal_flag=%u AND item_id=%u AND add_info=%llu "
                "AND upgrade=%u AND gold=0 AND letter_id=%llu AND avata_flag=%d AND creature_flag=%d "
                "AND endurance=%u AND unlimit_flag=1 AND delete_flag=0 "
                "FROM taiwan_cain_2nd.postal WHERE postal_id=%llu FOR UPDATE",
                c->no, str_c(&es), g->inc_type, g->inc_val, g->forge, g->seal, g->id,
                (unsigned long long)add_info, g->enhance, (unsigned long long)letter_id,
                avata_flag, creature_flag, g->dur, (unsigned long long)postal_id);
            MyResult r = {0};
            ok = select1(d, &s, &r) && r.nrow == 1 && my_i64(&r, 0, 0, 0) == 1;
            if (!ok) snprintf(err, ERRBUF, "邮件附件回读不一致，已回滚");
            my_free(&r);
        }
    }
    if (ok && item_table) {
        str_clear(&s);
        str_catf(&s, "SELECT charac_no=%u AND it_id=%u AND expire_date='9999-12-31 23:59:59' AND %s "
            "FROM taiwan_cain_2nd.%s WHERE ui_id=%llu FOR UPDATE", c->no, g->id,
            avata_flag ? "stat=2 AND obtain_from=1" : "stat=0 AND item_lock_key=0 AND creature_type=0 AND stomach=100",
            item_table, (unsigned long long)add_info);
        MyResult r = {0};
        ok = select1(d, &s, &r) && r.nrow == 1 && my_i64(&r, 0, 0, 0) == 1;
        if (!ok) snprintf(err, ERRBUF, "邮件时装／宠物回读不一致，已回滚");
        my_free(&r);
    }
    if (ok) ok = mail_letter_matches(d, c, letter_id, sender, text, err);
    str_free(&es); str_free(&s);
    if (!ok) goto fail;
    return db_tx_commit(d, err);

fail:
    db_tx_rollback(d);
    return 0;
}

// 全服 / 在线批量发邮件：逐角色发一封（文本 + 可选附件）。在线模式只发给
// login_account_3.login_status>=1 的账号下角色。item_id=0 时只发文本。
int db_send_mail_bulk(Db *d, int online_only, const char *sender, const char *text,
                      u32 item_id, u32 num, int avata_flag, int creature_flag, u32 *sent, Err err) {
    if (sent) *sent = 0;
    Str s = {0};
    str_cat(&s, "SELECT charac_no,m_id FROM taiwan_cain.charac_info WHERE delete_flag=0");
    if (online_only)
        str_cat(&s, " AND m_id IN (SELECT m_id FROM taiwan_login.login_account_3 WHERE login_status>=1)");
    str_cat(&s, " ORDER BY charac_no");
    MyResult r = {0};
    if (!select1(d, &s, &r)) { snprintf(err, ERRBUF, "%s", d->err); str_free(&s); return 0; }
    str_free(&s);
    u32 done = 0;
    int fail = 0;
    for (int i = 0; i < r.nrow; i++) {
        u32 cno = (u32)my_u64(&r, i, 0, 0);
        Charac c;
        memset(&c, 0, sizeof(c));
        c.no = cno;
        c.mid = (u32)my_u64(&r, i, 1, 0);
        int ok;
        if (item_id) {
            GrantItem g;
            memset(&g, 0, sizeof(g));
            g.id = item_id;
            g.num = num ? num : 1;
            g.type = IT_STACK;
            ok = db_send_item_mail_ex(d, &c, sender, text, &g, avata_flag, creature_flag, err);
        } else {
            ok = db_send_text_mail(d, &c, sender, text, err);
        }
        if (ok) done++;
        else { fail = 1; break; }
    }
    my_free(&r);
    if (sent) *sent = done;
    return fail ? 0 : 1;
}

// 全服软删所有邮件（delete_flag=1），用于「清服」类维护。
int db_delete_all_mails_global(Db *d, Err err) {
    return delete_mail_where(d, "1", err);
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
        cp_game_text(p->name, sizeof(p->name), &r, i, 4);
        cp_text(p->expire, sizeof(p->expire), &r, i, 5);
    }
    my_free(&r);
    return 1;
}

// Lock the login row (or its gap), character and item until the edit commits.
// The UI snapshot alone cannot protect against login or ownership changes.
static int companion_edit(Db *d, const Charac *c, u32 ui_id, int avatar,
                          const char *field, const char *value, Err err) {
    if (!c->no || !c->mid || !ui_id) {
        snprintf(err, ERRBUF, "宠物／时装修改缺少角色、账号或物品编号"); return 0;
    }
    const char *table = avatar ? "user_items" : "creature_items";
    Str s = {0}; MyResult r = {0};
    str_catf(&s, "SELECT COUNT(*) FROM information_schema.TABLES WHERE ENGINE='InnoDB' AND "
        "((TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME='charac_info') "
        "OR (TABLE_SCHEMA='taiwan_login' AND TABLE_NAME='login_account_3') "
        "OR (TABLE_SCHEMA='taiwan_cain_2nd' AND TABLE_NAME='%s'))", table);
    int ok = select1(d, &s, &r) && my_u64(&r, 0, 0, 0) == 3;
    my_free(&r); str_clear(&s);
    if (!ok) { snprintf(err, ERRBUF, "宠物／时装修改需要 InnoDB 角色、登录和物品表，未写入"); goto finish; }
    if (!d->tx_depth) {
        str_cat(&s, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
        ok = run(d, &s); str_clear(&s);
        if (!ok) { snprintf(err, ERRBUF, "%s", d->err); goto finish; }
    }
    if (!db_tx_begin(d, err)) { ok = 0; goto finish; }
    str_catf(&s, "SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%u FOR UPDATE", c->mid);
    ok = select1(d, &s, &r);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    else if (r.nrow && my_i64(&r, 0, 0, -1) != 0) {
        snprintf(err, ERRBUF, "账号在线或正在退出，请先退出游戏后修改宠物／时装，未写入"); ok = 0;
    }
    my_free(&r); str_clear(&s);
    if (ok) {
        str_catf(&s, "SELECT m_id,delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%u FOR UPDATE", c->no);
        ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, 0) == c->mid && my_i64(&r, 0, 1, 1) == 0;
        if (!ok) snprintf(err, ERRBUF, "未写入：角色不存在、已删除或账号归属变化");
        my_free(&r); str_clear(&s);
    }
    if (ok) {
        str_catf(&s, "SELECT charac_no FROM taiwan_cain_2nd.%s WHERE ui_id=%u FOR UPDATE", table, ui_id);
        ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, 0) == c->no;
        if (!ok) snprintf(err, ERRBUF, "未写入：所选物品不存在或已不属于当前角色，请刷新");
        my_free(&r); str_clear(&s);
    }
    if (ok) {
        if (field) str_catf(&s, "UPDATE taiwan_cain_2nd.%s SET %s=%s WHERE ui_id=%u AND charac_no=%u", table, field, value, ui_id, c->no);
        else str_catf(&s, "DELETE FROM taiwan_cain_2nd.%s WHERE ui_id=%u AND charac_no=%u", table, ui_id, c->no);
        ok = run(d, &s); str_clear(&s);
        if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    }
    if (ok) {
        if (field) str_catf(&s, "SELECT BINARY %s=BINARY %s FROM taiwan_cain_2nd.%s WHERE ui_id=%u AND charac_no=%u FOR UPDATE", field, value, table, ui_id, c->no);
        else str_catf(&s, "SELECT ui_id FROM taiwan_cain_2nd.%s WHERE ui_id=%u FOR UPDATE", table, ui_id);
        ok = select1(d, &s, &r) && (field ? (r.nrow == 1 && my_i64(&r, 0, 0, 0) == 1) : r.nrow == 0);
        if (!ok) snprintf(err, ERRBUF, "宠物／时装修改回读不一致，已回滚");
    }
    if (!ok) db_tx_rollback(d);
    else ok = db_tx_commit(d, err);
finish:
    my_free(&r); str_free(&s); return ok;
}

int db_set_pet_name(Db *d, const Charac *c, u32 ui_id, const char *name, Err err) {
    if (!name || strlen(name) > 12 || !utf8_valid((const u8 *)name, (int)strlen(name))) {
        snprintf(err, ERRBUF, "宠物名最多 12 字节，且必须为 UTF-8 文字");
        return 0;
    }
    if (!set_names(d, db_cs_name(d->charset))) { snprintf(err, ERRBUF, "%s", d->err); return 0; }
    Str value = {0};
    game_text_literal(&value, name);
    int ok = companion_edit(d, c, ui_id, 0, "name", str_c(&value), err);
    str_free(&value);
    return ok;
}

int db_set_pet_full(Db *d, const Charac *c, u32 ui_id, Err err) {
    return companion_edit(d, c, ui_id, 0, "stomach", "100", err);
}

int db_set_pet_expire(Db *d, const Charac *c, u32 ui_id, Err err) {
    return companion_edit(d, c, ui_id, 0, "expire_date", "'9999-12-31 23:59:59'", err);
}

int db_delete_pet(Db *d, const Charac *c, u32 ui_id, Err err) {
    return companion_edit(d, c, ui_id, 0, NULL, NULL, err);
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

int db_set_avatar_hidden(Db *d, const Charac *c, u32 ui_id, int hidden, Err err) {
    if (hidden < 0 || hidden > 65535) { snprintf(err, ERRBUF, "时装选项超出 0..65535，未写入"); return 0; }
    char value[16]; snprintf(value, sizeof(value), "%d", hidden);
    return companion_edit(d, c, ui_id, 1, "hidden_option", value, err);
}

int db_set_avatar_expire(Db *d, const Charac *c, u32 ui_id, Err err) {
    return companion_edit(d, c, ui_id, 1, "expire_date", "'9999-12-31 23:59:59'", err);
}

int db_delete_avatar(Db *d, const Charac *c, u32 ui_id, Err err) {
    return companion_edit(d, c, ui_id, 1, NULL, NULL, err);
}

// ---------------- 任务 ----------------
int db_list_quests(Db *d, const Charac *c, Vec *out, Err err) {
    // 显式列名，不写 SELECT *：这张表在 play_10_trigger 和 play_11 之间插了一列
    // auto_clear，按位置两两配对会整段错位 —— 参考实现是靠 `if i==20: i+=1`
    // 跳过去的（它先切掉了前三列）。按名字取，以后再加杂列也不怕。
    Str s = {0};
    str_cat(&s, "SELECT charac_no,clear_quest,quest_notify");
    for (int k = 1; k <= 20; k++) str_catf(&s, ",play_%d,play_%d_trigger", k, k);
    str_catf(&s, " FROM taiwan_cain.new_charac_quest WHERE charac_no=%u", c->no);
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

int db_quest_write_begin(Db *d, const Charac *c, Err err) {
    if (!c->no || !c->mid) { snprintf(err, ERRBUF, "任务修改缺少角色或账号编号"); return 0; }
    Str s = {0}; MyResult r = {0};
    // Both rollback and the account row/gap lock are required. Older MyISAM
    // installations cannot safely combine a quest edit with inventory/import.
    str_cat(&s, "SELECT COUNT(*) FROM information_schema.TABLES WHERE ENGINE='InnoDB' AND "
        "((TABLE_SCHEMA='taiwan_cain' AND TABLE_NAME IN ('charac_info','new_charac_quest','charac_quest_shop')) "
        "OR (TABLE_SCHEMA='taiwan_login' AND TABLE_NAME='login_account_3') "
        "OR (TABLE_SCHEMA='taiwan_cain_2nd' AND TABLE_NAME='inventory'))");
    int ok = select1(d, &s, &r) && my_u64(&r, 0, 0, 0) == 5;
    my_free(&r); str_clear(&s);
    if (!ok) { snprintf(err, ERRBUF, "任务修改需要完整的 InnoDB 角色、登录、任务和背包表，未写入"); goto finish; }
    // The absent-login-row case needs a range lock as well. Only this next
    // transaction changes isolation; the connection's default is preserved.
    if (!d->tx_depth) {
        str_cat(&s, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
        ok = run(d, &s); str_clear(&s);
        if (!ok) { snprintf(err, ERRBUF, "%s", d->err); goto finish; }
    }
    if (!db_tx_begin(d, err)) { ok = 0; goto finish; }
    str_catf(&s, "SELECT login_status FROM taiwan_login.login_account_3 WHERE m_id=%u FOR UPDATE", c->mid);
    ok = select1(d, &s, &r);
    if (ok && r.nrow && my_i64(&r, 0, 0, -1) != 0) {
        snprintf(err, ERRBUF, "账号在线或正在退出，请先退出游戏后修改任务，未写入"); ok = 0;
    } else if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    my_free(&r); str_clear(&s);
    if (ok) {
        str_catf(&s, "SELECT m_id,delete_flag FROM taiwan_cain.charac_info WHERE charac_no=%u FOR UPDATE", c->no);
        ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, 0) == c->mid && my_i64(&r, 0, 1, 1) == 0;
        if (!ok) snprintf(err, ERRBUF, "任务未写入：角色不存在、已删除或账号归属变化");
    }
    if (!ok) db_tx_rollback(d);
finish:
    my_free(&r); str_free(&s); return ok;
}

static int quest_save_blob(Db *d, const Charac *c, const char *table, const char *field,
                           const void *raw, size_t n, Err err) {
    if (!db_save_blob(d, c, table, field, raw, n, err)) return 0;
    Str s = {0}; MyResult r = {0};
    str_catf(&s, "SELECT %s FROM %s WHERE charac_no=%u FOR UPDATE", field, table, c->no);
    int ok = select1(d, &s, &r) && r.nrow == 1 && my_len(&r, 0, 0) == (int)n &&
             my_text(&r, 0, 0) && !memcmp(my_text(&r, 0, 0), raw, n);
    if (!ok) snprintf(err, ERRBUF, "任务相关数据回读不一致，已回滚");
    my_free(&r); str_free(&s); return ok;
}

// 放弃任务后，背包里为它攒的任务材料就成了垃圾。
// 只清 type=4（任务材料）且编号命中的格子，普通材料一律不动。
static int quest_purge_items(Db *d, const Charac *c, const u32 *ids, int n, Err err) {
    Str s = {0};
    MyResult r = {0};
    str_catf(&s, "SELECT inventory FROM taiwan_cain_2nd.inventory WHERE charac_no=%u FOR UPDATE", c->no);
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
    int ok = quest_save_blob(d, c, "taiwan_cain_2nd.inventory", "inventory", out.p, out.n, err);
    buf_free(&out);
    return ok;
}

// The expected quest prevents an old UI selection from editing a reused slot.
// Abandon and material removal are one edit, never two independent commits.
int db_set_quest_slot(Db *d, const Charac *c, int slot, u32 expected_id, u32 id, u32 trigger,
                       const u32 *purge_ids, int purge_n, Err err) {
    if (slot < 1 || slot > 20 || purge_n < 0 || (purge_n && !purge_ids)) {
        snprintf(err, ERRBUF, "任务槽位或材料参数无效"); return 0;
    }
    int online=0;
    if (!db_account_online(d, c->mid, &online, err)) return 0;
    if (online) {
        if (!expected_id || id!=expected_id || trigger || purge_n) {
            snprintf(err, ERRBUF, "此项任务修改需先退出游戏，未写入"); return 0;
        }
        OnlineCommand request;
        return db_online_quest_ready_submit(d, c, slot, id, &request, err) && db_online_finish(d, &request, err);
    }
    if (!db_quest_write_begin(d, c, err)) return 0;
    Str s = {0}; MyResult r = {0};
    str_catf(&s, "SELECT play_%d FROM taiwan_cain.new_charac_quest WHERE charac_no=%u FOR UPDATE", slot, c->no);
    int ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, UINT64_MAX) == expected_id;
    if (!ok) snprintf(err, ERRBUF, "任务记录缺失或所选槽位已变化，请刷新后重试，未写入");
    my_free(&r); str_clear(&s);
    if (ok) {
        str_catf(&s, "UPDATE taiwan_cain.new_charac_quest SET play_%d=%u,play_%d_trigger=%u WHERE charac_no=%u",
                 slot, id, slot, trigger, c->no);
        ok = run(d, &s);
        if (!ok) snprintf(err, ERRBUF, "%s", d->err);
        str_clear(&s);
    }
    if (ok) {
        str_catf(&s, "SELECT play_%d,play_%d_trigger FROM taiwan_cain.new_charac_quest WHERE charac_no=%u FOR UPDATE",
                 slot, slot, c->no);
        ok = select1(d, &s, &r) && r.nrow == 1 && my_u64(&r, 0, 0, UINT64_MAX) == id &&
             my_u64(&r, 0, 1, UINT64_MAX) == trigger;
        if (!ok) snprintf(err, ERRBUF, "任务槽位回读不一致，已回滚");
    }
    if (ok && purge_n) ok = quest_purge_items(d, c, purge_ids, purge_n, err);
    my_free(&r); str_free(&s);
    if (!ok) { db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}

// ---------------- 任务：批量完成 / 初始化 ----------------
// Original CQuestClear indexes 30000 individual bytes by quest ID (1..29999).
// A completed quest has byte 1; this is not a list of u32 quest IDs.
#define QUEST_CLEAR_BYTES 30000

static int quest_clear_write(Db *d, const Charac *c, const u8 *raw, Err err) {
    Buf z = {0}, out = {0};
    if (!zlib_pack(raw, QUEST_CLEAR_BYTES, &z)) {
        snprintf(err, ERRBUF, "压缩任务完成表失败");
        return 0;
    }
    buf_put32(&out, QUEST_CLEAR_BYTES);
    buf_put(&out, z.p, z.n);
    buf_free(&z);
    int ok = quest_save_blob(d, c, "taiwan_cain.new_charac_quest", "clear_quest",
                          (const char *)out.p, out.n, err);
    buf_free(&out);
    return ok;
}

static int quest_slots_zero(Db *d, const Charac *c, Err err) {
    Str s = {0};
    str_cat(&s, "UPDATE taiwan_cain.new_charac_quest SET ");
    for (int i = 1; i <= 20; i++)
        str_catf(&s, "%splay_%d=0,play_%d_trigger=0", i > 1 ? "," : "", i, i);
    str_catf(&s, " WHERE charac_no=%u", c->no);
    int ok = run(d, &s);
    if (!ok) snprintf(err, ERRBUF, "%s", d->err);
    if (ok) {
        MyResult r = {0}; str_clear(&s);
        str_cat(&s, "SELECT 1 FROM taiwan_cain.new_charac_quest WHERE ");
        for (int i = 1; i <= 20; i++) str_catf(&s, "play_%d=0 AND play_%d_trigger=0 AND ", i, i);
        str_catf(&s, "charac_no=%u FOR UPDATE", c->no);
        ok = select1(d, &s, &r) && r.nrow == 1;
        if (!ok) snprintf(err, ERRBUF, "任务槽位缺失或回读不一致，已回滚");
        my_free(&r);
    }
    str_free(&s);
    return ok;
}

// 任务初始化：槽位、完成表、通知表全部归零（回到刚建号的形态）
int db_quest_init(Db *d, const Charac *c, Err err) {
    if (!db_quest_write_begin(d, c, err)) return 0;
    u8 *zeros = (u8 *)xalloc(QUEST_CLEAR_BYTES);
    memset(zeros, 0, QUEST_CLEAR_BYTES);
    static const u8 notif[40] = {0};
    int ok = quest_clear_write(d, c, zeros, err) &&
             quest_save_blob(d, c, "taiwan_cain.new_charac_quest", "quest_notify",
                          (const char *)notif, (int)sizeof(notif), err) &&
             quest_slots_zero(d, c, err);
    free(zeros);
    if (!ok) { db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}

// 放弃全部进行中的任务：把 20 个槽位清零而不是删行——
// 服务器期望这行存在，删掉可能让角色再也接不了任务。
int db_clear_quests(Db *d, const Charac *c, Err err) {
    if (!db_quest_write_begin(d, c, err)) return 0;
    if (!quest_slots_zero(d, c, err)) { db_tx_rollback(d); return 0; }
    return db_tx_commit(d, err);
}
