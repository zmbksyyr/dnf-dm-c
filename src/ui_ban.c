// ui_ban：封停页。封停/解封/重置密码都走 act.c 的命令表（K_BAN/K_UNBAN/K_RESETPWD）。
#include "ui.h"
#include "act.h"

// ================================================================ 封停页
enum { B_LIST = PAGE_ID(PAGE_BAN, 1), B_ACC, B_DAYS, B_REASON, B_BAN, B_UNBAN, B_REFRESH, B_INFO,
       B_PWD, B_RESETPWD };

static HWND b_list, b_acc, b_days, b_reason, b_info, b_pwd;

static void ban_fill(void) {
    ui_rows_clear(b_list);
    ui_batch(b_list, 1);
    for (int i = 0; i < g.bans.n; i++) {
        BanRow *b = VEC_AT(&g.bans, BanRow, i);
        char mid[16], d1[8], d2[8];
        fmt(mid, sizeof(mid), "%u", b->mid);
        fmt(d1, sizeof(d1), "%d", b->type);
        fmt(d2, sizeof(d2), "%d", b->value);
        const char *cells[7] = {b->account, mid, d1, d2, b->start, b->end, b->reason};
        ui_row_add(b_list, cells, 7, (LPARAM)i);
    }
    ui_batch(b_list, 0);
    ui_text(b_info, fmt_new("共 %d 条封停记录", g.bans.n));
}

static void ban_reload(Page *p) {
    (void)p;
    if (!g.db_ok) { ui_text(b_info, "请先连接数据库"); return; }
    if (!(g.lists_ready & L_BANS)) {
        if (!job_running(&g.job_act)) {
            kc.kind = K_BANLIST;
            app_act("读取封停", char_work, &kc);
        }
        ui_text(b_info, "正在读取封停记录…");
        return;
    }
    ban_fill();
}

// 输入框里可以填账号名、角色名或纯数字 mid
static int ban_mid(u32 *out) {
    char acc[64];
    ui_get(b_acc, acc, sizeof(acc));
    str_trim(acc);
    if (!acc[0]) { app_info("请输入账号名或 mid。"); return 0; }
    if (acc[0] >= '0' && acc[0] <= '9') { *out = (u32)atoi(acc); return 1; }
    Err err;
    if (!db_find_mid(&g.db, acc, out, err)) {
        app_info(fmt_new("找不到账号「%s」：%s", acc, err));
        return 0;
    }
    return 1;
}

static LRESULT ban_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m != WM_COMMAND) return -1;
    switch (LOWORD(wp)) {
        case B_REFRESH:
            g.bans.n = 0;
            g.lists_ready &= ~(u32)L_BANS;
            ban_reload(p);
            return 0;
        case B_BAN: {
            char acc[64], reason[160];
            ui_get(b_acc, acc, sizeof(acc));
            ui_get(b_reason, reason, sizeof(reason));
            str_trim(acc);
            str_trim(reason);
            if (!acc[0] || acc[0] < '0' || acc[0] > '9') { app_info("封停请填数字 mid。"); return 0; }
            u32 mid = (u32)atoi(acc);
            int days = ui_int(b_days);
            if (days < 1) days = 1;
            if (!app_confirm(fmt_new("封停 mid=%u 共 %d 天？", mid, days))) return 0;
            kc.kind = K_BAN;
            kc.mid = mid;
            kc.i1 = days;
            kc.i2 = 1;
            kc.i3 = 1;
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
            if (!app_confirm(fmt_new("解除 mid=%u（%s）的封停？", b->mid, b->account))) return 0;
            kc.kind = K_UNBAN;
            kc.mid = b->mid;
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
    int btm = rc.bottom - pad - rowh * 2 - g * 2;
    MoveWindow(b_list, pad, y, rc.right - pad * 2, btm - y, TRUE);

    y = btm + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "账号/mid"), ui_scale(60));
    ui_add(&r, b_acc, ui_scale(140));
    ui_add(&r, ui_lbl_at(p->wnd, "天数"), ui_scale(34));
    ui_add(&r, b_days, ui_scale(56));
    ui_add(&r, ui_lbl_at(p->wnd, "原因"), ui_scale(34));
    ui_add(&r, b_reason, -1);
    // 最后一行：封停 + 账号密码管理，窄窗口也只占两行
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_btn_at(p->wnd, B_BAN, "执行封停"), ui_scale(88));
    ui_add(&r, ui_lbl_at(p->wnd, "新密码"), ui_scale(52));
    ui_add(&r, b_pwd, ui_scale(100));
    ui_add(&r, ui_btn_at(p->wnd, B_RESETPWD, "重置密码"), ui_scale(76));
    ui_add(&r, ui_lbl_at(p->wnd, "（账号级操作在 GM 页）"), -1);
}

static void ban_lock(Page *p) {
    (void)p;
    int on = g.db_ok;
    int ids[] = {B_BAN, B_UNBAN, B_REFRESH, B_ACC, B_DAYS, B_REASON, B_PWD, B_RESETPWD};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void ban_build(Page *p, HWND parent) {
    b_list = ui_list(parent, B_LIST);
    b_info = ui_label(parent, "请先连接数据库", B_INFO);
    b_acc = ui_edit(parent, "", B_ACC, 0);
    b_days = ui_edit(parent, "7", B_DAYS, 0);
    b_reason = ui_edit(parent, "", B_REASON, 0);
    b_pwd = ui_edit(parent, "123456", B_PWD, 0);
    ui_cols(b_list, (const char *const[]){"账号", "mid", "类型", "值", "开始", "结束", "原因"},
            (const int[]){120, 60, 44, 44, 130, 130, 180}, 7);
    ui_button(parent, "刷新", B_REFRESH);
    ui_button(parent, "解除封停", B_UNBAN);
    ui_button(parent, "执行封停", B_BAN);
    ui_button(parent, "重置密码", B_RESETPWD);
    ui_tip(parent, GetDlgItem(parent, B_REFRESH), "重新读取封停记录（已过期的不会列出）");
    ui_tip(parent, GetDlgItem(parent, B_BAN), "按上面填的 mid 与天数写入一条封停");
    ui_tip(parent, GetDlgItem(parent, B_UNBAN), "解除列表里选中那条封停");
    ui_tip(parent, GetDlgItem(parent, B_RESETPWD), "把上面账号/mid 的登录密码改成「新密码」里的值");
}

void page_ban_create(Page *p) {
    p->build = ban_build;
    p->layout = ban_layout;
    p->reload = ban_reload;
    p->lock = ban_lock;
    p->on = ban_on;
}
