// item：61 字节物品结构 + 背包 blob（zlib 解压 / 切片 / 重打包）。
// port of dnfpkgtool/itemSlotFrame.py + sqlManager2.py: blob 部分
#pragma once
#include "util.h"

#define ITEM_SIZE 61

enum {
    IT_EMPTY = 0,
    IT_EQUIP = 1,
    IT_STACK = 2,
    IT_MATERIAL = 3,
    IT_QUEST = 4,
    IT_CREATURE = 5,
    IT_CREATURE_EQUIP = 6,
    IT_CREATURE_STACK = 7,
    IT_EXPERT = 0x0A,
};

typedef struct {
    u8 raw[ITEM_SIZE];
} Item;

static inline u8 item_type(const Item *it) { return it->raw[1]; }
static inline u32 item_id(const Item *it) { return rd32(it->raw + 2); }
static inline int item_enhance(const Item *it) { return it->raw[6] & 0x1F; }
static inline int item_seal_cnt(const Item *it) { return it->raw[6] >> 5; }
static inline int item_forge(const Item *it) { return it->raw[51]; }
static inline int item_inc_type(const Item *it) { return it->raw[17]; }
static inline u16 item_durability(const Item *it) { return rd16(it->raw + 11); }
static inline u16 item_inc_value(const Item *it) { return rd16(it->raw + 18); }
static inline u32 item_orb(const Item *it) { return rd32(it->raw + 13); }        // 宝珠/徽章
static inline u16 item_otherworld(const Item *it) { return rd16(it->raw + 31); } // 异界
static inline int item_seal(const Item *it) { return it->raw[0]; }
static inline int item_empty(const Item *it) { return it->raw[1] == 0 || item_id(it) == 0; }

static inline u32 item_num(const Item *it) {
    return item_type(it) == IT_EQUIP ? rd32be(it->raw + 7) : rd32(it->raw + 7);
}

static inline void item_set_type(Item *it, u8 v) { it->raw[1] = v; }
static inline void item_set_id(Item *it, u32 v) { wr32(it->raw + 2, v); }
static inline void item_set_enhance(Item *it, int v) { it->raw[6] = (u8)((it->raw[6] & 0xE0) | (v & 0x1F)); }
static inline void item_set_seal_cnt(Item *it, int v) { it->raw[6] = (u8)((it->raw[6] & 0x1F) | ((v & 7) << 5)); }
static inline void item_set_forge(Item *it, int v) { it->raw[51] = (u8)v; }
static inline void item_set_inc_type(Item *it, int v) { it->raw[17] = (u8)v; }
static inline void item_set_durability(Item *it, u16 v) { wr16(it->raw + 11, v); }
static inline void item_set_inc_value(Item *it, u16 v) { wr16(it->raw + 18, v); }
static inline void item_set_orb(Item *it, u32 v) { wr32(it->raw + 13, v); }
static inline void item_set_otherworld(Item *it, u16 v) { wr16(it->raw + 31, v); }
static inline void item_set_seal(Item *it, int v) { it->raw[0] = (u8)v; }

static inline void item_set_num(Item *it, u32 v) {
    if (item_type(it) == IT_EQUIP) wr32be(it->raw + 7, v);
    else wr32(it->raw + 7, v);
}

// 魔法封印：3 组 (id, 大端等级) + 第 4 组在 37+10
static inline int item_seal_id(const Item *it, int i) { return it->raw[37 + (i < 3 ? i * 3 : 10)]; }
static inline u16 item_seal_lv(const Item *it, int i) {
    const u8 *p = it->raw + 37 + (i < 3 ? i * 3 : 10) + 1;
    return (u16)(p[0] << 8 | p[1]);
}
static inline void item_set_seal_id(Item *it, int i, int v) { it->raw[37 + (i < 3 ? i * 3 : 10)] = (u8)v; }
static inline void item_set_seal_lv(Item *it, int i, u16 v) {
    u8 *p = it->raw + 37 + (i < 3 ? i * 3 : 10) + 1;
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}

const char *item_type_name(u8 t);
int         item_type_valid(u8 t);
void        item_hex(const Item *it, char *out, int cap);

// 背包 blob
typedef struct {
    Item *items;
    int   n;
    u8    prefix[4];
    int   has_prefix;
    int   ok;
    char  err[64];
} Blob;

// raw = 4 字节前缀 + zlib(N*61)
int  blob_unpack(const void *raw, size_t n, Blob *out, Err err);
int  blob_pack(const Blob *b, Buf *out);
void blob_free(Blob *b);

// 错误物品判定。
//   has  问「这个编号在不在名字表里」
//   lim  问「这个编号的堆叠上限是多少」，返回 0 表示没声明（不查）
// 两个都可以传 NULL，传 NULL 就跳过那一项检查。
typedef int (*ItemHasName)(u32 id, void *ctx);
typedef u32 (*ItemStackLim)(u32 id, void *ctx);
int item_bad(const Item *it, ItemHasName has, ItemStackLim lim, void *ctx, char *why, int cap);
