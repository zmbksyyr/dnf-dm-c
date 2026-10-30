// act：命令表的实现。所有 db_* 调用集中在这里，页面那边只填 kind 和参数。
#include "act.h"

Ctx kc;

// 列表暂存：job 线程只写这里，UI 线程在 job_poll 后搬到 g.*。
// 直接写 g.bans/g.events 是 vec_clear+VEC_ADD（realloc），UI 线程同时遍历/绘制
// 就是 use-after-free。双缓冲：job 写暂存，UI 线程在 job_poll 成功后交换。
static Vec  s_bans_next, s_ban_chars_next, s_events_next, s_event_info_next;
static volatile LONG s_lists_dirty;   // L_BANS | L_EVENTS 位掩码

// UI 线程调用：把暂存列表搬到 g.*（在 app_tick 的 job_poll 之后）
void act_apply_lists(void) {
    LONG dirty = InterlockedExchange(&s_lists_dirty, 0);
    if (dirty & L_BANS) {
        vec_free(&g.bans);
        g.bans = s_bans_next;
        vec_init(&s_bans_next, sizeof(BanRow));
        g.lists_ready |= L_BANS;
    }
    if (dirty & L_BANCHARS) {
        vec_free(&g.ban_chars);
        g.ban_chars = s_ban_chars_next;
        vec_init(&s_ban_chars_next, sizeof(Charac));
        g.lists_ready |= L_BANCHARS;
    }
    if (dirty & L_EVENTS) {
        vec_free(&g.events);
        g.events = s_events_next;
        vec_init(&s_events_next, sizeof(EventRow));
        vec_free(&g.event_info);
        g.event_info = s_event_info_next;
        vec_init(&s_event_info_next, sizeof(EventInfo));
        g.lists_ready |= L_EVENTS;
    }
}

void act_init(void) {
    vec_init(&s_bans_next, sizeof(BanRow));
    vec_init(&s_ban_chars_next, sizeof(Charac));
    vec_init(&s_events_next, sizeof(EventRow));
    vec_init(&s_event_info_next, sizeof(EventInfo));
}

void act_free(void) {
    vec_free(&s_bans_next);
    vec_free(&s_ban_chars_next);
    vec_free(&s_events_next);
    vec_free(&s_event_info_next);
}

// 这些操作会改到内存里那份快照：写完要回读校验一次（其余动作只动库，快照不变）
static int sync_after(int kind) {
    switch (kind) {
        case K_RENAME: case K_LEV: case K_JOB: case K_MONEY: case K_COIN: case K_CERA:
        case K_ACCM: case K_SP: case K_QP: case K_PVP:
            return 1;
        default:
            return 0;
    }
}

static int char_do(void *ctx, Err err) {
    Ctx *c = (Ctx *)ctx;
    // 归属角色在入队那一刻就定下来了。等队列排到、这个线程开跑时，用户可能已经
    // 切到别的角色 —— 那时再读 g.cur，A 的改动就写进 B 了。这里只认 ctx 里的编号。
    Charac who;
    memset(&who, 0, sizeof(who));
    who.no = c->cno;
    who.mid = c->mid;
    switch (c->kind) {
        case K_RENAME:
            // 重名检查放在任务线程里做：UI 线程直接查库会和后台作业抢同一条连接
            if (db_name_taken(&g.db, c->s, who.no)) {
                snprintf(err, ERRBUF, "名字「%s」已被占用", c->s);
                return 0;
            }
            return db_set_name(&g.db, &who, c->s, err);
        case K_LEV: return db_set_lev(&g.db, &who, c->i1, (u32)c->u1, err);
        case K_MONEY: return db_set_money(&g.db, &who, c->u1, err);
        case K_COIN: return db_set_coin(&g.db, &who, c->u1, err);
        case K_CERA: return db_set_cera(&g.db, &who, c->u1, c->u2, err);
        case K_JOB: return db_set_job(&g.db, &who, c->i1, c->i2, err);
        case K_ACCM: return db_set_account_money(&g.db, &who, c->u1, err);
        case K_SP: return db_set_sp(&g.db, &who, c->i1, c->i1, c->i2, c->i2, err);
        case K_QP: return db_set_qp(&g.db, &who, c->i1, err);
        case K_NOWEIGHT: return db_no_weight(&g.db, &who, err);
        case K_SLOT: return db_open_equip_slot(&g.db, &who, err);
        case K_UNLOCK: return db_unlock_lev_equip(&g.db, &who, err);
        case K_EXPERT: return db_max_expert(&g.db, &who, err);
        case K_FATIGUE: return db_set_fatigue(&g.db, &who, err);
        case K_DUNGEON: return db_unlock_dungeon(&g.db, &who, err);
        case K_PUNISH: return db_reset_punish(&g.db, c->mid, err);
        case K_QCLR: return db_clear_quests(&g.db, &who, err);
        case K_PVP: return db_set_pvp(&g.db, &who, c->i1, c->i2, c->i3, err);
        case K_BAN: return db_ban(&g.db, c->mid, c->i1, c->i2, c->i3, c->s, err);
        case K_UNBAN: return db_unban(&g.db, c->mid, c->i2, err);
        case K_BANLIST:
            // 写暂存，不碰 g.bans——UI 线程可能正在遍历
            return db_list_bans(&g.db, &s_bans_next, err);
        case K_BANCHARS:
            // 封停页角色选择器：加载全部角色（含已删除的，封禁操作可能针对任意角色）
            InterlockedOr(&s_lists_dirty, L_BANCHARS);
            return db_list_chars(&g.db, &s_ban_chars_next, 5000, NULL, err);
        case K_EVLIST:
            // 同上，写暂存
            if (!s_event_info_next.n && !g.event_info.n &&
                !db_list_event_info(&g.db, &s_event_info_next, err)) return 0;
            return db_list_events(&g.db, &s_events_next, err);
        case K_EVADD: return db_add_event(&g.db, (u32)c->i1, c->u1, c->u2, err);
        case K_EVDEL: return db_del_event(&g.db, (u64)c->u1, err);
        case K_RESETPWD: return db_reset_pwd(&g.db, c->mid, c->s, err);
        case K_UNLCREATE: return db_unlock_create(&g.db, c->mid, err);
    }
    return 0;
}

int char_work(void *ctx, Err err) {
    Ctx *c = (Ctx *)ctx;
    // 只有归属还是当前角色的，回读/回滚才有意义：用户已经切走，这次写属于上一个
    // 角色，拿它去动当前快照就是把别人的数据糊到眼前这个角色上
    int mine = c->cno == 0 || c->cno == g.cur.no;
    if (!char_do(ctx, err)) {
        // 写失败：内存里已经按「用户的意图」改过（乐观更新要抢先堵住旧值回填），
        // 现在得整段重读回滚，否则表单和左栏会停在那个根本没写进库的值上
        // （改名被拒最典型：界面上留着那个重名，看着像改成功了）
        if (mine && sync_after(c->kind)) app_reload_later();
        return 0;
    }
    if (mine && sync_after(c->kind)) app_sync_char();
    // 列表操作：暂存就绪，标脏等 UI 线程在 job_poll 后搬入
    if (c->kind == K_BANLIST) InterlockedOr(&s_lists_dirty, L_BANS);
    if (c->kind == K_BANCHARS) InterlockedOr(&s_lists_dirty, L_BANCHARS);
    if (c->kind == K_EVLIST)  InterlockedOr(&s_lists_dirty, L_EVENTS);
    return 1;
}
