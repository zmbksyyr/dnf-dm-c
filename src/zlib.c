// zlib 实现：inflate 采用 puff 风格逐位 Huffman 解码（无查表，代码短、够快）。
#include "zlib.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} Huff;

typedef struct {
    const u8 *in;
    size_t inlen, incnt;
    u32 bitbuf;
    int bitcnt;
    Buf *out;
    int err;
} Inf;

static int bits(Inf *s, int need) {
    u32 val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) { s->err = -1; return 0; }
        val |= (u32)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static void put(Inf *s, u8 c) {
    if (s->out->n + 1 > s->out->cap) buf_reserve(s->out, s->out->n + 1);
    s->out->p[s->out->n++] = c;
}

// 规范 Huffman：由码长表建 symbol 排序表
static int construct(Huff *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;              // 全是 0 长度：合法但无码
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;               // 码长超额
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                    33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,
                                     4, 4, 5, 5, 6, 6, 7, 7,  8,  8,
                                     9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inf *s, const Huff *lencode, const Huff *distcode) {
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            put(s, (u8)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = kLenBase[sym] + bits(s, kLenExtra[sym]);
            if (s->err) return -1;
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            size_t dist = (size_t)kDistBase[dsym] + (size_t)bits(s, kDistExtra[dsym]);
            if (s->err) return -1;
            if (dist > s->out->n) return -1;
            while (len-- > 0) put(s, s->out->p[s->out->n - dist]);
        }
    }
}

static int stored(Inf *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    int len = s->in[s->incnt] | s->in[s->incnt + 1] << 8;
    int nlen = s->in[s->incnt + 2] | s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xffff)) return -1;
    if (s->incnt + (size_t)len > s->inlen) return -1;
    buf_put(s->out, s->in + s->incnt, (size_t)len);
    s->incnt += (size_t)len;
    return 0;
}

static int fixed(Inf *s) {
    static int built = 0;
    static Huff lencode, distcode;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(Inf *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXCODES];
    Huff lencode, distcode;
    int nlen = bits(s, 5) + 257;
    int ndist = bits(s, 5) + 1;
    int ncode = bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return -1;

    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err) return -1;
    if (construct(&lencode, lengths, 19) != 0) return -1;

    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + bits(s, 3);
            } else {
                rep = 11 + bits(s, 7);
            }
            if (s->err) return -1;
            if (index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int e = construct(&lencode, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1)) return -1;
    e = construct(&distcode, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1)) return -1;
    return codes(s, &lencode, &distcode);
}

int zlib_unpack(const void *src, size_t n, Buf *out) {
    const u8 *p = (const u8 *)src;
    if (n < 6) return 0;
    int cmf = p[0], flg = p[1];
    if ((cmf & 0x0f) != 8) return 0;                      // 必须 deflate
    if (((cmf << 8) | flg) % 31) return 0;                 // 头校验
    size_t off = 2;
    if (flg & 0x20) off += 4;                              // 预置字典，跳过 DICTID
    // 尾部还有 4 字节 adler32，输入必须比 off+4 长。
    // 只写 off >= n 是不够的：n=6 时 off+4 > n，n-off-4 会按 size_t 回绕成天文数字，
    // 于是 inlen 变成「无边」，解压器就会一路读过缓冲区。
    if (off + 4 > n) return 0;

    Inf s;
    memset(&s, 0, sizeof(s));
    s.in = p + off;
    s.inlen = n - off - 4;                                 // 去掉 adler32
    s.out = out;
    out->n = 0;

    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) return 0;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r) return 0;
    } while (!last);
    return 1;
}

u32 adler32_calc(const void *d, size_t n) {
    const u8 *p = (const u8 *)d;
    u32 a = 1, b = 0;
    while (n) {
        size_t k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

int zlib_pack(const void *src, size_t n, Buf *out) {
    buf_clear(out);
    buf_putc(out, 0x78);
    buf_putc(out, 0x01);
    const u8 *p = (const u8 *)src;
    size_t left = n;
    do {
        size_t chunk = left > 65535 ? 65535 : left;
        left -= chunk;
        buf_putc(out, left ? 0x00 : 0x01);                 // BFINAL + BTYPE=00
        buf_put16(out, (u16)chunk);
        buf_put16(out, (u16)~chunk);
        buf_put(out, p, chunk);
        p += chunk;
    } while (left);
    u32 ad = adler32_calc(src, n);
    buf_putc(out, (u8)(ad >> 24));
    buf_putc(out, (u8)(ad >> 16));
    buf_putc(out, (u8)(ad >> 8));
    buf_putc(out, (u8)ad);
    return 1;
}

u32 crc32_calc(const void *d, size_t n, u32 prev) {
    static u32 table[256];
    static int built = 0;
    if (!built) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = 1;
    }
    u32 c = prev ^ 0xFFFFFFFFu;
    const u8 *p = (const u8 *)d;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
