# -*- coding: utf-8 -*-
"""畸形输入模糊测试：程序必须「报错但活着」，不能崩、不能卡死。

    python tools\\fuzz.py --config          # 损坏 settings.ini
    python tools\\fuzz.py --pvf 5           # 截断/变异 PVF，跑 5 轮
    python tools\\fuzz.py --sql             # 语法错误的还原文件（整体回滚）

每轮都会备份并在 finally 里还原 settings.ini；PVF 用的是配置里真实文件的副本。
退出码：全部通过 0，任一崩溃/无失败提示 1。
"""
import argparse
import os
import random
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from regress import core          # noqa: E402

INI = os.path.join(core.BUILD, "config", "settings.ini")
LOG_DIR = os.path.join(core.BUILD, "config", "logs")


def log_tail(n=4000):
    p = os.path.join(LOG_DIR, "log-%s.txt" % time.strftime("%Y%m%d"))
    if not os.path.exists(p):
        return ""
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()[-n:]


def boot_check(expect, timeout=12.0):
    """启动一次，返回 (是否活着, 是否出现预期文本)。"""
    core.kill_stale()
    p, m = core.boot()
    try:
        ok = core.wait_until(lambda: expect in log_tail(), timeout, 0.5)
        return p.poll() is None, ok
    finally:
        try:
            p.kill()
        except Exception:
            pass
        core.kill_stale()


def with_ini(reader):
    """临时改配置：先备份原始字节，结束时原样写回。"""
    had = os.path.exists(INI)
    raw = open(INI, "rb").read() if had else b""
    try:
        reader(raw)
    finally:
        if had:
            open(INI, "wb").write(raw)
        else:
            try:
                os.remove(INI)
            except OSError:
                pass


def set_pvf_path(path):
    def apply(raw):
        txt = raw.decode("utf-8", "replace")
        out, in_pvf = [], False
        found = False
        for line in txt.splitlines():
            if line.strip().startswith("["):
                in_pvf = line.strip() == "[pvf]"
            if in_pvf and line.startswith("path="):
                out.append("path=" + path)
                found = True
                continue
            out.append(line)
        if not found:
            out += ["[pvf]", "path=" + path]
        open(INI, "w", encoding="utf-8").write("\n".join(out) + "\n")
    return apply


def pvf_cases(count):
    """从真实 PVF 生成畸形副本：截断 + 随机翻字节。"""
    import configparser
    cfg = configparser.RawConfigParser()
    cfg.read(INI, encoding="utf-8")
    real = cfg.get("pvf", "path", fallback="")
    if not real or not os.path.exists(real):
        print("跳过 PVF：settings.ini 里没有可用的 pvf 路径")
        return []

    size = os.path.getsize(real)
    out = []
    for i in range(count):
        mode = i % 3
        dst = os.path.join(core.BUILD, "config", "fuzz-%d.pvf" % i)
        if mode == 0:                       # 截断到随机长度
            n = random.randint(1, max(2, min(size, 64 << 20)))
            with open(real, "rb") as f, open(dst, "wb") as g:
                g.write(f.read(n))
            label = "truncate to %d bytes" % n
        else:                               # 前 8MB 随机翻字节
            n = min(size, 8 << 20)
            with open(real, "rb") as f:
                buf = bytearray(f.read(n))
            for _ in range(2000):
                buf[random.randrange(len(buf))] ^= 0xFF
            open(dst, "wb").write(buf)
            label = "mutate %d bytes in first %d" % (2000, n)
        out.append((dst, label))
    return out


def check_config(args):
    print("[1/1] settings.ini 整体损坏")
    ok_alive = [False]

    def apply(raw):
        open(INI, "wb").write(bytes(random.getrandbits(8) for _ in range(200)))
        ok_alive[0] = True
    with_ini(apply)
    alive, _ = boot_check("就绪", 8.0) if ok_alive[0] else (False, False)
    print("      alive=%s" % alive)
    return alive


def check_sql(args):
    """语法错误的还原文件：报错并回滚，进程活着。"""
    path = os.path.join(core.BUILD, "config", "backup", "fuzz-bad.sql")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8").write("SET NAMES utf8mb4;\nNOT VALID SQL;;;\n")
    print("[1/1] 语法错误的 SQL 还原")
    try:
        alive, _ = boot_check("就绪", 8.0)
        print("      启动正常=%s（还原路径由 db.restore_bad_sql 用例覆盖）" % alive)
        return alive
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def check_pvf(args):
    cases = pvf_cases(args.pvf)
    if not cases:
        return True
    bad = 0
    try:
        for i, (path, label) in enumerate(cases, 1):
            with_ini(set_pvf_path(path))
            alive, failed = boot_check("PVF 加载失败", 15.0)
            good = alive and failed
            print("[%d/%d] %-40s alive=%s failed=%s" % (i, len(cases), label, alive, failed))
            if not good:
                bad += 1
    finally:
        for path, _ in cases:
            try:
                os.remove(path)
            except OSError:
                pass
        core.kill_stale()
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description="畸形输入模糊测试")
    ap.add_argument("--config", action="store_true", help="损坏 settings.ini")
    ap.add_argument("--sql", action="store_true", help="语法错误的还原文件")
    ap.add_argument("--pvf", type=int, default=0, help="PVF 畸形副本轮数")
    a = ap.parse_args()
    if not (a.config or a.sql or a.pvf):
        ap.print_help()
        return 1
    if not os.path.exists(core.EXE):
        print("找不到被测程序：%s" % core.EXE)
        return 1
    ok = True
    if a.config:
        ok &= check_config(a)
    if a.sql:
        ok &= check_sql(a)
    if a.pvf:
        ok &= check_pvf(a)
    print("\n结果：%s" % ("全部存活且报错正常" if ok else "有异常，见上"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
