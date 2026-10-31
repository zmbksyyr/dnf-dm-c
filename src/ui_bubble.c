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
    BUB_BOX = PAGE_ID(PAGE_BUBBLE, 1), BUB_BOX_RW, BUB_BOX_PLAN,
    BUB_ON, BUB_SEC, BUB_TARGET, BUB_TNO, BUB_HOURS,
    BUB_COIN, BUB_DP, BUB_DC, BUB_ITEM, BUB_NUM,
    BUB_APPLY, BUB_EXP, BUB_INFO, BUB_TIP, BUB_PLAN,
};
// 说明块从 6 行压成 1 行，那段独立号段（BUB_NOTE1..6）已经不需要了。
// 留个教训：别把「行数」写成 enum 尾项 —— 它的值是控件 ID（1900+），
// 当数量用会让循环跑上千次，数组也按 ID 大小开（踩过：说明文字全挤在框顶，
// 因为 block 被算成了 3 万像素）。

static HWND b_on, b_sec, b_target, b_tno, b_hours;
static HWND b_coin, b_dp, b_dc, b_item, b_num;
static HWND b_info, b_sep, b_plan, b_box, b_box_rw, b_box_plan;

static char s_info_last[192];   // 状态行每 tick 都被问一次，文字没变就不写（写一次就是一次重绘）
static int  s_plan_ver = -1;    // 已填进列表的流水版本
static int  s_last_target = -2; // 上次看到的目标人群，用来兜住「联动没跟上」

// 生效时长下拉：可编辑，选项只是常用档；填 0 = 持续（不自动停）
static const char *kBubDurLbl[] = {"0", "1", "3", "5", "7"};

// 状态行：倒计时 / 上次结算结果 / 剩余时长都在这行里
static void bub_status_sync(void) {
    if (!b_info) return;
    char buf[192];
    app_bubble_status(buf, sizeof(buf));
    if (!strcmp(buf, s_info_last)) return;
    snprintf(s_info_last, sizeof(s_info_last), "%s", buf);
    ui_text(b_info, buf);
}

// ---------------- 最近计划 ----------------
static void bub_sync_tno(void);   // 定义在构建之后：build 里要用它设一次初始状态
static void bub_fit_cols(void);   // 同上：布局里摆完列表再按实际宽度摊列宽

static void bub_fill_plan(void) {
    if (!b_plan) return;
    ui_rows_clear(b_plan);
    ui_batch(b_plan, 1);
    int n = app_bubble_plan_count();
    for (int i = 0; i < n; i++) {
        const BubblePlan *pl = app_bubble_plan_at(i);
        if (!pl) continue;
        const char *cells[4] = {pl->t, pl->tg, pl->what, pl->st};
        ui_row_add(b_plan, cells, 4, (LPARAM)i);
    }
    ui_batch(b_plan, 0);
    s_plan_ver = app_bubble_plan_ver();
    bub_fit_cols();
}

// 前三列定宽，「内容」吃掉剩下的宽度：否则「状态」列右边空一条白，
// 看着像少了一列。出了竖向滚动条就让出它的宽度，不然最后一列被压在条下面。
static void bub_fit_cols(void) {
    if (!b_plan) return;
    RECT rc;
    if (!GetClientRect(b_plan, &rc)) return;
    int usable = (int)(rc.right - rc.left);
    if (GetWindowLongW(b_plan, GWL_STYLE) & WS_VSCROLL)
        usable -= GetSystemMetrics(SM_CXVSCROLL);
    int cw = usable - ui_scale(78) - ui_scale(90) - ui_scale(96) - ui_scale(2);
    if (cw < ui_scale(160)) cw = ui_scale(160);
    ListView_SetColumnWidth(b_plan, 2, cw);
}

void page_bubble_pump(void) {
    bub_status_sync();
    // 目标 ID 的启用状态跟着目标人群走。正常情况下 WM_COMMAND 里已经同步过，
    // 这里再兜一道：从别处改配置、或者下拉的通知没进来时，也不会留下"选仅在线
    // 却能填目标 ID"这种自相矛盾的界面。
    int t = b_target ? ui_combo_sel(b_target) : -1;
    if (t != s_last_target) {
        s_last_target = t;
        ui_enable(b_tno, g.db_ok && (t == 2 || t == 3));
    }
    if (b_plan && app_bubble_plan_ver() != s_plan_ver) bub_fill_plan();
}

