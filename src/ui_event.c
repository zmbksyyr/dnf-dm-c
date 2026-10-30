// ui_event：活动页。启用/停用/清空走 act.c 的 K_EVADD/K_EVDEL/K_EVCLEAR；
// 新增活动定义走 K_EVADDICT。
#include "ui.h"
#include "act.h"

// ================================================================ 活动页
// dnf_event_info 是活动字典（编号→中文说明），dnf_event_log 是「当前启用的活动」。
// 启用 = 往 log 插一条（带两个参数 + 起止时间），停用 = 删掉对应 log 行；对全服生效。
enum {
    E_COMBO = PAGE_ID(PAGE_EVENT, 1), E_P1, E_P2, E_ADD,
    E_LIST, E_DEL, E_REF, E_INFO, E_GRP1, E_GRP2,
    E_EID, E_DAYS, E_CLEAR,            // 编号直接填 / 持续时间(天) / 清空活动
    E_DID, E_DNAME, E_DEXP, E_ADDICT   // 新增活动定义：编号 / 名称 / 说明 / 提交
};

static HWND e_combo, e_p1, e_p2, e_list, e_info;
static HWND e_eid, e_days, e_did, e_dname, e_dexp;
static char  eBuf[256][96];
static const char *eLbl[256];
static int   eN, eSync;

static void ev_fill_combo(void) {
    int n = g.event_info.n > (int)COUNT_OF(eBuf) ? (int)COUNT_OF(eBuf) : g.event_info.n;
    // 活动字典没变就不重建：CB_RESETCONTENT 会把用户选中的活动和正展开的列表清掉
    if (n == eN && (int)SendMessageW(e_combo, CB_GETCOUNT, 0, 0) == n) {
        if (ui_combo_sel(e_combo) < 0) ui_combo_set(e_combo, 0);
        return;
    }
    eN = n;
    for (int i = 0; i < eN; i++) {
        EventInfo *ei = VEC_AT(&g.event_info, EventInfo, i);
        snprintf(eBuf[i], sizeof(eBuf[i]), "%u · %s", ei->id,
                 ei->explain[0] ? ei->explain : ei->name);
        eLbl[i] = eBuf[i];
    }
    ui_combo_fill(e_combo, eLbl, eN, 0);
}

static void ev_fill_list(void) {
    eSync = 1;
    ui_rows_clear(e_list);
    ui_batch(e_list, 1);
    for (int i = 0; i < g.events.n; i++) {
        EventRow *e = VEC_AT(&g.events, EventRow, i);
        char logid[16], typ[16], p1[24], p2[24];
        fmt(logid, sizeof(logid), "%llu", (unsigned long long)e->log_id);
        fmt(typ, sizeof(typ), "%u", e->type);
        fmt(p1, sizeof(p1), "%lld", (long long)e->p1);
        fmt(p2, sizeof(p2), "%lld", (long long)e->p2);
        const char *cells[6] = {logid, e->explain[0] ? e->explain : e->name, typ, p1, p2, e->name};
        ui_row_add(e_list, cells, 6, (LPARAM)i);
    }
    ui_batch(e_list, 0);
    eSync = 0;
    ui_text(e_info, fmt_new("已启用 %d 项 · 活动字典 %d 条", g.events.n, g.event_info.n));
}

static void ev_reload(Page *p) {
    (void)p;
    if (!g.db_ok) {
        ui_text(e_info, "请先连接数据库");
        return;
    }
    if (!(g.lists_ready & L_EVENTS)) {
        if (!job_running(&g.job_act)) {
            kc.kind = K_EVLIST;
            app_act("读取活动", char_work, &kc);
        }
        ui_text(e_info, "正在读取活动…");
        return;
    }
    ev_fill_combo();
    ev_fill_list();
}

// 取下拉里选中活动的类型；下拉没选但「编号」填了有效数字，则用填的。
// 返回类型编号（0 表示都没给），label 带回展示名。
static u32 ev_pick_type(char *label, int cap) {
    u64 eid = (u64)ui_i64(e_eid);
    if (eid > 0) {
        const char *nm = NULL;
        for (int i = 0; i < g.event_info.n; i++) {
            EventInfo *ei = VEC_AT(&g.event_info, EventInfo, i);
            if (ei->id == (u32)eid) { nm = ei->explain[0] ? ei->explain : ei->name; break; }
        }
        snprintf(label, cap, "%s (编号 %u)", nm ? nm : "未在字典中", (u32)eid);
        return (u32)eid;
    }
    int sel = ui_combo_sel(e_combo);
    if (sel < 0 || sel >= eN) return 0;
    EventInfo *ei = VEC_AT(&g.event_info, EventInfo, sel);
    snprintf(label, cap, "%s", ei->explain[0] ? ei->explain : ei->name);
    return ei->id;
}

