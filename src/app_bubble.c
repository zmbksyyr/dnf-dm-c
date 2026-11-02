// app_bubble：泡点（按间隔给目标人群发点券 / D点 / D币 / 物品）。
//
// 只算本工具开着的时间：关掉就停、重开从零计时，不补算——补算要给每个账号
// 记上次结算时刻，等于把服务端的账本搬到 GM 台上。
#include "app.h"
#include <time.h>

static u64 s_bubble_next;      // 下次结算时刻（0 = 还没开始计时）
static u32 s_bubble_paid;      // 上次命中的对象数
static u32 s_bubble_runs;      // 已结算次数
static int s_bub_live;         // 上一 tick 是否在结算中，用来抓「刚刚到期」这个边沿

// 最近计划 / 结算流水：界面上那张表就吃这 20 条。
// 一条「计划」是点一次应用（等级：等待发放），之后每次结算追加一条结果。
// 只留内存不落盘；要长期留档走「导出记录」。
#define BUB_PLAN_MAX 20
static BubblePlan s_plan[BUB_PLAN_MAX];
static int s_plan_n;
static int s_plan_ver;

static const char *bub_target_name(int t) {
    return t == 0 ? "仅在线" : t == 1 ? "全服" : t == 2 ? "指定角色" : "指定账号";
}

static void bub_plan_add(const char *tg, const char *what, const char *st) {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    if (s_plan_n < BUB_PLAN_MAX) s_plan_n++;
    if (s_plan_n > 1)
        memmove(&s_plan[1], &s_plan[0], sizeof(BubblePlan) * (size_t)(s_plan_n - 1));
    BubblePlan *p = &s_plan[0];
    snprintf(p->t, sizeof(p->t), "%02d:%02d:%02d", tm.wHour, tm.wMinute, tm.wSecond);
    snprintf(p->tg, sizeof(p->tg), "%s", tg && tg[0] ? tg : "—");
    snprintf(p->what, sizeof(p->what), "%s", what ? what : "");
    snprintf(p->st, sizeof(p->st), "%s", st ? st : "");
    s_plan_ver++;
}

int app_bubble_plan_count(void) { return s_plan_n; }
const BubblePlan *app_bubble_plan_at(int i) {
    if (i < 0 || i >= s_plan_n) return NULL;
    return &s_plan[i];
}
int app_bubble_plan_ver(void) { return s_plan_ver; }

static int w_bubble(void *ctx, Err err) {
    (void)ctx;
    u32 n = 0;
    BubbleCfg bc = {
        g.cfg.bubble_target, g.cfg.bubble_coin, g.cfg.bubble_dpoint, g.cfg.bubble_dcoin,
        g.cfg.bubble_item, g.cfg.bubble_itemnum, g.cfg.bubble_tno
    };
    if (!db_bubble_tick(&g.db, &bc, &n, err)) return 0;
    s_bubble_paid = n;
    s_bubble_runs++;
    return 1;
}

