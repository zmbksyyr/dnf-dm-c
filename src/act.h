// act：异步写动作的「命令表」。
//
// GM / 封停 / 活动三个页面都用 app_act("...", char_work, &kc) 提交：页面只负责把
// 参数填进 kc，真正落到哪张表由这里的 kind 分支决定。抽出来单独一个文件，是因为
// 这张表本来就不属于任何一个页面——它被三个页面共用，而且和界面布局毫无关系。
#pragma once
#include "app.h"

enum {
    K_RENAME = 1, K_LEV, K_MONEY, K_COIN, K_CERA, K_JOB, K_ACCM,
    K_SP, K_QP, K_NOWEIGHT, K_SLOT, K_UNLOCK, K_EXPERT, K_FATIGUE, K_DUNGEON, K_PUNISH, K_QCLR,
    K_PVP, K_BAN, K_UNBAN, K_BANLIST, K_EVLIST, K_EVADD, K_EVDEL,
    K_RESETPWD, K_UNLCREATE
};

typedef struct {
    int  kind;
    u32  cno;        // 归属角色：入队时定下，任务线程不再读 g.cur
    u32  mid;
    int  i1, i2, i3;
    u64  u1, u2;
    char s[160];
} Ctx;

// 各页面拼好参数后就地提交。app_act 会把整个结构拷进队列，所以这个共享实例
// 在下一次提交之前被改写是安全的。
extern Ctx kc;

int char_work(void *ctx, Err err);
void act_init(void);             // 初始化列表暂存 Vec（app_init 调一次）
void act_free(void);             // 释放列表暂存 Vec（app_shutdown 调一次）
void act_apply_lists(void);   // UI 线程：把暂存列表搬到 g.*（job_poll 后调）
