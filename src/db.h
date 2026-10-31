// db：DNF 业务读写。唯一出现 SQL 的地方。
// port of dnfpkgtool/sqlManager2.py
#pragma once
#include "item.h"
#include "mysql.h"

typedef struct {
    u32  no, mid;
    char name[64];        // UTF-8
    char account[48];     // 所属账号名（d_taiwan.accounts），列表展示与搜索用
    int  job, grow, lev;
    int  online;
} Charac;

typedef struct {
    Blob inv, equip, creature, cargo, jewel, account_cargo;
    u32  inv_cap, cargo_cap;
    u64  money, coin, account_money, cera, cera_point;
    int  sp, sp2, tp, tp2, qp;
    int  pvp_grade, pvp_win, pvp_point;
    char vip[64];        // 账号级 VIP 标记（accounts.VIP，varchar，按文本处理）
    int  loaded;
} CharData;

typedef struct {
    u64  postal_id;
    u32  item_id, add_info;
    u64  gold;
    int  avata, creature;
    char time[24];
} MailRow;

typedef struct {
    u32  ui_id, it_id;
    int  type, stomach;
    char name[64];
    char expire[24];
} PetRow;

typedef struct {
    u32  ui_id, it_id;
    int  hidden;
    char expire[24];
} AvatarRow;

typedef struct {
    u32  mid;
    int  type, value;
    u32  charac_no;      // 角色封禁时的角色编号（type=2 时有效）
    char account[48], char_name[48], ip[24], start[24], end[24], reason[160];
} BanRow;

// 活动：log 是「当前启用的活动」，info 是「活动字典」
typedef struct {
    u64  log_id;
    u32  type;
    i64  p1, p2;
    char name[64];
    char explain[104];
} EventRow;

typedef struct {
    u32  id;
    char name[40];
    char explain[104];
} EventInfo;

typedef struct {
    int  slot;
    u32  id, trigger;
    char name[64];        // PVF 里查到的任务名，查不到为空
    char need[72];        // 需求物品摘要，如「火焰结晶 x100」
    u32  need_id[4], need_cnt[4];
    int  nneed;
} QuestRow;

typedef struct {
    MySql conn;
    char  host[64], user[48], pwd[64];
    int   port, charset;
    Err   err;
} Db;

// 连接
int  db_open(Db *d, const char *host, int port, const char *user, const char *pwd, int charset, Err err);
void db_close(Db *d);
int  db_alive(const Db *d);
// 表/列自检：拿 information_schema 对一遍本工具需要的表与列，结果写进 out。
// 返回问题条数（0 = 就位）。报告是多行文本，每条以 \r\n 结尾。
int  db_selfcheck(Db *d, char *out, int cap, Err err);
// 事务：一个动作写多张表时用，失败整体回滚
int  db_tx_begin(Db *d, Err err);
int  db_tx_commit(Db *d, Err err);
void db_tx_rollback(Db *d);

// 角色
int  db_list_chars(Db *d, Vec *out, int limit, const char *kw, Err err);   // Vec<Charac>
u32  db_count_chars(Db *d, const char *kw);
int  db_poll_chars(Db *d, Vec *out, int limit, Err err);   // 列表+在线+当前角色，一条查询
int  db_is_online(Db *d, u32 mid);
// 封停页专用：全服角色 + 精确在线位（与 db_poll_chars 同口径）。
// online_only=1 时只留「账号在线且正在玩该角色」的，用来做「加载在线」。
int  db_list_ban_chars(Db *d, Vec *out, int online_only, int limit, Err err);   // Vec<Charac>

// 数据容器 + 状态
int  db_load_data(Db *d, const Charac *c, CharData *out, Err err);
void db_free_data(CharData *cd);
// 写操作后回读（界面刷新用）：只取显示字段，不碰 blob
int  db_read_charac(Db *d, u32 no, Charac *out, Err err);            // 名字/职业/觉醒/等级
int  db_read_state(Db *d, const Charac *c, CharData *out, Err err);  // 金币/复活币/点券/PVP/SP/QP
int  db_save_blob(Db *d, const Charac *c, const char *table, const char *field,
                  const void *raw, size_t n, Err err);

// 基础信息
int  db_set_name(Db *d, const Charac *c, const char *name, Err err);
int  db_name_taken(Db *d, const char *name, u32 except_no);
int  db_set_job(Db *d, const Charac *c, int job, int grow, Err err);
int  db_set_lev(Db *d, const Charac *c, int lev, i64 exp, Err err);   // exp<0 表示不改经验
int  db_set_money(Db *d, const Charac *c, u64 money, Err err);
int  db_set_coin(Db *d, const Charac *c, u64 coin, Err err);
int  db_set_account_money(Db *d, const Charac *c, u64 money, Err err);
int  db_set_cera(Db *d, const Charac *c, u64 cera, u64 point, Err err);

