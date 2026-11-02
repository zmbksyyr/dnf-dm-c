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
