// ui_bubble：泡点页。
// 泡点是一整套独立配置（目标人群、四种奖励、生效时长），跟选中角色无关，所以单独一页。
//
// 语义（几处容易踩的点）：
// - 只在工具开着且连库时结算，关掉不补算；
// - 生效时长是「从点应用那一刻起算的小时数」，不是每天几点到几点；
// - 「全服」目标不支持发物品（离线角色没有角色上下文，发不了邮件）。
#include "ui.h"
#include "act.h"

enum {
    BUB_BOX = PAGE_ID(PAGE_BUBBLE, 1), BUB_BOX_RW, BUB_BOX_NOTE,
    BUB_ON, BUB_SEC, BUB_TARGET, BUB_TNO, BUB_HOURS,
    BUB_COIN, BUB_DP, BUB_DC, BUB_ITEM, BUB_NUM,
    BUB_APPLY, BUB_EXP, BUB_INFO,
    // 说明每行一个控件，单独起一段号。千万别再把「行数」写成 BUB_NOTE_N 这种
    // enum 尾项 —— 它的值是控件 ID（1900+），当数量用会让循环跑上千次，
    // 数组也按 ID 大小开（踩过：说明文字全挤在框顶，因为 block 被算成了 3 万像素）。
    BUB_NOTE1 = PAGE_ID(PAGE_BUBBLE, 30),
    BUB_NOTE2, BUB_NOTE3, BUB_NOTE4, BUB_NOTE5, BUB_NOTE6
};
#define BUB_NOTE_N 6

static HWND b_on, b_sec, b_target, b_tno, b_hours;
static HWND b_coin, b_dp, b_dc, b_item, b_num;
static HWND b_info, b_box, b_box_rw, b_box_note, b_note[BUB_NOTE_N];

static char s_info_last[192];   // 状态行每 tick 都被问一次，文字没变就不写（写一次就是一次重绘）

// 生效时长下拉：0 = 持续（不自动停），其余是小时数
static const char *kBubDurLbl[] = {"持续（不自动停）", "1 小时", "3 小时", "5 小时", "7 小时"};
static const int   kBubDurVal[] = {0, 1, 3, 5, 7};

// 状态行：倒计时 / 上次结算结果 / 剩余时长都在这行里
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
// 一行 = 左列（标签 + 控件）与可选的右列；标签在控件上方而不是左边：
// 标签在左时每行宽窄不一，控件左边缘对不齐。和 GM 页同一套摆法。
typedef struct {
    const char *lab;
    HWND        ctl;
    const char *lab2;
    HWND        ctl2;
} BubRow;

static void bub_field_rows(HWND pw, int bx, int by, int bw, int bh, const BubRow *r, int n) {
    const int inx = ui_scale(12), inb = ui_scale(10);
    const int lbh = ui_scale(15), lgap = ui_scale(4), rh = ui_scale(26);
    const int gapx = ui_scale(12);
    const int nat = lbh + lgap + rh;
    int inner = bw - inx * 2;
    int half = (inner - gapx) / 2;
    int avail = bh - inb;
    if (avail < 0) avail = 0;
    // 行距固定，不跟着框高拉开：这一页字段本来就少，按可用高度均分会把两行撑到
    // 相隔上百像素，上下各留一大片空，读起来像控件掉了一地。固定行距、整块居中，
    // 框再高也是一整块，不会散开。
    const int gapv = ui_scale(14);
    int used = n * nat + (n - 1) * gapv;
    int ytop = by + (avail > used ? (avail - used) / 2 : 0);

    for (int i = 0; i < n; i++) {
        const BubRow *q = &r[i];
        int ry = ytop + i * (nat + gapv);
        int w1 = q->lab2 ? half : inner;
        ui_place(ui_lbl_at(pw, q->lab), bx + inx, ry, w1, lbh);
        ui_place(q->ctl, bx + inx, ry + lbh + lgap, w1, rh);
        if (q->lab2) {
            int x2 = bx + inx + half + gapx;
            ui_place(ui_lbl_at(pw, q->lab2), x2, ry, half, lbh);
            ui_place(q->ctl2, x2, ry + lbh + lgap, half, rh);
        }
    }
}

