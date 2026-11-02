// app_load：角色载入 / 载入后重载 / 选角。
//
// 载入写进 g_load_ctx.d 而不是直接写 g.data：db_load_data 第一件事就是
// db_free_data(out)（先 free 六个 blob 再重填）。直接对着 g.data 做，UI 线程在那一
// 瞬间正画着物品列表，读到的就是刚被释放的指针。用暂存区就没有这个问题——
// g.data 在交换之前一直是完整可用的旧快照。
#include "app.h"

typedef struct {
    Charac   c;
    CharData d;        // 载入到这里，成功那一刻整块换给 g.data
} LoadCtx;

static LoadCtx g_load_ctx;

static int s_load_want;                  // 想载入但连接正忙：等空闲了再发
static int s_load_pending = -1;          // 载入期间点过的角色：读完再接
static volatile LONG s_reload_later;     // 写失败的回滚请求（job 线程置，UI 线程取）
static volatile LONG s_load_recheck;     // 载入前先回读 charac_info（UI 线程置 1）

static int w_load(void *ctx, Err err) {
    LoadCtx *c = (LoadCtx *)ctx;
    // 回滚/重载要用库里的真值当快照：db_load_data 的 SELECT 里没有 charac_name
    // （lev/job/grow 也没有），光靠它重读一遍，名字永远回不来 —— 改名被拒时界面
    // 就一直停在那个没写进库的重名上。回读失败就维持传进来的那份：一条小查询
    // 不该把整个载入打挂。
    if (InterlockedExchange(&s_load_recheck, 0)) {
        Err e = {0};
        if (!db_read_charac(&g.db, c->c.no, &c->c, e))
            app_err("重载：回读角色失败，按内存快照载入：%s", e);
    }
    db_free_data(&c->d);                 // 上一轮失败留下的半成品
    return db_load_data(&g.db, &c->c, &c->d, err);
}

static void load_kick(void) {
    if (!s_load_want || app_db_busy()) return;
    s_load_want = 0;
    job_start(&g.job_load, w_load, &g_load_ctx);
}

void app_select_char(int idx) {
    if (idx < 0 || idx >= g.chars.n) return;
    if (job_running(&g.job_load)) {          // 上一轮还在读库：记下意图，读完再接
        s_load_pending = idx;
        return;
    }
    // 切换前把合并窗里的背包改动落盘。直接清零等于丢数据，而「先切再写」更糟：
    // blob 数据会被新角色的载入换掉，写进去的就是别人的背包。
    if (g.pend_mask) {
        g.pend_ms = 0;                       // 不等 200ms 合并窗
        for (int i = 0; i < 12 && g.pend_mask; i++) {
            app_tick();                      // 里面会尝试 flush
            Sleep(50);
        }
        if (g.pend_mask) {
            app_status("正在写库，稍后再切换角色");
            return;
        }
    }
    Charac *c = VEC_AT(&g.chars, Charac, idx);
    // 这里不碰 g.cur：快照换人放在载入成功那一刻，一次完成。
    // 提前把 g.cur 改掉，正在跑的写作业就会拿旧数据去写新角色。
    g.sel = idx;
    g.lists_ready = 0;
    g.char_ok = 0;                  // 数据到位前先锁住页面
    g_load_ctx.c = *c;
    app_status("正在载入 %s 的数据…", c->name);
    s_load_want = 1;
    load_kick();
}

void app_reload(void) {
    if (!g.char_ok) return;
    if (job_running(&g.job_load)) {          // 同上：别在作业读 g_load_ctx 时改它
        InterlockedExchange(&s_reload_later, 1);
        return;
    }
    // 写失败后的回滚要回到库里的真值，而不是内存里这份：改名/改等级是「先改快照、
    // 再写库」（gm_apply 的乐观更新），失败时 g.cur 里躺的正是那个没写进库的值，
    // 照它重读等于把假值再确认一次。所以让载入作业先回读 charac_info 再载入。
    g_load_ctx.c = g.cur;
    InterlockedExchange(&s_load_recheck, 1);
    s_load_want = 1;
    load_kick();
}

// 任务线程不能直接开作业（job_start 要跟 UI 线程抢同一份状态机），只留个请求
void app_reload_later(void) { InterlockedExchange(&s_reload_later, 1); }

// UI tick：上一轮点过、但当时载入在跑的角色，这一帧补上
void app_load_pending(void) {
    if (s_load_pending >= 0 && !job_running(&g.job_load)) {
        int next = s_load_pending;
        s_load_pending = -1;
        app_select_char(next);
    }
}

// UI tick：载入作业的结果。快照在这里整体换代——job_poll 已等到线程结束，
// 此刻没有并发读者，交换 Blob 指针（浅拷贝）是安全的。
void app_load_poll(void) {
    Err err;
    if (!job_poll(&g.job_load, err)) return;
    if (err[0]) {
        g.char_ok = 0;
        db_free_data(&g_load_ctx.d);      // 半成品不留在暂存区里
        app_status("载入角色失败：%s", err);
        return;
    }
    db_free_data(&g.data);
    g.data = g_load_ctx.d;
    memset(&g_load_ctx.d, 0, sizeof(g_load_ctx.d));   // 暂存区交出去，别再动它
    // 载入期间 g.cur 一直是旧角色，所以这期间完成的旧角色写操作
    // 也不会把数据糊到新角色头上。
    g.cur = g_load_ctx.c;
    if (g.sel >= 0 && g.sel < g.chars.n) {       // 在线状态以轮询为准
        Charac *lc = VEC_AT(&g.chars, Charac, g.sel);
        if (lc->no == g.cur.no) g.cur.online = lc->online;
    }
    g.char_ok = 1;
    g.data_ver++;
    int used = 0;
    for (int i = 0; i < g.data.inv.n; i++)
        if (!item_empty(&g.data.inv.items[i])) used++;
    app_status("已载入 %s · 物品栏 %d 件", g.cur.name, used);
}

// 连接空闲后补发挂起的载入
void app_load_kick_if_wanted(void) {
    if (s_load_want) load_kick();
}

// 断开/换库：挂起的意图与暂存区都作废
void app_load_drop(void) {
    s_load_want = 0;
    s_load_pending = -1;
    InterlockedExchange(&s_load_recheck, 0);
    db_free_data(&g_load_ctx.d);
}

// UI tick：写失败后请求的回滚重载
int app_load_take_reload(void) {
    return InterlockedExchange(&s_reload_later, 0) != 0;
}
