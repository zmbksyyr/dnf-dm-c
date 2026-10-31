// ui_common：DPI/字体、控件封装、列表辅助、布局游标、页面注册与宿主。
#include "ui.h"
#include <commctrl.h>

static int        g_dpi = 96;
static int        g_pt = 9;
static HINSTANCE  g_inst;
static HFONT      g_f, g_fb;
static int        g_cur = -1;
static int        g_reg;

void ui_pages_wire(void);

// ---------------- 页面注册 ----------------
Page g_pages[PAGE_COUNT] = {
    [PAGE_ITEM] = {"物品"},     [PAGE_QUERY] = {"查询"},  [PAGE_GRANT] = {"发放"},
    [PAGE_MAIL] = {"邮件"},     [PAGE_PET] = {"宠物"},    [PAGE_AVATAR] = {"时装"},
    [PAGE_QUEST] = {"任务"},    [PAGE_EVENT] = {"活动"},  [PAGE_GM] = {"GM"},
    [PAGE_BUBBLE] = {"泡点"},   [PAGE_BAN] = {"封停"},    [PAGE_LOG] = {"日志"},
};

void ui_pages_wire(void) {
    page_item_create(&g_pages[PAGE_ITEM]);
    page_query_create(&g_pages[PAGE_QUERY]);
    page_grant_create(&g_pages[PAGE_GRANT]);
    page_mail_create(&g_pages[PAGE_MAIL]);
    page_pet_create(&g_pages[PAGE_PET]);
    page_avatar_create(&g_pages[PAGE_AVATAR]);
    page_quest_create(&g_pages[PAGE_QUEST]);
    page_event_create(&g_pages[PAGE_EVENT]);
    page_gm_create(&g_pages[PAGE_GM]);
    page_bubble_create(&g_pages[PAGE_BUBBLE]);
    page_ban_create(&g_pages[PAGE_BAN]);
    page_log_create(&g_pages[PAGE_LOG]);
}

HINSTANCE ui_inst(void) { return g_inst; }
int       ui_dpi(void) { return g_dpi; }
int       ui_scale(int px) { return MulDiv(px, g_dpi, 96); }
HFONT     ui_font(void) { return g_f; }
HFONT     ui_font_bold(void) { return g_fb ? g_fb : g_f; }

// 量一行文字的宽（用界面字体）。STATIC 控件本身可以拉得很宽，文字仍然只占前面
// 那一段，所以「让按钮右端和上面那句提示的右端对齐」时不能拿控件宽度当文字宽度。
int ui_text_w(const char *s) {
    if (!s || !s[0]) return 0;
    WCHAR w[512];
    utf16(s, w, COUNT_OF(w));
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    HFONT old = (HFONT)SelectObject(dc, ui_font());
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, w, (int)wcslen(w), &sz);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return sz.cx;
}

// ---------------- 配色 ----------------
// 系统默认的 BS_GROUPBOX 是「凹陷灰坑 + 内部比页面略亮」，满屏六个就是一堆色块。
// 这里统一成：页面底色做底，组框只留一条 1px 浅灰线，标题骑在顶边上。
// 三层颜色够了 —— 底色、白（可编辑区与卡片）、线。
static COLORREF g_c_bg = RGB(0xF6, 0xF7, 0xF9);
static COLORREF g_c_line = RGB(0xD8, 0xDD, 0xE4);
static COLORREF g_c_ink = RGB(0x2A, 0x2F, 0x36);
static COLORREF g_c_dim = RGB(0x62, 0x69, 0x74);
static HBRUSH g_br_bg, g_br_card;

COLORREF ui_bg_color(void) { return g_c_bg; }
COLORREF ui_line_color(void) { return g_c_line; }
COLORREF ui_text_ink(void) { return g_c_ink; }
COLORREF ui_text_dim(void) { return g_c_dim; }

// 惰性创建：主窗口 RegisterClass 时可能还没进 ui_init，这里不能依赖初始化顺序
HBRUSH ui_bg_brush(void) {
    if (!g_br_bg) g_br_bg = CreateSolidBrush(g_c_bg);
    return g_br_bg ? g_br_bg : (HBRUSH)(COLOR_BTNFACE + 1);
}

HBRUSH ui_card_brush(void) {
    if (!g_br_card) g_br_card = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    return g_br_card;
}

// 主窗口 / 页 / 对话框共用：标签跟页面同色（否则每个 STATIC 都留一块 3DFACE 灰印），
// 只读 EDIT 用白底（日志、路径这类"内容区"应该像张白纸，不是又一块灰）
// 禁用控件的底色。只 EnableWindow(FALSE) 不改颜色的话，EDIT 还是白底黑字，
// 看上去跟能编辑的一模一样 —— 「该灰的没灰」用户根本看不出这个框不能填。
// 固定一个比正常输入框明显深一点的灰。不能用 GetSysColorBrush(COLOR_BTNFACE)：
// 开了视觉主题时它返回的是主题刷，颜色跟白底差不了两三度，禁用等于没标出来。
static HBRUSH g_dis_brush;
static HBRUSH dis_brush(void) {
    if (!g_dis_brush) g_dis_brush = CreateSolidBrush(RGB(0xE2, 0xE2, 0xE2));
    return g_dis_brush;
}

LRESULT ui_ctl_color(WPARAM wp, LPARAM lp) {
    wchar_t cls[32] = L"";
    HWND w = (HWND)lp;
    GetClassNameW(w, cls, (int)COUNT_OF(cls));
    int is_edit = !lstrcmpiW(cls, L"Edit");
    HDC dc = (HDC)wp;
    if (is_edit) {
        if (!IsWindowEnabled(w)) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(0xE2, 0xE2, 0xE2));
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            return (LRESULT)dis_brush();
        }
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
        return (LRESULT)ui_card_brush();
    }
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, g_c_bg);
    return (LRESULT)ui_bg_brush();
}

static HFONT mk_font(int pt, int bold) {
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) {
        wcscpy(lf.lfFaceName, L"SimSun");
        f = CreateFontIndirectW(&lf);
    }
    return f;
}