void app_bubble_apply(int on, int sec, int target, int coin, int dpoint, int dcoin,
                      u32 item, int itemnum, u32 tno, int hours) {
    if (sec < 5) sec = 5;             // 下限 5 秒：再快就是拿一条连接刷库
    if (sec > 86400) sec = 86400;
    if (coin < 0) coin = 0;
    if (dpoint < 0) dpoint = 0;
    if (dcoin < 0) dcoin = 0;
    if (itemnum < 0) itemnum = 0;
    if (target < 0 || target > 3) target = 0;
    if (hours < 0) hours = 0;
    if (hours > 8760) hours = 8760;   // 一年封顶：再大 until 会溢出到离谱的年份
    g.cfg.bubble_on = on ? 1 : 0;
    g.cfg.bubble_sec = sec;
    g.cfg.bubble_target = target;
    g.cfg.bubble_coin = coin;
    g.cfg.bubble_dpoint = dpoint;
    g.cfg.bubble_dcoin = dcoin;
    g.cfg.bubble_item = item;
    g.cfg.bubble_itemnum = itemnum;
    g.cfg.bubble_tno = tno;
    g.cfg.bubble_hours = hours;
    // 时长从「应用」这一刻起算，存绝对截止时刻；存「还剩几小时」的话，
    // 每次重开工具都得重新数，等于把 3 小时续成无数遍。
    g.cfg.bubble_until = (on && hours) ? (u64)time(NULL) + (u64)hours * 3600 : 0;
    s_bubble_next = 0;                // 从改的这一刻重新计时
    app_cfg_save();

    const char *tg = bub_target_name(target);
    char rw[128] = "";
    if (coin)   snprintf(rw + strlen(rw), sizeof(rw) - strlen(rw), "点券+%d ", coin);
    if (dpoint) snprintf(rw + strlen(rw), sizeof(rw) - strlen(rw), "D点+%d ", dpoint);
    if (dcoin)  snprintf(rw + strlen(rw), sizeof(rw) - strlen(rw), "D币+%d ", dcoin);
    if (item)   snprintf(rw + strlen(rw), sizeof(rw) - strlen(rw), "物品%d×%d ", item,
                         itemnum > 0 ? itemnum : 1);
    if (!rw[0]) snprintf(rw, sizeof(rw), "（无奖励）");
    const char *dur = hours == 0 ? "持续" : fmt_new("%d 小时", hours);
    app_status("泡点：%s · 每 %d 秒 · %s · %s · 时长 %s",
               on ? "已启用" : "已停用", sec, tg, rw, dur);
    app_log("泡点：%s · 每 %d 秒 · 目标%s · %s · 时长 %s",
            on ? "已启用" : "已停用", sec, tg, rw, dur);

    char desc[120];
    if (on) {
        if (hours) snprintf(desc, sizeof(desc), "每 %d 秒 · %s · %d 小时", sec, rw, hours);
        else       snprintf(desc, sizeof(desc), "每 %d 秒 · %s · 持续", sec, rw);
        bub_plan_add(tg, desc, "等待发放");
    } else {
        bub_plan_add(tg, "手动停用", "已停止");
    }
}

// 当前是否还在生效时长内（until==0 表示持续，不自动停）
static int bubble_in_window(void) {
    if (!g.cfg.bubble_until) return 1;
    return (u64)time(NULL) < g.cfg.bubble_until;
}

void app_bubble_status(char *out, int cap) {
    if (!g.cfg.bubble_on) {
        fmt(out, cap, "未启用");
        return;
    }
    char tw[48] = "";
    if (!g.cfg.bubble_until) {
        snprintf(tw, sizeof(tw), " · 持续");
    } else {
        u64 now = (u64)time(NULL);
        if (g.cfg.bubble_until <= now) {
            snprintf(tw, sizeof(tw), " · 已到时长");
        } else {
            u64 r = g.cfg.bubble_until - now;
            snprintf(tw, sizeof(tw), " · 剩余 %llu 小时 %llu 分",
                     (unsigned long long)(r / 3600), (unsigned long long)(r % 3600 / 60));
        }
    }
    if (!bubble_in_window()) {
        fmt(out, cap, "已启用 · 已到生效时长，停止结算%s", tw);
        return;
    }
    if (!g.db_ok) {
        fmt(out, cap, "已启用 · 等数据库连接后开始计时%s", tw);
        return;
    }
    u64 now = now_ms();
    int left = s_bubble_next > now ? (int)((s_bubble_next - now) / 1000) : 0;
    const char *tg = bub_target_name(g.cfg.bubble_target);
    if (!s_bubble_runs)
        fmt(out, cap, "已启用 · %s · %d 秒后首次结算%s", tg, left, tw);
    else if (s_bubble_paid)
        fmt(out, cap, "已启用 · %s · 上次 %u 个对象已加奖励 · %d 秒后下次%s",
                 tg, s_bubble_paid, left, tw);
    else
        fmt(out, cap, "已启用 · %s · 上次没有符合条件的对象 · %d 秒后下次%s", tg, left, tw);
}

