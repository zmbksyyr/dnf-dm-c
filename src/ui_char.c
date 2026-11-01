// ui_char：GM 页（基础信息 + 一键 + PVP + 数据库维护）、封停页、活动页。
#include "ui.h"
#include "act.h"
#include <commdlg.h>

static int need_char(void) {
    if (!g.char_ok) { app_info("请先选择角色。"); return 0; }
    return 1;
}

// ================================================================ GM 页
// 交互与发放页一致：字段改完点到别处（失焦）立刻写库，没有「设置」按钮，
// 也没有「保存改动」这一步。破坏性动作（备份/还原/清服）仍然要确认。
enum {
    G_NAME = PAGE_ID(PAGE_GM, 1), G_LEV, G_JOB, G_GROW, G_MONEY, G_COIN, G_CERA, G_POINT,
    G_ACCM, G_GRADE, G_WIN, G_PVP_POINT,
    G_SP, G_QP, G_NOWEIGHT, G_SLOT, G_UNLOCK, G_EXPERT, G_FATIGUE, G_DUNGEON, G_PUNISH, G_QCLR,
    G_DUMP, G_RESTORE, G_CDUMP, G_UNLCREATE, G_SCANBAD, G_CLONE,
    // VIP：独立号段。上面 G_SP..G_QCLR 是连续的（G_SP+i 批量置灰依赖这一点），
    // 插进去会挪动整段编号，所以单独占一个号，不并进递增链。
    G_VIP = PAGE_ID(PAGE_GM, 50),
    // 任务模版导入/导出：同样单独占号。一键组那 10 个按钮是 G_SP+i 连续编号的，
    // 再往后加会撞上 G_DUMP 那一整段，所以这两个也走独立号段。
    G_QEXP = PAGE_ID(PAGE_GM, 51), G_QIMP,
    // 分组框：纯视觉容器，不参与 enable / 提交。ID 单独起一段，避开上面连续递增的控件号
    G_BOX_ROLE = PAGE_ID(PAGE_GM, 60), G_BOX_ASSET, G_BOX_PVP, G_BOX_ONCE, G_BOX_ACCT,
    G_BOX_MISC,
    // 数值框右边的「+数量」快捷按钮：8 个连号，顺序见 kQAdd
    G_QADD = PAGE_ID(PAGE_GM, 70)
};

static HWND c_name, c_lev, c_job, c_grow, c_money, c_coin, c_cera, c_point, c_accm, c_vip;
static HWND g_grade, g_win, g_point;

// 全服清错误物品的状态（实现在本文件后面）
static Vec  s_scan;
static int  s_scan_chars;        // 本次扫到的角色数
static int  s_scan_busy;         // 后台任务在跑（扫描或清除）
static int  s_phase;             // 0=扫描完成 1=清除完成、待复扫
static int  s_scan_open;         // 扫完自动弹窗（首次点按钮时）
// 作业真的还在写 s_scan：由作业线程收尾时自己清零。
// s_scan_busy 不能代替它 —— 关窗时 WM_DESTROY 会把 busy 清零（那是给入口用的），
// 而作业可能还在往 s_scan 里追加；job_running(&g.job_act) 也不能代替它 ——
// 那个槽位会被别的动作复用。
static volatile LONG s_scan_live;
static HWND s_slv, s_sinfo;

// 职业/觉醒都做成下拉：值→文字，避免让人对着裸数字猜
// 职业名沿用参考工具（x/2026.3.14GMTOOL源码）的叫法：6 号职业叫「刺客」而不是「盗贼」
static const char *kJobList[] = {"未转职", "鬼剑士", "格斗家", "神枪手", "魔法师", "圣职者",
                                 "刺客",   "男法师", "缔造者", "黑暗武士", "影舞者"};
static char kJobBuf[COUNT_OF(kJobList)][32];
static const char *kJobLbl[COUNT_OF(kJobList)];

static const char *kGrowList[] = {"未觉醒", "一次觉醒", "二次觉醒", "三次觉醒", "四次觉醒"};

// 段位 0~34：与参考实现（PVPRankList）一致，34 是最高的「斗神」
static char kPvpBuf[35][40];
static const char *kPvpLabel[35];
static const int kPvpMax = 34;

static void pvp_labels(void) {
    for (int i = 0; i <= kPvpMax; i++) {
        snprintf(kPvpBuf[i], sizeof(kPvpBuf[i]), "%d · %s", i, app_pvp_text(i));
        kPvpLabel[i] = kPvpBuf[i];
    }
}

// PVF 里的职业名比内置表准，加载了就换过来
static void job_labels(void) {
    for (int i = 0; i < COUNT_OF(kJobList); i++) {
        snprintf(kJobBuf[i], sizeof(kJobBuf[i]), "%s", app_job_text(i));
        kJobLbl[i] = kJobBuf[i];
    }
}

static void scan_show(void);

// 职业名只有 PVF 加载后才会变；每次 reload 都 CB_RESETCONTENT 重填会让下拉框闪一下，
// 还会把用户正展开的列表收起来。只在来源变了时重建。
static int s_job_src = -1;

// ---- 失焦写库的发送队列 ----
// 一次只能跑一个写任务（共用一条连接）。字段失焦时把「要写的值」抄进队列立刻排队，
// 不等后台任务回来——否则连着改几个框，前一个还在写、后一个就把值丢了。
// 值在失焦那一刻就定下来，之后 reload 覆盖控件也不影响已经排上的改动。
#define GM_QN 8
static Ctx s_gmq[GM_QN];
static int s_gmq_n;
static Ctx s_gm_run;                 // 正在跑的那份（任务期间不能被覆写）

// 强制重填一次（校验不过退回原值、PVF 换了职业名）。正常走换代判断，用不到它。
// 换代重填时直接覆盖控件，不再判断"这个框有没有焦点"：会触发换代的只有
// 选/换角色、主动重载、写后校验不符——这三种情况下用户手里的草稿都没有保留意义。
static int s_refill;

static const char *gm_what(int kind) {
    switch (kind) {
        case K_LEV: return "设置等级";
        case K_MONEY: return "设置金币";
        case K_COIN: return "设置复活币";
        case K_CERA: return "设置点券";
        case K_ACCM: return "设置账号金币";
        case K_VIP: return "设置VIP";
        case K_JOB: return "设置职业";
        case K_PVP: return "设置 PVP";
        case K_RENAME: return "改名";
        default: return "写入";
    }
}

static void gm_refill(void);          // 实现见下（gm_kick 提交失败时要回滚界面）

static void gm_kick(void) {
    if (!s_gmq_n || !app_act_idle()) return;   // 连接忙时先不弹队，等连接空闲后由 page_gm_pump 发
    s_gm_run = s_gmq[0];
    memmove(s_gmq, s_gmq + 1, sizeof(Ctx) * (size_t)(s_gmq_n - 1));
    s_gmq_n--;
    if (app_act(gm_what(s_gm_run.kind), char_work, &s_gm_run)) return;
    // 没提交出去。条目已经弹出去了，先塞回队首 —— 静默丢是这里最坏的走法：
    // 界面那一格显示「已经改过」，库里一个字没动。
    memmove(s_gmq + 1, s_gmq, sizeof(Ctx) * (size_t)s_gmq_n);
    s_gmq[0] = s_gm_run;
    s_gmq_n++;
    if (!g.db_ok) {
        // 连接已经没了：这份改值属于上一个会话，重试只会把它按旧角色号写进
        // 下一个库（app.c 的泄洪注释要避免的正是这件事）。整队丢掉，界面回填。
        s_gmq_n = 0;
        gm_refill();
        app_status("未连接，%s 没有写入", gm_what(s_gm_run.kind));
        return;
    }
    app_status("%s 没提交出去，等连接空闲再发", gm_what(s_gm_run.kind));
}

// 队列的推进挂在这里，而不是挂在 gm_reload 上。
// 挂在 reload 上意味着"页面不刷新 → 队列不发"，于是隐藏页也必须跟着刷，
// 刷新就从"让界面跟上"变成了写入流程的一环——这条耦合正是各种刷新打架的根源。
// 现在每 tick 推进一次，与页面可见性无关；reload 只管显示。
void page_gm_pump(void) { gm_kick(); }

