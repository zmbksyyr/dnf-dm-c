// ui_mail：邮件列表、删除、发送邮件。
// 发送只有一个入口：附件 ID 留空 = 纯文本邮件，填了 = 附件邮件。原先是两个按钮，
// 但两条路径的其它字段完全一样，拆成两个只是让人每次先判断"我这次算哪种"。
// 目标可设为「当前角色 / 全服 / 在线玩家」；另有「删除全服邮件」这类清服维护。
#include "ui.h"

enum {
    M_LIST = PAGE_ID(PAGE_MAIL, 1), M_DEL, M_DELALL, M_DELALLS, M_INFO,
    M_TARGET, M_ATYPE, M_AID, M_ANUM, M_TEXT, M_SEND, M_HINT,
    M_REFRESH = PAGE_ID(PAGE_MAIL, 20)
};

static HWND m_list, m_info, m_text, m_aid, m_anum, m_target, m_atype, m_hint;
static HWND m_sep1, m_sep2;   // 列表上下的分隔线

static const char *kMailTarget[] = {"当前角色", "全服", "在线玩家"};
// 邮件附件落到哪张表，看 avata_flag / creature_flag 这两个开关：
//   普通 → 直接把数量写进 postal.add_info
//   时装 → 先在 user_items 建一行，拿 ui_id 当 add_info（avata_flag=1）
//   宠物 → 先在 creature_items 建一行，拿 ui_id 当 add_info（creature_flag=1）
// 三者的 ID 空间互相独立，选错就是把时装当普通道具发出去，收的人拿到一件废道具。
// 所以这一格不由人填：填了 ID 就由 PVF 判定，判不出来一律按「普通」兜。
enum { AK_ITEM = 0, AK_AVATA = 1, AK_CREATURE = 2 };

// 宠物不落在 user_items，而是自成一表（creature_items）；但它在 PVF 里的编号
// 长得和装备/消耗品一样 —— 63000「佛拉斯」按所在目录算就是件装备，光看 PVF
// 分不出来。说得清的只有库里的 dnf_creature_info：it_id 那一列就是宠物清单，
// 连库时收一次（见 db_mail.c 的 db_creature_ids），这里照它对。清单为空（架服没带
// web 库）时按普通物品发，不发宠物。
// 附件种类判定统一走 app 层（app_attach_kind），发放页走同一个判据，
// 改判据只需改一处。AK_* 只是本地显示用的别名。
static int attach_kind_of(u32 id) {
    switch (app_attach_kind(id)) {
        case ACK_AVATAR:   return AK_AVATA;
        case ACK_CREATURE: return AK_CREATURE;
        default:           return AK_ITEM;
    }
}

static const char *kAttachText[] = {"普通物品", "时装", "宠物"};
static const char *kAttachIdLab[] = {"物品 ID", "时装 ID", "宠物 ID"};

