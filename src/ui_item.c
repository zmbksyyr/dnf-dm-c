// ui_item：物品页（背包 6 子页 + 清除错误物品）、物品编辑弹窗、查询页、发放页。
#include "ui.h"
#include <commdlg.h>
// 常用物品编号表（内置，运行时不需要任何 ini）。663 个编号压到 689 字节。
#include "items_data.h"

// ================================================================ 物品编辑弹窗
typedef struct {
    Item it;
    HWND id, num, enh, forge, dur, seal_cnt, seal_chk, type, name;
    HWND inc_type, inc_val, orb, ow;
    HWND sid[4], slv[4], snm[4];
    HWND hex;
} EditCtx;

static const char *kTypeList[] = {"空槽位",   "装备", "消耗品", "材料", "任务材料",
                                  "宠物",     "宠物装备", "宠物消耗品", "副职业"};
static const char *kIncList[] = {"无", "体力", "精神", "力量", "智力"};

static int type_to_sel(int t) {
    static const int m[] = {0, 1, 2, 3, 4, 5, 6, 7, 0x0A};
    for (int i = 0; i < 9; i++)
        if (m[i] == t && (i > 0 || t == 0)) return i;
    return 0;
}

static int sel_to_type(int s) {
    static const int m[] = {0, 1, 2, 3, 4, 5, 6, 7, 0x0A};
    if (s < 0 || s > 8) return 0;
    return m[s];
}

// 装备的 4 字节「数量」位不是堆叠数，改由耐久表达；堆叠物则相反
static void edit_sync(EditCtx *e) {
    int t = sel_to_type(ui_combo_sel(e->type));
    int is_eq = (t == IT_EQUIP || t == IT_CREATURE_EQUIP);
    if (is_eq) {
        ui_text(e->num, "—");
        ui_enable(e->num, 0);
        ui_enable(e->dur, 1);
    } else {
        char b[24];
        fmt(b, sizeof(b), "%u", item_num(&e->it));
        ui_text(e->num, b);
        ui_enable(e->num, 1);
        ui_enable(e->dur, 0);
    }
}

static void edit_fill(EditCtx *e) {
    const Item *it = &e->it;
    ui_set_int(e->id, (int)item_id(it));
    ui_combo_set(e->type, type_to_sel(item_type(it)));
    ui_set_int(e->num, (int)item_num(it));
    ui_set_int(e->enh, item_enhance(it));
    ui_set_int(e->forge, item_forge(it));
    ui_set_int(e->dur, item_durability(it));
    ui_set_int(e->seal_cnt, item_seal_cnt(it));
    SendMessageW(e->seal_chk, BM_SETCHECK, item_seal(it) ? BST_CHECKED : BST_UNCHECKED, 0);
    ui_combo_set(e->inc_type, item_inc_type(it) > 4 ? 0 : item_inc_type(it));
    ui_set_int(e->inc_val, item_inc_value(it));
    ui_set_int(e->orb, (int)item_orb(it));
    ui_set_int(e->ow, item_otherworld(it));
    for (int i = 0; i < 4; i++) {
        ui_set_int(e->sid[i], item_seal_id(it, i));
        ui_set_int(e->slv[i], item_seal_lv(it, i));
        const char *nm = g.pvf_ok ? pvf_seal_name(&g.pvf, item_seal_id(it, i)) : "";
        ui_text(e->snm[i], *nm ? nm : "—");
    }
    char hex[ITEM_SIZE * 3 + 8];
    item_hex(it, hex, sizeof(hex));
    ui_text(e->hex, hex);
    char nb[96];
    ui_text(e->name, app_item_name(item_id(it), nb, sizeof(nb)));
    edit_sync(e);
}

