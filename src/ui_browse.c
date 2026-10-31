// ui_browse.c：按类浏览 PVF 里的全部物品（材料 / 消耗品 / 装备 / 宠物 / 时装），
// 挑中的一行（或一批）直接进发放队列。
//
// 为什么不直接把这几类灌进发放页那张队列列表 —— 量级不对：
// 本机 Script.pvf 实测 装备 49840、时装 38739、消耗品 26000 条。一次性
// LVM_INSERTITEM 几万次要卡界面十几秒，内存也按每行近百字节往上涨，
// 而人真正会去看的只有筛选后的几十行。
//
// 所以这里用虚拟列表（LVS_OWNERDATA）：ListView 只在要把某一行画出来的时候才
// 回头问"第 N 行的内容是什么"，可见的那二三十行才轮到执行。五万行照样秒开、
// 拖动不卡。数据侧只留一份「编号 + 名称」数组，几 MB，重填就是一次线性扫。

#include "pvf.h"
#include "ui.h"

// 一次最多入队这么多。队列里每一项最后都要落一条库记录，一次性灌几万件既发不出去
// 也没法撤 —— 真要那么多就用筛选分批。
#define BR_MAX_ADD 3000

// 控件 ID（弹窗自己的号段，和其它页不冲突）
enum {
    B_KW = 3001, B_LIST, B_CNT, B_MSG,
    B_ADD, B_ALL, B_SEL, B_CLR, B_CLOSE
};

typedef struct {
    u32  id;
    char name[100];
} BrRow;

typedef struct {
    int      cat;
    HWND     kw, lv, cnt, msg;
    Vec      rows;       // BrRow
    int      total;      // 这一类的总数（不含筛选）
    int      init;
} BrCtx;

// ---------------------------------------------------------------- 数据

static const char *cat_name(int cat) {
    switch (cat) {
        case BR_MAT: return "材料";
        case BR_STACK: return "消耗品";
        case BR_EQUIP: return "装备";
        case BR_PET: return "宠物";
        case BR_AVATAR: return "时装";
        default: return "物品";
    }
}

static int kind_of_cat(int cat) {
    switch (cat) {
        case BR_MAT: return PK_MATERIAL;
        case BR_STACK: return PK_STACK;
        case BR_EQUIP: return PK_EQUIP;
        case BR_AVATAR: return PK_AVATAR;
        default: return PK_NONE;
    }
}