// 断开/重连时由 app_drop_queued() 调：把本页「还没开始」的东西全部放下。
// 这里能放心清 s_scan_live，是因为调用点已经确认所有作业线程都停了 ——
// 还在跑的那种情况 app_disconnect 会整个取消，根本走不到这儿。
static void gm_drop(void) {
    s_gmq_n = 0;                       // 页面写队列：属于旧连接的意图
    InterlockedExchange(&s_scan_live, 0);
    s_scan_busy = 0;
    s_phase = 0;
    s_scan_open = 0;
}

// 先把值写进内存快照，再排进队列。
// 快照是表单和左栏的唯一数据源：不先更新它，左栏那一行要等写库完成才变，
// 中间几百毫秒里列表还写着旧等级，看着像点了没反应。写入成功由 app_sync_char
// 回读校验校正（一致时什么都不做），失败则由 app_reload 整段重读回滚。
static void gm_apply(int kind, const Ctx *c) {
    switch (kind) {
        case K_LEV: g.cur.lev = c->i1; break;
        case K_JOB: g.cur.job = c->i1; g.cur.grow = c->i2; break;
        case K_RENAME: snprintf(g.cur.name, sizeof(g.cur.name), "%s", c->s); break;
        case K_MONEY: g.data.money = c->u1; break;
        case K_COIN: g.data.coin = c->u1; break;
        case K_CERA: g.data.cera = c->u1; g.data.cera_point = c->u2; break;
        case K_ACCM: g.data.account_money = c->u1; break;
        case K_VIP: snprintf(g.data.vip, sizeof(g.data.vip), "%s", c->s); break;
        case K_PVP:
            g.data.pvp_grade = c->i1;
            g.data.pvp_win = c->i2;
            g.data.pvp_point = c->i3;
            break;
        default: break;
    }
    // 左栏那一行也立刻跟上：别的字段（金币/点券）只在表单里，改名/等级/职业
    // 却是列表和卡片上看得见的，晚几百毫秒更新会显得"点了没反应"
    app_row_sync();
}

static void gm_push(int kind, const Ctx *c) {
    if (s_gmq_n >= GM_QN) {
        app_status("改动太频繁，等上一个写完再改");
        gm_refill();                        // 队列满：界面也退回库里的值，别留着假数据
        return;
    }
    gm_apply(kind, c);
    s_gmq[s_gmq_n] = *c;
    s_gmq[s_gmq_n].kind = kind;
    s_gmq[s_gmq_n].cno = g.cur.no;     // 归属：排队期间用户可能已经切了角色
    s_gmq[s_gmq_n].mid = g.cur.mid;
    s_gmq_n++;
    app_status("%s…", gm_what(kind));
    gm_kick();
}

// 表单是快照的一份投影，不是数据库的一份实时镜像：
// s_fill_gen 记住"当前显示的是第几代快照"，只有换代才重填一次。
// 换代只有三种：选/换角色、主动重载、写后校验发现库里的值跟写入的不一致。
// 写库成功、别的页写完成、轮询发现玩家自己在游戏里升级 —— 都不重填：
// 内容一样是白做功加白闪一下，内容不一样则会把用户正在敲的字顶掉。
static u32 s_fill_gen = 0xFFFFFFFFu;

static void gm_reload(Page *p) {
    (void)p;
    // 首次点「全服清错误物品」后，扫描在后台跑；完成时这里自动弹出结果
    if (s_scan_open && s_scan_busy && !s_scan_live) {
        s_scan_open = 0;
        s_scan_busy = 0;
        scan_show();
    }
    // 职业下拉的选项来自 PVF：来源变了要重建，重建后选中项回到 0，所以必须重填
    int src = g.pvf_ok ? 1 : 0;
    if (src != s_job_src) {
        s_job_src = src;
        job_labels();
        ui_combo_fill(c_job, kJobLbl, COUNT_OF(kJobList), 0);
        s_refill = 1;
    }
    if (!g.char_ok) return;              // 没选角色：控件由 gm_lock 置灰，无需回填
    if (!s_refill && s_fill_gen == g.data_ver) return;
    ui_text(c_name, g.cur.name);
    // 数值框一律填 0：这些框收的是「这次要设成多少」，填完一次就交出去，
    // 交完框自己清回 0（见 gm_commit）。把当前值填进来会让人以为「框里是什么
    // 库里就是什么」，其实这框只管这一次的设定。当前值看左边角色信息。
    ui_set_int(c_lev, 0);
    ui_combo_set(c_job, g.cur.job >= 0 && g.cur.job < COUNT_OF(kJobList) ? g.cur.job : 0);
    ui_combo_set(c_grow, g.cur.grow >= 0 && g.cur.grow < COUNT_OF(kGrowList) ? g.cur.grow : 0);
    ui_set_i64(c_money, 0);
    ui_set_i64(c_coin, 0);
    ui_set_i64(c_cera, 0);
    ui_set_i64(c_point, 0);
    ui_set_i64(c_accm, 0);
    SendMessageW(c_vip, BM_SETCHECK,
                 (g.data.vip[0] && strcmp(g.data.vip, "0")) ? BST_CHECKED : BST_UNCHECKED, 0);

    int gr = g.data.pvp_grade;
    if (gr < 0) gr = 0;
    if (gr > kPvpMax) gr = kPvpMax;
    ui_combo_set(g_grade, gr);
    ui_set_int(g_win, 0);
    ui_set_int(g_point, 0);
    s_fill_gen = g.data_ver;             // 这一代快照已经填过了
    s_refill = 0;
    // 队列的推进不在这里：page_gm_pump 每 tick 都会调，和本页刷不刷新无关
}

// 校验不过/角色在线时把界面退回快照里的值（快照就是上次从库里读到的真值）
static void gm_refill(void) {
    s_refill = 1;
    gm_reload(NULL);
}

// 备份：直接落到 config/backup/db_<时间戳>.sql，不再弹「另存为」打断操作
static void on_dump(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    char path[MAX_PATH];
    db_dump_path(path, sizeof(path));      // 命名和留存策略统一在 db 层：backup/db-<时间>.sql
    if (!app_confirm(fmt_new("把整库备份到\n%s？\n\n备份期间请勿关闭程序。", path))) return;
    app_status("正在备份整库…");
    app_db_backup(path);
}