static LRESULT ev_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == E_LIST) return 0;
        return -1;
    }
    if (m != WM_COMMAND) return -1;
    switch (LOWORD(wp)) {
        case E_ADD: {
            char label[128];
            u32 type = ev_pick_type(label, sizeof(label));
            if (!type) { app_info("请先在「活动」下拉里选一个，或在「编号」里直接填活动编号。"); return 0; }
            u64 p1 = (u64)ui_i64(e_p1), p2 = (u64)ui_i64(e_p2);
            int days = ui_int(e_days);
            if (days < 0) days = 0;
            if (!app_confirm(fmt_new("启用活动「%s」？\n\n编号 %u · 参数 %llu / %llu%s\n全局生效，全服玩家都会受影响。",
                                     label, type,
                                     (unsigned long long)p1, (unsigned long long)p2,
                                     days > 0 ? fmt_new(" · 持续 %d 天", days) : " · 永久(无时限)")))
                return 0;
            kc.kind = K_EVADD;
            kc.i1 = (int)type;
            kc.u1 = p1;
            kc.u2 = p2;
            kc.i2 = days;
            g.lists_ready &= ~(u32)L_EVENTS;
            app_act("启用活动", char_work, &kc);
            return 0;
        }
        case E_CLEAR: {
            if (!app_confirm("清空所有已启用活动？\n\ndnf_event_log 会被整张 DELETE，所有当前活动立即全部停用，不可恢复。"))
                return 0;
            kc.kind = K_EVCLEAR;
            g.lists_ready &= ~(u32)L_EVENTS;
            app_act("清空活动", char_work, &kc);
            return 0;
        }
        case E_ADDICT: {
            u32 id = (u32)ui_i64(e_did);
            char name[160], exp[160];
            ui_get(e_dname, name, sizeof(name)); str_trim(name);
            ui_get(e_dexp, exp, sizeof(exp)); str_trim(exp);
            if (!id) { app_info("请填写活动编号。"); return 0; }
            if (!name[0]) { app_info("请填写活动名称。"); return 0; }
            if (!app_confirm(fmt_new("新增活动定义：编号 %u · 名称「%s」？\n写入 dnf_event_info（活动字典），之后可在上方直接选用。",
                                     id, name)))
                return 0;
            kc.kind = K_EVADDICT;
            kc.u1 = id;
            snprintf(kc.s, sizeof(kc.s), "%s", name);
            snprintf(kc.s2, sizeof(kc.s2), "%s", exp);
            g.lists_ready &= ~(u32)L_EVENTS;   // 字典变了，刷新后重拉
            app_act("新增活动定义", char_work, &kc);
            return 0;
        }
        case E_DEL: {
            int row = ui_row_sel(e_list);
            if (row < 0 || row >= g.events.n) { app_info("请先在列表里选一项。"); return 0; }
            EventRow *e = VEC_AT(&g.events, EventRow, row);
            if (!app_confirm(fmt_new("停用「%s」？（删掉 log_id=%llu）",
                                     e->explain[0] ? e->explain : e->name,
                                     (unsigned long long)e->log_id)))
                return 0;
            kc.kind = K_EVDEL;
            kc.u1 = e->log_id;
            g.lists_ready &= ~(u32)L_EVENTS;
            app_act("停用活动", char_work, &kc);
            return 0;
        }
        case E_REF:
            g.lists_ready &= ~(u32)L_EVENTS;
            ev_reload(p);
            return 0;
        case E_COMBO:
            return 0;
    }
    return -1;
}

