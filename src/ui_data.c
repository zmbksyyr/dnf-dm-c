// ui_data：宠物、时装、任务、数据库、日志。
#include "ui.h"
#include "pvf.h"


// ================================================================ 宠物页
enum { P_LIST = PAGE_ID(PAGE_PET, 1), P_NAME, P_RENAME, P_FULL, P_EXPIRE, P_DEL, P_REFRESH, P_INFO, P_BAD };

static HWND p_list, p_name, p_info;
static HWND p_sep;   // 列表和底部改名条之间的分隔线

enum { D_PETS = 1, D_PETNAME, D_PETFULL, D_PETEXPIRE, D_PETDEL };

typedef struct {
    int  kind;
    u32  ui_id;
    char s[64];
    int  sub;          // 通用：avatar 标记等
    u64  u64v;         // 通用：大 id
} DCtx;
static DCtx dc;

// ================================================================ 表内错误记录
// 宠物表 / 时装表里的错误行：it_id 在 PVF 里查不到，或所属角色已删。
// 一个入口扫两张表，弹窗里可以单删也可以全清；it_id=0 的空行是合法的，不动。
enum { D_TABSCAN = 40, D_TABDEL };

static Vec  g_tabscan;          // Vec<TabOrphan>
static Vec  g_tabdel;           // 本轮要删的行
static int  g_tab_pending;      // 扫描完成等下一次刷新弹窗
static int  g_tab_rescan;       // 删除完成后自动重扫

static int tab_has(u32 id, void *ctx) {
    (void)ctx;
    return g.pvf_ok && *pvf_item_name(&g.pvf, id);
}

static int tab_work(void *ctx, Err err) {
    DCtx *c = (DCtx *)ctx;
    if (c->kind == D_TABSCAN)
        return db_scan_tab_orphan(&g.db, &g_tabscan, g.pvf_ok ? tab_has : NULL, NULL, err);
    if (c->kind == D_TABDEL) {
        for (int i = 0; i < g_tabdel.n; i++) {
            TabOrphan *o = VEC_AT(&g_tabdel, TabOrphan, i);
            if (!db_del_tab_orphan(&g.db, o->avatar, o->ui_id, err)) return 0;
        }
        return 1;
    }
    return 0;
}

static void tab_scan_start(void) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return; }
    if (job_running(&g.job_act)) { app_info("上一个操作还在进行，等它完成"); return; }
    if (!g_tabscan.cap) vec_init(&g_tabscan, sizeof(TabOrphan));
    g_tabscan.n = 0;
    g_tab_pending = 1;
    dc.kind = D_TABSCAN;
    app_status("正在扫描宠物/时装表…");
    app_act("扫表内错误", tab_work, &dc);
}

static HWND t_list, t_info;
static u32  t_ver;          // 与 g.act_ver 同类型：混用 int 时 -Wextra 报符号比较

static void tab_fill(void) {
    ui_rows_clear(t_list);
    for (int i = 0; i < g_tabscan.n; i++) {
        TabOrphan *o = VEC_AT(&g_tabscan, TabOrphan, i);
        char cn[16], ii[24], it[16], nm[96];
        fmt(cn, sizeof(cn), "%u", o->cno);
        snprintf(ii, sizeof(ii), "%llu", (unsigned long long)o->ui_id);
        fmt(it, sizeof(it), "%u", o->it_id);
        const char *cells[6] = {o->avatar ? "时装" : "宠物", cn,
                                o->cname[0] ? o->cname : "?", it,
                                app_item_name(o->it_id, nm, sizeof(nm)), o->why};
        ui_row_add(t_list, cells, 6, (LPARAM)i);
    }
    ui_text(t_info, fmt_new("发现 %d 条错误记录%s", g_tabscan.n,
                            g.pvf_ok ? "" : "（PVF 未加载，只查角色归属）"));
}

