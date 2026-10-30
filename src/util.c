// util 实现。// port of dnfpkgtool/ps.py + 通用工具
#include "util.h"
#include <time.h>

// ---------------- 内存 ----------------
void *xalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) ExitProcess(2);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) ExitProcess(2);
    return q;
}

char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)xalloc(n);
    memcpy(p, s, n);
    return p;
}

// ---------------- Str ----------------
void str_free(Str *s) {
    free(s->p);
    s->p = NULL;
    s->n = s->cap = 0;
}

void str_reserve(Str *s, int cap) {
    if (cap <= s->cap) return;
    int c = s->cap ? s->cap : 32;
    while (c < cap) c <<= 1;
    s->p = (char *)xrealloc(s->p, (size_t)c);
    s->cap = c;
}

void str_clear(Str *s) {
    s->n = 0;
    if (s->p) s->p[0] = 0;
}

void str_catn(Str *s, const char *p, int n) {
    if (!p || n <= 0) return;
    str_reserve(s, s->n + n + 1);
    memcpy(s->p + s->n, p, (size_t)n);
    s->n += n;
    s->p[s->n] = 0;
}

void str_cat(Str *s, const char *z) {
    if (z) str_catn(s, z, (int)strlen(z));
}

void str_catc(Str *s, char c) { str_catn(s, &c, 1); }

void str_catf(Str *s, const char *f, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, f);
    fmtv(tmp, sizeof(tmp), f, ap);
    va_end(ap);
    str_cat(s, tmp);
}

void str_setn(Str *s, const char *p, int n) {
    s->n = 0;
    str_catn(s, p, n);
    if (s->p) s->p[s->n] = 0;
}

void str_set(Str *s, const char *z) { str_setn(s, z, z ? (int)strlen(z) : 0); }

char *str_take(Str *s) {
    char *p = s->p;
    if (!p) p = str_dup("");
    s->p = NULL;
    s->n = s->cap = 0;
    return p;
}

// ---------------- Buf ----------------
void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void buf_clear(Buf *b) { b->n = 0; }

void buf_reserve(Buf *b, size_t cap) {
    if (cap <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < cap) c <<= 1;
    b->p = (u8 *)xrealloc(b->p, c);
    b->cap = c;
}

