// ui_bubble：泡点页。
// 原来挤在 GM 页右下角那一格里（开关 + 11 个字段 + 按钮 + 状态行压在约 290px 高的
// 框内），字段被压到最窄、状态行只能塞在按钮旁边。泡点是一整套独立配置（目标人群、
// 四种奖励、生效时段），跟角色没有关系，单独一页才摆得开，也让 GM 页松一口气。
//
// 语义（几处容易踩的点）：
// - 只在工具开着且连库时结算，关掉不补算；
// - 生效时段留空 = 全天；开始 > 结束表示跨夜；
// - 「全服」目标不支持发物品（离线角色没有角色上下文，发不了邮件）。
#include "ui.h"
#include "act.h"

enum {
    BUB_BOX = PAGE_ID(PAGE_BUBBLE, 1), BUB_BOX_NOTE,
    BUB_ON, BUB_SEC, BUB_TARGET, BUB_TNO,
    BUB_START, BUB_END,
    BUB_COIN, BUB_DP, BUB_DC, BUB_ITEM, BUB_NUM,
    BUB_APPLY, BUB_EXP, BUB_INFO,
    BUB_NOTE1, BUB_NOTE2, BUB_NOTE3, BUB_NOTE4, BUB_NOTE5, BUB_NOTE6, BUB_NOTE7,
    BUB_NOTE8, BUB_NOTE9, BUB_NOTE10, BUB_NOTE11, BUB_NOTE12, BUB_NOTE13, BUB_NOTE14,
    BUB_NOTE_N
};

static HWND b_on, b_sec, b_target, b_tno, b_start, b_end;
static HWND b_coin, b_dp, b_dc, b_item, b_num;
static HWND b_info;
static HWND b_box, b_box_note;
static HWND b_note[BUB_NOTE_N];

static char s_info_last[192];   // 状态行每 tick 都被问一次，文字没变就不写（写一次就是一次重绘）

// 状态行：倒计时 / 上次结算结果都在这行里
static void bub_status_sync(void) {
    if (!b_info) return;
    char buf[192];
    app_bubble_status(buf, sizeof(buf));
    if (!strcmp(buf, s_info_last)) return;
    snprintf(s_info_last, sizeof(s_info_last), "%s", buf);
    ui_text(b_info, buf);
}

void page_bubble_pump(void) { bub_status_sync(); }

// ---------------- 布局 ----------------
// 一行 = 左列（标签 + 控件）与可选的右列；标签在控件上方而不是左边，和 GM 页一致：
// 标签在左时每行宽窄不一，控件左边缘对不齐。
typedef struct {
    const char *lab;
    HWND        ctl;
    const char *lab2;
    HWND        ctl2;
} BubRow;

static void bub_place_row(HWND pw, int x, int y, int w, const BubRow *r) {
    const int lbh = ui_scale(15), lgap = ui_scale(4), rh = ui_scale(26);
    const int gap = ui_scale(12);
    int half = (w - gap) / 2;
    ui_place(ui_lbl_at(pw, r->lab), x, y, half, lbh);
    ui_place(r->ctl, x, y + lbh + lgap, half, rh);
    if (r->lab2) {
        int bx = x + half + gap;
        ui_place(ui_lbl_at(pw, r->lab2), bx, y, half, lbh);
        ui_place(r->ctl2, bx, y + lbh + lgap, half, rh);
    }
}

