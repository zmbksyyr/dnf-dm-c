// 泡点：先固定收件名单，再在同一事务内结算角色与账号奖励。
#include "db_priv.h"

#define BUB_BATCH 256

static int cmp_mid(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return (x > y) - (x < y);
}

static void id_list(Str *s, const Vec *ids, int start, int count, int quoted) {
    for (int j = 0; j < count; j++) {
        if (j) str_catc(s, ',');
        str_catf(s, quoted ? "'%u'" : "%u", *(const u32 *)vec_at(ids, start + j));
    }
}

static int bubble_targets(Db *d, const BubbleCfg *cfg, Vec *chars, Vec *accounts, Err err) {
    Str s = {0};
    MyResult r = {0};
    int ok = 0;
    if (cfg->target == 3) {
        str_catf(&s, "SELECT UID FROM d_taiwan.accounts WHERE UID=%u", cfg->tno);
        if (!select1(d, &s, &r)) goto done;
        if (!r.nrow) { snprintf(err, ERRBUF, "泡点：指定账号不存在"); goto done; }
        *VEC_ADD(accounts, u32) = cfg->tno;
        my_free(&r);
        str_clear(&s);
    }
    str_cat(&s, "SELECT c.charac_no,c.m_id FROM taiwan_cain.charac_info c "
                "WHERE c.delete_flag=0 AND EXISTS "
                "(SELECT 1 FROM d_taiwan.accounts a WHERE a.UID=c.m_id)");
    if (cfg->target == 0)
        str_cat(&s, " AND EXISTS (SELECT 1 FROM taiwan_login.login_account_3 l "
                    "WHERE l.m_id=c.m_id AND l.login_status>=1)");
    else if (cfg->target == 2) str_catf(&s, " AND c.charac_no=%u", cfg->tno);
    else if (cfg->target == 3) str_catf(&s, " AND c.m_id=%u", cfg->tno);
    str_cat(&s, " ORDER BY c.charac_no FOR UPDATE");
    if (!select1(d, &s, &r)) goto done;
    if (cfg->target == 2 && !r.nrow) {
        snprintf(err, ERRBUF, "泡点：指定角色不存在、已删除或所属账号不存在");
        goto done;
    }
    for (int i = 0; i < r.nrow; i++) {
        *VEC_ADD(chars, u32) = (u32)my_u64(&r, i, 0, 0);
        *VEC_ADD(accounts, u32) = (u32)my_u64(&r, i, 1, 0);
    }
    if (accounts->n > 1) {
        qsort(accounts->p, (size_t)accounts->n, sizeof(u32), cmp_mid);
        u32 *ids = (u32 *)accounts->p;
        int n = 1;
        for (int i = 1; i < accounts->n; i++)
            if (ids[i] != ids[n - 1]) ids[n++] = ids[i];
        accounts->n = n;
    }
    ok = 1;
done:
    if (!ok && !err[0]) snprintf(err, ERRBUF, "泡点目标：%s", d->err);
    my_free(&r);
    str_free(&s);
    return ok;
}

