// mysql 实现。// port of pymysql 握手/结果集（仅取本工具所需子集）
#include "mysql.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#define CAP_LONG_PASSWORD 0x00000001u
#define CAP_LONG_FLAG 0x00000004u
#define CAP_CONNECT_WITH_DB 0x00000008u
#define CAP_PROTOCOL_41 0x00000200u
#define CAP_TRANSACTIONS 0x00002000u
#define CAP_SECURE_CONNECTION 0x00008000u
#define CAP_PLUGIN_AUTH 0x00080000u

// ---------------- 结果集 ----------------
static void row_reserve(MyResult *r, int n) {
    if (n <= r->cap) return;
    int c = r->cap ? r->cap : 16;
    while (c < n) c *= 2;
    r->offs = (int *)xrealloc(r->offs, sizeof(int) * (size_t)c * (size_t)(r->ncol ? r->ncol : 1));
    r->lens = (int *)xrealloc(r->lens, sizeof(int) * (size_t)c * (size_t)(r->ncol ? r->ncol : 1));
    r->nulls = (u8 *)xrealloc(r->nulls, (size_t)c * (size_t)(r->ncol ? r->ncol : 1));
    r->cap = c;
}

void my_free(MyResult *r) {
    if (!r) return;
    if (r->cols) {
        for (int i = 0; i < r->ncol; i++) free(r->cols[i]);
        free(r->cols);
    }
    free(r->offs);
    free(r->lens);
    free(r->nulls);
    buf_free(&r->arena);
    memset(r, 0, sizeof(*r));
}

const char *my_text(const MyResult *r, int row, int col) {
    if (!r || row < 0 || row >= r->nrow || col < 0 || col >= r->ncol) return NULL;
    int i = row * r->ncol + col;
    if (r->nulls[i] || !r->arena.p) return NULL;
    return (const char *)r->arena.p + r->offs[i];
}

int my_len(const MyResult *r, int row, int col) {
    if (!r || row < 0 || row >= r->nrow || col < 0 || col >= r->ncol) return 0;
    return r->lens[row * r->ncol + col];
}

u64 my_u64(const MyResult *r, int row, int col, u64 def) {
    const char *s = my_text(r, row, col);
    if (!s) return def;
    u64 v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (u64)(*s - '0');
    return v;
}

i64 my_i64(const MyResult *r, int row, int col, i64 def) {
    const char *s = my_text(r, row, col);
    if (!s) return def;
    int neg = *s == '-';
    if (neg) s++;
    i64 v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

int my_col(const MyResult *r, const char *name) {
    for (int i = 0; i < r->ncol; i++)
        if (r->cols[i] && lstrcmpiA(r->cols[i], name) == 0) return i;
    return -1;
}

// ---------------- 编解码 ----------------
static void put_lenenc(Buf *b, u64 v) {
    if (v < 251) buf_putc(b, (u8)v);
    else if (v < 65536) { buf_putc(b, 0xFC); buf_put16(b, (u16)v); }
    else if (v < 16777216) {
        buf_putc(b, 0xFD);
        buf_putc(b, (u8)v);
        buf_putc(b, (u8)(v >> 8));
        buf_putc(b, (u8)(v >> 16));
    } else {
        buf_putc(b, 0xFE);
        for (int i = 0; i < 8; i++) buf_putc(b, (u8)(v >> (8 * i)));
    }
}

static int get_lenenc(const u8 *p, size_t n, size_t *off, u64 *v) {
    if (*off >= n) return 0;
    u8 b = p[(*off)++];
    if (b < 251) { *v = b; return 1; }
    if (b == 0xFB) { *v = UINT64_MAX; return 1; }        // NULL
    if (b == 0xFC) { if (*off + 2 > n) return 0; *v = (u64)p[*off] | (u64)p[*off + 1] << 8; *off += 2; return 1; }
    if (b == 0xFD) {
        if (*off + 3 > n) return 0;
        *v = (u64)p[*off] | (u64)p[*off + 1] << 8 | (u64)p[*off + 2] << 16;
        *off += 3;
        return 1;
    }
    if (b == 0xFE) {
        if (*off + 8 > n) return 0;
        *v = 0;
        for (int i = 0; i < 8; i++) *v |= (u64)p[*off + i] << (8 * i);
        *off += 8;
        return 1;
    }
    return 0;
}

static void native_scramble(const char *pwd, const u8 nonce[20], u8 out[20]) {
    u8 s1[20], s2[20], s3[20], buf[40];
    hash_sha1(pwd, strlen(pwd), s1);
    hash_sha1(s1, 20, s2);
    memcpy(buf, nonce, 20);
    memcpy(buf + 20, s2, 20);
    hash_sha1(buf, 40, s3);
    for (int i = 0; i < 20; i++) out[i] = (u8)(s3[i] ^ s1[i]);
}

char *mysql_escape(const char *s) {
    size_t n = strlen(s);
    char *o = (char *)xalloc(n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case 0: o[k++] = '\\'; o[k++] = '0'; break;
            case '\n': o[k++] = '\\'; o[k++] = 'n'; break;
            case '\r': o[k++] = '\\'; o[k++] = 'r'; break;
            case '\\': o[k++] = '\\'; o[k++] = '\\'; break;
            case '\'': o[k++] = '\\'; o[k++] = '\''; break;
            case '"': o[k++] = '\\'; o[k++] = '"'; break;
            case 0x1a: o[k++] = '\\'; o[k++] = 'Z'; break;
            default: o[k++] = s[i];
        }
    }
    o[k] = 0;
    return o;
}