void ui_font_reload(int pt) {
    if (pt >= 8 && pt <= 14) g_pt = pt;
    if (g_f) { DeleteObject(g_f); g_f = NULL; }
    if (g_fb) { DeleteObject(g_fb); g_fb = NULL; }
    g_f = mk_font(g_pt, 0);
    g_fb = mk_font(g_pt, 1);
}

static BOOL CALLBACK setfont_cb(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

void ui_set_dpi(int dpi) {
    if (dpi < 72 || dpi > 480) return;
    if (dpi == g_dpi) return;
    g_dpi = dpi;
    ui_font_reload(0);
    if (g_main) EnumChildWindows(g_main, setfont_cb, (LPARAM)g_f);
}

void ui_init(HINSTANCE inst, int pt) {
    g_inst = inst;
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    ui_font_reload(pt);
    ui_pages_wire();
}

// ---------------- 控件 ----------------
HWND ui_make(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, DWORD ex, int id) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    if (h && g_f) SendMessageW(h, WM_SETFONT, (WPARAM)g_f, TRUE);
    return h;
}

static HWND mktext(HWND parent, const char *text, int id, HFONT f, DWORD style) {
    WCHAR w[1024];
    utf16(text, w, COUNT_OF(w));
    HWND h = CreateWindowExW(0, L"STATIC", w, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)(f ? f : g_f), TRUE);
    return h;
}

HWND ui_label(HWND parent, const char *text, int id) {
    return mktext(parent, text, id, NULL, SS_LEFT | SS_CENTERIMAGE);
}

HWND ui_label_r(HWND parent, const char *text, int id) {
    return mktext(parent, text, id, NULL, SS_RIGHT | SS_CENTERIMAGE);
}

HWND ui_label_b(HWND parent, const char *text, int id) {
    return mktext(parent, text, id, g_fb ? g_fb : g_f, SS_LEFT | SS_CENTERIMAGE);
}

// 禁用的输入框自己画：带视觉样式的 EDIT 即使 disabled 也还是白底，
// 跟能填的一模一样 —— 现场看到的就是"该灰的没灰"。父窗口的 WM_CTLCOLOR
// 在这台机器上也没落下来（禁用/正常两种底色完全一致），所以干脆在
// WM_PAINT 里铺一层灰底、再用灰字重画内容。
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    (void)ref;
    if (!IsWindowEnabled(h) && (m == WM_PAINT || m == WM_ERASEBKGND)) {
        HDC dc;
        PAINTSTRUCT ps;
        int own = (m == WM_PAINT);
        dc = own ? BeginPaint(h, &ps) : (HDC)wp;
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, dis_brush());
        if (!own) return 1;
        wchar_t t[512] = L"";
        GetWindowTextW(h, t, (int)COUNT_OF(t));
        if (t[0]) {
            RECT tr = rc;
            tr.left += 4;
            tr.right -= 2;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            HGDIOBJ of = SelectObject(dc, (HGDIOBJ)SendMessageW(h, WM_GETFONT, 0, 0));
            DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

HWND ui_edit(HWND parent, const char *text, int id, int ro) {
    WCHAR w[1024];
    utf16(text, w, COUNT_OF(w));
    // 可编辑的框必须带 WS_TABSTOP：不加的话 Tab 键会把它整个跳过去。
    // 以前这里没有，GM 页 14 个输入框全都不在 Tab 序列里，只能靠鼠标点。
    DWORD st = WS_BORDER | ES_AUTOHSCROLL | (ro ? ES_READONLY : WS_TABSTOP);
    HWND h = ui_make(parent, L"EDIT", w, st, 0, id);
    if (h) SetWindowSubclass(h, edit_proc, 1, 0);
    return h;
}

// ---------------- 只许填数字的输入框 ----------------
// 光靠提交时校验不够：填「12abc」提交出来是 12，填 999999999999 转成 u32 又变成
// 另一个数，用户看到的却始终是自己敲的那串字符。所以输入这一层就掐死：
// 非数字键吞掉，粘贴先过检查，失焦时把值夹进 [lo, hi] 再把规范化的数字写回框。
typedef struct {
    i64  lo, hi;
    char last[24];          // 上一次的合法文本：空框被清空时用它兜回去
} NumRange;

// 把 ins 插到选区位置后的完整值是否还在上限内
static int num_fit(HWND h, const char *ins, i64 hi) {
    char t[48];
    ui_get(h, t, sizeof(t));
    DWORD sel = (DWORD)SendMessageW(h, EM_GETSEL, 0, 0);
    int a = (int)LOWORD(sel), b = (int)HIWORD(sel), n = (int)strlen(t);
    if (a > n) a = n;
    if (b > n) b = n;
    if (b < a) b = a;
    char cand[96];
    int k = 0;
    for (int i = 0; i < a && k < 90; i++) cand[k++] = t[i];
    for (const char *q = ins; q && *q && k < 90; q++) cand[k++] = *q;
    for (int i = b; i < n && k < 90; i++) cand[k++] = t[i];
    cand[k] = 0;
    return strtoll(cand, NULL, 10) <= hi;
}

static LRESULT CALLBACK num_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    NumRange *rg = (NumRange *)ref;
    if (!rg) return DefSubclassProc(h, m, wp, lp);
    switch (m) {
    case WM_CHAR: {
        if (wp < 32 || wp == 127) break;            // 退格、方向键、Ctrl 组合照常走
        if (wp < '0' || wp > '9') return 0;         // 字母/负号/小数点一律不收
        char one[2] = {(char)wp, 0};
        if (!num_fit(h, one, rg->hi)) return 0;     // 会让值超上限：这一下不进去
        break;
    }
    case WM_IME_CHAR: {
        // 中文输入法上屏走的是 WM_IME_CHAR，不经过 WM_CHAR：这里再挡一道，
        // 否则在数字框里打汉字能塞进去，失焦时才被规范化成 0。
        if (wp < '0' || wp > '9') return 0;
        char one[2] = {(char)wp, 0};
        if (!num_fit(h, one, rg->hi)) return 0;
        break;
    }
    case WM_PASTE: {
        int good = 0;
        char add[48] = "";
        if (OpenClipboard(h)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) {
                const WCHAR *s = (const WCHAR *)GlobalLock(hd);
                if (s) {
                    char a[48];
                    utf8(s, a, sizeof(a));
                    GlobalUnlock(hd);
                    int k = 0;
                    good = 1;
                    for (const char *q = a; *q; q++) {
                        if (*q < '0' || *q > '9') { good = 0; break; }
                        if (k < (int)sizeof(add) - 1) add[k++] = *q;
                    }
                    add[k] = 0;
                }
            }
            CloseClipboard();
        }
        if (!good || !add[0] || !num_fit(h, add, rg->hi)) return 0;
        break;
    }
    case WM_SETTEXT: {
        // 程序设值（ui_set_int / ui_set_i64 / 页面回填）和用户输入都走这里，
        // 顺手记下来：框被清空时不至于把「清空重打」当成 0 落到库里。
        // 只在非空时记 —— 清空动作本身不能把历史值擦掉。
        LRESULT r = DefSubclassProc(h, m, wp, lp);
        char now[48];
        ui_get(h, now, sizeof(now));
        if (now[0]) snprintf(rg->last, sizeof(rg->last), "%s", now);
        return r;
    }
    case WM_KILLFOCUS: {
        // 空框先兜回上一次的值（什么都不改），没历史才按最小值落定；
        // 超范围的夹回上限。先规范化再让 EN_KILLFOCUS 出去，父窗口读到的就是能用的数字。
        char t[48];
        ui_get(h, t, sizeof(t));
        if (!t[0] && rg->last[0]) {
            ui_set_i64(h, strtoll(rg->last, NULL, 10));
            break;
        }
        i64 v = t[0] ? strtoll(t, NULL, 10) : rg->lo;
        if (v < rg->lo) v = rg->lo;
        if (v > rg->hi) v = rg->hi;
        char want[32];
        fmt(want, sizeof(want), "%lld", (long long)v);
        if (strcmp(want, t)) ui_set_i64(h, v);
        break;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, num_proc, 2);
        free(rg);
        return DefSubclassProc(h, m, wp, lp);
    }
    return DefSubclassProc(h, m, wp, lp);
}

