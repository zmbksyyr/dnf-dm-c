// main：WinMain、主窗口、工具栏、页签容器、角色卡、状态栏、DPI、拖入 PVF、取码热键。
#include "ui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

HWND g_main;

static HINSTANCE g_hinst;

static HWND g_tab, g_status;
static HWND g_lhost, g_host, g_lport, g_port, g_luser, g_user, g_lpwd, g_pwd;
static HWND g_conn, g_disc;
static HWND g_lkw, g_kw, g_kwgo, g_kwclr, g_chlist, g_chinfo, g_online;
static HWND g_lpvf, g_pvf, g_pvfbtn;
static HWND g_limg, g_img, g_imgbtn;      // ImagePacks2 目录（悬浮卡图标来源）

// 地址预设：Python 版 GM 工具 config.json 里的 saved_servers / 默认 DB_IP。
// 本机自架的服最常见，127.0.0.1 排第一；其余是人家现成配过的机器。
// 框是可编辑的下拉，不在预设里直接敲也行，敲过的照样存进 settings.ini。
static const char *const kHostList[] = {"127.0.0.1", "192.168.200.131"};

// 密码预置：常见私服端默认的两套口令。下拉框是「可编辑」的（CBS_DROPDOWN），
// 不在预置里就直接敲，不会被限制成只能选。
// 注意：这里是**明文**，界面不掩码、settings.ini 不加密，这是有意为之，不是漏做。
// 理由：它是本机连库用的口令，藏成圆点之后看不见、改不动、连不上时也核对不了；
// 而真要防「有人看到这台机器」，掩码和本地加密都挡不住（进程内存、ini 文件都在同一台机）。
// 见 tools/v_pwd.py —— 那条用例把这个约定钉住了，免得以后有人"顺手"改成密码框。
static const char *const kPwdList[] = {"uu5!^%jg", "123456"};

// 把配置里的地址放回下拉框：命中预设就选中，不命中就当成手填的文本。
// 切回来时要能看出"现在连的是哪台"，而不是永远停在第一个预设上。
static void host_sync(void) {
    if (!g_host || !g.cfg.host[0]) return;
    WCHAR w[256];
    utf16(g.cfg.host, w, COUNT_OF(w));
    int idx = (int)SendMessageW(g_host, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w);
    if (idx >= 0) {
        ui_combo_set(g_host, idx);
        return;
    }
    SendMessageW(g_host, CB_INSERTSTRING, 0, (LPARAM)w);
    ui_combo_set(g_host, 0);
}

// 同 host_sync：配置里的口令命中预置就选中，不命中就插到最前面当「手填项」。
// 不能简单 ui_text —— 下拉框里没有这一项时，编辑框会被 SetWindowText 覆盖掉
// 但列表里仍缺一条，用户点开下拉就看不到自己正在用的口令。
static void pwd_sync(void) {
    if (!g_pwd || !g.cfg.pwd[0]) return;
    WCHAR w[128];
    utf16(g.cfg.pwd, w, COUNT_OF(w));
    int idx = (int)SendMessageW(g_pwd, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w);
    if (idx >= 0) {
        ui_combo_set(g_pwd, idx);
        return;
    }
    SendMessageW(g_pwd, CB_INSERTSTRING, 0, (LPARAM)w);
    ui_combo_set(g_pwd, 0);
}

// ---- 角色卡 ----
// SP 和 TP 各占一行：挤成「SP a / b · TP c / d」一行时，满额（90000/90000）
// 这串字在卡片里放不下，末尾会被卡片边裁掉一半。
enum { CARD_N = 14 };
static const char *kCardKey[CARD_N] = {"名称", "等级",   "职业", "状态",     "金币", "点券", "复活币",
                                       "账号金币", "SP", "TP", "QP", "PVP", "物品栏", "装备 / 仓库"};
static HWND g_grp_chars, g_grp_card;
static HWND g_ck[CARD_N], g_cv[CARD_N];

// 布局常量
#define PAD      8
#define ROWH     23
#define LEFTW    300

// ---------------------------------------------------------------- 角色卡
// 内容没变就不重写：tick 每 100ms 跑一次，无条件 SetWindowText 会把
// 13 个标签每秒刷 130 遍，白白制造无效区和闪烁。
static char s_card_txt[CARD_N][160];
static int  s_card_init;

static void card_put(int i, const char *s) {
    if (s_card_init && strcmp(s_card_txt[i], s) == 0) return;
    snprintf(s_card_txt[i], sizeof(s_card_txt[i]), "%s", s);
    ui_text(g_cv[i], s);
}

