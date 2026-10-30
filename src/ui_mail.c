// ui_mail：邮件列表、删除、发送文本/附件邮件。
// 增强：附件可发为「时装(avata) / 宠物(creature)」（先建 user_items/creature_items 行取 ui_id）；
//       目标可设为「全服 / 在线玩家」批量发信；并支持「删除全服邮件」。
#include "ui.h"

enum {
    M_LIST = PAGE_ID(PAGE_MAIL, 1), M_DEL, M_DELALL, M_INFO, M_TEXT, M_SENDT, M_IID, M_INUM, M_IMSG,
    M_SENDI, M_REFRESH = PAGE_ID(PAGE_MAIL, 20),
    M_TARGET = PAGE_ID(PAGE_MAIL, 21), M_ATYPE, M_DELALLS
};

static HWND m_list, m_info, m_text, m_iid, m_inum, m_imsg, m_target, m_atype;

static const char *kMailTarget[] = {"当前角色", "全服", "在线玩家"};
static const char *kMailAttach[] = {"普通物品", "时装(avata)", "宠物(creature)"};

// ---- 异步动作 ----
enum { A_LIST = 1, A_DEL, A_DELALL, A_SENDT, A_SENDI, A_DELALLS };
typedef struct {
    int  kind;
    u64  postal;
    u32  iid;
    int  inum;
    int  target;     // 0=当前角色 1=全服 2=在线玩家
    int  atype;      // 0=普通 1=时装 2=宠物
    int  avata;      // 派生：atype==1
    int  creature;   // 派生：atype==2
    char text[512];
} MailCtx;
static MailCtx mc;

static int mail_work(void *ctx, Err err) {
    MailCtx *c = (MailCtx *)ctx;
    if (c->kind == A_LIST) g.lists_ready |= L_MAILS;
    switch (c->kind) {
        case A_LIST: return db_list_mails(&g.db, &g.cur, &g.mails, err);
        case A_DEL: return db_delete_mail(&g.db, c->postal, err);
        case A_DELALL: return db_delete_all_mails(&g.db, &g.cur, err);
        case A_SENDT: return db_send_text_mail(&g.db, &g.cur, "GM", c->text, err);
        case A_SENDI: {
            if (c->target == 0) {
                GrantItem gi;
                memset(&gi, 0, sizeof(gi));
                gi.id = c->iid;
                gi.num = (u32)(c->inum > 0 ? c->inum : 1);
                gi.type = IT_STACK;
                return db_send_item_mail_ex(&g.db, &g.cur, "GM", c->text, &gi, c->avata, c->creature, err);
            }
            // 全服 / 在线：逐角色批量发
            u32 sent = 0;
            int ok = db_send_mail_bulk(&g.db, c->target == 2, "GM", c->text, c->iid,
                                       (u32)(c->inum > 0 ? c->inum : 1), c->avata, c->creature, &sent, err);
            if (ok) app_log("批量发信：成功发往 %u 个角色", sent);
            else app_log("批量发信中断：已发送 %u 个后失败", sent);
            return ok;
        }
        case A_DELALLS: return db_delete_all_mails_global(&g.db, err);
    }
    return 0;
}

static void mail_refresh_list(void) {
    ui_rows_clear(m_list);
    ui_batch(m_list, 1);
    for (int i = 0; i < g.mails.n; i++) {
        MailRow *r = VEC_AT(&g.mails, MailRow, i);
        char pid[24], iid[16], nm[96], num[16], gold[32];
        fmt(pid, sizeof(pid), "%llu", (unsigned long long)r->postal_id);
        fmt(iid, sizeof(iid), "%u", r->item_id);
        app_money_text(r->gold, gold, sizeof(gold));
        fmt(num, sizeof(num), "%u", r->add_info);
        const char *kind = r->avata ? "时装" : (r->creature ? "宠物" : "物品");
        const char *cells[6] = {pid, r->item_id ? app_item_name(r->item_id, nm, sizeof(nm)) : "—", iid,
                                num, r->item_id ? kind : "纯文本", r->time};
        ui_row_add(m_list, cells, 6, (LPARAM)i);
        (void)gold;
    }
    ui_batch(m_list, 0);
    ui_text(m_info, g.char_ok ? fmt_new("%s · 共 %d 封邮件", g.cur.name, g.mails.n) : "请先选择角色");
}

