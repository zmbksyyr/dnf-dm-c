// app 实现：配置、异步任务、连接/角色/数据/保存。
#include "app.h"

App g;

// 载入暂存区。断开连接、退出、载入完成这三处都要动它，所以实例放在全局区，
// 别藏在「角色」小节里——那边看到它的人容易以为它只跟选角色有关。
typedef struct {
    Charac   c;
    CharData d;        // 载入到这里，成功那一刻整块换给 g.data
} LoadCtx;
static LoadCtx g_load_ctx;

// ---------------- 配置 ----------------
static int ini_int(const WCHAR *sec, const WCHAR *key, int def, const WCHAR *path) {
    return (int)GetPrivateProfileIntW(sec, key, def, path);
}

static void ini_str(const WCHAR *sec, const WCHAR *key, char *out, int cap, const WCHAR *path) {
    WCHAR w[1024] = {0};
    GetPrivateProfileStringW(sec, key, L"", w, COUNT_OF(w), path);
    utf8(w, out, cap);
}

static void ini_set(const WCHAR *sec, const WCHAR *key, const char *val, const WCHAR *path) {
    WCHAR w[1024];
    utf16(val, w, COUNT_OF(w));
    WritePrivateProfileStringW(sec, key, w, path);
}

static void cfg_load(Config *c, const char *path) {
    WCHAR w[MAX_PATH];
    utf16(path, w, MAX_PATH);
    memset(c, 0, sizeof(*c));
    ini_str(L"db", L"host", c->host, sizeof(c->host), w);
    if (!c->host[0]) snprintf(c->host, sizeof(c->host), "127.0.0.1");
    c->port = ini_int(L"db", L"port", 3306, w);
    ini_str(L"db", L"user", c->user, sizeof(c->user), w);
    if (!c->user[0]) snprintf(c->user, sizeof(c->user), "game");
    ini_str(L"db", L"pwd", c->pwd, sizeof(c->pwd), w);
    if (!c->pwd[0]) snprintf(c->pwd, sizeof(c->pwd), "uu5!^%%jg");
    c->charset = ini_int(L"db", L"charset", 33, w);

    ini_str(L"pvf", L"path", c->pvf, sizeof(c->pvf), w);
    c->font = ini_int(L"ui", L"font", 9, w);
    c->poll_sec = ini_int(L"ui", L"poll_sec", 3, w);
    ini_str(L"ui", L"hotkey", c->hotkey, sizeof(c->hotkey), w);
    if (!c->hotkey[0]) snprintf(c->hotkey, sizeof(c->hotkey), "Tab");

    ini_str(L"grab", L"proc", c->grab_proc, sizeof(c->grab_proc), w);
    if (!c->grab_proc[0]) snprintf(c->grab_proc, sizeof(c->grab_proc), "DNF.exe");
    c->grab_auto = ini_int(L"grab", L"auto", 0, w);

    c->bubble_on = ini_int(L"bubble", L"on", 0, w);
    c->bubble_sec = ini_int(L"bubble", L"sec", 60, w);
    c->bubble_val = ini_int(L"bubble", L"val", 1, w);

    if (c->font < 8 || c->font > 14) c->font = 9;
    if (c->poll_sec < 2) c->poll_sec = 3;
}

static void cfg_save(const Config *c, const char *path) {
    WCHAR w[MAX_PATH];
    utf16(path, w, MAX_PATH);
    ini_set(L"db", L"host", c->host, w);
    ini_set(L"db", L"port", fmt_new("%d", c->port), w);
    ini_set(L"db", L"user", c->user, w);
    ini_set(L"db", L"pwd", c->pwd, w);
    ini_set(L"db", L"charset", fmt_new("%d", c->charset), w);
    ini_set(L"pvf", L"path", c->pvf, w);
    ini_set(L"ui", L"font", fmt_new("%d", c->font), w);
    ini_set(L"ui", L"poll_sec", fmt_new("%d", c->poll_sec), w);
    ini_set(L"ui", L"hotkey", c->hotkey, w);
    ini_set(L"grab", L"proc", c->grab_proc, w);
    ini_set(L"grab", L"auto", fmt_new("%d", c->grab_auto), w);
    ini_set(L"bubble", L"on", fmt_new("%d", c->bubble_on), w);
    ini_set(L"bubble", L"sec", fmt_new("%d", c->bubble_sec), w);
    ini_set(L"bubble", L"val", fmt_new("%d", c->bubble_val), w);
}

// ---------------- 日志/提示 ----------------
// 逐行 append 意味着每条日志都开/关一次文件；改成内存攒批，1s 或 8KB 落一次盘。
// 文件按天分：config/logs/log-20261030.txt，只留最近 7 天。
// s_log 被 job 线程（app_log）和 UI 线程（app_log_flush）同时访问，必须加锁。
static Str s_log;
static u64 s_log_ms;
static CRITICAL_SECTION s_log_cs;
static int s_log_cs_ok;

void app_log_path(char *out, int cap) {
    char dir[MAX_PATH], day[16];
    art_dir("logs", dir, sizeof(dir));
    now_str(day, sizeof(day), "%Y%m%d");
    snprintf(out, (size_t)cap, "%s\\log-%s.txt", dir, day);
}

void app_log_flush(void) {
    if (!s_log_cs_ok) return;
    EnterCriticalSection(&s_log_cs);
    if (s_log.n <= 0) { LeaveCriticalSection(&s_log_cs); return; }
    char path[MAX_PATH];
    app_log_path(path, sizeof(path));
    file_append(path, s_log.p, s_log.n);
    s_log.n = 0;
    if (s_log.p) s_log.p[0] = 0;
    s_log_ms = now_ms();
    LeaveCriticalSection(&s_log_cs);
}

void app_log(const char *fmt, ...) {
    char msg[1024], line[1200], ts[32];
    va_list ap;
    va_start(ap, fmt);
    fmtv(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    now_str(ts, sizeof(ts), "%H:%M:%S");
    snprintf(line, sizeof(line), "[%s] %s\r\n", ts, msg);
    if (s_log_cs_ok) EnterCriticalSection(&s_log_cs);
    str_cat(&s_log, line);
    int full = s_log.n >= 8192;
    if (s_log_cs_ok) LeaveCriticalSection(&s_log_cs);
    if (full) app_log_flush();
}

void app_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fmtv(g.status, sizeof(g.status), fmt, ap);
    va_end(ap);
}