static LRESULT edit_proc(HWND d, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    EditCtx *e = (EditCtx *)ctx;
    if (m == WM_COMMAND) {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == 4002 && code == CBN_SELCHANGE) {
            edit_sync(e);
            return 0;
        }
        if (id == IDOK) {
            Item it = e->it;
            int t = sel_to_type(ui_combo_sel(e->type));
            item_set_type(&it, (u8)t);
            item_set_id(&it, (u32)ui_int(e->id));
            // 装备的 amount 位原样保留，只有堆叠物才改「数量」
            if (t != IT_EQUIP && t != IT_CREATURE_EQUIP) item_set_num(&it, (u32)ui_int(e->num));
            item_set_enhance(&it, ui_int(e->enh));
            item_set_forge(&it, ui_int(e->forge));
            item_set_durability(&it, (u16)ui_int(e->dur));
            item_set_seal_cnt(&it, ui_int(e->seal_cnt));
            item_set_seal(&it, SendMessageW(e->seal_chk, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            item_set_inc_type(&it, ui_combo_sel(e->inc_type) < 0 ? 0 : ui_combo_sel(e->inc_type));
            item_set_inc_value(&it, (u16)ui_int(e->inc_val));
            item_set_orb(&it, (u32)ui_int(e->orb));
            item_set_otherworld(&it, (u16)ui_int(e->ow));
            for (int i = 0; i < 4; i++) {
                item_set_seal_id(&it, i, ui_int(e->sid[i]));
                item_set_seal_lv(&it, i, (u16)ui_int(e->slv[i]));
            }
            if (item_empty(&it)) memset(&it, 0, sizeof(it));
            e->it = it;
            ui_dlg_end(1);
            return 0;
        }
        if (id == 2001) {   // 按编号刷新名称
            char nb[96];
            ui_text(e->name, app_item_name((u32)ui_int(e->id), nb, sizeof(nb)));
            return 0;
        }
    }
    (void)d;
    (void)wp;
    (void)lp;
    return -1;
}

static int item_edit(HWND owner, const Item *in, Item *out) {
    EditCtx e;
    memset(&e, 0, sizeof(e));
    e.it = *in;

    HWND d = ui_dlg_new(owner, "编辑物品", 470, 430, edit_proc, &e);
    if (!d) return 0;

    int y = ui_scale(12), h = ui_scale(24), g4 = ui_scale(5);
    URow r;

    ui_row(&r, d, ui_scale(12), y, h);
    ui_add(&r, ui_label(d, "编号", 0), ui_scale(34));
    e.id = ui_edit(d, "", 4001, 0);
    ui_add(&r, e.id, ui_scale(90));
    ui_add(&r, ui_button(d, "查名", 2001), ui_scale(50));
    e.name = ui_label(d, "", 0);
    ui_add(&r, e.name, -1);

    y += h + g4;
    ui_row(&r, d, ui_scale(12), y, h);
    ui_add(&r, ui_label(d, "类型", 0), ui_scale(34));
    e.type = ui_combo(d, 4002, 1);
    ui_combo_fill(e.type, kTypeList, COUNT_OF(kTypeList), 0);
    ui_add(&r, e.type, ui_scale(104));
    ui_add(&r, ui_label(d, "数量", 0), ui_scale(34));
    e.num = ui_edit(d, "", 4003, 0);
    ui_add(&r, e.num, ui_scale(70));
    ui_add(&r, ui_label(d, "耐久", 0), ui_scale(34));
    e.dur = ui_edit(d, "", 4004, 0);
    ui_add(&r, e.dur, ui_scale(70));

    y += h + g4;
    ui_row(&r, d, ui_scale(12), y, h);
    ui_add(&r, ui_label(d, "强化", 0), ui_scale(34));
    e.enh = ui_edit(d, "", 4005, 0);
    ui_add(&r, e.enh, ui_scale(50));
    ui_add(&r, ui_label(d, "锻造", 0), ui_scale(34));
    e.forge = ui_edit(d, "", 4006, 0);
    ui_add(&r, e.forge, ui_scale(50));
    e.seal_chk = ui_check(d, "封装", 4007, 0);
    ui_add(&r, e.seal_chk, ui_scale(64));
    ui_add(&r, ui_label(d, "封印次数", 0), ui_scale(58));
    e.seal_cnt = ui_edit(d, "", 4008, 0);
    ui_add(&r, e.seal_cnt, ui_scale(50));

    y += h + g4;
    ui_row(&r, d, ui_scale(12), y, h);
    ui_add(&r, ui_label(d, "增幅", 0), ui_scale(34));
    e.inc_type = ui_combo(d, 4009, 1);
    ui_combo_fill(e.inc_type, kIncList, COUNT_OF(kIncList), 0);
    ui_add(&r, e.inc_type, ui_scale(80));
    ui_add(&r, ui_label(d, "值", 0), ui_scale(24));
    e.inc_val = ui_edit(d, "", 4010, 0);
    ui_add(&r, e.inc_val, ui_scale(70));
    ui_add(&r, ui_label(d, "宝珠", 0), ui_scale(34));
    e.orb = ui_edit(d, "", 4011, 0);
    ui_add(&r, e.orb, ui_scale(62));
    ui_add(&r, ui_label(d, "异界", 0), ui_scale(34));
    e.ow = ui_edit(d, "", 4012, 0);
    ui_add(&r, e.ow, ui_scale(50));

    y += h + ui_scale(8);
    ui_place(ui_label_b(d, "魔法封印", 0), ui_scale(12), y, ui_scale(80), h);
    y += h;
    for (int i = 0; i < 4; i++) {
        ui_row(&r, d, ui_scale(12), y, h);
        ui_add(&r, ui_label(d, fmt_new("第 %d 组", i + 1), 0), ui_scale(46));
        ui_add(&r, ui_label(d, "id", 0), ui_scale(16));
        e.sid[i] = ui_edit(d, "", 4020 + i, 0);
        ui_add(&r, e.sid[i], ui_scale(50));
        ui_add(&r, ui_label(d, "等级", 0), ui_scale(34));
        e.slv[i] = ui_edit(d, "", 4030 + i, 0);
        ui_add(&r, e.slv[i], ui_scale(50));
        e.snm[i] = ui_label(d, "—", 0);
        ui_add(&r, e.snm[i], -1);
        y += h + ui_scale(2);
    }

    // 数值框统一按「写回二进制时的字段宽度」限制：这个对话框的框本来就窄，
    // 手打一个超范围的值只会被 cast 截成另一个数，看起来像是工具算错了。
    ui_num(e.id, 1, 4294967295LL);
    ui_num(e.num, 0, 4294967295LL);
    ui_num(e.dur, 0, 65535);
    ui_num(e.enh, 0, 31);
    ui_num(e.forge, 0, 255);
    ui_num(e.seal_cnt, 0, 7);
    ui_num(e.inc_val, 0, 65535);
    ui_num(e.orb, 0, 4294967295LL);
    ui_num(e.ow, 0, 65535);
    for (int i = 0; i < 4; i++) {
        ui_num(e.sid[i], 0, 255);
        ui_num(e.slv[i], 0, 65535);
    }

    ui_place(ui_label(d, "原始数据（未解释字节原样回写）", 0), ui_scale(12), y + ui_scale(6), ui_scale(300), h);
    e.hex = ui_edit(d, "", 4040, 1);
    ui_place(e.hex, ui_scale(12), y + ui_scale(6) + h, ui_scale(430), ui_scale(40));

    ui_place(ui_button(d, "确定", IDOK), ui_scale(280), ui_scale(388), ui_scale(78), ui_scale(26));
    ui_place(ui_button(d, "取消", IDCANCEL), ui_scale(366), ui_scale(388), ui_scale(78), ui_scale(26));

    edit_fill(&e);
    int ok = ui_dlg_run(d, owner);
    if (ok) *out = e.it;
    return ok;
}

// ================================================================ 物品页
enum { I_SUB = PAGE_ID(PAGE_ITEM, 1), I_LIST, I_EDIT, I_CLR, I_CLRALL, I_INFO, I_BAD };

static const char *kSubName[] = {"背包", "穿戴", "宠物", "仓库", "珠宝", "账号金库"};
static HWND s_sub, s_list, s_info, s_sep;

// ---- 错误物品：结构异常，或编号在 PVF 里查不到 ----
static void item_reload(Page *p);

typedef struct {
    int  sub, slot;
    u32  id;
    char why[48];
} BadRow;

// 名字是否存在的判定交给 PVF，未加载时退化成只查结构
static int has_name(u32 id, void *ctx) {
    (void)ctx;
    return g.pvf_ok && *pvf_item_name(&g.pvf, id);
}

static LRESULT bad_proc(HWND d, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    (void)d;
    (void)lp;
    (void)ctx;
    if (m == WM_COMMAND && LOWORD(wp) == IDOK) {
        ui_dlg_end(1);
        return 0;
    }
    return -1;
}

static void item_bad_clear(void) {
    if (!g.char_ok) {
        app_info("请先选择角色。");
        return;
    }
    if (g.cur.online) {
        app_info("该角色当前在线，改动会被服务器覆盖。请先退出游戏。");
        return;
    }
    Vec rows;
    vec_init(&rows, sizeof(BadRow));
    int loaded = 0;
    for (int sub = 0; sub < 6; sub++) {
        Blob *b = app_page_blob(sub);
        if (!b || !b->ok) continue;
        loaded++;
        for (int i = 0; i < b->n; i++) {
            char why[48];
            const Item *it = &b->items[i];
            if (!it || !item_bad(it, has_name, NULL, why, sizeof(why))) continue;
            BadRow *r = VEC_ADD(&rows, BadRow);
            r->sub = sub;
            r->slot = i;
            r->id = item_id(it);
            snprintf(r->why, sizeof(r->why), "%s", why);
        }
    }
    if (!rows.n) {
        app_info(fmt_new("已扫描 %d 个容器，没有发现错误物品。", loaded));
        vec_free(&rows);
        return;
    }

    HWND d = ui_dlg_new(g_main, "清除错误物品", 560, 430, bad_proc, NULL);
    if (!d) {
        vec_free(&rows);
        return;
    }
    char head[160];
    if (g.pvf_ok)
        snprintf(head, sizeof(head), "扫描 %d 个容器 · 发现 %d 处异常（编号已按 PVF 校验）", loaded, rows.n);
    else
        snprintf(head, sizeof(head), "扫描 %d 个容器 · 发现 %d 处异常（PVF 未加载，仅检查结构）", loaded, rows.n);
    ui_place(ui_label(d, head, 0), ui_scale(12), ui_scale(10), ui_scale(530), ui_scale(20));

    HWND lv = ui_list(d, 5001);
    ui_cols(lv, (const char *const[]){"容器", "槽位", "编号", "名称", "原因"},
            (const int[]){86, 50, 90, 170, 118}, 5);
    ui_place(lv, ui_scale(12), ui_scale(34), ui_scale(536), ui_scale(316));
    for (int i = 0; i < rows.n; i++) {
        BadRow *r = VEC_AT(&rows, BadRow, i);
        char sl[12], id[16], nm[96];
        snprintf(sl, sizeof(sl), "%d", r->slot + 1);
        snprintf(id, sizeof(id), "%u", r->id);
        const char *cells[5] = {kSubName[r->sub], sl, id, app_item_name(r->id, nm, sizeof(nm)), r->why};
        ui_row_add(lv, cells, 5, (LPARAM)i);
    }

    ui_place(ui_label(d, "清除 = 把这些格子写成空槽，确定后立即写库。", 0),
             ui_scale(12), ui_scale(358), ui_scale(400), ui_scale(20));
    ui_place(ui_button(d, fmt_new("清除 %d 处", rows.n), IDOK), ui_scale(348), ui_scale(386),
             ui_scale(110), ui_scale(26));
    ui_place(ui_button(d, "取消", IDCANCEL), ui_scale(466), ui_scale(386), ui_scale(82), ui_scale(26));

    int ok = ui_dlg_run(d, g_main);
    if (!ok) {
        vec_free(&rows);
        return;
    }
    Item z;
    memset(&z, 0, sizeof(z));
    u32 pages = 0;
    for (int i = 0; i < rows.n; i++) {
        BadRow *r = VEC_AT(&rows, BadRow, i);
        Blob *b = app_page_blob(r->sub);
        if (b && b->ok && r->slot >= 0 && r->slot < b->n) b->items[r->slot] = z;
        pages |= 1u << r->sub;
    }
    int n = rows.n;
    vec_free(&rows);
    // 一次扫描可能跨多个容器：逐页排入写队列，200ms 后的合并写会一起落盘
    for (int sub = 0; sub < 6; sub++)
        if (pages & (1u << sub)) app_pending_page(sub);
    item_reload(NULL);
    app_status("已清空 %d 处错误物品 · 正在写库", n);
}

static void item_reload(Page *p) {
    (void)p;
    int sub = ui_combo_sel(s_sub);
    if (sub < 0) sub = 0;
    Blob *b = app_page_blob(sub);
    // 上一次的选中与滚动位置：写库完成、别的动作完成都会走到这里，
    // 每回都从顶部重新填一遍的话，正在往下翻的人会被弹回第一行、选中也没了
    int keep = -1, top = 0;
    if (s_list) {
        int r = ui_row_sel(s_list);
        if (r >= 0) keep = (int)ui_row_data(s_list, r);
        top = (int)SendMessageW(s_list, LVM_GETTOPINDEX, 0, 0);
    }
    ui_rows_clear(s_list);
    int used = 0;
    int keep_row = -1;
    if (b && b->ok) {
        ui_batch(s_list, 1);
        for (int i = 0; i < b->n; i++) {
            const Item *it = &b->items[i];
            if (!it || item_empty(it)) continue;
            used++;
            char sn[16], id[16], nm[96], num[24], enh[16], inc[40], seal[24], forge[16], st[16];
            fmt(sn, sizeof(sn), "%d", i + 1);
            fmt(id, sizeof(id), "%u", item_id(it));
            // 装备的 amount 字段不是堆叠数，显示耐久更有意义
            u8 ty = item_type(it);
            if (ty == IT_EQUIP || ty == IT_CREATURE_EQUIP)
                fmt(num, sizeof(num), "耐久 %u", (unsigned)item_durability(it));
            else
                fmt(num, sizeof(num), "%u", (unsigned)item_num(it));
            fmt(enh, sizeof(enh), item_enhance(it) ? "+%d" : "", item_enhance(it));
            fmt(forge, sizeof(forge), item_forge(it) ? "+%d" : "", item_forge(it));
            const char *incn = item_inc_type(it) > 0 && item_inc_type(it) <= 4 ? kIncList[item_inc_type(it)] : "";
            if (*incn) fmt(inc, sizeof(inc), "%s +%u", incn, item_inc_value(it));
            else inc[0] = 0;
            if (item_seal(it)) fmt(seal, sizeof(seal), "封装 %d", item_seal_cnt(it));
            else snprintf(seal, sizeof(seal), "未封装");
            st[0] = 0;
            if (app_page_busy(sub)) snprintf(st, sizeof(st), "写入中");
            const char *cells[10] = {sn, app_item_name(item_id(it), nm, sizeof(nm)), id,
                                     item_type_name(item_type(it)), num, enh, inc, seal, forge, st};
            int row = ui_row_add(s_list, cells, 10, (LPARAM)i);
            if (i == keep) keep_row = row;
        }
        ui_batch(s_list, 0);
        // 恢复：先选中原来那格（行号可能因空格被跳过而变，按槽位认），
        // 再把视线挪回去；两者都做才算真的"没打断"
        if (keep_row >= 0) ui_row_sel_set(s_list, keep_row);
        else if (top > 0 && top < (int)SendMessageW(s_list, LVM_GETITEMCOUNT, 0, 0))
            SendMessageW(s_list, LVM_ENSUREVISIBLE, (WPARAM)top, FALSE);
    }
    char b2[160];
    if (!g.char_ok) snprintf(b2, sizeof(b2), "请先选择角色");
    else if (!b || !b->ok) snprintf(b2, sizeof(b2), "该容器未载入");
    else snprintf(b2, sizeof(b2), "%s %d/%d 格", kSubName[sub], used, b->n);
    ui_text(s_info, b2);
}

static void item_edit_sel(void) {
    int sub = ui_combo_sel(s_sub);
    Blob *b = app_page_blob(sub < 0 ? 0 : sub);
    int row = ui_row_sel(s_list);
    if (!b || !b->ok || row < 0) return;
    int slot = (int)ui_row_data(s_list, row);
    if (slot < 0 || slot >= b->n) return;
    Item out;
    if (!item_edit(g_main, &b->items[slot], &out)) return;
    app_item_set(sub, slot, &out);        // 确定即写库，不再走「保存改动」
    item_reload(NULL);
}

static void item_clear_sel(void) {
    int sub = ui_combo_sel(s_sub);
    Blob *b = app_page_blob(sub < 0 ? 0 : sub);
    int row = ui_row_sel(s_list);
    if (!b || !b->ok || row < 0) return;
    int slot = (int)ui_row_data(s_list, row);
    if (slot < 0 || slot >= b->n) return;
    if (!app_confirm(fmt_new("清空第 %d 格？（立即写库）", slot + 1))) return;
    Item z;
    memset(&z, 0, sizeof(z));
    app_item_set(sub, slot, &z);
    item_reload(NULL);
}

static void item_clear_all(void) {
    int sub = ui_combo_sel(s_sub);
    Blob *b = app_page_blob(sub < 0 ? 0 : sub);
    if (!b || !b->ok) return;
    if (g.cur.online) { app_info("该角色当前在线，改动会被服务器覆盖。请先退出游戏。"); return; }
    if (!app_confirm(fmt_new("把「%s」全部 %d 格清空？立即写库。", kSubName[sub], b->n)))
        return;
    Item z;
    memset(&z, 0, sizeof(z));
    for (int i = 0; i < b->n; i++) b->items[i] = z;
    app_pending_page(sub);
    item_reload(NULL);
}


// ---------------- 全服错误物品扫描 ----------------
// （已移到 GM 页：全服级清理属于维护动作，跟角色级「清除错误物品」分开）

static LRESULT item_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND) {
        switch (LOWORD(wp)) {
            case I_SUB:
                if (HIWORD(wp) == CBN_SELCHANGE) item_reload(p);
                return 0;
            case I_EDIT: item_edit_sel(); return 0;
            case I_CLR: item_clear_sel(); return 0;
            case I_CLRALL: item_clear_all(); return 0;
            case I_BAD: item_bad_clear(); return 0;
        }
    }
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == I_LIST && nh->code == NM_DBLCLK) {
            item_edit_sel();
            return 0;
        }
    }
    return -1;
}

