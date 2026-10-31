# -*- coding: utf-8 -*-
"""
从三份 ini 里只取物品 ID，编码成 src/items_data.h（内置进程序，运行时不再需要 ini）。

为什么这么压：663 个 ID 排序后做差分，575 条的相邻差值只有 1，base128 varint
后总共 700 多字节，占 420KB 的 exe 不到 0.2%。名字一个都不存 —— 显示时按
id 去 PVF 索引里查，查不到的直接跳过（正好就是「PVF 里有的才加载」）。

用法：python tools/gen_items.py [ini 目录]
默认从 桌面/xx 读，输出到 src/items_data.h。
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (输出数组名, ini 文件名)
GROUPS = [
    ("kCommonMat", "材料.ini"),
    ("kCommonEquip", "装备.ini"),
    ("kCommonAvatar", "时装.ini"),
]


def load_ids(path):
    raw = open(path, "rb").read().decode("gbk", "replace")
    out = []
    for line in raw.splitlines()[1:]:          # 首行是 "-----" 占位
        line = line.strip()
        if "-----" not in line:
            continue
        try:
            out.append(int(line.split("-----", 1)[0].strip()))
        except ValueError:
            continue
    return sorted(set(out))


def enc_varint(v, out):
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def encode(ids):
    out = []
    prev = 0
    for i in ids:
        enc_varint(i - prev, out)
        prev = i
    return out


def c_array(name, data):
    lines = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + 16])
        lines.append("    " + chunk + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.expanduser("~"), "Desktop", "xx")

    body = []
    total = 0
    count = 0
    for arr, fname in GROUPS:
        p = os.path.join(src, fname)
        if not os.path.exists(p):
            print("跳过（没有这个文件）：", p)
            continue
        ids = load_ids(p)
        data = encode(ids)
        total += len(data)
        count += len(ids)
        body.append(c_array(arr, data))
        print("%-14s %3d 个 ID → %4d 字节" % (fname, len(ids), len(data)))

    if not body:
        print("没有可用的数据，未生成文件。")
        return 1

    text = """// 自动生成，请勿手改。
// 源：tools/gen_items.py（从三份 ini 里只取 ID，名字不进程序 —— 显示时按 id
// 去 PVF 索引里查，查不到就跳过，这正好就是「PVF 里有的才加载」）。
// 编码：ID 升序后做差分，再 base128 varint。相邻 ID 大多只差 1，所以极省。
// 解码见 ui_item.c 的 common_load()。
#pragma once

/* 共 %d 个 ID，编码后 %d 字节 */
""" % (count, total)
    text += "\n\n".join(body) + "\n"

    out = os.path.join(ROOT, "src", "items_data.h")
    open(out, "w", encoding="utf-8", newline="\n").write(text)
    print("已写出 %s（%d 个 ID，%d 字节）" % (out, count, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