static void tab_del_run(int all) {
    if (!g_tabscan.n) { app_info("没有可删的记录。"); return; }
    if (job_running(&g.job_act)) { app_info("上一个操作还在进行，等它完成"); return; }
    if (!g_tabdel.cap) vec_init(&g_tabdel, sizeof(TabOrphan));
    g_tabdel.n = 0;
    if (all) {
        for (int i = 0; i < g_tabscan.n; i++)
            *(TabOrphan *)VEC_ADD(&g_tabdel, TabOrphan) = *(TabOrphan *)VEC_AT(&g_tabscan, TabOrphan, i);
    } else {
        int row = ui_row_sel(t_list);
        if (row < 0 || row >= g_tabscan.n) { app_info("先在列表里选中一行。"); return; }
        *(TabOrphan *)VEC_ADD(&g_tabdel, TabOrphan) =
            *(TabOrphan *)VEC_AT(&g_tabscan, TabOrphan, row);
    }
    if (!app_confirm(fmt_new("删除 %d 条错误记录？\n\n删除后自动重新扫描。", g_tabdel.n)))
        return;
    g_tab_rescan = 1;
    dc.kind = D_TABDEL;
    app_act("删除错误记录", tab_work, &dc);
}

static LRESULT tab_proc(HWND d, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    (void)lp;
    (void)ctx;
    if (m == WM_TIMER) {
        if (g.act_ver == t_ver) return 0;
        t_ver = g.act_ver;
        if (g_tab_rescan) { g_tab_rescan = 0; tab_scan_start(); }   // 删完自动重扫
        else tab_fill();
        return 0;
    }
    if (m == WM_DESTROY) { KillTimer(d, 1); t_list = t_info = NULL; return 0; }
    if (m == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case 6301: tab_del_run(0); return 0;            // 删除所选
        case 6302: tab_del_run(1); return 0;            // 全部删除
        case 6303:                                      // 重新扫描
            t_ver = g.act_ver;
            tab_scan_start();
            return 0;
        }
    }
    return -1;
}

static void tab_show(void) {
    g_tab_pending = 0;
    HWND d = ui_dlg_new(g_main, "宠物/时装表内错误记录", 640, 420, tab_proc, NULL);
    if (!d) return;
    int pad = ui_scale(10), rowh = ui_scale(24), g2 = ui_scale(6);
    t_info = ui_label(d, "", 0);
    ui_place(t_info, pad, pad, 640 - pad * 2, rowh);

    t_list = ui_list(d, 6300);
    ui_cols(t_list, (const char *const[]){"表", "角色编号", "角色", "实例ID", "物品", "原因"},
            (const int[]){56, 80, 120, 110, 170, 60}, 6);
    ui_idtip_bind(t_list, 4);          // 「物品」列放的是 it_id
    // 列表让一行给下面的说明：悬停气泡会跟编号悬浮卡撞，写成常驻一句更省事
    int lt = pad + rowh + g2;
    int lh = ui_scale(280) - rowh - g2;
    ui_place(t_list, pad, lt, 640 - pad * 2, lh);
    ui_place(ui_label(d, "只列 it_id 非法或为 0 的记录；it_id=0 的空行算正常，不动", 0),
            pad, lt + lh + g2, 640 - pad * 2, rowh);

    int y = pad + rowh + g2 + ui_scale(280) + g2;
    URow r;
    ui_row(&r, d, pad, y, rowh);
    ui_add(&r, ui_btn_at(d, 6301, "删除所选"), ui_scale(84));
    ui_add(&r, ui_btn_at(d, 6302, "全部删除"), ui_scale(84));
    ui_add(&r, ui_btn_at(d, 6303, "重新扫描"), ui_scale(84));

    ui_place(ui_button(d, "关闭", IDCANCEL), 640 - pad - ui_scale(90),
             420 - pad - rowh, ui_scale(90), rowh);

    t_ver = g.act_ver;
    tab_fill();
    SetTimer(d, 1, 300, NULL);
    ui_dlg_run(d, g_main);
}


static int pet_work(void *ctx, Err err) {
    DCtx *c = (DCtx *)ctx;
    if (c->kind == D_PETS) g.lists_ready |= L_PETS;
    switch (c->kind) {
        case D_PETS: return db_list_pets(&g.db, &g.cur, &g.pets, err);
        case D_PETNAME: return db_set_pet_name(&g.db, c->ui_id, c->s, err);
        case D_PETFULL: return db_set_pet_full(&g.db, c->ui_id, err);
        case D_PETEXPIRE: return db_set_pet_expire(&g.db, c->ui_id, err);
        case D_PETDEL: return db_delete_pet(&g.db, c->ui_id, err);
    }
    return 0;
}