// 附件类型那格跟着附件 ID 走，并把左边那个叫法标签一并改成三种之一。
// 每次都现问 PVF：主键ini 里的 ID 改了、或者 PVF 才刚加载完，旧结果就作废了。
static void mail_type_sync(void) {
    if (!m_aid || !m_atype) return;
    u32 id = ui_int(m_aid) > 0 ? (u32)ui_int(m_aid) : 0;
    int k = attach_kind_of(id);
    ui_text(m_atype, kAttachText[k]);
    ui_text(m_hint, kAttachIdLab[k]);
}

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
            // 全服 / 在线：逐角色批量发（item_id 为 0 时 db_send_mail_bulk 会发纯文本）
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
    // PVF 可能是进页面之后才加载完的，每次刷新都重问一次类型
    mail_type_sync();
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
    if (LOWORD(wp) == M_AID && HIWORD(wp) == EN_CHANGE) {
        // 附件 ID 一变就重新问 PVF：这格是只读的，用户改不了它，只能改 ID。
        // 这里不接 m_atype 的 EN_CHANGE —— 那格正是本函数写的，再接就自己触发自己了。
        mail_type_sync();
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
        case M_SEND: {
            int target = ui_combo_sel(m_target);
            if (target < 0) target = 0;
            char idbuf[24];
            ui_get(m_aid, idbuf, sizeof(idbuf));
            str_trim(idbuf);
            u32 id = idbuf[0] ? (u32)ui_int(m_aid) : 0;
            if (idbuf[0] && !id) { app_info("附件 ID 只能填数字；不想带附件就留空。"); return 0; }
            ui_get(m_text, mc.text, sizeof(mc.text));
            str_trim(mc.text);
            if (!mc.text[0] && !id) { app_info("请填写正文，或填一个附件 ID。"); return 0; }
            mc.target = target;
            // 附件种类按 PVF 对 ID 的分类定，不信界面上那格（那格只是把结论显示出来）。
            // PVF 没加载时 attach_kind_of 一律给「普通」，照样能发出去。
            mc.atype = attach_kind_of(id);
            mc.avata = (mc.atype == AK_AVATA);
            mc.creature = (mc.atype == AK_CREATURE);
            mc.iid = id;
            mc.inum = ui_int(m_anum) > 0 ? ui_int(m_anum) : 1;
            // 附件 ID 留空 = 纯文本邮件；当前角色才有「单发文本」这条路径，
            // 全服/在线两种情况 db_send_mail_bulk 自己会按 item_id=0 发纯文本
            mc.kind = (!id && target == 0) ? A_SENDT : A_SENDI;
            app_act(target == 0 ? (id ? "发送附件邮件" : "发送文本邮件") : "批量发信", mail_work, &mc);
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
    // 顶行：刷新 / 删除所选 / 全部删除 / 删除全服邮件 / 状态
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_btn_at(p->wnd, M_REFRESH, "刷新"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, M_DEL, "删除所选"), ui_scale(76));
    ui_add(&r, ui_btn_at(p->wnd, M_DELALL, "全部删除"), ui_scale(76));
    ui_add(&r, ui_btn_at(p->wnd, M_DELALLS, "删除全服邮件"), ui_scale(120));
    ui_gap(&r, ui_scale(10));       // 状态文字别紧贴按钮，否则像按钮文字的一部分
    ui_add(&r, m_info, -1);

    // 底部两行：① 投递参数 ② 正文 + 发送
    int bottom = rc.bottom - pad;
    int sendh = rowh * 2 + g;
    // 列表上下各断一刀：上面是操作按钮，下面是投递表单，中间这块才是邮件列表
    int sepy1 = pad + rowh + ui_scale(4);
    ui_place(m_sep1, pad, sepy1, rc.right - pad * 2, ui_scale(2));
    int ltop = sepy1 + ui_scale(6);
    int y = bottom - sendh;
    int sepy2 = y - ui_scale(8);
    ui_place(m_sep2, pad, sepy2, rc.right - pad * 2, ui_scale(2));
    int lh = sepy2 - ui_scale(6) - ltop;
    if (lh < ui_scale(60)) lh = ui_scale(60);
    MoveWindow(m_list, pad, ltop, rc.right - pad * 2, lh, TRUE);

    ui_row(&r, p->wnd, pad, y, rowh);
    // 「（ID 留空 = 纯文本邮件）」这句不写在页面上了：附件 ID 框和发送按钮的提示里都有。
    // 末端的提示原来兼作右边界锚点，现在直接对齐到页面右边，正文框按它反算。
    ui_add(&r, ui_lbl_at(p->wnd, "目标"), ui_scale(34));
    ui_add(&r, m_target, ui_scale(96));
    ui_add(&r, ui_lbl_at(p->wnd, "附件类型"), ui_scale(58));
    ui_add(&r, m_atype, ui_scale(96));           // 只读，跟着附件 ID 变
    ui_add(&r, m_hint, ui_scale(58));            // 文字随类型变：物品 ID / 时装 ID / 宠物 ID
    ui_add(&r, m_aid, ui_scale(90));
    ui_add(&r, ui_lbl_at(p->wnd, "数量"), ui_scale(34));
    ui_add(&r, m_anum, ui_scale(50));

    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "正文"), ui_scale(34));
    // 正文框定宽：原来它一直算到窗口右边，一千多像素宽，写两句话的地方
    // 却横穿整个窗口，把「发送邮件」推到很远。现在只占一段，按钮紧跟其后。
    const int bw = ui_scale(88);
    int tw = ui_scale(460);
    if (tw > rc.right - pad - r.x - r.gap - bw) tw = rc.right - pad - r.x - r.gap - bw;
    if (tw < ui_scale(120)) tw = ui_scale(120);
    ui_add(&r, m_text, tw);
    ui_add(&r, ui_btn_at(p->wnd, M_SEND, "发送邮件"), bw);
}

