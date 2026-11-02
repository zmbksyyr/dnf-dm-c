// mysql：MySQL 4.1+ 文本协议客户端（握手 / mysql_native_password / 查询 / 结果集）。
// 只做本工具需要的子集：无预处理、无压缩、无 SSL。
#pragma once
#include "util.h"

typedef struct {
    char **cols;
    int    ncol;

    int  *offs;        // nrow * ncol，值在 arena 内的字节偏移（arena 会扩容，不能存指针）
    int  *lens;
    u8   *nulls;
    int   nrow, cap;

    Buf   arena;
    u64   affected, insert_id;
} MyResult;

typedef struct {
    uintptr_t sock;
    u32       seq;
    char      err[ERRBUF];

    // 收包用的读缓冲：一次 recv 尽量多收，避免每个包头/包体各来一次系统调用。
    // buf_off 是已消费位置，buf_n 是有效末尾；两者相等表示空。
    u8    *buf;
    size_t buf_off, buf_n, buf_cap;
} MySql;

int  mysql_probe(const char *host, int port, int timeout_ms);
int  mysql_open(MySql *m, const char *host, int port, const char *user, const char *pwd,
                const char *db, int charset, int probe_ms, int handshake_ms);
void mysql_close(MySql *m);
static inline int mysql_alive(const MySql *m) { return m->sock != (uintptr_t)-1; }

int  mysql_query(MySql *m, const char *sql, MyResult *r);
void my_free(MyResult *r);

const char *my_text(const MyResult *r, int row, int col);   // NULL 表示越界/为 NULL
int         my_len(const MyResult *r, int row, int col);
u64         my_u64(const MyResult *r, int row, int col, u64 def);
i64         my_i64(const MyResult *r, int row, int col, i64 def);
char *mysql_escape(const char *s);   // malloc，调用方 free
