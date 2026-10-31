// pvf：PVF 解析（头 / 文件树 / StringTable / Lst / Stk）+ 字典 + 二进制缓存。
// port of dnfpkgtool/pvfReader.py + cacheManager.py
#pragma once
#include "util.h"

typedef struct {
    u32  fn, len, crc, off;
    u32  len_raw;        // 文件树里记录的原始长度（len 是 4 字节对齐后的）
    char *path;          // 小写、无前导 '/'
} Leaf;

// 紧凑字符串表：所有字符串存于同一块内存，NUL 分隔
typedef struct {
    char *blob;
    u32  *off;
    int   n;
    size_t blob_len;
} StrTab;

const char *stab_get(const StrTab *t, int i);     // 越界返回 ""
void        stab_free(StrTab *t);

// u32 键 → 名称（排序后二分）
typedef struct {
    u32  *key;
    u32  *stroff;
    int   n;
    StrTab names;
} IdTab;

const char *idtab_get(const IdTab *t, u32 key);   // 未命中返回 ""
void        idtab_free(IdTab *t);

typedef struct {
    int  *pct;           // 进度输出（可为 NULL）
    char *stage;         // 阶段文本缓冲（可为 NULL）
    int   stage_cap;
} PvfProg;

typedef struct {
    char  path[MAX_PATH];
    char  md5[33];
    char  uuid[64];
    u32   version, tree_len, tree_crc, num_files, data_start;

    HANDLE file, map;
    const u8 *base;
    size_t size;

    Leaf *leafs;
    int   nleaf;
    u32  *by_path;       // 按 path 排序的 leaf 下标
    int   nby_path;

    StrTab strings;
    IdTab  items;        // it_id → 名称
    IdTab  jobs;         // job → 名称
    StrTab seals;        // 下标 = 封印 id
    u32   *exp;
    int    nexp;
    StrTab avatar_hidden;
    IdTab  nstr;         // n_string id → .str 路径

    unsigned str_cp;     // stringtable 解码代码页（936/950）
    int      opened;
} Pvf;

int   pvf_open(Pvf *p, const char *path, PvfProg prog);   // 命中缓存秒开
void  pvf_close(Pvf *p);
void  pvf_decrypt(u8 *p, size_t n, u32 crc);              // 自反：解密与加密同一变换
int   pvf_loaded(const Pvf *p);
const Leaf *pvf_find(const Pvf *p, const char *path);
int   pvf_read(const Pvf *p, const Leaf *lf, Buf *out);

typedef struct {
    u32  id;
    char name[128];
    char kind[32];        // [seeking] / [hunt enemy] / [meet npc] / [condition under clear]
    u32  item[4];         // 需求物品（仅收集类）
    u32  cnt[4];
    int  nitem;
    int  lev_min, lev_max;
} QuestInfo;

const char *pvf_item_name(const Pvf *p, u32 id);
int         pvf_quest_info(Pvf *p, u32 id, QuestInfo *out);

// 任务材料（收集类任务要的东西）。同一件材料按「单个任务最多要多少个」记，
// 不累加 —— 不可能同时接所有任务。
typedef struct {
    u32 id;
    u32 cnt;      // 单个任务最多要多少个
    u32 quests;   // 有多少个任务要它
} QuestMat;

int pvf_quest_materials(Pvf *p, QuestMat **out);   // 返回条数，*out 由调用方 free
const char *pvf_job_name(const Pvf *p, int job);
const char *pvf_seal_name(const Pvf *p, int id);
u32         pvf_exp_for(const Pvf *p, int lev);
int         pvf_is_equip(const Pvf *p, u32 id);
int         pvf_item_count(const Pvf *p);
u32         pvf_item_at(const Pvf *p, int i, const char **name);   // 按下标枚举物品名

// 把一个 stk 内容转成人类可读文本
void pvf_stk_text(const Pvf *p, const void *c, size_t n, Str *out);

