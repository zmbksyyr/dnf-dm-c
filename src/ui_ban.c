// ui_ban：封停页。封停/解封/重置密码都走 act.c 的命令表（K_BAN/K_UNBAN/K_RESETPWD）。
// 支持账号封禁（punish_type=1）与角色封禁（punish_type=2，punish_value=charac_no）。
//
// 列表 = 角色清单（「加载全服 / 加载在线」决定范围），右侧几列挂该角色生效中的封停信息，
// 没被封的留空。点一行就把这个人填进下方的「角色」，处置完列表重拉一次，封停状态跟着变。
#include "ui.h"
#include "act.h"

// ================================================================ 封停页
enum { B_LIST = PAGE_ID(PAGE_BAN, 1), B_CHAR, B_BANTYPE, B_DAYS, B_REASON, B_BAN, B_UNBAN,
       B_REFRESH, B_INFO, B_PWD, B_RESETPWD, B_Q1, B_Q3, B_Q7, B_Q30,
       B_LOADALL, B_LOADON };

static HWND b_list, b_char, b_bantype, b_days, b_reason, b_info, b_pwd;

// 角色下拉选择器：数据源 g.ban_chars（Vec<Charac>）
// 用平行数组存 charac_no / mid，CB_SETITEMDATA 存索引
static u32 s_char_no[5000], s_char_mid[5000];
static int s_char_n;

static int s_online_only;    // 0=列表是「全服」 1=只显示在线（决定重新加载时的范围）
static int s_filling;        // 重填列表期间屏蔽 LVN_ITEMCHANGED
static u32 s_keep_cno;       // 处置完重填后要找回的选中角色（0=不找）

