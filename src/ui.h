// ui：界面共享声明（控件 ID、页面注册、通用控件封装、布局游标）。
#pragma once
#include "app.h"
#include <commctrl.h>

// ---- 主窗口控件 ----
// 编号是隐式连续的，新控件一律贴到尾部：插在中间会把后面所有编号顶走，
// 而验证脚本和 ui.h 里的常量是写死的（角色列表 = 113 之类），一挪就全错。
enum {
    IDC_HOST = 100, IDC_PORT, IDC_USER, IDC_PWD, IDC_CONN, IDC_DISC,
    IDC_PVFB, IDC_PVFS,
    IDC_KW, IDC_KWGO, IDC_KWCLR, IDC_SAVE, IDC_DISCARD,
    IDC_CHLIST, IDC_ONLINE,
    IDC_GRP_CHARS, IDC_GRP_CARD,
    IDC_IMGB, IDC_IMGS,          // 贴图包目录（后加，放尾部）
    IDC_TAB = 190, IDC_STATUS,
};

// ---- 页签 ----
enum {
    PAGE_ITEM = 0, PAGE_QUERY, PAGE_GRANT, PAGE_MAIL, PAGE_PET, PAGE_AVATAR, PAGE_QUEST,
    PAGE_EVENT, PAGE_GM, PAGE_BUBBLE, PAGE_BAN, PAGE_LOG, PAGE_COUNT
};

// 每页控件 ID 从 1000 + 页号*100 开始
#define PAGE_ID(page, n) (1000 + (page) * 100 + (n))

typedef struct Page Page;
struct Page {
    const char *name;                     // 页签名（UTF-8）
    HWND        wnd;
    int         busy;                     // build/reload 进行中：屏蔽消息转发与重入布局
    void (*build)(Page *p, HWND parent);  // 首次创建子控件
    void (*layout)(Page *p, int w, int h);
    void (*reload)(Page *p);              // 数据变化后刷新
    void (*lock)(Page *p);                // 依就绪状态置灰
    LRESULT (*on)(Page *p, UINT msg, WPARAM wp, LPARAM lp);
};

extern Page g_pages[PAGE_COUNT];
extern HWND g_main;

// ---- 基础 ----
void      ui_init(HINSTANCE inst, int pt);
void      ui_set_dpi(int dpi);
int       ui_dpi(void);
int       ui_scale(int px);
HINSTANCE ui_inst(void);
void      ui_font_reload(int pt);
HFONT     ui_font(void);
HFONT     ui_font_bold(void);
// 用当前界面字体量一行文字的像素宽。布局要「按钮右端对齐到某段文字右端」时
// 必须靠它：label 控件宽度可以随便给，但文字只占前面一段，按控件宽算会差几十像素。
int       ui_text_w(const char *s);

// ---- 配色 ----
// 全界面只有三层颜色：底色（页面与组框内）、白（可编辑区/卡片）、一条浅灰线。
// 惰性创建，RegisterClass 之前调用也安全。
HBRUSH    ui_bg_brush(void);
HBRUSH    ui_card_brush(void);
COLORREF  ui_bg_color(void);
COLORREF  ui_line_color(void);
COLORREF  ui_text_ink(void);
COLORREF  ui_text_dim(void);
LRESULT   ui_ctl_color(WPARAM wp, LPARAM lp);   // WM_CTLCOLORSTATIC/BTN 统一处理

// ---- 控件 ----
HWND ui_make(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, DWORD ex, int id);
HWND ui_label(HWND parent, const char *text, int id);
HWND ui_label_r(HWND parent, const char *text, int id);   // 右对齐
HWND ui_label_b(HWND parent, const char *text, int id);
HWND ui_edit(HWND parent, const char *text, int id, int ro);
HWND ui_button(HWND parent, const char *text, int id);
HWND ui_check(HWND parent, const char *text, int id, int on);
HWND ui_combo(HWND parent, int id, int droplist);
HWND ui_list(HWND parent, int id);
HWND ui_group(HWND parent, const char *text, int id);
HWND ui_btn_at(HWND parent, int id, const char *text);   // 布局期幂等：已存在则复用
HWND ui_lbl_at(HWND parent, const char *text);           // 同上，按文本缓存
void ui_text(HWND w, const char *utf8);
void ui_get(HWND w, char *out, int cap);
int  ui_int(HWND w);
void ui_set_int(HWND w, int v);
i64  ui_i64(HWND w);
void ui_set_i64(HWND w, i64 v);
// 把输入框限制成「只能填 [lo, hi] 的整数」：非数字键、超范围粘贴都进不来，
// 失焦时把值夹回范围并规范化（空框按 lo）。所有数值框建完都要套一遍。
void ui_num(HWND e, i64 lo, i64 hi);
void ui_enable(HWND w, int on);
void ui_show(HWND w, int on);
void ui_place(HWND w, int x, int y, int cw, int ch);