static void item_layout(Page *p, int w, int h) {
    (void)w;
    int pad = ui_scale(6), rowh = ui_scale(23);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "子页"), ui_scale(32));
    ui_add(&r, s_sub, ui_scale(104));
    ui_add(&r, ui_btn_at(p->wnd, I_EDIT, "编辑"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, I_CLR, "清空所选"), ui_scale(76));
    ui_add(&r, ui_btn_at(p->wnd, I_CLRALL, "整页清空"), ui_scale(76));
    ui_add(&r, ui_btn_at(p->wnd, I_BAD, "清除错误物品"), ui_scale(104));
    // 窄窗口先保按钮，提示收起来（状态栏还有一份）
    RECT rc;
    GetClientRect(p->wnd, &rc);
    ui_show(s_info, rc.right - r.x >= ui_scale(88));
    ui_add(&r, s_info, -1);
    int ly = pad + rowh + ui_scale(5);
    ui_place(s_sep, pad, ly, w - pad * 2, ui_scale(2));
    ui_fill_rest(p->wnd, s_list, ly + ui_scale(6));
    (void)h;
}

static void item_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    ui_enable(s_list, on);
    ui_enable(s_sub, on);
    HWND bs[4] = {GetDlgItem(p->wnd, I_EDIT), GetDlgItem(p->wnd, I_CLR),
                  GetDlgItem(p->wnd, I_CLRALL), GetDlgItem(p->wnd, I_BAD)};
    for (int i = 0; i < 4; i++) ui_enable(bs[i], on);
}

static void item_build(Page *p, HWND parent) {
    s_sub = ui_combo(parent, I_SUB, 1);
    ui_combo_fill(s_sub, kSubName, COUNT_OF(kSubName), 0);
    s_list = ui_list(parent, I_LIST);
    s_sep = ui_hline(parent);
    s_info = ui_label(parent, "请先选择角色", I_INFO);
    ui_cols(s_list,
            (const char *const[]){"槽位", "物品", "编号", "类型", "数量/耐久", "强化", "增幅", "封印",
                                  "锻造", "状态"},
            (const int[]){40, 160, 84, 68, 78, 44, 70, 56, 44, 56}, 10);
    ui_button(parent, "编辑", I_EDIT);
    ui_button(parent, "清空所选", I_CLR);
    ui_button(parent, "整页清空", I_CLRALL);
    ui_button(parent, "清除错误物品", I_BAD);
    ui_tip(parent, GetDlgItem(parent, I_BAD),
           "把当前角色各容器里类型/编号/数量非法或 PVF 认不出的物品写成空槽（确定后立即写库）");
}

void page_item_create(Page *p) {
    p->build = item_build;
    p->layout = item_layout;
    p->reload = item_reload;
    p->lock = item_lock;
    p->on = item_on;
}

// ================================================================ 查询页
enum {
    Q_KW = PAGE_ID(PAGE_QUERY, 1), Q_LIST, Q_INFO, Q_GO, Q_ADD, Q_ALL,
    Q_MAT, Q_EQU, Q_AVA, Q_TASK       // 加载 常用材料 / 装备 / 时装 / 任务材料
};

static HWND q_kw, q_list, q_info, q_sep;
static void q_fit_cols(void);      // 按列表实际客户区摊列宽，见下

// 列表行的附带数据：低 32 位是物品编号，高 32 位是「这道工序要几个」。
// LPARAM 在 x64 上是 64 位，一个字段装得下，不用再挂一张跟行号对齐的并排数组
// （列表随时会被清空重填，那种数组很容易错位）。
#define QROW(id, num) ((LPARAM)(((u64)(u32)(num) << 32) | (u32)(id)))
#define QROW_ID(v)    ((u32)(v))
#define QROW_NUM(v)   ((u32)(((u64)(v)) >> 32))