void ui_num(HWND e, i64 lo, i64 hi) {
    if (!e || hi < 0) return;
    if (lo > hi) { i64 t = lo; lo = hi; hi = t; }
    // ES_NUMBER 让系统那一层就只认数字（输入法上屏的汉字也过不去），
    // 下面的子类再补范围检查 —— 两道都留着，短路了也不会把脏数据写进库。
    LONG_PTR st = GetWindowLongPtrW(e, GWL_STYLE);
    SetWindowLongPtrW(e, GWL_STYLE, st | ES_NUMBER);
    NumRange *rg = (NumRange *)xalloc(sizeof(*rg));
    rg->lo = lo;
    rg->hi = hi;
    SetWindowSubclass(e, num_proc, 2, (DWORD_PTR)rg);
}

// ---------------- 按钮：接管绘制 ----------------
// 主题按钮的悬停高亮（蓝框）点完不消失：它由 uxtheme 自己缓存，按钮即便收到
// WM_MOUSELEAVE 也未必清得掉。实测真实点击后按钮区的蓝像素从 44 涨到 344，
// 之后把鼠标移进移出都回不到 44；而同样一次点击改用 BM_CLICK 触发就一点残留
// 都没有 —— 说明问题出在系统那层状态，代码没法从外部复位。
//
// 所以这里不问系统要状态了，两个状态源都自己管：
//   · 光标在不在客户区 —— 自己 SetProp 记，自己 TrackMouseEvent
//   · 按钮是不是按下   —— 自己跟 WM_LBUTTONDOWN / UP / CAPTURECHANGED
// 画法照系统那套灰底 + 1px 边框，只把悬停/按下的配色换成 Win11 的蓝。
// 焦点框不画：这个工具几乎全靠鼠标点，留着只会让人以为按钮卡住了。
#define BTN_HOT(s)   ((s) & 1)
#define BTN_DOWN(s)  ((s) & 2)
static const WCHAR kBtnProp[] = L"dnfbtn";

static int btn_st(HWND h) { return (int)(INT_PTR)GetPropW(h, kBtnProp); }

static void btn_st_set(HWND h, int v) {
    if (btn_st(h) == v) return;
    SetPropW(h, kBtnProp, (HANDLE)(INT_PTR)v);   // v=0 即移除属性
    InvalidateRect(h, NULL, FALSE);
}