static void on_restore(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    char dir[MAX_PATH], back[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    snprintf(back, sizeof(back), "%s\\config\\backup", dir);

    WCHAR file[MAX_PATH] = L"";
    WCHAR wdir[MAX_PATH];
    utf16(back, wdir, MAX_PATH);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"SQL 备份 (*.sql)\0*.sql\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = wdir;     // 默认从备份目录里挑
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    char path[MAX_PATH];
    utf8(file, path, sizeof(path));
    if (!app_confirm(fmt_new("把 %s 导入数据库？\n\n%s", path,
                             strstr(path, "char_")
                                 ? "角色备份：会先删掉该角色现有数据，再按备份写回。"
                                 : "整库备份：按语句逐条执行，已存在的行可能重复。")))
        return;
    if (!app_confirm("确认执行导入？建议先做一次整库备份。")) return;
    app_status("正在还原整库…");
    app_db_restore(path);
}

static void on_char_dump(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (!g.char_ok) { app_info("请先选择角色。"); return; }
    char path[MAX_PATH];
    db_backup_char_path(path, sizeof(path), &g.cur);
    if (!app_confirm(fmt_new("备份角色「%s」(%u) 到\n%s？\n\n"
                             "含背包/装备/时装/宠物/技能/任务等 15 张表。",
                             g.cur.name, g.cur.no, path)))
        return;
    app_status("正在备份角色…");
    app_db_backup_char();
}

// ---------------- 任务模版 导入 / 导出 ----------------
static void on_quest_exp(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (!g.char_ok) { app_info("请先选择角色。"); return; }
    char path[MAX_PATH];
    db_quest_tpl_path(path, sizeof(path), &g.cur);
    if (!app_confirm(fmt_new("把「%s」的任务进度导出成模版？\n\n%s\n\n"
                             "含已完成任务、20 个进行中任务槽及其触发器、任务商店属性加成。",
                             g.cur.name, path)))
        return;
    app_status("正在导出任务模版…");
    app_quest_export();
}

static void on_quest_imp(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (!g.char_ok) { app_info("请先选择角色。"); return; }
    char dir[MAX_PATH], back[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    snprintf(back, sizeof(back), "%s\\config\\export", dir);

    WCHAR file[MAX_PATH] = L"";
    WCHAR wdir[MAX_PATH];
    utf16(back, wdir, MAX_PATH);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"任务模版 (*.txt)\0*.txt\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = wdir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    char path[MAX_PATH];
    utf8(file, path, sizeof(path));
    if (!app_confirm(fmt_new("把\n%s\n的任务进度套到「%s」身上？\n\n"
                             "会先删掉该角色现有的任务记录，再按模版写回。",
                             path, g.cur.name)))
        return;
    app_status("正在导入任务模版…");
    app_quest_import(path);
}

// ---------------- 全服清错误物品（维护动作，不挑角色） ----------------
// 扫描全服所有角色的容器、邮件附件、时装/宠物表，列出错误物品并可一键清除。
// 名字是否存在的判定交给 PVF，未加载时退化成只查结构
static int scan_has_name(u32 id, void *ctx) {
    (void)ctx;
    return g.pvf_ok && *pvf_item_name(&g.pvf, id);
}

// 堆叠上限。没 PVF 时返回 0，那一整项检查自动跳过
static u32 scan_stack_lim(u32 id, void *ctx) {
    (void)ctx;
    return app_item_stack_limit(id);
}

static int scan_work(void *ctx, Err err) {
    (void)ctx;
    int ok = db_scan_bad(&g.db, &s_scan, &s_scan_chars, scan_has_name, scan_stack_lim, NULL, err);
    // 收尾自己放开闸：窗口关掉之后没有 WM_TIMER 来收，外面谁也判断不了它是否还在
    // 往 s_scan 里追加，只有这里知道「这一轮确实写完了」。
    InterlockedExchange(&s_scan_live, 0);
    return ok;
}

static int purge_work(void *ctx, Err err) {
    (void)ctx;
    int done = 0;
    db_purge_bad(&g.db, &s_scan, &done, err);
    InterlockedExchange(&s_scan_live, 0);       // 同上：清除也会改写 s_scan
    app_log("清除错误物品 · 完成 %d 处", done);
    return 1;
}

static void scan_run(void) {
    s_scan_busy = 1;
    InterlockedExchange(&s_scan_live, 1);
    if (!app_act("全服扫描", scan_work, NULL)) {     // 没提交出去：闸不能留着，否则这页再也扫不了
        InterlockedExchange(&s_scan_live, 0);
        s_scan_busy = 0;
    }
}

static int scan_start(void) {
    // 关窗时 WM_DESTROY 会把 s_scan_busy 清零（否则入口永远回「上一轮还没跑完」），
    // 但作业可能还在 db_scan_bad 里往这个 Vec 追加。这时 s_scan.n=0 就是两个线程
    // 同时改 n，而且可能一边追加、一边 xrealloc 换缓冲，写到已经释放的旧块上。
    // s_scan_live 是唯一可靠信号：它只由作业线程在收尾时清。
    if (s_scan_live) { app_info("上一轮还没跑完，等它结束再扫。"); return 0; }
    if (!s_scan.cap) vec_init(&s_scan, sizeof(ScanRow));
    s_scan.n = 0;
    s_scan_chars = 0;
    app_status("正在扫描全服…");
    scan_run();
    return 1;
}

static void on_scanbad(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (s_scan_busy) { app_info("上一轮还没跑完。"); return; }
    if (!app_confirm("扫描全服所有角色的容器、邮件、时装、宠物，找出错误物品？\n\n"
                     "角色较多时需要一点时间。"))
        return;
    // 没真的提交出去就别置「扫完自动弹窗」，否则 gm_reload 会拿着一份旧结果弹窗
    if (scan_start()) s_scan_open = 1;
}

static void scan_fill(void) {
    if (!s_slv) return;
    ui_rows_clear(s_slv);
    ui_batch(s_slv, 1);
    for (int i = 0; i < s_scan.n; i++) {
        ScanRow *r = VEC_AT(&s_scan, ScanRow, i);
        char sl[12], id[16], nm[96];
        if (r->slot > 0) fmt(sl, sizeof(sl), "%d", r->slot);
        else snprintf(sl, sizeof(sl), "—");
        fmt(id, sizeof(id), "%u", r->id);
        const char *cells[7] = {r->name, r->acc, r->cont, sl, id,
                                app_item_name(r->id, nm, sizeof(nm)), r->why};
        ui_row_add(s_slv, cells, 7, (LPARAM)i);
    }
    ui_batch(s_slv, 0);
    ui_text(s_sinfo, fmt_new("扫描 %d 个角色 · 发现 %d 处异常%s", s_scan_chars, s_scan.n,
                             g.pvf_ok ? "（已按 PVF 校验）" : "（PVF 未加载，仅查结构）"));
}

static LRESULT scan_proc(HWND d, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    (void)lp;
    (void)ctx;
    if (m == WM_TIMER) {
        if (s_scan_busy && !s_scan_live) {
            s_scan_busy = 0;
            if (s_phase) {                       // 清除完，自动复扫一遍看结果
                s_phase = 0;
                ui_text(s_sinfo, "清除完成，正在重新扫描…");
                scan_run();
            } else {
                scan_fill();
            }
        }
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(d, 1);
        s_slv = s_sinfo = NULL;
        // 关窗时必须把扫描状态一起复位：WM_TIMER 是这里唯一的完成检测点，
        // 窗口没了它就断了，而 s_scan_open 只在 on_scanbad 里置 1、又被 gm_reload
        // 清成 0 —— 于是 s_scan_busy 再也回不到 0，入口（on_scanbad / 6002 / 6003）
        // 此后一律回「上一轮还没跑完」，全服清错误物品整页废掉，直到重启进程。
        // 关窗时若作业还在跑，就让它照旧在后台把库改完 —— 这里放弃的只是
        // 「等它跑完再填列表」这件事，不能连入口一起锁死。作业还活着这件事由
        // s_scan_live 记住，scan_start / 6002 会据此拒绝复用 s_scan。
        s_scan_busy = 0;
        s_phase = 0;
        s_scan_open = 0;
        return 0;
    }
    if (m == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case 6002:                               // 清除全部
            if (s_scan_busy || s_scan_live) return 0;     // 同上：别跟还在追加的作业抢 s_scan
            if (!s_scan.n) { app_info("没有需要清除的记录。"); return 0; }
            if (!app_confirm(fmt_new("清除全部 %d 处错误物品？\n\n"
                                     "容器槽位写成空槽（原数据自动备份到 config/backup），\n"
                                     "时装/宠物表删行，邮件只清附件、金币保留。", s_scan.n)))
                return 0;
            s_phase = 1;
            s_scan_busy = 1;
            InterlockedExchange(&s_scan_live, 1);
            if (!app_act("清除错误物品", purge_work, NULL)) {
                InterlockedExchange(&s_scan_live, 0);
                s_scan_busy = 0;
                s_phase = 0;
                return 0;
            }
            ui_text(s_sinfo, "正在清除…完成后自动重新扫描。");
            return 0;
        case 6003:                               // 重新扫描
            if (s_scan_busy) return 0;
            if (scan_start()) ui_text(s_sinfo, "正在扫描…");
            return 0;
        }
    }
    return -1;
}

static void scan_show(void) {
    HWND d = ui_dlg_new(g_main, "全服清错误物品", 780, 480, scan_proc, NULL);
    if (!d) return;
    s_sinfo = ui_label(d, "", 0);
    ui_place(s_sinfo, ui_scale(12), ui_scale(10), ui_scale(756), ui_scale(20));

    s_slv = ui_list(d, 6000);
    ui_cols(s_slv, (const char *const[]){"角色", "账号", "容器", "槽位", "物品编号", "名称", "原因"},
            (const int[]){100, 96, 64, 44, 80, 180, 130}, 7);
    ui_idtip_bind(s_slv, 4);           // 「物品编号」列
    ui_place(s_slv, ui_scale(12), ui_scale(34), ui_scale(756), ui_scale(360));

    ui_place(ui_button(d, "清除全部", 6002), ui_scale(496), ui_scale(402), ui_scale(88), ui_scale(26));
    ui_place(ui_button(d, "重新扫描", 6003), ui_scale(592), ui_scale(402), ui_scale(88), ui_scale(26));
    ui_place(ui_button(d, "关闭", IDCANCEL), ui_scale(688), ui_scale(402), ui_scale(80), ui_scale(26));
    ui_tip(d, GetDlgItem(d, 6002),
           "容器里的错误槽位写成空槽（原数据自动备份到 config/backup）；"
           "时装/宠物表删行；邮件只清附件、金币保留");

    scan_fill();
    SetTimer(d, 1, 200, NULL);
    ui_dlg_run(d, g_main);
}

// ---------------- GM 数值框：框里是「设成多少」，按钮是「加多少」 ----------------
// 两个语义分开，别混：失焦提交按框里这个数写库（和库里当前值比，没变就不发 SQL）；
// 右边的 + 按钮在当前值上加，加完把结果回写进框 —— 框和库始终是同一份数字，
// 不会出现「框里是增量、点到别处却当成现值」的歧义。
// 金币/复活币/账号金币/点券/赠送点券在库里都是 int unsigned，塞不进 2^32。
#define GM_U32_MAX 4294967295LL
#define GM_PVP_MAX 2000000000          // 胜场/胜点，库里是 int

// 等级上限不写死：各端封顶不一样（多数是 85），写在 settings.ini 的 [gm] lev_max，
// 缺省 85。以前硬编码 100，在 85 封顶的服上按 100 写会越界。
static int gm_lev_max(void) {
    int v = g.cfg.lev_max;
    if (v < 1 || v > 255) v = 85;
    return v;
}

static i64 gm_u32(HWND e) {
    i64 v = ui_i64(e);
    if (v < 0) v = 0;
    if (v > GM_U32_MAX) v = GM_U32_MAX;    // ui_num 已经拦过一次，这里是双保险
    return v;
}

// 字段 -> 输入框。提交和 + 按钮都靠它，省掉几排平行的 switch
static HWND gm_field(int id) {
    switch (id) {
        case G_LEV:   return c_lev;
        case G_MONEY: return c_money;
        case G_COIN:  return c_coin;
        case G_ACCM:  return c_accm;
        case G_CERA:  return c_cera;
        case G_POINT: return c_point;
        case G_WIN:   return g_win;
        case G_PVP_POINT: return g_point;
        default: return NULL;
    }
}

// 数值框是一次性的设定框：填好 → 失焦提交 → 框自己清回 0。
// 所以框里的 0 表示「这次没有要设的值」，不能当成「设为 0」——否则用户点一下
// 空框再离开、什么都没改，金币就被清零了。当前值只从左边角色信息看。
static void gm_take(HWND e) {
    if (e) ui_set_i64(e, 0);
}

// 快照里的当前值：+ 按钮在它上面加，而不是在框里加（框里恒为 0）。
static i64 gm_value(int id) {
    switch (id) {
        case G_LEV:       return g.cur.lev;
        case G_MONEY:     return (i64)g.data.money;
        case G_COIN:      return (i64)g.data.coin;
        case G_ACCM:      return (i64)g.data.account_money;
        case G_CERA:      return (i64)g.data.cera;
        case G_POINT:     return (i64)g.data.cera_point;
        case G_WIN:       return g.data.pvp_win;
        case G_PVP_POINT: return g.data.pvp_point;
        default:          return 0;
    }
}

// 失焦即写：只有值真的变了才发一条 UPDATE（点来点去、来回切页都不产生流量）。
// 校验不过不弹框打断——失焦时弹模态框会跟焦点切换打架，状态栏说一句就够了。
static void gm_commit(int id) {
    if (!g.char_ok) return;
    if (g.cur.online) {
        app_status("角色在线，改动未写入——请先退出游戏");
        gm_refill();                     // 顺便把刚填的值清掉
        return;
    }
    Ctx c;
    memset(&c, 0, sizeof(c));
    switch (id) {
        case G_LEV: {
            int v = ui_int(c_lev);
            gm_take(c_lev);
            if (v == 0) return;                    // 没填
            if (v < 1 || v > gm_lev_max()) {
                app_status("等级范围 1~%d，未写入", gm_lev_max());
                return;
            }
            if (v == g.cur.lev) return;
            c.i1 = v;
            // 经验写 0，不写「这一级的门槛值」：exp 是「本级已经攒了多少」，
            // 填成门槛等于让人顶着满经验条进游戏，服务端一结算就再升一级
            // （85 级的门槛就 10.8 亿，94 级往上还超过 int 上限）。0 = 刚到这一级。
            c.u1 = 0;
            gm_push(K_LEV, &c);
            return;
        }
        case G_MONEY: {
            i64 v = gm_u32(c_money);
            gm_take(c_money);
            if (v == 0) return;
            if (v == (i64)g.data.money) return;
            c.u1 = (u64)v;
            gm_push(K_MONEY, &c);
            return;
        }
        case G_COIN: {
            i64 v = gm_u32(c_coin);
            gm_take(c_coin);
            if (v == 0) return;
            if (v == (i64)g.data.coin) return;
            c.u1 = (u64)v;
            gm_push(K_COIN, &c);
            return;
        }
        case G_CERA:          // 点券和赠送点券是一条 UPDATE，两个框谁失焦都一起提交
        case G_POINT: {
            i64 ce = gm_u32(c_cera), pt = gm_u32(c_point);
            gm_take(c_cera);
            gm_take(c_point);
            // 只填了一个就只改一个，另一个沿用当前值
            if (ce == 0) ce = (i64)g.data.cera;
            if (pt == 0) pt = (i64)g.data.cera_point;
            if (ce == (i64)g.data.cera && pt == (i64)g.data.cera_point) return;
            c.u1 = (u64)ce;
            c.u2 = (u64)pt;
            gm_push(K_CERA, &c);
            return;
        }
        case G_ACCM: {
            i64 v = gm_u32(c_accm);
            gm_take(c_accm);
            if (v == 0) return;
            if (v == (i64)g.data.account_money) return;
            c.u1 = (u64)v;
            gm_push(K_ACCM, &c);
            return;
        }
        case G_VIP: {
            // 勾上写 "1"，取消写 "0"。两条路都要有：原来只能设不能撤，
            // 想取消只能自己去库里改。
            const char *v = (SendMessageW(c_vip, BM_GETCHECK, 0, 0) == BST_CHECKED) ? "1" : "0";
            if (!strcmp(v, g.data.vip)) return;
            snprintf(c.s, sizeof(c.s), "%s", v);
            gm_push(K_VIP, &c);
            return;
        }
        case G_JOB:
        case G_GROW: {
            int j = ui_combo_sel(c_job), gr = ui_combo_sel(c_grow);
            if (j < 0) j = 0;
            if (gr < 0) gr = 0;
            if (j == g.cur.job && gr == g.cur.grow) return;
            c.i1 = j;
            c.i2 = gr;
            gm_push(K_JOB, &c);
            return;
        }
        case G_GRADE:
        case G_WIN:
        case G_PVP_POINT: {
            // 段位是下拉（选中的就是目标值），胜场/胜点是设定框：框里 0 表示
            // 这一项不动，沿用当前值 —— 只改段位时不用把另外两个数字再抄一遍。
            int gr = ui_combo_sel(g_grade), win = ui_int(g_win), pt = ui_int(g_point);
            gm_take(g_win);
            gm_take(g_point);
            if (gr < 0) gr = 0;
            if (gr > kPvpMax) gr = kPvpMax;
            if (win <= 0) win = g.data.pvp_win;
            if (win > GM_PVP_MAX) win = GM_PVP_MAX;
            if (pt <= 0) pt = g.data.pvp_point;
            if (pt > GM_PVP_MAX) pt = GM_PVP_MAX;
            if (gr == g.data.pvp_grade && win == g.data.pvp_win && pt == g.data.pvp_point) return;
            c.i1 = gr;
            c.i2 = win;
            c.i3 = pt;
            gm_push(K_PVP, &c);
            return;
        }
        case G_NAME: {
            char nm[64];
            ui_get(c_name, nm, sizeof(nm));
            str_trim(nm);
            if (!nm[0]) {
                app_status("名字不能为空，已退回原值");
                gm_refill();
                return;
            }
            if (!strcmp(nm, g.cur.name)) return;
            // 重名/占用由写任务在任务线程里判定（UI 线程不能直接查库）
            snprintf(c.s, sizeof(c.s), "%s", nm);
            gm_push(K_RENAME, &c);
            return;
        }
        default: return;
    }
}

// ---------------- 数值框的「+数量」快捷按钮 ----------------
// 按钮只写「+数量」：在当前值上加这么多，立刻写库。
// 加法的起点是快照里的当前值（左栏那份），不是输入框 —— 框里恒为 0，
// 拿它当起点等于每次都从 0 加到同一个数，点两下也只有一份。
#define QADD_N 8
static HWND s_qadd[QADD_N];
static const struct {
    int         target;    // 加到哪个字段（决定走哪条提交）
    u64         step;
    const char *txt;
    const char *name;      // 提示语里怎么称呼这个字段
} kQAdd[QADD_N] = {
    {G_LEV,       10,       "+10",    "等级"},      // 加到 gm_lev_max() 封顶（默认 85）
    {G_MONEY,     10000000, "+1000万", "金币"},
    {G_COIN,      100,      "+100",   "复活币"},
    {G_ACCM,      10000000, "+1000万", "账号金币"},
    {G_CERA,      10000,    "+1万",   "点券"},
    {G_POINT,     10000,    "+1万",   "赠送点券"},
    {G_WIN,       100,      "+100",   "胜场"},
    {G_PVP_POINT, 1000,     "+1000",  "胜点"},
};

static void gm_quick_add(int i) {
    if (i < 0 || i >= QADD_N) return;
    if (!g.char_ok) return;
    if (g.cur.online) {
        app_status("角色在线，改动未写入——请先退出游戏");
        return;
    }
    int tgt = kQAdd[i].target;
    HWND e = gm_field(tgt);
    if (!e) return;
    i64 cap = (tgt == G_LEV) ? gm_lev_max()
            : (tgt == G_WIN || tgt == G_PVP_POINT) ? GM_PVP_MAX
                                                   : GM_U32_MAX;
    i64 cur = gm_value(tgt);           // 当前值来自快照（框里恒为 0）
    if (cur < 0) cur = 0;
    if (cur >= cap) {
        app_status("%s 已经是上限 %lld 了", kQAdd[i].name, (long long)cap);
        return;
    }
    i64 v = cur + (i64)kQAdd[i].step;
    if (v > cap) v = cap;
    ui_set_i64(e, v);                  // 借框走一遍统一的校验/提交，提交完框自己回 0
    gm_commit(tgt);
}

// 点页面空白处不会把焦点从输入框上拿走，但用户心里那就算「改完了」：
// 把当前有焦点的那个框提交掉（值没变自然什么都不做）
static void gm_commit_focus(void) {
    HWND f = GetFocus();
    if (!f || GetParent(f) != g_pages[PAGE_GM].wnd) return;
    gm_commit(GetDlgCtrlID(f));
}

static LRESULT gm_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    (void)lp;
    if (m == WM_LBUTTONDOWN) {
        gm_commit_focus();
        return 0;
    }
    if (m != WM_COMMAND) return -1;
    int id = LOWORD(wp);
    int code = HIWORD(wp);
    if (code == EN_KILLFOCUS) {
        gm_commit(id);
        return 0;
    }
    // 下拉框：选完（SELENDOK）就落库；键盘上下改完再点别处（KILLFOCUS）也兜一次
    if (code == CBN_SELENDOK || code == CBN_KILLFOCUS) {
        gm_commit(id);
        return 0;
    }
    // 勾选框不属于输入框，收不到 EN_KILLFOCUS，勾了要自己接一手：BN_CLICKED 就提交。
    // 漏了这一条的表现是「勾上去像是生效了，库里一点没动」——只有勾选框会这样。
    if (code == BN_CLICKED && id == G_VIP) {
        gm_commit(id);
        return 0;
    }
    // 「+数量」按钮：先加到框里，再交给上面的提交路径。按钮区间是连号的，不用逐个 if
    if (code == BN_CLICKED && id >= G_QADD && id < G_QADD + QADD_N) {
        gm_quick_add(id - G_QADD);
        return 0;
    }
    if (id == G_DUMP) { on_dump(); return 0; }
    if (id == G_RESTORE) { on_restore(); return 0; }
    if (id == G_CDUMP) { on_char_dump(); return 0; }
    if (id == G_SCANBAD) { on_scanbad(); return 0; }
    if (id == G_CLONE) { app_clone_char(); return 0; }
    if (id == G_QEXP) { on_quest_exp(); return 0; }
    if (id == G_QIMP) { on_quest_imp(); return 0; }
    if (id == G_UNLCREATE) {
        if (!need_char()) return 0;
        if (!app_confirm(fmt_new("解除账号 mid=%u 的建角次数限制？", g.cur.mid))) return 0;
        memset(&kc, 0, sizeof(kc));
        kc.kind = K_UNLCREATE;
        kc.mid = g.cur.mid;
        kc.cno = g.cur.no;
        app_act("解除建角限制", char_work, &kc);
        return 0;
    }
    if (!need_char()) return 0;
    // kc 是全局 Ctx，跨动作复用：不先清零，上一次动作留下的 i1/i2/s 会跟着跑；
    // 更糟的是 cno/mid 从没人填过 —— 一键按钮全在写 charac_no=0，0 行影响被
    // 判成失败，界面上就是"点了没反应/报错"。归属一律在这里定，和 gm_push 对齐。
    memset(&kc, 0, sizeof(kc));
    kc.cno = g.cur.no;
    kc.mid = g.cur.mid;
    switch (id) {
        case G_SP: kc.kind = K_SP; kc.i1 = 90000; kc.i2 = 90000; break;
        case G_QP: kc.kind = K_QP; kc.i1 = 90000; break;
        case G_NOWEIGHT: kc.kind = K_NOWEIGHT; break;
        case G_SLOT: kc.kind = K_SLOT; break;
        case G_UNLOCK: kc.kind = K_UNLOCK; break;
        case G_EXPERT: kc.kind = K_EXPERT; break;
        case G_FATIGUE: kc.kind = K_FATIGUE; break;
        case G_DUNGEON: kc.kind = K_DUNGEON; break;
        case G_PUNISH: kc.kind = K_PUNISH; kc.mid = g.cur.mid; break;
        case G_QCLR:
            kc.kind = K_QCLR;
            g.lists_ready &= ~(u32)L_QUESTS;   // 任务被清空，任务页重新去库里取
            break;
        default: return -1;
    }
    app_act("GM 操作", char_work, &kc);
    return 0;
}