// 内置编号表是「排序后差分 + base128 varint」，这里逐条还原成编号
static u32 varint_rd(const unsigned char **pp) {
    const unsigned char *p = *pp;
    u32 v = 0;
    int sh = 0;
    for (;;) {
        unsigned char b = *p++;
        v |= (u32)(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    *pp = p;
    return v;
}

// 「加载常用材料 / 装备 / 时装」：把这个清单填进结果列表。
// 只查不灌 —— 队列是发放页的事，这里回答的是「这批常用件在当前的 PVF 里都有哪些」；
// 看中了再双击或按「加入发放」进队列。每次都先清空列表，不叠加。
static void common_list(const unsigned char *data, size_t len, const char *what) {
    if (!g.pvf_ok) {
        app_info("还没加载 PVF。\n\n"
                 "常用物品只存了编号，名字和「当前 PVF 里有没有这件」都得查 PVF 索引，"
                 "所以得先在左上角把 Script.pvf 选好。");
        return;
    }
    ui_rows_clear(q_list);
    ui_batch(q_list, 1);
    const unsigned char *p = data, *end = data + len;
    u32 id = 0;
    int hit = 0, miss = 0;
    while (p < end) {
        id += varint_rd(&p);
        const char *nm = pvf_item_name(&g.pvf, id);
        if (!nm || !*nm) { miss++; continue; }   // 这版 PVF 里没有，跳过
        char ids[16];
        fmt(ids, sizeof(ids), "%u", id);
        const char *cells[4] = {ids, nm, "1", "双击加入发放队列"};
        ui_row_add(q_list, cells, 4, QROW(id, 1));
        hit++;
    }
    ui_batch(q_list, 0);
    q_fit_cols();                       // 数据多了才有竖滚动条，客户区变窄，重新摊一次
    ui_text(q_info, fmt_new("%s %d 条 · PVF 里没有 %d 条", what, hit, miss));
    app_status("已列出%s %d 条", what, hit);
}

// 列宽按列表「实际客户区」摊：编号、数量定宽，名称/说明吃掉剩下的宽。
// 不能按页面宽度算 —— 列表带边框，出了竖向滚动条还要再让出一条，差这十几像素
// 列表底下就会多一条横向滚动条，看着像多出一条白带。填充完数据后还要再算一次：
// 数据多了才出现竖向滚动条，那时客户区比布局时窄。
static void q_fit_cols(void) {
    if (!q_list) return;
    RECT rc;
    if (!GetClientRect(q_list, &rc)) return;
    int usable = (int)(rc.right - rc.left) - ui_scale(1);
    int c0 = ui_scale(72), c2 = ui_scale(56);
    int rest = usable - c0 - c2;
    if (rest < ui_scale(160)) rest = ui_scale(160);
    int c1 = rest * 40 / 100;
    int c3 = rest - c1;
    SendMessageW(q_list, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(c0, 0));
    SendMessageW(q_list, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(c1, 0));
    SendMessageW(q_list, LVM_SETCOLUMNWIDTH, 2, MAKELPARAM(c2, 0));
    SendMessageW(q_list, LVM_SETCOLUMNWIDTH, 3, MAKELPARAM(c3, 0));
}

// 「加载任务材料」：把 PVF 里所有收集类任务要的物品列出来。
// 数量是「单个任务最多要多少个」—— 同一件材料被几十个任务要，累加没有意义。
// 这个数量跟着行进队列：任务要 30 个，发放列表里就是 30，不用再手动改一遍。
static void query_task_mats(void) {
    if (!g.pvf_ok) {
        app_info("还没加载 PVF。\n\n任务材料是从任务文件里读的，得先选好 Script.pvf。");
        return;
    }
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));   // 要翻几千个任务文件
    QuestMat *v = NULL;
    int n = pvf_quest_materials(&g.pvf, &v);
    SetCursor(old);

    ui_rows_clear(q_list);
    ui_batch(q_list, 1);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = pvf_item_name(&g.pvf, v[i].id);
        if (!nm || !*nm) continue;      // 任务里引用了但 PVF 没这件东西，跳过
        char ids[16], cnt[16], note[64];
        fmt(ids, sizeof(ids), "%u", v[i].id);
        fmt(cnt, sizeof(cnt), "%u", v[i].cnt);
        fmt(note, sizeof(note), "%u 个任务要用到它", v[i].quests);
        const char *cells[4] = {ids, nm, cnt, note};
        ui_row_add(q_list, cells, 4, QROW(v[i].id, v[i].cnt));
        shown++;
    }
    ui_batch(q_list, 0);
    q_fit_cols();
    free(v);
    ui_text(q_info, fmt_new("任务材料 %d 条 · 数量随行进队列", shown));
    app_status("任务材料：%d 种", shown);
}

// 「全部加入」：把列表里所有行灌进发放队列，已在队列里的不重复加行、只把数量补大。
static void query_add_all(void) {
    int n = (int)SendMessageW(q_list, LVM_GETITEMCOUNT, 0, 0);
    if (n <= 0) {
        app_status("列表是空的，先查一批出来");
        return;
    }
    int add = 0, upd = 0, dup = 0;
    for (int i = 0; i < n; i++) {
        LPARAM v = ui_row_data(q_list, i);
        u32 id = QROW_ID(v);
        if (!id) continue;
        int r = app_grant_add_new(id, QROW_NUM(v));
        if (r == 1) add++;
        else if (r == 2) upd++;
        else dup++;
    }
    ui_pages_reload(PAGE_GRANT);
    if (upd) app_status("已加入发放队列 %d 项 · 数量补大 %d 项 · 已有 %d 项", add, upd, dup);
    else app_status("已加入发放队列 %d 项 · 队列里已有 %d 项", add, dup);
}

static void query_reload(Page *p) {
    (void)p;
    if (!g.pvf_ok) {
        ui_text(q_info, "未加载 PVF，无法按名字查询；先选 PVF 或用「发放」页按编号发。");
        return;
    }
    ui_text(q_info, fmt_new("PVF 共 %d 条物品名", pvf_item_count(&g.pvf)));
}

static void query_do(void) {
    if (!g.pvf_ok) {
        app_info("请先加载 PVF。");
        return;
    }
    char kw[64] = "";
    ui_get(q_kw, kw, sizeof(kw));
    str_trim(kw);
    if (!kw[0]) return;

    ui_rows_clear(q_list);
    ui_batch(q_list, 1);
    int hit = 0;
    int total = pvf_item_count(&g.pvf);
    for (int i = 0; i < total && hit < 800; i++) {
        const char *nm = "";
        u32 id = pvf_item_at(&g.pvf, i, &nm);
        if (!*nm) continue;
        int match = 0;
        for (const char *s = nm; *s && !match; s++) {
            const char *a = s, *b = kw;
            while (*a && *b && (unsigned char)*a == (unsigned char)*b) { a++; b++; }
            if (!*b) match = 1;
        }
        if (!match) continue;
        char ids[16];
        fmt(ids, sizeof(ids), "%u", id);
        const char *cells[4] = {ids, nm, "1", "双击加入发放队列"};
        ui_row_add(q_list, cells, 4, QROW(id, 1));
        hit++;
    }
    ui_batch(q_list, 0);
    q_fit_cols();
    ui_text(q_info, fmt_new("命中 %d 条（最多显示 800）", hit));
}

static void query_add(void) {
    int row = ui_row_sel(q_list);
    if (row < 0) return;
    LPARAM v = ui_row_data(q_list, row);
    app_grant_add(QROW_ID(v), QROW_NUM(v));   // 任务材料带着「需要 30 个」一起进队列
    ui_pages_reload(PAGE_GRANT);
}

static LRESULT query_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    if (m == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == Q_GO) { query_do(); return 0; }
        if (id == Q_ADD) { query_add(); return 0; }
        if (id == Q_ALL) { query_add_all(); return 0; }
        // 常用物品按钮：把清单填进结果列表（每次先清空），看中了再双击进队列
        if (id == Q_MAT) { common_list(kCommonMat, sizeof(kCommonMat), "常用材料"); return 0; }
        if (id == Q_EQU) { common_list(kCommonEquip, sizeof(kCommonEquip), "常用装备"); return 0; }
        if (id == Q_AVA) { common_list(kCommonAvatar, sizeof(kCommonAvatar), "常用时装"); return 0; }
        if (id == Q_TASK) { query_task_mats(); return 0; }
        if (id == Q_KW && HIWORD(wp) == EN_CHANGE) return 0;
    }
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == Q_LIST && nh->code == NM_DBLCLK) {
            query_add();
            return 0;
        }
    }
    return -1;
}

static void query_layout(Page *p, int w, int h) {
    (void)h;
    int pad = ui_scale(6), rowh = ui_scale(23), g = ui_scale(6);
    URow r;
    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "物品名"), ui_scale(44));
    ui_add(&r, q_kw, ui_scale(180));
    ui_add(&r, ui_btn_at(p->wnd, Q_GO, "搜索"), ui_scale(60));
    ui_add(&r, ui_btn_at(p->wnd, Q_ADD, "加入发放"), ui_scale(80));
    ui_add(&r, ui_btn_at(p->wnd, Q_ALL, "全部加入"), ui_scale(80));
    ui_add(&r, q_info, -1);

    // 第二行：四个批量抽屉。放在查询页是因为「先查后发」是这条链路的起点 ——
    // 查不到的东西不会有事，灌进去的都是 PVF 里真有的。
    // 「双击某行加入队列」这类说明不占版面了，挂在列表和按钮的提示里。
    int y = pad + rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_btn_at(p->wnd, Q_MAT, "加载常用材料"), ui_scale(100));
    ui_add(&r, ui_btn_at(p->wnd, Q_EQU, "加载常用装备"), ui_scale(100));
    ui_add(&r, ui_btn_at(p->wnd, Q_AVA, "加载常用时装"), ui_scale(100));
    ui_add(&r, ui_btn_at(p->wnd, Q_TASK, "加载任务材料"), ui_scale(100));

    int ly = y + rowh + g;                    // 按钮和列表之间断一刀
    ui_place(q_sep, pad, ly, w - pad * 2, ui_scale(2));
    ui_fill_rest(p->wnd, q_list, ly + ui_scale(8));
    q_fit_cols();
}