static void card_refresh(void) {
    char b[192], t1[48], t2[48];
    if (!g.char_ok) {
        for (int i = 0; i < CARD_N; i++) card_put(i, "—");
        s_card_init = 1;
        return;
    }
    // 名字/等级/职业取左栏列表那一行：那是轮询维护的实时视图（玩家在游戏里升级、
    // 改名，左栏立刻跟上）。g.cur 是快照——GM 表单的基准值，只在选角色/重载时换代，
    // 拿它当"最新"会让卡片落后于玩家在游戏里的变化。
    const Charac *c = &g.cur;
    if (g.sel >= 0 && g.sel < g.chars.n) {
        const Charac *lc = VEC_AT(&g.chars, Charac, g.sel);
        if (lc->no == g.cur.no) c = lc;
    }
    const CharData *d = &g.data;

    card_put(0, c->name);
    fmt(b, sizeof(b), "Lv.%d", c->lev);
    card_put(1, b);
    card_put(2, app_job_text(c->job));
    card_put(3, c->online ? "在线（写库前请退出游戏）" : "离线");

    app_money_text(d->money, t1, sizeof(t1));
    card_put(4, t1);
    app_money_text(d->cera, t1, sizeof(t1));
    app_money_text(d->cera_point, t2, sizeof(t2));
    fmt(b, sizeof(b), "%s / %s", t1, t2);
    card_put(5, b);
    app_money_text(d->coin, t1, sizeof(t1));
    card_put(6, t1);
    app_money_text(d->account_money, t1, sizeof(t1));
    card_put(7, t1);

    // 三行都只写当前值，不写「/ 上限」：上限是各渠道自己算的满额（SP 90000、
    // TP 90000、QP 却是 0），三行摆在一起时 QP 独独少一半，看着像没读到数据。
    // 需要看满额的场合去游戏里看，这里只回答「现在有多少」。
    fmt(b, sizeof(b), "%d", d->sp);
    card_put(8, b);
    fmt(b, sizeof(b), "%d", d->tp);
    card_put(9, b);
    fmt(b, sizeof(b), "%d", d->qp);
    card_put(10, b);
    // 胜」是段位场次，第二个是胜点；两边单位不一样，分开写才对得上游戏里的说法
    fmt(b, sizeof(b), "%s · %d 胜 · %d 点", app_pvp_text(d->pvp_grade), d->pvp_win, d->pvp_point);
    card_put(11, b);

    int used = 0;
    for (int i = 0; i < d->inv.n; i++)
        if (!item_empty(&d->inv.items[i])) used++;
    fmt(b, sizeof(b), "%d / %u 格", used, d->inv_cap ? d->inv_cap : (u32)d->inv.n);
    card_put(12, b);

    int eq = 0;
    for (int i = 0; i < d->equip.n; i++)
        if (!item_empty(&d->equip.items[i])) eq++;
    fmt(b, sizeof(b), "%d 件 / 仓库 %u 格", eq, d->cargo_cap ? d->cargo_cap : (u32)d->cargo.n);
    card_put(13, b);
    s_card_init = 1;
}

static void card_build(HWND parent) {
    // 两张卡片先建：z 序在列表和标签之下，否则卡片会盖住内容
    g_grp_chars = ui_group(parent, "角色", IDC_GRP_CHARS);
    g_grp_card = ui_group(parent, "角色信息", IDC_GRP_CARD);
    g_online = ui_label_r(parent, "在线 — / —", IDC_ONLINE);
    g_chinfo = ui_label(parent, "未连接数据库", 0);
    g_chlist = ui_list(parent, IDC_CHLIST);
    ui_cols(g_chlist, (const char *const[]){"名称", "账号", "等级 · 职业 · 状态"},
            (const int[]){92, 90, 112}, 3);
    for (int i = 0; i < CARD_N; i++) {
        g_ck[i] = ui_label(parent, kCardKey[i], 0);
        g_cv[i] = ui_label(parent, "—", 0);
    }
}

// 左栏：两张卡片，和右侧同款（细线 + 粗体标题），底部与右侧组框齐平
static void left_layout(int x, int y0, int w, int h) {
    int rowh = ui_scale(21), crh = ui_scale(20);
    int padx = ui_scale(12);
    int th = ui_scale(24);                    // 卡片标题带：与 ui_group 的标题区对齐
    int gap = ui_scale(8);

    int g2h = th + CARD_N * crh + ui_scale(8);
    int g2y = y0 + h - g2h;                   // 下卡贴底：两栏底边齐平
    int g1h = g2y - gap - y0;
    int listh = g1h - th - rowh - ui_scale(4) - ui_scale(8);
    if (listh < ui_scale(60)) listh = ui_scale(60);

    ui_place(g_grp_chars, x, y0, w, g1h);
    // 在线人数挂在卡片标题行右端（原来它单独占一行，和标题重复）
    ui_place(g_online, x + w - ui_scale(116), y0 + ui_scale(3), ui_scale(104), ui_scale(18));
    ui_place(g_chinfo, x + padx, y0 + th, w - padx * 2, rowh);
    ui_place(g_chlist, x + padx, y0 + th + rowh + ui_scale(4), w - padx * 2, listh);

    // 列宽跟着卡片走。写死像素在窄窗口或高 DPI 下会超出客户区，撑出横向滚动区，
    // 列表底下就多一条白带，看着像少画了一块。
    static int s_chw = -1;
    int inner = w - padx * 2;
    if (inner != s_chw && inner > 0) {
        s_chw = inner;
        static const int kPct[3] = {30, 28, 42};   // 状态列最长（Lv.85 · 职业 · 在线）
        for (int i = 0; i < 3; i++)
            SendMessageW(g_chlist, LVM_SETCOLUMNWIDTH, i,
                         MAKELPARAM((inner - ui_scale(4)) * kPct[i] / 100, 0));
    }

    ui_place(g_grp_card, x, g2y, w, g2h);
    // 标签列宽按最长的那条量，不写死像素：写死 56 时「装备 / 仓库」的「库」、
    // 72 时「SP / TP」后面那个值又换成被裁，换字号/DPI 还得再调一次。
    static int s_kw;
    if (s_kw <= 0) {
        for (int i = 0; i < CARD_N; i++) {
            int tw = ui_text_w(kCardKey[i]);
            if (tw > s_kw) s_kw = tw;
        }
        s_kw += ui_scale(10);           // 留一点缝，别让标签贴住值
    }
    int kw = s_kw;
    int yy = g2y + th;
    for (int i = 0; i < CARD_N; i++) {
        ui_place(g_ck[i], x + padx, yy, kw, crh);
        ui_place(g_cv[i], x + padx + kw + ui_scale(2), yy, w - padx * 2 - kw - ui_scale(2), crh);
        yy += crh;
    }
}

// ---------------------------------------------------------------- 状态
// 同理：状态栏也只在文字真的变了才写，避免每 100ms 重绘一次
static char s_st0[700], s_st1[200], s_st_init;
// 固定窗口的外框尺寸（创建时算好，WM_GETMINMAXINFO 用它把大小锁死）
static int s_win_w, s_win_h;