static void mail_reload(Page *p) {
    (void)p;
    if (!g.char_ok) { ui_text(m_info, "请先选择角色"); return; }
    if (!(g.lists_ready & L_MAILS)) {
        if (!job_running(&g.job_act)) {
            mc.kind = A_LIST;
            app_act("读取邮件", mail_work, &mc);
        }
        ui_text(m_info, "正在读取邮件…");
        return;
    }
    mail_refresh_list();
}

static LRESULT mail_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    (void)lp;
    if (m != WM_COMMAND) return -1;
    if (LOWORD(wp) == M_REFRESH) {
        g.mails.n = 0;
        g.lists_ready &= ~(u32)L_MAILS;
        mail_reload(p);
        return 0;
    }
    switch (LOWORD(wp)) {
        case M_DEL: {
            int row = ui_row_sel(m_list);
            if (row < 0 || row >= g.mails.n) return 0;
            MailRow *r = VEC_AT(&g.mails, MailRow, row);
            if (!app_confirm(fmt_new("删除邮件 %llu？", (unsigned long long)r->postal_id))) return 0;
            mc.kind = A_DEL;
            mc.postal = r->postal_id;
            g.mails.n = 0;
            g.lists_ready &= ~(u32)L_MAILS;
            app_act("删除邮件", mail_work, &mc);
            return 0;
        }
        case M_DELALL:
            if (!app_confirm("清空该角色的全部邮件？")) return 0;
            mc.kind = A_DELALL;
            g.mails.n = 0;
            g.lists_ready &= ~(u32)L_MAILS;
            app_act("清空邮件", mail_work, &mc);
            return 0;
        case M_DELALLS:
            if (!app_confirm("清空【全服】所有角色的邮件？\n\n此操作不可恢复，建议先整库备份。")) return 0;
            mc.kind = A_DELALLS;
            app_act("删除全服邮件", mail_work, &mc);
            return 0;
        case M_SENDT: {
            char t[512];
            ui_get(m_text, t, sizeof(t));
            str_trim(t);
            if (!t[0]) { app_info("请输入正文。"); return 0; }
            snprintf(mc.text, sizeof(mc.text), "%s", t);
            mc.kind = A_SENDT;
            app_act("发送邮件", mail_work, &mc);
            return 0;
        }
        case M_SENDI: {
            int target = ui_combo_sel(m_target);
            if (target < 0) target = 0;
            int atype = ui_combo_sel(m_atype);
            if (atype < 0) atype = 0;
            u32 id = (u32)ui_int(m_iid);
            if (target == 0 && !id) { app_info("请先输入物品编号。"); return 0; }
            mc.kind = A_SENDI;
            mc.target = target;
            mc.atype = atype;
            mc.avata = (atype == 1);
            mc.creature = (atype == 2);
            mc.iid = id;
            mc.inum = ui_int(m_inum) > 0 ? ui_int(m_inum) : 1;
            ui_get(m_imsg, mc.text, sizeof(mc.text));
            app_act(target == 0 ? "发送附件邮件" : "批量发信", mail_work, &mc);
            return 0;
        }
    }
    return -1;
}

