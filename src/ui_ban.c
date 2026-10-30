// ui_ban：封停页。封停/解封/重置密码都走 act.c 的命令表（K_BAN/K_UNBAN/K_RESETPWD）。
// 支持账号封禁（punish_type=1）与角色封禁（punish_type=2，punish_value=charac_no）。
#include "ui.h"
#include "act.h"

// ================================================================ 封停页
enum { B_LIST = PAGE_ID(PAGE_BAN, 1), B_CHAR, B_BANTYPE, B_DAYS, B_REASON, B_BAN, B_UNBAN,
       B_REFRESH, B_INFO, B_PWD, B_RESETPWD };

static HWND b_list, b_char, b_bantype, b_days, b_reason, b_info, b_pwd;

// 角色下拉选择器：数据源 g.ban_chars（Vec<Charac>）
// 用平行数组存 charac_no / mid，CB_SETITEMDATA 存索引
static u32 s_char_no[5000], s_char_mid[5000];
static int s_char_n;

static void ban_fill_chars(void) {
    SendMessageW(b_char, CB_RESETCONTENT, 0, 0);
    s_char_n = 0;
    for (int i = 0; i < g.ban_chars.n && s_char_n < 5000; i++) {
        Charac *c = VEC_AT(&g.ban_chars, Charac, i);
        char label[128];
        fmt(label, sizeof(label), "%s (No.%u) · %s", c->name, c->no, c->account);
        SendMessageW(b_char, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageW(b_char, CB_SETITEMDATA, (WPARAM)s_char_n, (LPARAM)s_char_n);
        s_char_no[s_char_n] = c->no;
        s_char_mid[s_char_n] = c->mid;
        s_char_n++;
    }
    if (s_char_n > 0) SendMessageW(b_char, CB_SETCURSEL, 0, 0);
}

// 返回当前下拉选中角色的索引（-1 = 没选）
static int ban_sel_idx(void) {
    int idx = (int)SendMessageW(b_char, CB_GETCURSEL, 0, 0);
    return (idx >= 0 && idx < s_char_n) ? idx : -1;
}

static void ban_fill(void) {
    ui_rows_clear(b_list);
    ui_batch(b_list, 1);
    for (int i = 0; i < g.bans.n; i++) {
        BanRow *b = VEC_AT(&g.bans, BanRow, i);
        char mid[16], type[12], target[64];
        fmt(mid, sizeof(mid), "%u", b->mid);
        if (b->type == 2) {
            fmt(type, sizeof(type), "角色");
            fmt(target, sizeof(target), "%s (No.%u)", b->char_name[0] ? b->char_name : "?", b->charac_no);
        } else {
            fmt(type, sizeof(type), "账号");
            fmt(target, sizeof(target), "%s", b->account);
        }
        const char *cells[7] = {b->account, mid, type, target, b->start, b->end, b->reason};
        ui_row_add(b_list, cells, 7, (LPARAM)i);
    }
    ui_batch(b_list, 0);
    ui_text(b_info, fmt_new("共 %d 条封停记录 · %d 个角色可选", g.bans.n, s_char_n));
}

static void ban_reload(Page *p) {
    (void)p;
    if (!g.db_ok) { ui_text(b_info, "请先连接数据库"); return; }
    // 并行拉封停记录和角色列表
    if (!(g.lists_ready & L_BANS) || !(g.lists_ready & L_BANCHARS)) {
        if (!job_running(&g.job_act)) {
            if (!(g.lists_ready & L_BANS)) {
                memset(&kc, 0, sizeof(kc));
                kc.kind = K_BANLIST;
                app_act("读取封停", char_work, &kc);
            } else {
                memset(&kc, 0, sizeof(kc));
                kc.kind = K_BANCHARS;
                app_act("读取角色列表", char_work, &kc);
            }
        }
        ui_text(b_info, "正在读取数据…");
        return;
    }
    if (!s_char_n && g.ban_chars.n) ban_fill_chars();
    ban_fill();
}

// 重置密码用：从下拉选中的角色取 mid
static int ban_mid(u32 *out) {
    int idx = ban_sel_idx();
    if (idx < 0) { app_info("请先在列表中选一个角色。"); return 0; }
    *out = s_char_mid[idx];
    return 1;
}

static LRESULT ban_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m != WM_COMMAND) return -1;
    switch (LOWORD(wp)) {
        case B_REFRESH:
            g.bans.n = 0;
            g.ban_chars.n = 0;
            s_char_n = 0;
            g.lists_ready &= ~(u32)(L_BANS | L_BANCHARS);
            ban_reload(p);
            return 0;
        case B_BAN: {
            int idx = ban_sel_idx();
            char reason[160];
            ui_get(b_reason, reason, sizeof(reason));
            str_trim(reason);
            if (idx < 0) { app_info("请先在列表中选一个角色。"); return 0; }
            u32 mid = s_char_mid[idx];
            u32 charac_no = s_char_no[idx];
            // 封禁类型：0=账号封禁 1=角色封禁
            int is_char_ban = (int)SendMessageW(b_bantype, CB_GETCURSEL, 0, 0) == 1;
            int days = ui_int(b_days);
            if (days < 1) days = 1;

            if (is_char_ban) {
                if (!app_confirm(fmt_new("角色封禁：封停角色 No.%u（所属账号 mid=%u）共 %d 天？",
                                         charac_no, mid, days)))
                    return 0;
                kc.kind = K_BAN;
                kc.mid = mid;
                kc.i1 = days;
                kc.i2 = 2;           // punish_type=2 角色封禁
                kc.i3 = (int)charac_no;  // punish_value=charac_no
            } else {
                if (!app_confirm(fmt_new("账号封禁：封停账号 mid=%u 共 %d 天？", mid, days)))
                    return 0;
                kc.kind = K_BAN;
                kc.mid = mid;
                kc.i1 = days;
                kc.i2 = 1;           // punish_type=1 账号封禁
                kc.i3 = 1;
            }
            snprintf(kc.s, sizeof(kc.s), "%s", reason[0] ? reason : "违规");
            g.bans.n = 0;
            g.lists_ready &= ~(u32)L_BANS;
            app_act("封停", char_work, &kc);
            return 0;
        }
        case B_UNBAN: {
            int row = ui_row_sel(b_list);
            if (row < 0 || row >= g.bans.n) { app_info("请先在列表中选一条记录。"); return 0; }
            BanRow *b = VEC_AT(&g.bans, BanRow, row);
            const char *type_name = b->type == 2 ? "角色" : "账号";
            if (!app_confirm(fmt_new("解除 %s mid=%u（%s）的封停？", type_name, b->mid, b->account)))
                return 0;
            kc.kind = K_UNBAN;
            kc.mid = b->mid;
            kc.i2 = b->type;   // 按惩罚类型精确解封
            g.bans.n = 0;
            g.lists_ready &= ~(u32)L_BANS;
            app_act("解封", char_work, &kc);
            return 0;
        }
        case B_RESETPWD: {
            char pwd[64];
            ui_get(b_pwd, pwd, sizeof(pwd));
            str_trim(pwd);
            if (!pwd[0]) { app_info("请输入新密码。"); return 0; }
            u32 mid = 0;
            if (!ban_mid(&mid)) return 0;
            if (!app_confirm(fmt_new("把 mid=%u 的密码重置为「%s」？", mid, pwd))) return 0;
            kc.kind = K_RESETPWD;
            kc.mid = mid;
            snprintf(kc.s, sizeof(kc.s), "%s", pwd);
            app_act("重置密码", char_work, &kc);
            return 0;
        }
    }
    return -1;
}