// 多行文本按行写进日志：整块塞进一条的话，日志页里就是一根读不动的长行，
// 而且前面只有一个时间戳，看不出哪些是并列的条目
void app_log_block(const char *s) {
    char line[512];
    while (s && *s) {
        const char *e = strstr(s, "\r\n");
        int n = e ? (int)(e - s) : (int)strlen(s);
        if (n > 0) {
            if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            memcpy(line, s, (size_t)n);
            line[n] = 0;
            app_log("%s", line);
        }
        if (!e) break;
        s = e + 2;
    }
}

int app_confirm(const char *text) {
    WCHAR w[1024], t[128] = L"DNF GM 工具";
    utf16(text, w, COUNT_OF(w));
    return MessageBoxW(GetActiveWindow(), w, t, MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

void app_info(const char *text) {
    WCHAR w[1024], t[128] = L"DNF GM 工具";
    utf16(text, w, COUNT_OF(w));
    MessageBoxW(GetActiveWindow(), w, t, MB_OK | MB_ICONINFORMATION);
}

// ---------------- 任务 ----------------
static DWORD WINAPI job_thread(LPVOID param) {
    Job *j = (Job *)param;
    Err err = {0};
    int ok = j->fn(j->ctx, err);
    if (ok) {
        j->err[0] = 0;
        InterlockedExchange(&j->state, 2);
    } else {
        snprintf(j->err, ERRBUF, "%s", err[0] ? err : "未知错误");
        InterlockedExchange(&j->state, 3);
    }
    return 0;
}

int job_running(const Job *j) { return j->state == 1; }

int job_start(Job *j, int (*fn)(void *ctx, Err err), void *ctx) {
    if (j->state == 1) return 0;                 // 还在跑：不能重入
    if (j->state == 2 || j->state == 3) {
        // 上一轮跑完了但没人 poll。不先收掉的话，下面 CloseHandle 会连同那份
        // 结果一起扔掉——错误信息就这么静默消失。按现有调用方不该走到这里，
        // 真走到这里说明有人漏了 poll，至少别让结论也跟着丢
        Err drop;
        job_poll(j, drop);
    }
    if (j->th) { CloseHandle(j->th); j->th = NULL; }
    j->fn = fn;
    j->ctx = ctx;
    j->progress = 0;
    j->stage[0] = 0;
    j->err[0] = 0;
    InterlockedExchange(&j->state, 1);
    j->th = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (!j->th) {
        InterlockedExchange(&j->state, 3);
        snprintf(j->err, ERRBUF, "创建线程失败");
        return 0;
    }
    return 1;
}

int job_poll(Job *j, Err err) {
    LONG st = j->state;
    if (st != 2 && st != 3) return 0;
    if (j->th) {
        WaitForSingleObject(j->th, INFINITE);
        CloseHandle(j->th);
        j->th = NULL;
    }
    snprintf(err, ERRBUF, "%s", j->err);       // err 是数组参数，不可能为 NULL
    InterlockedExchange(&j->state, 0);
    return 1;
}

// 共用同一条 MySQL 连接的作业：协议是严格串行的，任一在跑，别的必须等。
// 只互斥 app_act 是不够的——轮询/载入撞上写库会在同一连接上交错收发，包就串了
static int db_busy(void) {
    return job_running(&g.job_conn) || job_running(&g.job_load) ||
           job_running(&g.job_act) || job_running(&g.job_poll) ||
           job_running(&g.job_bubble);
}

void job_stop(Job *j) {
    if (j->th) {
        WaitForSingleObject(j->th, 30000);
        CloseHandle(j->th);
        j->th = NULL;
    }
    InterlockedExchange(&j->state, 0);
}

// ---------------- 页面 ----------------
Blob *app_page_blob(int page) {
    switch (page) {
        case 0: return &g.data.inv;
        case 1: return &g.data.equip;
        case 2: return &g.data.creature;
        case 3: return &g.data.cargo;
        case 4: return &g.data.jewel;
        case 5: return &g.data.account_cargo;
        default: return NULL;
    }
}

const char *app_page_table(int page) {
    switch (page) {
        case 0:
        case 1:
        case 2: return "taiwan_cain_2nd.inventory";
        case 3:
        case 4: return "taiwan_cain_2nd.charac_inven_expand";
        default: return "taiwan_cain.account_cargo";
    }
}

const char *app_page_field(int page) {
    switch (page) {
        case 0: return "inventory";
        case 1: return "equipslot";
        case 2: return "creature";
        case 3: return "cargo";
        case 4: return "jewel";
        default: return "cargo";
    }
}

// ---------------- 即时落库 ----------------
// 页面改完立刻写库（不再有「保存改动」按钮，也不攒草稿）：
// 内存先改，200ms 内的多次编辑合并成一次写，写完再刷新界面。
void app_pending_page(int page) {
    if (page < 0 || page > 5 || !g.char_ok) return;
    Blob *b = app_page_blob(page);
    if (!b || !b->ok) return;
    if (g.cur.online) { app_status("角色在线，改动未写入——请先退出游戏"); return; }
    g.pend_mask |= 1u << page;
    g.pend_ms = now_ms();
}

void app_item_set(int page, int slot, const Item *it) {
    if (page < 0 || page > 5 || !g.char_ok || !it) return;
    Blob *b = app_page_blob(page);
    if (!b || !b->ok || slot < 0 || slot >= b->n) return;
    if (g.cur.online) { app_status("角色在线，改动未写入——请先退出游戏"); return; }
    b->items[slot] = *it;
    g.pend_mask |= 1u << page;
    g.pend_ms = now_ms();
}

int app_page_busy(int page) {
    if (page < 0 || page > 5) return 0;
    return (int)((g.pend_mask | g.write_mask) & (1u << page)) != 0;
}

typedef struct {
    u32 mask;
    u32 cno;      // 归属角色：写 6 个容器要好几百毫秒，期间用户可能已经切了角色
    u32 mid;
} WriteCtx;
static WriteCtx g_wctx;

static int w_write(void *ctx, Err err) {
    WriteCtx *c = (WriteCtx *)ctx;
    // 只认入队时记下的编号。若在任务线程里读 g.cur，用户中途切角色就会
    // 前几个容器写进 A、后几个写进 B —— 数据污染，比写失败严重得多。
    Charac who;
    memset(&who, 0, sizeof(who));
    who.no = c->cno;
    who.mid = c->mid;
    int n = 0;
    for (int p = 0; p < 6; p++) {
        if (!(c->mask & (1u << p))) continue;
        Blob *b = app_page_blob(p);
        if (!b || !b->ok) continue;
        Buf packed = {0};
        if (!blob_pack(b, &packed)) {
            buf_free(&packed);
            snprintf(err, ERRBUF, "打包失败");
            return 0;
        }
        int ok = db_save_blob(&g.db, &who, app_page_table(p), app_page_field(p), packed.p,
                              packed.n, err);
        buf_free(&packed);
        if (!ok) return 0;
        n++;
    }
    g.write_done = c->mask;
    return 1;
}

void app_flush_pending(void) {
    if (!g.pend_mask || !g.db_ok || !g.char_ok) return;
    if (db_busy()) return;                       // 连接被别的作业占着：等它让出来
    if (now_ms() - g.pend_ms < 200) return;      // 合并连续编辑
    u32 m = g.pend_mask;
    g.pend_mask = 0;
    g.write_mask = m;
    g_wctx.mask = m;
    g_wctx.cno = g.cur.no;
    g_wctx.mid = g.cur.mid;
    if (!app_act("写入容器", w_write, &g_wctx)) {
        g.pend_mask |= m;
        g.write_mask = 0;
    }
}

// ---------------- 展示 ----------------
const char *app_item_name(u32 id, char *buf, int cap) {
    if (g.pvf_ok) {
        const char *n = pvf_item_name(&g.pvf, id);
        if (*n) return n;
    }
    snprintf(buf, (size_t)cap, "#%u", id);
    return buf;
}

const char *app_job_text(int job) {
    static const char *kJ[] = {"未转职", "鬼剑士", "格斗家", "神枪手", "魔法师", "圣职者",
                               "盗贼",   "男法师", "缔造者", "黑暗武士", "影舞者"};
    if (g.pvf_ok) {
        const char *n = pvf_job_name(&g.pvf, job);
        if (*n) return n;
    }
    if (job >= 0 && job < COUNT_OF(kJ)) return kJ[job];
    return "未知职业";
}

// PVP 段位名：与参考实现（PVPRankList）一致
// 0=10级…9=1级，10=1段…19=10段，20=至尊1…29=至尊10，30~34 达人/名人/小霸王/霸王/斗神
const char *app_pvp_text(int grade) {
    static const char *kTop[] = {"达人", "名人", "小霸王", "霸王", "斗神"};
    static char buf[32];
    if (grade < 0 || grade > 34) {
        snprintf(buf, sizeof(buf), "段位 %d", grade);
        return buf;
    }
    if (grade <= 9) snprintf(buf, sizeof(buf), "%d级", 10 - grade);
    else if (grade <= 19) snprintf(buf, sizeof(buf), "%d段", grade - 9);
    else if (grade <= 29) snprintf(buf, sizeof(buf), "至尊%d", grade - 19);
    else snprintf(buf, sizeof(buf), "%s", kTop[grade - 30]);
    return buf;
}

void app_money_text(u64 v, char *buf, int cap) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)v);
    int n = (int)strlen(raw), o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        if (i && (n - i) % 3 == 0 && o < cap - 2) buf[o++] = ',';
        buf[o++] = raw[i];
    }
    buf[o] = 0;
}