static void btn_paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    if (!dc) return;
    RECT rc;
    GetClientRect(h, &rc);
    int en = IsWindowEnabled(h);
    int st = en ? btn_st(h) : 0;
    int hot = BTN_HOT(st);
    // 按下态问两个来源：鼠标按的是自己记的，空格键按的是按钮内部的 BST_PUSHED。
    int down = BTN_DOWN(st) || (DefSubclassProc(h, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

    COLORREF bg, bd, tx = RGB(0x1A, 0x1A, 0x1A);
    if (!en) {
        bg = RGB(0xF2, 0xF2, 0xF2);
        bd = RGB(0xD8, 0xD8, 0xD8);
        tx = GetSysColor(COLOR_GRAYTEXT);
    } else if (down) {
        bg = RGB(0xCA, 0xE0, 0xF5);
        bd = RGB(0x00, 0x54, 0x99);
    } else if (hot) {
        bg = RGB(0xE5, 0xF1, 0xFB);
        bd = RGB(0x00, 0x78, 0xD7);
    } else {
        bg = RGB(0xF0, 0xF0, 0xF0);
        bd = RGB(0xAD, 0xAD, 0xAD);
    }

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, bd);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    WCHAR t[256];
    int n = GetWindowTextW(h, t, COUNT_OF(t));
    if (n > 0) {
        HFONT of = (HFONT)SelectObject(dc, ui_font());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, tx);
        RECT tr = rc;
        if (down) OffsetRect(&tr, 1, 1);
        DrawTextW(dc, t, n, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK btn_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    (void)ref;
    switch (m) {
        case WM_PAINT:
            btn_paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(h, NULL, FALSE);
            break;
        case WM_MOUSEMOVE: {
            if (!BTN_HOT(btn_st(h))) btn_st_set(h, btn_st(h) | 1);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            btn_st_set(h, btn_st(h) & ~1);
            break;
        case WM_LBUTTONDOWN:
            btn_st_set(h, btn_st(h) | 2);
            break;
        case WM_LBUTTONUP: {
            HWND par = GetParent(h);
            LRESULT r = DefSubclassProc(h, m, wp, lp);   // 先让按钮把 BN_CLICKED 发出去
            // 动作里可能已经把页面重建掉，后面每一步都要重新确认窗口还在。
            if (IsWindow(h)) {
                // 悬停位一起清掉：动作跑完鼠标常常已经不在按钮上了，而这条
                // MOUSELEAVE 未必还送得到，留着就是那个「点过的按钮一直亮着」。
                btn_st_set(h, 0);
            }
            if (par && IsWindow(par)) SetFocus(par);
            return r;
        }
        case WM_CAPTURECHANGED:
            btn_st_set(h, btn_st(h) & ~2);
            break;
        case WM_ENABLE:
            InvalidateRect(h, NULL, FALSE);
            break;
        default:
            break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

HWND ui_button(HWND parent, const char *text, int id) {
    WCHAR w[256];
    utf16(text, w, COUNT_OF(w));
    HWND h = ui_make(parent, L"BUTTON", w, WS_TABSTOP | BS_PUSHBUTTON, 0, id);
    if (h) SetWindowSubclass(h, btn_proc, 1, 0);
    return h;
}

HWND ui_check(HWND parent, const char *text, int id, int on) {
    WCHAR w[256];
    utf16(text, w, COUNT_OF(w));
    HWND h = ui_make(parent, L"BUTTON", w, WS_TABSTOP | BS_AUTOCHECKBOX, 0, id);
    if (h && on) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    return h;
}

HWND ui_combo(HWND parent, int id, int droplist) {
    return ui_make(parent, L"COMBOBOX", L"", WS_TABSTOP | (droplist ? CBS_DROPDOWNLIST : CBS_DROPDOWN), 0,
                   id);
}

HWND ui_list(HWND parent, int id) {
    HWND lv = ui_make(parent, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                      WS_EX_CLIENTEDGE, id);
    // 不开 LVS_EX_LABELTIP：列宽不够时它会自己弹一条气泡，正好压在编号悬浮卡上。
    // 列表要看说明，把鼠标移到列头那一条上即可。
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    return lv;
}

// 分隔线：SS_ETCHEDHORZ 是上下各 1px 的凹线，2px 高。
// 用系统的而不是自己画，深浅色主题下颜色都对；宽度由 ui_place 给
HWND ui_hline(HWND parent) {
    return ui_make(parent, L"STATIC", L"", SS_ETCHEDHORZ, 0, 0);
}

// ---------------- 组框：细线卡片 ----------------
// 仍是 BS_GROUPBOX（布局按它的客户区算，几何一点不动），只接管外观：
// 填页面底色、画一条 1px 圆角线、标题放进框内第一行。
// 系统默认的凹陷边框 + 略亮的内部底色，在大屏上就是一排灰补丁。
static void group_draw(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    FillRect(dc, &rc, ui_bg_brush());

    int r = ui_scale(6);
    HPEN pen = CreatePen(PS_SOLID, 1, ui_line_color());
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    // 右/下各留 1px，否则 1px 线正好落在裁切边上被吃掉一半
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);

    wchar_t t[160];
    int n = GetWindowTextW(h, t, (int)COUNT_OF(t));
    if (n <= 0) return;
    HGDIOBJ of = SelectObject(dc, ui_font_bold());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui_text_ink());
    RECT tr = {rc.left + ui_scale(10), rc.top + ui_scale(2), rc.right - ui_scale(8),
               rc.top + ui_scale(20)};
    DrawTextW(dc, t, n, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

static LRESULT CALLBACK group_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id,
                                   DWORD_PTR ref) {
    (void)id;
    (void)ref;
    switch (m) {
        case WM_ERASEBKGND:
            return 1;                        // 底层不刷，WM_PAINT 一次画完，不闪
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            group_draw(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:                 // PrintWindow / 截图走这条，不处理就是空白
            group_draw(h, (HDC)wp);
            return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

HWND ui_group(HWND parent, const char *text, int id) {
    WCHAR w[256];
    utf16(text, w, COUNT_OF(w));
    HWND h = CreateWindowExW(0, L"BUTTON", w, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0,
                             parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    // 组框标题用粗体：它跟组内的字段标签只差一个位置，不加重就和普通标签分不出层次
    if (h) {
        SendMessageW(h, WM_SETFONT, (WPARAM)ui_font_bold(), TRUE);
        SetWindowSubclass(h, group_proc, 1, 0);
    }
    return h;
}

HWND ui_btn_at(HWND parent, int id, const char *text) {
    HWND w = GetDlgItem(parent, id);
    return w ? w : ui_button(parent, text, id);
}

typedef struct {
    HWND parent;
    char *text;
    HWND w;
} LblReg;
static LblReg *g_lr;
static int g_lrn, g_lrcap;

HWND ui_lbl_at(HWND parent, const char *text) {
    for (int i = 0; i < g_lrn; i++)
        if (g_lr[i].parent == parent && strcmp(g_lr[i].text, text) == 0) return g_lr[i].w;
    HWND w = ui_label(parent, text, 0);
    if (g_lrn == g_lrcap) {
        g_lrcap = g_lrcap ? g_lrcap * 2 : 64;
        g_lr = (LblReg *)xrealloc(g_lr, sizeof(LblReg) * (size_t)g_lrcap);
    }
    g_lr[g_lrn].parent = parent;
    g_lr[g_lrn].text = str_dup(text);
    g_lr[g_lrn].w = w;
    g_lrn++;
    return w;
}

void ui_text(HWND w, const char *utf8) {
    WCHAR tmp[2048];
    WCHAR *p = tmp;
    int cap = COUNT_OF(tmp);
    if ((int)strlen(utf8) + 1 > cap) {
        cap = (int)strlen(utf8) + 1;
        p = (WCHAR *)xalloc((size_t)cap * sizeof(WCHAR));
        if (!p) return;
    }
    utf16(utf8, p, cap);
    SetWindowTextW(w, p);
    if (p != tmp) free(p);
}

void ui_get(HWND w, char *out, int cap) {
    int n = GetWindowTextLengthW(w);
    if (n <= 0) { if (cap > 0) out[0] = 0; return; }
    WCHAR *p = (WCHAR *)xalloc(((size_t)n + 2) * sizeof(WCHAR));
    if (!p) { out[0] = 0; return; }
    GetWindowTextW(w, p, n + 1);
    utf8(p, out, cap);
    free(p);
}

int ui_int(HWND w) {
    char b[64];
    ui_get(w, b, sizeof(b));
    return atoi(b);
}

void ui_set_int(HWND w, int v) {
    char b[32];
    fmt(b, sizeof(b), "%d", v);
    ui_text(w, b);
}

i64 ui_i64(HWND w) {
    char b[64];
    ui_get(w, b, sizeof(b));
    return (i64)strtoll(b, NULL, 10);
}

void ui_set_i64(HWND w, i64 v) {
    char b[40];
    fmt(b, sizeof(b), "%lld", (long long)v);
    ui_text(w, b);
}

void ui_enable(HWND w, int on) {
    if (!w) return;
    EnableWindow(w, on ? TRUE : FALSE);
    // 禁用的 EDIT 由 edit_proc 自己画灰底，状态一变就得重绘
    InvalidateRect(w, NULL, TRUE);
}

void ui_show(HWND w, int on) {
    if (w) ShowWindow(w, on ? SW_SHOW : SW_HIDE);
}

// ---------------- 悬浮提示 ----------------
// 每个宿主窗口一个 tooltip 实例，控件按句柄注册；重复注册同一控件以最后一次为准。
typedef struct {
    HWND host, tip;
} TipReg;
static TipReg *g_tr;
static int     g_trn, g_trcap;

static HWND tip_of(HWND host) {
    for (int i = 0; i < g_trn; i++)
        if (g_tr[i].host == host) return g_tr[i].tip;
    HWND t = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                             WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, host, NULL, g_inst, NULL);
    if (!t) return NULL;
    SendMessageW(t, TTM_SETMAXTIPWIDTH, 0, ui_scale(340));
    SendMessageW(t, TTM_SETDELAYTIME, TTDT_AUTOPOP, (LPARAM)12000);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_f, TRUE);
    if (g_trn == g_trcap) {
        g_trcap = g_trcap ? g_trcap * 2 : 16;
        g_tr = (TipReg *)xrealloc(g_tr, sizeof(TipReg) * (size_t)g_trcap);
    }
    g_tr[g_trn].host = host;
    g_tr[g_trn].tip = t;
    g_trn++;
    return t;
}

// 悬浮提示：host 每个窗口共用一个 tooltip，ctl 上悬停时显示说明。
// 列表上不挂提示 —— 鼠标停在行上时会弹「编号悬浮卡」，两个气泡撞在一起。
// 列表的用法说明统一写成列表下方一行常驻小字。
void ui_tip(HWND host, HWND ctl, const char *text) {
    if (!host || !ctl || !text || !text[0]) return;
    HWND t = tip_of(host);
    if (!t) return;
    WCHAR w[512];
    utf16(text, w, COUNT_OF(w));
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.hwnd = host;
    ti.uId = (UINT_PTR)ctl;
    SendMessageW(t, TTM_DELTOOLW, 0, (LPARAM)&ti);   // 同一控件重复挂提示时以新文案为准
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.lpszText = w;
    SendMessageW(t, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void ui_place(HWND w, int x, int y, int cw, int ch) {
    if (w) MoveWindow(w, x, y, cw, ch, TRUE);
}

// ---------------- 下拉框 ----------------
void ui_combo_fill(HWND c, const char *const *items, int n, int sel) {
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        WCHAR w[256];
        utf16(items[i], w, COUNT_OF(w));
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w);
    }
    ui_combo_set(c, sel);
}

int ui_combo_sel(HWND c) { return (int)SendMessageW(c, CB_GETCURSEL, 0, 0); }

void ui_combo_set(HWND c, int i) { SendMessageW(c, CB_SETCURSEL, (WPARAM)i, 0); }

// ---------------- 列表 ----------------
static void setcol(HWND lv, int i, const char *title, int width) {
    WCHAR w[128];
    utf16(title, w, COUNT_OF(w));
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = w;
    c.cx = ui_scale(width);
    c.iSubItem = i;
    SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
}

void ui_cols(HWND lv, const char *const *titles, const int *widths, int n) {
    for (int i = 0; i < n; i++) setcol(lv, i, titles[i], widths[i]);
}

void ui_cols_add(HWND lv, const char *title, int width) {
    int n = Header_GetItemCount((HWND)SendMessageW(lv, LVM_GETHEADER, 0, 0));
    setcol(lv, n, title, width);
}

void ui_cols_width(HWND lv, const int *widths, int n) {
    for (int i = 0; i < n; i++) ListView_SetColumnWidth(lv, i, ui_scale(widths[i]));
}

void ui_rows_clear(HWND lv) {
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

int ui_row_add(HWND lv, const char *const *cells, int n, LPARAM data) {
    WCHAR w[512];
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    utf16(cells[0] ? cells[0] : "", w, COUNT_OF(w));
    it.pszText = w;
    it.lParam = data;
    it.iItem = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (int c = 1; c < n; c++) {
        utf16(cells[c] ? cells[c] : "", w, COUNT_OF(w));
        ListView_SetItemText(lv, it.iItem, c, w);
    }
    return it.iItem;
}

// 原地刷新已有的一行：不删行，选中项不会丢（编辑保存类操作用它）
void ui_row_set(HWND lv, int row, const char *const *cells, int n) {
    if (row < 0) return;
    WCHAR w[512];
    for (int c = 0; c < n; c++) {
        utf16(cells[c] ? cells[c] : "", w, COUNT_OF(w));
        ListView_SetItemText(lv, row, c, w);
    }
}

int ui_row_sel(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

LPARAM ui_row_data(HWND lv, int row) {
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = row;
    if (!SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&it)) return 0;
    return it.lParam;
}

void ui_row_sel_set(HWND lv, int row) {
    ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, row, FALSE);
}

void ui_batch(HWND lv, int begin) {
    SendMessageW(lv, WM_SETREDRAW, begin ? FALSE : TRUE, 0);
    if (!begin) InvalidateRect(lv, NULL, FALSE);
}

// ---------------- 布局游标 ----------------
void ui_row(URow *r, HWND host, int x, int y, int h) {
    memset(r, 0, sizeof(*r));
    r->x = x;
    r->y = y;
    r->h = h;
    r->gap = ui_scale(4);
    r->host = host;
}

void ui_add(URow *r, HWND w, int width) {
    if (!w) return;
    int cw = width;
    if (cw < 0) {
        RECT rc;
        GetClientRect(r->host ? r->host : GetParent(w), &rc);
        cw = rc.right - r->x - ui_scale(8);
        if (cw < ui_scale(40)) cw = ui_scale(40);
    }
    MoveWindow(w, r->x, r->y, cw, r->h, TRUE);
    r->x += cw + r->gap;
    r->last = w;
}

void ui_gap(URow *r, int px) { r->x += px; }

void ui_hide(URow *r) { (void)r; }

void ui_fill_rest(HWND page, HWND child, int top) {
    if (!page || !child) return;
    RECT rc;
    GetClientRect(page, &rc);
    int m = ui_scale(6);
    int cw = rc.right - 2 * m;
    int ch = rc.bottom - top - m;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    MoveWindow(child, m, top, cw, ch, TRUE);
}

// ---------------- 页面容器 ----------------
static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Page *p = (Page *)GetWindowLongPtrW(h, GWLP_USERDATA);
    // build/reload 期间不转发：建带初始文本的 EDIT 会发 EN_CHANGE，
    // 直接进页面处理函数可能弹模态框，嵌套消息循环又把布局重入一遍。
    if (p && p->on && !p->busy) {
        LRESULT r = p->on(p, m, wp, lp);
        if (r != -1) return r;
    }
    // EDIT 走 WM_CTLCOLOREDIT，不是 STATIC：漏了这条，禁用状态永远是系统白底，
    // SetTextColor 也落不下去（置灰后看着和可编辑的一样）。
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN || m == WM_CTLCOLOREDIT)
        return ui_ctl_color(wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

HWND ui_page_new(Page *p, HWND parent) {
    if (!g_reg) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = page_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = ui_bg_brush();
        wc.lpszClassName = L"DnfPage";
        RegisterClassExW(&wc);
        g_reg = 1;
    }
    HWND h = CreateWindowExW(0, L"DnfPage", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                             NULL, g_inst, NULL);
    if (h) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_f, FALSE);
    }
    return h;
}

int ui_page_cur(void) { return g_cur; }

void ui_pages_switch(int page) {
    if (page < 0 || page >= PAGE_COUNT) return;
    Page *p = &g_pages[page];
    if (!p->wnd) {
        p->wnd = ui_page_new(p, g_main);
        p->busy = 1;
        if (p->build) p->build(p, p->wnd);
        p->busy = 0;
    }
    for (int i = 0; i < PAGE_COUNT; i++)
        if (g_pages[i].wnd && i != page) ShowWindow(g_pages[i].wnd, SW_HIDE);
    g_cur = page;
    ShowWindow(p->wnd, SW_SHOW);
    // 这里只切可见性。尺寸与填充交给 ui_pages_place()：只有拿到真实尺寸后填列表，
    // 控件才会按正确行高自绘（0 尺寸时插入的行会一直画不出文字）。
}

void ui_pages_place(int page, int x, int y, int w, int h) {
    if (page < 0 || page >= PAGE_COUNT) return;
    Page *p = &g_pages[page];
    if (!p->wnd || p->busy) return;
    MoveWindow(p->wnd, x, y, w, h, TRUE);
    if (p->layout) p->layout(p, w, h);
    if (p->lock) p->lock(p);
    // Tab 顺序不再在这里重排：改 Z 序会让组框的填充画到控件上面（组框是整块
    // 填底色的自绘窗口，一旦它压在控件之上，控件就整片消失）。控件的创建顺序
    // 本来就按视觉从上到下排（字段在前、按钮在后），够用了。
}

void ui_pages_reload(int page) {
    if (page < 0 || page >= PAGE_COUNT) return;
    Page *p = &g_pages[page];
    if (p->wnd && p->reload && !p->busy) p->reload(p);
}

// 整页重画：页面重新显示时系统只会让页面自己重画，子控件（内容没变的 combo 最典型）
// 会留一块空白，直到鼠标划过才补画。RDW_ALLCHILDREN 实测不会把子控件标脏，
// 必须逐个枚举失效（EnumChildWindows 会递归到孙控件）。
static BOOL CALLBACK redraw_cb(HWND h, LPARAM lp) {
    (void)lp;
    InvalidateRect(h, NULL, TRUE);
    return TRUE;
}

void ui_page_redraw(HWND page) {
    if (!page) return;
    InvalidateRect(page, NULL, TRUE);
    EnumChildWindows(page, redraw_cb, 0);
}

// 「数据变了，让界面跟上」——只刷当前看得见的那页。
// 隐藏页切回来时 select_page 必然 reload 一次，现在给它算一遍纯属白干活
// （11 页列表全清重填、日志页重读 100KB 盘），还会把用户在别处做到一半的
// 选中/滚动状态打掉。语义是「可见的界面跟上」，不是「把每页都算一遍」。
void ui_pages_reload_all(void) {
    int cur = ui_page_cur();
    if (cur >= 0 && cur < PAGE_COUNT) ui_pages_reload(cur);
}

void ui_pages_lock(void) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        Page *p = &g_pages[i];
        if (p->wnd && p->lock) p->lock(p);
    }
}

