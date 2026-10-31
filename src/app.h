// app：全局状态 g、配置、异步任务、日志。除 g 外无全局可变状态。
#pragma once
#include "db.h"
#include "grab.h"
#include "pvf.h"

typedef struct {
    char host[64];
    int  port;
    char user[48];
    char pwd[64];
    int  charset;
    char pvf[MAX_PATH];
    int  font;          // 字号（pt）
    int  poll_sec;
    char hotkey[32];
    int  grab_auto;
    char grab_proc[64];
    int  bubble_on;
    int  bubble_sec;     // 结算间隔（秒）
    int  bubble_target;  // 0=仅在线 1=全服(含离线) 2=指定角色 3=指定账号
    int  bubble_coin;    // 赠送点券 (inventory.coin)
    int  bubble_dpoint;  // D点 (cash_cera.cera)
    int  bubble_dcoin;   // D币 (cash_cera_point.cera_point)
    u32  bubble_item;    // 物品 ID（0=不发）
    int  bubble_itemnum; // 物品数量
    u32  bubble_tno;     // 指定角色 charac_no / 指定账号 m_id
    // 生效时长：原来存的是「每天几点到几点」（分钟），表达不了「这次活动跑 3 小时就停」。
    // 现在存档位 + 绝对截止时刻：点「应用」那一刻算出 until，重启工具后仍按真实时间判断，
    // 不会因为重开一次就把时长又续一遍。
    int  bubble_hours;   // 档位：0=持续（不自动停），1/3/5/7 小时
    u64  bubble_until;   // 绝对截止时刻（time_t 秒），0 = 持续
} Config;

// 一次性后台任务：UI 只轮询 state，不阻塞
typedef struct {
    HANDLE th;
    volatile LONG state;   // 0 空闲 1 运行 2 成功 3 失败
    Err err;
    int  progress;
    char stage[64];
    int (*fn)(void *ctx, Err err);
    void *ctx;
} Job;

typedef int (*ActFn)(void *ctx, Err err);

typedef struct {
    u32  id;
    char name[64];
    u32  num;
    u8   type, enhance, forge, seal, seal_cnt, inc_type;
    u16  inc_val, dur;
} NamedId;

typedef struct {
    Config cfg;
    char   cfg_path[MAX_PATH];

    Db     db;
    int    db_ok;
    Err    db_err;

    Vec    chars;          // Charac：左栏列表的数据源，轮询独占维护
    u32    char_total;
    int    sel;
    Charac cur;            // 当前角色的「快照」：选角色时从库里读一次，写入时就地更新
    int    char_ok;
    CharData data;         // 同上，当前角色的容器/标量快照
    u32    data_ver;       // 快照换代（载入 / 重载 / 写后校验不符），页面据此重填
    u32    chars_ver;      // 左栏列表重排
    u32    act_ver;        // 每次写操作完成自增
    u32    lists_ready;    // 各列表页是否已取过数据（位掩码 L_*）

    u32    pend_mask;      // 待写库的容器页（位 = 页号），200ms 内的编辑合并成一次写
    u64    pend_ms;
    u32    write_mask;     // 正在写的容器页
    u32    write_done;     // 上次成功写入的容器页（界面显示"已写入"用）

    Pvf    pvf;
    int    pvf_ok;
    int    pvf_pct;
    char   pvf_stage[64];

    Job    job_conn, job_load, job_pvf, job_poll, job_act, job_grab, job_bubble;
    char   act_what[64];

    GrabHit grab;          // 最近一次取码结果
    u32     grab_ver;      // 每次取码完成自增，界面据此回填

    Vec    grant;          // NamedId
    Vec    recent;         // NamedId

    Vec    mails;          // MailRow
    Vec    pets;           // PetRow
    Vec    avatars;        // AvatarRow
    Vec    quests;         // QuestRow
    Vec    bans;           // BanRow
    Vec    ban_chars;      // Charac：封停页的角色选择器数据源
    Vec    events;         // EventRow
    Vec    event_info;     // EventInfo

    char   status[512];    // 状态栏
    // 表/列自检报告：连接作业在后台线程里写，job_poll 等到线程结束才由 UI 读，
    // 所以这里不需要锁
    char   self[2048];
    int    self_bad;       // 0 = 全部就位；>0 = 问题条数
    int    quit;
} App;