static void bub_layout(Page *p, int w, int h) {
    HWND pw = p->wnd;
    // 控件尺寸只跟系统 DPI 走（ui_scale），不跟窗口大小走——窗口本身尺寸是固定的。
    const int pad = ui_scale(10), gap = ui_scale(12);
    const int lbh = ui_scale(15), lgap = ui_scale(4), rh = ui_scale(26);
    const int step = lbh + lgap + rh + ui_scale(14);     // 一行占的高度（含行间距）
    const int inx = ui_scale(14), iny = ui_scale(26), inb = ui_scale(12);

    // 左宽右窄：左边是表单，右边是静态说明（说明只占一行行文字的宽度）
    int lw = (w - pad * 2 - gap) * 62 / 100;
    if (lw < ui_scale(420)) lw = w - pad * 2 - gap - ui_scale(220);
    int rw = w - pad * 2 - gap - lw;

    BubRow r1[] = {
        {"结算间隔（秒，下限 5）", b_sec,  "目标人群", b_target},
        {"目标 ID（角色号 / 账号 m_id）", b_tno, "生效开始 HH:MM", b_start},
        {"生效结束 HH:MM", b_end, NULL, NULL},
        {"赠送点券（每角色）", b_coin, "D点（每账号）", b_dp},
        {"D币（每账号）", b_dc, "物品 ID（0=不发）", b_item},
        {"物品数量", b_num, NULL, NULL},
    };

    // 组框高度按内容定，不铺满整页：这一页只有 6 行字段，铺满会在框内留一大片
    // 空白，看着像「还有东西没加载出来」。窗口高度不够时才退化成铺满。
    const int need = iny + rh + ui_scale(14) + (int)COUNT_OF(r1) * step + ui_scale(2) +
                     ui_scale(30) + inb;
    int boxh = h - pad * 2;
    if (boxh > need) boxh = need;

    ui_place(b_box, pad, pad, lw, boxh);
    ui_place(b_box_note, pad + lw + gap, pad, rw, boxh);

    int x = pad + inx, y = pad + iny;
    int fw = lw - inx * 2;

    // 开关独占一行（它管着下面整组字段有没有意义）
    ui_place(b_on, x, y, fw, rh);
    y += rh + ui_scale(14);

    for (int i = 0; i < COUNT_OF(r1); i++, y += step) bub_place_row(pw, x, y, fw, &r1[i]);

    // 按钮行：状态行跟在按钮右边，占掉剩余宽度（一行字，长了就截断）
    y += ui_scale(2);
    const int bw = ui_scale(80), b2 = ui_scale(96), bg = ui_scale(8);
    ui_place(GetDlgItem(pw, BUB_APPLY), x, y, bw, ui_scale(30));
    ui_place(GetDlgItem(pw, BUB_EXP), x + bw + bg, y, b2, ui_scale(30));
    int sx = x + bw + bg + b2 + gap;
    ui_place(b_info, sx, y + ui_scale(6), pad + lw - inb - sx, ui_scale(20));

    // 右侧说明：逐行摆，不换行（STATIC 是单行居中样式，靠这里一行一句）
    int nx = pad + lw + gap + inx, ny = pad + iny, nw = rw - inx - ui_scale(10);
    int nstep = lbh + ui_scale(9);
    for (int i = 0; i < COUNT_OF(b_note); i++, ny += nstep) ui_place(b_note[i], nx, ny, nw, lbh);

    (void)h;
}

// ---------------- 构建 ----------------
static void bub_build(Page *p, HWND parent) {
    // 分组框先建，后建的控件才压在框上面。
    // 这两个必须接住返回值：组框是自绘容器，只靠 ID 拿不到位置，忘了赋值它就一直是
    // 0×0，界面上等于没有框（踩过一次）。
    b_box = ui_group(parent, "泡点设置（账号级 / 全服，与选中角色无关）", BUB_BOX);
    b_box_note = ui_group(parent, "说明", BUB_BOX_NOTE);

    static const char *kTarget[] = {"仅在线", "全服(含离线)", "指定角色", "指定账号"};
    b_on = ui_check(parent, "启用泡点", BUB_ON, 0);
    b_sec = ui_edit(parent, "", BUB_SEC, 0);
    b_target = ui_combo(parent, BUB_TARGET, 1);
    ui_combo_fill(b_target, kTarget, COUNT_OF(kTarget), 0);
    b_tno = ui_edit(parent, "", BUB_TNO, 0);
    b_start = ui_edit(parent, "", BUB_START, 0);
    b_end = ui_edit(parent, "", BUB_END, 0);
    b_coin = ui_edit(parent, "", BUB_COIN, 0);
    b_dp = ui_edit(parent, "", BUB_DP, 0);
    b_dc = ui_edit(parent, "", BUB_DC, 0);
    b_item = ui_edit(parent, "", BUB_ITEM, 0);
    b_num = ui_edit(parent, "", BUB_NUM, 0);
    ui_button(parent, "应用", BUB_APPLY);
    ui_button(parent, "导出记录", BUB_EXP);
    b_info = ui_label(parent, "", BUB_INFO);

    // 说明文字一行一句：ui_label 是 SS_CENTERIMAGE（单行不折行），长句子会被右边界
    // 直接切掉，所以这里每句都控制在一行放得下的长度内。
    static const char *const kNote[BUB_NOTE_N] = {
        "只在工具开着、且已连库时结算，",
        "关掉工具不补算；间隔下限 5 秒。",
        "",
        "仅在线 = login_status ≥ 1 的账号；",
        "全服 = 含离线账号（不发物品）。",
        "指定角色填 charac_no，账号填 m_id。",
        "",
        "奖励四项可自由组合，0 表示不发；",
        "物品走邮件，赠送点券是角色级、",
        "D点/D币是账号级。",
        "",
        "生效时段留空 = 全天；开始 > 结束",
        "表示跨夜（如 20:00 → 08:00）。",
        "点「应用」写进 settings.ini 并重新计时。",
    };
    for (int i = 0; i < COUNT_OF(b_note); i++) b_note[i] = ui_label(parent, kNote[i], BUB_NOTE1 + i);

    ui_tip(parent, b_on, "开启后按间隔给目标人群发放奖励；只在工具开着时结算，关掉不补算");
    ui_tip(parent, b_sec, "结算间隔，单位秒，下限 5");
    ui_tip(parent, b_target, "仅在线：login_status>=1 的账号；全服：含离线；指定角色/账号：填右边的 ID");
    ui_tip(parent, b_tno, "目标选「指定角色」填角色号 charac_no；选「指定账号」填账号 m_id");
    ui_tip(parent, b_start, "生效开始时间，HH:MM（如 20:00）；留空 = 全天生效");
    ui_tip(parent, b_end, "生效结束时间，HH:MM（如 08:00）；留空 = 全天生效。支持跨夜（开始 > 结束）");
    ui_tip(parent, b_coin, "每次给每个目标角色加的赠送点券（inventory.coin），0 表示不加");
    ui_tip(parent, b_dp, "每次给每个目标账号加的 D点（cash_cera.cera），0 表示不加");
    ui_tip(parent, b_dc, "每次给每个目标账号加的 D币（cash_cera_point.cera_point），0 表示不加");
    ui_tip(parent, b_item, "随邮件发放的物品 ID，0 表示不发物品（全服目标不支持发物品）");
    ui_tip(parent, b_num, "物品数量（指定/在线目标最多 200）");
    ui_tip(parent, GetDlgItem(parent, BUB_APPLY), "把以上设置写进 settings.ini，立即生效并重新计时");
    ui_tip(parent, GetDlgItem(parent, BUB_EXP), "把日志里的泡点结算记录导出为 config/export/泡点记录.txt");

    // 初值来自 settings.ini
    if (g.cfg.bubble_on) SendMessageW(b_on, BM_SETCHECK, BST_CHECKED, 0);
    ui_set_int(b_sec, g.cfg.bubble_sec);
    ui_combo_set(b_target, g.cfg.bubble_target < 0 || g.cfg.bubble_target > 3 ? 0 : g.cfg.bubble_target);
    if (g.cfg.bubble_target >= 2) ui_set_int(b_tno, (int)g.cfg.bubble_tno);
    ui_set_int(b_coin, g.cfg.bubble_coin);
    ui_set_int(b_dp, g.cfg.bubble_dpoint);
    ui_set_int(b_dc, g.cfg.bubble_dcoin);
    ui_set_int(b_item, (int)g.cfg.bubble_item);
    ui_set_int(b_num, g.cfg.bubble_itemnum);
    // start==end 表示全天：这时候两个框都留空，比显示 00:00 更好懂
    if (g.cfg.bubble_start != g.cfg.bubble_end) {
        ui_text(b_start, fmt_new("%02d:%02d", g.cfg.bubble_start / 60, g.cfg.bubble_start % 60));
        ui_text(b_end, fmt_new("%02d:%02d", g.cfg.bubble_end / 60, g.cfg.bubble_end % 60));
    }
    bub_status_sync();
    (void)p;
}