static void query_lock(Page *p) {
    (void)p;
    ui_enable(q_kw, g.pvf_ok);
    ui_enable(GetDlgItem(p->wnd, Q_GO), g.pvf_ok);
    ui_enable(GetDlgItem(p->wnd, Q_ADD), g.pvf_ok && g.char_ok);
    ui_enable(GetDlgItem(p->wnd, Q_ALL), g.pvf_ok && g.char_ok);
    // 加载类按钮只往列表里填东西，不碰队列，所以只要有 PVF 就够
    ui_enable(GetDlgItem(p->wnd, Q_MAT), g.pvf_ok);
    ui_enable(GetDlgItem(p->wnd, Q_EQU), g.pvf_ok);
    ui_enable(GetDlgItem(p->wnd, Q_AVA), g.pvf_ok);
    ui_enable(GetDlgItem(p->wnd, Q_TASK), g.pvf_ok);
}

static void query_build(Page *p, HWND parent) {
    q_kw = ui_edit(parent, "", Q_KW, 0);
    q_list = ui_list(parent, Q_LIST);
    q_info = ui_label(parent, "未加载 PVF", Q_INFO);
    q_sep = ui_hline(parent);
    ui_cols(q_list, (const char *const[]){"编号", "名称", "数量", "说明"},
            (const int[]){80, 300, 60, 520}, 4);
    ui_button(parent, "搜索", Q_GO);
    ui_button(parent, "加入发放", Q_ADD);
    ui_button(parent, "全部加入", Q_ALL);
    // 创建顺序就是 Tab 顺序，所以紧跟在这一行后面
    ui_button(parent, "加载常用材料", Q_MAT);
    ui_button(parent, "加载常用装备", Q_EQU);
    ui_button(parent, "加载常用时装", Q_AVA);
    ui_button(parent, "加载任务材料", Q_TASK);
    ui_tip(parent, q_list, "列出当前 PVF 里真有的物品；双击某行等于「加入发放」，"
                           "「数量」列（任务材料要的个数）一起进队列");
    ui_tip(parent, GetDlgItem(parent, Q_GO), "按物品名或编号在当前 PVF 里模糊查，最多列 800 条");
    ui_tip(parent, GetDlgItem(parent, Q_ADD), "把选中的那一行加入发放队列，数量按「数量」列");
    ui_tip(parent, GetDlgItem(parent, Q_ALL), "把列表里所有行加进发放队列；队列里已有的不重复加行，只把数量补大");
    ui_tip(parent, GetDlgItem(parent, Q_MAT), "内置的常用材料编号，和当前 PVF 比对后只列真有的");
    ui_tip(parent, GetDlgItem(parent, Q_EQU), "内置的常用装备编号，和当前 PVF 比对后只列真有的");
    ui_tip(parent, GetDlgItem(parent, Q_AVA), "内置的常用时装编号，和当前 PVF 比对后只列真有的");
    ui_tip(parent, GetDlgItem(parent, Q_TASK), "扫 PVF 里所有收集类任务，列出它们要的材料和个数");
}

void page_query_create(Page *p) {
    p->build = query_build;
    p->layout = query_layout;
    p->reload = query_reload;
    p->lock = query_lock;
    p->on = query_on;
}

// ================================================================ 发放页
enum {
    G_ID = PAGE_ID(PAGE_GRANT, 1), G_NUM, G_TYPE, G_ENH, G_INC, G_INCV, G_FORGE,
    G_SEAL, G_SEALC, G_DUR, G_ADD, G_LIST, G_DEL, G_CLR, G_MAIL, G_INV, G_INFO,
    G_EXPORT, G_IMPORT, G_GAUTO
};

static HWND g_id, g_num, g_type, g_enh, g_inc, g_incv, g_forge, g_seal, g_sealc, g_dur, g_list, g_info;
static HWND g_gauto;   // 连续取码开关
static HWND g_sep;     // 表单区 / 队列区之间的分隔线
static int g_sync;      // 重填队列时屏蔽 LVN_ITEMCHANGED
static int g_bind = -1; // 表单当前绑定的队列行；-1 = 未绑定（只填表单，不写回）
static u8  s_gtype = IT_STACK;   // 当前编号对应的物品类型字节（由 PVF 决定，见 grant_type_sync）

