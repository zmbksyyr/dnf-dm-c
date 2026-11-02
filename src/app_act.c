// app_act：通用异步动作 + 写队列。
//
// 一次只能有一个写操作在跑（共用一条 MySQL 连接）；忙时排队，完成后按顺序自动执行。
// 上下文一律拷贝一份再交给线程：调用方多半传共享的静态结构（gm 页的 kc、写容器时的
// g_warg），当场不改、线程稍后才读，中间被下一次点击改写就会出现「点 A 执行 B」。
// 容器写入的快照（WriteCtx）走堆 + 指针，不放进这里的固定缓冲：它带 6 个动态 Buf，
// 按值拷贝会让两个副本各认一份所有权。
#include "app.h"

#define PEND_BUF 768
#define PEND_N   4

typedef struct {
    ActFn fn;
    void *ctx;
    unsigned char buf[PEND_BUF];
} ActCtx;

static ActCtx g_act_ctx;

// 队列：单槽会被后来的动作直接覆盖——活动页连点「启用」再点「停用」，
// 第一个就静默消失了，状态栏还把两个都写成"…"，用户以为都提交了。
static struct {
    char   what[64];
    ActFn  fn;
    int    sz;
    unsigned char buf[PEND_BUF];
} g_pq[PEND_N];
static int g_pq_head, g_pq_n;

static int w_act(void *ctx, Err err) {
    ActCtx *a = (ActCtx *)ctx;
    return a->fn(a->ctx, err);
}

static int act_start(const char *what, ActFn fn, void *ctx, int ctxsz) {
    snprintf(g.act_what, sizeof(g.act_what), "%s", what);
    db_cancel_clear();                  // 新任务从「未被取消」开始
    app_job_pct_reset();
    g_act_ctx.fn = fn;
    if (ctx && ctxsz > 0 && ctxsz <= PEND_BUF) {
        memcpy(g_act_ctx.buf, ctx, (size_t)ctxsz);
        g_act_ctx.ctx = g_act_ctx.buf;
    } else {
        g_act_ctx.ctx = ctx;             // 无上下文：fn 自己不读
    }
    app_status("%s…", what);
    return job_start(&g.job_act, w_act, &g_act_ctx);
}

int app_act_n(const char *what, ActFn fn, void *ctx, int ctxsz) {
    // 加载 PVF 期间物品类型/附件种类都算不出来，这时提交写库会写错字段
    if (job_running(&g.job_pvf)) {
        app_status("PVF 正在加载，稍后再操作");
        return 0;
    }
    if (!g.db_ok) {
        app_status("请先连接数据库");
        return 0;
    }
    if (!app_db_busy() && !g_pq_n) return act_start(what, fn, ctx, ctxsz);
    if (ctxsz < 0 || ctxsz > PEND_BUF) return 0;
    if (g_pq_n >= PEND_N) {
        app_status("已有 %d 个操作在排队，这个没提交", g_pq_n);
        return 0;
    }
    int i = (g_pq_head + g_pq_n) % PEND_N;
    snprintf(g_pq[i].what, sizeof(g_pq[i].what), "%s", what);
    g_pq[i].fn = fn;
    // 上下文和上下文大小必须一起为空：app_act 的 ctxsz 是 sizeof(*(ctx))，
    // 传 NULL 时算出来是 sizeof(void)==1（GCC 扩展），照抄进队列就是
    // memcpy(..., NULL, 1) —— 全服扫描/清除这两个无上下文的动作一旦排队就崩。
    g_pq[i].sz = ctx ? ctxsz : 0;
    if (ctx && ctxsz > 0) memcpy(g_pq[i].buf, ctx, (size_t)ctxsz);
    g_pq_n++;
    return 1;
}

// 现在能不能直接发一个写动作：连接空闲，且队列是空的。
// 页面自己攒队列时用它判断，免得自己的动作排到队尾等太久
int app_act_idle(void) {
    return !app_db_busy() && !g_pq_n;
}

// 会话边界：丢掉还没开始的排队动作
void app_act_clear_queue(void) { g_pq_n = g_pq_head = 0; }

// 取队首（app_tick 在连接空出来后调用），同时出队
int app_act_take_queued(char *what, int what_cap, ActFn *fn, void **ctx, int *ctxsz) {
    if (!g_pq_n) return 0;
    int i = g_pq_head;
    g_pq_head = (g_pq_head + 1) % PEND_N;
    g_pq_n--;
    snprintf(what, (size_t)what_cap, "%s", g_pq[i].what);
    *fn = g_pq[i].fn;
    *ctxsz = g_pq[i].sz;
    *ctx = g_pq[i].sz > 0 ? g_pq[i].buf : NULL;
    return 1;
}

// 直接起一个排队中的动作（act_start 对调用方不可见，给它一个入口）
int app_act_run(const char *what, ActFn fn, void *ctx, int ctxsz) {
    return act_start(what, fn, ctx, ctxsz);
}