// 悬浮提示：host 每个窗口共用一个 tooltip，ctl 上悬停时显示说明。
// 列表的说明比较特殊：整块悬停都弹会和编号悬浮卡重叠，改成只在列头上弹（见 ui_tip_tick）。
void ui_tip(HWND host, HWND ctl, const char *text);
void ui_tip_tick(void);      // 主窗口定时器里调（ui_idtip_tick 会带上）

// 编号悬浮卡：鼠标停在列表 id_col 列所在行的任一格上 ~0.4s，浮出物品详情 + 图标。
// 详情查 PVF、图标查贴图包，两者谁没就绪就退化成只显示编号。
// 页面/对话框建好列表后调一次即可；列表销毁后绑定在 tick 里自动回收。
void ui_idtip_bind(HWND lv, int id_col);                 // 编号从该列文本里读
void ui_idtip_bind_ex(HWND lv, int col, u32 (*get)(int row));  // 虚拟列表：格子文本读不出来
void ui_idtip_tick(void);           // 主窗口定时器里调（~100ms 一次）

// 一条分隔线：用来把「表单区」和「列表区」断开，省得整页糊成一片。
// 高度固定 2px（SS_ETCHEDHORZ 就是上下两条 1px 的凹线），宽度交给 ui_place
HWND ui_hline(HWND parent);

// ---- 下拉框 ----
void ui_combo_fill(HWND c, const char *const *items, int n, int sel);
int  ui_combo_sel(HWND c);
void ui_combo_set(HWND c, int i);

// ---- 列表 ----
void ui_cols(HWND lv, const char *const *titles, const int *widths, int n);
void ui_cols_add(HWND lv, const char *title, int width);
void ui_rows_clear(HWND lv);
int  ui_row_add(HWND lv, const char *const *cells, int n, LPARAM data);
void ui_row_set(HWND lv, int row, const char *const *cells, int n);   // 原地刷新一行，选中不丢
int  ui_row_sel(HWND lv);
LPARAM ui_row_data(HWND lv, int row);
void ui_row_sel_set(HWND lv, int row);
void ui_batch(HWND lv, int begin);
void ui_cols_width(HWND lv, const int *widths, int n);

// ---- 布局游标：一行内顺序摆放 ----
typedef struct {
    int x, y, h, gap;
    HWND last, host;
} URow;
void ui_row(URow *r, HWND host, int x, int y, int h);
void ui_add(URow *r, HWND w, int width);       // width<0 → 撑到右边界
void ui_gap(URow *r, int px);
void ui_hide(URow *r);                         // 本行后续控件移到零尺寸

void ui_fill_rest(HWND page, HWND child, int top);   // child 占满 page 的剩余区域

// ---- 页面容器 ----
HWND ui_page_new(Page *p, HWND parent);
void ui_pages_reload(int page);
void ui_pages_reload_all(void);
void ui_pages_lock(void);
void ui_pages_switch(int page);
int  ui_page_cur(void);
void ui_pages_place(int page, int x, int y, int w, int h);   // 主窗口布局时调用
void ui_page_redraw(HWND page);          // 整页（含所有子控件）标脏，等下一轮绘制

// ---- 模态弹窗 ----
typedef LRESULT (*UiDlgProc)(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, void *ctx);
HWND ui_dlg_new(HWND owner, const char *title, int w, int h, UiDlgProc proc, void *ctx);
int  ui_dlg_run(HWND dlg, HWND owner);   // 返回 1=确定 0=取消
void ui_dlg_end(int ok);

// ---- 按类浏览 PVF 物品 ----
// 材料 / 消耗品 / 装备 / 宠物 / 时装 都是成千上万条，所以单开一个窗口、用虚拟列表，
// 挑好（双击一行或多选）直接灌进发放队列。cat 取 BR_*。
enum { BR_NONE = 0, BR_MAT, BR_STACK, BR_EQUIP, BR_PET, BR_AVATAR };
void ui_browse_open(HWND owner, int cat);

// ---- 各页实现 ----
void page_item_create(Page *p);
void page_query_create(Page *p);
void page_grant_create(Page *p);
void page_grant_setid(u32 id);          // 取码结果回填编号
void page_grant_autoadd(const GrabHit *h);  // 取码命中：整行入队，同编号静默跳过
void page_grant_syncauto(void);         // 复选框同步到 g.cfg.grab_auto（热键改开关后用）
int  page_grant_built(void);            // 发放页是否已建：连续取码要有列表才收得下东西
void page_mail_create(Page *p);
void page_pet_create(Page *p);
void page_avatar_create(Page *p);
void page_quest_create(Page *p);
void page_event_create(Page *p);
void page_gm_create(Page *p);
void page_gm_pump(void);                // 把 GM 页排队的写入往前推（每 tick 调，与是否可见无关）
void page_bubble_create(Page *p);
void page_bubble_pump(void);            // 泡点状态行（倒计时/上次结算）跟着 tick 刷
void page_ban_create(Page *p);
void page_log_create(Page *p);
