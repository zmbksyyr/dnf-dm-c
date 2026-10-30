// item 实现。
#include "item.h"
#include "zlib.h"

const char *item_type_name(u8 t) {
    switch (t) {
        case IT_EQUIP: return "装备";
        case IT_STACK: return "消耗品";
        case IT_MATERIAL: return "材料";
        case IT_QUEST: return "任务材料";
        case IT_CREATURE: return "宠物";
        case IT_CREATURE_EQUIP: return "宠物装备";
        case IT_CREATURE_STACK: return "宠物消耗品";
        case IT_EXPERT: return "副职业";
        default: return "空槽位";
    }
}

int item_type_valid(u8 t) { return t <= 7 || t == IT_EXPERT; }

void item_hex(const Item *it, char *out, int cap) {
    static const char *hd = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; i < ITEM_SIZE && o + 4 < cap; i++) {
        if (i && i % 8 == 0) out[o++] = ' ';
        out[o++] = hd[it->raw[i] >> 4];
        out[o++] = hd[it->raw[i] & 15];
    }
    out[o] = 0;
}

void blob_free(Blob *b) {
    free(b->items);
    b->items = NULL;
    b->n = 0;
    b->ok = 0;
}

int blob_unpack(const void *raw, size_t n, Blob *out, Err err) {
    memset(out, 0, sizeof(*out));
    if (n < 6) { snprintf(err, ERRBUF, "blob 过短（%u 字节）", (unsigned)n); return 0; }
    memcpy(out->prefix, raw, 4);
    out->has_prefix = 1;

    Buf dst = {0};
    if (!zlib_unpack((const u8 *)raw + 4, n - 4, &dst)) {
        buf_free(&dst);
        snprintf(err, ERRBUF, "zlib 解压失败");
        return 0;
    }
    if (dst.n == 0 || dst.n % ITEM_SIZE) {
        snprintf(err, ERRBUF, "长度 %u 不是 61 的整数倍", (unsigned)dst.n);
        buf_free(&dst);
        return 0;
    }
    out->n = (int)(dst.n / ITEM_SIZE);
    out->items = (Item *)xalloc(dst.n);
    memcpy(out->items, dst.p, dst.n);
    buf_free(&dst);
    out->ok = 1;
    return 1;
}

int blob_pack(const Blob *b, Buf *out) {
    size_t n = (size_t)b->n * ITEM_SIZE;
    buf_clear(out);
    if (b->has_prefix) buf_put(out, b->prefix, 4);
    else { u8 z[4] = {0}; buf_put(out, z, 4); }
    Buf z2 = {0};
    if (!zlib_pack(b->items, n, &z2)) { buf_free(&z2); return 0; }
    buf_put(out, z2.p, z2.n);
    buf_free(&z2);
    return 1;
}

// 空槽不算错；类型/编号/数量非法，或名字表里查不到（客户端读不出来）算错
int item_bad(const Item *it, ItemHasName has, void *ctx, char *why, int cap) {
    u8 t = item_type(it);
    u32 id = item_id(it);
    if (t == IT_EMPTY && id == 0) return 0;
    if (t == IT_EMPTY) {
        snprintf(why, (size_t)cap, "类型为空但有编号");
        return 1;
    }
    if (!item_type_valid(t)) {
        snprintf(why, (size_t)cap, "类型 %u 非法", (unsigned)t);
        return 1;
    }
    if (id == 0) {
        snprintf(why, (size_t)cap, "编号为 0");
        return 1;
    }
    if ((t == IT_STACK || t == IT_MATERIAL) && item_num(it) == 0) {
        snprintf(why, (size_t)cap, "数量为 0");
        return 1;
    }
    if (has && !has(id, ctx)) {
        snprintf(why, (size_t)cap, "PVF 无此编号");
        return 1;
    }
    return 0;
}