// 泡点结算：给每个在线账号的赠送点券加 val，paid 带回命中账号数。
// 0 行是正常结果（没人在线），不是失败。
typedef struct {
    int  target;      // 0=仅在线 1=全服(含离线) 2=指定角色 3=指定账号
    int  coin;        // 赠送点券 (inventory.coin)
    int  dpoint;      // D点 (cash_cera.cera)
    int  dcoin;       // D币 (cash_cera_point.cera_point)
    u32  item;        // 物品 ID（0=不发）
    int  itemnum;     // 物品数量
    u32  tno;         // 指定角色 charac_no / 指定账号 m_id
} BubbleCfg;
int  db_bubble_tick(Db *d, const BubbleCfg *cfg, u32 *paid, Err err);
int  db_set_delete_flag(Db *d, const Charac *c, int deleted, Err err);

// GM 一键
int  db_set_sp(Db *d, const Charac *c, int sp, int sp2, int tp, int tp2, Err err);
int  db_set_qp(Db *d, const Charac *c, int qp, Err err);
int  db_set_pvp(Db *d, const Charac *c, int grade, int win, int point, Err err);
int  db_no_weight(Db *d, const Charac *c, Err err);
int  db_open_equip_slot(Db *d, const Charac *c, Err err);
int  db_unlock_lev_equip(Db *d, const Charac *c, Err err);
int  db_max_expert(Db *d, const Charac *c, Err err);
int  db_set_fatigue(Db *d, const Charac *c, Err err);      // 补满疲劳
int  db_unlock_dungeon(Db *d, const Charac *c, Err err);   // 全图全难度
int  db_reset_punish(Db *d, u32 mid, Err err);

typedef struct {
    u32  id, num;
    u8   type, enhance, forge, seal, seal_cnt, inc_type;
    u16  inc_val, dur;
} GrantItem;

// 邮件
int  db_list_mails(Db *d, const Charac *c, Vec *out, Err err);            // Vec<MailRow>
int  db_delete_mail(Db *d, u64 postal_id, Err err);
int  db_delete_all_mails(Db *d, const Charac *c, Err err);
int  db_send_text_mail(Db *d, const Charac *c, const char *sender, const char *text, Err err);
int  db_send_item_mail(Db *d, const Charac *c, const char *sender, const char *text,
                       u32 item_id, u32 num, Err err);
int  db_send_item_mail_ex(Db *d, const Charac *c, const char *sender, const char *text,
                          const GrantItem *g, int avata_flag, int creature_flag, Err err);
// 全服 / 在线批量发邮件：online_only=1 仅发给在线账号下的角色；sent 带回成功数。
// item_id=0 时只发文本（不带附件）。avata_flag/creature_flag 走宠物/时装邮件逻辑。
int  db_send_mail_bulk(Db *d, int online_only, const char *sender, const char *text,
                       u32 item_id, u32 num, int avata_flag, int creature_flag, u32 *sent, Err err);
// 全服软删所有邮件（delete_flag=1），镜像 sqlManager2.delete_all_mail_cNo(-1)
int  db_delete_all_mails_global(Db *d, Err err);

// 宠物 / 时装
int  db_list_pets(Db *d, const Charac *c, Vec *out, Err err);             // Vec<PetRow>
int  db_set_pet_name(Db *d, u32 ui_id, const char *name, Err err);
int  db_set_pet_full(Db *d, u32 ui_id, Err err);
int  db_set_pet_expire(Db *d, u32 ui_id, Err err);
int  db_delete_pet(Db *d, u32 ui_id, Err err);
int  db_list_avatars(Db *d, const Charac *c, Vec *out, Err err);          // Vec<AvatarRow>
int  db_set_avatar_hidden(Db *d, u32 ui_id, int hidden, Err err);
int  db_set_avatar_expire(Db *d, u32 ui_id, Err err);
int  db_delete_avatar(Db *d, u32 ui_id, Err err);

// 任务
int  db_list_quests(Db *d, const Charac *c, Vec *out, Err err);           // Vec<QuestRow>
int  db_clear_quests(Db *d, const Charac *c, Err err);
int  db_set_quest_slot(Db *d, const Charac *c, int slot, u32 id, u32 trigger, Err err);
int  db_purge_quest_items(Db *d, const Charac *c, const u32 *ids, int n, int *removed, Err err);