// ---------------- 连接 ----------------
static int w_connect(void *ctx, Err err) {
    (void)ctx;
    if (!db_open(&g.db, g.cfg.host, g.cfg.port, g.cfg.user, g.cfg.pwd, g.cfg.charset, err)) return 0;
    // 连上先自查一遍表结构。私服之间差得很多（少表、少列、把 LONGBLOB 建成 BLOB），
    // 以前这些差异都表现为「改完弹回去」；现在一次说清楚，写进日志。
    // 自检本身的失败不算连接失败——查不了结构不该让人连不上库。
    g.self_bad = db_selfcheck(&g.db, g.self, (int)sizeof(g.self), err);
    err[0] = 0;
    if (!db_list_chars(&g.db, &g.chars, 200, "", err)) return 0;
    g.char_total = db_count_chars(&g.db, "");
    g.chars_ver++;
    return 1;
}

typedef struct {
    char kw[64];
} KwCtx;
static KwCtx g_kw_ctx;

static int w_chars(void *ctx, Err err) {
    KwCtx *k = (KwCtx *)ctx;
    if (!db_list_chars(&g.db, &g.chars, 200, k->kw, err)) return 0;
    g.char_total = db_count_chars(&g.db, k->kw);
    g.chars_ver++;
    return 1;
}

void app_list_chars(const char *kw) {
    if (!g.db_ok) return;
    snprintf(g_kw_ctx.kw, sizeof(g_kw_ctx.kw), "%s", kw ? kw : "");
    app_act("刷新角色", w_chars, &g_kw_ctx);
}

// 轮询：一条查询拿回全部角色的最新状态（新建/改名/升级/在线都会反映）。
// 它是「左栏」的唯一数据源 —— 列表、在线人数、角色卡都只看 g.chars，
// 所以这里只更新 g.chars 让列表重排，绝不碰 data_ver：g.cur/g.data 是当前角色的
// 快照，玩家自己在游戏里升级、GM 页正开着的表单都不该被这几秒一次的轮询冲掉。
static Vec s_poll;

static int chars_differs(const Vec *a, const Vec *b) {
    if (a->n != b->n) return 1;
    for (int i = 0; i < a->n; i++) {
        const Charac *x = (const Charac *)vec_at((Vec *)a, i);
        const Charac *y = (const Charac *)vec_at((Vec *)b, i);
        if (x->no != y->no || x->mid != y->mid || x->lev != y->lev || x->job != y->job ||
            x->grow != y->grow || x->online != y->online || strcmp(x->name, y->name) != 0 ||
            strcmp(x->account, y->account) != 0)
            return 1;
    }
    return 0;
}

static int w_poll(void *ctx, Err err) {
    (void)ctx;
    if (!s_poll.cap) vec_init(&s_poll, sizeof(Charac));
    if (!db_poll_chars(&g.db, &s_poll, 500, err)) return 1;   // 轮询失败不打扰界面
    if (!chars_differs(&s_poll, &g.chars)) return 1;
    Vec t = g.chars;
    g.chars = s_poll;
    s_poll = t;                    // 交换缓冲，省一次整体拷贝
    if (g.chars.n < 500) g.char_total = (u32)g.chars.n;
    // 列表可能插了新角色/少了角色，选中索引跟着角色编号重新定位
    g.sel = -1;
    for (int i = 0; i < g.chars.n; i++) {
        Charac *c = VEC_AT(&g.chars, Charac, i);
        if (g.char_ok && c->no == g.cur.no) {
            g.sel = i;
            // 在线状态是写库的闸门（游戏在线时改库会被它覆盖回去），必须实时；
            // 等级/职业的实时值留在 g.chars 里给角色卡用，快照 g.cur 不动。
            g.cur.online = c->online;
            break;
        }
    }
    g.chars_ver++;
    return 1;
}

