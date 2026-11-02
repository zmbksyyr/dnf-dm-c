// npk 实现。见 npk.h 头注释。
#include "npk.h"
#include "zlib.h"

// ---------------- NPK 索引 ----------------
static void npk_key_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
        else if (*s == '\\') *s = '/';
}

static int ent_cmp(const void *a, const void *b) {
    return strcmp(((const NpkEnt *)a)->key, ((const NpkEnt *)b)->key);
}

// 密钥流：36 字节短语 + "DNF" 循环。整段应用于 256 字节的文件名字段。
static u8 ks_byte(int i) {
    static const char head[] = "puchikon@neople dungeon and fighter ";
    if (i < (int)sizeof(head) - 1) return (u8)head[i];
    return (u8) "DNF"[(i - (int)sizeof(head) + 1) % 3];
}

// 目录里收 npk 文件。子目录不递归 —— ImagePacks2 是平的。
static void npk_list_files(Npk *p, const char *dir) {
    WCHAR wd[MAX_PATH * 2];
    utf16(dir, wd, MAX_PATH * 2);
    WCHAR pat[MAX_PATH * 2];
    _snwprintf(pat, COUNT_OF(pat), L"%s\\*.npk", wd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        utf8(fd.cFileName, path, MAX_PATH);
        char *full = (char *)xalloc(strlen(dir) + strlen(path) + 2);
        sprintf(full, "%s\\%s", dir, path);
        p->files = (char **)xrealloc(p->files, sizeof(char *) * (size_t)(p->nfiles + 1));
        p->files[p->nfiles++] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 读文件里 [off, off+n) 一段。悬浮/扫描都只碰需要的字节，整个包最大上百 MB，不整读。
static int read_at(const char *path, u32 off, size_t n, Buf *out) {
    buf_clear(out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    int ok = 0;
    if (!fseek(fp, (long)off, SEEK_SET)) {
        buf_reserve(out, n + 1);
        out->n = fread(out->p, 1, n, fp);
        ok = out->n == n;
    }
    fclose(fp);
    return ok;
}

int npk_build(Npk *p, const char *dir, int *pct, char *stage, int stage_cap) {
    memset(p, 0, sizeof(*p));
    snprintf(p->dir, sizeof(p->dir), "%s", dir);
    npk_list_files(p, dir);
    if (!p->nfiles) return 0;

    int cap = 4096;
    p->ents = (NpkEnt *)xalloc(sizeof(NpkEnt) * (size_t)cap);
    p->nents = 0;
    for (int f = 0; f < p->nfiles; f++) {
        if (stage) snprintf(stage, (size_t)stage_cap, "扫描贴图包 %d/%d", f + 1, p->nfiles);
        if (pct) *pct = f * 90 / p->nfiles;
        Buf head = {0};
        if (!read_at(p->files[f], 0, 20, &head)) continue;
        int bad = head.n < 20 || memcmp(head.p, "NeoplePack_Bill", 15) != 0;
        u32 cnt = bad ? 0 : rd32(head.p + 16);
        buf_free(&head);
        if (bad || cnt == 0 || cnt > 200000) continue;
        Buf b = {0};
        if (!read_at(p->files[f], 20, (size_t)cnt * 264, &b)) continue;
        for (u32 i = 0; i < cnt; i++) {
            const u8 *r = b.p + (size_t)i * 264;
            u32 off = rd32(r), len = rd32(r + 4);
            char name[257];
            int n = 0;
            for (int k = 0; k < 256; k++) {
                u8 c = (u8)(r[8 + k] ^ ks_byte(k));
                if (!c) break;
                name[n++] = (char)c;
            }
            name[n] = 0;
            if (!n || !len) continue;
            npk_key_lower(name);
            if (p->nents == cap) {
                cap *= 2;
                p->ents = (NpkEnt *)xrealloc(p->ents, sizeof(NpkEnt) * (size_t)cap);
            }
            NpkEnt *e = &p->ents[p->nents++];
            e->key = str_dup(name);
            e->file = f;
            e->off = off;
            e->len = len;
        }
        buf_free(&b);
    }
    if (!p->nents) return 0;
    if (stage) snprintf(stage, (size_t)stage_cap, "贴图包索引排序");
    if (pct) *pct = 95;
    qsort(p->ents, (size_t)p->nents, sizeof(NpkEnt), ent_cmp);
    // 同名条目：后面的补丁包应该盖过前面的 —— 查找时取最后一个即可（排序稳定查找用上界）
    p->ok = 1;
    if (pct) *pct = 100;
    return 1;
}

void npk_free(Npk *p) {
    for (int i = 0; i < p->nents; i++) free(p->ents[i].key);
    free(p->ents);
    for (int i = 0; i < p->nfiles; i++) free(p->files[i]);
    free(p->files);
    memset(p, 0, sizeof(*p));
}

static NpkEnt *npk_find(Npk *p, const char *key) {
    if (!p->ok) return NULL;
    int lo = 0, hi = p->nents - 1, hit = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(p->ents[mid].key, key);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid - 1;
        else { hit = mid; lo = mid + 1; }   // 同名取最后一个（补丁包覆盖旧包）
    }
    return hit >= 0 ? &p->ents[hit] : NULL;
}

// ---------------- IMG 解码 ----------------
#define IMG_CACHE 24
typedef struct {
    char  key[MAX_PATH];
    int   frame;
    HBITMAP bmp;
    int   w, h;
} IconCache;
static IconCache g_ic[IMG_CACHE];
static int       g_ic_n;

static void cache_put(const char *key, int frame, HBITMAP bmp, int w, int h) {
    if (g_ic_n == IMG_CACHE) {
        DeleteObject(g_ic[0].bmp);
        memmove(&g_ic[0], &g_ic[1], sizeof(IconCache) * (size_t)(IMG_CACHE - 1));
        g_ic_n--;
    }
    IconCache *c = &g_ic[g_ic_n++];
    snprintf(c->key, sizeof(c->key), "%s", key);
    c->frame = frame;
    c->bmp = bmp;
    c->w = w;
    c->h = h;
}

void npk_cache_clear(void) {
    for (int i = 0; i < g_ic_n; i++) DeleteObject(g_ic[i].bmp);
    g_ic_n = 0;
}

static HBITMAP cache_get(const char *key, int frame, int *w, int *h) {
    for (int i = 0; i < g_ic_n; i++) {
        if (g_ic[i].frame == frame && str_ieq(g_ic[i].key, key)) {
            *w = g_ic[i].w;
            *h = g_ic[i].h;
            return g_ic[i].bmp;
        }
    }
    return NULL;
}

// 读出 IMG 那一块（悬浮时才调，一次最多几 MB）
static int img_read(Npk *p, const char *key, Buf *out) {
    NpkEnt *e = npk_find(p, key);
    if (!e) return 0;
    return read_at(p->files[e->file], e->off, e->len, out);
}

// 帧号 → (帧数据指针, 长度, 类型, 压缩, 宽, 高)。指向型(0x11)一层层解析到图片帧。
static const u8 *img_frame(const u8 *img, size_t n, int want, u32 *plen, u32 *ptype, u32 *pcomp,
                           u32 *pw, u32 *ph) {
    if (n < 32 || memcmp(img, "Neople Img File", 15) != 0) return NULL;
    u32 idx_size = rd32(img + 16);
    u32 ver = rd32(img + 24);
    u32 cnt = rd32(img + 28);
    if (ver != 2 || cnt == 0 || cnt > 200000) return NULL;
    if ((size_t)32 + (size_t)idx_size > n) return NULL;
    const u8 *idx = img + 32;
    const u8 *data = idx + idx_size;
    // 指向型(0x11)一层层解析到图片帧；数据区按索引顺序首尾相接，
    // 所以每轮都要从头累一遍前面帧的长度
    int cur = want;
    for (int hop = 0; hop < 8; hop++) {
        if (cur < 0 || cur >= (int)cnt) return NULL;
        size_t o = 0;
        u32 skip = 0;
        for (int i = 0; i < (int)cnt; i++) {
            // cnt 由文件给出，索引区未必真装得下这么多条目 —— 每一项都要先
            // 确认读得完整，否则畸形包会把我们带出缓冲区
            if (o + 8 > (size_t)idx_size) return NULL;
            u32 t = rd32(idx + o);
            if (t == 0x11) {
                if (i == cur) { cur = (int)rd32(idx + o + 4); goto next; }
                o += 8;
                continue;
            }
            if (o + 36 > (size_t)idx_size) return NULL;
            u32 l = rd32(idx + o + 16);
            if (i == cur) {
                if (skip + l > n - (size_t)(data - img)) return NULL;
                u32 w = rd32(idx + o + 8), h = rd32(idx + o + 12);
                if (w == 0 || w > 4096 || h == 0 || h > 4096) return NULL;
                *plen = l; *ptype = t; *pcomp = rd32(idx + o + 4); *pw = w; *ph = h;
                return data + skip;
            }
            skip += l;
            o += 36;
        }
        return NULL;
    next:;
    }
    return NULL;
}

static HBITMAP dib_from_bgra(const u8 *px, int w, int h) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return NULL;
    memcpy(bits, px, (size_t)w * (size_t)h * 4);
    return bmp;
}

HBITMAP npk_icon(Npk *p, const char *img, int frame, int *w, int *h) {
    if (!p || !p->ok || !img || !*img) return NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    HBITMAP hit = cache_get(img, frame, w, h);
    if (hit) return hit;

    char key[MAX_PATH * 2];
    // PVF 里写的是 "Item/IconArmor.img"，NPK 内部是 "sprite/item/iconarmor.img"
    snprintf(key, sizeof(key), "sprite/%s", img);
    npk_key_lower(key);
    Buf b = {0};
    if (!img_read(p, key, &b)) {
        snprintf(key, sizeof(key), "%s", img);
        npk_key_lower(key);
        if (!img_read(p, key, &b)) { buf_free(&b); return NULL; }
    }
    u32 len, type, comp, wpx, hpx;
    const u8 *raw = img_frame(b.p, b.n, frame, &len, &type, &comp, &wpx, &hpx);
    if (!raw) { buf_free(&b); return NULL; }

    Buf unc = {0};
    const u8 *px = raw;
    if (comp == 6) {
        if (!zlib_unpack(raw, len, &unc)) { buf_free(&b); return NULL; }
        px = unc.p;
    } else if (comp != 5) {
        buf_free(&b);
        return NULL;
    }
    int W = (int)wpx, H = (int)hpx;
    size_t need = (size_t)W * (size_t)H * 4;
    HBITMAP bmp = NULL;               // goto fail 会跳过它的初始化，先落个 NULL
    u8 *out = (u8 *)xalloc(need);
    if (type == 0x10) {                        // 32 位，字节序 B G R A，直接用
        if ((comp == 6 ? unc.n : (size_t)len) < need) { free(out); goto fail; }
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) {
            u8 bl = px[i * 4], gr = px[i * 4 + 1], r = px[i * 4 + 2], a = px[i * 4 + 3];
            // AlphaBlend 要预乘
            out[i * 4]     = (u8)(bl * a / 255);
            out[i * 4 + 1] = (u8)(gr * a / 255);
            out[i * 4 + 2] = (u8)(r * a / 255);
            out[i * 4 + 3] = a;
        }
    } else if (type == 0x0E) {                 // 1555
        if ((comp == 6 ? unc.n : (size_t)len) < (size_t)W * (size_t)H * 2) { free(out); goto fail; }
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) {
            u16 v = rd16(px + i * 2);
            u8 a = (v & 0x8000) ? 255 : 0;
            out[i * 4]     = (u8)((v & 31) * 255 / 31 * a / 255);
            out[i * 4 + 1] = (u8)(((v >> 5) & 31) * 255 / 31 * a / 255);
            out[i * 4 + 2] = (u8)(((v >> 10) & 31) * 255 / 31 * a / 255);
            out[i * 4 + 3] = a;
        }
    } else if (type == 0x0F) {                 // 4444
        if ((comp == 6 ? unc.n : (size_t)len) < (size_t)W * (size_t)H * 2) { free(out); goto fail; }
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) {
            u16 v = rd16(px + i * 2);
            u8 a = (u8)(((v >> 12) & 15) * 17);
            out[i * 4]     = (u8)((v & 15) * 17 * a / 255);
            out[i * 4 + 1] = (u8)(((v >> 4) & 15) * 17 * a / 255);
            out[i * 4 + 2] = (u8)(((v >> 8) & 15) * 17 * a / 255);
            out[i * 4 + 3] = a;
        }
    } else {
        free(out);
        goto fail;
    }
    bmp = dib_from_bgra(out, W, H);
    free(out);
    if (bmp) {
        cache_put(img, frame, bmp, W, H);
        if (w) *w = W;
        if (h) *h = H;
    }
fail:
    buf_free(&unc);
    buf_free(&b);
    return bmp;
}