// ---------------- 类型：从 PVF 来，没 PVF 也有默认值 ----------------
// 类型字节决定这件东西在库里算哪一类（装备 1 / 消耗品 2 / 材料 3 / 任务材料 4 …），
// 填错的后果是这一格按另一套规则解析，轻则显示成别的东西，重则整格报错。
// 所以这一格只读，用户能改的只有编号；PVF 不在时兜成消耗品，不阻塞发放。
static void grant_sync(void) {
    int is_eq = (s_gtype == IT_EQUIP || s_gtype == IT_CREATURE_EQUIP);
    ui_enable(g_dur, is_eq);      // 装备看耐久
    ui_enable(g_num, !is_eq);     // 堆叠物看数量
    ui_enable(g_sealc, SendMessageW(g_seal, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

// 类型那格跟着编号走：编号一变就重新判一次，并顺手把数量/耐久的可用状态拨对。
static void grant_type_sync(void) {
    u32 id = (u32)ui_int(g_id);
    // 没 PVF 也照样出结果：app_item_type 兜的是消耗品，不会给 0，
    // 所以不存在「这一格空着、东西发不出去」的状态
    s_gtype = app_item_type(id);
    ui_text(g_type, app_item_type_text(id));
    grant_sync();
}

static void grant_form_get(NamedId *v) {
    memset(v, 0, sizeof(*v));
    v->id = (u32)ui_int(g_id);
    v->num = (u32)(ui_int(g_num) > 0 ? ui_int(g_num) : 1);
    // 类型每次都现判一遍，不信界面上那格缓存：切角色/换 PVF 之后旧值就过期了
    v->type = app_item_type(v->id);
    v->enhance = (u8)ui_int(g_enh);
    v->inc_type = (u8)(ui_combo_sel(g_inc) < 0 ? 0 : ui_combo_sel(g_inc));
    v->inc_val = (u16)ui_int(g_incv);
    v->forge = (u8)ui_int(g_forge);
    v->seal = SendMessageW(g_seal, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    v->seal_cnt = (u8)ui_int(g_sealc);
    v->dur = (u16)ui_int(g_dur);
    char nb[96];
    snprintf(v->name, sizeof(v->name), "%s", app_item_name(v->id, nb, sizeof(nb)));
}

static void grant_form_set(const NamedId *v) {
    ui_set_int(g_id, (int)v->id);
    ui_set_int(g_num, (int)(v->num ? v->num : 1));
    ui_set_int(g_enh, v->enhance);
    ui_combo_set(g_inc, v->inc_type > 4 ? 0 : v->inc_type);
    ui_set_int(g_incv, v->inc_val);
    ui_set_int(g_forge, v->forge);
    SendMessageW(g_seal, BM_SETCHECK, v->seal ? BST_CHECKED : BST_UNCHECKED, 0);
    ui_set_int(g_sealc, v->seal_cnt);
    ui_set_int(g_dur, v->dur);
    grant_type_sync();
}

// 一行的显示文本：整表重填与单行刷新共用，保证两处长得一样
typedef struct {
    char id[16], num[16], enh[16], inc[32], forge[16], seal[24], dur[16];
} GrantCells;

static void grant_row_cells(const NamedId *n, GrantCells *b, const char **cells) {
    fmt(b->id, sizeof(b->id), "%u", n->id);
    fmt(b->num, sizeof(b->num), "%u", n->num);
    fmt(b->enh, sizeof(b->enh), n->enhance ? "+%d" : "", n->enhance);
    if (n->inc_type > 0 && n->inc_type <= 4)
        fmt(b->inc, sizeof(b->inc), "%s +%u", kIncList[n->inc_type], n->inc_val);
    else b->inc[0] = 0;
    fmt(b->forge, sizeof(b->forge), n->forge ? "+%d" : "", n->forge);
    if (n->seal) fmt(b->seal, sizeof(b->seal), "封装 %d", n->seal_cnt);
    else snprintf(b->seal, sizeof(b->seal), "未封装");
    fmt(b->dur, sizeof(b->dur), "%u", n->dur);
    const char *c8[8] = {n->name, b->id, b->num, b->enh, b->inc, b->forge, b->seal, b->dur};
    memcpy(cells, c8, sizeof(c8));
}

// 表单失焦/切行时把内容写回绑定的行；没绑行、或表单没填编号，就不动
static void grant_commit(void) {
    if (g_bind < 0 || g_bind >= g.grant.n) { g_bind = -1; return; }
    NamedId v;
    grant_form_get(&v);
    if (!v.id) return;
    NamedId *n = VEC_AT(&g.grant, NamedId, g_bind);
    if (v.id == n->id && v.num == n->num && v.type == n->type && v.enhance == n->enhance &&
        v.inc_type == n->inc_type && v.inc_val == n->inc_val && v.forge == n->forge &&
        v.seal == n->seal && v.seal_cnt == n->seal_cnt && v.dur == n->dur) return;
    *n = v;
    GrantCells b;
    const char *cells[8];
    grant_row_cells(n, &b, cells);
    ui_row_set(g_list, g_bind, cells, 8);   // 只刷这一行，选中不丢
    app_status("已写回队列第 %d 项：%s ×%u", g_bind + 1, v.name, v.num);
}

// 焦点正在落到「加入队列」上：表单是要入队的新数据，不是旧行的改动，
// 这时候写回会把旧行改成新物品。其余焦点转移（列表/空白/其他控件）照常写回。
static int focus_is_add(void) {
    HWND nf = GetFocus();
    return nf && GetDlgCtrlID(nf) == G_ADD;
}

static void grant_reload(Page *p) {
    (void)p;
    // 切页签/切角色也会走到这里：表单里没写回的改动先落回旧行，别让用户白改
    // （add/del/clr/发送等路径在此之前 g_bind 已是 -1 或内容一致，这里是空操作）
    grant_commit();
    // 队列长度没变（写库完成、别的页面动作完成这类「无关刷新」）就把绑定和选中保下来：
    // 用户在右边表单改到一半、或列表里选着某行时，不该因为别处写了一次库就跳回未绑定。
    // 长度变了（加入/删除/清空）说明队列本身在动，交给调用方重新指定。
    int keep = g_bind, n0 = (int)g.grant.n;
    g_bind = -1;                    // 整表重填后没有选中行，表单回到未绑定
    g_sync = 1;
    ui_rows_clear(g_list);
    ui_batch(g_list, 1);
    for (int i = 0; i < g.grant.n; i++) {
        NamedId *n = VEC_AT(&g.grant, NamedId, i);
        GrantCells b;
        const char *cells[8];
        grant_row_cells(n, &b, cells);
        ui_row_add(g_list, cells, 8, (LPARAM)i);
    }
    ui_batch(g_list, 0);
    // 选中恢复放在 g_sync 打开的时候：选中的行会发 LVN_ITEMCHANGED，
    // 那种情况下会走 grant_load_sel 把整张表单回灌一遍，正在输入的光标就没了
    if (keep >= 0 && keep < (int)g.grant.n && n0 == (int)g.grant.n) {
        g_bind = keep;
        ui_row_sel_set(g_list, keep);
    }
    g_sync = 0;
    // PVF 是异步加载的：编号早就填好、那时只能显示「未加载」，加载完这里跟着补上
    grant_type_sync();
    if (!g.char_ok) ui_text(g_info, "请先选择角色");
    else ui_text(g_info, fmt_new("目标：%s · 队列 %d 项 · 点行载入表单，改完点别处自动写回",
                                 g.cur.name, g.grant.n));
}

static void grant_add_manual(void) {
    NamedId v;
    grant_form_get(&v);
    if (!v.id) { app_info("请填写物品编号。"); return; }
    NamedId *n = VEC_ADD(&g.grant, NamedId);
    *n = v;
    app_status("已加入队列：%s ×%u", n->name, n->num);
    g_bind = -1;                     // 表单内容已作为新行入队，旧绑定作废，别把新物品写回旧行
    grant_reload(NULL);
    int last = (int)g.grant.n - 1;
    if (last >= 0) {                 // 选中新入队的行，接着改表单就是改它
        ui_row_sel_set(g_list, last);
        g_bind = last;
    }
}

// ---- 常用物品的内置编号表 → 逐条查 PVF 的逻辑在查询页那一段 ----
// 表在 items_data.h（tools/gen_items.py 生成）。只存编号不存名字，有两个好处：
// 体积小（663 个编号 689 字节），而且名字是显示时从 PVF 取的，不会出现 ini 里
// 的名字和实际 PVF 对不上（比如 8387 手写成「天空礼盒」，PVF 里叫「稀有装扮自选礼盒」）。
// 查不到名字 = 当前这版 PVF 里根本没这件东西，正好跳过。

static void grant_load_sel(void) {
    // 先把表单里没写回的改动落回旧绑定行，再换行/解绑：
    // 点另一行时若列表先发 LVN_ITEMCHANGED 后转移焦点，编辑内容会被新行数据冲掉；
    // 点列表空白取消选中时 likewise。commit 幂等（无改动直接返回），
    // 且所有 grant_reload 路径都会先把 g_bind 置 -1，这里不会误写。
    grant_commit();
    int row = ui_row_sel(g_list);
    if (row < 0 || row >= g.grant.n) { g_bind = -1; return; }
    grant_form_set(VEC_AT(&g.grant, NamedId, row));
    g_bind = row;
}

// ---- 队列导出 / 导入：TSV，列名可读，人能直接拿记事本改 ----
// 导出时把类型名写出来，是为了让人拿到记事本里能看懂；导入时这一列不看 ——
// 类型只认 PVF，写上去也改不了。
static int inc_from_text(const char *s) {
    for (int i = 0; i < (int)COUNT_OF(kIncList); i++)
        if (strcmp(s, kIncList[i]) == 0) return i;
    return atoi(s);
}

// config\export 下最近一次的队列文件（导入时预填，回车即用）
static void newest_grant(char *out, int cap) {
    char dir[MAX_PATH], pat[MAX_PATH];
    art_dir("export", dir, sizeof(dir));
    snprintf(pat, sizeof(pat), "%s\\grant-*.tsv", dir);
    WCHAR wp[MAX_PATH];
    utf16(pat, wp, MAX_PATH);
    out[0] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME best = fd.ftLastWriteTime;
    char nm[MAX_PATH];
    utf8(fd.cFileName, nm, sizeof(nm));
    snprintf(out, cap, "%s\\%s", dir, nm);
    while (FindNextFileW(h, &fd)) {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) <= 0) continue;
        best = fd.ftLastWriteTime;
        utf8(fd.cFileName, nm, sizeof(nm));
        snprintf(out, cap, "%s\\%s", dir, nm);
    }
    FindClose(h);
}

static void grant_export(void) {
    if (g.grant.n <= 0) { app_info("队列是空的。"); return; }
    char dir[MAX_PATH], ts[32];
    art_dir("export", dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    WCHAR wdir[MAX_PATH], file[MAX_PATH];
    utf16(dir, wdir, MAX_PATH);
    WCHAR wname[64];
    utf16(fmt_new("grant-%s.tsv", ts), wname, COUNT_OF(wname));
    wcscpy(file, wname);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"发放列表 (*.tsv)\0*.tsv\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = wdir;      // 默认落在 config\export，和备份分开放
    ofn.lpstrDefExt = L"tsv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    char path[MAX_PATH];
    utf8(file, path, sizeof(path));

    Str s = {0};
    str_cat(&s, "编号\t名称\t数量\t类型\t强化\t增幅\t增幅值\t锻造\t封装\t封印次数\t耐久\r\n");
    for (int i = 0; i < g.grant.n; i++) {
        NamedId *n = VEC_AT(&g.grant, NamedId, i);
        // 类型名按编号现查 PVF，队列里存的字节只作兜底
        const char *tn = app_item_type_text(n->id);
        const char *in = n->inc_type <= 4 ? kIncList[n->inc_type] : kIncList[0];
        str_catf(&s, "%u\t%s\t%u\t%s\t%d\t%s\t%u\t%d\t%s\t%d\t%u\r\n", n->id, n->name, n->num, tn,
                 n->enhance, in, n->inc_val, n->forge, n->seal ? "是" : "否", n->seal_cnt, n->dur);
    }
    int ok = file_write(path, s.p, s.n);
    str_free(&s);
    if (!ok) app_info("写入失败。");
    else {
        art_prune(dir, "grant-", 10);      // 只留最近 10 份导出的队列
        app_status("已导出 %d 项 → %s", g.grant.n, path);
    }
}

// 取一行里第 idx 个制表符分段
static int fld(const u8 *line, int len, int idx, char *out, int cap) {
    int f = 0, st = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || line[i] == '\t') {
            if (f == idx) {
                int k = i - st;
                if (k > cap - 1) k = cap - 1;
                if (k > 0) memcpy(out, line + st, (size_t)k);
                out[k] = 0;
                return 1;
            }
            f++;
            st = i + 1;
        }
    }
    out[0] = 0;
    return 0;
}

static void grant_import(void) {
    WCHAR file[MAX_PATH] = L"";
    char last[MAX_PATH];
    newest_grant(last, sizeof(last));
    if (last[0]) utf16(last, file, MAX_PATH);   // 预填上次导出的文件，回车即导入
    char edir[MAX_PATH];
    art_dir("export", edir, sizeof(edir));
    WCHAR wdir[MAX_PATH];
    utf16(edir, wdir, MAX_PATH);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"发放列表 (*.tsv)\0*.tsv\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = wdir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    char path[MAX_PATH];
    utf8(file, path, sizeof(path));

    Buf b = {0};
    if (!file_read(path, &b)) { app_info("读不到文件。"); return; }

    int added = 0;
    for (int i = 0; i < (int)b.n;) {
        int st = i;
        while (i < (int)b.n && b.p[i] != '\n') i++;
        int len = i - st;
        i++;
        if (len > 0 && b.p[st + len - 1] == '\r') len--;
        if (len <= 0 || b.p[st] == '#') continue;
        if (len >= 6 && memcmp(b.p + st, "编号", 6) == 0) continue;   // 跳过表头

        char f0[32], f1[80], f2[16], f3[32], f4[16], f5[16], f6[16], f7[16], f8[8], f9[16], f10[16];
        fld(b.p + st, len, 0, f0, sizeof(f0));
        fld(b.p + st, len, 1, f1, sizeof(f1));
        fld(b.p + st, len, 2, f2, sizeof(f2));
        fld(b.p + st, len, 3, f3, sizeof(f3));
        fld(b.p + st, len, 4, f4, sizeof(f4));
        fld(b.p + st, len, 5, f5, sizeof(f5));
        fld(b.p + st, len, 6, f6, sizeof(f6));
        fld(b.p + st, len, 7, f7, sizeof(f7));
        fld(b.p + st, len, 8, f8, sizeof(f8));
        fld(b.p + st, len, 9, f9, sizeof(f9));
        fld(b.p + st, len, 10, f10, sizeof(f10));

        u32 id = (u32)atol(f0);
        if (!id) continue;
        NamedId v;
        memset(&v, 0, sizeof(v));
        v.id = id;
        v.num = (u32)(atoi(f2) > 0 ? atoi(f2) : 1);
        v.type = app_item_type(id);              // 第 4 列（类型）只导出给人看，不回读
        v.enhance = (u8)atoi(f4);
        int inc = inc_from_text(f5);
        v.inc_type = (u8)(inc < 0 ? 0 : inc);
        v.inc_val = (u16)atoi(f6);
        v.forge = (u8)atoi(f7);
        v.seal = (strcmp(f8, "是") == 0 || atoi(f8) > 0) ? 1 : 0;
        v.seal_cnt = (u8)atoi(f9);
        v.dur = (u16)atoi(f10);
        if (f1[0]) snprintf(v.name, sizeof(v.name), "%s", f1);
        else {
            char nb[96];
            snprintf(v.name, sizeof(v.name), "%s", app_item_name(id, nb, sizeof(nb)));
        }
        *VEC_ADD(&g.grant, NamedId) = v;
        added++;
    }
    buf_free(&b);
    grant_reload(NULL);
    app_status("已导入 %d 项 · 队列共 %d 项", added, g.grant.n);
}

static LRESULT grant_on(Page *p, UINT m, WPARAM wp, LPARAM lp) {
    (void)p;
    // 点页面空白处也算「点别处」：把表单写回绑定的行
    if (m == WM_LBUTTONDOWN) {
        grant_commit();
        return 0;
    }
    if (m == WM_COMMAND) {
        int id = LOWORD(wp), code = HIWORD(wp);
        // 表单失焦即写回绑定的行：点空白处、切控件、点按钮，都算「点别处」
        // （「加入队列」例外：那一下表单是当新数据入队的，不能写回旧行）
        if (code == EN_KILLFOCUS && !focus_is_add() &&
            (id == G_ID || id == G_NUM || id == G_ENH || id == G_INCV ||
             id == G_FORGE || id == G_SEALC || id == G_DUR)) {
            grant_commit();
            return 0;
        }
        // 编号边敲边更新「类型」那格：类型只能跟着 PVF 走，早一拍显示也是真值
        if (id == G_ID && code == EN_CHANGE) {
            grant_type_sync();
            return 0;
        }
        if (id == G_INC && code == CBN_KILLFOCUS && !focus_is_add()) {
            grant_commit();
            return 0;
        }
        if (id == G_SEAL && code == BN_KILLFOCUS && !focus_is_add()) {
            grant_commit();
            return 0;
        }
        switch (id) {
            case G_ADD: grant_add_manual(); return 0;
            case G_SEAL:
                grant_sync();
                return 0;
            case G_DEL: {
                int row = ui_row_sel(g_list);
                if (row >= 0 && row < g.grant.n) {
                    memmove(vec_at(&g.grant, row), vec_at(&g.grant, row + 1),
                            sizeof(NamedId) * (size_t)(g.grant.n - row - 1));
                    g.grant.n--;
                    if (g_bind == row) g_bind = -1;
                    else if (g_bind > row) g_bind--;
                    grant_reload(NULL);
                    int sel = row < (int)g.grant.n ? row : (int)g.grant.n - 1;
                    if (sel >= 0) {           // 选回相邻的一行，表单接着编辑
                        ui_row_sel_set(g_list, sel);
                        g_bind = sel;
                    }
                }
                return 0;
            }
            case G_CLR:
                vec_clear(&g.grant);
                g_bind = -1;
                grant_reload(NULL);
                return 0;
            case G_MAIL:
                g.lists_ready &= ~(u32)L_MAILS;   // 发了新邮件，邮件页重新去库里取
                app_grant_send(1);
                grant_reload(NULL);
                return 0;
            case G_INV: app_grant_send(0); grant_reload(NULL); return 0;
            case G_GAUTO:
                // 复选框自己就是新状态，直接交给 app 层落盘并驱动轮询
                app_grab_auto(SendMessageW(g_gauto, BM_GETCHECK, 0, 0) == BST_CHECKED);
                return 0;
            case G_EXPORT: grant_export(); return 0;
            case G_IMPORT: grant_import(); return 0;
        }
    }
    if (m == WM_NOTIFY) {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == G_LIST) {
            // 单击（选中变化）就把这一项载入表单，不用双击
            if (nh->code == LVN_ITEMCHANGED && !g_sync) grant_load_sel();
            return 0;
        }
    }
    return -1;
}