static void ban_layout(Page *p, int w, int h) {
    (void)w;
    (void)h;
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int pad = ui_scale(6), rowh = ui_scale(23), g = ui_scale(5);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, B_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, B_UNBAN, "解除封停"), ui_scale(80));
    ui_add(&r, b_info, -1);

    int y = pad + rowh + g;
    // 列表 + 底部操作区（3 行）
    int btm = rc.bottom - pad - rowh * 3 - g * 3;
    MoveWindow(b_list, pad, y, rc.right - pad * 2, btm - y, TRUE);

    // 第一行：角色选择 + 封禁类型 + 天数
    y = btm + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "角色"), ui_scale(34));
    ui_add(&r, b_char, ui_scale(280));
    ui_add(&r, ui_lbl_at(p->wnd, "类型"), ui_scale(34));
    ui_add(&r, b_bantype, ui_scale(90));
    ui_add(&r, ui_lbl_at(p->wnd, "天数"), ui_scale(34));
    ui_add(&r, b_days, ui_scale(56));
    ui_add(&r, ui_lbl_at(p->wnd, "原因"), ui_scale(34));
    ui_add(&r, b_reason, -1);

    // 第二行：封停按钮 + 密码管理
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_btn_at(p->wnd, B_BAN, "执行封停"), ui_scale(88));
    ui_add(&r, ui_lbl_at(p->wnd, "新密码"), ui_scale(52));
    ui_add(&r, b_pwd, ui_scale(100));
    ui_add(&r, ui_btn_at(p->wnd, B_RESETPWD, "重置密码"), ui_scale(76));
    ui_add(&r, ui_lbl_at(p->wnd, "（账号级操作在 GM 页）"), -1);

    // 第三行：提示
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd,
        "封禁类型：账号 = 封停整个账号登录；角色 = 只禁该角色，账号仍可登录其它角色"), -1);
}

