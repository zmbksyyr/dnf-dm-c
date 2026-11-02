// util：基础类型、迷你容器（Str/Buf/Vec）、编码转换、文件、MD5/SHA1、时间。
// 除 Win32 外不依赖任何库。
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

#define ERRBUF 256
typedef char Err[ERRBUF];

// ---------------- 迷你容器 ----------------
// Str：始终以 '\0' 结尾，n 不含结尾
typedef struct {
    char *p;
    int n, cap;
} Str;

void str_free(Str *s);
void str_clear(Str *s);
void str_reserve(Str *s, int cap);
void str_catn(Str *s, const char *p, int n);
void str_cat(Str *s, const char *z);
void str_catc(Str *s, char c);
void str_catf(Str *s, const char *f, ...);
void str_set(Str *s, const char *z);
void str_setn(Str *s, const char *p, int n);
char *str_take(Str *s);            // 交出缓冲，调用方负责 free

// Buf：二进制缓冲
typedef struct {
    u8 *p;
    size_t n, cap;
} Buf;

void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_reserve(Buf *b, size_t cap);
void buf_put(Buf *b, const void *d, size_t n);
void buf_putc(Buf *b, u8 c);
void buf_put32(Buf *b, u32 v);
void buf_put16(Buf *b, u16 v);

// Vec：任意元素动态数组（按字节拷贝）
typedef struct {
    void *p;
    int n, cap, esz;
} Vec;

#define VEC(T)            \
    struct {              \
        T *p;             \
        int n, cap;       \
    }

void vec_init(Vec *v, int esz);
void vec_free(Vec *v);
void vec_clear(Vec *v);
void *vec_add(Vec *v);             // 返回新元素地址，已清零
void *vec_at(Vec *v, int i);

// ---------------- 数值 ----------------
static inline u16 rd16(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u16)((u32)b[0] | ((u32)b[1] << 8));
}
static inline u32 rd32(const void *p) {
    return (u32)((const u8 *)p)[0] | (u32)((const u8 *)p)[1] << 8 | (u32)((const u8 *)p)[2] << 16 |
           (u32)((const u8 *)p)[3] << 24;
}
static inline u32 rd32be(const void *p) {
    return (u32)((const u8 *)p)[3] | (u32)((const u8 *)p)[2] << 8 | (u32)((const u8 *)p)[1] << 16 |
           (u32)((const u8 *)p)[0] << 24;
}
static inline u64 rd64(const void *p) { return (u64)rd32(p) | (u64)rd32((const u8 *)p + 4) << 32; }
static inline void wr16(void *p, u16 v) { ((u8 *)p)[0] = (u8)v; ((u8 *)p)[1] = (u8)(v >> 8); }
static inline void wr32(void *p, u32 v) {
    ((u8 *)p)[0] = (u8)v; ((u8 *)p)[1] = (u8)(v >> 8); ((u8 *)p)[2] = (u8)(v >> 16); ((u8 *)p)[3] = (u8)(v >> 24);
}
static inline void wr32be(void *p, u32 v) {
    ((u8 *)p)[3] = (u8)v; ((u8 *)p)[2] = (u8)(v >> 8); ((u8 *)p)[1] = (u8)(v >> 16); ((u8 *)p)[0] = (u8)(v >> 24);
}

// ---------------- 编码 ----------------
// 内部一律 UTF-8（char*），Win32 边界用 u16()/u8()
int  u16_from_u8(const char *src, int n, WCHAR *out, int cap);   // 返回写入字符数（含结尾）
int  u8_from_u16(const WCHAR *src, int n, char *out, int cap);
void utf16(const char *s, WCHAR *out, int cap);                  // 便捷包装
void utf8(const WCHAR *s, char *out, int cap);

int  utf8_valid(const u8 *p, int n);
void utf8_from_cp(const char *raw, int n, unsigned cp, Str *out);  // 指定代码页 → UTF-8
unsigned cn_codepage(const char *raw, int n);                      // 936 或 950
void decode_cn(const char *raw, int n, Str *out);
void decode_auto(const char *raw, int n, Str *out);

// ---------------- 文件 ----------------
int  file_read(const char *path, Buf *out);
int  file_write(const char *path, const void *d, size_t n);
int  file_append(const char *path, const char *text, size_t n);
int  file_exists(const char *path);
int  dir_exists(const char *path);
int  dir_make(const char *path);
void exe_dir(char *out, int cap);
void path_join(char *out, int cap, const char *a, const char *b);

// ---------------- 运行期生成物 ----------------
// 统一放在 <exe>/config/<sub>/ 下，命名一律「前缀-时间戳.扩展名」，
// 同前缀只保留最近 keep 份，避免越跑越多。
void art_dir(const char *sub, char *out, int cap);        // 取目录并确保存在
void art_stamp(char *out, int cap);                       // 20261030-024315
void art_path(char *out, int cap, const char *sub, const char *prefix, const char *ext);
void art_prune(const char *dir, const char *prefix, int keep);

// ---------------- 摘要 ----------------
void hash_md5(const void *d, size_t n, char out[33]);
int  hash_file_fp(const char *path, char out[33]);   // 长度+头尾各 64KB 的 md5：判断大文件换没换，比全量快三个数量级
void hash_sha1(const void *d, size_t n, u8 out[20]);

// ---------------- 其它 ----------------
void fmtv(char *out, int cap, const char *f, va_list ap);
void fmt(char *out, int cap, const char *f, ...);
char *fmt_new(const char *f, ...);                       // 轮转静态缓冲，下次调用失效，不要 free
void hex_str(const void *d, size_t n, Str *out);
void now_str(char *out, int cap, const char *f);
u64  now_ms(void);
void str_trim(char *s);
int  str_ieq(const char *a, const char *b);
int  str_istr(const char *hay, const char *needle);   // 大小写无关子串；needle 空则命中
char *str_dup(const char *s);
void *xalloc(size_t n);                                  // 失败即 ExitProcess
void *xrealloc(void *p, size_t n);

#define RDH(p, off) rd32((const u8 *)(p) + (off))
#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static inline const char *str_c(const Str *s) { return s->p ? s->p : ""; }
#define VEC_N(v) ((v)->n)
#define VEC_ADD(v, T) ((T *)vec_add(v))
#define VEC_AT(v, T, i) ((T *)vec_at((v), (i)))