void app_connect(void) {
    if (job_running(&g.job_conn)) return;
    g.lists_ready = 0;
    g.db_ok = 0;
    g.char_ok = 0;
    g.sel = -1;
    vec_clear(&g.chars);
    app_status("正在连接数据库…");
    job_start(&g.job_conn, w_connect, NULL);
}

void app_disconnect(void) {
    if (g.db_ok) db_close(&g.db);
    g.lists_ready = 0;
    g.db_ok = 0;
    g.char_ok = 0;
    g.sel = -1;
    g.pend_mask = g.write_mask = 0;
    vec_clear(&g.chars);
    db_free_data(&g.data);
    db_free_data(&g_load_ctx.d);      // 载入到一半断开时，暂存区里还有东西
    app_status("已断开数据库");
}

// ---------------- PVF ----------------
typedef struct {
    char path[MAX_PATH];
} PvfCtx;
static PvfCtx g_pvf_ctx;

static int w_pvf(void *ctx, Err err) {
    PvfCtx *c = (PvfCtx *)ctx;
    PvfProg prog = {&g.pvf_pct, g.pvf_stage, (int)sizeof(g.pvf_stage)};
    (void)err;
    return pvf_open(&g.pvf, c->path, prog);
}

void app_load_pvf(const char *path) {
    if (job_running(&g.job_pvf)) return;
    snprintf(g_pvf_ctx.path, sizeof(g_pvf_ctx.path), "%s", path);
    snprintf(g.cfg.pvf, sizeof(g.cfg.pvf), "%s", path);
    g.pvf_pct = 0;
    snprintf(g.pvf_stage, sizeof(g.pvf_stage), "准备中");
    app_status("正在加载 PVF…");
    job_start(&g.job_pvf, w_pvf, &g_pvf_ctx);
}

// ---------------- 取码 ----------------
// 作业上下文：wait_ms>0 = 手动「取码」（等悬停回来，见 grab_hit_wait）；
// 0 = 连取的快速轮次（只试一次特征码，没悬停就静默跳过）。
// 用 static 单例而不是栈上变量：job_start 只存指针，作业跑完才读它。
#define GRAB_WAIT_MS 5000        // 点「取码」后等用户把鼠标放回游戏的时长
typedef struct { int wait_ms; } GrabCtx;
static GrabCtx s_grab_ctx;
static int s_grab_pending;       // 连取轮次占着作业线程时点了「取码」：补发

static int w_grab(void *ctx, Err err) {
    GrabCtx *c = (GrabCtx *)ctx;
    return c && c->wait_ms > 0 ? grab_hit_wait(&g.grab, c->wait_ms, err)
                               : grab_hit_fast(&g.grab, err);
}

void app_grab_once(void) {
    if (job_running(&g.job_grab)) {
        // 正在等悬停（用户又点了一次）：等的就是这件事，不用再排
        if (s_grab_ctx.wait_ms > 0) return;
        // 被连取的快轮次占着（每 250ms 一次，单次十几毫秒）：记下来，
        // 作业一空就补发——点了没反应比慢半秒更难琢磨
        s_grab_pending = 1;
        app_status("取码：把鼠标放回游戏，悬停到目标物品上（%d 秒内）…", GRAB_WAIT_MS / 1000);
        return;
    }
    // 点击会把鼠标带离游戏，悬停槽当场失效——所以这里不是"采一次"而是"等一会儿"：
    // 用户把鼠标放回游戏里的物品上，最多 5 秒内自动采到，编号落到发放页。
    app_status("取码：把鼠标放回游戏，悬停到目标物品上（%d 秒内）…", GRAB_WAIT_MS / 1000);
    s_grab_ctx.wait_ms = GRAB_WAIT_MS;
    job_start(&g.job_grab, w_grab, &s_grab_ctx);
}

void app_grab_fast_once(void) {
    if (job_running(&g.job_grab)) return;
    s_grab_ctx.wait_ms = 0;
    job_start(&g.job_grab, w_grab, &s_grab_ctx);
}

void app_grab_auto(int on) {
    on = on ? 1 : 0;
    if (on == g.cfg.grab_auto) return;
    g.cfg.grab_auto = on;
    cfg_save(&g.cfg, g.cfg_path);
    if (on) {
        app_status("连取：已开启 · 在游戏里把鼠标放到物品上，会自动入队发放列表");
        app_log("连取：已开启 · 悬停到的物品整行入队，队列里已有的编号跳过");
    } else {
        app_status("连取：已关闭");
        app_log("连取：已关闭");
    }
}

// ---------------- 角色 ----------------
static int s_load_want;        // 想载入但连接正忙：等空闲了再发
static int s_reload_later;     // 任务线程要求重读（写失败回滚），由 app_tick 代劳

// 载入写进 g_load_ctx.d 而不是直接写 g.data：
// db_load_data 第一件事就是 db_free_data(out)，也就是先 free 掉 6 个 blob 再重填。
// 直接对着 g.data 做，UI 线程在那一瞬间正在画物品列表，读到的就是刚被释放的指针
// （现在之所以没崩，只是因为 blob_unpack 把 ok=1 放在最后一句，属于撞对了顺序）。
// 用一块暂存区就没有这个问题——g.data 在交换之前一直是完整可用的旧快照。
static int w_load(void *ctx, Err err) {
    LoadCtx *c = (LoadCtx *)ctx;
    db_free_data(&c->d);                 // 上一轮失败留下的半成品
    return db_load_data(&g.db, &c->c, &c->d, err);
}

static void load_kick(void) {
    if (!s_load_want || db_busy()) return;
    s_load_want = 0;
    job_start(&g.job_load, w_load, &g_load_ctx);
}

void app_select_char(int idx) {
    if (idx < 0 || idx >= g.chars.n) return;
    // 切换前把合并窗里的背包改动落盘。直接清零等于丢数据，而"先切再写"更糟：
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
    g_load_ctx.c = g.cur;
    s_load_want = 1;
    load_kick();
}

// 任务线程不能直接开作业（job_start 要跟 UI 线程抢同一份状态机），只留个请求
void app_reload_later(void) { s_reload_later = 1; }