static void bub_layout(Page *p, int w, int h) {
    HWND pw = p->wnd;
    const int pad = ui_scale(10), gap = ui_scale(12);
    const int bh = ui_scale(30);        // 按钮高
    const int hdr = ui_scale(26);       // 组框标题占的高度

    // 自下而上：按钮贴底 → 说明框吃掉中间剩余 → 上排两框按内容定高。
    // 上排不铺满：这一页只有 5 个字段，框拉到整页高度时字段在中间缩成一条，
    // 上下各一大片空，看着像加载失败。按内容给高、下面用说明框补满，版面才站得住。
    const int btn_y = h - pad - bh;
    const int top_h = ui_scale(206);       // 右组三行 + 标题 + 组内留白
    const int note_y = pad + top_h + gap;
    int note_h = btn_y - gap - note_y;
    if (note_h < ui_scale(120)) note_h = ui_scale(120);

    const int fullW = w - pad * 2;
    const int cw = (fullW - gap) / 2;
    const int x0 = pad, x1 = pad + cw + gap;

    ui_place(b_box, x0, pad, cw, top_h);
    ui_place(b_box_rw, x1, pad, cw, top_h);
    ui_place(b_box_note, x0, note_y, fullW, note_h);

    // 左组：开关在标题下方独占一行，下面两行两列
    const int onh = ui_scale(26) + ui_scale(16);
    ui_place(b_on, x0 + ui_scale(12), pad + hdr, cw - ui_scale(24), ui_scale(24));
    BubRow base[2] = {
        {"结算间隔（秒，下限 5）", b_sec, "生效时长", b_hours},
        {"目标人群", b_target, "目标 ID", b_tno},
    };
    bub_field_rows(pw, x0, pad + hdr + onh, cw, top_h - hdr - onh, base, COUNT_OF(base));

    // 右组：奖励三行两列
    BubRow rw[3] = {
        {"赠送点券（每角色）", b_coin, "D点（每账号）", b_dp},
        {"D币（每账号）", b_dc, "物品 ID（0=不发）", b_item},
        {"物品数量", b_num, NULL, NULL},
    };
    bub_field_rows(pw, x1, pad + hdr, cw, top_h - hdr, rw, COUNT_OF(rw));

    // 底部：应用 / 导出记录 / 状态行
    const int b1 = ui_scale(88), b2 = ui_scale(100), bg = ui_scale(8);
    ui_place(GetDlgItem(pw, BUB_APPLY), pad, btn_y, b1, bh);
    ui_place(GetDlgItem(pw, BUB_EXP), pad + b1 + bg, btn_y, b2, bh);
    int sx = pad + b1 + bg + b2 + gap;
    ui_place(b_info, sx, btn_y + ui_scale(6), w - pad - sx, ui_scale(20));

    // 说明框：整块文字在框内垂直居中。框高是页面剩余空间，字少时贴顶会留一大片
    // 空在下面，像还有内容没加载出来。
    int nx = x0 + ui_scale(12);
    int nw = fullW - ui_scale(24);
    int nstep = ui_scale(20);
    int block = BUB_NOTE_N * nstep;
    int ny = note_y + hdr + (note_h - hdr - block) / 2;
    if (ny < note_y + hdr) ny = note_y + hdr;
    for (int i = 0; i < BUB_NOTE_N; i++)
        ui_place(b_note[i], nx, ny + i * nstep, nw, ui_scale(16));
}