static void ban_fill_chars(void) {
    SendMessageW(b_char, CB_RESETCONTENT, 0, 0);
    s_char_n = 0;
    for (int i = 0; i < g.ban_chars.n && s_char_n < 5000; i++) {
        Charac *c = VEC_AT(&g.ban_chars, Charac, i);
        // label 是 UTF-8，CB_ADDSTRING 走的是 W 版接口，必须先转 UTF-16，
        // 否则那串字节会被当成 UTF-16 读，下拉里就是一堆乱码。
        char label[128];
        WCHAR w[160];
        fmt(label, sizeof(label), "%s (No.%u) · %s", c->name, c->no, c->account);
        utf16(label, w, COUNT_OF(w));
        SendMessageW(b_char, CB_ADDSTRING, 0, (LPARAM)w);
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

// 该角色身上生效的封停记录。账号封禁优先：它封的是整个账号，比只封这个角色影响大，
// 两个都有的时候显示账号那条才符合实际（这个人反正登不上去）。
static BanRow *ban_of(const Charac *c) {
    BanRow *by_char = NULL;
    for (int k = 0; k < g.bans.n; k++) {
        BanRow *b = VEC_AT(&g.bans, BanRow, k);
        if (b->type == 1 && b->mid == c->mid) return b;
        if (b->type == 2 && b->charac_no == c->no) by_char = b;
    }
    return by_char;
}

static void ban_fill(void) {
    s_filling = 1;
    ui_rows_clear(b_list);
    ui_batch(b_list, 1);
    int nban = 0;
    for (int i = 0; i < g.ban_chars.n; i++) {
        Charac *c = VEC_AT(&g.ban_chars, Charac, i);
        BanRow *b = ban_of(c);
        char mid[16], target[64];
        fmt(mid, sizeof(mid), "%u", c->mid);
        fmt(target, sizeof(target), "%s (No.%u)", c->name, c->no);
        // 没被封的角色：类型/开始/结束/原因四列留空
        const char *cells[7] = {c->account, mid,
                                b ? (b->type == 2 ? "角色" : "账号") : "",
                                target,
                                b ? b->start : "", b ? b->end : "", b ? b->reason : ""};
        if (b) nban++;
        ui_row_add(b_list, cells, 7, (LPARAM)i);
    }
    ui_batch(b_list, 0);
    ui_text(b_info, fmt_new("%s · 共 %d 个角色，%d 个在封禁中",
                            s_online_only ? "在线" : "全服", g.ban_chars.n, nban));
    // 处置完重填时把选中行找回来：按 charac_no 定位，不能按行号——
    // 「加载在线」得到的清单会随上下线变化，行号对不上人。
    if (s_keep_cno) {
        for (int i = 0; i < g.ban_chars.n; i++) {
            if (VEC_AT(&g.ban_chars, Charac, i)->no != s_keep_cno) continue;
            ui_row_sel_set(b_list, i);
            if (i < s_char_n) SendMessageW(b_char, CB_SETCURSEL, (WPARAM)i, 0);
            break;
        }
        s_keep_cno = 0;
    }
    s_filling = 0;
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
                kc.i1 = s_online_only;      // 0=全服 1=只在线
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

// 「加载全服 / 加载在线」：换的是列表范围。封停状态是拿封停记录叠在角色行上的，
// 所以两张数据一起重拉，否则新范围里会出现状态缺一块。
static LRESULT ban_load(Page *p, int online_only) {
    if (!g.db_ok) { app_info("请先连接数据库。"); return 0; }
    if (job_running(&g.job_act)) { app_info("上一个操作还在进行，等它完成再加载。"); return 0; }
    s_online_only = online_only;
    s_keep_cno = 0;
    g.bans.n = 0;
    g.ban_chars.n = 0;
    s_char_n = 0;
    g.lists_ready &= ~(u32)(L_BANS | L_BANCHARS);
    ban_reload(p);
    return 0;
}

static LRESULT ban_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        // 点行 → 下方「角色」自动切到这个人，天数/类型/原因不动，接着点执行就行。
        // 只改下拉选中，不碰列表，所以不存在重入问题；重填期间要屏蔽，不然
        // ban_fill 自己发出的通知会把选中项来回拨。
        if (nh->idFrom == B_LIST && nh->code == LVN_ITEMCHANGED && !s_filling) {
            int row = ui_row_sel(b_list);
            if (row >= 0 && row < s_char_n) SendMessageW(b_char, CB_SETCURSEL, (WPARAM)row, 0);
        }
        return 0;
    }
    if (m != WM_COMMAND) return -1;
    switch (LOWORD(wp)) {
        case B_REFRESH:
            s_keep_cno = 0;
            g.bans.n = 0;
            g.ban_chars.n = 0;
            s_char_n = 0;
            g.lists_ready &= ~(u32)(L_BANS | L_BANCHARS);
            ban_reload(p);
            return 0;
        case B_LOADALL: return ban_load(p, 0);
        case B_LOADON:  return ban_load(p, 1);
        case B_Q1:  SetWindowTextA(b_days, "1");  return 0;
        case B_Q3:  SetWindowTextA(b_days, "3");  return 0;
        case B_Q7:  SetWindowTextA(b_days, "7");  return 0;
        case B_Q30: SetWindowTextA(b_days, "30"); return 0;
        case B_BAN: {
            int idx = ban_sel_idx();
            char reason[160];
            ui_get(b_reason, reason, sizeof(reason));
            str_trim(reason);
            if (idx < 0) { app_info("请先在列表中选一个角色。"); return 0; }
            u32 mid = s_char_mid[idx];
            u32 charac_no = s_char_no[idx];
            // 封禁类型：0=账号封禁 1=角色封禁 2=永久封禁
            int sel = (int)SendMessageW(b_bantype, CB_GETCURSEL, 0, 0);
            int is_char_ban = (sel == 1);
            int is_perm     = (sel == 2);
            int days = is_perm ? 0 : ui_int(b_days);
            if (!is_perm && days < 1) days = 1;   // 非永久至少 1 天，避免误封永久
            const char *dur = is_perm ? "永久" : fmt_new("%d 天", days);

            if (is_char_ban) {
                if (!app_confirm(fmt_new("角色封禁：封停角色 No.%u（所属账号 mid=%u）%s？",
                                         charac_no, mid, dur)))
                    return 0;
                kc.kind = K_BAN;
                kc.mid = mid;
                kc.i1 = days;
                kc.i2 = 2;           // punish_type=2 角色封禁
                kc.i3 = (int)charac_no;  // punish_value=charac_no
            } else {
                if (!app_confirm(fmt_new("%s：封停账号 mid=%u %s？",
                                         is_perm ? "永久账号封禁" : "账号封禁", mid, dur)))
                    return 0;
                kc.kind = K_BAN;
                kc.mid = mid;
                kc.i1 = days;
                kc.i2 = 1;           // punish_type=1 账号封禁
                kc.i3 = 1;
            }
            snprintf(kc.s, sizeof(kc.s), "%s", reason[0] ? reason : "违规");
            s_keep_cno = charac_no;     // 重填后回到这个人身上，方便接着看状态
            g.bans.n = 0;
            g.lists_ready &= ~(u32)L_BANS;
            app_act("封停", char_work, &kc);
            return 0;
        }
        case B_UNBAN: {
            // 列表是角色清单，解封就按「这个人身上的封禁」找记录，不再要求用户先挑一条记录
            int idx = ban_sel_idx();
            if (idx < 0) { app_info("请先在列表里选一行，或用下方「角色」下拉挑一个。"); return 0; }
            Charac *who = VEC_AT(&g.ban_chars, Charac, idx);
            BanRow *b = ban_of(who);
            if (!b) {
                app_info(fmt_new("「%s」名下没有生效的封停记录。", who->name));
                return 0;
            }
            const char *type_name = b->type == 2 ? "角色" : "账号";
            if (!app_confirm(fmt_new("解除 %s mid=%u（%s）的封停？", type_name, b->mid, b->account)))
                return 0;
            kc.kind = K_UNBAN;
            kc.mid = b->mid;
            kc.i2 = b->type;   // 按惩罚类型精确解封
            s_keep_cno = who->no;
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
    ui_add(&r, ui_btn_at(p->wnd, B_LOADALL, "加载全服"), ui_scale(78));
    ui_add(&r, ui_btn_at(p->wnd, B_LOADON, "加载在线"), ui_scale(78));
    ui_add(&r, ui_btn_at(p->wnd, B_UNBAN, "解除封停"), ui_scale(80));
    ui_add(&r, b_info, -1);

    int y = pad + rowh + g;
    // 列表 + 底部操作区（4 行）
    int btm = rc.bottom - pad - rowh * 4 - g * 4;
    MoveWindow(b_list, pad, y, rc.right - pad * 2, btm - y, TRUE);

    // 第一行：角色选择 + 封禁类型 + 天数 + 原因
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

    // 第三行：快捷时长
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "快捷"), ui_scale(40));
    ui_add(&r, ui_btn_at(p->wnd, B_Q1, "1天"), ui_scale(48));
    ui_add(&r, ui_btn_at(p->wnd, B_Q3, "3天"), ui_scale(48));
    ui_add(&r, ui_btn_at(p->wnd, B_Q7, "7天"), ui_scale(48));
    ui_add(&r, ui_btn_at(p->wnd, B_Q30, "30天"), ui_scale(56));
    ui_add(&r, ui_lbl_at(p->wnd, "（选「永久封禁」忽略天数，封到 2099）"), -1);

    // 第四行：提示
    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd,
        "封禁类型：账号 = 封停整个账号登录；角色 = 只禁该角色，账号仍可登录其它角色；永久 = 封到 2099"), -1);
}