// ---------------- 布局 ----------------
// 一行 = 左列（标签 + 控件 + 可选单位）与可选的右列；标签在控件上方而不是左边：
// 标签在左时每行宽窄不一，控件左边缘对不齐。和 GM 页同一套摆法。
typedef struct {
    const char *lab;
    HWND        ctl;
    const char *unit;     // 控件右侧的单位标注（如「（小时）」），可为 NULL
    const char *lab2;
    HWND        ctl2;
    const char *unit2;
} BubRow;

static void bub_place_row(HWND pw, int bx, int ry, int w1, const char *lab, HWND ctl,
                          const char *unit, int lbh, int lgap, int rh) {
    int uw = unit && unit[0] ? ui_text_w(unit) + ui_scale(6) : 0;
    ui_place(ui_lbl_at(pw, lab), bx, ry, w1 - uw, lbh);
    ui_place(ctl, bx, ry + lbh + lgap, w1 - uw, rh);
    if (uw) ui_place(ui_lbl_at(pw, unit), bx + w1 - uw + ui_scale(4), ry + lbh + lgap, uw, rh);
}

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
        bub_place_row(pw, bx + inx, ry, w1, q->lab, q->ctl, q->unit, lbh, lgap, rh);
        if (q->lab2) {
            int x2 = bx + inx + half + gapx;
            bub_place_row(pw, x2, ry, half, q->lab2, q->ctl2, q->unit2, lbh, lgap, rh);
        }
    }
}

static void bub_layout(Page *p, int w, int h) {
    HWND pw = p->wnd;
    const int pad = ui_scale(10), gap = ui_scale(12);
    const int bh = ui_scale(30);        // 按钮高
    const int hdr = ui_scale(26);       // 组框标题占的高度

    // 自下而上：按钮贴底 → 一条分隔线 → 计划列表吃掉中间剩余 → 上排两框按内容定高。
    // 上排不铺满：这一页只有 5 个字段，框拉到整页高度时字段在中间缩成一条，
    // 上下各一大片空，看着像加载失败。
    // 底部原来那行总结说明已删：结算条件、时长、间隔全在各自控件的提示里，页面上再写一遍占地方。
    const int btn_y = h - pad - bh;
    const int sep_y = btn_y - ui_scale(10);
    const int top_h = ui_scale(206);       // 右组三行 + 标题 + 组内留白
    const int note_y = pad + top_h + gap;
    int note_h = sep_y - ui_scale(8) - note_y;
    if (note_h < ui_scale(120)) note_h = ui_scale(120);

    const int fullW = w - pad * 2;
    const int cw = (fullW - gap) / 2;
    const int x0 = pad, x1 = pad + cw + gap;

    ui_place(b_box, x0, pad, cw, top_h);
    ui_place(b_box_rw, x1, pad, cw, top_h);
    ui_place(b_box_plan, x0, note_y, fullW, note_h);

    // 左组：开关在标题下方独占一行，下面两行两列
    const int onh = ui_scale(26) + ui_scale(16);
    ui_place(b_on, x0 + ui_scale(12), pad + hdr, cw - ui_scale(24), ui_scale(24));
    BubRow base[2] = {
        {"结算间隔（秒，下限 5）", b_sec, NULL, "生效时长", b_hours, "（小时）"},
        {"目标人群", b_target, NULL, "目标 ID", b_tno, NULL},
    };
    bub_field_rows(pw, x0, pad + hdr + onh, cw, top_h - hdr - onh, base, COUNT_OF(base));

    // 右组：奖励三行两列
    BubRow rw[3] = {
        {"赠送点券（每角色）", b_coin, NULL, "D点（每账号）", b_dp, NULL},
        {"D币（每账号）", b_dc, NULL, "物品 ID（0=不发）", b_item, NULL},
        {"物品数量", b_num, NULL, NULL, NULL, NULL},
    };
    bub_field_rows(pw, x1, pad + hdr, cw, top_h - hdr, rw, COUNT_OF(rw));

    // 计划列表占满组框内区
    if (b_plan) {
        MoveWindow(b_plan, x0 + ui_scale(10), note_y + hdr, fullW - ui_scale(20),
                   note_h - hdr - ui_scale(10), TRUE);
        bub_fit_cols();
    }

    // 底部：一条线 + 应用 / 导出记录 / 状态行
    const int b1 = ui_scale(88), b2 = ui_scale(100), bg = ui_scale(8);
    ui_place(b_sep, pad, sep_y, fullW, ui_scale(2));
    ui_place(GetDlgItem(pw, BUB_APPLY), pad, btn_y, b1, bh);
    ui_place(GetDlgItem(pw, BUB_EXP), pad + b1 + bg, btn_y, b2, bh);
    int sx = pad + b1 + bg + b2 + gap;
    ui_place(b_info, sx, btn_y + ui_scale(6), w - pad - sx, ui_scale(20));
}

