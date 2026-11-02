// zlib：自带 inflate（stored/fixed/dynamic Huffman）+ stored 方式 deflate + adler32。
// 只服务于背包 blob，不追求压缩率。
#pragma once
#include "util.h"

int  zlib_unpack(const void *src, size_t n, Buf *out);   // 1 成功
int  zlib_pack(const void *src, size_t n, Buf *out);     // zlib 封装 + stored 块
u32  adler32_calc(const void *d, size_t n);