// lists_ready 位
enum { L_MAILS = 1, L_PETS = 2, L_AVAS = 4, L_QUESTS = 8, L_BANS = 16, L_EVENTS = 32,
       L_BANCHARS = 64 };

extern App g;

void app_init(void);
void app_tick(void);
void app_shutdown(void);
void app_log(const char *fmt, ...);
void app_log_block(const char *s);        // 多行文本按行写日志
void app_status(const char *fmt, ...);
int  app_confirm(const char *text);
void app_info(const char *text);

// 任务
int  job_start(Job *j, int (*fn)(void *ctx, Err err), void *ctx);
int  job_poll(Job *j, Err err);      // 返回 1 表示本轮刚结束（成功或失败）
void job_stop(Job *j);
int  job_running(const Job *j);

// 日志：先攒在内存里，1s 或 8KB 落一次盘（按天分文件）
void app_log_flush(void);
void app_log_path(char *out, int cap);

// 连接 / 角色 / 数据
void app_connect(void);
void app_disconnect(void);
void app_list_chars(const char *kw);
void app_select_char(int idx);
void app_reload(void);
void app_reload_later(void);       // 任务线程里用：让下一次 app_tick 重读当前角色
int  app_sync_char(void);          // 写后校验（job 线程）：结果暂存，返回 1=与缓存一致
void app_sync_apply(void);         // UI 线程：把暂存的校验结果搬到 g.*（job_poll 后调）
void app_row_sync(void);           // 把 g.cur 的名字/等级/职业写回左栏那一行
void app_load_pvf(const char *path);
void app_grab_fast_once(void);     // 连续取码一轮：只走特征码快路径，不回退扫堆
void app_grab_auto(int on);        // 连续取码总开关（复选框与热键共用，改完即写 settings.ini）

// 页面数据
Blob *app_page_blob(int page);
const char *app_page_table(int page);
const char *app_page_field(int page);

// 即时落库：改完立刻生效到内存，并在 200ms 后合并写库（不弹确认、不留草稿）
void app_item_set(int page, int slot, const Item *it);
void app_pending_page(int page);                 // 整页写回（清空整页等）
int  app_page_busy(int page);                    // 该页有未完成/进行中的写入
void app_flush_pending(void);                    // 立刻把攒着的写操作发出去（关窗/切角色前）
void app_save_draft(void);                       // 同上，旧名字，别处还在调

// 展示
const char *app_item_name(u32 id, char *buf, int cap);
const char *app_job_text(int job);
const char *app_pvp_text(int grade);   // PVP 段位名（0=10级 … 34=斗神）
void app_money_text(u64 v, char *buf, int cap);

// 异步写操作：忙时排队（复制 ctx），空闲后自动执行
int  app_act_n(const char *what, ActFn fn, void *ctx, int ctxsz);
int  app_act_idle(void);                 // 连接空闲且没有排队动作：能直接发
#define app_act(what, fn, ctx) app_act_n((what), (fn), (void *)(ctx), (int)sizeof(*(ctx)))

// 泡点：开关 / 间隔秒 / 目标人群 / 多奖励（赠送点券·D点·D币·物品）/ 生效时长。
// 改完立即写回 settings.ini，计时从改的那一刻重来。
// 结算由 app_tick 驱动，只在本工具开着、且连库期间发生（关掉不补算）。
// hours：生效时长档位，0=持续，1/3/5/7 小时（从点「应用」这一刻起算）。
void app_bubble_apply(int on, int sec, int target, int coin, int dpoint, int dcoin,
                      u32 item, int itemnum, u32 tno, int hours);
void app_bubble_status(char *out, int cap);   // 一行人话，给 GM 页底部标签用
void app_bubble_export(void);                // 把泡点结算记录导出为 txt

// 发放
void app_grant_add(u32 id);
int  app_grant_send(int by_mail);

// 整库备份 / 还原（后台执行）
void app_db_backup(const char *path);
void app_db_backup_char(void);          // 只备份当前选中角色
void app_db_restore(const char *path);

// 角色克隆（后台执行，单个事务，失败整体回滚）
void app_clone_char(void);

// 任务模版：当前角色的任务进度导出成文本 / 导入一份模版覆盖当前角色
void app_quest_export(void);
void app_quest_import(const char *path);