// ---------------- GM 页布局 ----------------
// 三条硬规矩，动这里之前先看：
// 1) 标签在控件上方，不在左边。标签在左时，「等级」后面空一大截、「账号金币」却紧贴
//    控件，每行的松紧都不一样，怎么调都觉得散；统一放上方后，同一列控件的左边缘和
//    宽度完全一致，整栏像一把尺子量出来的。
// 2) 组框铺满整栏，内容在组内居中。GM 页的东西不算多，按自然高度排会在页面下半截
//    留一大片空白；让组框吃掉剩余高度、内容在框内上下居中，页面就不会显得"没填满"。
// 3) 一个组框只放一类东西。字段、一键、维护混在一起时，按钮和输入框的基线永远对不齐。
// 一行 = 左列（标签 + 控件）与可选的右列（标签 + 控件）；lab2 为 NULL 时左列独占整行。
// 原本还有个「右列拆成两个等宽框」的 twin 模式，只被点券/赠送点券那一行用过——
// VIP 进来后资产组改成三个对称双列行，既然没人再用，就把这个模式连同字段一起删掉。
typedef struct {
    const char *lab;       // 左列标签
    HWND        ctl;       // 左列控件
    HWND        btn;       // 左列控件右边的「+数量」按钮（NULL = 没有）
    const char *lab2;      // 右列标签（NULL = 这行只有一列）
    HWND        ctl2;      // 右列控件
    HWND        btn2;      // 右列控件右边的「+数量」按钮
} GmRow;

