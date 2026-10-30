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
    G_DUMP, G_RESTORE, G_CDUMP, G_UNLCREATE, G_SCANBAD,
    // 泡点：开关 / 间隔 / 点数 / 应用 / 状态行
    G_BON, G_BSEC, G_BVAL, G_BAPPLY, G_BINFO,
    // 分组框：纯视觉容器，不参与 enable / 提交。ID 单独起一段，避开上面连续递增的控件号
    G_BOX_ROLE = PAGE_ID(PAGE_GM, 60), G_BOX_ASSET, G_BOX_PVP, G_BOX_ONCE, G_BOX_ACCT,
    G_BOX_MISC, G_BOX_BUB
};

static HWND c_name, c_lev, c_job, c_grow, c_money, c_coin, c_cera, c_point, c_accm;
static HWND g_grade, g_win, g_point;
// 泡点组：开关、间隔秒、每次点数、状态行
static HWND c_bon, c_bsec, c_bval, c_binfo;
static char s_binfo_last[192];   // 状态行只在文字变了才写，SetWindowText 会重绘

// 全服清错误物品的状态（实现在本文件后面）
static Vec  s_scan;
static int  s_scan_chars;        // 本次扫到的角色数
static int  s_scan_busy;         // 后台任务在跑（扫描或清除）
static int  s_phase;             // 0=扫描完成 1=清除完成、待复扫
static int  s_scan_open;         // 扫完自动弹窗（首次点按钮时）
static HWND s_slv, s_sinfo;

// 职业/觉醒都做成下拉：值→文字，避免让人对着裸数字猜
static const char *kJobList[] = {"未转职", "鬼剑士", "格斗家", "神枪手", "魔法师", "圣职者",
                                 "盗贼",   "男法师", "缔造者", "黑暗武士", "影舞者"};
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
        case K_JOB: return "设置职业";
        case K_PVP: return "设置 PVP";
        case K_RENAME: return "改名";
        default: return "写入";
    }
}

static void gm_kick(void) {
    if (!s_gmq_n || !app_act_idle()) return;   // 连接忙时先不弹队，等连接空闲后由 page_gm_pump 发
    s_gm_run = s_gmq[0];
    memmove(s_gmq, s_gmq + 1, sizeof(Ctx) * (size_t)(s_gmq_n - 1));
    s_gmq_n--;
    app_act(gm_what(s_gm_run.kind), char_work, &s_gm_run);
}

// 泡点状态行：每 tick 会被问一次，文字没变就不写（SetWindowText 会触发重绘）
static void gm_bubble_status_sync(void) {
    if (!c_binfo) return;
    char buf[192];
    app_bubble_status(buf, sizeof(buf));
    if (!strcmp(buf, s_binfo_last)) return;
    snprintf(s_binfo_last, sizeof(s_binfo_last), "%s", buf);
    ui_text(c_binfo, buf);
}

// 队列的推进挂在这里，而不是挂在 gm_reload 上。
// 挂在 reload 上意味着"页面不刷新 → 队列不发"，于是隐藏页也必须跟着刷，
// 刷新就从"让界面跟上"变成了写入流程的一环——这条耦合正是各种刷新打架的根源。
// 现在每 tick 推进一次，与页面可见性无关；reload 只管显示。
void page_gm_pump(void) {
    gm_kick();
    gm_bubble_status_sync();       // 倒计时和上次结算结果都在这行里
}