static void status_refresh(void) {
    WCHAR w[700];
    if (!s_st_init || strcmp(s_st0, g.status)) {
        snprintf(s_st0, sizeof(s_st0), "%s", g.status);
        utf16(g.status, w, COUNT_OF(w));
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)w);
    }

    char r[160];
    const char *w2 = g.pend_mask || g.write_mask ? "正在写库" : "已同步";
    fmt(r, sizeof(r), "%s · %s · %s", g.db_ok ? "库已连接" : "库未连接",
        g.pvf_ok ? "PVF 就绪" : "无 PVF", w2);
    if (!s_st_init || strcmp(s_st1, r)) {
        snprintf(s_st1, sizeof(s_st1), "%s", r);
        utf16(r, w, COUNT_OF(w));
        SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)w);
    }
    s_st_init = 1;
}

static void buttons_refresh(void) {
    ui_enable(g_conn, !g.db_ok && !job_running(&g.job_conn));
    ui_enable(g_disc, g.db_ok);
    ui_enable(g_lkw, g.db_ok);
    ui_enable(g_kw, g.db_ok);
    ui_enable(g_kwgo, g.db_ok);
    ui_enable(g_kwclr, g.db_ok);
    ui_enable(g_chlist, g.db_ok && g.chars.n > 0);
}

static void do_layout(void);

// 连库就绪（命令行模式共用）：最多等 10 秒
static int wait_db(void) {
    app_connect();
    for (int i = 0; i < 100 && !g.db_ok; i++) {
        app_tick();
        Sleep(100);
    }
    return g.db_ok;
}

// --db-restore=<路径>：按配置连库，把 .sql 逐条执行后退出（供脚本/排障用）
static int db_restore_main(void) {
    char path[MAX_PATH] = "";
    const WCHAR *cl = GetCommandLineW();
    const WCHAR *at = wcsstr(cl, L"--db-restore=");
    if (at) {
        char tmp[MAX_PATH];
        utf8(at + 13, tmp, sizeof(tmp));
        str_trim(tmp);
        snprintf(path, sizeof(path), "%s", tmp);
    }
    if (!path[0]) {
        app_log("用法：DNF_GM.exe --db-restore=<file.sql>");
        return 2;
    }
    if (!wait_db()) {
        app_shutdown();
        return 2;
    }
    Err err;
    int n = 0;
    int ok = db_restore_sql(&g.db, path, &n, err);
    if (ok) app_log("还原数据库 · 执行 %d 条语句 ← %s", n, path);
    else app_log("还原失败（第 %d 条起）：%s", n + 1, err);
    app_shutdown();
    return ok ? 0 : 1;
}

// --db-dump[=路径]：按配置连库，导出整库 SQL 后退出（供脚本/排障用）
static int db_dump_main(void) {
    char path[MAX_PATH];
    const WCHAR *cl = GetCommandLineW();
    const WCHAR *at = wcsstr(cl, L"--db-dump=");
    if (at) {
        char tmp[MAX_PATH];
        utf8(at + 10, tmp, sizeof(tmp));
        str_trim(tmp);
        snprintf(path, sizeof(path), "%s", tmp);
    } else {
        db_dump_path(path, sizeof(path));
    }
    if (!wait_db()) {
        app_shutdown();
        return 2;
    }
    Err err;
    int nt = 0;
    u64 nr = 0;
    int ok = db_dump_sql(&g.db, path, &nt, &nr, err);
    if (ok) app_log("备份数据库 · %d 张表 / %llu 行 → %s", nt, (unsigned long long)nr, path);
    else app_log("备份失败：%s", err);
    app_shutdown();
    return ok ? 0 : 1;
}

// --db-test：连库跑一遍「角色列表 → 在线状态 → 载入数据」三条查询，
// 把结果写日志后退出。改过 SQL 之后用它确认库里的表/列都对得上（排障用）
static int db_test_main(void) {
    if (!wait_db()) {
        app_shutdown();
        return 2;
    }
    Err err;
    Vec v = {0};
    vec_init(&v, sizeof(Charac));
    int ok = db_poll_chars(&g.db, &v, 500, err);
    if (!ok) app_log("db-test 轮询失败：%s", err);
    int online = 0;
    for (int i = 0; i < v.n; i++)
        if (VEC_AT(&v, Charac, i)->online) online++;
    app_log("db-test 轮询：%s · 角色 %d 个 · 在线 %d 个 · 总数 %u", ok ? "成功" : "失败", v.n,
            online, g.char_total);
    if (v.n) {
        Charac c = *(Charac *)vec_at(&v, 0);
        CharData d = {0};
        int ok2 = db_load_data(&g.db, &c, &d, err);
        if (!ok2) app_log("db-test 载入失败：%s", err);
        else
            app_log("db-test 载入 %s(%u)：金币 %llu · 复活币 %llu · 点券 %llu+%llu · 背包 %d 格 · "
                    "仓库 %d 格 · SP %d · PVP %d/%d/%d",
                    c.name, c.no, (unsigned long long)d.money, (unsigned long long)d.coin,
                    (unsigned long long)d.cera, (unsigned long long)d.cera_point, d.inv.n, d.cargo.n,
                    d.sp, d.pvp_grade, d.pvp_win, d.pvp_point);
        db_free_data(&d);

        // 再单独跑一次「状态回读」（GM 页写完后用它刷新卡片），列序错了这里就对不上
        CharData s2 = {0};
        if (db_read_state(&g.db, &c, &s2, err))
            app_log("db-test 状态回读：金币 %llu · 复活币 %llu · 账号金币 %llu · 点券 %llu+%llu · "
                    "仓库容量 %u · SP %d/%d · TP %d/%d · QP %d · PVP %d/%d/%d",
                    (unsigned long long)s2.money, (unsigned long long)s2.coin,
                    (unsigned long long)s2.account_money, (unsigned long long)s2.cera,
                    (unsigned long long)s2.cera_point, s2.cargo_cap, s2.sp, s2.sp2, s2.tp, s2.tp2,
                    s2.qp, s2.pvp_grade, s2.pvp_win, s2.pvp_point);
        else
            app_log("db-test 状态回读失败：%s", err);
        ok = ok && ok2;
    }
    vec_free(&v);
    app_log_flush();
    app_shutdown();
    return ok ? 0 : 1;
}

