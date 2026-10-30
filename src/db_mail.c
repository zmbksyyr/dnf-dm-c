// db_mail.c：邮件 / 宠物 / 时装 / 任务表操作。从 db.c 拆出，行为不变。
#include "db_priv.h"
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

// 邮件本身只是软删（delete_flag=1）；但它附带的时装/宠物是真实行，
// 不跟着清掉就成了占格的孤儿记录。add_info 就是那行的 ui_id。
void mail_drop_attach(Db *d, u64 postal_id) {
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