static void gm_refill(void);          // 实现见下（gm_push 队列满时要回滚界面）

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
    if (s_scan_open && s_scan_busy && !job_running(&g.job_act)) {
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
    ui_set_int(c_lev, g.cur.lev);
    ui_combo_set(c_job, g.cur.job >= 0 && g.cur.job < COUNT_OF(kJobList) ? g.cur.job : 0);
    ui_combo_set(c_grow, g.cur.grow >= 0 && g.cur.grow < COUNT_OF(kGrowList) ? g.cur.grow : 0);
    ui_set_i64(c_money, (i64)g.data.money);
    ui_set_i64(c_coin, (i64)g.data.coin);
    ui_set_i64(c_cera, (i64)g.data.cera);
    ui_set_i64(c_point, (i64)g.data.cera_point);
    ui_set_i64(c_accm, (i64)g.data.account_money);

    int gr = g.data.pvp_grade;
    if (gr < 0) gr = 0;
    if (gr > kPvpMax) gr = kPvpMax;
    ui_combo_set(g_grade, gr);
    ui_set_int(g_win, g.data.pvp_win);
    ui_set_int(g_point, g.data.pvp_point);
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

// ---------------- 全服清错误物品（维护动作，不挑角色） ----------------
// 扫描全服所有角色的容器、邮件附件、时装/宠物表，列出错误物品并可一键清除。
// 名字是否存在的判定交给 PVF，未加载时退化成只查结构
static int scan_has_name(u32 id, void *ctx) {
    (void)ctx;
    return g.pvf_ok && *pvf_item_name(&g.pvf, id);
}

static int scan_work(void *ctx, Err err) {
    (void)ctx;
    return db_scan_bad(&g.db, &s_scan, &s_scan_chars, scan_has_name, NULL, err);
}

static int purge_work(void *ctx, Err err) {
    (void)ctx;
    int done = 0;
    db_purge_bad(&g.db, &s_scan, &done, err);
    app_log("清除错误物品 · 完成 %d 处", done);
    return 1;
}

static void scan_run(void) {
    s_scan_busy = 1;
    app_act("全服扫描", scan_work, NULL);
}

static void scan_start(void) {
    if (!s_scan.cap) vec_init(&s_scan, sizeof(ScanRow));
    s_scan.n = 0;
    s_scan_chars = 0;
    app_status("正在扫描全服…");
    scan_run();
}

static void on_scanbad(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (s_scan_busy) { app_info("上一轮还没跑完。"); return; }
    if (!app_confirm("扫描全服所有角色的容器、邮件、时装、宠物，找出错误物品？\n\n"
                     "角色较多时需要一点时间。"))
        return;
    scan_start();
    s_scan_open = 1;             // 扫完自动弹窗
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
        if (s_scan_busy && !job_running(&g.job_act)) {
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
    if (m == WM_DESTROY) { KillTimer(d, 1); s_slv = s_sinfo = NULL; return 0; }
    if (m == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case 6001: {                             // 导出 CSV
            char path[MAX_PATH], dir[MAX_PATH], ts[32];
            art_dir("export", dir, sizeof(dir));
            art_stamp(ts, sizeof(ts));
            snprintf(path, sizeof(path), "%s\\badscan-%s.csv", dir, ts);
            Str o = {0};
            str_cat(&o, "\xEF\xBB\xBF角色,账号,角色编号,容器,槽位,物品编号,物品名,原因\r\n");
            for (int i = 0; i < s_scan.n; i++) {
                ScanRow *r = VEC_AT(&s_scan, ScanRow, i);
                char nm[96];
                str_catf(&o, "%s,%s,%u,%s,%d,%u,%s,%s\r\n", r->name, r->acc, r->cno, r->cont,
                         r->slot, r->id, app_item_name(r->id, nm, sizeof(nm)), r->why);
            }
            file_write(path, o.p, (size_t)o.n);
            str_free(&o);
            art_prune(dir, "badscan-", 5);
            app_log("导出全服扫描结果 %d 行 → %s", s_scan.n, path);
            app_info(fmt_new("已导出到\n%s", path));
            return 0;
        }
        case 6002:                               // 清除全部
            if (s_scan_busy) return 0;
            if (!s_scan.n) { app_info("没有需要清除的记录。"); return 0; }
            if (!app_confirm(fmt_new("清除全部 %d 处错误物品？\n\n"
                                     "容器槽位写成空槽（原数据自动备份到 config/backup），\n"
                                     "时装/宠物表删行，邮件只清附件、金币保留。", s_scan.n)))
                return 0;
            s_phase = 1;
            s_scan_busy = 1;
            app_act("清除错误物品", purge_work, NULL);
            ui_text(s_sinfo, "正在清除…完成后自动重新扫描。");
            return 0;
        case 6003:                               // 重新扫描
            if (s_scan_busy) return 0;
            scan_start();
            ui_text(s_sinfo, "正在扫描…");
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
    ui_place(s_slv, ui_scale(12), ui_scale(34), ui_scale(756), ui_scale(360));

    ui_place(ui_label(d, "清除＝容器错误槽位写成空槽（自动备份）、时装/宠物表删行、邮件清附件留金币。", 0),
             ui_scale(12), ui_scale(402), ui_scale(560), ui_scale(20));
    ui_place(ui_button(d, "清除全部", 6002), ui_scale(400), ui_scale(432), ui_scale(88), ui_scale(26));
    ui_place(ui_button(d, "导出 CSV", 6001), ui_scale(496), ui_scale(432), ui_scale(88), ui_scale(26));
    ui_place(ui_button(d, "重新扫描", 6003), ui_scale(592), ui_scale(432), ui_scale(88), ui_scale(26));
    ui_place(ui_button(d, "关闭", IDCANCEL), ui_scale(688), ui_scale(432), ui_scale(80), ui_scale(26));

    scan_fill();
    SetTimer(d, 1, 200, NULL);
    ui_dlg_run(d, g_main);
}

// 失焦即写：只有值真的变了才发一条 UPDATE（点来点去、来回切页都不产生流量）。
// 校验不过就退回快照里的值，不弹框打断——失焦时弹模态框会跟焦点切换打架。
static void gm_commit(int id) {
    if (!g.char_ok) return;
    if (g.cur.online) {
        app_status("角色在线，改动未写入——请先退出游戏");
        gm_refill();
        return;
    }
    Ctx c;
    memset(&c, 0, sizeof(c));
    switch (id) {
        case G_LEV: {
            int v = ui_int(c_lev);
            if (v < 1 || v > 100) {
                app_status("等级范围 1~100，已退回原值");
                gm_refill();
                return;
            }
            if (v == g.cur.lev) return;
            c.i1 = v;
            c.u1 = (u64)(g.pvf_ok ? pvf_exp_for(&g.pvf, v) : -1);
            gm_push(K_LEV, &c);
            return;
        }
        case G_MONEY: {
            i64 v = ui_i64(c_money);
            if (v < 0) v = 0;
            if (v == (i64)g.data.money) return;
            c.u1 = (u64)v;
            gm_push(K_MONEY, &c);
            return;
        }
        case G_COIN: {
            i64 v = ui_i64(c_coin);
            if (v < 0) v = 0;
            if (v == (i64)g.data.coin) return;
            c.u1 = (u64)v;
            gm_push(K_COIN, &c);
            return;
        }
        case G_CERA:          // 点券和赠送点券是一条 UPDATE，两个框谁失焦都一起提交
        case G_POINT: {
            i64 ce = ui_i64(c_cera), pt = ui_i64(c_point);
            if (ce < 0) ce = 0;
            if (pt < 0) pt = 0;
            if (ce == (i64)g.data.cera && pt == (i64)g.data.cera_point) return;
            c.u1 = (u64)ce;
            c.u2 = (u64)pt;
            gm_push(K_CERA, &c);
            return;
        }
        case G_ACCM: {
            i64 v = ui_i64(c_accm);
            if (v < 0) v = 0;
            if (v == (i64)g.data.account_money) return;
            c.u1 = (u64)v;
            gm_push(K_ACCM, &c);
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
            int gr = ui_combo_sel(g_grade), win = ui_int(g_win), pt = ui_int(g_point);
            if (gr < 0) gr = 0;
            if (gr > kPvpMax) gr = kPvpMax;
            if (win < 0) win = 0;
            if (pt < 0) pt = 0;
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
    if (id == G_DUMP) { on_dump(); return 0; }
    if (id == G_RESTORE) { on_restore(); return 0; }
    if (id == G_CDUMP) { on_char_dump(); return 0; }
    if (id == G_SCANBAD) { on_scanbad(); return 0; }
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
    if (id == G_BAPPLY) {
        // 泡点是账号级/全服设置，不依赖当前选中角色，所以放在 need_char 之前
        int on = (int)SendMessageW(c_bon, BM_GETCHECK, 0, 0) == BST_CHECKED;
        app_bubble_apply(on, (int)ui_i64(c_bsec), (int)ui_i64(c_bval));
        gm_bubble_status_sync();
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
typedef struct {
    const char *lab;       // 左列标签（twin 行是合并标签，如"点券 / 赠送"）
    HWND        ctl;       // 左列控件
    const char *lab2;      // 右列标签（NULL = 这行只有一列）
    HWND        ctl2;      // 右列控件
    HWND        ctl3;      // twin 行：右列拆两半时的第二个框
    int         twin;      // 右列放两个等宽框（点券 + 赠送点券）
} GmRow;

typedef struct { int x, y, w, h; } GmRect;

// 在组框里摆 n 行字段：行距按框高均分（夹在 0~24），整体在框内垂直居中
static void gm_rows(HWND pw, const GmRect *b, const GmRow *r, int n) {
    const int inx = ui_scale(12), iny = ui_scale(22), inb = ui_scale(10);
    const int lbh = ui_scale(15), lgap = ui_scale(4), rh = ui_scale(24);
    const int nat = lbh + lgap + rh;
    int inner = b->w - inx * 2;
    int half = (inner - ui_scale(10)) / 2;
    int avail = b->h - iny - inb;
    int gapv = (n > 1) ? (avail - n * nat) / (n - 1) : 0;
    if (gapv < 0) gapv = 0;
    if (gapv > ui_scale(22)) gapv = ui_scale(22);
    int used = n * nat + (n - 1) * gapv;
    int ytop = b->y + iny + (avail > used ? (avail - used) / 2 : 0);

    for (int i = 0; i < n; i++) {
        const GmRow *q = &r[i];
        int ry = ytop + i * (nat + gapv);
        if (q->twin) {                       // 右列两个框：点券 / 赠送
            int bw = (half - ui_scale(6)) / 2;
            int bx = b->x + inx + half + ui_scale(10);
            ui_place(ui_lbl_at(pw, q->lab), b->x + inx, ry, half, lbh);
            ui_place(q->ctl, b->x + inx, ry + lbh + lgap, half, rh);
            ui_place(ui_lbl_at(pw, q->lab2), bx, ry, half, lbh);
            ui_place(q->ctl2, bx, ry + lbh + lgap, bw, rh);
            ui_place(q->ctl3, bx + bw + ui_scale(6), ry + lbh + lgap, bw, rh);
            continue;
        }
        int w1 = q->lab2 ? half : inner;
        ui_place(ui_lbl_at(pw, q->lab), b->x + inx, ry, w1, lbh);
        ui_place(q->ctl, b->x + inx, ry + lbh + lgap, w1, rh);
        if (q->lab2) {
            int bx = b->x + inx + half + ui_scale(10);
            ui_place(ui_lbl_at(pw, q->lab2), bx, ry, half, lbh);
            ui_place(q->ctl2, bx, ry + lbh + lgap, half, rh);
        }
    }
}

// 在组框里摆按钮网格：cols 列；最后一个按钮不满一行就拉满整行
// （「全服清错误物品」正好是这类重操作，独占一行反而更醒目）
static void gm_btns(HWND pw, const GmRect *b, const int *ids, int n, int cols) {
    const int inx = ui_scale(12), iny = ui_scale(22), inb = ui_scale(10);
    const int gp = ui_scale(8), bh = ui_scale(30);
    int rows = (n + cols - 1) / cols;
    int inner = b->w - inx * 2;
    int bw = (inner - (cols - 1) * gp) / cols;
    int avail = b->h - iny - inb;
    int gapv = rows > 1 ? (avail - rows * bh) / (rows - 1) : 0;
    if (gapv < 0) gapv = 0;
    if (gapv > ui_scale(20)) gapv = ui_scale(20);
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

// 泡点组：开关一行、两个字段一行、应用 + 状态一行。
// 高度按「一行开关 + 一行字段 + 一行按钮」给权重，见 gm_layout 里的 wR[3]。
static void gm_bubble(HWND pw, const GmRect *b) {
    const int inx = ui_scale(12), iny = ui_scale(22);
    const int lbh = ui_scale(15), lgap = ui_scale(4), rh = ui_scale(24);
    const int inner = b->w - inx * 2;
    const int half = (inner - ui_scale(10)) / 2;

    int ry = b->y + iny;
    ui_place(c_bon, b->x + inx, ry, inner, rh);
    ry += rh + ui_scale(8);

    ui_place(ui_lbl_at(pw, "间隔（秒）"), b->x + inx, ry, half, lbh);
    ui_place(c_bsec, b->x + inx, ry + lbh + lgap, half, rh);
    int bx = b->x + inx + half + ui_scale(10);
    ui_place(ui_lbl_at(pw, "每次加（点）"), bx, ry, half, lbh);
    ui_place(c_bval, bx, ry + lbh + lgap, half, rh);
    ry += lbh + lgap + rh + ui_scale(8);

    ui_place(GetDlgItem(pw, G_BAPPLY), b->x + inx, ry, ui_scale(72), ui_scale(28));
    ui_place(c_binfo, b->x + inx + ui_scale(80), ry + ui_scale(5), inner - ui_scale(80),
             ui_scale(20));
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
    const int pad = ui_scale(10), gap = ui_scale(12);
    int cw = (w - pad * 2 - gap) / 2;
    int x0 = pad, x1 = pad + cw + gap;

    // 组框高度权重 = 框内留白 + 行数 × 该类型一行的自然高。
    // 不按"行数"平分：字段行要 43px（标签 15 + 缝 4 + 框 24），按钮行 34px 就够，
    // 按行数平分会把按钮多的组撑得过松，两栏疏密观感对不上。
    const int padY = ui_scale(22) + ui_scale(10);
    const int natF = ui_scale(15) + ui_scale(4) + ui_scale(24);   // 字段行
    const int natB = ui_scale(30) + ui_scale(4);                  // 按钮行

    static const int kBoxL[3] = {G_BOX_ROLE, G_BOX_ASSET, G_BOX_PVP};
    static const int kBoxR[4] = {G_BOX_ONCE, G_BOX_ACCT, G_BOX_MISC, G_BOX_BUB};
    static const int kOnce[] = {G_SP, G_QP, G_NOWEIGHT, G_SLOT, G_UNLOCK,
                                G_EXPERT, G_FATIGUE, G_DUNGEON, G_PUNISH, G_QCLR};
    static const int kAcct[] = {G_UNLCREATE};
    static const int kMisc[] = {G_DUMP, G_CDUMP, G_RESTORE, G_SCANBAD};
    int wL[3], wR[4];
    for (int i = 0; i < 3; i++) wL[i] = padY + 2 * natF;
    wR[0] = padY + 5 * natB;        // 一键：10 个按钮，2 列 5 行
    wR[1] = padY + 1 * natB;        // 账号：解除建角限制
    wR[2] = padY + 2 * natB;        // 维护：4 个按钮，2 列 2 行
    wR[3] = padY + natB + natF + ui_scale(26);   // 泡点：开关 + 字段行 + 应用行

    // 铺满整栏，但有个上限：窗口很高时别把组框拉成空盒子——内容最多长到自然高的
    // 1.3 倍。顶部照常贴齐，多出来的空间留在页面下方，和左栏列表、其它页的贴顶版面一致。
    // 两栏共用同一个高度，底部才会齐平。
    int natColL = 3 * wL[0] + 2 * gap;
    int natColR = wR[0] + wR[1] + wR[2] + wR[3] + 3 * gap;
    int natMax = natColL > natColR ? natColL : natColR;
    int cap = natMax + natMax * 3 / 10;
    int ch = h - pad * 2;
    if (ch > cap) ch = cap;
    int y0 = pad;

    GmRect L[3], R[4];
    gm_lane(x0, cw, y0, ch, gap, wL, 3, L);
    gm_lane(x1, cw, y0, ch, gap, wR, 4, R);
    for (int i = 0; i < 3; i++)
        ui_place(GetDlgItem(pw, kBoxL[i]), L[i].x, L[i].y, L[i].w, L[i].h);
    for (int i = 0; i < 4; i++)
        ui_place(GetDlgItem(pw, kBoxR[i]), R[i].x, R[i].y, R[i].w, R[i].h);

    GmRow role[2] = {
        {"角色名", c_name, "等级", c_lev, NULL, 0},
        {"职业",   c_job,  "觉醒", c_grow, NULL, 0},
    };
    GmRow asset[2] = {
        {"金币",     c_money, "复活币",       c_coin, NULL,   0},
        {"账号金币", c_accm,  "点券 / 赠送",  c_cera, c_point, 1},
    };
    GmRow pvp[2] = {
        {"段位", g_grade, "胜场", g_win, NULL, 0},
        {"分数", g_point, NULL,   NULL,  NULL, 0},
    };
    gm_rows(pw, &L[0], role, 2);
    gm_rows(pw, &L[1], asset, 2);
    gm_rows(pw, &L[2], pvp, 2);
    gm_btns(pw, &R[0], kOnce, 10, 2);
    gm_btns(pw, &R[1], kAcct, 1, 1);
    gm_btns(pw, &R[2], kMisc, 4, 2);
    gm_bubble(pw, &R[3]);
}
static void gm_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[16] = {G_NAME, G_LEV, G_JOB, G_GROW, G_MONEY, G_COIN, G_CERA, G_POINT, G_ACCM,
                   G_GRADE, G_WIN, G_PVP_POINT};
    // 表单是失焦即写，而角色在线时写完会被服务端回写覆盖（程序会拒绝提交）。
    // 与其让人改半天发现没生效，不如在线时把这些框置灰，并把条件写在组框标题上——
    // 灰掉的控件收不到悬停，提示只能写在旁边的文字里。一键按钮不受此限（它们直接改库）。
    int edit = g.char_ok && !g.cur.online;
    for (int i = 0; i < 12; i++) ui_enable(GetDlgItem(p->wnd, ids[i]), edit);
    for (int i = 0; i < 10; i++) ui_enable(GetDlgItem(p->wnd, G_SP + i), on);
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
}

static void gm_build(Page *p, HWND parent) {
    // 分组框先建：后建的控件在 z 轴上层，框才不会盖住输入框
    ui_group(parent, "角色", G_BOX_ROLE);
    ui_group(parent, "资产", G_BOX_ASSET);
    ui_group(parent, "PVP", G_BOX_PVP);
    ui_group(parent, "一键操作", G_BOX_ONCE);
    ui_group(parent, "账号", G_BOX_ACCT);
    ui_group(parent, "数据库维护", G_BOX_MISC);
    ui_group(parent, "泡点", G_BOX_BUB);

    c_name = ui_edit(parent, "", G_NAME, 0);
    c_lev = ui_edit(parent, "", G_LEV, 0);
    job_labels();
    c_job = ui_combo(parent, G_JOB, 1);
    ui_combo_fill(c_job, kJobLbl, COUNT_OF(kJobList), 0);
    s_job_src = g.pvf_ok ? 1 : 0;       // 建页时已按当前来源填过，reload 不必重填
    c_grow = ui_combo(parent, G_GROW, 1);
    ui_combo_fill(c_grow, kGrowList, COUNT_OF(kGrowList), 0);
    // 创建顺序就是 Tab 顺序：这几行必须和视觉上的行列一致
    // （金币/复活币 → 账号金币/点券/赠送），建反了 Tab 会在同一行里左右乱跳
    c_money = ui_edit(parent, "", G_MONEY, 0);
    c_coin = ui_edit(parent, "", G_COIN, 0);
    c_accm = ui_edit(parent, "", G_ACCM, 0);
    c_cera = ui_edit(parent, "", G_CERA, 0);
    c_point = ui_edit(parent, "", G_POINT, 0);

    pvp_labels();
    g_grade = ui_combo(parent, G_GRADE, 1);
    ui_combo_fill(g_grade, kPvpLabel, kPvpMax + 1, kPvpMax);   // 默认选最高段位
    // 初始文本留空：建带文本的 EDIT 会发 EN_CHANGE，页面处理函数可能在
    // 构建中途弹模态框，嵌套消息循环会把布局重入一遍。默认值放到末尾统一设。
    g_win = ui_edit(parent, "", G_WIN, 0);
    g_point = ui_edit(parent, "", G_PVP_POINT, 0);
    // 「库已连接」不再单独挂个标签：状态栏右侧本来就写着库/PVF/同步状态，
    // 页面上再放一个只是同一句话说两遍，还占掉维护组半行宽度

    static const char *kBtn[] = {"满 SP/TP", "满 QP", "无限负重", "开装备槽", "解锁装备",
                                 "副职业满级", "补满疲劳", "解锁全图", "清除惩罚", "清空任务"};
    for (int i = 0; i < COUNT_OF(kBtn); i++)
        ui_button(parent, kBtn[i], G_SP + i);

    // 同上：解除建角限制在账号组里，位置比维护组那排按钮高，先建它
    ui_button(parent, "解除建角限制", G_UNLCREATE);
    ui_button(parent, "备份整库", G_DUMP);
    ui_button(parent, "备份角色", G_CDUMP);
    ui_button(parent, "还原备份", G_RESTORE);
    ui_button(parent, "全服清错误物品", G_SCANBAD);
    ui_tip(parent, GetDlgItem(parent, G_DUMP), "导出整库 SQL 到 config/backup（只读，不动库里数据）");
    ui_tip(parent, GetDlgItem(parent, G_CDUMP), "只导出当前角色的 15 张表，可用于单角色回滚");
    ui_tip(parent, GetDlgItem(parent, G_RESTORE), "选一个 .sql 备份导入数据库（写库，建议先备份一次）");
    ui_tip(parent, GetDlgItem(parent, G_UNLCREATE), "清掉当前角色所属账号的建角次数限制");
    ui_tip(parent, GetDlgItem(parent, G_SCANBAD),
           "扫描全服所有角色的背包/穿戴/宠物栏/仓库/珠宝/账号金库/邮件/时装/宠物，"
           "列出错误物品并一键清除（容器写前自动备份）");
    // 字段都靠「点到别处」落库。最重要的一条写在最前面：角色在线时服务端会在
    // 登出/存档时把库里的值覆盖回去（工具这边看着是写成功了，进游戏又变回原样）。
    // 在线判定本身在有些端上不可靠（login_status 常为 0、member_play_info 无行），
    // 所以这里不只靠置灰，把条件直接写出来。
    ui_tip(parent, c_name, "改完点到别处即写入库；重名会退回原值。角色在线时改会被服务端存档覆盖——要改先退出游戏");
    ui_tip(parent, c_lev, "1~100；改完点到别处即写入库，经验按 PVF 表同步。角色在线时改了会被服务端覆盖");
    ui_tip(parent, c_money, "改完点到别处即写入库。角色在线时改了会被服务端存档覆盖");
    ui_tip(parent, c_coin, "改完点到别处即写入库。角色在线时改了会被服务端存档覆盖");
    ui_tip(parent, c_accm,
           "账号级金币，同账号所有角色共用；改完点到别处即写入库。"
           "账号金库记录不存在时会自动建一条（容量/物品为空）");
    ui_tip(parent, c_cera, "点券和赠送点券一起提交（同一条 UPDATE）。角色在线时改了会被服务端覆盖");

    // 泡点组：全服设置，跟选中哪个角色无关。开关 → 间隔/点数 → 应用，
    // 创建顺序就是 Tab 顺序，所以按这个次序建。
    c_bon = ui_check(parent, "启用泡点", G_BON, 0);
    c_bsec = ui_edit(parent, "", G_BSEC, 0);
    c_bval = ui_edit(parent, "", G_BVAL, 0);
    ui_button(parent, "应用", G_BAPPLY);
    c_binfo = ui_label(parent, "", G_BINFO);
    ui_tip(parent, c_bon, "开启后每隔一段时间给每个在线账号加赠送点券；只在工具开着时结算，关掉不补算");
    ui_tip(parent, c_bsec, "结算间隔，单位秒，下限 5");
    ui_tip(parent, c_bval, "每次给每个在线账号加多少赠送点券");
    ui_tip(parent, GetDlgItem(parent, G_BAPPLY),
           "把开关/间隔/点数写进 settings.ini，立即生效并重新计时");
    // PVP 默认值：最高段位 + 胜场/分数 999（选中角色后由 gm_reload 覆盖为实际值）
    ui_set_int(g_win, 999);
    ui_set_int(g_point, 999);
    // 泡点初值来自 settings.ini；ui_check/ui_edit 建的时候留空，在这里一次设好
    if (g.cfg.bubble_on) SendMessageW(c_bon, BM_SETCHECK, BST_CHECKED, 0);
    ui_set_int(c_bsec, g.cfg.bubble_sec);
    ui_set_int(c_bval, g.cfg.bubble_val);
    gm_bubble_status_sync();
    (void)p;
}

void page_gm_create(Page *p) {
    p->build = gm_build;
    p->layout = gm_layout;
    p->reload = gm_reload;
    p->lock = gm_lock;
    p->on = gm_on;
}