typedef struct { int x, y, w, h; } GmRect;

// 控件尺寸一律走 ui_scale（只跟系统 DPI 走，不跟窗口大小走）。
// 之前这里做过「按窗口等比缩放」，后来窗口本身固定了尺寸，缩放就没有意义了。
#define gm_sc(px) ui_scale(px)

// 在组框里摆 n 行字段：行距按框高均分（夹在 0~24），整体在框内垂直居中。
// 带按钮的列不整体换布局：按钮从这一列的宽度里切一条出来，输入框让位，
// 标签仍然按整列宽摆 —— 左边那排标签的基线才不会因为多了按钮而错开。
static void gm_rows(HWND pw, const GmRect *b, const GmRow *r, int n) {
    const int inx = gm_sc(12), iny = gm_sc(22), inb = gm_sc(10);
    const int lbh = gm_sc(15), lgap = gm_sc(4), rh = gm_sc(24);
    const int nat = lbh + lgap + rh;
    const int qbw = gm_sc(66), qgap = gm_sc(6);       // 快捷按钮宽 / 与输入框的缝
    int inner = b->w - inx * 2;
    int half = (inner - gm_sc(10)) / 2;
    int avail = b->h - iny - inb;
    int gapv = (n > 1) ? (avail - n * nat) / (n - 1) : 0;
    if (gapv < 0) gapv = 0;
    if (gapv > gm_sc(22)) gapv = gm_sc(22);
    int used = n * nat + (n - 1) * gapv;
    int ytop = b->y + iny + (avail > used ? (avail - used) / 2 : 0);

    for (int i = 0; i < n; i++) {
        const GmRow *q = &r[i];
        int ry = ytop + i * (nat + gapv);
        int w1 = q->lab2 ? half : inner;
        int bx = b->x + inx;
        ui_place(ui_lbl_at(pw, q->lab), bx, ry, w1, lbh);
        int wc = q->btn ? w1 - qbw - qgap : w1;
        ui_place(q->ctl, bx, ry + lbh + lgap, wc, rh);
        if (q->btn) ui_place(q->btn, bx + wc + qgap, ry + lbh + lgap, qbw, rh);
        if (q->lab2) {
            int bx2 = b->x + inx + half + gm_sc(10);
            ui_place(ui_lbl_at(pw, q->lab2), bx2, ry, half, lbh);
            int wc2 = q->btn2 ? half - qbw - qgap : half;
            ui_place(q->ctl2, bx2, ry + lbh + lgap, wc2, rh);
            if (q->btn2) ui_place(q->btn2, bx2 + wc2 + qgap, ry + lbh + lgap, qbw, rh);
        }
    }
}

