// ui_mail：邮件列表、删除、发送文本邮件 / 附件邮件。
#include "ui.h"

enum {
    M_LIST = PAGE_ID(PAGE_MAIL, 1), M_DEL, M_DELALL, M_INFO, M_TEXT, M_SENDT, M_IID, M_INUM, M_IMSG,
    M_SENDI, M_REFRESH = PAGE_ID(PAGE_MAIL, 20)
};

static HWND m_list, m_info, m_text, m_iid, m_inum, m_imsg;

// ---- 异步动作 ----
enum { A_LIST = 1, A_DEL, A_DELALL, A_SENDT, A_SENDI };
typedef struct {
    int  kind;
    u64  postal;
    u32  iid;
    int  inum;
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
        case A_SENDI:
            return db_send_item_mail(&g.db, &g.cur, "GM", c->text, c->iid, (u32)(c->inum > 0 ? c->inum : 1), err);
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
            u32 id = (u32)ui_int(m_iid);
            if (!id) { app_info("请输入物品编号。"); return 0; }
            mc.kind = A_SENDI;
            mc.iid = id;
            mc.inum = ui_int(m_inum) > 0 ? ui_int(m_inum) : 1;
            ui_get(m_imsg, mc.text, sizeof(mc.text));
            app_act("发送附件邮件", mail_work, &mc);
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
    int sendh = rowh * 2 + g;
    int ltop = pad + rowh + g;
    int lh = bottom - sendh - ui_scale(6) - ltop;
    if (lh < ui_scale(60)) lh = ui_scale(60);
    MoveWindow(m_list, pad, ltop, rc.right - pad * 2, lh, TRUE);

    // 发送区
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
    ui_add(&r, m_imsg, ui_scale(160));
    ui_add(&r, ui_btn_at(p->wnd, M_SENDI, "发送附件邮件"), ui_scale(104));
}

static void mail_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[7] = {M_DEL, M_DELALL, M_SENDT, M_SENDI, M_IID, M_INUM, M_IMSG};
    for (int i = 0; i < 7; i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static void mail_build(Page *p, HWND parent) {
    m_list = ui_list(parent, M_LIST);
    m_info = ui_label(parent, "请先选择角色", M_INFO);
    m_text = ui_edit(parent, "", M_TEXT, 0);
    m_iid = ui_edit(parent, "", M_IID, 0);
    m_inum = ui_edit(parent, "1", M_INUM, 0);
    m_imsg = ui_edit(parent, "", M_IMSG, 0);
    ui_cols(m_list, (const char *const[]){"邮件ID", "物品", "编号", "数量", "类型", "时间"},
            (const int[]){110, 200, 70, 56, 60, 130}, 6);
    ui_button(parent, "刷新", M_REFRESH);
    ui_button(parent, "删除所选", M_DEL);
    ui_button(parent, "全部删除", M_DELALL);
    ui_button(parent, "发送文本邮件", M_SENDT);
    ui_button(parent, "发送附件邮件", M_SENDI);
}

void page_mail_create(Page *p) {
    p->build = mail_build;
    p->layout = mail_layout;
    p->reload = mail_reload;
    p->lock = mail_lock;
    p->on = mail_on;
}
