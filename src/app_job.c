// app_job：作业线程原语（起、停、轮询）。
//
// 一条作业 = 一个后台线程 + 一个三态机（1 跑 / 2 成功 / 3 失败）。
// 约定：调用方起完作业后必须在 UI tick 里 poll，poll 之前结果（含错误）不可见。
#include "app.h"
#include "ui.h"      // g_main：停作业时要临时禁用主窗口并泵消息

static DWORD WINAPI job_thread(LPVOID param) {
    Job *j = (Job *)param;
    Err err = {0};
    int ok = j->fn(j->ctx, err);
    if (ok) {
        j->err[0] = 0;
        InterlockedExchange(&j->state, 2);
    } else {
        snprintf(j->err, ERRBUF, "%s", err[0] ? err : "未知错误");
        InterlockedExchange(&j->state, 3);
    }
    return 0;
}

int job_running(const Job *j) { return j->state == 1; }

int job_start(Job *j, int (*fn)(void *ctx, Err err), void *ctx) {
    if (j->state == 1) return 0;                 // 还在跑：不能重入
    if (j->state == 2 || j->state == 3) {
        // 上一轮跑完了但没人 poll。不先收掉的话，下面 CloseHandle 会连同那份
        // 结果一起扔掉——错误信息就这么静默消失。
        Err drop;
        job_poll(j, drop);
    }
    if (j->th) { CloseHandle(j->th); j->th = NULL; }
    j->fn = fn;
    j->ctx = ctx;
    j->progress = 0;
    j->stage[0] = 0;
    j->err[0] = 0;
    InterlockedExchange(&j->state, 1);
    j->th = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (!j->th) {
        InterlockedExchange(&j->state, 3);
        snprintf(j->err, ERRBUF, "创建线程失败");
        return 0;
    }
    return 1;
}

int job_poll(Job *j, Err err) {
    LONG st = j->state;
    if (st != 2 && st != 3) return 0;
    if (j->th) {
        WaitForSingleObject(j->th, INFINITE);
        CloseHandle(j->th);
        j->th = NULL;
    }
    snprintf(err, ERRBUF, "%s", j->err);       // err 是数组参数，不可能为 NULL
    InterlockedExchange(&j->state, 0);
    return 1;
}

// 共用同一条 MySQL 连接的作业：协议是严格串行的，任一在跑，别的必须等。
// 只互斥 app_act 是不够的——轮询/载入撞上写库会在同一连接上交错收发，包就串了
int app_db_busy(void) {
    return job_running(&g.job_conn) || job_running(&g.job_load) ||
           job_running(&g.job_act) || job_running(&g.job_poll) ||
           job_running(&g.job_bubble);
}

// 等作业线程退出。等的同时泵消息：停机可能持续几十秒，不泵的话窗口会变白、
// 系统把它标成「未响应」。消息里只跳过主窗口的 WM_TIMER —— 那一拍会去 poll
// 正在等的这个作业，和停机抢状态机。
static int wait_job(Job *j, u64 deadline) {
    int was = g_main && IsWindowEnabled(g_main);
    if (g_main) EnableWindow(g_main, FALSE);      // 停机期间不接受新操作
    int ok = 0;
    for (;;) {
        u64 now = now_ms();
        if (now >= deadline) { ok = WaitForSingleObject(j->th, 0) == WAIT_OBJECT_0; break; }
        DWORD left = (DWORD)(deadline - now);
        DWORD r = MsgWaitForMultipleObjects(1, &j->th, FALSE, left, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { ok = 1; break; }
        if (r == WAIT_TIMEOUT) { ok = 0; break; }
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_TIMER && m.hwnd == g_main) continue;
            if (!IsDialogMessageW(g_main, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
    if (g_main && was) EnableWindow(g_main, TRUE);   // 原本就禁用（模态中）就别解锁
    return ok;
}

int job_stop_by(Job *j, u64 deadline) {
    if (j->th) {
        if (!wait_job(j, deadline)) {
            // 超时：线程还在跑，不能关句柄（线程退出时句柄才可关）。
            // 必须把「没等到」告诉调用方：它接着要释放的正是这个线程正在读的
            // 收发包缓冲（db_close → free(m->buf)），不看返回值就是一边 free 一边 recv。
            app_warn("任务线程未在期限内退出，放弃等待（句柄保留）");
            return 0;                     // 保留 j->th 和 state，等下次 job_poll 收
        }
        CloseHandle(j->th);
        j->th = NULL;
    }
    InterlockedExchange(&j->state, 0);
    return 1;
}

int job_stop(Job *j) { return job_stop_by(j, now_ms() + 30000); }