static void grant_layout(Page *p, int w, int h) {
    (void)w;
    (void)h;
    RECT rc;
    GetClientRect(p->wnd, &rc);
    int pad = ui_scale(6), rowh = ui_scale(23), g = ui_scale(5);
    URow r;

    ui_row(&r, p->wnd, pad, pad, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "编号"), ui_scale(34));
    ui_add(&r, g_id, ui_scale(88));
    ui_add(&r, ui_lbl_at(p->wnd, "类型"), ui_scale(34));
    ui_add(&r, g_type, ui_scale(88));
    ui_add(&r, ui_lbl_at(p->wnd, "数量"), ui_scale(34));
    ui_add(&r, g_num, ui_scale(56));
    ui_add(&r, ui_lbl_at(p->wnd, "耐久"), ui_scale(34));
    ui_add(&r, g_dur, ui_scale(56));

    int y = pad + rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_lbl_at(p->wnd, "强化"), ui_scale(34));
    ui_add(&r, g_enh, ui_scale(44));
    ui_add(&r, ui_lbl_at(p->wnd, "增幅"), ui_scale(34));
    ui_add(&r, g_inc, ui_scale(76));
    ui_add(&r, ui_lbl_at(p->wnd, "值"), ui_scale(18));
    ui_add(&r, g_incv, ui_scale(44));
    ui_add(&r, ui_lbl_at(p->wnd, "锻造"), ui_scale(34));
    ui_add(&r, g_forge, ui_scale(44));
    ui_add(&r, g_seal, ui_scale(52));
    ui_add(&r, ui_lbl_at(p->wnd, "次数"), ui_scale(34));
    ui_add(&r, g_sealc, ui_scale(40));

    y += rowh + g;
    ui_row(&r, p->wnd, pad, y, rowh);
    ui_add(&r, ui_btn_at(p->wnd, G_ADD, "加入队列"), ui_scale(88));
    ui_add(&r, g_gauto, ui_scale(88));
    ui_add(&r, ui_btn_at(p->wnd, G_DEL, "移除所选"), ui_scale(84));
    ui_add(&r, ui_btn_at(p->wnd, G_CLR, "清空队列"), ui_scale(84));
    ui_add(&r, ui_btn_at(p->wnd, G_EXPORT, "导出"), ui_scale(56));
    ui_add(&r, ui_btn_at(p->wnd, G_IMPORT, "导入"), ui_scale(56));

    int ly = y + rowh + g;                   // 按钮行下面画一条线，把表单和队列断开
    ui_place(g_sep, pad, ly, rc.right - pad * 2, ui_scale(2));
    int listtop = ly + ui_scale(8);
    int by = rc.bottom - pad - rowh;
    MoveWindow(g_list, pad, listtop, rc.right - pad * 2, by - g - listtop, TRUE);
    int bw = ui_scale(96);
    ui_place(GetDlgItem(p->wnd, G_MAIL), rc.right - pad - bw * 2 - ui_scale(8), by, bw, rowh);
    ui_place(GetDlgItem(p->wnd, G_INV), rc.right - pad - bw, by, bw, rowh);
    ui_place(g_info, pad, by, rc.right - pad * 2 - bw * 2 - ui_scale(16), rowh);
}