// 先以零值建缺行并取得写锁，再带上界条件递增；非严格 SQL 模式也不能静默截断。
static int bubble_currency(Db *d, const Vec *ids, const char *table, const char *col,
                           int amount, int billing, Err err) {
    if (!amount) return 1;
    Str s = {0};
    int ok = 1;
    for (int i = 0; i < ids->n; i += BUB_BATCH) {
        if (db_cancelled()) { snprintf(err, ERRBUF, "已取消"); ok = 0; break; }
        int n = ids->n - i < BUB_BATCH ? ids->n - i : BUB_BATCH;
        if (billing) {
            str_clear(&s);
            str_catf(&s, "INSERT INTO %s (account,%s,%sreg_date,mod_date) VALUES ",
                     table, col, billing == 1 ? "mod_tran," : "");
            for (int j = 0; j < n; j++) {
                if (j) str_catc(&s, ',');
                str_catf(&s, "('%u',0,%sNOW(),NOW())", *(const u32 *)vec_at(ids, i + j),
                         billing == 1 ? "0," : "");
            }
            str_cat(&s, " ON DUPLICATE KEY UPDATE account=VALUES(account)");
            if (!run(d, &s)) { ok = 0; break; }
        }
        str_clear(&s);
        str_catf(&s, "UPDATE %s SET %s=%s+%d%s WHERE %s IN (", table, col, col,
                 amount, billing ? ",mod_date=NOW()" : "", billing ? "account" : "charac_no");
        id_list(&s, ids, i, n, billing);
        str_catf(&s, ") AND %s<=%u", col, UINT32_MAX - (u32)amount);
        u64 affected = 0;
        if (!run_aff(d, &s, &affected)) { ok = 0; break; }
        if (affected != (u64)n) {
            snprintf(err, ERRBUF, "泡点·%s：余额溢出、为空或缺少角色库存行，已回滚", col);
            ok = 0;
            break;
        }
    }
    if (!ok && !err[0]) snprintf(err, ERRBUF, "泡点·%s：%s", col, d->err);
    str_free(&s);
    return ok;
}

int db_bubble_tick(Db *d, const BubbleCfg *cfg, BubblePaid *paid, Err err) {
    if (paid) memset(paid, 0, sizeof(*paid));
    err[0] = 0;
    if (!cfg || cfg->target < 0 || cfg->target > 3 || (cfg->target >= 2 && !cfg->tno) ||
        cfg->coin < 0 || cfg->dpoint < 0 || cfg->dcoin < 0 ||
        (cfg->item && (cfg->itemnum < 1 || cfg->itemnum > 200 || cfg->target == 1)) ||
        cfg->avatar < 0 || cfg->avatar > 1 || cfg->creature < 0 || cfg->creature > 1 ||
        (cfg->avatar && cfg->creature)) {
        snprintf(err, ERRBUF, "泡点配置无效（全服不支持物品；数量须为 1~200）");
        return 0;
    }
    if (!cfg->coin && !cfg->dpoint && !cfg->dcoin && !cfg->item) return 1;
    if (!db_tx_begin(d, err)) return 0;
    Vec chars, accounts;
    vec_init(&chars, sizeof(u32));
    vec_init(&accounts, sizeof(u32));
    int ok = bubble_targets(d, cfg, &chars, &accounts, err) &&
        bubble_currency(d, &chars, "taiwan_cain_2nd.inventory", "coin", cfg->coin, 0, err) &&
        bubble_currency(d, &accounts, "taiwan_billing.cash_cera", "cera", cfg->dpoint, 1, err) &&
        bubble_currency(d, &accounts, "taiwan_billing.cash_cera_point", "cera_point", cfg->dcoin, 2, err);
    for (int i = 0; ok && cfg->item && i < chars.n; i++) {
        Charac c = {0};
        c.no = *(const u32 *)vec_at(&chars, i);
        GrantItem item = {0};
        item.id = cfg->item;
        item.type = IT_STACK;
        item.num = cfg->avatar || cfg->creature ? 1U : (u32)cfg->itemnum;
        int copies = cfg->avatar || cfg->creature ? cfg->itemnum : 1;
        for (int j = 0; ok && j < copies; j++) {
            if (db_cancelled()) { snprintf(err, ERRBUF, "已取消"); ok = 0; break; }
            ok = db_send_item_mail_ex(d, &c, "泡点", "泡点奖励", &item,
                                      cfg->avatar, cfg->creature, err);
        }
    }
    if (ok && db_cancelled()) { snprintf(err, ERRBUF, "已取消"); ok = 0; }
    if (ok) ok = db_tx_commit(d, err);
    else db_tx_rollback(d);
    if (ok && paid) {
        paid->chars = cfg->coin || cfg->item ? (u32)chars.n : 0;
        paid->accounts = cfg->dpoint || cfg->dcoin ? (u32)accounts.n : 0;
    }
    vec_free(&chars);
    vec_free(&accounts);
    return ok;
}