static void pet_fill(void) {
    ui_rows_clear(p_list);
    ui_batch(p_list, 1);
    for (int i = 0; i < g.pets.n; i++) {
        PetRow *r = VEC_AT(&g.pets, PetRow, i);
        char ui[16], iid[16], st[16];
        fmt(ui, sizeof(ui), "%u", r->ui_id);
        fmt(iid, sizeof(iid), "%u", r->it_id);
        fmt(st, sizeof(st), "%d", r->stomach);
        char nm[96];
        const char *cells[6] = {ui, app_item_name(r->it_id, nm, sizeof(nm)), iid, r->name, st, r->expire};
        ui_row_add(p_list, cells, 6, (LPARAM)i);
    }
    ui_batch(p_list, 0);
    ui_text(p_info, fmt_new("%s · 共 %d 只宠物", g.cur.name, g.pets.n));
}

static void pet_reload(Page *p) {
    (void)p;
    if (g_tab_pending && !job_running(&g.job_act)) tab_show();
    if (!g.char_ok) { ui_text(p_info, "请先选择角色"); return; }
    if (!(g.lists_ready & L_PETS)) {
        if (!job_running(&g.job_act)) {
            dc.kind = D_PETS;
            app_act("读取宠物", pet_work, &dc);
        }
        ui_text(p_info, "正在读取宠物…");
        return;
    }
    pet_fill();
}

static PetRow *pet_sel(void) {
    int row = ui_row_sel(p_list);
    if (row < 0 || row >= g.pets.n) {
        app_info("请先选中一只宠物。");
        return NULL;
    }
    return VEC_AT(&g.pets, PetRow, row);
}

static LRESULT pet_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    (void)lp;
    if (m != WM_COMMAND) return -1;
    if (LOWORD(wp) == P_BAD) { tab_scan_start(); return 0; }
    if (LOWORD(wp) == P_REFRESH) {
        g.pets.n = 0;
        g.lists_ready &= ~(u32)L_PETS;
        pet_reload(p);
        return 0;
    }
    PetRow *r = pet_sel();
    if (!r) return 0;
    switch (LOWORD(wp)) {
        case P_RENAME: {
            char nm[48];
            ui_get(p_name, nm, sizeof(nm));
            str_trim(nm);
            if (!nm[0]) { app_info("名字不能为空。"); return 0; }
            dc.kind = D_PETNAME;
            dc.ui_id = r->ui_id;
            snprintf(dc.s, sizeof(dc.s), "%s", nm);
            g.pets.n = 0;
            app_act("宠物改名", pet_work, &dc);
            return 0;
        }
        case P_FULL:
            dc.kind = D_PETFULL;
            dc.ui_id = r->ui_id;
            g.pets.n = 0;
            app_act("宠物满饱食", pet_work, &dc);
            return 0;
        case P_EXPIRE:
            dc.kind = D_PETEXPIRE;
            dc.ui_id = r->ui_id;
            g.pets.n = 0;
            app_act("宠物续期", pet_work, &dc);
            return 0;
        case P_DEL:
            if (!app_confirm(fmt_new("删除宠物「%s」？", r->name))) return 0;
            dc.kind = D_PETDEL;
            dc.ui_id = r->ui_id;
            g.pets.n = 0;
            app_act("删除宠物", pet_work, &dc);
            return 0;
    }
    return -1;
}

static void pet_layout(Page *p, int w, int h) {
    (void)w;
    (void)h;
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int pad = ui_scale(6), rowh = ui_scale(23);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, P_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, P_FULL, "满饱食"), ui_scale(70));
    ui_add(&r, ui_btn_at(p->wnd, P_EXPIRE, "续期"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, P_DEL, "删除"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, P_BAD, "清错误记录"), ui_scale(88));
    ui_add(&r, p_info, -1);
    int y = pad + rowh + ui_scale(6);
    int by = rc.bottom - pad - rowh;
    ui_place(p_sep, pad, by - ui_scale(8), rc.right - pad * 2, ui_scale(2));
    MoveWindow(p_list, pad, y, rc.right - pad * 2, by - ui_scale(10) - y, TRUE);
    ui_row(&r, p->wnd, pad, by, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "新名字"), ui_scale(52));
    ui_add(&r, p_name, ui_scale(160));
    ui_add(&r, ui_btn_at(p->wnd, P_RENAME, "改名"), ui_scale(60));
}

