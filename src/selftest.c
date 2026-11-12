// selftest：不依赖窗口和数据库的内部自检，给 CI 与排障用。
//   DNF_GM.exe --selftest
// 覆盖编码、摘要、压缩、物品 blob、PVF 解密这些纯函数——它们出错时
// 症状往往离现场很远（乱码、悄悄截断），单独钉住比事后查便宜。
#include "app.h"
#include "item.h"
#include "zlib.h"

static int s_fail;
static Str s_log;      // 同时写文件与 stdout：GUI 子系统下 stdout 未必可见

static void ok(const char *name, int cond) {
    if (!cond) s_fail++;
    str_catf(&s_log, "%s %s\n", cond ? "OK  " : "FAIL", name);
}

static void test_utf(void) {
    const char *src = "中文abc";
    WCHAR w[64];
    char back[128];
    int n = u16_from_u8(src, -1, w, COUNT_OF(w));
    ok("utf16 转换写入结尾", n > 0);
    u8_from_u16(w, -1, back, sizeof(back));
    ok("utf8/utf16 往返一致", strcmp(back, src) == 0);

    const u8 bad[] = {0xE4, 0xB8};                 // 截断的三字节序列
    ok("utf8_valid 拒绝截断序列", !utf8_valid(bad, 2));
    ok("utf8_valid 接受合法序列", utf8_valid((const u8 *)src, (int)strlen(src)));
    const u8 invalid[][4] = {
        {0xC0, 0x80}, {0xE0, 0x80, 0x80}, {0xED, 0xA0, 0x80},
        {0xF0, 0x80, 0x80, 0x80}, {0xF4, 0x90, 0x80, 0x80}, {0xF5, 0x80, 0x80, 0x80}
    };
    const int lengths[] = {2, 3, 3, 4, 4, 4};
    int rejected = 1;
    for (int i = 0; i < COUNT_OF(lengths); i++)
        if (utf8_valid(invalid[i], lengths[i])) rejected = 0;
    ok("utf8_valid 拒绝过长编码、代理项和越界码点", rejected);
    const u8 edges[] = {0xC2,0x80,0xE0,0xA0,0x80,0xED,0x9F,0xBF,
                        0xF0,0x90,0x80,0x80,0xF4,0x8F,0xBF,0xBF};
    ok("utf8_valid 接受合法边界", utf8_valid(edges, sizeof(edges)));
    WCHAR tinyw[2] = {7, 9};
    char tiny[2] = {7, 9};
    ok("编码转换零容量不写入", !u16_from_u8(src, -1, tinyw, 0) &&
       !u8_from_u16(w, -1, tiny, 0) && tinyw[0] == 7 && tiny[0] == 7);
    ok("编码转换单字符容量不越界", !u16_from_u8(src, -1, tinyw, 1) &&
       !u8_from_u16(w, -1, tiny, 1) && tinyw[0] == 0 && tiny[0] == 0 &&
       tinyw[1] == 9 && tiny[1] == 9);
    ok("编码转换拒绝空指针", !u16_from_u8(src, -1, NULL, 10) &&
       !u8_from_u16(w, -1, NULL, 10));
}