// 名字子串命中：UTF-8 直接按字节比，中文无所谓大小写，ASCII 部分（夹杂的字母）
// 当同一字母看。逐字节推进就行 —— UTF-8 的首字节 >= 0xC0，续字节 >= 0x80，
// 子串匹配不可能跨字符边界产生假阳性。
static int nm_match(const char *nm, const char *kw) {
    for (const char *s = nm; *s; s++) {
        const char *a = s, *b = kw;
        while (*a && *b) {
            int x = (unsigned char)*a, y = (unsigned char)*b;
            if (x >= 0x80 || y >= 0x80) { if (x != y) break; }
            else if (x == y || x - 32 == y || x + 32 == y) { /* 相同字母 */ }
            else break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

// 编号子串命中：输入的全是数字时，按编号前缀找往往比记名字快（比如 630 → 宠物）
static int id_match(u32 id, const char *kw) {
    char b[16];
    fmt(b, sizeof(b), "%u", id);
    return nm_match(b, kw);
}

static int id_in_creids(u32 id) {
    for (int i = 0; i < g.cre_ids.n; i++)
        if (*(const u32 *)VEC_AT(&g.cre_ids, u32, i) == id) return 1;
    return 0;
}

// 重建列表内容。每次筛选变化都整扫一遍 PVF 字典：115633 条 × 二分查找，
// 实测几十毫秒，够快，没必要再给它做索引。
static void br_fill(BrCtx *c) {
    char kw[64] = "";
    ui_get(c->kw, kw, sizeof(kw));
    str_trim(kw);

    vec_clear(&c->rows);
    c->total = 0;
    SendMessageW(c->lv, LVM_SETITEMCOUNT, 0, 0);

    if (!g.pvf_ok) {
        ui_text(c->cnt, "未加载 PVF");
        ui_text(c->msg, "先在主界面左上角选好 Script.pvf 再打开这个窗口。");
        return;
    }

    int want = kind_of_cat(c->cat);
    int total = pvf_item_count(&g.pvf);
    for (int i = 0; i < total; i++) {
        const char *nm = "";
        u32 id = pvf_item_at(&g.pvf, i, &nm);
        if (!*nm) continue;
        // 宠物不能按 PVF 目录认：63000（佛拉斯）在 PVF 里躺在 equipment/ 下，
        // 看着就是件装备。只有库里的 dnf_creature_info 说得清哪个编号是宠物，
        // 连库时收了一份 it_id 清单在这里比对。
        int hit = (c->cat == BR_PET) ? id_in_creids(id) : (pvf_item_kind(&g.pvf, id) == want);
        if (!hit) continue;
        c->total++;
        if (kw[0] && !nm_match(nm, kw) && !id_match(id, kw)) continue;
        BrRow *r = VEC_ADD(&c->rows, BrRow);
        r->id = id;
        snprintf(r->name, sizeof(r->name), "%s", nm);
    }

    SendMessageW(c->lv, LVM_SETITEMCOUNT, (WPARAM)c->rows.n,
                 LVSICF_NOINVALIDATEALL);

    if (c->cat == BR_PET && !g.cre_ids.n) {
        ui_text(c->cnt, "宠物 0 条");
        ui_text(c->msg, "宠物清单来自数据库（dnf_creature_info），先连上服再说。");
        return;
    }
    if (kw[0] && !c->rows.n) {
        ui_text(c->cnt, fmt_new("%s · 共 %d 条 · 命中 0 条", cat_name(c->cat), c->total));
        ui_text(c->msg, "没有命中。换个关键词，或者留空看全量。");
        return;
    }
    if (kw[0])
        ui_text(c->cnt, fmt_new("%s · 共 %d 条 · 命中 %d 条", cat_name(c->cat), c->total,
                                (int)c->rows.n));
    else
        ui_text(c->cnt, fmt_new("%s · 共 %d 条", cat_name(c->cat), c->total));
    ui_text(c->msg, fmt_new("双击一行，或选中多行点「加入所选」。一次最多加 %d 项。",
                            BR_MAX_ADD));
}

// ---------------------------------------------------------------- 加入队列

// 把编号批量灌进发放队列。返回实际处理了几条。
static int br_add_ids(BrCtx *c, const u32 *ids, int n) {
    if (n <= 0) return 0;
    int add = 0, upd = 0, dup = 0;
    for (int i = 0; i < n; i++) {
        int r = app_grant_add_new(ids[i], 1);
        if (r == 1) add++;
        else if (r == 2) upd++;
        else dup++;
    }
    ui_pages_reload(PAGE_GRANT);
    ui_text(c->msg, fmt_new("已加入队列：新增 %d 项 · 数量补大 %d 项 · 队列里已有 %d 项",
                            add, upd, dup));
    app_status("从浏览窗口加入 %d 项（新增 %d · 队列当前 %d 项）", n, add, (int)g.grant.n);
    return add;
}

// 收当前选中的行（虚拟列表本身不保存文本，得顺着选中态回一趟索引）
static int br_sel_ids(BrCtx *c, u32 *ids, int cap) {
    int n = 0;
    int row = -1;
    while (n < cap) {
        row = (int)SendMessageW(c->lv, LVM_GETNEXTITEM, (WPARAM)row, LVNI_SELECTED);
        if (row < 0 || row >= c->rows.n) break;
        ids[n++] = ((const BrRow *)VEC_AT(&c->rows, BrRow, row))->id;
    }
    return n;
}

static void br_add_sel(BrCtx *c) {
    int nsel = (int)SendMessageW(c->lv, LVM_GETSELECTEDCOUNT, 0, 0);
    if (nsel <= 0) {
        ui_text(c->msg, "先在列表里选中要发的东西（Ctrl / Shift 可以多选）。");
        return;
    }
    if (nsel > BR_MAX_ADD) {
        ui_text(c->msg, fmt_new("选中了 %d 项，超过一次上限 %d 项，先分批加。", nsel,
                                BR_MAX_ADD));
        return;
    }
    u32 *ids = (u32 *)xalloc(sizeof(u32) * (size_t)nsel);
    if (!ids) return;
    int n = br_sel_ids(c, ids, nsel);
    br_add_ids(c, ids, n);
    free(ids);
}

static void br_add_all(BrCtx *c) {
    if (!c->rows.n) {
        ui_text(c->msg, "列表是空的。");
        return;
    }
    if (c->rows.n > BR_MAX_ADD) {
        ui_text(c->msg, fmt_new("当前列出 %d 项，超过一次上限 %d 项，先用关键词缩小范围。",
                                (int)c->rows.n, BR_MAX_ADD));
        return;
    }
    u32 *ids = (u32 *)xalloc(sizeof(u32) * (size_t)c->rows.n);
    if (!ids) return;
    for (int i = 0; i < c->rows.n; i++) ids[i] = ((const BrRow *)VEC_AT(&c->rows, BrRow, i))->id;
    br_add_ids(c, ids, (int)c->rows.n);
    free(ids);
}

// ---------------------------------------------------------------- 窗口

static LRESULT br_proc(HWND d, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    BrCtx *c = (BrCtx *)ctx;

    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == B_LIST && nh->code == LVN_GETDISPINFOW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)lp;
            int row = di->item.iItem;
            if (!(di->item.mask & LVIF_TEXT) || row < 0 || row >= c->rows.n) return 0;
            const BrRow *r = (const BrRow *)VEC_AT(&c->rows, BrRow, row);
            if (di->item.iSubItem == 0) {
                char b[16];
                fmt(b, sizeof(b), "%u", r->id);
                utf16(b, di->item.pszText, (int)di->item.cchTextMax);
            } else {
                utf16(r->name, di->item.pszText, (int)di->item.cchTextMax);
            }
            return 0;
        }
        if (nh->idFrom == B_LIST && nh->code == NM_DBLCLK) {
            br_add_sel(c);
            return 0;
        }
    }

    if (m == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == B_KW && HIWORD(wp) == EN_CHANGE) { br_fill(c); return 0; }
        if (id == B_CLR) { ui_text(c->kw, ""); br_fill(c); return 0; }
        if (id == B_ADD) { br_add_sel(c); return 0; }
        if (id == B_ALL) { br_add_all(c); return 0; }
        if (id == B_SEL) {
            // iItem = -1 表示"所有行"，虚拟列表也认这个用法
            ListView_SetItemState(c->lv, -1, LVIS_SELECTED, LVIS_SELECTED);
            int nsel = (int)SendMessageW(c->lv, LVM_GETSELECTEDCOUNT, 0, 0);
            SetFocus(c->lv);
            ui_text(c->msg, fmt_new("已选中 %d 行。行太多就先用关键词筛。", nsel));
            return 0;
        }
        if (id == B_CLOSE) { ui_dlg_end(0); return 0; }
    }

    return -1;
}