static void ban_lock(Page *p) {
    (void)p;
    int on = g.db_ok;
    int ids[] = {B_BAN, B_UNBAN, B_REFRESH, B_LOADALL, B_LOADON, B_CHAR, B_BANTYPE, B_DAYS,
                 B_REASON, B_PWD, B_RESETPWD, B_Q1, B_Q3, B_Q7, B_Q30};
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
    SendMessageW(b_bantype, CB_ADDSTRING, 0, (LPARAM)L"永久封禁");
    SendMessageW(b_bantype, CB_SETCURSEL, 0, 0);
    // 列表主体是角色（「加载全服 / 加载在线」决定范围），右侧几列是该角色生效中的封停
    ui_cols(b_list, (const char *const[]){"账号", "mid", "封禁", "角色", "开始", "结束", "原因"},
            (const int[]){110, 56, 48, 160, 125, 125, 160}, 7);
    ui_button(parent, "刷新", B_REFRESH);
    ui_button(parent, "加载全服", B_LOADALL);
    ui_button(parent, "加载在线", B_LOADON);
    ui_button(parent, "解除封停", B_UNBAN);
    ui_button(parent, "执行封停", B_BAN);
    ui_button(parent, "重置密码", B_RESETPWD);
    ui_button(parent, "1天", B_Q1);
    ui_button(parent, "3天", B_Q3);
    ui_button(parent, "7天", B_Q7);
    ui_button(parent, "30天", B_Q30);
    ui_tip(parent, GetDlgItem(parent, B_REFRESH), "按当前范围（全服 / 在线）重新读取角色和封停状态");
    ui_tip(parent, GetDlgItem(parent, B_LOADALL),
           "把全服所有角色加载到上方列表，右侧附带各人当前的封停状态；点一行即填到下方");
    ui_tip(parent, GetDlgItem(parent, B_LOADON),
           "只加载当前在线的角色（账号在线且正在玩这个角色）。"
           "现场处置时用这个，不用在一堆离线角色里翻");
    ui_tip(parent, b_char, "要处置的角色；也可以直接点上方列表的某一行，会自动切到这里");
    ui_tip(parent, b_bantype, "账号封禁 = 封停整个账号登录；角色封禁 = 只禁该角色，账号仍可登录其它角色；永久封禁 = 封到 2099");
    ui_tip(parent, GetDlgItem(parent, B_BAN), "按选中的角色和封禁类型写入一条封停记录");
    ui_tip(parent, GetDlgItem(parent, B_UNBAN),
           "解除选中角色身上生效的封停（账号封禁优先，其次角色封禁）；"
           "没被封的角色点这个会提示没有记录");
    ui_tip(parent, GetDlgItem(parent, B_RESETPWD), "把选中角色所属账号的登录密码改成「新密码」里的值");
    ui_tip(parent, GetDlgItem(parent, B_Q1), "设时长为 1 天");
    ui_tip(parent, GetDlgItem(parent, B_Q3), "设时长为 3 天");
    ui_tip(parent, GetDlgItem(parent, B_Q7), "设时长为 7 天");
    ui_tip(parent, GetDlgItem(parent, B_Q30), "设时长为 30 天");
}

void page_ban_create(Page *p) {
    p->build = ban_build;
    p->layout = ban_layout;
    p->reload = ban_reload;
    p->lock = ban_lock;
    p->on = ban_on;
}