// ---------------- 写后校验 ----------------
// 写库之后把真值读回来，跟内存里的快照比一次（两条小查询，只在真写库后跑一次）：
//   一致（绝大多数）→ 一个字节都不动界面。表单上是用户刚敲的值，左栏那一行也
//     已经是同一个值，重填/重绘纯属白做功，还可能把正在编辑的框打断。
//   不一致        → 写入没生效或被库改写（列写错、值被截断、触发器改过）：
//     采纳库里的真值，并让快照换代，页面重填一次。界面暴露问题，而不是替库撒谎。
// 返回 1 表示「跟缓存一致」，调用方无需再做任何界面动作。
// 三层比较，对应"变了以后该刷哪里"：
//   same_char  —— 名字/等级/职业/觉醒：左栏那一行和角色卡要跟着改
//   same_form  —— GM 表单会填进控件的那些数：变了才需要重填表单
//   same_rest  —— SP/QP/仓库容量等只挂在角色卡上的数：改了也不用发任何通知
static int same_char(const Charac *a, const Charac *b) {
    return a->lev == b->lev && a->job == b->job && a->grow == b->grow &&
           strcmp(a->name, b->name) == 0;
}

static int same_form(const CharData *a, const CharData *b) {
    return a->money == b->money && a->coin == b->coin && a->cera == b->cera &&
           a->cera_point == b->cera_point && a->account_money == b->account_money &&
           a->pvp_grade == b->pvp_grade && a->pvp_win == b->pvp_win &&
           a->pvp_point == b->pvp_point;
}

static int same_rest(const CharData *a, const CharData *b) {
    return a->cargo_cap == b->cargo_cap && a->sp == b->sp && a->sp2 == b->sp2 &&
           a->tp == b->tp && a->tp2 == b->tp2 && a->qp == b->qp;
}

// 左栏那一行跟着快照走：改名/改等级之后列表上还是旧值，看着像没生效。
// 提交时先按「用户的意图」改一次（乐观更新，左栏和卡片立刻跟上），
// 写库后的校验再按库里的真值改一次；值一样时这里什么都不做。
void app_row_sync(void) {
    if (g.sel < 0 || g.sel >= g.chars.n) return;
    Charac *lc = VEC_AT(&g.chars, Charac, g.sel);
    if (lc->no != g.cur.no) return;
    if (lc->lev == g.cur.lev && lc->job == g.cur.job && lc->grow == g.cur.grow &&
        strcmp(lc->name, g.cur.name) == 0)
        return;
    lc->lev = g.cur.lev;
    lc->job = g.cur.job;
    lc->grow = g.cur.grow;
    snprintf(lc->name, sizeof(lc->name), "%s", g.cur.name);
    g.chars_ver++;                     // 左栏列表重排：只此一条，代价最小
}

int app_sync_char(void) {
    if (!g.db_ok || !g.char_ok) return 0;
    Err err = {0};
    Charac old_c = g.cur;
    CharData old_d = g.data;               // 浅拷贝：只为比较标量，blob 指针原样不动
    Charac c = g.cur;                      // no/mid/account 原样保留
    CharData d = g.data;
    if (!db_read_charac(&g.db, g.cur.no, &c, err)) {
        app_log("写后校验失败（角色）：%s", err);
        return 0;
    }
    if (!db_read_state(&g.db, &c, &d, err)) {
        app_log("写后校验失败（状态）：%s", err);
        return 0;
    }
    c.online = g.cur.online;               // 在线状态归轮询管，别被这次快照覆盖回去

    if (same_char(&old_c, &c) && same_form(&old_d, &d) && same_rest(&old_d, &d)) {
        app_row_sync();                    // 写入跟预期一致：界面一个字都不用动
        return 1;
    }
    // 库里的值跟内存里的不一样：以库为准
    int form_changed = !same_char(&old_c, &c) || !same_form(&old_d, &d);
    g.cur = c;
    g.data = d;    // 标量换成真值；blob 指针没变（d 本来就是 g.data 的浅拷贝）
    if (form_changed) {
        g.data_ver++;                      // 快照换代 → 表单重填一次
        app_log("写后校验：库里的值跟写入的不一致，已按库里的值刷新");
    }
    app_row_sync();
    return 0;
}

void app_save_draft(void) {
    // 已改为即时落库：这里只做一次「立刻写」，供关窗/切角色前调用
    g.pend_ms = 0;
    app_flush_pending();
}

// ---------------- 通用异步动作 ----------------
// 上下文一律拷贝一份再交给线程。调用方多半传的是共享的静态结构（gm 页的 kc、
// 物品页的 g_wctx），当场不改、任务线程稍后才读，中间被下一次点击改写，
// 就会出现"点 A 执行 B"这种查不出来的错。
#define PEND_BUF 768
#define PEND_N   4
typedef struct {
    ActFn fn;
    void *ctx;
    unsigned char buf[PEND_BUF];
} ActCtx;
static ActCtx g_act_ctx;

static int w_act(void *ctx, Err err) {
    ActCtx *a = (ActCtx *)ctx;
    return a->fn(a->ctx, err);
}

// 一次只能有一个写操作在跑（共用一条 MySQL 连接）；忙时排队，完成后按顺序自动执行。
// 单槽会被后来的动作直接覆盖：活动页连点「启用」再点「停用」，第一个就静默消失了，
// 状态栏还把两个都写成"…"，用户以为都提交了。
static struct {
    char   what[64];
    ActFn  fn;
    int    sz;
    unsigned char buf[PEND_BUF];
} g_pq[PEND_N];
static int g_pq_head, g_pq_n;            // 环形：head 出队，n 排队数

static int act_start(const char *what, ActFn fn, void *ctx, int ctxsz) {
    snprintf(g.act_what, sizeof(g.act_what), "%s", what);
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
    if (!g.db_ok) {
        app_status("请先连接数据库");
        return 0;
    }
    if (!db_busy() && !g_pq_n) return act_start(what, fn, ctx, ctxsz);
    if (ctxsz < 0 || ctxsz > PEND_BUF) return 0;
    if (g_pq_n >= PEND_N) {
        app_status("已有 %d 个操作在排队，这个没提交", g_pq_n);
        return 0;
    }
    int i = (g_pq_head + g_pq_n) % PEND_N;
    snprintf(g_pq[i].what, sizeof(g_pq[i].what), "%s", what);
    g_pq[i].fn = fn;
    g_pq[i].sz = ctxsz;
    if (ctxsz > 0) memcpy(g_pq[i].buf, ctx, (size_t)ctxsz);
    g_pq_n++;
    return 1;
}