// --grab-probe：等 PVF 就绪（最多 8s）后把诊断写到 config/grab-probe.txt，然后退出
static int grab_probe_main(void) {
    for (int i = 0; i < 80 && !g.pvf_ok; i++) {
        app_tick();
        Sleep(100);
    }
    Str s = {0};
    grab_probe(&s);
    char dir[MAX_PATH], path[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    path_join(path, sizeof(path), dir, "config");
    path_join(path, sizeof(path), path, "grab-probe.txt");
    int ok = file_write(path, str_c(&s), (int)s.n);
    str_free(&s);
    app_shutdown();
    return ok ? 0 : 1;
}

static u32 s_chars_ver, s_data_ver, s_act_ver, s_grab_ver;
static int s_ready = -1, s_sel = -2, s_pvf = -1;

// 「控件的启用状态可能要重算」的脏标记。
// 以前是每个变更点各自调 ui_pages_lock()，结果加载 PVF 那条漏了 —— 查询页在
// PVF 加载完还一直灰着，按钮点不动。改成变更点只置位、tick 统一重算一次：
// 以后再加状态量（比如「已选 PVF」之外的东西）只置位就行，不会再漏。
static int s_lock_dirty;
#define LOCK_REDO() (s_lock_dirty = 1)

// ---- 取码热键（轮询，避免 RegisterHotKey 抢走 Tab 等按键）----
static int hotkey_vk(const char *name) {
    if (!name || !name[0]) return 0;
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') return VK_F1 + (name[1] - '1');
    if (!lstrcmpiA(name, "Tab")) return VK_TAB;
    if (!lstrcmpiA(name, "Insert")) return VK_INSERT;
    if (!lstrcmpiA(name, "Home")) return VK_HOME;
    if (!lstrcmpiA(name, "End")) return VK_END;
    if (!lstrcmpiA(name, "PageUp")) return VK_PRIOR;
    if (!lstrcmpiA(name, "PageDown")) return VK_NEXT;
    if (name[0] && !name[1]) {
        char c = (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    return 0;
}

static void grab_poll(void) {
    static int down = 0;
    int vk = hotkey_vk(g.cfg.hotkey);
    if (!vk) return;
    int now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (now && !down) {
        HWND fg = GetForegroundWindow();
        // 本工具在前台时不抢键，免得影响页签切换。fg 为 NULL（激活交接的瞬间、
        // 桌面没有前台窗口）也算"不是本工具"——键这时根本送不进我们的控件。
        // 热键就是连续取码的总开关：坐在游戏里也能开能停，不用切回工具。
        // 复选框的同步不在这里做：见 tick() 里每 tick 的幂等刷新
        if (fg != g_main) app_grab_auto(!g.cfg.grab_auto);
    }
    down = now;
}

// 角色列表：本地过滤（搜索框只影响显示，不重新查库，选择不会丢）
static int s_chsync;

// 点选列表切换角色不能在 LVN_ITEMCHANGED 里同步做：切角色会重填列表并改选中项，
// 同步重入会把列表控件自己的状态机打断。这里只记下目标行，投递到消息循环再处理。
#define WM_SELCHAR (WM_APP + 1)
static int s_pending_sel = -1, s_sel_posted;

static void chars_fill(void) {
    char kw[64] = "";
    ui_get(g_kw, kw, sizeof(kw));
    str_trim(kw);

    int online = 0;
    for (int i = 0; i < g.chars.n; i++)
        if (VEC_AT(&g.chars, Charac, i)->online) online++;
    if (!g.db_ok) ui_text(g_online, "在线 — / —");
    else ui_text(g_online, fmt_new("在线 %d / 共 %u", online, g.char_total ? g.char_total : (u32)g.chars.n));

    s_chsync = 1;
    // 重填会把视图弹回顶部：角色多的时候用户正滚到下面挑人，3 秒一次的刷新
    // 就把他弹回去了。记下当前顶部行，填完再挪回去（选中另行按编号定位）。
    int top = (int)SendMessageW(g_chlist, LVM_GETTOPINDEX, 0, 0);
    ui_rows_clear(g_chlist);
    ui_batch(g_chlist, 1);
    int shown = 0, selrow = -1;
    for (int i = 0; i < g.chars.n; i++) {
        Charac *c = VEC_AT(&g.chars, Charac, i);
        // 关键字同时匹配角色名与账号名
        if (kw[0] && !str_istr(c->name, kw) && !str_istr(c->account, kw)) continue;
        char info[96];
        fmt(info, sizeof(info), "Lv.%d · %s · %s", c->lev, app_job_text(c->job),
            c->online ? "在线" : "离线");
        const char *cells[3] = {c->name, c->account, info};
        int row = ui_row_add(g_chlist, cells, 3, (LPARAM)i);
        if (g.char_ok && c->no == g.cur.no) selrow = row;
        shown++;
    }
    if (selrow >= 0) ui_row_sel_set(g_chlist, selrow);   // 先选再开重绘，否则高亮丢了
    // 选中不会自己滚动视图，所以这里把原来的顶部行挪回来即可
    if (top > 0 && top < (int)SendMessageW(g_chlist, LVM_GETITEMCOUNT, 0, 0))
        SendMessageW(g_chlist, LVM_ENSUREVISIBLE, (WPARAM)top, FALSE);
    ui_batch(g_chlist, 0);
    s_chsync = 0;

    if (!g.db_ok) ui_text(g_chinfo, "未连接数据库");
    else if (!g.chars.n) ui_text(g_chinfo, "该库没有角色");
    else if (kw[0]) ui_text(g_chinfo, fmt_new("匹配 %d / %d 个角色", shown, g.chars.n));
    else ui_text(g_chinfo, fmt_new("共 %d 个角色%s", g.chars.n,
                                   g.char_total > (u32)g.chars.n ? "（显示前 200）" : ""));
}

// 切页三连：建页 → 给尺寸 → 填数据。三步必须一气呵成，
// 中途若被重入（嵌套消息循环里定时器又 Tick），页面会建到一半。
static int s_switching;
static void select_page(int i) {
    if (s_switching || i < 0 || i >= PAGE_COUNT) return;
    s_switching = 1;
    ui_pages_switch(i);
    do_layout();                 // 先给页真实尺寸
    ui_pages_reload(i);          // 再填数据
    // 重新显示页面时，系统只让页面自己重画，子控件不会被重画：内容没变的控件
    // （下拉框最典型）会留一块空白，直到鼠标划过才补画。数据填完之后整页重画一次。
    ui_page_redraw(g_pages[i].wnd);
    s_switching = 0;
}

static void tick(void) {
    app_tick();
    ui_idtip_tick();     // 编号悬浮卡的悬停计时/显隐
    // PVF 刚就绪时贴图包目录可能是自动猜出来的，把输入框内容对齐一次
    static int s_img_synced;
    if (g.pvf_ok && !s_img_synced) {
        s_img_synced = 1;
        if (g.cfg.img_dir[0]) ui_text(g_img, g.cfg.img_dir);
    }
    // 连续取码开着而发放页还没建过：采到的东西没地方放。这里只改页签的选中项，
    // 真正的切页交给下面那段页签轮询去完成——直接调 select_page 的话，
    // tab 的选中项还是旧页，下一 tick 又会被打回去。
    // g_list 一建上这个判断就不再成立，不会反复切页打断用户。
    if (g.cfg.grab_auto && !page_grant_built())
        SendMessageW(g_tab, TCM_SETCURSEL, (WPARAM)PAGE_GRANT, 0);
    grab_poll();
    // 复选框是 g.cfg.grab_auto 的投影：热键在游戏里切换时发放页可能还没建、
    // 也可能不在前台，边沿那一下同步不上。每 tick 幂等刷一次，100ms 内必然收敛。
    page_grant_syncauto();
    page_gm_pump();      // GM 页排队的写入往前推：跟本页是否可见无关
    page_bubble_pump();  // 泡点状态行同理：倒计时要在不切页的时候也走

    // 从最小化恢复时，子控件的绘制表面已被系统丢弃，和切页一样会留白：
    // 恢复到前台后整页补画一次
    static int s_was_iconic = -1;
    int ic = IsIconic(g_main) ? 1 : 0;
    if (s_was_iconic == 1 && ic == 0) ui_page_redraw(g_pages[ui_page_cur()].wnd);
    s_was_iconic = ic;

    // 页签：轮询当前选中项，避免依赖 TCN_SELCHANGE（也便于脚本驱动）
    int tsel = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
    if (tsel >= 0 && tsel != ui_page_cur()) select_page(tsel);

    if (g.chars_ver != s_chars_ver) {
        s_chars_ver = g.chars_ver;
        chars_fill();
    }
    int ready = (g.db_ok ? 1 : 0) | (g.char_ok ? 2 : 0) | (g.cur.online ? 4 : 0);
    if (ready != s_ready) {
        s_ready = ready;
        buttons_refresh();
        chars_fill();
        // 在线状态也进 ready：GM 页的表单在线时要置灰并把条件写进组框标题，
        // 不重新 lock 一次，上下线切换界面不会跟着变
        LOCK_REDO();
    }
    if (g.sel != s_sel) {
        s_sel = g.sel;
        buttons_refresh();
    }
    if (g.data_ver != s_data_ver) {
        s_data_ver = g.data_ver;
        ui_pages_reload_all();
        buttons_refresh();
    }
    // 轮询只让左栏（列表 + 在线人数 + 角色卡）跟着走：chars_fill / card_refresh
    // 每 tick 自己取用，这里不需要任何"顺便刷一下页面"的分支。
    if (g.pvf_ok != s_pvf) {
        s_pvf = g.pvf_ok;
        if (g.pvf_ok) ui_pages_reload_all();
        LOCK_REDO();   // 查询页、发放页的「需要 PVF」按钮就靠这一下解锁
    }
    if (g.act_ver != s_act_ver) {
        s_act_ver = g.act_ver;
        ui_pages_reload_all();
        buttons_refresh();
        LOCK_REDO();
    }
    // 所有「状态可能变了」的判断都走完了，这里统一重算一次
    if (s_lock_dirty) {
        s_lock_dirty = 0;
        ui_pages_lock();
    }
    if (g.grab_ver != s_grab_ver) {
        s_grab_ver = g.grab_ver;
        page_grant_setid(g.grab.id);   // 取码结果落到发放页的编号框（看得见的回声）
        // 采到就整行入队：用户要的是"采到就进清单"，不是只留个编号还得再点一次
        // 「加入队列」。同编号已在队列里就跳过（去重）。
        page_grant_autoadd(&g.grab);
    }
    card_refresh();
    status_refresh();
}

// ---------------------------------------------------------------- 布局
static void do_layout(void) {
    RECT rc;
    GetClientRect(g_main, &rc);
    int w = rc.right, h = rc.bottom;

    int pad = ui_scale(PAD), rowh = ui_scale(ROWH), gap = ui_scale(4);
    int leftw = ui_scale(LEFTW);

    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    // 换到主窗口客户区坐标：后面的页签区高度要按「状态栏顶边」算，不能按
    // 「客户区高 - 状态栏高」算 —— 后者在状态栏还没定位时（首次布局读到 0）
    // 会把页签一路铺到窗口最底下，页面容器跟着压到状态栏下面，
    // 页面里贴底的按钮就被状态栏盖掉一截（泡点页的「应用」正是这么被遮的）。
    MapWindowPoints(NULL, g_main, (POINT *)&sr, 2);
    int sh = sr.bottom - sr.top;
    int status_top = sr.top;
    if (sh < ui_scale(18) || status_top <= 0) {
        sh = ui_scale(22);
        status_top = h - sh;
    }
    int parts[2];
    parts[0] = w - ui_scale(240);
    parts[1] = -1;
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);

    URow r;
    int y = pad;
    // 第一行：连接
    ui_row(&r, g_main, pad, y, rowh);
    ui_add(&r, g_lhost, ui_scale(32));
    ui_add(&r, g_host, ui_scale(104));
    ui_add(&r, g_lport, ui_scale(32));
    ui_add(&r, g_port, ui_scale(52));
    ui_add(&r, g_luser, ui_scale(32));
    ui_add(&r, g_user, ui_scale(76));
    ui_add(&r, g_lpwd, ui_scale(32));
    ui_add(&r, g_pwd, ui_scale(84));
    ui_add(&r, g_conn, ui_scale(60));
    ui_add(&r, g_disc, ui_scale(60));

    // 右上角不再有「保存/放弃」：各页改完即写库，状态见状态栏右段

    y += rowh + gap;
    ui_row(&r, g_main, pad, y, rowh);
    ui_add(&r, g_lkw, ui_scale(32));
    ui_add(&r, g_kw, ui_scale(110));
    ui_add(&r, g_kwgo, ui_scale(52));
    ui_add(&r, g_kwclr, ui_scale(48));
    // 页签区。先落位、再量内容区（TCM_ADJUSTRECT）—— 上面那行要和「页签里列表的右端」
    // 对齐，得先知道内容区右边界；而 ADJUSTRECT 只有在页签已有真实尺寸时才量得准。
    int top = y + rowh + gap + ui_scale(6);   // 和上面那行留一口气，页签不贴控件
    int tabx = pad + leftw + pad;
    int tabw = w - tabx - pad;
    int tabh = status_top - top - pad;     // 页签区到状态栏顶为止，不往下压
    if (tabw < ui_scale(200)) tabw = ui_scale(200);
    if (tabh < ui_scale(120)) tabh = ui_scale(120);
    ui_place(g_tab, tabx, top, tabw, tabh);
    left_layout(pad, top, leftw, tabh);   // 左栏（角色卡）跟着页签一起重排

    RECT tr, dr;
    GetWindowRect(g_tab, &tr);
    MapWindowPoints(NULL, g_main, (POINT *)&tr, 2);
    dr = tr;
    SendMessageW(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&dr);
    // 页面容器再往里让一个页内边距，就是下方列表的右边界 —— 这一行排到它为止
    int right = dr.right - ui_scale(6);

    // 两组「标签 + 目录框 + 按钮」并列，两组的框等长、按钮也等长。
    // 宽度按「右边界 - 起点」反推，不再手工预算间隔：之前少算了两个 gap，
    // 最后一个按钮被顶出客户区、右边被裁掉一截。
    // 窗口太窄时搜索段可能已经越过这条线，那就保持原位（不重叠优先于对齐）。
    int pvf_x = tabx + ui_scale(6);
    if (r.x < pvf_x) r.x = pvf_x;
    int lb = ui_scale(30);
    int bw = ui_text_w("选择 PVF");                  // 按钮取两条文案里更宽的那个，
    int tw = ui_text_w("ImagePacks2");               // 字号调大也不会把字截断
    if (tw > bw) bw = tw;
    bw += ui_scale(22);
    int sp = ui_scale(14);                           // 两组之间的分组间隔（不加竖线，标签本身就是分隔）
    int ngap = 5;                                    // 6 个控件排下来是 5 个间隙
    int editw = (right - r.x - lb * 2 - bw * 2 - sp - gap * ngap) / 2;
    if (editw < ui_scale(80)) editw = ui_scale(80);
    ui_add(&r, g_lpvf, lb);
    ui_add(&r, g_pvf, editw);
    ui_add(&r, g_pvfbtn, bw);
    ui_gap(&r, sp);
    ui_add(&r, g_limg, lb);
    ui_add(&r, g_img, editw);
    ui_add(&r, g_imgbtn, bw);

    // 当前页放到页签内容区（dr 上面已经量好，这里只做防越界的收口）
    int pi = ui_page_cur();
    if (pi >= 0 && g_pages[pi].wnd) {
        // 双保险：页签区无论算出多大，页面容器都不许压到状态栏上
        int maxb = status_top - ui_scale(2);
        if (dr.bottom > maxb) dr.bottom = maxb;
        if (dr.right > w - pad) dr.right = w - pad;
        ui_pages_place(pi, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top);
    }
    (void)sh;
}

// ---------------------------------------------------------------- 命令
static void on_connect(void) {
    ui_get(g_host, g.cfg.host, sizeof(g.cfg.host));
    int port = ui_int(g_port);
    if (port > 0) g.cfg.port = port;
    ui_get(g_user, g.cfg.user, sizeof(g.cfg.user));
    ui_get(g_pwd, g.cfg.pwd, sizeof(g.cfg.pwd));
    app_connect();
}

static void on_pick_pvf(void) {
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"PVF 脚本包 (*.pvf)\0*.pvf\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        char path[MAX_PATH];
        utf8(file, path, sizeof(path));
        app_load_pvf(path);
        ui_text(g_pvf, path);
    }
}

// ImagePacks2 是个目录，用系统的文件夹选择框
static void on_pick_img(void) {
    WCHAR dir[MAX_PATH] = L"";
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_main;
    bi.lpszTitle = L"选择客户端的 ImagePacks2 目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    int ok = SHGetPathFromIDListW(pidl, dir);
    CoTaskMemFree(pidl);
    if (!ok || !dir[0]) return;
    char path[MAX_PATH];
    utf8(dir, path, sizeof(path));
    ui_text(g_img, path);
    app_load_npk(path);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: {
            HINSTANCE in = (HINSTANCE)GetWindowLongPtrW(h, GWLP_HINSTANCE);
            g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                                                       WS_CLIPCHILDREN | WS_TABSTOP,
                                    0, 0, 0, 0, h, (HMENU)(INT_PTR)IDC_TAB, in, NULL);
            SendMessageW(g_tab, WM_SETFONT, (WPARAM)ui_font(), TRUE);

            g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                       0, 0, 0, 0, h, (HMENU)(INT_PTR)IDC_STATUS, in, NULL);
            SendMessageW(g_status, WM_SETFONT, (WPARAM)ui_font(), TRUE);
            // 状态栏默认是 3DFACE 灰条，在浅色底上就是最下面一块深色带
            SendMessageW(g_status, SB_SETBKCOLOR, 0, (LPARAM)ui_bg_color());

            g_lhost = ui_label(h, "地址", 0);
            // 下拉但可编辑：预设够用时点一下就走，不在预设里就自己敲。
            // 预设取自 Python 版 GM 工具（config.json 的 saved_servers 与默认 DB_IP），
            // 本机自架的服最常见，所以 127.0.0.1 放在第一项。
            g_host = ui_combo(h, IDC_HOST, 0);
            ui_combo_fill(g_host, kHostList, COUNT_OF(kHostList), 0);
            host_sync();
            g_lport = ui_label(h, "端口", 0);
            g_port = ui_edit(h, "", IDC_PORT, 0);
            ui_num(g_port, 1, 65535);          // 端口越界连接时会被系统截成别的端口
            ui_set_int(g_port, g.cfg.port);
            g_luser = ui_label(h, "用户", 0);
            g_user = ui_edit(h, g.cfg.user, IDC_USER, 0);
            g_lpwd = ui_label(h, "密码", 0);
            // 密码**明文显示、明文保存**，这是定好的取舍，不是没做保护：
            // 本机连库用的口令，掩码之后看不见、改不动、连不上时也核对不了；
            // 而掩码/本地加密都挡不住能碰到这台机器的人。所以不做密码框、不做加密。
            // 可编辑下拉：预置 uu5!^%jg 与 123456，不在预置里就自己敲。
            g_pwd = ui_combo(h, IDC_PWD, 0);
            ui_combo_fill(g_pwd, kPwdList, COUNT_OF(kPwdList), 0);
            pwd_sync();
            ui_tip(h, g_pwd, "数据库口令：明文显示、明文存进 settings.ini（有意如此，便于核对）。"
                   "预置 uu5!^%jg 与 123456 可直接选，也可以自己填");
            g_conn = ui_button(h, "连接", IDC_CONN);
            g_disc = ui_button(h, "断开", IDC_DISC);

            g_lkw = ui_label(h, "搜索", 0);
            g_kw = ui_edit(h, "", IDC_KW, 0);
            ui_tip(h, g_kw, "输入关键字即时过滤：匹配角色名或账号名，也支持角色编号");
            g_kwgo = ui_button(h, "搜索", IDC_KWGO);
            g_kwclr = ui_button(h, "清空", IDC_KWCLR);
            // 在线人数挂在左侧角色卡标题行（card_build 里建），这里不再单独放一个
            g_lpvf = ui_label(h, "PVF", 0);
            g_pvf = ui_edit(h, g.cfg.pvf, IDC_PVFB, 1);
            g_pvfbtn = ui_button(h, "选择 PVF", IDC_PVFS);
            g_limg = ui_label(h, "贴图", 0);
            g_img = ui_edit(h, g.cfg.img_dir, IDC_IMGB, 1);
            g_imgbtn = ui_button(h, "ImagePacks2", IDC_IMGS);
            ui_tip(h, g_img, "ImagePacks2 目录：编号悬浮卡上的物品图标从这里取。"
                   "通常选完 PVF 会自动填好；没填上就点右边的按钮手动指到客户端的 ImagePacks2 目录。"
                   "留空不影响使用，只是卡片上没有图标");

            card_build(h);

            for (int i = 0; i < PAGE_COUNT; i++) {
                WCHAR w[96];
                TCITEMW it;
                utf16(g_pages[i].name, w, COUNT_OF(w));
                memset(&it, 0, sizeof(it));
                it.mask = TCIF_TEXT;
                it.pszText = w;
                SendMessageW(g_tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&it);
            }
            DragAcceptFiles(h, TRUE);
            return 0;
        }

        case WM_KEYDOWN:              // 重查角色表（不停留在按钮上，F5 即可）
            if (wp == VK_F5) { app_list_chars(""); return 0; }
            return 0;

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) do_layout();
            return 0;

        case WM_GETMINMAXINFO: {
            // 窗口固定：最大值与最小值都锁在创建时算出的尺寸上，边框拖不动、也最大化不了
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            if (s_win_w > 0 && s_win_h > 0) {
                mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = s_win_w;
                mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = s_win_h;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            int dpi = HIWORD(wp);
            ui_set_dpi(dpi);
            RECT *r = (RECT *)lp;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            do_layout();
            return 0;
        }

        case WM_SELCHAR: {
            // 延迟到消息循环：此刻列表不在自己的通知处理里，重填是安全的
            s_sel_posted = 0;
            int idx = s_pending_sel;
            s_pending_sel = -1;
            if (idx >= 0 && idx < g.chars.n) {
                int row = ui_row_sel(g_chlist);
                // 期间列表若被搜索/刷新重填过，选中的可能已是别的行，丢弃这次请求
                if (row >= 0 && (int)ui_row_data(g_chlist, row) == idx) {
                    Charac *c = VEC_AT(&g.chars, Charac, idx);
                    if (!g.char_ok || c->no != g.cur.no) app_select_char(idx);
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR nh = (LPNMHDR)lp;
            if (nh->idFrom == IDC_CHLIST) {
                if (nh->code == LVN_ITEMCHANGED && !s_chsync) {
                    int row = ui_row_sel(g_chlist);
                    int idx = row >= 0 ? (int)ui_row_data(g_chlist, row) : -1;
                    if (idx >= 0 && idx < g.chars.n) {
                        s_pending_sel = idx;
                        if (!s_sel_posted) {
                            s_sel_posted = 1;
                            PostMessageW(h, WM_SELCHAR, 0, 0);
                        }
                    }
                }
                return 0;
            }
            if (nh->idFrom == IDC_TAB) {
                if (nh->code == TCN_SELCHANGE) {
                    int i = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
                    if (i != ui_page_cur()) select_page(i);
                }
                return 0;
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_CONN) { on_connect(); return 0; }
            if (id == IDC_DISC) { app_disconnect(); buttons_refresh(); return 0; }
            if (id == IDC_PVFS) { on_pick_pvf(); return 0; }
            if (id == IDC_IMGS) { on_pick_img(); return 0; }
            if (id == IDC_IMGB && code == EN_KILLFOCUS) {   // 手敲路径，失焦生效
                char path[MAX_PATH];
                ui_get(g_img, path, sizeof(path));
                str_trim(path);
                if (path[0] && str_ieq(path, g.cfg.img_dir) == 0) app_load_npk(path);
                return 0;
            }
            if (id == IDC_KWGO) {         // 只过滤已拉到的角色，不重查库，选择不会丢
                chars_fill();
                return 0;
            }
            if (id == IDC_KWCLR) {
                ui_text(g_kw, "");
                chars_fill();
                return 0;
            }
            if (id == IDC_KW && code == EN_CHANGE) {
                chars_fill();
                return 0;
            }
            return 0;
        }

        case WM_DROPFILES: {
            HDROP d = (HDROP)wp;
            WCHAR w[MAX_PATH];
            if (DragQueryFileW(d, 0, w, MAX_PATH)) {
                char path[MAX_PATH];
                utf8(w, path, sizeof(path));
                int n = (int)strlen(path);
                if (n > 4 && str_ieq(path + n - 4, ".pvf")) {
                    ui_text(g_pvf, path);
                    app_load_pvf(path);
                }
            }
            DragFinish(d);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return ui_ctl_color(wp, lp);

        case WM_TIMER:
            if (wp == 1) tick();
            return 0;

        case WM_CLOSE:
            // 各页改完即写库：退出前把还在路上的写入落完，不再有「未保存的改动」。
            // 顺序不能反：必须先等写线程结束再关连接，否则线程会拿着已释放的连接继续跑。
            if (g.char_ok && g.pend_mask) {
                g.pend_ms = 0;
                for (int i = 0; i < 12 && g.pend_mask; i++) {
                    app_tick();
                    Sleep(80);
                }
                app_flush_pending();
            }
            if (g.write_mask || job_running(&g.job_act)) job_stop(&g.job_act);
            app_shutdown();
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            KillTimer(h, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    (void)cmd;
    g_hinst = inst;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    app_init();
    ui_init(inst, g.cfg.font);

    if (wcsstr(GetCommandLineW(), L"--db-test")) return db_test_main();
    if (wcsstr(GetCommandLineW(), L"--grab-probe")) return grab_probe_main();
    if (wcsstr(GetCommandLineW(), L"--db-dump")) return db_dump_main();
    if (wcsstr(GetCommandLineW(), L"--db-restore")) return db_restore_main();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = ui_bg_brush();
    wc.lpszClassName = L"DnfGmMain";
    if (!RegisterClassExW(&wc)) return 1;

    // 窗口尺寸固定，不给拉伸。界面按一套固定像素排版，能拉大拉小只会让某页在某个
    // 尺寸下摆不开（挤在左上角或者控件叠在一起），不如定死一个所有页面都摆得下的尺寸。
    // 客户区 1240×800：GM 页去掉泡点后自然高约 540，这个尺寸下按 100% 摆还有富余；
    // 宽度够两栏并排不挤。DPI 变化时 ui_scale 会跟着放大，和原来一致。
    int cw = ui_scale(1240), ch = ui_scale(800);
    int sw = GetSystemMetrics(SM_CXSCREEN), shh = GetSystemMetrics(SM_CYSCREEN);
    if (cw > sw * 96 / 100) cw = sw * 96 / 100;
    if (ch > shh * 92 / 100) ch = shh * 92 / 100;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc = {0, 0, cw, ch};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    s_win_w = rc.right - rc.left;
    s_win_h = rc.bottom - rc.top;

    int x = (sw - s_win_w) / 2, y = (shh - s_win_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_main = CreateWindowExW(0, L"DnfGmMain", L"DNF GM 工具", style,
                            x, y, s_win_w, s_win_h, NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    select_page(PAGE_ITEM);
    ui_pages_lock();
    buttons_refresh();
    chars_fill();
    status_refresh();
    SetTimer(g_main, 1, 100, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