static void grant_lock(Page *p) {
    (void)p;
    int on = g.char_ok;
    int ids[] = {G_ID, G_NUM, G_ENH, G_INC, G_INCV, G_FORGE, G_SEAL, G_SEALC, G_DUR,
                 G_ADD, G_DEL, G_CLR, G_MAIL, G_INV, G_EXPORT, G_IMPORT};
    // 类型那格是只读的，跟着物品走：没选角色时它也没内容可显示，和其它字段一起灰掉
    ui_enable(g_type, on);
    for (int i = 0; i < COUNT_OF(ids); i++) ui_enable(GetDlgItem(p->wnd, ids[i]), on);
    if (on) grant_sync();
    else grant_type_sync();          // 灰掉时也填默认值，别留着上一件物品的类型
}

static void grant_build(Page *p, HWND parent) {
    // 创建顺序就是 Tab 顺序：这一行照视觉排（编号 → 类型 → 数量 → 强化 …），
    // g_num 原来建在 g_type 前面，Tab 会从编号直接跳到数量再绕回类型。
    // 类型做成只读框：内容按编号从 PVF 判出来的，用户改不了，改编号就行。
    // 仍然保留 WS_TABSTOP（ui_edit 的 ro=0），Tab 能顺着整行走过去。
    g_id = ui_edit(parent, "", G_ID, 0);
    g_type = ui_edit(parent, "", G_TYPE, 0);
    SendMessageW(g_type, EM_SETREADONLY, TRUE, 0);
    grant_type_sync();                 // 建好就填默认值，不等用户敲编号
    g_num = ui_edit(parent, "1", G_NUM, 0);
    g_dur = ui_edit(parent, "0", G_DUR, 0);   // 耐久在第一行末尾，跟着建
    g_enh = ui_edit(parent, "0", G_ENH, 0);
    g_inc = ui_combo(parent, G_INC, 1);
    ui_combo_fill(g_inc, kIncList, COUNT_OF(kIncList), 0);
    g_incv = ui_edit(parent, "0", G_INCV, 0);
    g_forge = ui_edit(parent, "0", G_FORGE, 0);
    g_seal = ui_check(parent, "封装", G_SEAL, 0);
    g_sealc = ui_edit(parent, "0", G_SEALC, 0);
    // 每个字段按它写回物品结构时的宽度收窄：强化只有 5 位、封印次数 3 位，
    // 多填的部分原本会被 & 0x1F 悄悄截掉，用户以为设成了 50。
    ui_num(g_id, 1, 4294967295LL);
    ui_num(g_num, 1, 4294967295LL);
    ui_num(g_dur, 0, 65535);
    ui_num(g_enh, 0, 31);
    ui_num(g_incv, 0, 65535);
    ui_num(g_forge, 0, 255);
    ui_num(g_sealc, 0, 7);
    g_info = ui_label(parent, "请先选择角色", G_INFO);
    g_sep = ui_hline(parent);
    ui_button(parent, "加入队列", G_ADD);
    g_gauto = ui_check(parent, "连续取码", G_GAUTO, g.cfg.grab_auto);
    ui_button(parent, "移除所选", G_DEL);
    ui_button(parent, "清空队列", G_CLR);
    // 创建顺序就是 Tab 顺序：这一行照视觉排（…清空队列→导出→导入）
    ui_button(parent, "导出", G_EXPORT);
    ui_button(parent, "导入", G_IMPORT);
    // 列表建在按钮行后面；底部那一对（邮件发放/直接入包）建在最后，视觉上也在最下面
    g_list = ui_list(parent, G_LIST);
    ui_cols(g_list, (const char *const[]){"物品名", "编号", "数量", "强化", "增幅", "锻造", "封装", "耐久"},
            (const int[]){240, 84, 60, 50, 90, 50, 70, 56}, 8);
    ui_button(parent, "邮件发放", G_MAIL);
    ui_button(parent, "直接入包", G_INV);
    // 按钮名短，含义挂在悬浮提示上
    ui_tip(parent, GetDlgItem(parent, G_ADD),
           "把表单里的物品作为新的一项追加到队列；点了某行之后改表单会自动写回该行，点此按钮则把当前表单当新项入队");
    ui_tip(parent, g_id, "物品编号。类型格会跟着这个编号自动判，不用自己选");
    ui_tip(parent, g_type,
           "只读。按编号从 PVF 里查目录判出来的：equipment/ → 装备、avatar/ → 时装、"
           "stackable/quest → 任务材料、creature/ → 宠物，其余堆叠物算消耗品。"
           "没加载 PVF 或编号查不到时兜成「消耗品(默认)」，东西照样能发出去");
    ui_tip(parent, g_gauto,
           "默认开启。开启后持续读取游戏里鼠标悬停的物品，每取到一件就整行追加到队列；队列里已有的编号自动跳过。"
           "开着工具才算，关掉不补。热键可以随时开关");
    ui_tip(parent, GetDlgItem(parent, G_DEL), "从队列里删掉当前选中的那一行");
    ui_tip(parent, GetDlgItem(parent, G_CLR), "清空整个队列");
    ui_tip(parent, GetDlgItem(parent, G_EXPORT), "把队列导出成 config 下的 .tsv，可用记事本改");
    ui_tip(parent, GetDlgItem(parent, G_IMPORT), "从 .tsv 导入队列（默认预填上次导出的文件）");
    ui_tip(parent, GetDlgItem(parent, G_MAIL), "把队列里的物品以邮件附件发给目标角色（≤10 件/封）");
    ui_tip(parent, GetDlgItem(parent, G_INV), "把队列里的物品直接写入背包空槽，要求角色离线");
    ui_tip(parent, g_list, "单击某行把它的配置载回上方表单；改完点到别处就自动写回该行。"
                           "要从头建一件新物品，点「加入队列」，用的是表单当前的值");
    (void)p;
}

// 取码回填：只在发放页已建好时写编号
void page_grant_setid(u32 id) {
    if (!g_id || !id) return;
    ui_set_int(g_id, (int)id);
}

// 取码命中：整行入队，不是只填编号。同编号静默跳过：连续取码时鼠标在同一件
// 物品上会停好几拍（每 250ms 采到一次），不去重队列里就会灌进一堆重复行，
// 也不值得每次都弹状态栏——换到新物品时的「取到物品」提示就是反馈。
// 想要同编号不同配置，照旧用「加入队列」手动加（手动这条路不去重）。
void page_grant_autoadd(const GrabHit *h) {
    if (!h || !h->id || !g_list) return;      // 页没建过就没地方放，宁可不收
    for (int i = 0; i < (int)g.grant.n; i++) {
        NamedId *e = VEC_AT(&g.grant, NamedId, i);
        if (e->id == h->id) return;
    }
    // 数量/强化/增幅这些配置以表单为准：表单就是"下一件入队物品的模板"，
    // 想改默认配置先改表单再悬停物品；入队后选中新行，接着改表单就是改它
    NamedId v;
    grant_form_get(&v);
    v.id = h->id;
    snprintf(v.name, sizeof(v.name), "%s", h->name);
    NamedId *n = VEC_ADD(&g.grant, NamedId);
    *n = v;
    g_bind = -1;
    grant_reload(NULL);
    int last = (int)g.grant.n - 1;
    ui_row_sel_set(g_list, last);
    g_bind = last;
    app_status("入队 #%u %s（队列 %d 项）", h->id, h->name, (int)g.grant.n);
    app_log("入队 #%u %s · %s（队列 %d 项）", h->id, h->name, h->type, (int)g.grant.n);
}

// 热键切换开关后，复选框不会自己跟上，这里补同步（页没建就是空操作）
void page_grant_syncauto(void) {
    if (!g_gauto) return;
    int want = g.cfg.grab_auto ? BST_CHECKED : BST_UNCHECKED;
    if ((int)SendMessageW(g_gauto, BM_GETCHECK, 0, 0) != want)
        SendMessageW(g_gauto, BM_SETCHECK, (WPARAM)want, 0);
}

int page_grant_built(void) { return g_list != NULL; }

void page_grant_create(Page *p) {
    p->build = grant_build;
    p->layout = grant_layout;
    p->reload = grant_reload;
    p->lock = grant_lock;
    p->on = grant_on;
}