// 现在能不能直接发一个写动作：连接空闲，且队列是空的。
// 页面自己攒队列时用它判断，免得自己的动作排到队尾等太久
int app_act_idle(void) {
    return !db_busy() && !g_pq_n;
}

// ---------------- 发放 ----------------
void app_grant_add(u32 id) {
    char buf[64];
    const char *nm = app_item_name(id, buf, sizeof(buf));
    for (int i = 0; i < g.grant.n; i++) {
        NamedId *n = VEC_AT(&g.grant, NamedId, i);
        if (n->id == id) {
            n->num++;
            app_status("已加入队列：%s ×%u", n->name, n->num);
            return;
        }
    }
    NamedId *n = VEC_ADD(&g.grant, NamedId);
    n->id = id;
    n->num = 1;
    n->type = IT_STACK;
    snprintf(n->name, sizeof(n->name), "%s", nm);
    app_status("已加入队列：%s", nm);
}

// 发放队列快照：w_grant 在 job 线程跑，g.grant 在 UI 线程可被增删——直接遍历
// g.grant 是竞态（realloc 后指针悬空 / 删项后越界）。发前拷一份，job 只读快照。
// 快照放独立静态数组（不在 GrantCtx 里）：GrantCtx 要塞进 app_act 的 768B 队列缓冲，
// 带不下 256 项物品。一次只发一批，所以一份快照就够。
#define GRANT_SNAP_MAX 256
static NamedId s_grant_snap[GRANT_SNAP_MAX];
static int     s_grant_snap_n;
typedef struct {
    int by_mail;
} GrantCtx;
static GrantCtx g_grant_ctx;

static int w_grant(void *ctx, Err err) {
    GrantCtx *c = (GrantCtx *)ctx;
    if (c->by_mail) {
        for (int i = 0; i < s_grant_snap_n; i++) {
            NamedId *n = &s_grant_snap[i];
            GrantItem gi;
            memset(&gi, 0, sizeof(gi));
            gi.id = n->id;
            gi.num = n->num;
            gi.type = n->type;
            gi.enhance = n->enhance;
            gi.forge = n->forge;
            gi.seal = n->seal;
            gi.seal_cnt = n->seal_cnt;
            gi.inc_type = n->inc_type;
            gi.inc_val = n->inc_val;
            gi.dur = n->dur;
            if (!db_send_item_mail_ex(&g.db, &g.cur, "GM", "系统发放", &gi, err)) return 0;
        }
        return 1;
    }
    Blob *b = &g.data.inv;
    if (!b->ok) { snprintf(err, ERRBUF, "背包未载入"); return 0; }
    for (int i = 0; i < s_grant_snap_n; i++) {
        NamedId *n = &s_grant_snap[i];
        int slot = -1;
        for (int k = 0; k < b->n; k++)
            if (item_empty(&b->items[k])) { slot = k; break; }
        if (slot < 0) { snprintf(err, ERRBUF, "背包已满"); return 0; }
        Item it;
        memset(&it, 0, sizeof(it));
        item_set_type(&it, n->type ? n->type : IT_STACK);
        item_set_id(&it, n->id);
        item_set_num(&it, n->num);
        item_set_enhance(&it, n->enhance);
        item_set_forge(&it, n->forge);
        item_set_seal(&it, n->seal);
        item_set_seal_cnt(&it, n->seal_cnt);
        item_set_inc_type(&it, n->inc_type);
        item_set_inc_value(&it, n->inc_val);
        item_set_durability(&it, n->dur);
        b->items[slot] = it;
    }
    Buf packed = {0};
    if (!blob_pack(b, &packed)) { buf_free(&packed); snprintf(err, ERRBUF, "打包失败"); return 0; }
    int ok = db_save_blob(&g.db, &g.cur, "taiwan_cain_2nd.inventory", "inventory", packed.p, packed.n, err);
    buf_free(&packed);
    return ok;
}

int app_grant_send(int by_mail) {
    if (!g.char_ok) { app_info("请先选择角色"); return 0; }
    if (g.grant.n == 0) { app_info("发放队列为空"); return 0; }
    if (g.grant.n > GRANT_SNAP_MAX) {
        app_info(fmt_new("队列超过 %d 项，请分批发放", GRANT_SNAP_MAX));
        return 0;
    }
    if (job_running(&g.job_act)) {
        app_info("上一个操作还在进行，等它完成再发");
        return 0;
    }
    if (!by_mail && db_is_online(&g.db, g.cur.mid)) {
        app_info("角色在线，请改用「邮件」发放。");
        return 0;
    }
    if (!app_confirm(by_mail ? fmt_new("把 %d 种物品发到 %s 的邮箱？", g.grant.n, g.cur.name)
                             : fmt_new("把 %d 种物品直接放进 %s 的背包？", g.grant.n, g.cur.name)))
        return 0;
    s_grant_snap_n = (int)g.grant.n;
    memcpy(s_grant_snap, g.grant.p, sizeof(NamedId) * (size_t)g.grant.n);
    g_grant_ctx.by_mail = by_mail;
    return app_act(by_mail ? "邮件发放" : "直接入包", w_grant, &g_grant_ctx);
}

// ---------------- 整库备份 / 还原 ----------------
typedef struct {
    char path[MAX_PATH];
} DbJobCtx;
static DbJobCtx g_dbc;

static int w_db_dump(void *ctx, Err err) {
    DbJobCtx *c = (DbJobCtx *)ctx;
    int nt = 0;
    u64 nr = 0;
    if (!db_dump_sql(&g.db, c->path, &nt, &nr, err)) return 0;
    app_log("备份数据库 · %d 张表 / %llu 行 → %s", nt, (unsigned long long)nr, c->path);
    return 1;
}

static int w_db_restore(void *ctx, Err err) {
    DbJobCtx *c = (DbJobCtx *)ctx;
    int n = 0;
    if (!db_restore_sql(&g.db, c->path, &n, err)) return 0;
    app_log("还原数据库 · 执行 %d 条语句 ← %s", n, c->path);
    g.lists_ready = 0;
    return 1;
}

void app_db_backup(const char *path) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    snprintf(g_dbc.path, sizeof(g_dbc.path), "%s", path);
    app_act("备份数据库", w_db_dump, &g_dbc);
}