static void pet_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[] = {P_NAME, P_RENAME, P_FULL, P_EXPIRE, P_DEL, P_REFRESH, P_BAD};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void pet_build(Page *p, HWND parent) {
    p_list = ui_list(parent, P_LIST);
    p_sep = ui_hline(parent);
    p_info = ui_label(parent, "请先选择角色", P_INFO);
    p_name = ui_edit(parent, "", P_NAME, 0);
    ui_cols(p_list, (const char *const[]){"实例ID", "物品", "编号", "宠物名", "饱食", "到期"},
            (const int[]){80, 180, 70, 140, 50, 130}, 6);
    ui_idtip_bind(p_list, 2);
    ui_button(parent, "刷新", P_REFRESH);
    ui_button(parent, "满饱食", P_FULL);
    ui_button(parent, "续期", P_EXPIRE);
    ui_button(parent, "删除", P_DEL);
    ui_button(parent, "清错误记录", P_BAD);
    ui_button(parent, "改名", P_RENAME);
}

void page_pet_create(Page *p) {
    p->build = pet_build;
    p->layout = pet_layout;
    p->reload = pet_reload;
    p->lock = pet_lock;
    p->on = pet_on;
}

// ================================================================ 时装页
enum { A_LIST = PAGE_ID(PAGE_AVATAR, 1), A_HIDE, A_EXPIRE, A_DEL, A_REFRESH, A_INFO, A_BAD };
enum { D_AVAS = 20, D_AVAHIDE, D_AVAEXPIRE, D_AVADEL };

static HWND a_list, a_info;
static HWND a_sep;

static int ava_work(void *ctx, Err err) {
    DCtx *c = (DCtx *)ctx;
    if (c->kind == D_AVAS) g.lists_ready |= L_AVAS;
    switch (c->kind) {
        case D_AVAS: return db_list_avatars(&g.db, &g.cur, &g.avatars, err);
        case D_AVAHIDE: return db_set_avatar_hidden(&g.db, c->ui_id, (int)c->s[0], err);
        case D_AVAEXPIRE: return db_set_avatar_expire(&g.db, c->ui_id, err);
        case D_AVADEL: return db_delete_avatar(&g.db, c->ui_id, err);
    }
    return 0;
}

static void ava_fill(void) {
    ui_rows_clear(a_list);
    ui_batch(a_list, 1);
    for (int i = 0; i < g.avatars.n; i++) {
        AvatarRow *r = VEC_AT(&g.avatars, AvatarRow, i);
        char ui[16], iid[16];
        fmt(ui, sizeof(ui), "%u", r->ui_id);
        fmt(iid, sizeof(iid), "%u", r->it_id);
        char nm[96];
        const char *cells[4] = {ui, app_item_name(r->it_id, nm, sizeof(nm)), iid,
                                r->hidden ? "已隐藏" : "显示中"};
        ui_row_add(a_list, cells, 4, (LPARAM)i);
    }
    ui_batch(a_list, 0);
    ui_text(a_info, fmt_new("%s · 共 %d 件时装", g.cur.name, g.avatars.n));
}

static void ava_reload(Page *p) {
    (void)p;
    if (g_tab_pending && !job_running(&g.job_act)) tab_show();
    if (!g.char_ok) { ui_text(a_info, "请先选择角色"); return; }
    if (!(g.lists_ready & L_AVAS)) {
        if (!job_running(&g.job_act)) {
            dc.kind = D_AVAS;
            app_act("读时装", ava_work, &dc);
        }
        ui_text(a_info, "正在读取时装…");
        return;
    }
    ava_fill();
}

static AvatarRow *ava_sel(void) {
    int row = ui_row_sel(a_list);
    if (row < 0 || row >= g.avatars.n) {
        app_info("请先选中一件时装。");
        return NULL;
    }
    return VEC_AT(&g.avatars, AvatarRow, row);
}

static LRESULT ava_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    (void)lp;
    if (m != WM_COMMAND) return -1;
    if (LOWORD(wp) == A_BAD) { tab_scan_start(); return 0; }
    if (LOWORD(wp) == A_REFRESH) {
        g.avatars.n = 0;
        g.lists_ready &= ~(u32)L_AVAS;
        ava_reload(p);
        return 0;
    }
    AvatarRow *r = ava_sel();
    if (!r) return 0;
    switch (LOWORD(wp)) {
        case A_HIDE:
            dc.kind = D_AVAHIDE;
            dc.ui_id = r->ui_id;
            dc.s[0] = (char)(r->hidden ? 0 : 1);
            g.avatars.n = 0;
            app_act("切换显示", ava_work, &dc);
            return 0;
        case A_EXPIRE:
            dc.kind = D_AVAEXPIRE;
            dc.ui_id = r->ui_id;
            g.avatars.n = 0;
            app_act("时装续期", ava_work, &dc);
            return 0;
        case A_DEL:
            if (!app_confirm("删除这件时装？")) return 0;
            dc.kind = D_AVADEL;
            dc.ui_id = r->ui_id;
            g.avatars.n = 0;
            app_act("删除时装", ava_work, &dc);
            return 0;
    }
    return -1;
}