// ---------------- 构建 ----------------
static void bub_build(Page *p, HWND parent) {
    // 分组框先建，后建的控件才压在框上面。
    // 这几个必须接住返回值：组框是自绘容器，只靠 ID 拿不到位置，忘了赋值它就一直是
    // 0×0，界面上等于没有框（踩过一次）。
    b_box = ui_group(parent, "基础设置", BUB_BOX);
    b_box_rw = ui_group(parent, "奖励设置（每结算一次发放的量）", BUB_BOX_RW);
    b_box_plan = ui_group(parent, "最近计划（最多 20 条，最新在上）", BUB_BOX_PLAN);

    b_on = ui_check(parent, "启用泡点", BUB_ON, 0);
    // 建的时候一律留空、默认值统一在末尾设：带文本建 EDIT 会发 EN_CHANGE，
    // 页面处理函数若在构建中途弹模态框，嵌套消息循环会把布局重入一遍。
    b_sec = ui_edit(parent, "", BUB_SEC, 0);
    b_target = ui_combo(parent, BUB_TARGET, 1);
    static const char *kTarget[] = {"仅在线", "全服(含离线)", "指定角色", "指定账号"};
    ui_combo_fill(b_target, kTarget, COUNT_OF(kTarget), 0);
    b_tno = ui_edit(parent, "", BUB_TNO, 0);
    // 可编辑下拉（和左上角「地址」同一个写法）：常用档点一下就走，不在档位里
    // 就直接敲数字。原来只有 1/3/5/7 四档，想跑 12 小时就只能砍成 7 或 24。
    b_hours = ui_combo(parent, BUB_HOURS, 0);
    ui_combo_fill(b_hours, kBubDurLbl, COUNT_OF(kBubDurLbl), 2);
    b_coin = ui_edit(parent, "", BUB_COIN, 0);
    b_dp = ui_edit(parent, "", BUB_DP, 0);
    b_dc = ui_edit(parent, "", BUB_DC, 0);
    b_item = ui_edit(parent, "", BUB_ITEM, 0);
    b_num = ui_edit(parent, "", BUB_NUM, 0);
    b_plan = ui_list(parent, BUB_PLAN);
    ui_cols(b_plan, (const char *const[]){"时间", "目标", "内容", "状态"},
            (const int[]){78, 90, 520, 96}, 4);
    ui_button(parent, "应用", BUB_APPLY);
    ui_button(parent, "导出记录", BUB_EXP);
    b_info = ui_label(parent, "", BUB_INFO);
    b_sep = ui_hline(parent);

    ui_tip(parent, b_on, "开启后按间隔给目标人群发放奖励；只在工具开着时结算，关掉不补算");
    ui_tip(parent, b_sec, "结算间隔，单位秒，下限 5");
    ui_tip(parent, b_target, "仅在线：登录中的账号；全服：含离线（不发物品）；指定角色/账号：在右边填 ID");
    ui_tip(parent, b_tno, "目标选「指定角色」填角色号 charac_no；选「指定账号」填账号 m_id。"
                          "选其它目标时这个框不用填，会置灰");
    ui_tip(parent, b_hours, "生效时长（小时），从点「应用」那一刻起算，可自己填数字；"
                            "0 = 持续（不自动停）。到时长后自动停止结算，重开工具也不会重新续时");
    ui_tip(parent, b_coin, "每次给每个目标角色加的赠送点券（inventory.coin），0 表示不加");
    ui_tip(parent, b_dp, "每次给每个目标账号加的 D点（cash_cera.cera），0 表示不加");
    ui_tip(parent, b_dc, "每次给每个目标账号加的 D币（cash_cera.cera_point），0 表示不加");
    ui_tip(parent, b_item, "随邮件发放的物品 ID，0 表示不发物品（全服目标不支持发物品）");
    ui_tip(parent, b_num, "物品数量（指定/在线目标最多 200）");
    ui_tip(parent, GetDlgItem(parent, BUB_APPLY), "把以上设置写进 settings.ini，立即生效并重新计时");
    ui_tip(parent, GetDlgItem(parent, BUB_EXP), "把日志里的泡点结算记录导出为 config/export/泡点记录.txt");
    ui_tip(parent, b_plan, "点一次「应用」是一条「等待发放」；之后每次结算追加一条结果，超过 20 条挤掉最旧的");

    // 初值来自 settings.ini
    if (g.cfg.bubble_on) SendMessageW(b_on, BM_SETCHECK, BST_CHECKED, 0);
    ui_set_int(b_sec, g.cfg.bubble_sec >= 5 ? g.cfg.bubble_sec : 60);
    ui_combo_set(b_target, g.cfg.bubble_target < 0 || g.cfg.bubble_target > 3 ? 0 : g.cfg.bubble_target);
    if (g.cfg.bubble_target >= 2 && g.cfg.bubble_tno) ui_set_int(b_tno, (int)g.cfg.bubble_tno);
    ui_set_int(b_hours, g.cfg.bubble_hours > 0 ? g.cfg.bubble_hours : 0);
    ui_set_int(b_coin, g.cfg.bubble_coin);
    ui_set_int(b_dp, g.cfg.bubble_dpoint);
    ui_set_int(b_dc, g.cfg.bubble_dcoin);
    ui_set_int(b_item, (int)g.cfg.bubble_item);
    ui_set_int(b_num, g.cfg.bubble_itemnum > 0 ? g.cfg.bubble_itemnum : 1);

    s_last_target = ui_combo_sel(b_target);
    bub_sync_tno();
    bub_fill_plan();
    bub_status_sync();
    (void)p;
}