// ---------------- 模态弹窗 ----------------
typedef struct {
    UiDlgProc proc;
    void *ctx;
} DlgBox;

static int g_dlg_running, g_dlg_ok;
static int g_dlg_reg;

void ui_dlg_end(int ok) {
    g_dlg_ok = ok;
    g_dlg_running = 0;
}

static LRESULT CALLBACK dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    DlgBox *d = (DlgBox *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CLOSE) {
        ui_dlg_end(0);
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        int id = LOWORD(wp);
        if (id == IDCANCEL) {
            ui_dlg_end(0);
            return 0;
        }
    }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) return ui_ctl_color(wp, lp);
    if (d && d->proc) {
        LRESULT r = d->proc(h, m, wp, lp, d->ctx);
        if (r != -1) return r;
    }
    return DefWindowProcW(h, m, wp, lp);
}

HWND ui_dlg_new(HWND owner, const char *title, int cw, int ch, UiDlgProc proc, void *ctx) {
    if (!g_dlg_reg) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dlg_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = ui_bg_brush();
        wc.lpszClassName = L"DnfDlg";
        RegisterClassExW(&wc);
        g_dlg_reg = 1;
    }
    WCHAR w[128];
    utf16(title, w, COUNT_OF(w));
    RECT orc = {0, 0, ui_scale(640), ui_scale(480)};
    if (owner) GetWindowRect(owner, &orc);
    // cw/ch 按客户区算：CreateWindow 的宽高含标题栏与边框，直接给会把底部按钮裁掉
    RECT adj = {0, 0, ui_scale(cw), ui_scale(ch)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&adj, style, FALSE, WS_EX_DLGMODALFRAME);
    int dw = adj.right - adj.left, dh = adj.bottom - adj.top;
    int x = orc.left + ((orc.right - orc.left) - dw) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - dh) / 2;

    DlgBox *d = (DlgBox *)xalloc(sizeof(*d));
    d->proc = proc;
    d->ctx = ctx;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DnfDlg", w, style, x, y, dw, dh, owner, NULL,
                             g_inst, NULL);
    if (!h) {
        free(d);
        return NULL;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_f, TRUE);
    return h;
}