// 活动
int  db_list_events(Db *d, Vec *out, Err err);                            // Vec<EventRow>
int  db_list_event_info(Db *d, Vec *out, Err err);                        // Vec<EventInfo>
int  db_add_event(Db *d, u32 type, i64 p1, i64 p2, int days, Err err);
int  db_del_event(Db *d, u64 log_id, Err err);
int  db_clear_events(Db *d, Err err);     // 清空 dnf_event_log（全服活动停启用）
int  db_add_event_info(Db *d, u32 id, const char *name, const char *explain, Err err);  // 往活动字典插一条

// 封停
int  db_list_bans(Db *d, Vec *out, Err err);                              // Vec<BanRow>
int  db_ban(Db *d, u32 mid, int days, int type, int value, const char *reason, Err err);
// type=1 账号封禁（value 不用），type=2 角色封禁（value=charac_no）
int  db_unban(Db *d, u32 mid, int type, Err err);   // 按惩罚类型解封（type=0 全部清除）
int  db_find_mid(Db *d, const char *name, u32 *mid, Err err);

// 账号
int  db_online_accounts(Db *d, Vec *out);                                 // Vec<u32> mid
int  db_reset_pwd(Db *d, u32 mid, const char *pwd, Err err);               // 密码按 md5 存
int  db_unlock_create(Db *d, u32 mid, Err err);                            // 解除建角次数限制
int  db_get_vip(Db *d, u32 mid, char *out, int cap, Err err);              // 读 accounts.VIP
int  db_set_vip(Db *d, u32 mid, const char *vip, Err err);                 // 写 accounts.VIP

// 整库备份 / 还原（.sql 文本）
void db_dump_path(char *out, int cap);
void db_undo_flush(int force);      // 把本会话的写前快照落盘（force=0 时限流 15s）
int  db_dump_sql(Db *d, const char *path, int *tables, u64 *rows, Err err);
int  db_restore_sql(Db *d, const char *path, int *stmts, Err err);

// 全服错误物品扫描：容器槽位 / 邮件附件 / 时装宠物表行，一份清单全覆盖
typedef struct {
    u32  cno, mid;
    char name[64];
    char acc[48];
    char cont[16];
    int  slot;          // 容器内格位（1 起）；表行 / 邮件类为 0
    u32  id;
    char why[48];
    int  kind;          // 0 容器槽位 / 1 user_items 时装 / 2 creature_items 宠物 / 3 邮件附件
    u64  key;           // kind 1/2: ui_id；kind 3: postal_id
} ScanRow;
int  db_scan_bad(Db *d, Vec *out, int *nchar, ItemHasName has, void *hctx, Err err);
int  db_purge_bad(Db *d, Vec *rows, int *done, Err err);   // 清掉清单里的错误物品，返回成功条数

// 宠物/时装表内错误记录（it_id 查不到、角色已删）
typedef struct {
    u32  cno;
    char cname[64];
    int  avatar;        // 1 = user_items（时装），0 = creature_items（宠物）
    u64  ui_id;
    u32  it_id;
    char why[48];
} TabOrphan;
int  db_scan_tab_orphan(Db *d, Vec *out, ItemHasName has, void *hctx, Err err);
int  db_del_tab_orphan(Db *d, int avatar, u64 ui_id, Err err);

// 角色备份 / 还原（.sql 文本，还原直接走 db_restore_sql）
void db_backup_char_path(char *out, int cap, const Charac *c);
int  db_backup_char(Db *d, const Charac *c, const char *path, int *tables, u64 *rows, Err err);

// 任务模版：把角色的任务进度（已完成 / 进行中 / 任务商店属性）导成一份纯文本，
// 也能把模版套到别的角色上。文件是 key=value，blob 写成 X'..'，可以手工改。
void db_quest_tpl_path(char *out, int cap, const Charac *c);
int  db_export_quest(Db *d, const Charac *c, const char *path, Err err);
int  db_import_quest(Db *d, const Charac *c, const char *path, int *tables, Err err);

// 角色克隆：把一个角色连同它的背包/装备/技能/任务等整份复制成一个新角色。
// 全程在一个事务里，中途任何一步失败就整体回滚，不会留下半个角色。
// out_name 回填新角色名（调用方拿去提示 / 刷新），out_no 回填新 charac_no。
int  db_clone_char(Db *d, const Charac *c, char *out_name, int cap, u32 *out_no,
                   int *tables, u64 *rows, Err err);