// 在组框里摆按钮网格：cols 列；最后一个按钮不满一行就拉满整行
// （「全服清错误物品」正好是这类重操作，独占一行反而更醒目）
static void gm_btns(HWND pw, const GmRect *b, const int *ids, int n, int cols) {
    const int inx = gm_sc(12), iny = gm_sc(22), inb = gm_sc(10);
    const int gp = gm_sc(8), bh = gm_sc(30);
    int rows = (n + cols - 1) / cols;
    int inner = b->w - inx * 2;
    int bw = (inner - (cols - 1) * gp) / cols;
    int avail = b->h - iny - inb;
    int gapv = rows > 1 ? (avail - rows * bh) / (rows - 1) : 0;
    if (gapv < 0) gapv = 0;
    if (gapv > gm_sc(20)) gapv = gm_sc(20);
    int used = rows * bh + (rows - 1) * gapv;
    int ytop = b->y + iny + (avail > used ? (avail - used) / 2 : 0);

    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        int cw = bw;
        if (i == n - 1 && n % cols) cw = inner - c * (bw + gp);    // 末行补满
        ui_place(GetDlgItem(pw, ids[i]), b->x + inx + c * (bw + gp), ytop + r * (bh + gapv),
                 cw, bh);
    }
}

// 一栏纵向分 n 个组框：按组高权重 wgt 分配，铺满整栏
static void gm_lane(int x, int w, int y, int h, int gap, const int *wgt, int n, GmRect *out) {
    int total = 0;
    for (int i = 0; i < n; i++) total += wgt[i];
    int avail = h - gap * (n - 1);
    int yy = y;
    for (int i = 0; i < n; i++) {
        int gh = total > 0 ? avail * wgt[i] / total : avail / n;
        if (i == n - 1) gh = y + h - yy;            // 末组吃掉除不尽的零头，正好贴底
        out[i].x = x; out[i].y = yy; out[i].w = w; out[i].h = gh;
        yy += gh + gap;
    }
}