static void bub_lock(Page *p) {
    int on = g.db_ok;
    int ids[] = {BUB_ON, BUB_SEC, BUB_TARGET, BUB_TNO, BUB_START, BUB_END, BUB_COIN, BUB_DP,
                 BUB_DC, BUB_ITEM, BUB_NUM, BUB_APPLY, BUB_EXP};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
}

static LRESULT bub_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    (void)p;
    if (m != WM_COMMAND) return -1;
    int id = LOWORD(wp);
    if (id == BUB_APPLY) {
        int on = (int)SendMessageW(b_on, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int target = ui_combo_sel(b_target);          // 0仅在线 1全服 2指定角色 3指定账号
        if (target < 0) target = 0;
        u32 tno = (u32)ui_i64(b_tno);
        int coin = (int)ui_i64(b_coin);
        int dpoint = (int)ui_i64(b_dp);
        int dcoin = (int)ui_i64(b_dc);
        u32 item = (u32)ui_i64(b_item);
        int inum = (int)ui_i64(b_num);
        // 解析起止时间：HH:MM，留空 = 全天（start==end==0）
        int start_min = 0, end_min = 0;
        char ts[16], te[16];
        ui_get(b_start, ts, sizeof(ts));
        ui_get(b_end, te, sizeof(te));
        str_trim(ts);
        str_trim(te);
        if (ts[0] || te[0]) {
            int h, mm;
            if (ts[0] && sscanf(ts, "%d:%d", &h, &mm) == 2 && h >= 0 && h < 24 && mm >= 0 && mm < 60)
                start_min = h * 60 + mm;
            if (te[0] && sscanf(te, "%d:%d", &h, &mm) == 2 && h >= 0 && h < 24 && mm >= 0 && mm < 60)
                end_min = h * 60 + mm;
            // 只填了一个时间：另一个取同一个值（= 全天，避免出现只生效一半的怪状态）
            if (ts[0] && !te[0]) end_min = start_min;
            if (te[0] && !ts[0]) start_min = end_min;
        }
        app_bubble_apply(on, (int)ui_i64(b_sec), target, coin, dpoint, dcoin,
                         item, inum, tno, start_min, end_min);
        bub_status_sync();
        return 0;
    }
    if (id == BUB_EXP) { app_bubble_export(); return 0; }
    return -1;
}

void page_bubble_create(Page *p) {
    p->build = bub_build;
    p->layout = bub_layout;
    p->reload = NULL;
    p->lock = bub_lock;
    p->on = bub_on;
}