void buf_put(Buf *b, const void *d, size_t n) {
    if (!n) return;
    buf_reserve(b, b->n + n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

void buf_putc(Buf *b, u8 c) { buf_put(b, &c, 1); }
void buf_put16(Buf *b, u16 v) { u8 t[2]; wr16(t, v); buf_put(b, t, 2); }
void buf_put32(Buf *b, u32 v) { u8 t[4]; wr32(t, v); buf_put(b, t, 4); }

// ---------------- Vec ----------------
void vec_init(Vec *v, int esz) {
    v->p = NULL;
    v->n = v->cap = 0;
    v->esz = esz;
}

void vec_free(Vec *v) {
    free(v->p);
    v->p = NULL;
    v->n = v->cap = 0;
}

void vec_clear(Vec *v) { v->n = 0; }

void *vec_add(Vec *v) {
    if (v->n >= v->cap) {
        int c = v->cap ? v->cap * 2 : 8;
        v->p = xrealloc(v->p, (size_t)c * v->esz);
        v->cap = c;
    }
    void *e = (u8 *)v->p + (size_t)v->n * v->esz;
    memset(e, 0, (size_t)v->esz);
    v->n++;
    return e;
}

void *vec_at(Vec *v, int i) { return (u8 *)v->p + (size_t)i * v->esz; }

// ---------------- 编码 ----------------
int u16_from_u8(const char *src, int n, WCHAR *out, int cap) {
    if (n < 0) n = (int)strlen(src);
    if (!n) { if (cap > 0) out[0] = 0; return 1; }
    int r = MultiByteToWideChar(CP_UTF8, 0, src, n, out, cap - 1);
    if (r < 0) r = 0;
    out[r] = 0;
    return r + 1;
}

int u8_from_u16(const WCHAR *src, int n, char *out, int cap) {
    if (n < 0) n = (int)wcslen(src);
    if (!n) { if (cap > 0) out[0] = 0; return 1; }
    int r = WideCharToMultiByte(CP_UTF8, 0, src, n, out, cap - 1, NULL, NULL);
    if (r < 0) r = 0;
    out[r] = 0;
    return r + 1;
}

void utf16(const char *s, WCHAR *out, int cap) {
    if (cap <= 0) return;
    u16_from_u8(s ? s : "", -1, out, cap);
}

void utf8(const WCHAR *s, char *out, int cap) {
    if (cap <= 0) return;
    u8_from_u16(s ? s : L"", -1, out, cap);
}

int utf8_valid(const u8 *p, int n) {
    int i = 0;
    while (i < n) {
        u8 c = p[i];
        int need = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (need < 0) return 0;
        if (i + need >= n && need) return 0;
        for (int k = 1; k <= need; k++)
            if ((p[i + k] >> 6) != 2) return 0;
        i += need + 1;
    }
    return 1;
}

void utf8_from_cp(const char *raw, int n, unsigned cp, Str *out) {
    str_clear(out);
    if (n <= 0) return;
    int wlen = MultiByteToWideChar(cp, 0, raw, n, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *w = (WCHAR *)xalloc((size_t)(wlen + 1) * sizeof(WCHAR));
    MultiByteToWideChar(cp, 0, raw, n, w, wlen);
    w[wlen] = 0;
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    if (ulen > 0) {
        str_reserve(out, ulen + 1);
        WideCharToMultiByte(CP_UTF8, 0, w, wlen, out->p, ulen, NULL, NULL);
        out->n = ulen;
        out->p[ulen] = 0;
    }
    free(w);
}

// 约 100 个高频汉字：正确解码时命中率明显更高
static const char *kHotCn =
    "的一是不了在人有我他这为之大来以个中上们到说国和地也子时道出而要于就下得可你年生"
    "自会那后能对着事其里所去行过家十用发天如然作方成者多日都三小军二无同么经法当起与"
    "好看学进种将还分此心前面又定见只主没公从已经给两最意点情外但身些";

static int cn_score(const char *u8s, int n) {
    int hit = 0, i = 0;
    while (i < n) {
        u8 c = (u8)u8s[i];
        int step = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (step == 3) {
            for (const char *p = kHotCn; *p;) {
                u8 h = (u8)*p;
                int hn = h < 0x80 ? 1 : (h >> 5) == 6 ? 2 : 3;
                if (hn == 3 && memcmp(p, u8s + i, 3) == 0) { hit++; break; }
                p += hn;
            }
        }
        i += step;
    }
    return hit;
}

unsigned cn_codepage(const char *raw, int n) {
    Str a = {0}, b = {0};
    utf8_from_cp(raw, n, 936, &a);
    utf8_from_cp(raw, n, 950, &b);
    int sa = cn_score(a.p ? a.p : "", a.n), sb = cn_score(b.p ? b.p : "", b.n);
    str_free(&a);
    str_free(&b);
    return sa >= sb ? 936u : 950u;
}

void decode_cn(const char *raw, int n, Str *out) {
    if (n <= 0) { str_clear(out); return; }
    int probe = n > 4096 ? 4096 : n;   // 只需采样判码页，全量扫描太贵
    utf8_from_cp(raw, n, cn_codepage(raw, probe), out);
}

void decode_auto(const char *raw, int n, Str *out) {
    str_clear(out);
    if (n <= 0) return;
    if (utf8_valid((const u8 *)raw, n)) { str_setn(out, raw, n); return; }
    utf8_from_cp(raw, n, 936, out);
    if (out->n && utf8_valid((const u8 *)out->p, out->n)) return;
    utf8_from_cp(raw, n, 950, out);
    if (out->n && utf8_valid((const u8 *)out->p, out->n)) return;
    utf8_from_cp(raw, n, 1252, out);
}

// ---------------- 文件 ----------------
static HANDLE open_w(const char *path, DWORD access, DWORD share, DWORD disp) {
    WCHAR w[MAX_PATH * 2];
    utf16(path, w, MAX_PATH * 2);
    return CreateFileW(w, access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
}

int file_read(const char *path, Buf *out) {
    buf_clear(out);
    HANDLE h = open_w(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    buf_reserve(out, (size_t)sz.QuadPart + 1);
    size_t total = 0;
    while (total < (size_t)sz.QuadPart) {
        DWORD got = 0;
        DWORD want = (DWORD)((size_t)sz.QuadPart - total);
        if (want > (1u << 24)) want = 1u << 24;
        if (!ReadFile(h, out->p + total, want, &got, NULL) || !got) break;
        total += got;
    }
    CloseHandle(h);
    out->n = total;
    return total == (size_t)sz.QuadPart;
}

int file_write(const char *path, const void *d, size_t n) {
    HANDLE h = open_w(path, GENERIC_WRITE, 0, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    const u8 *p = (const u8 *)d;
    size_t done = 0;
    while (done < n) {
        DWORD put = 0;
        DWORD want = (DWORD)(n - done);
        if (want > (1u << 24)) want = 1u << 24;
        if (!WriteFile(h, p + done, want, &put, NULL) || !put) { ok = 0; break; }
        done += put;
    }
    CloseHandle(h);
    return ok;
}

int file_append(const char *path, const char *text, int n) {
    HANDLE h = open_w(path, FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    WriteFile(h, text, (DWORD)n, &put, NULL);
    CloseHandle(h);
    return (int)put == n;
}

int file_exists(const char *path) {
    WCHAR w[MAX_PATH * 2];
    utf16(path, w, MAX_PATH * 2);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

u64 file_size(const char *path) {
    WCHAR w[MAX_PATH * 2];
    utf16(path, w, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return 0;
    return ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int dir_make(const char *path) {
    if (!path || !*path) return 1;
    WCHAR w[MAX_PATH * 2];
    utf16(path, w, MAX_PATH * 2);
    if (CreateDirectoryW(w, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

int file_copy(const char *from, const char *to) {
    WCHAR a[MAX_PATH * 2], b[MAX_PATH * 2];
    utf16(from, a, MAX_PATH * 2);
    utf16(to, b, MAX_PATH * 2);
    return CopyFileW(a, b, FALSE) != 0;
}

int file_replace(const char *from, const char *to) {
    WCHAR a[MAX_PATH * 2], b[MAX_PATH * 2];
    utf16(from, a, MAX_PATH * 2);
    utf16(to, b, MAX_PATH * 2);
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void exe_dir(char *out, int cap) {
    WCHAR w[MAX_PATH * 2];
    GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    WCHAR *p = wcsrchr(w, L'\\');
    if (p) *p = 0;
    utf8(w, out, cap);
}

void path_join(char *out, int cap, const char *a, const char *b) {
    if (!a || !*a) { snprintf(out, cap, "%s", b ? b : ""); return; }
    size_t n = strlen(a);
    char last = a[n - 1];
    snprintf(out, cap, "%s%s%s", a, (last == '\\' || last == '/') ? "" : "\\", b ? b : "");
}

// ---------------- 运行期生成物 ----------------
void art_dir(const char *sub, char *out, int cap) {
    char dir[MAX_PATH], cfg[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    path_join(cfg, sizeof(cfg), dir, "config");
    dir_make(cfg);
    if (sub && *sub) {
        path_join(cfg, sizeof(cfg), cfg, sub);
        dir_make(cfg);
    }
    snprintf(out, (size_t)cap, "%s", cfg);
}

void art_stamp(char *out, int cap) { now_str(out, cap, "%Y%m%d-%H%M%S"); }

void art_path(char *out, int cap, const char *sub, const char *prefix, const char *ext) {
    char dir[MAX_PATH], ts[32];
    art_dir(sub, dir, sizeof(dir));
    art_stamp(ts, sizeof(ts));
    snprintf(out, (size_t)cap, "%s\\%s-%s%s%s", dir, prefix, ts, ext && ext[0] ? "." : "",
             ext ? ext : "");
}

// 按最后写入时间保留最近 keep 份（同前缀的老文件删掉）。
// 只删本程序自己生成的目录里的文件，prefix 为空时不动手。
void art_prune(const char *dir, const char *prefix, int keep) {
    if (!dir || !prefix || !prefix[0] || keep < 1) return;
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s\\%s*", dir, prefix);
    WCHAR wp[MAX_PATH * 2];
    utf16(pat, wp, COUNT_OF(wp));

    typedef struct {
        WCHAR name[MAX_PATH];
        FILETIME t;
    } Ent;
    enum { MAXENT = 128 };
    Ent *ent = (Ent *)xalloc(sizeof(Ent) * MAXENT);
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (n >= MAXENT) break;
            wcscpy(ent[n].name, fd.cFileName);
            ent[n].t = fd.ftLastWriteTime;
            n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n <= keep) { free(ent); return; }

    // 新的排前面
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (CompareFileTime(&ent[j].t, &ent[i].t) > 0) {
                Ent t = ent[i];
                ent[i] = ent[j];
                ent[j] = t;
            }
    for (int i = keep; i < n; i++) {
        WCHAR full[MAX_PATH * 2];
        swprintf(full, COUNT_OF(full), L"%s\\%s", wp, ent[i].name);
        DeleteFileW(full);
    }
    free(ent);
}

// ---------------- MD5 ----------------
typedef struct {
    u32 h[4];
    u64 len;
    u8 buf[64];
    int n;
} Md5;

static u32 rol32(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(Md5 *m, const u8 *p) {
    static const u32 K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    u32 w[16];
    for (int i = 0; i < 16; i++) w[i] = rd32(p + i * 4);
    u32 a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        u32 f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        u32 t = d;
        d = c;
        c = b;
        b = b + rol32(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    m->h[0] += a;
    m->h[1] += b;
    m->h[2] += c;
    m->h[3] += d;
}

static void md5_init(Md5 *m) {
    m->h[0] = 0x67452301;
    m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe;
    m->h[3] = 0x10325476;
    m->len = 0;
    m->n = 0;
}

static void md5_update(Md5 *m, const void *d, size_t sz) {
    const u8 *p = (const u8 *)d;
    m->len += sz;
    while (sz--) {
        m->buf[m->n++] = *p++;
        if (m->n == 64) { md5_block(m, m->buf); m->n = 0; }
    }
}

static void md5_final(Md5 *m, u8 out[16]) {
    u64 bits = m->len * 8;
    u8 pad = 0x80, z = 0;
    md5_update(m, &pad, 1);
    while (m->n != 56) md5_update(m, &z, 1);
    u8 lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (u8)(bits >> (8 * i));
    md5_update(m, lb, 8);
    for (int i = 0; i < 4; i++) wr32(out + i * 4, m->h[i]);
}

// zlib 多项式 CRC-32（PVF 的 tree_crc / 叶子 crc 都用它）
u32 crc32_buf(const void *d, size_t n) {
    static u32 tab[256];
    static int init = 0;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = 1;
    }
    u32 c = 0xFFFFFFFFu;
    const u8 *p = (const u8 *)d;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void hash_md5(const void *d, size_t n, char out[33]) {
    Md5 m;
    u8 raw[16];
    md5_init(&m);
    md5_update(&m, d, n);
    md5_final(&m, raw);
    static const char *hd = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i * 2] = hd[raw[i] >> 4]; out[i * 2 + 1] = hd[raw[i] & 15]; }
    out[32] = 0;
}

void hash_md5_file(const char *path, char out[33], void (*progress)(int)) {
    out[0] = 0;
    HANDLE h = open_w(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    u64 total = (u64)sz.QuadPart, done = 0;
    Md5 m;
    u8 raw[16];
    u8 *buf = (u8 *)xalloc(1 << 20);
    md5_init(&m);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, 1 << 20, &got, NULL) || !got) break;
        md5_update(&m, buf, got);
        done += got;
        if (progress && total) progress((int)(done * 100 / total));
    }
    free(buf);
    CloseHandle(h);
    md5_final(&m, raw);
    static const char *hd = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i * 2] = hd[raw[i] >> 4]; out[i * 2 + 1] = hd[raw[i] & 15]; }
    out[32] = 0;
}

// ---------------- SHA1 ----------------
typedef struct {
    u32 h[5];
    u64 len;
    u8 buf[64];
    int n;
} Sha1;

static void sha1_block(Sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(Sha1 *s) {
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->len = 0;
    s->n = 0;
}

static void sha1_update(Sha1 *s, const void *d, size_t sz) {
    const u8 *p = (const u8 *)d;
    s->len += sz;
    while (sz--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(Sha1 *s, u8 out[20]) {
    u64 bits = s->len * 8;
    u8 pad = 0x80, z = 0;
    sha1_update(s, &pad, 1);
    while (s->n != 56) sha1_update(s, &z, 1);
    u8 lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(s, lb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

void hash_sha1(const void *d, size_t n, u8 out[20]) {
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, d, n);
    sha1_final(&s, out);
}

// ---------------- 其它 ----------------
void fmtv(char *out, int cap, const char *f, va_list ap) { vsnprintf(out, (size_t)cap, f, ap); }

void fmt(char *out, int cap, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    fmtv(out, cap, f, ap);
    va_end(ap);
}

char *fmt_new(const char *f, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, f);
    fmtv(tmp, sizeof(tmp), f, ap);
    va_end(ap);
    return str_dup(tmp);
}

void hex_str(const void *d, size_t n, Str *out) {
    static const char *hd = "0123456789ABCDEF";
    const u8 *p = (const u8 *)d;
    str_reserve(out, out->n + (int)n * 2 + 1);
    for (size_t i = 0; i < n; i++) {
        out->p[out->n++] = hd[p[i] >> 4];
        out->p[out->n++] = hd[p[i] & 15];
    }
    out->p[out->n] = 0;
}

void now_str(char *out, int cap, const char *f) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    strftime(out, (size_t)cap, f ? f : "%Y-%m-%d %H:%M:%S", &tmv);
}

u64 now_ms(void) { return GetTickCount64(); }

void str_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

int str_ieq(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

// 字节级、大小写无关的子串（UTF-8 的高位字节原样比较，中文不受影响）
int str_istr(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 1;
    for (const char *s = hay; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return 0;
}