static void gm_layout(Page *p, int w, int h) {
    HWND pw = p->wnd;

    static const int kBoxL[3] = {G_BOX_ROLE, G_BOX_ASSET, G_BOX_PVP};
    static const int kBoxR[3] = {G_BOX_ONCE, G_BOX_ACCT, G_BOX_MISC};
    static const int kOnce[] = {G_SP, G_QP, G_NOWEIGHT, G_SLOT, G_UNLOCK,
                                G_EXPERT, G_FATIGUE, G_DUNGEON, G_PUNISH, G_QCLR,
                                G_QEXP, G_QIMP};
    static const int kAcct[] = {G_UNLCREATE};
    static const int kMisc[] = {G_DUMP, G_CDUMP, G_RESTORE, G_SCANBAD, G_CLONE};
    // 按钮网格的行数一律由数组长度推，不再手写数字。之前往 kMisc 里加了「克隆角色」
    // 却忘了把这里的 4 改成 5，那个按钮就一直没被摆出来（坐标为 0）。
    const int rowsOnce = (COUNT_OF(kOnce) + 1) / 2;
    const int rowsMisc = (COUNT_OF(kMisc) + 1) / 2;

    // 组框高度权重 = 框内留白 + 行数 × 该类型一行的自然高。
    // 不按"行数"平分：字段行要 43px（标签 15 + 缝 4 + 框 24），按钮行 34px 就够，
    // 按行数平分会把按钮多的组撑得过松，两栏疏密观感对不上。
    // 窗口尺寸是固定的（见 main.c），所以这里不用再按可用空间反推缩放比例，
    // 直接按 ui_scale 排；组框再把剩余高度吃掉，两栏底部自然对齐。
    const int pad = gm_sc(10), gap = gm_sc(12);
    int cw = (w - pad * 2 - gap) / 2;
    int x0 = pad, x1 = pad + cw + gap;

    const int padY = gm_sc(22) + gm_sc(10);
    const int natF = gm_sc(15) + gm_sc(4) + gm_sc(24);   // 字段行
    const int natB = gm_sc(30) + gm_sc(4);               // 按钮行

    int wL[3], wR[3];
    for (int i = 0; i < 3; i++) wL[i] = padY + 2 * natF;
    wL[1] = padY + 3 * natF;        // 资产组三行（多了 VIP），按行数给高才不会挤成一团
    wL[2] = padY + 3 * natF;        // PVP 组也三行（段位/胜场/胜点纵向排）
    wR[0] = padY + rowsOnce * natB;
    wR[1] = padY + COUNT_OF(kAcct) * natB;
    wR[2] = padY + rowsMisc * natB;

    // 铺满整栏，但有个上限：窗口很高时别把组框拉成空盒子——内容最多长到自然高的
    // 1.3 倍。顶部照常贴齐，多出来的空间留在页面下方，和左栏列表、其它页的贴顶版面一致。
    // 两栏共用同一个高度，底部才会齐平。
    // 逐项相加而不是用单一行高估算：资产组已经比另两组高一行，
    // 再按同一数字乘三会低估左栏自然高度，算出来的上限会把内容框压矮。
    int natColL = wL[0] + wL[1] + wL[2] + 2 * gap;
    int natColR = wR[0] + wR[1] + wR[2] + 2 * gap;
    int natMax = natColL > natColR ? natColL : natColR;
    int cap = natMax + natMax * 3 / 10;
    int ch = h - pad * 2;
    if (ch > cap) ch = cap;
    int y0 = pad;

    GmRect L[3], R[3];
    gm_lane(x0, cw, y0, ch, gap, wL, 3, L);
    gm_lane(x1, cw, y0, ch, gap, wR, 3, R);
    for (int i = 0; i < 3; i++)
        ui_place(GetDlgItem(pw, kBoxL[i]), L[i].x, L[i].y, L[i].w, L[i].h);
    for (int i = 0; i < 3; i++)
        ui_place(GetDlgItem(pw, kBoxR[i]), R[i].x, R[i].y, R[i].w, R[i].h);

    GmRow role[2] = {
        {"角色名", c_name, NULL, "等级", c_lev, s_qadd[0]},
        {"职业",   c_job,  NULL, "觉醒", c_grow, NULL},
    };
    // VIP 单独占一行，点券与赠送点券拆成两个带标签的框：
    // 原先两者挤在右半格共用一个「点券 / 赠送」标题，插进 VIP 后那半格就成了三个控件，
    // 挤在一行里标签对不上框，不如让每行都是左框 / 右框的对称结构。
    // 五颗「+数量」跟着各自的框走（VIP 是勾选框，没有数值可加）
    GmRow asset[3] = {
        {"金币",     c_money, s_qadd[1], "复活币",   c_coin,  s_qadd[2]},
        {"账号金币", c_accm,  s_qadd[3], "VIP",      c_vip,   NULL},
        {"点券",     c_cera,  s_qadd[4], "赠送点券", c_point, s_qadd[5]},
    };
    // PVP 三项纵向排（段位 / 胜场 / 胜点），和参考工具一致。
    // 原来是「段位│胜场 / 分数│空」，两个数字框左右并排、下面还空半边，
    // 读的时候眼睛要在两列之间来回跳；纵向一行一项，扫下来就是一条线。
    GmRow pvp[3] = {
        {"段位", g_grade, NULL,       NULL, NULL, NULL},
        {"胜场", g_win,   s_qadd[6],  NULL, NULL, NULL},
        {"胜点", g_point, s_qadd[7],  NULL, NULL, NULL},
    };
    gm_rows(pw, &L[0], role, 2);
    gm_rows(pw, &L[1], asset, 3);
    gm_rows(pw, &L[2], pvp, COUNT_OF(pvp));
    gm_btns(pw, &R[0], kOnce, COUNT_OF(kOnce), 2);
    gm_btns(pw, &R[1], kAcct, COUNT_OF(kAcct), 1);
    gm_btns(pw, &R[2], kMisc, COUNT_OF(kMisc), 2);
}
static void gm_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[16] = {G_NAME, G_LEV, G_JOB, G_GROW, G_MONEY, G_COIN, G_CERA, G_POINT, G_ACCM,
                   G_GRADE, G_WIN, G_PVP_POINT, G_VIP};
    // 表单是失焦即写，而角色在线时写完会被服务端回写覆盖（程序会拒绝提交）。
    // 与其让人改半天发现没生效，不如在线时把这些框置灰，并把条件写在组框标题上——
    // 灰掉的控件收不到悬停，提示只能写在旁边的文字里。一键按钮不受此限（它们直接改库）。
    int edit = g.char_ok && !g.cur.online;
    for (int i = 0; i < 13; i++) ui_enable(GetDlgItem(p->wnd, ids[i]), edit);
    // 快捷按钮和它对应的输入框同生同死：框不能改的时候，按钮点了也没意义
    for (int i = 0; i < QADD_N; i++) ui_enable(GetDlgItem(p->wnd, G_QADD + i), edit);
    for (int i = 0; i < 10; i++) ui_enable(GetDlgItem(p->wnd, G_SP + i), on);
    ui_enable(GetDlgItem(p->wnd, G_QEXP), on);
    ui_enable(GetDlgItem(p->wnd, G_QIMP), on);
    static int s_edit = -1;
    if (edit != s_edit) {
        s_edit = edit;
        struct { int id; const char *base; } bx[3] = {
            {G_BOX_ROLE, "角色"}, {G_BOX_ASSET, "资产"}, {G_BOX_PVP, "PVP"},
        };
        for (int i = 0; i < 3; i++) {
            HWND h = GetDlgItem(p->wnd, bx[i].id);
            // 没选角色时不加任何后缀：那时候这些框是"还没东西可改"，不是"在线不能改"
            char *t = (!g.char_ok) ? fmt_new("%s", bx[i].base)
                      : edit       ? fmt_new("%s", bx[i].base)
                                   : fmt_new("%s（角色在线，退出游戏才能改）", bx[i].base);
            WCHAR w[96];
            utf16(t, w, COUNT_OF(w));
            if (h) SetWindowTextW(h, w);
        }
    }
    ui_enable(GetDlgItem(p->wnd, G_UNLCREATE), g.db_ok && g.char_ok);
    ui_enable(GetDlgItem(p->wnd, G_DUMP), g.db_ok);
    ui_enable(GetDlgItem(p->wnd, G_RESTORE), g.db_ok);
    ui_enable(GetDlgItem(p->wnd, G_CDUMP), g.db_ok && g.char_ok);
    ui_enable(GetDlgItem(p->wnd, G_SCANBAD), g.db_ok);
    ui_enable(GetDlgItem(p->wnd, G_CLONE), g.db_ok && g.char_ok);
}