static void ava_layout(Page *p, int w, int h) {
    (void)h;
    int pad = ui_scale(6), rowh = ui_scale(23);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, A_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, A_HIDE, "显示/隐藏"), ui_scale(88));
    ui_add(&r, ui_btn_at(p->wnd, A_EXPIRE, "续期"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, A_DEL, "删除"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, A_BAD, "清错误记录"), ui_scale(88));
    ui_add(&r, a_info, -1);
    int ly = pad + rowh + ui_scale(4);
    ui_place(a_sep, pad, ly, w - pad * 2, ui_scale(2));
    ui_fill_rest(p->wnd, a_list, ly + ui_scale(6));
}

static void ava_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[] = {A_REFRESH, A_HIDE, A_EXPIRE, A_DEL, A_LIST, A_BAD};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void ava_build(Page *p, HWND parent) {
    a_list = ui_list(parent, A_LIST);
    a_sep = ui_hline(parent);
    a_info = ui_label(parent, "请先选择角色", A_INFO);
    ui_cols(a_list, (const char *const[]){"实例ID", "时装", "编号", "状态"},
            (const int[]){80, 260, 80, 80}, 4);
    ui_idtip_bind(a_list, 2);
    ui_button(parent, "刷新", A_REFRESH);
    ui_button(parent, "显示/隐藏", A_HIDE);
    ui_button(parent, "续期", A_EXPIRE);
    ui_button(parent, "删除", A_DEL);
    ui_button(parent, "清错误记录", A_BAD);
}

void page_avatar_create(Page *p) {
    p->build = ava_build;
    p->layout = ava_layout;
    p->reload = ava_reload;
    p->lock = ava_lock;
    p->on = ava_on;
}

// ================================================================ 任务页
enum { Q_LIST = PAGE_ID(PAGE_QUEST, 1), Q_REFRESH, Q_CLEAR, Q_INFO,
       Q_DROP, Q_DONE, Q_SEND, Q_PURGE };
enum { D_QUESTS = 30, D_QUESTCLR, D_QSLOT, D_QSEND, D_QPURGE };

static HWND q_list, q_info;
static HWND q_sep;

// 任务页的动作都围绕「当前选中那一行」：放弃/完成只改槽位，
// 完美清除还会顺带清掉背包里为它攒的任务材料。
typedef struct {
    int  slot;
    u32  id, trigger;
    int  purge;               // 是否连带清材料
    u32  purge_id[4];
    int  purge_n;
} QJob;
static QJob qj;
static GrantItem qsend[4];
static int  qsend_n;

static void quest_enrich(void) {
    if (!g.pvf_ok) {
        for (int i = 0; i < g.quests.n; i++) {
            QuestRow *r = VEC_AT(&g.quests, QuestRow, i);
            r->name[0] = 0;
            r->nneed = 0;
            snprintf(r->need, sizeof(r->need), "%s", g.pvf_ok ? "" : "未加载 PVF");
        }
        return;
    }
    for (int i = 0; i < g.quests.n; i++) {
        QuestRow *r = VEC_AT(&g.quests, QuestRow, i);
        QuestInfo q;
        if (!pvf_quest_info(&g.pvf, r->id, &q)) {
            r->name[0] = 0;
            r->nneed = 0;
            snprintf(r->need, sizeof(r->need), "—");
            continue;
        }
        snprintf(r->name, sizeof(r->name), "%s", q.name);
        // 这里的 4 是三处数组共同的容量（QuestRow.need_id / QJob.purge_id / qsend），
        // 现在靠 pvf.c:1399 的 `nitem < 4` 保证。夹一下，将来那边放宽了也不会直接越界。
        r->nneed = q.nitem > 4 ? 4 : q.nitem;
        r->need[0] = 0;
        for (int k = 0; k < r->nneed; k++) {
            r->need_id[k] = q.item[k];
            r->need_cnt[k] = q.cnt[k];
            const char *nm = pvf_item_name(&g.pvf, q.item[k]);
            if (k) strncat(r->need, "、", sizeof(r->need) - strlen(r->need) - 1);
            snprintf(r->need + strlen(r->need), sizeof(r->need) - strlen(r->need),
                     "%s x%u", nm && *nm ? nm : "?", q.cnt[k]);
        }
        if (!q.nitem) snprintf(r->need, sizeof(r->need), "—");
    }
}

