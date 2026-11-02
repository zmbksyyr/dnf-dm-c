// pvf 实现。// port of dnfpkgtool/pvfReader.py + cacheManager.py
#include "pvf.h"

#define XOR_KEY 0x81A79011u
#define CACHE_MAGIC "PVFC7\0"   // 7：物品表多存一份 [stack limit]；旧缓存直接作废重建
#define CACHE_MAGIC_LEN 6

// ---------------- 解密 ----------------
// 解密与加密是同一个变换（自反），对外暴露给 pvf_write 用
void pvf_decrypt(u8 *p, size_t n, u32 crc) {
    const u32 k = crc ^ XOR_KEY;
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        u32 v = rd32(p + i * 4) ^ k;
        v = ((v & 0x3Fu) << 26) | (v >> 6);
        wr32(p + i * 4, v);
    }
}

// ---------------- 字符串表 ----------------
const char *stab_get(const StrTab *t, int i) {
    if (!t || i < 0 || i >= t->n || !t->blob) return "";
    if (t->off[i] >= t->blob_len) return "";
    return t->blob + t->off[i];
}

void stab_free(StrTab *t) {
    free(t->blob);
    free(t->off);
    memset(t, 0, sizeof(*t));
}

typedef struct {
    StrTab *t;
    int     cap;
    size_t  blob_cap;
} StabBuild;

static void sb_init(StabBuild *b, StrTab *t) {
    memset(t, 0, sizeof(*t));
    b->t = t;
    b->cap = 0;
    b->blob_cap = 0;
}

static void sb_add(StabBuild *b, const char *s, int len) {
    StrTab *t = b->t;
    if (len < 0) len = 0;
    if (t->n + 1 > b->cap) {
        int c = b->cap ? b->cap * 2 : 1024;
        t->off = (u32 *)xrealloc(t->off, sizeof(u32) * (size_t)c);
        b->cap = c;
    }
    if (t->blob_len + (size_t)len + 1 > b->blob_cap) {
        size_t c = b->blob_cap ? b->blob_cap : 65536;
        while (c < t->blob_len + (size_t)len + 1) c <<= 1;
        t->blob = (char *)xrealloc(t->blob, c);
        b->blob_cap = c;
    }
    t->off[t->n] = (u32)t->blob_len;
    if (len) memcpy(t->blob + t->blob_len, s, (size_t)len);
    t->blob_len += (size_t)len;
    t->blob[t->blob_len] = 0;
    t->blob_len++;
    t->n++;
}