static void ev_layout(Page *p, int w, int h) {
    (void)w;
    (void)h;
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int pad = ui_scale(8), rowh = ui_scale(23), g = ui_scale(6);
    int x0 = pad, cw = rc.right - pad * 2;
    URow r;

    // 组一：启用活动 —— 三行：下拉+参数 / 编号+时长+清空 / 新增定义
    int y = pad;
    int rows = 3;
    int g1h = ui_scale(20) + rowh * rows + g * (rows - 1) + ui_scale(4);
    ui_place(GetDlgItem(p->wnd, E_GRP1), x0, y, cw, g1h);

    // 行 1：活动 + 两个参数 + 启用
    int iy = y + ui_scale(20);
    ui_row(&r, p->wnd, x0 + ui_scale(10), iy, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "活动"), ui_scale(32));
    int fixed = ui_scale(32 + 38 + 50 + 38 + 50 + 76) + ui_scale(6) * 6;
    int combo_w = cw - ui_scale(20) - fixed;
    if (combo_w < ui_scale(120)) combo_w = ui_scale(120);
    ui_add(&r, e_combo, combo_w);
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_lbl_at(p->wnd, "参数1"), ui_scale(38));
    ui_add(&r, e_p1, ui_scale(50));
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_lbl_at(p->wnd, "参数2"), ui_scale(38));
    ui_add(&r, e_p2, ui_scale(50));
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_btn_at(p->wnd, E_ADD, "启用活动"), ui_scale(76));

    // 行 2：编号直接填 + 持续时间 + 清空活动
    iy += rowh + g;
    ui_row(&r, p->wnd, x0 + ui_scale(10), iy, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "编号"), ui_scale(32));
    ui_add(&r, e_eid, ui_scale(64));
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_lbl_at(p->wnd, "持续天"), ui_scale(40));
    ui_add(&r, e_days, ui_scale(44));
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_btn_at(p->wnd, E_CLEAR, "清空活动"), ui_scale(80));

    // 行 3：新增活动定义（编号/名称/说明/提交）
    iy += rowh + g;
    ui_row(&r, p->wnd, x0 + ui_scale(10), iy, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "新定义"), ui_scale(44));
    ui_add(&r, ui_lbl_at(p->wnd, "编号"), ui_scale(32));
    ui_add(&r, e_did, ui_scale(56));
    ui_gap(&r, ui_scale(4));
    ui_add(&r, ui_lbl_at(p->wnd, "名称"), ui_scale(32));
    ui_add(&r, e_dname, ui_scale(120));
    ui_gap(&r, ui_scale(4));
    ui_add(&r, ui_lbl_at(p->wnd, "说明"), ui_scale(32));
    ui_add(&r, e_dexp, ui_scale(160));
    ui_gap(&r, ui_scale(4));
    ui_add(&r, ui_btn_at(p->wnd, E_ADDICT, "新增定义"), ui_scale(76));

    // 组二：当前启用的活动 —— 刷新/停用所选跟列表待在一起，状态在标题行
    y += g1h + g;
    int g2h = rc.bottom - pad - y;
    ui_place(GetDlgItem(p->wnd, E_GRP2), x0, y, cw, g2h);
    iy = y + ui_scale(20);
    ui_row(&r, p->wnd, x0 + ui_scale(10), iy, rowh);
    ui_add(&r, ui_btn_at(p->wnd, E_REF, "刷新"), ui_scale(60));
    ui_gap(&r, ui_scale(6));
    ui_add(&r, ui_btn_at(p->wnd, E_DEL, "停用所选"), ui_scale(80));
    ui_gap(&r, ui_scale(8));
    ui_add(&r, e_info, -1);
    iy += rowh + ui_scale(4);
    MoveWindow(e_list, x0 + ui_scale(10), iy, cw - ui_scale(20),
               y + g2h - iy - ui_scale(10), TRUE);
}

static void ev_lock(Page *p) {
    (void)p;
    int ids[] = {E_COMBO, E_P1, E_P2, E_ADD, E_DEL, E_REF, E_LIST,
                 E_EID, E_DAYS, E_CLEAR, E_DID, E_DNAME, E_DEXP, E_ADDICT};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), g.db_ok);
}

static void ev_build(Page *p, HWND parent) {
    // 组框先建，后面的控件才盖在上面
    ui_group(parent, "启用活动", E_GRP1);
    ui_group(parent, "当前启用的活动", E_GRP2);
    e_combo = ui_combo(parent, E_COMBO, 1);
    e_p1 = ui_edit(parent, "1", E_P1, 0);
    e_p2 = ui_edit(parent, "0", E_P2, 0);
    e_eid = ui_edit(parent, "", E_EID, 0);
    e_days = ui_edit(parent, "0", E_DAYS, 0);
    e_did = ui_edit(parent, "", E_DID, 0);
    e_dname = ui_edit(parent, "", E_DNAME, 0);
    e_dexp = ui_edit(parent, "", E_DEXP, 0);
    e_list = ui_list(parent, E_LIST);
    e_info = ui_label(parent, "请先连接数据库", E_INFO);
    ui_cols(e_list, (const char *const[]){"日志号", "活动", "编号", "参数1", "参数2", "内部名"},
            (const int[]){64, 220, 56, 64, 64, 200}, 6);
    ui_button(parent, "启用活动", E_ADD);
    ui_button(parent, "停用所选", E_DEL);
    ui_button(parent, "刷新", E_REF);
    ui_button(parent, "清空活动", E_CLEAR);
    ui_button(parent, "新增定义", E_ADDICT);
    ui_tip(parent, GetDlgItem(parent, E_ADD), "把选中的活动写进 dnf_event_log（带两个参数），全服立即生效；「持续天」填 0 表示永久");
    ui_tip(parent, GetDlgItem(parent, E_CLEAR), "整张 DELETE dnf_event_log，所有当前活动立即全部停用");
    ui_tip(parent, GetDlgItem(parent, E_EID), "直接填活动编号即可启用，不必在长下拉里翻找");
    ui_tip(parent, GetDlgItem(parent, E_DAYS), "启用的持续天数；0 = 永久（start_time/end_time 写 0，无时限）");
    ui_tip(parent, GetDlgItem(parent, E_ADDICT), "往 dnf_event_info 活动字典插一条，下拉里没有的活动可自己补定义");
    ui_tip(parent, GetDlgItem(parent, E_DEL), "删掉列表里选中的那条启用记录，该活动随即停用");
    ui_tip(parent, GetDlgItem(parent, E_REF), "重新读取活动字典与当前启用列表");
    (void)p;
}

void page_event_create(Page *p) {
    p->build = ev_build;
    p->layout = ev_layout;
    p->reload = ev_reload;
    p->lock = ev_lock;
    p->on = ev_on;
}