static int quest_work(void *ctx, Err err) {
    DCtx *c = (DCtx *)ctx;
    if (c->kind == D_QUESTS) {
        g.lists_ready |= L_QUESTS;
        return db_list_quests(&g.db, &g.cur, &g.quests, err);
    }
    if (c->kind == D_QUESTCLR) return db_clear_quests(&g.db, &g.cur, err);
    if (c->kind == D_QSLOT) {
        if (!db_set_quest_slot(&g.db, &g.cur, qj.slot, qj.id, qj.trigger, err)) return 0;
        if (qj.purge && qj.purge_n)
            return db_purge_quest_items(&g.db, &g.cur, qj.purge_id, qj.purge_n, NULL, err);
        return 1;
    }
    if (c->kind == D_QSEND) {
        for (int i = 0; i < qsend_n; i++)
            if (!db_send_item_mail_ex(&g.db, &g.cur, "GM", "任务材料补发", &qsend[i], 0, 0, err))
                return 0;
        return 1;
    }
    return 0;
}

static void quest_fill(void) {
    // PVF 解析留在这里：它是内存映射 + 有 LRU 缓存，放后台线程会和界面抢
    quest_enrich();
    ui_rows_clear(q_list);
    ui_batch(q_list, 1);
    for (int i = 0; i < g.quests.n; i++) {
        QuestRow *r = VEC_AT(&g.quests, QuestRow, i);
        char slot[16], id[16], tr[16];
        fmt(slot, sizeof(slot), "%d", r->slot);
        fmt(id, sizeof(id), "%u", r->id);
        fmt(tr, sizeof(tr), "%s", r->trigger ? "进行中" : "已完成");
        const char *cells[5] = {slot, id, r->name[0] ? r->name : "—", r->need[0] ? r->need : "", tr};
        ui_row_add(q_list, cells, 5, (LPARAM)i);
    }
    ui_batch(q_list, 0);
    ui_text(q_info, fmt_new("%s · 共 %d 条任务记录", g.cur.name, g.quests.n));
}

static void quest_reload(Page *p) {
    (void)p;
    if (!g.char_ok) { ui_text(q_info, "请先选择角色"); return; }
    if (!(g.lists_ready & L_QUESTS)) {
        if (!job_running(&g.job_act)) {
            dc.kind = D_QUESTS;
            app_act("读任务", quest_work, &dc);
        }
        ui_text(q_info, "正在读取任务…");
        return;
    }
    quest_fill();
}

// 取当前选中任务；返回 0 表示没选
static QuestRow *quest_sel(void) {
    int i = ui_row_sel(q_list);
    if (i < 0 || i >= g.quests.n) return NULL;
    return VEC_AT(&g.quests, QuestRow, i);
}

static void quest_rerun(void) {
    g.quests.n = 0;
    g.lists_ready &= ~(u32)L_QUESTS;
}