int ui_dlg_run(HWND dlg, HWND owner) {
    if (!dlg) return 0;
    EnableWindow(owner ? owner : g_main, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    g_dlg_running = 1;
    g_dlg_ok = 0;
    MSG m;
    while (g_dlg_running) {
        if (GetMessageW(&m, NULL, 0, 0) <= 0) break;
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(owner ? owner : g_main, TRUE);
    if (owner ? owner : g_main) SetActiveWindow(owner ? owner : g_main);
    DlgBox *d = (DlgBox *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, 0);
    free(d);
    DestroyWindow(dlg);
    return g_dlg_ok;
}


// ---------------- 编号悬浮卡 ----------------
// 列表的编号列上悬停片刻，浮出一张「图标 + 详情」的小卡片。
// 图标来自贴图包（npk），详情来自 PVF；两边谁没就绪就退化成只显示编号。
// 悬停检测放在主窗口定时器里轮询，不用子类化列表，也不用管列表在哪个页/对话框里。

#define IDTIP_MAX 24
#define IDTIP_DELAY 400      // 悬停多久后浮出（ms）
typedef struct {
    HWND lv;
    int  col;
    u32 (*get)(int row);   // 虚拟列表读不出格子文本，改由调用方直接给编号
} IdTipBind;
static IdTipBind g_binds[IDTIP_MAX];
static int       g_nbinds;

static struct {
    HWND     wnd;          // 卡片窗口（惰性建）
    HWND     lv;
    int      row, col;
    u32      id;
    u64      since;        // 鼠标落到这个格子的时刻
    int      showing;
    HBITMAP  bmp;          // 展示中卡片的图标（npk 缓存所有，这里只借用）
    int      bw, bh;
    Str      text;         // 展示中卡片的文本
} s_tip;

static void tip_hide(void) {
    s_tip.showing = 0;
    s_tip.id = 0;
    s_tip.since = 0;
    if (s_tip.wnd) ShowWindow(s_tip.wnd, SW_HIDE);
}


static HBRUSH line_brush(void) {
    static HBRUSH b;
    if (!b) b = CreateSolidBrush(ui_line_color());
    return b;
}

static void frame_rect(HDC dc, RECT rc) {
    // 1px 线框：四条填色，不用 DrawEdge（它带 3D 凹凸，跟这套平面风格不搭）
    RECT t = rc;
    t.bottom = rc.top + 1;
    FillRect(dc, &t, line_brush());
    t = rc; t.top = rc.bottom - 1;
    FillRect(dc, &t, line_brush());
    t = rc; t.right = rc.left + 1;
    FillRect(dc, &t, line_brush());
    t = rc; t.left = rc.right - 1;
    FillRect(dc, &t, line_brush());
}

// 卡片内容画在给定 DC 上。WM_PAINT 和 WM_PRINTCLIENT（PrintWindow 截图走这条）
// 共用一份，不然截图工具拍到的浮层就是一块黑。
static void tip_paint(HDC dc, RECT rc) {
    FillRect(dc, &rc, ui_card_brush());
    frame_rect(dc, rc);
    int pad = ui_scale(8);
    if (s_tip.bmp) {
        // 图标画在左边一块小方格里，原图太小（28px）就放大一倍，看得清
        int box = ui_scale(60);
        int bw = s_tip.bw * 2, bh = s_tip.bh * 2;
        if (bw > box - pad) bw = box - pad;
        if (bh > box - pad) bh = box - pad;
        int ix = pad + (box - pad - bw) / 2, iy = pad + (box - pad - bh) / 2;
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ om = SelectObject(mdc, s_tip.bmp);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(dc, ix, iy, bw, bh, mdc, 0, 0, s_tip.bw, s_tip.bh, bf);
        SelectObject(mdc, om);
        DeleteDC(mdc);
    }
    // 文本
    RECT tr = rc;
    if (s_tip.bmp) tr.left += ui_scale(60) + pad;
    tr.left += pad;
    tr.top += pad;
    tr.right -= pad;
    tr.bottom -= pad;
    WCHAR wt[1024];
    utf16(str_c(&s_tip.text), wt, COUNT_OF(wt));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui_text_ink());
    SelectObject(dc, ui_font());
    DrawTextW(dc, wt, -1, &tr, DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL);
}

static LRESULT CALLBACK tip_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        tip_paint(dc, rc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(w, &rc);
        tip_paint((HDC)wp, rc);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void ui_idtip_bind_ex(HWND lv, int col, u32 (*get)(int row)) {
    if (!lv) return;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].lv == lv) { g_binds[i].col = col; g_binds[i].get = get; return; }
    if (g_nbinds == IDTIP_MAX) {
        // 挤掉最老的一个（列表销毁本来就会在 tick 里回收，这里只是保底）
        memmove(&g_binds[0], &g_binds[1], sizeof(IdTipBind) * (size_t)(IDTIP_MAX - 1));
        g_nbinds--;
    }
    g_binds[g_nbinds].lv = lv;
    g_binds[g_nbinds].col = col;
    g_binds[g_nbinds].get = get;
    g_nbinds++;
}