// ---------------- 构建 ----------------
static void bub_build(Page *p, HWND parent) {
    // 分组框先建，后建的控件才压在框上面。
    // 这两个必须接住返回值：组框是自绘容器，只靠 ID 拿不到位置，忘了赋值它就一直是
    // 0×0，界面上等于没有框（踩过一次）。
    b_box = ui_group(parent, "基础设置", BUB_BOX);
    b_box_rw = ui_group(parent, "奖励设置（每结算一次发放的量）", BUB_BOX_RW);
    b_box_note = ui_group(parent, "说明", BUB_BOX_NOTE);

    b_on = ui_check(parent, "启用泡点", BUB_ON, 0);
    // 建的时候一律留空、默认值统一在末尾设：带文本建 EDIT 会发 EN_CHANGE，
    // 页面处理函数若在构建中途弹模态框，嵌套消息循环会把布局重入一遍。
    b_sec = ui_edit(parent, "", BUB_SEC, 0);
    b_target = ui_combo(parent, BUB_TARGET, 1);
    static const char *kTarget[] = {"仅在线", "全服(含离线)", "指定角色", "指定账号"};
    ui_combo_fill(b_target, kTarget, COUNT_OF(kTarget), 0);
    b_tno = ui_edit(parent, "", BUB_TNO, 0);
    b_hours = ui_combo(parent, BUB_HOURS, 1);
    ui_combo_fill(b_hours, kBubDurLbl, COUNT_OF(kBubDurLbl), 0);
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
        "只在工具开着、且已连库时结算；关掉工具不补算。结算间隔下限 5 秒。",
        "生效时长从点「应用」那一刻起算，到时自动停止结算；选「持续」则一直结算到手动停用。",
        "仅在线 = 登录中的账号；全服 = 含离线账号，但不给离线角色发物品。",
        "目标 ID：选「指定角色」填角色号 charac_no，选「指定账号」填账号 m_id；其它目标不用填。",
        "奖励四项可自由组合，填 0 表示该项不发；赠送点券是角色级，D点 / D币 是账号级。",
        "点「应用」写进 settings.ini 并重新计时；「导出记录」把结算历史存成 txt。",
    };
    for (int i = 0; i < BUB_NOTE_N; i++) b_note[i] = ui_label(parent, kNote[i], BUB_NOTE1 + i);

    ui_tip(parent, b_on, "开启后按间隔给目标人群发放奖励；只在工具开着时结算，关掉不补算");
    ui_tip(parent, b_sec, "结算间隔，单位秒，下限 5");
    ui_tip(parent, b_target, "仅在线：登录中的账号；全服：含离线（不发物品）；指定角色/账号：在下面填 ID");
    ui_tip(parent, b_tno, "目标选「指定角色」填角色号 charac_no；选「指定账号」填账号 m_id。"
                          "选其它目标时这个框不用填，会置灰");
    ui_tip(parent, b_hours, "生效时长，从点「应用」那一刻起算；选「持续」则一直结算到手动停用。"
                            "到时长后自动停止结算，重开工具也不会重新续时");
    ui_tip(parent, b_coin, "每次给每个目标角色加的赠送点券（inventory.coin），0 表示不加");
    ui_tip(parent, b_dp, "每次给每个目标账号加的 D点（cash_cera.cera），0 表示不加");
    ui_tip(parent, b_dc, "每次给每个目标账号加的 D币（cash_cera.cera_point），0 表示不加");
    ui_tip(parent, b_item, "随邮件发放的物品 ID，0 表示不发物品（全服目标不支持发物品）");
    ui_tip(parent, b_num, "物品数量（指定/在线目标最多 200）");
    ui_tip(parent, GetDlgItem(parent, BUB_APPLY), "把以上设置写进 settings.ini，立即生效并重新计时");
    ui_tip(parent, GetDlgItem(parent, BUB_EXP), "把日志里的泡点结算记录导出为 config/export/泡点记录.txt");

    // 初值来自 settings.ini
    if (g.cfg.bubble_on) SendMessageW(b_on, BM_SETCHECK, BST_CHECKED, 0);
    ui_set_int(b_sec, g.cfg.bubble_sec >= 5 ? g.cfg.bubble_sec : 60);
    ui_combo_set(b_target, g.cfg.bubble_target < 0 || g.cfg.bubble_target > 3 ? 0 : g.cfg.bubble_target);
    if (g.cfg.bubble_target >= 2 && g.cfg.bubble_tno) ui_set_int(b_tno, (int)g.cfg.bubble_tno);
    int di = 0;
    for (int i = 0; i < COUNT_OF(kBubDurVal); i++)
        if (kBubDurVal[i] == g.cfg.bubble_hours) { di = i; break; }
    ui_combo_set(b_hours, di);
    ui_set_int(b_coin, g.cfg.bubble_coin);
    ui_set_int(b_dp, g.cfg.bubble_dpoint);
    ui_set_int(b_dc, g.cfg.bubble_dcoin);
    ui_set_int(b_item, (int)g.cfg.bubble_item);
    ui_set_int(b_num, g.cfg.bubble_itemnum > 0 ? g.cfg.bubble_itemnum : 1);
    bub_status_sync();
    (void)p;
}

// 目标 ID 只在「指定角色 / 指定账号」下才有意义，其余情况置灰：
// 灰着比让人填完再被忽略好——填了不生效，最容易被当成"功能坏了"。
static void bub_sync_tno(Page *p) {
    int t = ui_combo_sel(b_target);
    ui_enable(b_tno, g.db_ok && (t == 2 || t == 3));
    (void)p;
}

static void bub_lock(Page *p) {
    int on = g.db_ok;
    int ids[] = {BUB_ON, BUB_SEC, BUB_TARGET, BUB_HOURS, BUB_COIN, BUB_DP, BUB_DC,
                 BUB_ITEM, BUB_NUM, BUB_APPLY, BUB_EXP};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
    bub_sync_tno(p);
}

static LRESULT bub_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m != WM_COMMAND) return -1;
    int id = LOWORD(wp);
    if (id == BUB_TARGET) { bub_sync_tno(p); return 0; }
    if (id == BUB_APPLY) {
        int on = (int)SendMessageW(b_on, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int target = ui_combo_sel(b_target);          // 0仅在线 1全服 2指定角色 3指定账号
        if (target < 0) target = 0;
        u32 tno = (target >= 2) ? (u32)ui_i64(b_tno) : 0;
        if (target >= 2 && !tno) {
            app_info(target == 2 ? "请填写要发到哪个角色（角色号 charac_no）。"
                                 : "请填写要发到哪个账号（m_id）。");
            return 0;
        }
        int di = ui_combo_sel(b_hours);
        if (di < 0 || di >= COUNT_OF(kBubDurVal)) di = 0;
        app_bubble_apply(on, (int)ui_i64(b_sec), target,
                         (int)ui_i64(b_coin), (int)ui_i64(b_dp), (int)ui_i64(b_dc),
                         (u32)ui_i64(b_item), (int)ui_i64(b_num), tno, kBubDurVal[di]);
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