static void ban_lock(Page *p) {
    (void)p;
    int on = g.db_ok;
    int ids[] = {B_BAN, B_UNBAN, B_REFRESH, B_CHAR, B_BANTYPE, B_DAYS, B_REASON, B_PWD, B_RESETPWD};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void ban_build(Page *p, HWND parent) {
    b_list = ui_list(parent, B_LIST);
    b_info = ui_label(parent, "请先连接数据库", B_INFO);
    b_char = ui_combo(parent, B_CHAR, 1);   // droplist：只能从下拉选
    b_bantype = ui_combo(parent, B_BANTYPE, 1);
    b_days = ui_edit(parent, "7", B_DAYS, 0);
    b_reason = ui_edit(parent, "", B_REASON, 0);
    b_pwd = ui_edit(parent, "123456", B_PWD, 0);
    // 封禁类型下拉
    SendMessageW(b_bantype, CB_ADDSTRING, 0, (LPARAM)L"账号封禁");
    SendMessageW(b_bantype, CB_ADDSTRING, 0, (LPARAM)L"角色封禁");
    SendMessageW(b_bantype, CB_SETCURSEL, 0, 0);
    ui_cols(b_list, (const char *const[]){"账号", "mid", "类型", "目标", "开始", "结束", "原因"},
            (const int[]){110, 56, 44, 160, 125, 125, 160}, 7);
    ui_button(parent, "刷新", B_REFRESH);
    ui_button(parent, "解除封停", B_UNBAN);
    ui_button(parent, "执行封停", B_BAN);
    ui_button(parent, "重置密码", B_RESETPWD);
    ui_tip(parent, GetDlgItem(parent, B_REFRESH), "重新读取封停记录与角色列表");
    ui_tip(parent, b_char, "从下拉列表中选择要封停的角色；列表包含所有角色（含已删除的）");
    ui_tip(parent, b_bantype, "账号封禁 = 封停整个账号登录；角色封禁 = 只禁该角色，账号仍可登录其它角色");
    ui_tip(parent, GetDlgItem(parent, B_BAN), "按选中的角色和封禁类型写入一条封停记录");
    ui_tip(parent, GetDlgItem(parent, B_UNBAN), "解除列表里选中那条封停（按惩罚类型精确解封）");
    ui_tip(parent, GetDlgItem(parent, B_RESETPWD), "把选中角色所属账号的登录密码改成「新密码」里的值");
}

void page_ban_create(Page *p) {
    p->build = ban_build;
    p->layout = ban_layout;
    p->reload = ban_reload;
    p->lock = ban_lock;
    p->on = ban_on;
}