static void gm_build(Page *p, HWND parent) {
    // 分组框先建：后建的控件在 z 轴上层，框才不会盖住输入框
    ui_group(parent, "角色", G_BOX_ROLE);
    ui_group(parent, "资产", G_BOX_ASSET);
    ui_group(parent, "PVP", G_BOX_PVP);
    ui_group(parent, "一键操作", G_BOX_ONCE);
    ui_group(parent, "账号", G_BOX_ACCT);
    ui_group(parent, "数据库维护", G_BOX_MISC);

    c_name = ui_edit(parent, "", G_NAME, 0);
    c_lev = ui_edit(parent, "", G_LEV, 0);
    // 下限 0：框里恒为 0 表示「这次没有要设的值」，等级也走这条 —— 若下限卡 1，
    // 用户点一下空框再离开，ui_num 会把 0 兜成 1，等于把角色打成 1 级。
    ui_num(c_lev, 0, gm_lev_max());
    job_labels();
    c_job = ui_combo(parent, G_JOB, 1);
    ui_combo_fill(c_job, kJobLbl, COUNT_OF(kJobList), 0);
    s_job_src = g.pvf_ok ? 1 : 0;       // 建页时已按当前来源填过，reload 不必重填
    c_grow = ui_combo(parent, G_GROW, 1);
    ui_combo_fill(c_grow, kGrowList, COUNT_OF(kGrowList), 0);
    // 创建顺序就是 Tab 顺序：这几行必须和视觉上的行列一致
    // （金币/复活币 → 账号金币/VIP → 点券/赠送点券），建反了 Tab 会在同一行里左右乱跳
    c_money = ui_edit(parent, "", G_MONEY, 0);
    c_coin = ui_edit(parent, "", G_COIN, 0);
    c_accm = ui_edit(parent, "", G_ACCM, 0);
    // 库里这几列是 int unsigned，框里就只认 0~2^32-1：敲不进去、粘不进来
    ui_num(c_money, 0, GM_U32_MAX);
    ui_num(c_coin, 0, GM_U32_MAX);
    ui_num(c_accm, 0, GM_U32_MAX);
    // VIP 是 0/1 标记，不是让人手填的文本：勾上 = 开通，取消勾选 = 关掉。
    // 原来是个自由输入框，用户不知道该填什么（填 0？1？日期？），而且只能设不能撤。
    c_vip = ui_check(parent, "已开通", G_VIP, 0);
    c_cera = ui_edit(parent, "", G_CERA, 0);
    c_point = ui_edit(parent, "", G_POINT, 0);
    ui_num(c_cera, 0, GM_U32_MAX);
    ui_num(c_point, 0, GM_U32_MAX);

    // 「+数量」快捷按钮按 kQAdd 的顺序建，ID 连号（点击靠 id - G_QADD 反查）。
    // 去掉 WS_TABSTOP：Tab 只该在输入框之间走，按钮挤进序列会让人每改一个数都多按几次。
    for (int i = 0; i < QADD_N; i++) {
        s_qadd[i] = ui_button(parent, kQAdd[i].txt, G_QADD + i);
        if (s_qadd[i])
            SetWindowLongPtrW(s_qadd[i], GWL_STYLE,
                              GetWindowLongPtrW(s_qadd[i], GWL_STYLE) & ~WS_TABSTOP);
    }

    pvp_labels();
    g_grade = ui_combo(parent, G_GRADE, 1);
    ui_combo_fill(g_grade, kPvpLabel, kPvpMax + 1, kPvpMax);   // 默认选最高段位
    // 初始文本留空：建带文本的 EDIT 会发 EN_CHANGE，页面处理函数可能在
    // 构建中途弹模态框，嵌套消息循环会把布局重入一遍。默认值放到末尾统一设。
    g_win = ui_edit(parent, "", G_WIN, 0);
    g_point = ui_edit(parent, "", G_PVP_POINT, 0);
    ui_num(g_win, 0, GM_PVP_MAX);
    ui_num(g_point, 0, GM_PVP_MAX);
    // 「库已连接」不再单独挂个标签：状态栏右侧本来就写着库/PVF/同步状态，
    // 页面上再放一个只是同一句话说两遍，还占掉维护组半行宽度

    // 按钮文字统一用参考工具（x/2026.3.14GMTOOL源码）的叫法：
    // 开装备槽→开启左右槽、解锁装备→取消装备等级限制、解锁全图→开启全图全难度、
    // 副职业满级→设置副职业满级。玩家在两边看到的是同一件事，叫法该一致。
    static const char *kBtn[] = {"满 SP/TP", "满 QP", "无限负重", "开启左右槽", "取消装备等级限制",
                                 "设置副职业满级", "补满疲劳", "开启全图全难度", "清除惩罚", "清空任务"};
    for (int i = 0; i < COUNT_OF(kBtn); i++)
        ui_button(parent, kBtn[i], G_SP + i);
    ui_button(parent, "导出任务模版", G_QEXP);
    ui_button(parent, "导入任务模版", G_QIMP);

    // 同上：解除建角限制在账号组里，位置比维护组那排按钮高，先建它
    ui_button(parent, "解除账号限制", G_UNLCREATE);
    ui_button(parent, "备份整库", G_DUMP);
    ui_button(parent, "备份角色", G_CDUMP);
    ui_button(parent, "还原备份", G_RESTORE);
    ui_button(parent, "全服清错误物品", G_SCANBAD);
    ui_button(parent, "克隆角色", G_CLONE);
    ui_tip(parent, GetDlgItem(parent, G_DUMP), "导出整库 SQL 到 config/backup（只读，不动库里数据）");
    ui_tip(parent, GetDlgItem(parent, G_CDUMP), "只导出当前角色的 15 张表，可用于单角色回滚");
    ui_tip(parent, GetDlgItem(parent, G_RESTORE), "选一个 .sql 备份导入数据库（写库，建议先备份一次）");
    ui_tip(parent, GetDlgItem(parent, G_UNLCREATE), "清掉当前角色所属账号的建角次数限制");
    ui_tip(parent, GetDlgItem(parent, G_SCANBAD),
           "扫描全服所有角色的背包/穿戴/宠物栏/仓库/珠宝/账号金库/邮件/时装/宠物，"
           "列出错误物品并一键清除（容器写前自动备份）");
    ui_tip(parent, GetDlgItem(parent, G_CLONE),
           "把当前角色整份复制成一个新角色：背包/穿戴/仓库/技能/任务/时装宠物一起复制，"
           "新角色自动取名（原名 + 数字后缀）。整个过程是一个事务，出错会整体回滚。"
           "账号共享的账号金库不复制");
    ui_tip(parent, GetDlgItem(parent, G_QEXP),
           "把当前角色的任务进度存成一份文本模版：已完成任务、20 个进行中任务槽及触发器、"
           "任务商店属性加成。存到 config/export，可手工改数值");
    ui_tip(parent, GetDlgItem(parent, G_QIMP),
           "选一份任务模版套到当前角色上：先删掉该角色现有的任务记录，再按模版写回。"
           "适合用大号的「主线全清」模版给小号跳过推图");
    // 字段都靠「点到别处」落库。最重要的一条写在最前面：角色在线时服务端会在
    // 登出/存档时把库里的值覆盖回去（工具这边看着是写成功了，进游戏又变回原样）。
    // 在线判定本身在有些端上不可靠（login_status 常为 0、member_play_info 无行），
    // 所以这里不只靠置灰，把条件直接写出来。
    // 数值框收的是「设成多少」，和库里当前值一致：填好点到别处就按这个数写库。
    // 要「加一笔」走右边的 + 按钮，那条路会把加法结果写回框，两个语义不混。
    ui_tip(parent, c_name, "改完点到别处即写入库；重名会退回原值。角色在线时改会被服务端存档覆盖——要改先退出游戏");
    // 这些框都是「一次性设定框」：填好点到别处就写库，然后框自己清回 0。
    // 提示必须把这两点说清（留 0 = 不改），否则用户会以为框里应该显示当前值。
    // 上限写进提示：不同端封顶不同，写死「1~100」会让人以为能填到 100
    ui_tip(parent, c_lev,
           fmt_new("填「设成几级」（1~%d，上限在 settings.ini 的 [gm] lev_max），"
                   "点到别处按这个数写库、经验清零，框随即回 0。"
                   "留 0 = 不改；要往上加几级用右边 +10。当前等级看左边角色信息",
                   gm_lev_max()));
    ui_tip(parent, c_money,
           "填「设成多少金币」（0~4294967295），点到别处写库，框随即回 0。"
           "留 0 = 不改；要加一笔用右边 +1000万。当前值看左边角色信息");
    ui_tip(parent, c_coin,
           "填「设成多少复活币」，点到别处写库，框随即回 0。留 0 = 不改；要加一笔用右边 +100");
    ui_tip(parent, c_accm,
           "账号级金币，同账号所有角色共用。填「设成多少」，点到别处写库，框随即回 0。"
           "留 0 = 不改；要加一笔用右边 +1000万。账号金库记录不存在时会自动建一条");
    ui_tip(parent, c_vip,
           "账号级 VIP 标记（d_taiwan.accounts.VIP），同账号所有角色共用。"
           "勾上写 1，取消勾选写 0 —— 不用自己填。"
           "角色在线时改了会被服务端存档覆盖");
    ui_tip(parent, c_cera,
           "填「设成多少点券」，点到别处写库，框随即回 0；留 0 = 这一项不改。"
           "点券和赠送点券是同一条 UPDATE，只填一个就只改那一个");

    // 快捷按钮上只有「+数量」：点一下在「当前值」上加，不经过输入框
    ui_tip(parent, s_qadd[0], fmt_new("在当前等级上加 10 级（封顶 %d），立刻写库；不动输入框",
                                      gm_lev_max()));
    ui_tip(parent, s_qadd[1], "在当前金币上加 1000 万，立刻写库；不动输入框（上限 4294967295）");
    ui_tip(parent, s_qadd[2], "在当前复活币上加 100，立刻写库；不动输入框");
    ui_tip(parent, s_qadd[3], "在当前账号金币上加 1000 万，立刻写库；不动输入框");
    ui_tip(parent, s_qadd[4], "在当前点券上加 1 万，立刻写库；赠送点券一起提交");
    ui_tip(parent, s_qadd[5], "在当前赠送点券上加 1 万，立刻写库；点券一起提交");
    ui_tip(parent, s_qadd[6], "在当前胜场上加 100，立刻写库；不动输入框");
    ui_tip(parent, s_qadd[7], "在当前胜点上加 1000，立刻写库；不动输入框");

    ui_set_int(g_win, 0);
    ui_set_int(g_point, 0);
    (void)p;
}

void page_gm_create(Page *p) {
    p->build = gm_build;
    p->layout = gm_layout;
    p->reload = gm_reload;
    p->lock = gm_lock;
    p->on = gm_on;
    // 断开的泄洪要连本页的写队列和扫描闸一起放下：注册在页面自己这边，
    // app.c 不必认识页面名，以后加页面也不会漏掉这条路径
    app_on_drop(gm_drop);
}