void ui_idtip_bind(HWND lv, int id_col) { ui_idtip_bind_ex(lv, id_col, NULL); }

// 编号列的格子读出来是个纯数字才算数；名字列 /「—」之类直接跳过
static u32 cell_id(HWND lv, int row, int col) {
    char buf[64];
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    WCHAR w[64] = L"";
    it.iSubItem = col;
    it.pszText = w;
    it.cchTextMax = COUNT_OF(w);
    SendMessageW(lv, LVM_GETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    utf8(w, buf, sizeof(buf));
    str_trim(buf);
    if (!buf[0]) return 0;
    for (char *q = buf; *q; q++)
        if (*q < '0' || *q > '9') return 0;
    return (u32)strtoul(buf, NULL, 10);
}

// 卡片内容：图标 + 详情文本，按内容定窗口大小
static void tip_show(u32 id) {
    if (!s_tip.wnd) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = tip_proc;
        wc.hInstance = ui_inst();
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"DnfIdTip";
        RegisterClassW(&wc);
        s_tip.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"DnfIdTip", L"",
                                    WS_POPUP, 0, 0, 0, 0, NULL, NULL, ui_inst(), NULL);
        if (!s_tip.wnd) return;
    }
    str_clear(&s_tip.text);
    app_item_tip(id, &s_tip.text);
    s_tip.bmp = app_item_icon(id, &s_tip.bw, &s_tip.bh);

    // 文本区域宽高：宽度固定上限，高度按实际折行算
    int maxw = ui_scale(300);
    int pad = ui_scale(8);
    int tw = maxw - pad * 2 - (s_tip.bmp ? ui_scale(60) + pad : 0);
    WCHAR wt[1024];
    utf16(str_c(&s_tip.text), wt, COUNT_OF(wt));
    HDC dc = GetDC(s_tip.wnd);
    HFONT of = (HFONT)SelectObject(dc, ui_font());
    RECT tr = {0, 0, tw, 0};
    DrawTextW(dc, wt, -1, &tr, DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT);
    SelectObject(dc, of);
    ReleaseDC(s_tip.wnd, dc);
    int iw = s_tip.bmp ? ui_scale(60) : 0;
    int w = pad * 2 + iw + (tr.right - tr.left);
    int h = pad * 2 + (s_tip.bmp ? ui_scale(60) : 0);
    if (h < tr.bottom + pad * 2) h = tr.bottom + pad * 2;
    if (w < ui_scale(120)) w = ui_scale(120);
    if (w > ui_scale(360)) w = ui_scale(360);

    POINT pt;
    GetCursorPos(&pt);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // 说明写得长的物品（[explain] 一大段）卡片高度跟着涨，别高过工作区
    int maxh = (wa.bottom - wa.top) - ui_scale(8);
    if (h > maxh) h = maxh;
    int x = pt.x + ui_scale(18), y = pt.y + ui_scale(20);
    if (x + w > wa.right) x = pt.x - w - ui_scale(8);
    if (y + h > wa.bottom) y = pt.y - h - ui_scale(8);
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(s_tip.wnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    s_tip.showing = 1;
    s_tip.id = id;
    InvalidateRect(s_tip.wnd, NULL, TRUE);
}