static void mail_layout(Page *p, int w, int h) {
    (void)w;
    (void)h;
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int pad = ui_scale(6), rowh = ui_scale(23), g = ui_scale(5);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, M_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, M_DEL, "删除所选"), ui_scale(76));
    ui_add(&r, ui_btn_at(p->wnd, M_DELALL, "全部删除"), ui_scale(76));
    ui_add(&r, m_info, -1);

    int bottom = rc.bottom - pad;
    int sendh = rowh * 4 + g * 3;
    int ltop = pad + rowh + g;
    int lh = bottom - sendh - ui_scale(6) - ltop;
    if (lh < ui_scale(60)) lh = ui_scale(60);
    MoveWindow(m_list, pad, ltop, rc.right - pad * 2, lh, TRUE);

    // 发送区：正文行 / 附件行 / 目标+类型行 / 全服删除行
    int y = bottom - sendh;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "正文"), ui_scale(34));
    ui_add(&r, m_text, ui_scale(300));
    ui_add(&r, ui_btn_at(p->wnd, M_SENDT, "发送文本邮件"), ui_scale(104));
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "附件编号"), ui_scale(58));
    ui_add(&r, m_iid, ui_scale(84));
    ui_add(&r, ui_lbl_at(p->wnd, "数量"), ui_scale(34));
    ui_add(&r, m_inum, ui_scale(50));
    ui_add(&r, ui_lbl_at(p->wnd, "说明"), ui_scale(34));
    ui_add(&r, m_imsg, ui_scale(150));
    ui_add(&r, ui_btn_at(p->wnd, M_SENDI, "发送附件邮件"), ui_scale(104));
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "目标"), ui_scale(34));
    ui_add(&r, m_target, ui_scale(96));
    ui_add(&r, ui_lbl_at(p->wnd, "附件类型"), ui_scale(58));
    ui_add(&r, m_atype, ui_scale(120));
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_btn_at(p->wnd, M_DELALLS, "删除全服邮件"), ui_scale(120));
}

static void mail_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[7] = {M_DEL, M_DELALL, M_SENDT, M_SENDI, M_IID, M_INUM, M_IMSG};
    for (int i = 0; i < 7; i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
    // 目标 / 附件类型 / 全服删除：连库即可用（不依赖选中角色）
    ui_enable(m_target, g.db_ok);
    ui_enable(m_atype, g.db_ok);
    ui_enable(GetDlgItem(p->wnd, M_DELALLS), g.db_ok);
}

static void mail_build(Page *p, HWND parent) {
    m_list = ui_list(parent, M_LIST);
    m_info = ui_label(parent, "请先选择角色", M_INFO);
    m_text = ui_edit(parent, "", M_TEXT, 0);
    m_iid = ui_edit(parent, "", M_IID, 0);
    m_inum = ui_edit(parent, "1", M_INUM, 0);
    m_imsg = ui_edit(parent, "", M_IMSG, 0);
    m_target = ui_combo(parent, M_TARGET, 1);
    ui_combo_fill(m_target, kMailTarget, COUNT_OF(kMailTarget), 0);
    m_atype = ui_combo(parent, M_ATYPE, 1);
    ui_combo_fill(m_atype, kMailAttach, COUNT_OF(kMailAttach), 0);
    ui_cols(m_list, (const char *const[]){"邮件ID", "物品", "编号", "数量", "类型", "时间"},
            (const int[]){110, 200, 70, 56, 60, 130}, 6);
    ui_button(parent, "刷新", M_REFRESH);
    ui_button(parent, "删除所选", M_DEL);
    ui_button(parent, "全部删除", M_DELALL);
    ui_button(parent, "发送文本邮件", M_SENDT);
    ui_button(parent, "发送附件邮件", M_SENDI);
    ui_button(parent, "删除全服邮件", M_DELALLS);
    ui_tip(parent, m_target, "当前角色：只发给左侧选中的角色；全服：所有未删角色；在线玩家：仅 login_status>=1 的账号");
    ui_tip(parent, m_atype, "普通物品：作为可叠加道具入邮件；时装：先在 user_items 建行再以 avata_flag 发；宠物：先在 creature_items 建行再以 creature_flag 发");
    ui_tip(parent, m_iid, "物品编号（PVF 里的 ID）。全服/在线模式允许留空=只发文本信");
    ui_tip(parent, GetDlgItem(parent, M_DELALLS), "把 postal 表所有行 delete_flag 置 1（软删）。不可恢复，建议先整库备份");
}

void page_mail_create(Page *p) {
    p->build = mail_build;
    p->layout = mail_layout;
    p->reload = mail_reload;
    p->lock = mail_lock;
    p->on = mail_on;
}