// ---------------- 网络 ----------------
static SOCKET resolve(const char *host, int port, int *ok) {
    *ok = 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, host, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return INVALID_SOCKET;
        addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) { closesocket(s); return INVALID_SOCKET; }
    *ok = 1;
    return s;
}

int mysql_probe(const char *host, int port, int timeout_ms) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (InetPtonA(AF_INET, host, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
        addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int ok = 0;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        ok = 1;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(0, NULL, &wf, &ef, &tv);
        ok = r > 0 && FD_ISSET(s, &wf) && !FD_ISSET(s, &ef);
    }
    closesocket(s);
    return ok;
}

static int send_all(MySql *m, const void *d, size_t n) {
    const u8 *p = (const u8 *)d;
    size_t sent = 0;
    while (sent < n) {
        int r = send((SOCKET)m->sock, (const char *)p + sent, (int)(n - sent), 0);
        if (r <= 0) return 0;
        sent += (size_t)r;
    }
    return 1;
}

// 收包缓冲：保证 buf 里至少有 need 字节可读
static int buffered(MySql *m, size_t need) {
    if (m->buf_n - m->buf_off >= need) return 1;
    if (!m->buf) {
        m->buf_cap = 64 * 1024;
        m->buf = (u8 *)xalloc(m->buf_cap);
        m->buf_off = m->buf_n = 0;
    }
    if (m->buf_off > 0) {                       // 把残包挪到头部，腾出空间
        memmove(m->buf, m->buf + m->buf_off, m->buf_n - m->buf_off);
        m->buf_n -= m->buf_off;
        m->buf_off = 0;
    }
    if (need > m->buf_cap) {
        m->buf_cap = need + 64 * 1024;
        m->buf = (u8 *)xrealloc(m->buf, m->buf_cap);
    }
    while (m->buf_n < need) {
        int r = recv((SOCKET)m->sock, (char *)m->buf + m->buf_n, (int)(m->buf_cap - m->buf_n), 0);
        if (r <= 0) return 0;
        m->buf_n += (size_t)r;
    }
    return 1;
}

static int take(MySql *m, void *dst, size_t n) {
    if (!buffered(m, n)) return 0;
    if (dst) memcpy(dst, m->buf + m->buf_off, n);
    m->buf_off += n;
    return 1;
}

// 包头与载荷合成一次 send：两次 send 在 Nagle 下会和延迟 ACK 撞出 ~40ms 停顿
static int send_packet(MySql *m, const void *data, size_t n) {
    const u8 *p = (const u8 *)data;
    do {
        size_t chunk = n > 0xFFFFFF ? 0xFFFFFF : n;
        Buf one = {0};
        buf_reserve(&one, chunk + 4);
        buf_putc(&one, (u8)chunk);
        buf_putc(&one, (u8)(chunk >> 8));
        buf_putc(&one, (u8)(chunk >> 16));
        buf_putc(&one, (u8)(m->seq++));
        if (chunk) buf_put(&one, p, chunk);
        int ok = send_all(m, one.p, one.n);
        buf_free(&one);
        if (!ok) return 0;
        p += chunk;
        n -= chunk;
    } while (n > 0);
    return 1;
}

static int recv_packet(MySql *m, Buf *out) {
    buf_clear(out);
    u8 hdr[4];
    if (!take(m, hdr, 4)) return 0;
    size_t len = (size_t)hdr[0] | (size_t)hdr[1] << 8 | (size_t)hdr[2] << 16;
    m->seq = (u32)hdr[3] + 1;
    buf_reserve(out, len + 1);
    if (len && !take(m, out->p, len)) return 0;
    out->n = len;
    out->p[len] = 0;
    return 1;
}

void mysql_close(MySql *m) {
    if (m->sock != (uintptr_t)-1) closesocket((SOCKET)m->sock);
    m->sock = (uintptr_t)-1;
    m->seq = 0;
    free(m->buf);
    m->buf = NULL;
    m->buf_off = m->buf_n = m->buf_cap = 0;
}

static void die(MySql *m, const char *msg) {
    snprintf(m->err, ERRBUF, "%s", msg);
    mysql_close(m);
}