void ui_idtip_tick(void) {
    // 回收已销毁列表的绑定（页面对话框是动态建删的）
    for (int i = g_nbinds - 1; i >= 0; i--)
        if (!IsWindow(g_binds[i].lv)) {
            memmove(&g_binds[i], &g_binds[i + 1],
                    sizeof(IdTipBind) * (size_t)(g_nbinds - i - 1));
            g_nbinds--;
        }
    if (!g_nbinds) {
        if (s_tip.showing) tip_hide();
        return;
    }

    POINT pt;
    GetCursorPos(&pt);
    // 光标下真正最上层的窗口：只看它，不再按绑定顺序挑第一个「可见且包含光标」的。
    // 否则弹窗盖住主窗口时（EnableWindow 不改 WS_VISIBLE，后面列表照样"可见"，
    // SendMessage 也不受禁用影响）会命中后面那个列表，卡片显示成别处的物品。
    HWND top = WindowFromPoint(pt);
    HWND hit = NULL;
    int row = -1, col = 0;
    for (int i = 0; i < g_nbinds; i++) {
        // 顶层窗口被禁用的（正被模态弹窗盖着）不接：那种窗口此刻不可交互，
        // 卡片留在旧编号上就是「显示后面的物品」。
        if (!IsWindowEnabled(GetAncestor(g_binds[i].lv, GA_ROOT))) continue;
        if (g_binds[i].lv != top) continue;
        LVHITTESTINFO ht;
        memset(&ht, 0, sizeof(ht));
        POINT c = pt;
        ScreenToClient(g_binds[i].lv, &c);
        ht.pt = c;
        SendMessageW(g_binds[i].lv, LVM_SUBITEMHITTEST, 0, (LPARAM)&ht);
        // 行上任意一列都算：编号从绑定的那一列取，别的列悬停也能看到这张卡
        if (ht.iItem >= 0 && (ht.flags & LVHT_ONITEM)) {
            hit = g_binds[i].lv;
            row = ht.iItem;
            col = g_binds[i].col;
        }
        break;
    }
    if (!hit) { tip_hide(); return; }
    u32 id = 0;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].lv == hit) {
            id = g_binds[i].get ? g_binds[i].get(row) : cell_id(hit, row, g_binds[i].col);
            break;
        }
    if (!id) { tip_hide(); return; }
    if (s_tip.id != id || s_tip.lv != hit || s_tip.row != row) {
        // 换了格子：重新计时
        s_tip.lv = hit;
        s_tip.row = row;
        s_tip.col = col;
        s_tip.id = id;
        s_tip.since = now_ms();
        if (s_tip.showing) tip_hide();
        return;
    }
    if (!s_tip.showing && s_tip.since && now_ms() - s_tip.since >= IDTIP_DELAY)
        tip_show(id);
}