void ui_browse_open(HWND owner, int cat) {
    if (cat != BR_MAT && cat != BR_STACK && cat != BR_EQUIP && cat != BR_PET &&
        cat != BR_AVATAR)
        return;

    BrCtx c;
    memset(&c, 0, sizeof(c));
    c.cat = cat;
    vec_init(&c.rows, sizeof(BrRow));

    HWND d = ui_dlg_new(owner, fmt_new("浏览%s · 挑好直接进发放队列", cat_name(cat)),
                        720, 520, br_proc, &c);
    if (!d) { vec_free(&c.rows); return; }

    int pad = ui_scale(12), h = ui_scale(24), gap = ui_scale(6);
    URow r;

    ui_row(&r, d, pad, pad, h);
    ui_add(&r, ui_label(d, "筛选", 0), ui_scale(34));
    c.kw = ui_edit(d, "", B_KW, 0);
    ui_add(&r, c.kw, ui_scale(200));
    ui_add(&r, ui_button(d, "清空", B_CLR), ui_scale(50));
    c.cnt = ui_label(d, "", B_CNT);
    ui_add(&r, c.cnt, -1);

    int ly = pad + h + gap;
    // 虚拟列表不能走 ui_list（它不带 LVS_OWNERDATA），这里自己建。
    // 去掉 LVS_SINGLESEL 才能多选批量发放。
    c.lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                               LVS_SHOWSELALWAYS,
                           0, 0, 0, 0, d, (HMENU)(INT_PTR)B_LIST, ui_inst(), NULL);
    if (c.lv) {
        SendMessageW(c.lv, WM_SETFONT, (WPARAM)ui_font(), TRUE);
        ListView_SetExtendedListViewStyle(c.lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                    LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
        ui_cols(c.lv, (const char *const[]){"编号", "名称"}, (const int[]){100, 520}, 2);
    }
    // 底部一行：说明在左，按钮从右往左码回去（ui_add 撑满只认最后一个，所以这里手动排）
    int by = ui_scale(520) - pad - h;
    int right = ui_scale(720) - pad;
    if (c.lv) ui_place(c.lv, pad, ly, ui_scale(720) - pad * 2, by - gap - ly);

    int bw = ui_scale(80);
    int xp = right;
    xp -= ui_scale(60); ui_place(ui_button(d, "关闭", B_CLOSE), xp, by, ui_scale(60), h);
    xp -= gap; xp -= bw; ui_place(ui_button(d, "加入所选", B_ADD), xp, by, bw, h);
    xp -= gap; xp -= bw; ui_place(ui_button(d, "加入全部", B_ALL), xp, by, bw, h);
    xp -= gap; xp -= ui_scale(60); ui_place(ui_button(d, "全选", B_SEL), xp, by, ui_scale(60), h);
    c.msg = ui_label(d, "", B_MSG);
    ui_place(c.msg, pad, by, xp - pad - gap, h);

    ui_tip(d, c.kw, "按物品名或编号过滤，边打边筛，不用回车");
    ui_tip(d, c.lv, "这几类都是成千上万条，列表是虚拟的：只有看得见的那几十行才去读内容。"
                    "双击一行，或 Ctrl / Shift 多选后点「加入所选」");
    ui_tip(d, GetDlgItem(d, B_ALL), "把当前列表全部加进发放队列（受筛选影响）；有上限，太多了会提示先筛");
    ui_tip(d, GetDlgItem(d, B_SEL), "选中当前列表的所有行，再点「加入所选」");
    ui_tip(d, GetDlgItem(d, B_ADD), "把选中的行加进发放队列，每项数量 1");

    br_fill(&c);
    SetFocus(c.kw);

    ui_dlg_run(d, owner);
    // 关窗口就把这批索引放掉，免得占着几 MB 不放；下次打开重扫一遍也就几十毫秒
    vec_clear(&c.rows);
    vec_free(&c.rows);
}