static LRESULT quest_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m != WM_COMMAND) return -1;
    int id = LOWORD(wp);
    if (id == Q_REFRESH) {
        quest_rerun();
        quest_reload(p);
        return 0;
    }
    if (id == Q_CLEAR) {
        if (!app_confirm("清空该角色的全部任务记录？")) return 0;
        dc.kind = D_QUESTCLR;
        quest_rerun();
        app_act("清空任务", quest_work, &dc);
        return 0;
    }
    if (id == Q_DROP || id == Q_DONE || id == Q_SEND || id == Q_PURGE) {
        QuestRow *r = quest_sel();
        if (!r) { app_info("请先选中一行任务。"); return 0; }

        if (id == Q_DROP) {
            if (!app_confirm(fmt_new("放弃任务「%s」(%u)？", r->name[0] ? r->name : "?", r->id)))
                return 0;
            memset(&qj, 0, sizeof(qj));
            qj.slot = r->slot;
            dc.kind = D_QSLOT;
            app_act("放弃任务", quest_work, &dc);
            quest_rerun();
            return 0;
        }
        if (id == Q_DONE) {
            if (!app_confirm(fmt_new("把任务「%s」(%u) 标记为完成？\n"
                                     "需要交物品的任务，请改用「补发材料」。",
                                     r->name[0] ? r->name : "?", r->id)))
                return 0;
            memset(&qj, 0, sizeof(qj));
            qj.slot = r->slot;
            qj.id = r->id;
            dc.kind = D_QSLOT;
            app_act("标记完成", quest_work, &dc);
            quest_rerun();
            return 0;
        }
        if (id == Q_SEND) {
            if (!r->nneed) { app_info("这个任务没有可补发的物品。"); return 0; }
            if (!app_confirm(fmt_new("把任务所需材料 %s 邮件发给 %s？", r->need, g.cur.name)))
                return 0;
            qsend_n = r->nneed;
            for (int k = 0; k < r->nneed; k++) {
                GrantItem *g0 = &qsend[k];
                memset(g0, 0, sizeof(*g0));
                g0->id = r->need_id[k];
                g0->num = r->need_cnt[k] ? r->need_cnt[k] : 1;
                g0->type = (u8)(pvf_is_equip(&g.pvf, g0->id) ? IT_EQUIP : IT_STACK);
                g0->dur = 1;
            }
            dc.kind = D_QSEND;
            app_act("补发材料", quest_work, &dc);
            return 0;
        }
        // Q_PURGE：放弃任务，同时把为它攒的任务材料从背包里清掉
        if (!app_confirm(fmt_new("完美清除任务「%s」(%u)？\n\n"
                                 "会放弃该任务，并清掉背包里对应的任务材料%s。",
                                 r->name[0] ? r->name : "?", r->id,
                                 r->nneed ? r->need : "（此任务无需材料）")))
            return 0;
        memset(&qj, 0, sizeof(qj));
        qj.slot = r->slot;
        qj.purge = 1;
        qj.purge_n = r->nneed;
        for (int k = 0; k < r->nneed; k++) qj.purge_id[k] = r->need_id[k];
        dc.kind = D_QSLOT;
        app_act("完美清除", quest_work, &dc);
        quest_rerun();
        return 0;
    }
    return -1;
}

static void quest_layout(Page *p, int w, int h) {
    (void)h;
    int pad = ui_scale(6), rowh = ui_scale(23);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, Q_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, Q_DROP, "放弃所选"), ui_scale(70));
    ui_add(&r, ui_btn_at(p->wnd, Q_DONE, "标记完成"), ui_scale(70));
    ui_add(&r, ui_btn_at(p->wnd, Q_SEND, "补发材料"), ui_scale(70));
    ui_add(&r, ui_btn_at(p->wnd, Q_PURGE, "完美清除"), ui_scale(70));
    ui_add(&r, ui_btn_at(p->wnd, Q_CLEAR, "清空任务"), ui_scale(70));
    ui_add(&r, q_info, -1);
    int ly = pad + rowh + ui_scale(4);
    ui_place(q_sep, pad, ly, w - pad * 2, ui_scale(2));
    ui_fill_rest(p->wnd, q_list, ly + ui_scale(6));
}

static void quest_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[] = {Q_REFRESH, Q_CLEAR, Q_LIST, Q_DROP, Q_DONE, Q_SEND, Q_PURGE};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void quest_build(Page *p, HWND parent) {
    q_list = ui_list(parent, Q_LIST);
    q_sep = ui_hline(parent);
    q_info = ui_label(parent, "请先选择角色", Q_INFO);
    ui_cols(q_list, (const char *const[]){"槽位", "编号", "任务名", "需求物品", "状态"},
            (const int[]){48, 64, 200, 220, 72}, 5);
    ui_idtip_bind(q_list, 1);
    ui_button(parent, "刷新", Q_REFRESH);
    ui_button(parent, "清空任务", Q_CLEAR);
    ui_button(parent, "放弃所选", Q_DROP);
    ui_button(parent, "标记完成", Q_DONE);
    ui_button(parent, "补发材料", Q_SEND);
    ui_button(parent, "完美清除", Q_PURGE);
}

void page_quest_create(Page *p) {
    p->build = quest_build;
    p->layout = quest_layout;
    p->reload = quest_reload;
    p->lock = quest_lock;
    p->on = quest_on;
}

// ================================================================ 日志页
enum { L_TEXT = PAGE_ID(PAGE_LOG, 1), L_REFRESH, L_CLR, L_INFO };

static HWND l_text, l_info;
static HWND l_sep;