// ---------------- IdTab ----------------
const char *idtab_get(const IdTab *t, u32 key) {
    if (!t || t->n == 0) return "";
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (t->key[mid] == key) return stab_get(&t->names, (int)t->stroff[mid]);
        if (t->key[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return "";
}

void idtab_free(IdTab *t) {
    free(t->key);
    free(t->stroff);
    stab_free(&t->names);
    memset(t, 0, sizeof(*t));
}

typedef struct {
    IdTab    *t;
    StabBuild sb;
    int       cap;
    u8       *kind;      // 与 t->key 平行的分类标记（只有物品表要，见 want_kind）
    u8        cur_kind;  // ib_add 时写进去的值，调用方在每张表/每个目录前设定
    u32      *stk;       // 与 t->key 平行的堆叠上限 [stack limit]，0 = 这条没声明
    u32       cur_stk;
    int       want_kind;
} IdBuild;

static void ib_init2(IdBuild *b, IdTab *t, int want_kind) {
    memset(t, 0, sizeof(*t));
    b->t = t;
    sb_init(&b->sb, &t->names);
    b->cap = 0;
    b->kind = NULL;
    b->cur_kind = 0;
    b->stk = NULL;
    b->cur_stk = 0;
    b->want_kind = want_kind;
}

static void ib_init(IdBuild *b, IdTab *t) { ib_init2(b, t, 0); }

// stk = 堆叠上限，0 表示这条没写 [stack limit]（装备一般没有，也确实不该堆叠）
static void ib_add(IdBuild *b, u32 key, const char *name, u32 stk) {
    if (b->t->n + 1 > b->cap) {
        int c = b->cap ? b->cap * 2 : 64;
        b->t->key = (u32 *)xrealloc(b->t->key, sizeof(u32) * (size_t)c);
        b->t->stroff = (u32 *)xrealloc(b->t->stroff, sizeof(u32) * (size_t)c);
        if (b->want_kind) {
            b->kind = (u8 *)xrealloc(b->kind, (size_t)c);
            b->stk = (u32 *)xrealloc(b->stk, sizeof(u32) * (size_t)c);
        }
        b->cap = c;
    }
    b->t->key[b->t->n] = key;
    b->t->stroff[b->t->n] = (u32)b->t->names.n;
    if (b->kind) {
        b->kind[b->t->n] = b->cur_kind;
        b->stk[b->t->n] = stk;
    }
    sb_add(&b->sb, name, (int)strlen(name));
    b->t->n++;
}

typedef struct {
    u32 key;
    u32 idx;
} KeyIdx;

static int cmp_keyidx(const void *a, const void *b) {
    u32 x = ((const KeyIdx *)a)->key, y = ((const KeyIdx *)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void ib_sort(IdBuild *b) {
    int n = b->t->n;
    if (n < 2) return;
    KeyIdx *ki = (KeyIdx *)xalloc(sizeof(KeyIdx) * (size_t)n);
    for (int i = 0; i < n; i++) { ki[i].key = b->t->key[i]; ki[i].idx = (u32)i; }
    qsort(ki, (size_t)n, sizeof(KeyIdx), cmp_keyidx);
    u32 *nk = (u32 *)xalloc(sizeof(u32) * (size_t)n);
    u32 *ns = (u32 *)xalloc(sizeof(u32) * (size_t)n);
    u8  *nd = b->kind ? (u8 *)xalloc((size_t)n) : NULL;
    u32 *nt = b->stk ? (u32 *)xalloc(sizeof(u32) * (size_t)n) : NULL;
    for (int i = 0; i < n; i++) {
        nk[i] = ki[i].key;
        ns[i] = b->t->stroff[ki[i].idx];
        if (nd) nd[i] = b->kind[ki[i].idx];
        if (nt) nt[i] = b->stk[ki[i].idx];
    }
    free(ki);
    free(b->t->key);
    free(b->t->stroff);
    b->t->key = nk;
    b->t->stroff = ns;
    if (nd) {
        free(b->kind);
        b->kind = nd;
    }
    if (nt) {
        free(b->stk);
        b->stk = nt;
    }
}

// ---------------- 路径索引 ----------------
typedef struct {
    const char *path;
    u32         idx;
} PathIdx;

static int cmp_pathidx(const void *a, const void *b) {
    return strcmp(((const PathIdx *)a)->path, ((const PathIdx *)b)->path);
}

static void build_path_index(Pvf *p) {
    free(p->by_path);
    p->by_path = NULL;
    p->nby_path = p->nleaf;
    if (p->nleaf == 0) return;
    PathIdx *pi = (PathIdx *)xalloc(sizeof(PathIdx) * (size_t)p->nleaf);
    for (int i = 0; i < p->nleaf; i++) {
        pi[i].path = p->leafs[i].path;
        pi[i].idx = (u32)i;
    }
    qsort(pi, (size_t)p->nleaf, sizeof(PathIdx), cmp_pathidx);
    p->by_path = (u32 *)xalloc(sizeof(u32) * (size_t)p->nleaf);
    for (int i = 0; i < p->nleaf; i++) p->by_path[i] = pi[i].idx;
    free(pi);
}

const Leaf *pvf_find(const Pvf *p, const char *path) {
    if (!p || !p->by_path || p->nby_path == 0 || !path) return NULL;
    char low[MAX_PATH * 2];
    snprintf(low, sizeof(low), "%s", path);
    for (char *q = low; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    int lo = 0, hi = p->nby_path - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(p->leafs[p->by_path[mid]].path, low);
        if (c == 0) return &p->leafs[p->by_path[mid]];
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int pvf_read(const Pvf *p, const Leaf *lf, Buf *out) {
    buf_clear(out);
    if (!p || !lf || !p->base) return 0;
    if ((size_t)p->data_start + lf->off + lf->len > p->size) return 0;
    buf_reserve(out, (size_t)lf->len + 4);
    memcpy(out->p, p->base + p->data_start + lf->off, lf->len);
    out->n = lf->len;
    pvf_decrypt(out->p, out->n, lf->crc);
    return 1;
}

// ---------------- stringtable / n_string / lst / stk ----------------
static void load_stringtable(Pvf *p) {
    stab_free(&p->strings);
    const Leaf *lf = pvf_find(p, "stringtable.bin");
    if (!lf) return;
    Buf d = {0};
    if (!pvf_read(p, lf, &d) || d.n < 8) { buf_free(&d); return; }

    u32 count = rd32(d.p);
    if (count > 4000000 || 4 + (size_t)count * 8 > d.n) { buf_free(&d); return; }

    p->str_cp = cn_codepage((const char *)d.p, (int)(d.n < 262144 ? d.n : 262144));

    // 索引里的 s/e 是相对「表数据起点(data+4)」的偏移，flat 是连续 offsets 数组
    const u8 *flat = d.p + 4;
    size_t flat_len = d.n - 4;
    int nflat = (int)(flat_len / 4);
    int nstr = (int)(count * 2);
    if (nstr > nflat - 1) nstr = nflat - 1;

    StabBuild sb;
    sb_init(&sb, &p->strings);
    Str tmp = {0};
    for (int i = 0; i < nstr; i++) {
        u32 s = rd32(flat + i * 4), e = rd32(flat + (i + 1) * 4);
        if (s >= e || e > flat_len) { sb_add(&sb, "", 0); continue; }
        utf8_from_cp((const char *)flat + s, (int)(e - s), p->str_cp, &tmp);
        sb_add(&sb, str_c(&tmp), tmp.n);
    }
    str_free(&tmp);
    buf_free(&d);
}

static void load_nstring(Pvf *p) {
    idtab_free(&p->nstr);
    const Leaf *lf = pvf_find(p, "n_string.lst");
    if (!lf) return;
    Buf c = {0};
    if (!pvf_read(p, lf, &c)) { buf_free(&c); return; }

    IdBuild ib;
    ib_init(&ib, &p->nstr);
    for (size_t i = 2; i + 10 <= c.n; i += 10) {
        i8 a = (i8)c.p[i], b = (i8)c.p[i + 5];
        u32 aa = rd32(c.p + i + 1), bb = rd32(c.p + i + 6);
        u32 index = 0, sidx = 0;
        if (a == 2) index = aa;
        else if (a == 7) sidx = aa;
        if (b == 2) index = bb;
        else if (b == 7) sidx = bb;
        const char *s = stab_get(&p->strings, (int)sidx);
        if (*s) ib_add(&ib, index, s, 0);
    }
    ib_sort(&ib);
    buf_free(&c);
}

typedef struct {
    u32  id;
    char path[MAX_PATH * 2];
} LstPair;

// *.lst：2 字节版本，之后每 5 字节 (type:u8, value:u32)；type 2 索引、7 路径
static void parse_lst(Pvf *p, const Buf *c, const char *base_dir, Vec *out) {
    vec_clear(out);
    if (c->n < 7) return;
    size_t n = (c->n - 2) / 5;
    u32 pending = 0;
    int has = 0;
    for (size_t i = 0; i < n; i++) {
        u8 t = c->p[2 + i * 5];
        u32 v = rd32(c->p + 2 + i * 5 + 1);
        if (t == 2) { pending = v; has = 1; continue; }
        if (t != 7 || !has) continue;
        has = 0;
        const char *s = stab_get(&p->strings, (int)v);
        if (!*s) continue;
        LstPair *lp = VEC_ADD(out, LstPair);
        lp->id = pending;
        if (base_dir && *base_dir) snprintf(lp->path, sizeof(lp->path), "%s/%s", base_dir, s);
        else snprintf(lp->path, sizeof(lp->path), "%s", s);
        char *d = lp->path;
        for (char *q = lp->path; *q;) {
            if (q[0] == '/' && q[1] == '/') { q++; continue; }
            if (*q >= 'A' && *q <= 'Z') { *d++ = (char)(*q++ - 'A' + 'a'); continue; }
            *d++ = *q++;
        }
        *d = 0;
    }
}

// 二次替换：n_string[id] 指向的 .str，内容是「key>value」逐行。
// 这份文件是整份语言名单（几 MB），必须一次建成 key→value 索引后二分，
// 逐行线性扫描会让字典构建从 1.5s 变成 45s。
#define NSTR_CACHE 8

typedef struct {
    const char *k;
    const char *v;
} NstrPair;

typedef struct {
    u32         id;
    int         used;
    char       *blob;
    NstrPair   *pair;    // 按 key 排序
    int         n;
} NstrMap;

static NstrMap g_nstr[NSTR_CACHE];
static int g_nstr_pos;

static int cmp_nstrpair(const void *a, const void *b) {
    return strcmp(((const NstrPair *)a)->k, ((const NstrPair *)b)->k);
}

static void nstr_build(Pvf *p, NstrMap *m, u32 id) {
    m->used = 1;
    m->id = id;
    free(m->blob);
    free(m->pair);
    m->blob = NULL;
    m->pair = NULL;
    m->n = 0;

    const char *sub = idtab_get(&p->nstr, id);
    if (!*sub) return;
    const Leaf *lf = pvf_find(p, sub);
    if (!lf) return;
    Buf c = {0};
    if (!pvf_read(p, lf, &c)) { buf_free(&c); return; }

    Str text = {0};
    decode_cn((const char *)c.p, (int)c.n, &text);
    buf_free(&c);

    m->blob = str_take(&text);
    int cap = 64;
    m->pair = (NstrPair *)xalloc(sizeof(NstrPair) * (size_t)cap);
    for (char *line = m->blob; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        char *gt = strchr(line, '>');
        if (gt) {
            *gt = 0;
            char *v = gt + 1;
            size_t len = strlen(v);
            while (len && (v[len - 1] == '\r' || v[len - 1] == '\n')) v[--len] = 0;
            if (m->n == cap) {
                cap *= 2;
                m->pair = (NstrPair *)xrealloc(m->pair, sizeof(NstrPair) * (size_t)cap);
            }
            m->pair[m->n].k = line;
            m->pair[m->n].v = v;
            m->n++;
        }
        line = eol ? eol + 1 : NULL;
    }
    if (m->n > 1) qsort(m->pair, (size_t)m->n, sizeof(NstrPair), cmp_nstrpair);
}

static const char *nstr_value(Pvf *p, u32 id, const char *key, Str *out) {
    str_clear(out);
    NstrMap *m = NULL;
    for (int i = 0; i < NSTR_CACHE; i++)
        if (g_nstr[i].used && g_nstr[i].id == id) { m = &g_nstr[i]; break; }
    if (!m) {
        m = &g_nstr[g_nstr_pos];
        g_nstr_pos = (g_nstr_pos + 1) % NSTR_CACHE;
        nstr_build(p, m, id);
    }
    int lo = 0, hi = m->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(m->pair[mid].k, key);
        if (c == 0) { str_set(out, m->pair[mid].v); return str_c(out); }
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return "";
}

static int is_seg_name(const char *s) {
    size_t n = strlen(s);
    return n >= 3 && s[0] == '[' && s[n - 1] == ']';
}

// 取 [name] 段下第一个非段名字符串。段名只需记住「当前段是否就是要找的段」，
// 因此不做任何字符串拷贝——装备 stk 动辄上千单元，拷贝会成为瓶颈。
static void stk_field(Pvf *p, const Buf *c, const char *want, Str *out) {
    str_clear(out);
    if (c->n < 7) return;
    size_t n = (c->n - 2) / 5;
    int in_want = 0;
    Str tmp = {0};
    for (size_t i = 0; i < n; i++) {
        u8 t = c->p[2 + i * 5];
        u32 v = rd32(c->p + 2 + i * 5 + 1);
        const char *sv;
        if (t >= 5 && t <= 8) {
            sv = stab_get(&p->strings, (int)v);
        } else if (t == 9) {
            if (i + 1 >= n) break;
            u32 k = rd32(c->p + 2 + (i + 1) * 5 + 1);
            i++;
            const char *key = stab_get(&p->strings, (int)k);
            if (!*key) continue;
            sv = nstr_value(p, v, key, &tmp);
        } else {
            continue;
        }
        if (!sv || !*sv) continue;
        if (is_seg_name(sv)) {
            in_want = strcmp(sv, want) == 0;
            continue;
        }
        if (in_want) {
            str_set(out, sv);
            break;
        }
    }
    str_free(&tmp);
}

// 同上，但段名本身也算值：[type] 段里放的是 [seeking] 这种「段名当枚举值」的写法
static void stk_field_any(Pvf *p, const Buf *c, const char *want, Str *out) {
    str_clear(out);
    if (c->n < 7) return;
    size_t n = (c->n - 2) / 5;
    int in_want = 0;
    Str tmp = {0};
    for (size_t i = 0; i < n; i++) {
        u8 t = c->p[2 + i * 5];
        u32 v = rd32(c->p + 2 + i * 5 + 1);
        const char *sv;
        if (t >= 5 && t <= 8) {
            sv = stab_get(&p->strings, (int)v);
        } else if (t == 9) {
            if (i + 1 >= n) break;
            u32 k = rd32(c->p + 2 + (i + 1) * 5 + 1);
            i++;
            const char *key = stab_get(&p->strings, (int)k);
            if (!*key) continue;
            sv = nstr_value(p, v, key, &tmp);
        } else {
            continue;
        }
        if (!sv || !*sv) continue;
        if (is_seg_name(sv)) {
            if (in_want) { str_set(out, sv); break; }
            in_want = strcmp(sv, want) == 0;
            continue;
        }
        if (in_want) { str_set(out, sv); break; }
    }
    str_free(&tmp);
}

// 取当前段下的一串 u32（最多 max 个），返回实际个数。
// 数字在 stk 里是 t=2/3 的单元，不是字符串 —— 拿 stk_field 去找 [stack limit]
// 这类段只会一无所获，它眼里只有字符串单元。
static int stk_ints(Pvf *p, const Buf *c, const char *want, u32 *out, int max) {
    if (c->n < 7) return 0;
    size_t n = (c->n - 2) / 5;
    int in_want = 0, got = 0;
    for (size_t i = 0; i < n; i++) {
        u8 t = c->p[2 + i * 5];
        u32 v = rd32(c->p + 2 + i * 5 + 1);
        if (t >= 5 && t <= 8) {
            const char *sv = stab_get(&p->strings, (int)v);
            if (*sv && is_seg_name(sv)) { in_want = strcmp(sv, want) == 0; continue; }
            continue;
        }
        if (t == 9) {                       // 引用型字符串，跳过它的键
            if (i + 1 < n) i++;
            continue;
        }
        if ((t == 2 || t == 3) && in_want && got < max) out[got++] = v;
    }
    return got;
}

// 物品分类只看文件落在哪个目录：PVF 没有「类型」字段，目录就是它唯一的分类依据。
// 分界与游戏里的物品栏号段一一对应（装备 9-56 / 消耗品 57-104 / 材料 105-152 /
// 任务材料 153-200），因此这里判错，发出去的物品格子就不合法。
static u8 kind_of_equip(const char *path) {
    return strstr(path, "/avatar/") ? PK_AVATAR : PK_EQUIP;
}

static u8 kind_of_stack(const char *path) {
    if (strncmp(path, "stackable/material", 18) == 0) return PK_MATERIAL;
    if (strncmp(path, "stackable/quest", 15) == 0) return PK_QUEST;
    return PK_STACK;
}

// 把 lst 里每个 id 的名称塞进 ib。kof 非空时按文件路径顺手记下分类
static void lst_into_idtab(Pvf *p, const char *lst_path, const char *base_dir, IdBuild *ib,
                           u8 (*kof)(const char *path)) {
    const Leaf *lf = pvf_find(p, lst_path);
    if (!lf) return;
    Buf data = {0};
    if (!pvf_read(p, lf, &data)) { buf_free(&data); return; }

    Vec pairs;
    vec_init(&pairs, sizeof(LstPair));
    parse_lst(p, &data, base_dir, &pairs);
    buf_free(&data);

    Str name = {0};
    Buf c = {0};
    for (int i = 0; i < pairs.n; i++) {
        LstPair *lp = VEC_AT(&pairs, LstPair, i);
        const Leaf *sub = pvf_find(p, lp->path);
        if (!sub) continue;
        if (!pvf_read(p, sub, &c)) continue;
        stk_field(p, &c, "[name]", &name);
        // 堆叠上限只在物品表里要：旁的表（职业 / 宠物 / n_string）没有这个段，
        // 为它们白扫一遍纯属浪费 —— 装备 stk 动辄上千单元，这是热路径。
        u32 stk = 0;
        if (ib->want_kind) {
            u32 v[1] = {0};
            if (stk_ints(p, &c, "[stack limit]", v, 1)) stk = v[0];
        }
        ib->cur_kind = kof ? kof(lp->path) : 0;
        ib_add(ib, lp->id, name.n ? name.p : "", stk);
    }
    ib->cur_kind = 0;
    str_free(&name);
    buf_free(&c);
    vec_free(&pairs);
}

// ---------------- 头/树解析 ----------------
static int map_file(Pvf *p, Err err) {
    WCHAR w[MAX_PATH * 2];
    utf16(p->path, w, MAX_PATH * 2);
    p->file = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (p->file == INVALID_HANDLE_VALUE) { p->file = NULL; snprintf(err, ERRBUF, "打开 PVF 失败"); return 0; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(p->file, &sz)) { snprintf(err, ERRBUF, "读取文件大小失败"); return 0; }
    p->size = (size_t)sz.QuadPart;
    p->map = CreateFileMappingW(p->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!p->map) { snprintf(err, ERRBUF, "映射 PVF 失败"); return 0; }
    p->base = (const u8 *)MapViewOfFile(p->map, FILE_MAP_READ, 0, 0, 0);
    if (!p->base) { snprintf(err, ERRBUF, "映射视图失败"); return 0; }
    return 1;
}

static int parse_tree(Pvf *p, Err err, PvfProg prog) {
    if (!map_file(p, err)) return 0;
    if (p->size < 64) { snprintf(err, ERRBUF, "文件过小，不是有效 PVF"); return 0; }

    const u8 *b = p->base;
    u32 uuid_len = rd32(b);
    // 上界必须按 uuid 字段本身的大小来卡。以前写的是 256，而 uuid 只有 64 字节：
    // 一个 uuid_len=200 的坏文件就会把 version / tree_len / file / base 一起踩掉。
    // 正常 PVF 的 uuid 是 32 字节左右，卡到 sizeof-1 不影响真实文件。
    if (uuid_len == 0 || uuid_len > sizeof(p->uuid) - 1 || uuid_len + 20 > p->size) {
        snprintf(err, ERRBUF, "PVF 头非法");
        return 0;
    }
    memcpy(p->uuid, b + 4, uuid_len);
    p->uuid[uuid_len] = 0;
    size_t off = 4 + uuid_len;
    p->version = rd32(b + off);
    p->tree_len = rd32(b + off + 4);
    p->tree_crc = rd32(b + off + 8);
    p->num_files = rd32(b + off + 12);
    if (p->num_files == 0 || p->num_files > 4000000) { snprintf(err, ERRBUF, "PVF 头字段异常"); return 0; }
    u32 header_len = (u32)(off + 16);
    if ((size_t)header_len + p->tree_len > p->size) { snprintf(err, ERRBUF, "文件树长度越界"); return 0; }
    p->data_start = header_len + p->tree_len;

    if (prog.pct) *prog.pct = 12;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解密文件树");
    u8 *tree = (u8 *)xalloc(p->tree_len + 4);
    memcpy(tree, b + header_len, p->tree_len);
    pvf_decrypt(tree, p->tree_len, p->tree_crc);

    if (prog.pct) *prog.pct = 22;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解析文件树");
    p->leafs = (Leaf *)xalloc(sizeof(Leaf) * (size_t)p->num_files);
    size_t o = 0;
    for (u32 i = 0; i < p->num_files; i++) {
        if (o + 8 > p->tree_len) {
            snprintf(err, ERRBUF, "文件树在第 %u 条截断", i);
            free(tree);
            return 0;
        }
        Leaf *lf = &p->leafs[i];
        lf->fn = rd32(tree + o);
        o += 4;
        u32 plen = rd32(tree + o);
        o += 4;
        if (o + plen + 12 > p->tree_len) {
            snprintf(err, ERRBUF, "文件树路径越界 @%u", i);
            free(tree);
            return 0;
        }
        lf->path = (char *)xalloc(plen + 1);
        memcpy(lf->path, tree + o, plen);
        lf->path[plen] = 0;
        o += plen;
        lf->len_raw = rd32(tree + o);
        lf->len = (lf->len_raw + 3) & 0xFFFFFFFCu;
        o += 4;
        lf->crc = rd32(tree + o);
        o += 4;
        lf->off = rd32(tree + o);
        o += 4;
        p->nleaf++;
        for (char *q = lf->path; *q; q++)
            if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
        if (lf->path[0] == '/') memmove(lf->path, lf->path + 1, strlen(lf->path));
    }
    free(tree);
    build_path_index(p);

    if (prog.pct) *prog.pct = 45;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "读取字符串表");
    load_stringtable(p);
    load_nstring(p);
    return 1;
}

// ---------------- 字典 ----------------
static void build_dicts(Pvf *p, PvfProg prog) {
    if (prog.pct) *prog.pct = 55;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解析物品名称");
    idtab_free(&p->items);
    free(p->kind);
    free(p->stk);
    p->kind = NULL;
    p->stk = NULL;
    {
        IdBuild ib;
        ib_init2(&ib, &p->items, 1);    // 物品表要带分类和堆叠上限，别的表不要
        lst_into_idtab(p, "equipment/equipment.lst", "equipment", &ib, kind_of_equip);
        lst_into_idtab(p, "stackable/stackable.lst", "stackable", &ib, kind_of_stack);
        ib_sort(&ib);
        p->kind = ib.kind;
        p->stk = ib.stk;
        ib.kind = NULL;
        ib.stk = NULL;
    }

    if (prog.pct) *prog.pct = 8;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解析职业");
    idtab_free(&p->jobs);
    {
        IdBuild ib;
        ib_init(&ib, &p->jobs);
        lst_into_idtab(p, "character/character.lst", "character", &ib, NULL);
        ib_sort(&ib);
    }

    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解析宠物");
    idtab_free(&p->cre);
    {
        // 宠物编号不在物品表里（两边的编号会撞，比如 1 既是金币又是一只宠物的编号），
        // 单独存一张表，只用来认「这个编号属于宠物」。
        IdBuild ib;
        ib_init(&ib, &p->cre);
        lst_into_idtab(p, "creature/creature.lst", "creature", &ib, NULL);
        lst_into_idtab(p, "pet/pet.lst", "pet", &ib, NULL);
        ib_sort(&ib);
    }

    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "解析封印");
    stab_free(&p->seals);
    {
        // 前 144 组 (id + 等级名索引)，按 i%256 落位
        Str names[256];
        for (int i = 0; i < 256; i++) memset(&names[i], 0, sizeof(Str));
        const Leaf *lf = pvf_find(p, "etc/randomoption/randomizedoptionoverall2.etc");
        if (lf) {
            Buf c = {0};
            if (pvf_read(p, lf, &c) && c.n > 4) {
                size_t n = (c.n - 2) / 5;
                for (size_t i = 0; i < n; i++) {
                    u8 t = c.p[2 + i * 5];
                    if (t < 5 || t > 8) continue;
                    const char *s = stab_get(&p->strings, (int)rd32(c.p + 2 + i * 5 + 1));
                    int slot = (int)(i % 256);
                    if (*s && !names[slot].n) str_set(&names[slot], s);
                }
            }
            buf_free(&c);
        }
        StabBuild sb;
        sb_init(&sb, &p->seals);
        for (int i = 0; i < 256; i++) sb_add(&sb, str_c(&names[i]), names[i].n);
        for (int i = 0; i < 256; i++) str_free(&names[i]);
    }

    free(p->exp);
    p->exp = NULL;
    p->nexp = 0;
    {
        const Leaf *lf = pvf_find(p, "character/exptable.tbl");
        Buf c = {0};
        if (lf)
        if (pvf_read(p, lf, &c) && c.n > 7) {
            size_t n = (c.n - 2) / 5;
            p->exp = (u32 *)xalloc(sizeof(u32) * (n + 2));
            for (size_t i = 0; i < n; i++)
                if (c.p[2 + i * 5] == 2) p->exp[p->nexp++] = rd32(c.p + 2 + i * 5 + 1);
        }
        buf_free(&c);
    }

    stab_free(&p->avatar_hidden);
    {
        const Leaf *lf = pvf_find(p, "etc/avatar_roulette/avatarfixedhiddenoptionlist.etc");
        Buf c = {0};
        if (lf && pvf_read(p, lf, &c)) {
            StabBuild sb;
            sb_init(&sb, &p->avatar_hidden);
            size_t n = (c.n - 2) / 5;
            for (size_t i = 0; i < n; i++) {
                u8 t = c.p[2 + i * 5];
                if (t < 5 || t > 8) continue;
                const char *s = stab_get(&p->strings, (int)rd32(c.p + 2 + i * 5 + 1));
                sb_add(&sb, s, (int)strlen(s));
            }
        }
        buf_free(&c);
    }
}

// ---------------- 缓存 ----------------
// 一份 PVF 的缓存有几十 MB，单独放 config/cache，同一份 PVF 只留最新一个，
// 换端时最多留 2 份，免得越攒越多。
static void cache_path_of(const Pvf *p, char *out, int cap) {
    char dir[MAX_PATH];
    art_dir("cache", dir, sizeof(dir));
    snprintf(out, (size_t)cap, "%s\\pvf-%.8s.cache", dir, p->fp);
}

static void w_str(Buf *b, const char *s, int n) {
    buf_put32(b, (u32)n);
    buf_put(b, s, (size_t)n);
}

static void write_stab(Buf *b, const StrTab *t) {
    buf_put32(b, (u32)t->n);
    buf_put32(b, (u32)t->blob_len);
    buf_put(b, t->blob, t->blob_len);
    for (int i = 0; i < t->n; i++) buf_put32(b, t->off[i]);
}

static void write_idtab(Buf *b, const IdTab *t) {
    buf_put32(b, (u32)t->n);
    for (int i = 0; i < t->n; i++) buf_put32(b, t->key[i]);
    for (int i = 0; i < t->n; i++) buf_put32(b, t->stroff[i]);
    write_stab(b, &t->names);
}

static void save_cache(Pvf *p) {
    Buf b = {0};
    buf_put(&b, CACHE_MAGIC, CACHE_MAGIC_LEN);
    buf_put32(&b, p->version);
    buf_put32(&b, p->tree_len);
    buf_put32(&b, p->tree_crc);
    buf_put32(&b, p->num_files);
    buf_put32(&b, p->data_start);
    buf_put32(&b, p->str_cp);
    w_str(&b, p->uuid, (int)strlen(p->uuid));
    w_str(&b, p->fp, 32);
    buf_put32(&b, (u32)p->nleaf);
    for (int i = 0; i < p->nleaf; i++) {
        Leaf *l = &p->leafs[i];
        buf_put32(&b, l->fn);
        buf_put32(&b, l->len);
        buf_put32(&b, l->len_raw);
        buf_put32(&b, l->crc);
        buf_put32(&b, l->off);
        w_str(&b, l->path, (int)strlen(l->path));
    }
    write_stab(&b, &p->strings);
    write_idtab(&b, &p->items);
    buf_put32(&b, (u32)p->items.n);
    for (int i = 0; i < p->items.n; i++) buf_putc(&b, p->kind ? p->kind[i] : 0);
    buf_put32(&b, (u32)p->items.n);
    for (int i = 0; i < p->items.n; i++) buf_put32(&b, p->stk ? p->stk[i] : 0);
    write_idtab(&b, &p->cre);
    write_idtab(&b, &p->jobs);
    write_stab(&b, &p->seals);
    buf_put32(&b, (u32)p->nexp);
    for (int i = 0; i < p->nexp; i++) buf_put32(&b, p->exp[i]);
    write_stab(&b, &p->avatar_hidden);
    write_idtab(&b, &p->nstr);

    char path[MAX_PATH];
    cache_path_of(p, path, sizeof(path));
    file_write(path, b.p, b.n);
    buf_free(&b);

    char dir[MAX_PATH];
    art_dir("cache", dir, sizeof(dir));
    art_prune(dir, "pvf-", 2);
}

typedef struct {
    const u8 *p;
    size_t n, o;
    int ok;
} Rd;

static u32 r_u32(Rd *r) {
    if (r->o + 4 > r->n) { r->ok = 0; return 0; }
    u32 v = rd32(r->p + r->o);
    r->o += 4;
    return v;
}

static const char *r_bytes(Rd *r, int *len) {
    u32 l = r_u32(r);
    *len = 0;
    if (!r->ok || r->o + l > r->n) { r->ok = 0; return ""; }
    const char *s = (const char *)r->p + r->o;
    r->o += l;
    *len = (int)l;
    return s;
}

static void read_stab(Rd *r, StrTab *t) {
    memset(t, 0, sizeof(*t));
    int n = (int)r_u32(r);
    u32 blob_len = r_u32(r);
    if (!r->ok || n < 0 || n > 9000000 || r->o + blob_len > r->n) { r->ok = 0; return; }
    t->blob = (char *)xalloc(blob_len + 1);
    memcpy(t->blob, r->p + r->o, blob_len);
    t->blob[blob_len] = 0;
    r->o += blob_len;
    t->blob_len = blob_len;
    t->off = (u32 *)xalloc(sizeof(u32) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) t->off[i] = r_u32(r);
    t->n = n;
}

static void read_idtab(Rd *r, IdTab *t) {
    memset(t, 0, sizeof(*t));
    int n = (int)r_u32(r);
    if (!r->ok || n < 0 || n > 4000000) { r->ok = 0; return; }
    t->key = (u32 *)xalloc(sizeof(u32) * (size_t)(n + 1));
    t->stroff = (u32 *)xalloc(sizeof(u32) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) t->key[i] = r_u32(r);
    for (int i = 0; i < n; i++) t->stroff[i] = r_u32(r);
    read_stab(r, &t->names);
    t->n = n;
}

static int load_cache(Pvf *p) {
    char path[MAX_PATH];
    cache_path_of(p, path, sizeof(path));
    Buf raw = {0};
    if (!file_read(path, &raw) || raw.n < 24) { buf_free(&raw); return 0; }
    if (memcmp(raw.p, CACHE_MAGIC, CACHE_MAGIC_LEN) != 0) { buf_free(&raw); return 0; }

    Rd r = {raw.p, raw.n, CACHE_MAGIC_LEN, 1};
    p->version = r_u32(&r);
    p->tree_len = r_u32(&r);
    p->tree_crc = r_u32(&r);
    p->num_files = r_u32(&r);
    p->data_start = r_u32(&r);
    p->str_cp = r_u32(&r);
    int len = 0;
    const char *u = r_bytes(&r, &len);
    snprintf(p->uuid, sizeof(p->uuid), "%.*s", len, u);
    const char *m = r_bytes(&r, &len);
    if (!r.ok || len != 32 || memcmp(m, p->fp, 32) != 0) { buf_free(&raw); return 0; }

    int nl = (int)r_u32(&r);
    if (!r.ok || nl <= 0 || nl > 4000000) { buf_free(&raw); return 0; }
    p->leafs = (Leaf *)xalloc(sizeof(Leaf) * (size_t)nl);
    for (int i = 0; i < nl; i++) {
        Leaf *l = &p->leafs[i];
        l->fn = r_u32(&r);
        l->len = r_u32(&r);
        l->len_raw = r_u32(&r);
        l->crc = r_u32(&r);
        l->off = r_u32(&r);
        const char *s = r_bytes(&r, &len);
        l->path = (char *)xalloc((size_t)len + 1);
        memcpy(l->path, s, (size_t)len);
        l->path[len] = 0;
        p->nleaf++;
    }
    read_stab(&r, &p->strings);
    read_idtab(&r, &p->items);
    {
        int n = (int)r_u32(&r);
        if (!r.ok || n != p->items.n || r.o + (size_t)n > r.n) { buf_free(&raw); return 0; }
        free(p->kind);
        p->kind = (u8 *)xalloc((size_t)n + 1);
        memcpy(p->kind, r.p + r.o, (size_t)n);
        r.o += (size_t)n;
    }
    {
        int n = (int)r_u32(&r);
        if (!r.ok || n != p->items.n || r.o + sizeof(u32) * (size_t)n > r.n) { buf_free(&raw); return 0; }
        free(p->stk);
        p->stk = (u32 *)xalloc(sizeof(u32) * (size_t)(n + 1));
        memcpy(p->stk, r.p + r.o, sizeof(u32) * (size_t)n);
        r.o += sizeof(u32) * (size_t)n;
    }
    read_idtab(&r, &p->cre);
    read_idtab(&r, &p->jobs);
    read_stab(&r, &p->seals);
    p->nexp = (int)r_u32(&r);
    if (r.ok && p->nexp >= 0 && p->nexp <= 10000) {
        p->exp = (u32 *)xalloc(sizeof(u32) * (size_t)(p->nexp + 1));
        for (int i = 0; i < p->nexp; i++) p->exp[i] = r_u32(&r);
    }
    read_stab(&r, &p->avatar_hidden);
    read_idtab(&r, &p->nstr);
    buf_free(&raw);
    if (!r.ok) return 0;

    build_path_index(p);
    Err err = {0};
    if (!map_file(p, err)) return 0;
    if (p->strings.n == 0) load_stringtable(p);
    load_nstring(p);
    return 1;
}

// ---------------- 对外 ----------------
static void reset_keep_id(Pvf *p) {
    char path[MAX_PATH], fp[33];
    snprintf(path, sizeof(path), "%s", p->path);
    snprintf(fp, sizeof(fp), "%s", p->fp);
    pvf_close(p);
    snprintf(p->path, sizeof(p->path), "%s", path);
    snprintf(p->fp, sizeof(p->fp), "%s", fp);
}

int pvf_open(Pvf *p, const char *path, PvfProg prog) {
    Err err = {0};
    pvf_close(p);
    snprintf(p->path, sizeof(p->path), "%s", path);
    if (prog.pct) *prog.pct = 2;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "比对文件指纹");

    if (!file_exists(path)) return 0;
    // 指纹只取长度 + 头尾各 64KB：全量 md5 要半秒多，而判断「包换没换」用不着那么严
    if (!hash_file_fp(path, p->fp)) return 0;

    if (load_cache(p)) {
        p->opened = 1;
        if (prog.pct) *prog.pct = 100;
        if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "缓存已就绪");
        return 1;
    }
    reset_keep_id(p);

    if (!parse_tree(p, err, prog)) return 0;
    build_dicts(p, prog);
    if (prog.pct) *prog.pct = 96;
    if (prog.stage) snprintf(prog.stage, (size_t)prog.stage_cap, "写入缓存");
    save_cache(p);
    if (prog.pct) *prog.pct = 100;
    p->opened = 1;
    return 1;
}

void pvf_close(Pvf *p) {
    if (!p) return;
    if (p->base) { UnmapViewOfFile(p->base); p->base = NULL; }
    if (p->map) { CloseHandle(p->map); p->map = NULL; }
    if (p->file) { CloseHandle(p->file); p->file = NULL; }
    for (int i = 0; i < p->nleaf; i++) free(p->leafs[i].path);
    free(p->leafs);
    free(p->by_path);
    p->leafs = NULL;
    p->by_path = NULL;
    p->nleaf = p->nby_path = 0;
    stab_free(&p->strings);
    idtab_free(&p->items);
    free(p->kind);
    free(p->stk);
    free(p->ileaf);
    p->kind = NULL;
    p->stk = NULL;
    p->ileaf = NULL;
    idtab_free(&p->cre);
    idtab_free(&p->jobs);
    stab_free(&p->seals);
    stab_free(&p->avatar_hidden);
    idtab_free(&p->nstr);
    free(p->exp);
    p->exp = NULL;
    p->nexp = 0;
    p->size = 0;
    p->opened = 0;
    p->fp[0] = 0;
    p->path[0] = 0;
}

const char *pvf_item_name(const Pvf *p, u32 id) { return idtab_get(&p->items, id); }

// 物品编号在物品表里的下标（-1 = 没有这件）
static int key_index(const IdTab *t, u32 key) {
    if (!t || t->n == 0) return -1;
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (t->key[mid] == key) return mid;
        if (t->key[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int pvf_item_kind(const Pvf *p, u32 id) {
    if (!p || !p->opened) return PK_NONE;
    int i = key_index(&p->items, id);
    if (i >= 0) {
        u8 k = p->kind ? p->kind[i] : 0;
        return k ? (int)k : PK_STACK;
    }
    // 物品表里没有，再看是不是宠物：宠物编号自成一套，会和物品编号撞车
    // （1 既是金币又是一种宠物），所以顺序不能反 —— 先认物品，认不出来才当宠物。
    if (p->cre.n && key_index(&p->cre, id) >= 0) return PK_CREATURE;
    return PK_NONE;
}

// 堆叠上限 [stack limit]。返回 0 表示这个编号没声明上限 —— 装备大多没有这一段，
// 而它们的「数量」位装的是别的东西，本来就不该按堆叠来判，见 item_bad 里的处理。
u32 pvf_item_stack_limit(const Pvf *p, u32 id) {
    if (!p || !p->opened || !p->stk) return 0;
    int i = key_index(&p->items, id);
    return i >= 0 ? p->stk[i] : 0;
}

const char *pvf_kind_name(int kind) {
    switch (kind) {
        case PK_EQUIP: return "装备";
        case PK_STACK: return "消耗品";
        case PK_MATERIAL: return "材料";
        case PK_QUEST: return "任务材料";
        case PK_AVATAR: return "时装";
        case PK_CREATURE: return "宠物";
        default: return "未知";
    }
}

const char *pvf_job_name(const Pvf *p, int job) { return idtab_get(&p->jobs, (u32)job); }

const char *pvf_seal_name(const Pvf *p, int id) {
    return (id >= 0 && id < p->seals.n) ? stab_get(&p->seals, id) : "";
}

int pvf_is_equip(const Pvf *p, u32 id) {
    int k = pvf_item_kind(p, id);
    return k == PK_EQUIP || k == PK_AVATAR;
}

int pvf_item_count(const Pvf *p) { return p ? p->items.n : 0; }

u32 pvf_item_at(const Pvf *p, int i, const char **name) {
    if (!p || i < 0 || i >= p->items.n) {
        if (name) *name = "";
        return 0;
    }
    if (name) *name = stab_get(&p->items.names, (int)p->items.stroff[i]);
    return p->items.key[i];
}

// ---------------- 物品路径 / 图标 / 详情（悬浮卡片用） ----------------
// lst 是「编号 → 文件相对路径」的对照表。建索引时只留了名字没留路径，
// 这里懒建一张平行表：第一次要用路径时把两张 lst 各扫一遍，之后 O(1)。
static void item_leafs(Pvf *p) {
    if (p->ileaf) return;
    int n = p->items.n;
    p->ileaf = (int *)xalloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) p->ileaf[i] = -1;
    static const char *lsts[] = {"equipment/equipment.lst", "stackable/stackable.lst"};
    for (int li = 0; li < 2; li++) {
        const Leaf *lf = pvf_find(p, lsts[li]);
        if (!lf) continue;
        Buf data = {0};
        if (!pvf_read(p, lf, &data)) continue;
        if (data.n >= 7) {
            size_t cnt = (data.n - 2) / 5;
            u32 pending = 0;
            int has = 0;
            for (size_t i = 0; i < cnt; i++) {
                u8 t = data.p[2 + i * 5];
                u32 v = rd32(data.p + 2 + i * 5 + 1);
                if (t == 2) { pending = v; has = 1; continue; }
                if (t != 7 || !has) continue;
                has = 0;
                const char *s = stab_get(&p->strings, (int)v);
                if (!*s) continue;
                int ti = key_index(&p->items, pending);
                if (ti < 0 || p->ileaf[ti] >= 0) continue;
                char path[MAX_PATH * 2];
                const char *base = li == 0 ? "equipment" : "stackable";
                snprintf(path, sizeof(path), "%s/%s", base, s);
                for (char *q = path; *q; q++)
                    if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
                const Leaf *sub = pvf_find(p, path);
                if (sub) p->ileaf[ti] = (int)(sub - p->leafs);
            }
        }
        buf_free(&data);
    }
}

const char *pvf_item_path(const Pvf *p, u32 id, char *out, int cap) {
    out[0] = 0;
    if (!p || !p->opened) return out;
    item_leafs((Pvf *)p);
    int i = key_index(&p->items, id);
    if (i < 0 || p->ileaf[i] < 0) return out;
    snprintf(out, (size_t)cap, "%s", p->leafs[p->ileaf[i]].path);
    return out;
}

// [icon] 段：一个字符串（img 路径）+ 一个数（帧号）。别的段不管。
int pvf_item_icon(const Pvf *p, u32 id, char *img, int cap, int *frame) {
    img[0] = 0;
    if (frame) *frame = 0;
    if (!p || !p->opened) return 0;
    char path[MAX_PATH * 2];
    pvf_item_path(p, id, path, sizeof(path));
    if (!path[0]) return 0;
    const Leaf *lf = pvf_find(p, path);
    if (!lf) return 0;
    Buf c = {0};
    if (!pvf_read(p, lf, &c)) return 0;
    int ok = 0;
    if (c.n >= 7) {
        size_t n = (c.n - 2) / 5;
        int in_want = 0;
        for (size_t i = 0; i < n; i++) {
            u8 t = c.p[2 + i * 5];
            u32 v = rd32(c.p + 2 + i * 5 + 1);
            const char *sv = (t >= 5 && t <= 8) ? stab_get(&p->strings, (int)v) : NULL;
            if (sv && is_seg_name(sv)) {
                in_want = strcmp(sv, "[icon]") == 0;
                continue;
            }
            if (!in_want) continue;
            if (t >= 5 && t <= 8) {
                if (!ok && sv && *sv) {
                    snprintf(img, (size_t)cap, "%s", sv);
                    ok = 1;
                }
            } else if (ok && (t == 2 || t == 3)) {
                if (frame) *frame = (int)v;
                break;
            }
        }
    }
    buf_free(&c);
    return ok;
}

// 取一个段的「值」，拼成文本：数字带空格连排，字符串原样（引用串会顺着 n_string 解）。
// [type] 这类「段名当枚举值」的写法（[seeking] 等）跳过。
static void stk_seg_text(Pvf *p, const Buf *c, const char *want, Str *out) {
    str_clear(out);
    if (c->n < 7) return;
    size_t n = (c->n - 2) / 5;
    int in_want = 0;
    Str tmp = {0};
    for (size_t i = 0; i < n; i++) {
        u8 t = c->p[2 + i * 5];
        u32 v = rd32(c->p + 2 + i * 5 + 1);
        const char *sv;
        if (t >= 5 && t <= 8) {
            sv = stab_get(&p->strings, (int)v);
        } else if (t == 9) {
            if (i + 1 >= n) break;
            u32 k = rd32(c->p + 2 + (i + 1) * 5 + 1);
            i++;
            const char *key = stab_get(&p->strings, (int)k);
            if (!*key) continue;
            sv = nstr_value(p, v, key, &tmp);
        } else {
            if (in_want && (t == 2 || t == 3)) str_catf(out, "%u ", v);
            else if (in_want && t == 4) {
                float f;
                memcpy(&f, &v, 4);
                str_catf(out, "%g ", (double)f);
            }
            continue;
        }
        if (!sv || !*sv) continue;
        if (is_seg_name(sv)) {
            if (in_want) break;        // 下一段开始，本段结束
            in_want = strcmp(sv, want) == 0;
            continue;
        }
        if (in_want) str_catf(out, "%s ", sv);
    }
    str_free(&tmp);
    // 掉尾部空格
    while (out->n && out->p[out->n - 1] == ' ') out->p[--out->n] = 0;
}

int pvf_item_detail(const Pvf *p, u32 id, Str *out) {
    str_clear(out);
    if (!p || !p->opened) return 0;
    const char *name = pvf_item_name(p, id);
    if (!*name) return 0;
    int kind = pvf_item_kind(p, id);

    char path[MAX_PATH * 2];
    pvf_item_path(p, id, path, sizeof(path));
    if (!path[0]) return 0;
    const Leaf *lf = pvf_find(p, path);
    if (!lf) return 0;
    Buf c = {0};
    if (!pvf_read(p, lf, &c)) return 0;

    str_catf(out, "%s  ·  %s  ·  编号 %u\n", name, pvf_kind_name(kind), id);
    Str v = {0};
    stk_seg_text((Pvf *)p, &c, "[minimum level]", &v);
    if (v.n) str_catf(out, "等级 %s 以上", str_c(&v));
    int show_rarity = kind == PK_EQUIP || kind == PK_AVATAR;
    if (show_rarity) {
        stk_seg_text((Pvf *)p, &c, "[rarity]", &v);
        if (v.n) {
            static const char *rar[] = {"普通", "高级", "稀有", "神器", "传说"};
            int r = atoi(str_c(&v));
            str_catf(out, " · %s", r >= 0 && r < 5 ? rar[r] : str_c(&v));
        }
    }
    stk_seg_text((Pvf *)p, &c, "[price]", &v);
    if (v.n) str_catf(out, "%s价格 %s", out->n ? " · " : "", str_c(&v));
    stk_seg_text((Pvf *)p, &c, "[weight]", &v);
    if (v.n) str_catf(out, " · 重量 %s", str_c(&v));
    if (out->n) str_catc(out, '\n');
    if (kind == PK_EQUIP) {
        stk_seg_text((Pvf *)p, &c, "[durability]", &v);
        if (v.n) str_catf(out, "耐久 %s", str_c(&v));
    }
    stk_seg_text((Pvf *)p, &c, "[explain]", &v);
    if (v.n) str_catf(out, "%s说明：%s", out->n ? " · " : "", str_c(&v));
    // 末尾不再附 PVF 内部路径：那是给解析用的，对看卡片的人没用，还占一行
    str_free(&v);
    buf_free(&c);
    return 1;
}

// ---------------- 任务（n_quest/*.qst）----------------
// 任务文件的段和物品 stk 同构：[type] 之后紧跟目标类型（[seeking] 即收集），
// [int data] 给出「物品ID 数量」对。只按需解析，不在加载 PVF 时全量建索引。
typedef struct {
    u32  id;
    char path[MAX_PATH * 2];
} QPair;

static QPair    *g_qp;
static int       g_qn;
static const Pvf *g_qpvf;

static void quest_index(Pvf *p) {
    if (g_qpvf == p && g_qn) return;
    free(g_qp);
    g_qp = NULL;
    g_qn = 0;
    g_qpvf = p;

    const Leaf *lf = pvf_find(p, "n_quest/quest.lst");
    if (!lf) return;
    Buf b = {0};
    if (!pvf_read(p, lf, &b)) return;

    size_t cnt = (b.n - 2) / 5;
    g_qp = calloc(cnt + 1, sizeof(QPair));
    u32 pend = 0;
    int has = 0;
    for (size_t i = 0; i < cnt; i++) {
        u8 t = b.p[2 + i * 5];
        u32 v = rd32(b.p + 2 + i * 5 + 1);
        if (t == 2) { pend = v; has = 1; continue; }
        if (t != 7 || !has) continue;
        has = 0;
        const char *s = stab_get(&p->strings, (int)v);
        if (!*s) continue;
        QPair *q = &g_qp[g_qn++];
        q->id = pend;
        snprintf(q->path, sizeof(q->path), "n_quest/%s", s);
        for (char *c = q->path; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    }
    buf_free(&b);
}

// 解析一个任务文件。路径由调用方给：按 id 查是一遍线性扫，全量扫描时直接
// 拿索引里的路径，省掉那层查找。
static int quest_parse(Pvf *p, const char *path, QuestInfo *out) {
    memset(out, 0, sizeof(*out));
    const Leaf *lf = pvf_find(p, path);
    if (!lf) return 0;
    Buf b = {0};
    if (!pvf_read(p, lf, &b)) return 0;

    Str nm = {0};
    stk_field(p, &b, "[name]", &nm);
    snprintf(out->name, sizeof(out->name), "%s", str_c(&nm));
    str_free(&nm);

    // [type] 之后的第一个段名就是任务目标类型
    Str kd = {0};
    stk_field_any(p, &b, "[type]", &kd);
    snprintf(out->kind, sizeof(out->kind), "%s", str_c(&kd));
    str_free(&kd);

    u32 lv[2] = {0, 0};
    stk_ints(p, &b, "[level]", lv, 2);
    out->lev_min = (int)lv[0];
    out->lev_max = (int)lv[1];

    // 只有收集类任务的 [int data] 才是「物品ID 数量」
    if (strcmp(out->kind, "[seeking]") == 0) {
        u32 d[8] = {0};
        int got = stk_ints(p, &b, "[int data]", d, 8);
        for (int i = 0; i + 1 < got && out->nitem < 4; i += 2) {
            if (!d[i]) continue;
            out->item[out->nitem] = d[i];
            out->cnt[out->nitem] = d[i + 1] ? d[i + 1] : 1;
            out->nitem++;
        }
    }
    buf_free(&b);
    return 1;
}

static int cmp_questmat(const void *a, const void *b) {
    u32 x = ((const QuestMat *)a)->id, y = ((const QuestMat *)b)->id;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int pvf_quest_info(Pvf *p, u32 id, QuestInfo *out) {
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->lev_min = out->lev_max = 0;
    if (!p->opened) return 0;
    quest_index(p);
    const char *path = NULL;
    for (int i = 0; i < g_qn; i++)
        if (g_qp[i].id == id) { path = g_qp[i].path; break; }
    if (!path) return 0;
    if (!quest_parse(p, path, out)) return 0;
    out->id = id;
    return 1;
}

// 「加载任务材料」用：把全部任务过一遍，收「收集类」任务要的东西。
// 同一件材料常被几十个任务要，这里只记两件事：单个任务最多要多少个（取最大，
// 不是累加 —— 不可能同时接所有任务）、一共有多少个任务要它。
int pvf_quest_materials(Pvf *p, QuestMat **out) {
    *out = NULL;
    if (!p || !p->opened) return 0;
    quest_index(p);
    if (!g_qn) return 0;

    int cap = 64, n = 0;
    QuestMat *v = calloc((size_t)cap, sizeof(QuestMat));
    if (!v) return 0;
    for (int i = 0; i < g_qn; i++) {
        QuestInfo qi;
        if (!quest_parse(p, g_qp[i].path, &qi)) continue;
        if (strcmp(qi.kind, "[seeking]") != 0) continue;
        for (int k = 0; k < qi.nitem; k++) {
            u32 id = qi.item[k], cnt = qi.cnt[k];
            int j = 0;
            for (; j < n; j++)
                if (v[j].id == id) break;
            if (j < n) {
                if (cnt > v[j].cnt) v[j].cnt = cnt;
                v[j].quests++;
                continue;
            }
            if (n == cap) {
                cap *= 2;
                QuestMat *nv = realloc(v, (size_t)cap * sizeof(QuestMat));
                if (!nv) break;
                v = nv;
            }
            v[n].id = id;
            v[n].cnt = cnt ? cnt : 1;
            v[n].quests = 1;
            n++;
        }
    }
    qsort(v, (size_t)n, sizeof(QuestMat), cmp_questmat);
    *out = v;
    return n;
}