static void test_hash(void) {
    char md5[33];
    hash_md5("abc", 3, md5);
    ok("md5(abc)", strcmp(md5, "900150983cd24fb0d6963f7d28e17f72") == 0);

    u8 sha[20];
    char hex[41];
    hash_sha1("abc", 3, sha);
    for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", sha[i]);
    ok("sha1(abc)", strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
}

static void test_zlib(void) {
    const char text[] = "dnf-dm-c selftest payload 0123456789";
    size_t n = sizeof(text) - 1;
    Buf z = {0}, back = {0};
    ok("zlib_pack 成功", zlib_pack(text, n, &z));
    ok("zlib_unpack 成功", zlib_unpack(z.p, z.n, &back, 0));
    ok("解压内容一致", back.n == n && memcmp(back.p, text, n) == 0);

    // 改一个字节：adler32 必须发现
    z.p[z.n / 2] ^= 0xFF;
    Buf bad = {0};
    ok("坏数据被校验和拦下", !zlib_unpack(z.p, z.n, &bad, 0));
    buf_free(&bad);
    buf_free(&back);
    buf_free(&z);

    // Python zlib 生成的固定 / 动态 Huffman 流，独立于本项目的 stored 编码器。
    const u8 fixed[] = "\x78\x01\xcb\x48\xcd\xc9\xc9\x57\x48\xcb\xac\x48\x4d\x51\xf0\x28"
                       "\x4d\x4b\xcb\x4d\xcc\x53\x54\xc8\xa0\x4c\x10\x00\xff\x74\x1d\xa9";
    const char expected[] = "hello fixed Huffman! hello fixed Huffman! "
                            "hello fixed Huffman! hello fixed Huffman! ";
    ok("解压固定 Huffman 流", zlib_unpack(fixed, sizeof(fixed)-1, &back, 0) &&
       back.n == sizeof(expected)-1 && !memcmp(back.p, expected, sizeof(expected)-1));
    buf_free(&back);
    const u8 dynamic[] =
        "\x78\x9c\xb5\xc1\x85\x01\xc2\x40\x10\x04\xc0\x56\x82\x3b\x04\x77"
        "\x0b\xee\xee\xd6\x7f\x17\x94\xf0\x27\x7b\x33\x9e\xe7\x10\x72\x0a"
        "\xbb\x45\x08\xa2\x14\x31\x92\x38\x4d\x82\x28\x49\x95\x22\x4b\xd3"
        "\x65\x18\xb2\x1c\x39\x96\x3c\x4f\x81\xc9\xe7\x2a\xb2\x95\xf8\xca"
        "\x02\x15\x89\xaa\x48\x4d\xa6\x2e\xd4\x90\x6a\x8a\xb5\xe4\xda\x0a"
        "\x1d\x8d\xae\x4a\x4f\xa7\xaf\x34\xd0\x1a\xaa\x05\x7a\x23\x80\x31"
        "\xc2\x04\x62\x8a\x31\x03\x99\xa3\x2c\x60\x96\x38\x2b\xa0\x35\xd2"
        "\x06\x6a\x8b\xb5\x03\xdb\xa3\x1d\xe0\x8e\x78\x27\x03\x67\x0b\x17"
        "\x13\x57\x1b\x37\x23\x77\x2b\x0f\x33\x4f\x3b\x2f\x43\x6f\x4b\x1f"
        "\x53\x5f\x5b\x3f\x63\x7f\x28\xb3\x55\x7d";
    Buf plain = {0};
    for (int i = 32; i < 96; i++)
        for (int j = 0; j <= i; j++) buf_putc(&plain, (u8)i);
    ok("解压动态 Huffman 流", zlib_unpack(dynamic, sizeof(dynamic)-1, &back, 0) &&
       back.n == plain.n && !memcmp(back.p, plain.p, plain.n));
    buf_free(&back);
    ok("解压输出上限有效", !zlib_unpack(dynamic, sizeof(dynamic)-1, &back, 100));
    buf_free(&back);
    const u8 dictionary[] = {0x78,0x20,0,0,0,1,3,0,0,0,0,1};
    ok("拒绝不支持的预置字典", !zlib_unpack(dictionary, sizeof(dictionary), &back, 0));
    buf_free(&back);
    buf_free(&plain);
}

static void test_item(void) {
    Blob b = {0};
    b.n = 3;
    b.items = (Item *)xalloc(sizeof(Item) * 3);
    memset(b.items, 0, sizeof(Item) * 3);
    item_set_type(&b.items[0], IT_STACK);
    item_set_id(&b.items[0], 1001);
    item_set_num(&b.items[0], 999);
    item_set_type(&b.items[1], IT_EQUIP);
    item_set_id(&b.items[1], 2002);
    item_set_num(&b.items[1], 12345);
    ok("堆叠物数量按小端", item_num(&b.items[0]) == 999);
    ok("装备数量按大端", item_num(&b.items[1]) == 12345);

    Buf raw = {0};
    ok("blob_pack 成功", blob_pack(&b, &raw));
    Blob out = {0};
    Err err = {0};
    ok("blob_unpack 成功", blob_unpack(raw.p, raw.n, &out, err));
    ok("解包格数一致", out.n == 3);
    ok("解包内容一致",
       item_type(&out.items[0]) == IT_STACK && item_id(&out.items[0]) == 1001 &&
       item_num(&out.items[1]) == 12345);
    buf_free(&raw);
    blob_free(&out);
    blob_free(&b);
}

static void test_pvf(void) {
    u8 buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (u8)(i * 7 + 3);
    u8 orig[32];
    memcpy(orig, buf, sizeof(buf));
    pvf_decrypt(buf, sizeof(buf), 0x12345678);
    ok("pvf_decrypt 真的改了内容", memcmp(buf, orig, sizeof(buf)) != 0);
    pvf_encrypt(buf, sizeof(buf), 0x12345678);
    ok("pvf_encrypt 是 decrypt 的逆变换", memcmp(buf, orig, sizeof(buf)) == 0);
}

int selftest_run(void) {
    // GUI 子系统没有控制台：挂到父进程的控制台上，输出才能被 CI/脚本看到
    AttachConsole(ATTACH_PARENT_PROCESS);
    (void)freopen("CONOUT$", "w", stdout);
    str_cat(&s_log, "dnf-dm-c selftest\n");
    test_utf();
    test_hash();
    test_zlib();
    test_item();
    test_pvf();
    str_catf(&s_log, "\n%s（%d 项失败）\n", s_fail ? "失败" : "全部通过", s_fail);
    char dir[MAX_PATH], path[MAX_PATH];
    app_config_dir(dir, sizeof(dir));
    dir_make(dir);
    path_join(path, sizeof(path), dir, "selftest.txt");
    file_write(path, s_log.p, (size_t)s_log.n);     // 文件是给 CI / 排障看的
    fputs(str_c(&s_log), stdout);
    fflush(stdout);
    str_free(&s_log);
    return s_fail ? 1 : 0;
}