static void mail_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[5] = {M_DEL, M_DELALL, M_AID, M_ANUM, M_TEXT};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
    ui_enable(GetDlgItem(p->wnd, M_SEND), on || g.db_ok);
    // 目标 / 全服删除：连库即可用（不依赖选中角色）。附件类型是只读的跟随项，
    // 随 ID 自己变，不参与这里的灰亮切换。
    ui_enable(m_target, g.db_ok);
    ui_enable(GetDlgItem(p->wnd, M_DELALLS), g.db_ok);
}

static void mail_build(Page *p, HWND parent) {
    m_list = ui_list(parent, M_LIST);
    m_info = ui_label(parent, "请先选择角色", M_INFO);
    m_sep1 = ui_hline(parent);
    m_sep2 = ui_hline(parent);
    m_text = ui_edit(parent, "", M_TEXT, 0);
    // 正文默认给一个 "GM."：绝大多数邮件是 GM 发的，每次手打这两下没必要。
    // 建完再设，不带文本建 EDIT 造成的 EN_CHANGE 会惊动页面处理函数。
    ui_text(m_text, "GM.");
    m_aid = ui_edit(parent, "", M_AID, 0);
    m_anum = ui_edit(parent, "1", M_ANUM, 0);
    // 附件编号与数量：库里是 int unsigned，数量至少 1（填 0 当 1 处理，这里直接不让填）
    ui_num(m_aid, 0, 4294967295LL);
    ui_num(m_anum, 1, 4294967295LL);
    m_hint = ui_label(parent, "物品 ID", M_HINT);
    m_target = ui_combo(parent, M_TARGET, 1);
    ui_combo_fill(m_target, kMailTarget, COUNT_OF(kMailTarget), 0);
    // 类型做成只读的文本框而不是下拉：内容来自 PVF，下拉给人的错觉是"可以选一个"。
    // 只读靠 EM_SETREADONLY（不是 ro=1），这样 Tab 还能走到它，整行的 Tab 顺序不断。
    m_atype = ui_edit(parent, "", M_ATYPE, 0);
    SendMessageW(m_atype, EM_SETREADONLY, TRUE, 0);
    mail_type_sync();            // 建好就写上「普通物品」，不等用户输入 ID
    ui_cols(m_list, (const char *const[]){"邮件ID", "物品", "编号", "数量", "类型", "时间"},
            (const int[]){110, 200, 70, 56, 60, 130}, 6);
    ui_idtip_bind(m_list, 2);
    ui_button(parent, "刷新", M_REFRESH);
    ui_button(parent, "删除所选", M_DEL);
    ui_button(parent, "全部删除", M_DELALL);
    ui_button(parent, "删除全服邮件", M_DELALLS);
    ui_button(parent, "发送邮件", M_SEND);
    ui_tip(parent, m_target, "当前角色：只发给列表上方选中的角色；全服：所有未删角色；在线玩家：仅 login_status>=1 的账号");
    ui_tip(parent, m_atype,
           "附件种类按附件 ID 从 PVF 判定，不可手改：普通物品作为可叠加道具入邮件；"
           "时装先建 user_items 行再以 avata_flag 发；宠物先建 creature_items 行再以 creature_flag 发。"
           "PVF 未加载时按普通物品处理");
    ui_tip(parent, m_aid, "要附带的物品/时装/宠物 ID（PVF 里的编号）。留空 = 只发正文的纯文本邮件");
    ui_tip(parent, m_anum, "附件数量，留空按 1 发");
    ui_tip(parent, m_text, "邮件正文；正文和附件可以只给一个，但不能都没有");
    ui_tip(parent, GetDlgItem(parent, M_SEND), "附件 ID 留空发纯文本邮件，填了就发附件邮件，两条路只差这一个框");
    ui_tip(parent, GetDlgItem(parent, M_DELALLS), "把 postal 表所有行 delete_flag 置 1（软删）。不可恢复，建议先整库备份");
}

void page_mail_create(Page *p) {
    p->build = mail_build;
    p->layout = mail_layout;
    p->reload = mail_reload;
    p->lock = mail_lock;
    p->on = mail_on;
}