// 目标 ID 只在「指定角色 / 指定账号」下才有意义，其余情况置灰：
// 灰着比让人填完再被忽略好——填了不生效，最容易被当成"功能坏了"。
static void bub_sync_tno(void) {
    int t = ui_combo_sel(b_target);
    ui_enable(b_tno, g.db_ok && (t == 2 || t == 3));
}

static void bub_lock(Page *p) {
    int on = g.db_ok;
    int ids[] = {BUB_ON, BUB_SEC, BUB_TARGET, BUB_HOURS, BUB_COIN, BUB_DP, BUB_DC,
                 BUB_ITEM, BUB_NUM, BUB_APPLY, BUB_EXP};
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
    bub_sync_tno();
}

// 读「生效时长」：可编辑下拉，按文本解析。返回 -1 表示不合法。
static int bub_read_hours(void) {
    char buf[32];
    ui_get(b_hours, buf, sizeof(buf));
    str_trim(buf);
    if (!buf[0]) return 0;                 // 留空按「持续」处理
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || (end && *end)) return -1;
    if (v < 0 || v > 8760) return -1;
    return (int)v;
}

static LRESULT bub_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (m != WM_COMMAND) return -1;
    int id = LOWORD(wp);
    if (id == BUB_TARGET) {
        s_last_target = ui_combo_sel(b_target);
        bub_sync_tno();
        return 0;
    }
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
        int hours = bub_read_hours();
        if (hours < 0) {
            app_info("生效时长只能填 0~8760 的整数（小时）。\n\n0 = 持续，不自动停。");
            return 0;
        }
        app_bubble_apply(on, (int)ui_i64(b_sec), target,
                         (int)ui_i64(b_coin), (int)ui_i64(b_dp), (int)ui_i64(b_dc),
                         (u32)ui_i64(b_item), (int)ui_i64(b_num), tno, hours);
        bub_status_sync();
        bub_fill_plan();
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
