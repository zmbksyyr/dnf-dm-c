// app：全局状态 g、配置、异步任务、日志。除 g 外无全局可变状态。
#pragma once
#include "db.h"
#include "grab.h"
#include "pvf.h"
#include "npk.h"

typedef struct {
    char host[64];
    int  port;
    char user[48];
    char pwd[64];
    int  charset;
    char pvf[MAX_PATH];
    char img_dir[MAX_PATH]; // ImagePacks2 目录（编号悬浮卡的图标来源）。空 = 自动猜
    int  font;          // 字号（pt）
    int  poll_sec;
    int  lev_max;       // 等级上限（GM 页「设成几级」和 +10 的封顶）。默认 85 —— 各端
                        // 版本不同，写死 100 会在 85 封顶的服上把等级写过头。
                        // settings.ini [gm] lev_max，范围 1~255
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
    u32    ban_chars_ver;  // 封停页角色清单换代（下拉要跟着重建）
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

    Npk    npk;            // 贴图包索引（后台建，建好前悬浮卡没有图标）
    int    npk_ok;
    int    npk_pct;
    char   npk_stage[64];

    Job    job_conn, job_load, job_pvf, job_poll, job_act, job_grab, job_bubble, job_npk;
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
    Vec    cre_ids;        // u32：宠物编号清单（来自 dnf_creature_info）

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
// 贴图包自测（--npk-test）：建索引并真解前 max_ents 条的前 3 帧，返回解出的图标数
int  app_npk_decode_all(const char *dir, int max_ents);
// 内部自检（--selftest）：不依赖窗口与数据库，返回非 0 表示有失败项
int  selftest_run(void);
void app_tick(void);
void app_shutdown(void);
void app_log(const char *fmt, ...);
void app_warn(const char *fmt, ...);       // 行首加「警告」
void app_err(const char *fmt, ...);        // 行首加「错误」
void app_log_session(void);                // 每次连接换一个日志会话号
void app_log_block(const char *s);        // 多行文本按行写日志
void app_status(const char *fmt, ...);
int  app_confirm(const char *text);
void app_info(const char *text);

// 任务
int  job_start(Job *j, int (*fn)(void *ctx, Err err), void *ctx);
int  job_poll(Job *j, Err err);      // 返回 1 表示本轮刚结束（成功或失败）
int  job_stop(Job *j);               // 1 = 线程真的停了；0 = 30 秒没等到，还在跑
int  job_running(const Job *j);
int  job_stop_by(Job *j, u64 deadline);  // 共用截止时刻的停机（整批一起等）
int  app_db_busy(void);                  // 有作业占着那条 MySQL 连接

// 会话边界（断开 / 重连）：丢掉所有还没开始的动作，并让页面复位自己的队列与闸。
// 页面在自己的 create 里调 app_on_drop() 注册，新增页面不用回头改 app.c。
void app_on_drop(void (*fn)(void));
// 连接被服务端掐断时，按钮的可用状态要跟着变（main.c 实现）
void app_ui_refresh_buttons(void);
void app_drop_queued(void);

// 日志：先攒在内存里，1s 或 8KB 落一次盘（按天分文件）
void app_log_flush(void);
void app_log_path(char *out, int cap);
void app_config_dir(char *out, int cap);          // <exe>/config（清除配置用）
void app_cfg_save(void);                          // 把内存设置落盘（settings.ini）
void app_clear_config_and_exit(void);             // 标记清除配置并退出程序

// 连接 / 角色 / 数据
void app_connect(void);
// 取消待进行的自动重连（用户手动连接/断开时调）
void app_reconnect_cancel(void);
void app_disconnect(void);
void app_list_chars(const char *kw);
void app_select_char(int idx);
void app_reload(void);
// 载入状态由 app_load.c 维护，app_tick 只调这几个入口
void app_load_pending(void);             // 补上载入期间点过的角色
void app_load_poll(void);                // 载入作业的结果（快照换代 / 失败清暂存）
void app_load_kick_if_wanted(void);      // 连接空闲后补发挂起的载入
void app_load_drop(void);                // 断开/换库：作废挂起意图与暂存区
int  app_load_take_reload(void);         // 取走「写失败后要重载」的请求
void app_reload_later(void);       // 任务线程里用：让下一次 app_tick 重读当前角色
int  app_sync_char(void);          // 写后校验（job 线程）：结果暂存，返回 1=与缓存一致
void app_sync_apply(void);         // UI 线程：把暂存的校验结果搬到 g.*（job_poll 后调）
void app_row_sync(void);           // 把 g.cur 的名字/等级/职业写回左栏那一行
void app_load_pvf(const char *path);
void app_load_npk(const char *dir);        // ImagePacks2 目录（悬浮卡图标）
void app_npk_tick(void);                   // app_tick 里轮询贴图包扫描作业
void app_item_tip(u32 id, Str *out);       // 悬浮卡详情文本
HBITMAP app_item_icon(u32 id, int *w, int *h);   // 悬浮卡图标（调用方勿删）
void app_grab_fast_once(void);     // 连续取码一轮：只走特征码快路径，不回退扫堆
void app_grab_auto(int on);        // 连续取码总开关（复选框与热键共用，改完即写 settings.ini）

// 页面数据
Blob *app_page_blob(int page);
const char *app_page_table(int page);
const char *app_page_field(int page);

// 即时落库：改完立刻生效到内存，并在 200ms 后合并写库（不弹确认、不留草稿）
void app_item_set(int page, int slot, const Item *it);
// 这个容器的这一格能不能放该类型：背包按区段，穿戴/宠物各有允许类型
int  app_item_slot_ok(int page, int slot, u8 type);
int  app_pending_page(int page);                 // 整页写回（清空整页等）；0 = 被拒
                                                 // （在线/未载入）：调用方要把快照重读回来
int  app_page_busy(int page);                    // 该页有未完成/进行中的写入
void app_flush_pending(void);                    // 立刻把攒着的写操作发出去（关窗/切角色前）
void app_save_draft(void);                       // 同上，旧名字，别处还在调

// 展示
const char *app_item_name(u32 id, char *buf, int cap);
// 物品类型由 PVF 目录决定（equipment/ → 装备、stackable/quest → 任务材料 …），
// 不是界面上给人挑的选项：类型字节错了，同一个编号会被按另一套规则解析，
// 轻则显示成别的东西，重则整格读不出来。
u8          app_item_type(u32 id);       // IT_* 字节；没 PVF / 查不到时兜消耗品，不会返回 0
const char *app_item_type_text(u32 id);  // 「装备」「消耗品」…；兜底时带括号说明来源
u32         app_item_stack_limit(u32 id);// [stack limit]；0 = 该编号没声明上限

// 邮件附件种类：普通 / 时装 / 宠物。
// 三者 ID 空间互相独立，落到不同的表：普通把数量写进 postal.add_info，
// 时装 / 宠物要先在 user_items / creature_items 建一行、拿 ui_id 当 add_info。
// 邮件页和发放页共用一个判据 —— 各写一份迟早分叉，分叉的后果是发出去的东西
// 收件人取不出来。
enum { ACK_ITEM = 0, ACK_AVATAR = 1, ACK_CREATURE = 2 };
int  app_attach_kind(u32 id);
const char *app_job_text(int job);
const char *app_pvp_text(int grade);   // PVP 段位名（0=10级 … 34=斗神）
void app_money_text(u64 v, char *buf, int cap);

// 异步写操作：忙时排队（复制 ctx），空闲后自动执行
int  app_act_n(const char *what, ActFn fn, void *ctx, int ctxsz);
int  app_act_idle(void);                 // 连接空闲且没有排队动作：能直接发
void app_act_clear_queue(void);          // 丢掉还没开始的动作（会话边界）
int  app_queue_pending(void);            // 队列里还有没开始的动作
int  app_act_take_queued(char *what, int cap, ActFn *fn, void **ctx, int *ctxsz);
int  app_act_run(const char *what, ActFn fn, void *ctx, int ctxsz);
void app_job_pct_reset(void);            // 长任务进度清零
// 取消正在跑的长任务（备份/还原/全服扫描）；没有可取消的任务时只提示
void app_cancel_current(void);
#define app_act(what, fn, ctx) app_act_n((what), (fn), (void *)(ctx), (int)sizeof(*(ctx)))
// 无上下文的动作：不要用 app_act(..., NULL)，那会对 void 取 sizeof
#define app_act_null(what, fn) app_act_n((what), (fn), NULL, 0)

// 泡点：开关 / 间隔秒 / 目标人群 / 多奖励（赠送点券·D点·D币·物品）/ 生效时长。
// 改完立即写回 settings.ini，计时从改的那一刻重来。
// 结算由 app_tick 驱动，只在本工具开着、且连库期间发生（关掉不补算）。
// hours：生效时长小时数，0=持续（不自动停），可填任意 1~8760。
void app_bubble_apply(int on, int sec, int target, int coin, int dpoint, int dcoin,
                      u32 item, int itemnum, u32 tno, int hours);
void app_bubble_status(char *out, int cap);   // 一行人话，给 GM 页底部标签用
void app_bubble_tick(void);                   // 每个 UI tick 调一次：到点结算
void app_bubble_export(void);                // 把泡点结算记录导出为 txt

// 最近计划 / 结算流水（最多 20 条，最新的在第 0 条）。
// 「计划」= 一次应用；「完成发放 / 无人可发 / 发放失败」= 一次结算结果。
// 状态取值固定几个，UI 直接显示，不做二次映射。
typedef struct {
    char t[12];      // HH:MM:SS
    char tg[16];     // 目标人群
    char what[88];   // 内容摘要（间隔/奖励/对象数/错误）
    char st[16];     // 等待发放 / 完成发放 / 无人可发 / 发放失败 / 已停止 / 已到时长
} BubblePlan;
int  app_bubble_plan_count(void);
const BubblePlan *app_bubble_plan_at(int i);   // i=0 是最新一条
int  app_bubble_plan_ver(void);                // 每次增减都 +1，页面据此决定要不要重填

// 发放
// 一次发送的队列上限：界面上所有「加入队列」的批量入口也用它，免得攒出
// 发不出去的队列
#define APP_GRANT_MAX 256
void app_grant_add(u32 id, u32 num);           // num = 这次要几个，0 当 1
// 批量入队：不刷状态栏。队列里已有的把数量取大（不重复加行）。
// 返回 1 = 新增，2 = 沿用旧行、数量补大，0 = 原样没动。
int  app_grant_add_new(u32 id, u32 num);
int  app_grant_send(int by_mail);

// 整库备份 / 还原（后台执行）
int  app_db_backup(const char *path);        // 1 = 已提交（可能排队）
int  app_db_backup_char(void);               // 只备份当前选中角色
int  app_db_restore(const char *path);
// 长任务状态（进度窗轮询这几个）
int  app_job_progress(void);                 // 0..100；-1 = 该任务不报进度
int  app_act_running(void);
int  app_act_running_named(const char *what);  // 正在跑的就是这个动作（再点入口时重开进度窗）
const char *app_act_name(void);
// 还原前自动整库备份，再执行 SQL；GUI 与命令行共用这一条路径
int  app_restore_file(const char *path, int *stmts, Err err);

// 角色克隆（后台执行，单个事务，失败整体回滚）
void app_clone_char(void);

// 任务模版：当前角色的任务进度导出成文本 / 导入一份模版覆盖当前角色
void app_quest_export(void);
void app_quest_import(const char *path);