// ---------------- 周期结算 ----------------
// 每 bubble_sec 秒按目标人群 + 多奖励给账号/角色加点。账在账号维度（D点/D币）
// 或角色维度（点券/物品），一个账号挂多个角色也只发一份。
void app_bubble_tick(void) {
    Err err;
    // 必须连接空闲、队列里也没人的操作 —— 不跟人抢那条连接
    int live = g.db_ok && g.cfg.bubble_on && bubble_in_window();
    // 刚到时长这一瞬间记一条；之后 in_window 一直是 false，不会重复记
    if (s_bub_live && !live && g.cfg.bubble_on && !bubble_in_window())
        bub_plan_add(bub_target_name(g.cfg.bubble_target), "已到生效时长，停止结算", "已到时长");
    s_bub_live = live;

    if (!live) {
        s_bubble_next = 0;
    } else if (app_act_idle()) {
        u64 now = now_ms();
        u64 step = (u64)g.cfg.bubble_sec * 1000;
        if (!s_bubble_next) {
            s_bubble_next = now + step;
        } else if (now >= s_bubble_next) {
            s_bubble_next = now + step;
            job_start(&g.job_bubble, w_bubble, NULL);
        }
    }
    if (job_poll(&g.job_bubble, err)) {
        const char *tg = bub_target_name(g.cfg.bubble_target);
        char d[88];
        if (err[0]) {
            app_log("泡点结算失败：%s", err);
            snprintf(d, sizeof(d), "%s", err);
            bub_plan_add(tg, d, "发放失败");
        } else if (s_bubble_paid) {
            app_log("泡点结算：%s · %u 个对象已加奖励", tg, s_bubble_paid);
            snprintf(d, sizeof(d), "%u 个对象已加奖励", s_bubble_paid);
            bub_plan_add(tg, d, "完成发放");
        } else {
            // 一个都没命中也要记：否则界面上看不出「到底跑了没有」
            bub_plan_add(tg, "没有符合条件的对象", "无人可发");
        }
    }
}

// ---------------- 导出 ----------------
static int bubble_export_file(const char *path, FILE *fout) {
    FILE *fin = fopen(path, "rb");
    if (!fin) return 0;
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof(line), fin)) {
        if (strstr(line, "泡点结算") || strstr(line, "泡点：")) {
            fputs(line, fout);
            n++;
        }
    }
    fclose(fin);
    return n;
}

static int cmp_logname(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void app_bubble_export(void) {
    char dir[MAX_PATH], outp[MAX_PATH], logp[MAX_PATH];
    app_log_path(logp, sizeof(logp));
    art_dir("export", dir, sizeof(dir));
    path_join(outp, sizeof(outp), dir, "泡点记录.txt");
    FILE *fout = fopen(outp, "wb");
    if (!fout) { app_info("导出失败：无法写入 泡点记录.txt"); return; }

    // 日志按天轮转：跨天的活动记录在昨天或更早的文件里，不能只读今天那份
    char logdir[MAX_PATH];
    snprintf(logdir, sizeof(logdir), "%s", logp);
    char *slash = strrchr(logdir, '\\');
    if (slash) *slash = 0;

    char names[32][MAX_PATH];
    int nn = 0;
    char pat[MAX_PATH];
    path_join(pat, sizeof(pat), logdir, "log-*.txt");
    WCHAR wpat[MAX_PATH];
    utf16(pat, wpat, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (nn >= COUNT_OF(names)) break;
            utf8(fd.cFileName, names[nn], MAX_PATH);
            nn++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)nn, sizeof(names[0]), cmp_logname);   // 文件名带日期，字典序即时间序

    int n = 0;
    for (int i = 0; i < nn; i++) {
        char full[MAX_PATH];
        path_join(full, sizeof(full), logdir, names[i]);
        n += bubble_export_file(full, fout);
    }
    fclose(fout);
    if (!nn) {
        app_info("没有可导出的泡点记录（日志目录为空）");
        return;
    }
    app_info(fmt_new("已导出 %d 条泡点记录到：\n%s", n, outp));
}
