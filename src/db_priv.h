// db_priv.h：db.c / db_mail.c / db_admin.c 共用的内部辅助函数。
// 这些函数只在 db_* 编译单元之间共享，不对外暴露（对外接口在 db.h）。
#pragma once
#include "db.h"

// SQL 执行
int  run_aff(Db *d, Str *sql, u64 *aff);
int  run(Db *d, Str *sql);
int  select1(Db *d, Str *sql, MyResult *r);

// 字符串/结果集辅助
void cp_str(char *dst, int cap, const char *src);
void cp_text(char *dst, int cap, const MyResult *r, int row, int col);
int  set_names(Db *d, const char *cs);

// 整库备份辅助（db_admin.c 用）
int  is_bin_type(const char *t);
int  is_num_type(const char *t);
void sql_lit(Str *o, const char *p, int n, int binary);
int  split_tbl(const char *full, char *sc, int sccap, char *tb, int tbcap);

// 邮件附件清理（db_mail.c 定义，db_admin.c 的 purgue_quest_items 也用）
void mail_drop_attach(Db *d, u64 postal_id);