static int w_db_dump_char(void *ctx, Err err) {
    DbJobCtx *c = (DbJobCtx *)ctx;
    int nt = 0;
    u64 nr = 0;
    if (!db_backup_char(&g.db, &g.cur, c->path, &nt, &nr, err)) return 0;
    app_log("备份角色 %u · %d 张表 / %llu 行 → %s", g.cur.no, nt, (unsigned long long)nr, c->path);
    return 1;
}

void app_db_backup_char(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (!g.char_ok) { app_info("请先选择角色。"); return; }
    db_backup_char_path(g_dbc.path, sizeof(g_dbc.path), &g.cur);
    app_act("备份角色", w_db_dump_char, &g_dbc);
}

void app_db_restore(const char *path) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    snprintf(g_dbc.path, sizeof(g_dbc.path), "%s", path);
    app_act("还原数据库", w_db_restore, &g_dbc);
}

// ---------------- 生命周期 ----------------
void app_init(void) {
    InitializeCriticalSection(&s_log_cs);
    s_log_cs_ok = 1;
    char dir[MAX_PATH], sub[MAX_PATH], logs[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    path_join(sub, sizeof(sub), dir, "config");
    dir_make(sub);
    // 生成物按用途分目录：备份 / 导出 / 日志 / PVF 缓存
    art_dir("backup", sub, sizeof(sub));
    art_dir("export", sub, sizeof(sub));
    art_dir("logs", logs, sizeof(logs));
    art_dir("cache", sub, sizeof(sub));
    path_join(sub, sizeof(sub), dir, "config");
    path_join(g.cfg_path, sizeof(g.cfg_path), sub, "settings.ini");

    vec_init(&g.chars, sizeof(Charac));
    vec_init(&g.grant, sizeof(NamedId));
    vec_init(&g.recent, sizeof(NamedId));
    vec_init(&g.mails, sizeof(MailRow));
    vec_init(&g.pets, sizeof(PetRow));
    vec_init(&g.avatars, sizeof(AvatarRow));
    vec_init(&g.quests, sizeof(QuestRow));
    vec_init(&g.bans, sizeof(BanRow));
    vec_init(&g.events, sizeof(EventRow));
    vec_init(&g.event_info, sizeof(EventInfo));
    vec_init(&s_poll, sizeof(Charac));

    cfg_load(&g.cfg, g.cfg_path);
    app_status("就绪 · 请先连接数据库");
    char ts[40];
    now_str(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S");
    app_log("启动 · %s", ts);
    app_log_flush();
    art_prune(logs, "log-", 7);       // 日志只留最近 7 天
    // 上次用过的 PVF 仍在就自动加载，省得每次重选
    if (g.cfg.pvf[0] && file_exists(g.cfg.pvf)) app_load_pvf(g.cfg.pvf);
    grab_bind(g.cfg.grab_proc, app_item_name);
}

void app_shutdown(void) {
    cfg_save(&g.cfg, g.cfg_path);
    job_stop(&g.job_conn);
    job_stop(&g.job_load);
    job_stop(&g.job_pvf);
    job_stop(&g.job_poll);
    job_stop(&g.job_act);
    job_stop(&g.job_grab);
    job_stop(&g.job_bubble);
    db_undo_flush(1);                 // 本会话的写前快照落盘
    app_log("退出");
    app_log_flush();
    pvf_close(&g.pvf);
    db_free_data(&g.data);
    db_free_data(&g_load_ctx.d);
    if (g.db_ok) db_close(&g.db);
    if (s_log_cs_ok) { DeleteCriticalSection(&s_log_cs); s_log_cs_ok = 0; }
}

// ---------------- 泡点 ----------------
// 每 bubble_sec 秒给「在线账号」的赠送点券加 bubble_val 点（账在账号维度，
// 所以一个账号挂多个角色也只发一份）。只算本工具开着的时间：关掉就停、重开从零
// 计时，不补算——补算要给每个账号记上次结算时刻，等于把服务端的账本搬到 GM 台上。
static u64 s_bubble_next;      // 下次结算时刻（0 = 还没开始计时）
static u32 s_bubble_paid;      // 上次命中的账号数
static u32 s_bubble_runs;      // 已结算次数

typedef struct {
    int val;
} BubbleCtx;

static int w_bubble(void *ctx, Err err) {
    u32 n = 0;
    if (!db_bubble_tick(&g.db, ((BubbleCtx *)ctx)->val, &n, err)) return 0;
    s_bubble_paid = n;
    s_bubble_runs++;
    return 1;
}

void app_bubble_apply(int on, int sec, int val) {
    if (sec < 5) sec = 5;             // 下限 5 秒：再快就是拿一条连接刷库
    if (sec > 86400) sec = 86400;
    if (val < 1) val = 1;
    if (val > 1000000) val = 1000000;
    g.cfg.bubble_on = on ? 1 : 0;
    g.cfg.bubble_sec = sec;
    g.cfg.bubble_val = val;
    s_bubble_next = 0;                // 从改的这一刻重新计时，别沿用旧时刻
    cfg_save(&g.cfg, g.cfg_path);
    app_status("泡点：%s · 每 %d 秒 +%d 点", on ? "已启用" : "已停用", sec, val);
    app_log("泡点：%s · 每 %d 秒 +%d 点（赠送点券，只算在线账号）",
            on ? "已启用" : "已停用", sec, val);
}

void app_bubble_status(char *out, int cap) {
    if (!g.cfg.bubble_on) {
        snprintf(out, cap, "未启用");
        return;
    }
    if (!g.db_ok) {
        snprintf(out, cap, "已启用 · 等数据库连接后开始计时");
        return;
    }
    u64 now = now_ms();
    int left = s_bubble_next > now ? (int)((s_bubble_next - now) / 1000) : 0;
    if (!s_bubble_runs)
        snprintf(out, cap, "已启用 · %d 秒后首次结算", left);
    else if (s_bubble_paid)
        snprintf(out, cap, "已启用 · 上次 %u 个账号各 +%d 点 · %d 秒后下次",
                 s_bubble_paid, g.cfg.bubble_val, left);
    else
        snprintf(out, cap, "已启用 · 上次没有在线账号 · %d 秒后下次", left);
}

// ---------------- 周期轮询 ----------------
void app_tick(void) {
    Err err;
    static u64 last_poll = 0;

    if (job_poll(&g.job_conn, err)) {
        if (err[0]) {
            snprintf(g.db_err, sizeof(g.db_err), "%s", err);
            g.db_ok = 0;
            app_status("连接失败：%s", err);
            app_log("连接失败：%s", err);
        } else {
            g.db_ok = 1;
            app_status("数据库已连接 · 共 %u 个角色", g.char_total);
            app_log("数据库已连接 · 共 %u 个角色", g.char_total);
            // 自检结果只在这里落地：它是连接作业在后台写好的，job_poll 已经等到
            // 线程结束，所以这里读 g.self 不存在竞争
            if (g.self_bad) {
                app_log_block(g.self);
                app_status("数据库已连接 · 表/列自检 %d 处问题，详见日志", g.self_bad);
            } else if (g.self[0]) {
                app_log("%s", g.self);
            }
            last_poll = now_ms();
        }
    }

    if (job_poll(&g.job_load, err)) {
        if (err[0]) {
            g.char_ok = 0;
            db_free_data(&g_load_ctx.d);      // 半成品不留在暂存区里
            app_status("载入角色失败：%s", err);
        } else {
            // 快照在这里整体换代：job_poll 已经等到载入线程结束，此刻没有并发读者，
            // 交换 Blob 指针（浅拷贝）是安全的，而且旧快照直到这一刻都还能正常显示。
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
    }

    if (job_poll(&g.job_pvf, err)) {
        if (err[0]) {
            g.pvf_ok = 0;
            app_status("PVF 加载失败：%s", err);
            app_log("PVF 加载失败：%s", err);
        } else {
            g.pvf_ok = 1;
            app_status("PVF 已加载 · %d 个文件 · %d 条物品名", g.pvf.nleaf, g.pvf.items.n);
            app_log("PVF 已加载 · %d 个文件 · %d 条物品名", g.pvf.nleaf, g.pvf.items.n);
        }
    }

    if (job_poll(&g.job_act, err)) {
        int was_write = g.write_mask != 0;
        if (err[0]) {
            app_status("%s 失败：%s", g.act_what, err);
            app_log("%s 失败：%s", g.act_what, err);
            g.act_ver++;                      // 让页面按库里的值重填（失败时控件别停在假值上）
            if (was_write) {                  // 写库失败：以库里的值为准，别让界面停在假数据
                g.write_mask = 0;
                g.pend_mask = 0;
                app_reload();
            }
        } else {
            app_status("%s 完成", g.act_what);
            app_log("%s 完成", g.act_what);
            g.act_ver++;
            if (was_write) { g.write_mask = 0; g.data_ver++; }
            if (!strncmp(g.act_what, "邮件发放", 8)) vec_clear(&g.grant);
            if (!strncmp(g.act_what, "直接入包", 8)) { vec_clear(&g.grant); app_reload(); }
        }
    }

    if (job_poll(&g.job_poll, err)) {
        /* 角色列表/在线状态已刷新，界面下一帧自会取用 */
    }

    // 写失败的回滚请求：重读当前角色，把乐观更新改过的内存恢复成库里的值
    if (s_reload_later) {
        s_reload_later = 0;
        app_reload();
    }

    // 泡点：到点才发，而且必须连接空闲、队列里也没人的操作 —— 不跟人抢那条连接。
    // 没连库或没启用就把计时清零，下次连上从整段间隔重新开始。
    if (!g.db_ok || !g.cfg.bubble_on) {
        s_bubble_next = 0;
    } else if (!db_busy() && !g_pq_n) {
        u64 now = now_ms();
        u64 step = (u64)g.cfg.bubble_sec * 1000;
        if (!s_bubble_next) {
            s_bubble_next = now + step;
        } else if (now >= s_bubble_next) {
            s_bubble_next = now + step;
            BubbleCtx bc = {g.cfg.bubble_val};
            job_start(&g.job_bubble, w_bubble, &bc);
        }
    }
    if (job_poll(&g.job_bubble, err)) {
        if (err[0]) {
            app_log("泡点结算失败：%s", err);
        } else if (s_bubble_paid) {
            app_log("泡点结算：%u 个在线账号各 +%d 点（赠送点券）", s_bubble_paid,
                    g.cfg.bubble_val);
        }
    }

    // 连接空出来了：先把排队的动作发完，再补载入。
    // 顺序不能反 —— 载入会换掉 g.data，若排队的写还在等，它拿到的就是新角色的数据。
    if (!db_busy()) {
        if (g_pq_n) {
            int i = g_pq_head;
            g_pq_head = (g_pq_head + 1) % PEND_N;
            g_pq_n--;
            act_start(g_pq[i].what, g_pq[i].fn, g_pq[i].buf, g_pq[i].sz);
        } else if (s_load_want) {
            load_kick();
        }
    }

    app_flush_pending();              // 失焦写库的落地时机
    if (now_ms() - s_log_ms > 1000) app_log_flush();

    // 连取：开着就按 250ms 的节奏起快速取码。它只读游戏内存，不碰数据库连接，
    // 所以不用等 db_busy——和泡点/轮询各走各的作业线程，互不抢占。
    // builtin 命中后每轮只读几个地址（微秒级），代价可以忽略。
    static u64 s_grab_ms = 0;
    if (g.cfg.grab_auto && !job_running(&g.job_grab) && now_ms() - s_grab_ms > 250) {
        s_grab_ms = now_ms();
        app_grab_fast_once();
    }

    if (job_poll(&g.job_grab, err)) {
        static char s_grab_err[ERRBUF];   // 连取时同一句错误每 250ms 来一次，只报头一回
        if (err[0]) {
            if (!g.cfg.grab_auto || strcmp(err, s_grab_err)) {
                app_status("取码失败：%s", err);
                snprintf(s_grab_err, sizeof(s_grab_err), "%s", err);
            }
        } else {
            s_grab_err[0] = 0;
            g.grab_ver++;
            app_status("取到物品 #%u %s（%s · %ums）", g.grab.id, g.grab.name, g.grab.strategy,
                       (unsigned)g.grab.ms);
            app_log("取码 #%u %s · %s · 候选 %u · %ums", g.grab.id, g.grab.name, g.grab.strategy,
                    g.grab.cands, (unsigned)g.grab.ms);
        }
        // 刚才那一下「取码」被连取轮次挤掉了：作业刚空出来，补发
        if (s_grab_pending) {
            s_grab_pending = 0;
            app_grab_once();
        }
    }
    // 轮询：单条查询同时取角色列表/在线/当前角色，每 poll_sec 一次；
    // 连接被别的作业占着就跳过这一轮，不抢（抢会把包串了）
    if (g.db_ok && !db_busy() && now_ms() - last_poll > (u64)g.cfg.poll_sec * 1000) {
        last_poll = now_ms();
        job_start(&g.job_poll, w_poll, NULL);
    }
}