static void log_load(void) {
    char path[MAX_PATH];
    app_log_flush();                 // 攒在内存里的先落盘，否则刚做的事看不到
    app_log_path(path, sizeof(path));
    Buf b = {0};
    if (!file_read(path, &b)) {
        ui_text(l_text, "（暂无日志）");
        ui_text(l_info, "今天还没有日志文件");
        return;
    }
    size_t start = b.n > 100000 ? b.n - 100000 : 0;

    // 文件是顺序追写的，直接顺下来就是「最早那条在最上面」，看日志等于每次都要拖到
    // 最后。倒着拼：先把行首偏移收一遍（倒着走才知道每段到哪收尾），再从后往前取。
    Vec ln;
    vec_init(&ln, sizeof(size_t));
    *(size_t *)VEC_ADD(&ln, size_t) = start;
    for (size_t i = start; i + 1 < b.n; i++)
        if (b.p[i] == '\n') *(size_t *)VEC_ADD(&ln, size_t) = i + 1;

    Str s = {0};
    for (int k = ln.n - 1; k >= 0; k--) {
        size_t at = *(size_t *)VEC_AT(&ln, size_t, k);
        size_t to = (size_t)(k + 1 < ln.n ? *(size_t *)VEC_AT(&ln, size_t, k + 1) : b.n);
        // 行尾的 \r\n 都剥掉再统一补 \r\n —— 只减 1 会把上一行的 \r 留在
        // 行尾，编辑框里就冒出 \r\r\n 和空行（踩过：日志页行距忽宽忽窄）
        while (to > at && (b.p[to - 1] == '\r' || b.p[to - 1] == '\n')) to--;
        if (to > at) str_catn(&s, (const char *)b.p + at, (int)(to - at));
        str_cat(&s, "\r\n");
    }
    int first = (int)SendMessageW(l_text, EM_GETFIRSTVISIBLELINE, 0, 0);
    ui_text(l_text, str_c(&s));
    // 重写整段文本会把视口拽回第一行 —— 这版第一行恰恰是最新的，正是想看的，所以
    // 不用再做「跟到底」。只有正在往下翻历史的人要还原位置，别给他掀回开头。
    if (first > 0) SendMessageW(l_text, EM_LINESCROLL, 0, (LPARAM)first);
    str_free(&s);
    vec_free(&ln);
    buf_free(&b);
    // 只显示文件名，绝对路径太长会把按钮挤走
    const char *nm = strrchr(path, '\\');
    ui_text(l_info, nm ? nm + 1 : path);
}

static void log_reload(Page *p) {
    (void)p;
    // 日志页不在前台时别刷：一次要读 100KB 盘、重写整块文本，
    // 而用户在别的页做事的几十毫秒里它会被 data_ver/act_ver 反复吆喝。
    // 切回本页时 select_page 一定会 reload，不会漏。
    if (p->wnd && !IsWindowVisible(p->wnd)) return;
    // 正在选中/复制日志的人也别打断：选中区被 TextOut 重写会直接消失
    DWORD s0 = 0, s1 = 0;
    if (l_text) SendMessageW(l_text, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    if (s1 > s0) return;
    log_load();
}

static LRESULT log_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    (void)lp;
    if (m != WM_COMMAND) return -1;
    if (LOWORD(wp) == L_REFRESH) { log_load(); return 0; }
    if (LOWORD(wp) == L_CLR) {
        if (!app_confirm("清空今天的日志？")) return 0;
        char path[MAX_PATH];
        app_log_flush();
        app_log_path(path, sizeof(path));
        file_write(path, "", 0);
        log_load();
        return 0;
    }
    return -1;
}

static void log_layout(Page *p, int w, int h) {
    (void)h;
    int pad = ui_scale(6), rowh = ui_scale(23);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, L_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, L_CLR, "清空"), ui_scale(60));
    ui_add(&r, l_info, -1);
    int ly = pad + rowh + ui_scale(4);
    ui_place(l_sep, pad, ly, w - pad * 2, ui_scale(2));
    ui_fill_rest(p->wnd, l_text, ly + ui_scale(6));
}

static void log_lock(Page *p) {
    (void)p;
}

static void log_build(Page *p, HWND parent) {
    (void)p;
    l_text = ui_make(parent, L"EDIT", L"",
                     WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                     WS_EX_CLIENTEDGE, L_TEXT);
    l_info = ui_label(parent, "", L_INFO);
    l_sep = ui_hline(parent);
    ui_button(parent, "刷新", L_REFRESH);
    ui_button(parent, "清空", L_CLR);
}

void page_log_create(Page *p) {
    p->build = log_build;
    p->layout = log_layout;
    p->reload = log_reload;
    p->lock = log_lock;
    p->on = log_on;
}