static void err_packet(MySql *m, MyResult *r, const u8 *p, size_t n) {
    u16 code = n >= 3 ? rd16(p + 1) : 0;
    size_t e = 3;
    while (e < n && p[e]) e++;
    Str s = {0};
    decode_auto((const char *)p + 3, (int)(e - 3), &s);
    snprintf(m->err, ERRBUF, "MySQL %u: %s", code, s.p ? s.p : "");
    str_free(&s);
    (void)r;
}

static int read_ok(MySql *m, Buf *pkt, MyResult *r) {
    size_t off = 1;
    u64 aff = 0, id = 0;
    get_lenenc(pkt->p, pkt->n, &off, &aff);
    if (off < pkt->n) get_lenenc(pkt->p, pkt->n, &off, &id);
    if (r) { r->affected = aff; r->insert_id = id; }
    return 1;
}

static int handshake(MySql *m, const char *user, const char *pwd, const char *db, int charset) {
    Buf pkt = {0};
    int ret = 0;
    if (!recv_packet(m, &pkt) || pkt.n == 0) { die(m, "握手包读取失败"); goto out; }
    if (pkt.p[0] == 0xFF) { die(m, "服务器拒绝连接"); goto out; }
    if (pkt.p[0] != 10) {
        snprintf(m->err, ERRBUF, "不支持的协议版本 %d", pkt.p[0]);
        goto out;
    }
    size_t off = 1;
    while (off < pkt.n && pkt.p[off]) off++;
    off++;                              // 跳过 \0
    if (off + 13 > pkt.n) { die(m, "握手包长度异常"); goto out; }
    off += 4;                           // connection id
    u8 nonce[20];
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, pkt.p + off, 8);
    off += 8 + 1;                       // nonce 前段 + filler

    u32 caps = 0;
    if (off + 2 <= pkt.n) { caps = rd16(pkt.p + off); off += 2; }
    if (off < pkt.n) {
        off += 1;                       // charset
        off += 2;                       // status
        if (off + 2 <= pkt.n) { caps |= (u32)rd16(pkt.p + off) << 16; off += 2; }
        u8 auth_len = off < pkt.n ? pkt.p[off] : 0;
        off += 1 + 10;                  // auth 长度 + 保留字节
        size_t part2 = auth_len > 8 ? (size_t)(auth_len - 8) : 12;
        if (part2 > 12) part2 = 12;
        if (off + part2 > pkt.n) part2 = pkt.n > off ? pkt.n - off : 0;
        memcpy(nonce + 8, pkt.p + off, part2);
    }
    if (!(caps & CAP_PROTOCOL_41)) { die(m, "服务器不支持 4.1 协议"); goto out; }

    u32 want = CAP_LONG_PASSWORD | CAP_LONG_FLAG | CAP_PROTOCOL_41 | CAP_TRANSACTIONS |
               CAP_SECURE_CONNECTION | CAP_PLUGIN_AUTH;
    if (db && *db) want |= CAP_CONNECT_WITH_DB;
    u32 use = want & (caps | CAP_PROTOCOL_41);

    u8 auth[20];
    native_scramble(pwd, nonce, auth);

    Buf b = {0};
    buf_put32(&b, use);
    buf_put32(&b, 16 * 1024 * 1024);
    buf_putc(&b, (u8)(charset & 0xff));
    for (int i = 0; i < 23; i++) buf_putc(&b, 0);
    buf_put(&b, user, strlen(user) + 1);
    buf_putc(&b, 20);
    buf_put(&b, auth, 20);
    if (use & CAP_CONNECT_WITH_DB) buf_put(&b, db, strlen(db) + 1);
    buf_put(&b, "mysql_native_password", 22);

    if (!send_packet(m, b.p, b.n)) { buf_free(&b); die(m, "发送认证包失败"); goto out; }
    buf_free(&b);

    if (!recv_packet(m, &pkt) || pkt.n == 0) { die(m, "认证响应读取失败"); goto out; }
    if (pkt.p[0] == 0x00) ret = 1;
    else if (pkt.p[0] == 0xFF) err_packet(m, NULL, pkt.p, pkt.n);
    else if (pkt.p[0] == 0xFE) die(m, "服务器要求切换认证插件，仅支持 mysql_native_password");
    else snprintf(m->err, ERRBUF, "未知认证响应 0x%02X", pkt.p[0]);
out:
    buf_free(&pkt);
    return ret;
}

