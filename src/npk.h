// npk：ImagePacks2 贴图包读取。给「编号悬浮详情」提供物品图标。
//
// NPK 结构（本库客户端实测）：
//   magic   16 字节  "NeoplePack_Bill\0"
//   count    4 字节  条目数
//   count × 264 字节  条目：u32 偏移 + u32 长度 + 256 字节文件名（XOR 加密）
//   尾部    32 字节（每个文件都有，不用管）
//
// 文件名加密是流式 XOR，密钥前 36 字节是
//   "puchikon@neople dungeon and fighter "
// 之后循环填充 "DNF"（用两条已知内部路径反推并交叉验证过）。
//
// 条目里是 IMG 文件；IMG V2 结构：
//   magic 16 字节 "Neople Img File\0" / u32 索引表长 / u32 保留 / u32 版本 / u32 帧数
//   帧索引：图片型 36 字节（色彩系统 / 压缩 / 宽 / 高 / 数据长 / x / y / 帧域宽 / 帧域高），
//           指向型 8 字节（0x11 + 目标帧号）
//   数据区：按索引顺序首尾相接；压缩标志 6 = zlib，5 = 原始
//   色彩系统：0x10 = 32 位 BGRA，0x0F = ARGB4444，0x0E = ARGB1555
#pragma once
#include "util.h"

typedef struct {
    char *key;      // 内部路径（小写，如 sprite/item/iconarmor.img）
    int   file;     // files 下标
    u32   off, len;
} NpkEnt;

typedef struct {
    char     dir[MAX_PATH];
    char   **files;      // npk 文件全路径
    int      nfiles;
    NpkEnt  *ents;        // 按 key 排序
    int      nents;
    int      ok;          // 索引建好
} Npk;

// 扫描目录建索引（后台线程里跑）。pct/stage 可为 NULL。
int  npk_build(Npk *p, const char *dir, int *pct, char *stage, int stage_cap);
void npk_free(Npk *p);
void npk_cache_clear(void);   // 重建索引时清掉旧图标缓存

// 取图标。img 是 PVF [icon] 字段里的路径（如 "Item/IconArmor.img"，大小写随意，
// 里面自动补 "sprite/" 前缀再试）。成功返回 32 位 BGRA（已预乘 alpha）的 DIB 句柄，
// 调用方 DeleteObject；失败返回 NULL。带小缓存，同一个图标反复取不重复解码。
HBITMAP npk_icon(Npk *p, const char *img, int frame, int *w, int *h);
