// ui_progress：长任务的模态进度窗。
//
// 默认挡住主窗口，防止在备份/还原/扫描期间误点别的按钮；「后台运行」退回原来的
// 状态栏 + 工具栏取消，需要边跑边看别的页面（或把动作排进队列）的人有出口。
//
// 进度来自 app_job_progress()，任务结束（app_act_running 转 false）窗口自己关。
// app_tick 仍由主窗口的 WM_TIMER 驱动——父窗口被禁用不影响定时器，确认框一直是这么跑的。
#include "ui.h"
#include "db.h"

#define PG_ID_CANCEL 2001
#define PG_ID_BG     2002

static int s_pg_active;

typedef struct {
    HWND bar, txt, btn_cancel;
    int  started;      // 见过「在跑」没有（排队中的动作先显示排队）
    int  cancelling;
} PgCtx;

static LRESULT CALLBACK pg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, void *ctx) {
    PgCtx *p = (PgCtx *)ctx;
    (void)h;
    (void)lp;
    switch (m) {
    case WM_TIMER: {
        if (wp != 1) return -1;
        if (app_act_running()) p->started = 1;
        int pct = app_job_progress();
        if (pct < 0) pct = 0;
        SendMessageW(p->bar, PBM_SETPOS, (WPARAM)pct, 0);
        char line[160];
        if (p->cancelling) snprintf(line, sizeof(line), "正在取消…（等当前这一步返回）");
        else if (!p->started) snprintf(line, sizeof(line), "排队中…前面还有动作在跑");
        else if (app_job_progress() < 0) snprintf(line, sizeof(line), "正在执行…");
        else snprintf(line, sizeof(line), "%d%%", pct);
        ui_text(p->txt, line);
        // 结束判据：任务不在了，而且要么见过它在跑，要么队列也空了
        //（直接起的小任务可能在第一个 80ms tick 之前就结束了，不能只等 started）
        if (!app_act_running() && (p->started || !app_queue_pending())) ui_dlg_end(1);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == PG_ID_CANCEL) {
            p->cancelling = 1;
            app_cancel_current();
            EnableWindow(p->btn_cancel, FALSE);
            return 0;
        }
        if (LOWORD(wp) == PG_ID_BG) { ui_dlg_end(2); return 0; }
        return -1;
    case WM_CLOSE:
        // 点 X 等同「后台运行」：窗口关掉、任务继续，工具栏的「取消」还在
        ui_dlg_end(2);
        return 0;
    }
    return -1;
}

// 进度窗开着的时候，页面刷新要避开：别在它里面再弹出别的对话框（会套两层模态）
int ui_progress_active(void) { return s_pg_active; }

// 返回值：1 = 任务已结束；2 = 转后台运行。调用方一般不用区分。
int ui_progress_modal(const char *title) {
    // 已经有模态对话框时不再套一层：结果窗里的「重新扫描」、清除等都在对话框里，
    // 再压一个进度窗会让关窗顺序变得难以预料，那种场景沿用状态栏进度即可。
    if (ui_dlg_active()) return 1;
    PgCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    HWND d = ui_dlg_new(g_main, title, 420, 148, pg_proc, &ctx);
    if (!d) return 1;
    ctx.bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, d, NULL, NULL, NULL);
    SendMessageW(ctx.bar, PBM_SETRANGE32, 0, 100);
    SendMessageW(ctx.bar, PBM_SETPOS, 0, 0);
    ctx.txt = ui_label(d, "准备中…", 0);
    ctx.btn_cancel = ui_button(d, "取消", PG_ID_CANCEL);
    HWND bg = ui_button(d, "后台运行", PG_ID_BG);
    ui_tip(d, ctx.btn_cancel, "在下一个安全边界停下：还原会整体回滚，备份不保留半截文件");
    ui_tip(d, bg, "关掉这个窗口，任务继续；之后可用工具栏的「取消」");

    ui_place(ctx.bar, ui_scale(24), ui_scale(20), ui_scale(372), ui_scale(20));
    ui_place(ctx.txt, ui_scale(24), ui_scale(50), ui_scale(372), ui_scale(22));
    ui_place(bg, ui_scale(180), ui_scale(100), ui_scale(110), ui_scale(26));
    ui_place(ctx.btn_cancel, ui_scale(300), ui_scale(100), ui_scale(96), ui_scale(26));

    SetTimer(d, 1, 80, NULL);
    s_pg_active = 1;
    int ret = ui_dlg_run(d, g_main);
    s_pg_active = 0;
    return ret == 0 ? 1 : ret;
}