int mysql_open(MySql *m, const char *host, int port, const char *user, const char *pwd,
               const char *db, int charset, int probe_ms, int handshake_ms) {
    memset(m, 0, sizeof(*m));
    m->sock = (uintptr_t)-1;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    if (!mysql_probe(host, port, probe_ms)) { snprintf(m->err, ERRBUF, "主机不可达或端口未开放"); return 0; }

    int ok = 0;
    SOCKET s = resolve(host, port, &ok);
    if (!ok || s == INVALID_SOCKET) { snprintf(m->err, ERRBUF, "连接失败"); return 0; }
    DWORD tv = (DWORD)handshake_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));
    // 命令/结果都是「一问一答」的小包，Nagle 等合并只会白等 40ms
    {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
    }
    m->sock = (uintptr_t)s;
    m->seq = 0;

    if (!handshake(m, user, pwd, db ? db : "", charset)) {
        if (mysql_alive(m)) mysql_close(m);
        return 0;
    }
    tv = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
    return 1;
}

// 追加一行
static void add_row(MyResult *r, Buf *pkt) {
    row_reserve(r, r->nrow + 1);
    size_t off = 0;
    int base = r->nrow * r->ncol;
    for (int i = 0; i < r->ncol; i++) {
        u64 len = 0;
        int idx = base + i;
        r->nulls[idx] = 1;
        r->lens[idx] = 0;
        r->offs[idx] = -1;
        if (!get_lenenc(pkt->p, pkt->n, &off, &len)) continue;
        if (len == UINT64_MAX) continue;
        if (off + len > pkt->n) len = pkt->n - off;
        r->offs[idx] = (int)r->arena.n;
        buf_put(&r->arena, pkt->p + off, (size_t)len);
        buf_putc(&r->arena, 0);
        r->lens[idx] = (int)len;
        r->nulls[idx] = 0;
        off += (size_t)len;
    }
    r->nrow++;
}

int mysql_query(MySql *m, const char *sql, MyResult *r) {
    MyResult dummy;
    if (!r) { memset(&dummy, 0, sizeof(dummy)); r = &dummy; }
    else memset(r, 0, sizeof(*r));

    if (!mysql_alive(m)) { snprintf(m->err, ERRBUF, "未连接"); goto fail; }
    m->seq = 0;                        // 每条命令序号从 0 开始（否则 1156）

    Buf req = {0};
    buf_putc(&req, 0x03);
    buf_put(&req, sql, strlen(sql));
    int sent = send_packet(m, req.p, req.n);
    buf_free(&req);
    if (!sent) { die(m, "发送查询失败"); goto fail; }

    Buf pkt = {0};
    int ret = 0;
    if (!recv_packet(m, &pkt) || pkt.n == 0) { die(m, "读取查询结果失败"); goto done; }

    if (pkt.p[0] == 0x00) { ret = read_ok(m, &pkt, r); goto done; }
    if (pkt.p[0] == 0xFF) { err_packet(m, r, pkt.p, pkt.n); goto done; }

    {
        size_t off = 0;
        u64 ncol = 0;
        if (!get_lenenc(pkt.p, pkt.n, &off, &ncol) || ncol == 0) { die(m, "列数解析失败"); goto done; }
        r->ncol = (int)ncol;
        r->cols = (char **)xalloc(sizeof(char *) * (size_t)ncol);
        memset(r->cols, 0, sizeof(char *) * (size_t)ncol);
        for (int i = 0; i < r->ncol; i++) {
            if (!recv_packet(m, &pkt)) { die(m, "读取列定义失败"); goto done; }
            size_t o = 0;
            for (int k = 0; k < 4; k++) {
                u64 l = 0;
                if (!get_lenenc(pkt.p, pkt.n, &o, &l)) break;
                if (l != UINT64_MAX) o += (size_t)l;
            }
            u64 nameLen = 0;
            if (get_lenenc(pkt.p, pkt.n, &o, &nameLen) && nameLen != UINT64_MAX && o + nameLen <= pkt.n) {
                r->cols[i] = (char *)xalloc((size_t)nameLen + 1);
                memcpy(r->cols[i], pkt.p + o, (size_t)nameLen);
                r->cols[i][nameLen] = 0;
            } else {
                r->cols[i] = str_dup("");
            }
        }
        if (!recv_packet(m, &pkt)) { die(m, "读取列结束包失败"); goto done; }
        if (!(pkt.n >= 1 && pkt.p[0] == 0xFE && pkt.n < 9)) {
            if (pkt.p[0] == 0xFF) { err_packet(m, r, pkt.p, pkt.n); goto done; }
            add_row(r, &pkt);            // 服务器省略 EOF
        }
        for (;;) {
            if (!recv_packet(m, &pkt)) { die(m, "读取数据行失败"); goto done; }
            if (pkt.n >= 1 && pkt.p[0] == 0xFE && pkt.n < 9) break;
            if (pkt.n >= 1 && pkt.p[0] == 0xFF) { err_packet(m, r, pkt.p, pkt.n); goto done; }
            add_row(r, &pkt);
        }
        ret = 1;
    }
done:
    buf_free(&pkt);
    return ret;

fail:
    return 0;
}
