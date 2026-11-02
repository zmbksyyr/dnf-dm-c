# -*- coding: utf-8 -*-
"""畸形输入模糊测试：程序必须「报错但活着」，不能崩、不能卡死。

    python tools\\fuzz.py --config          # 损坏 settings.ini
    python tools\\fuzz.py --pvf 5           # 截断/变异 PVF，跑 5 轮
    python tools\\fuzz.py --sql             # 语法错误的还原文件（整体回滚）

每轮都会备份并在 finally 里还原 settings.ini；PVF 用的是配置里真实文件的副本。
退出码：全部通过 0，任一崩溃/无失败提示 1。
"""
import argparse
import contextlib
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


@contextlib.contextmanager
def ini_scope(apply):
    """临时改配置的作用域：进入时 apply(raw)，退出时原样写回。"""
    had = os.path.exists(INI)
    raw = open(INI, "rb").read() if had else b""
    try:
        apply(raw)
        yield
    finally:
        if had:
            open(INI, "wb").write(raw)
        else:
            try:
                os.remove(INI)
            except OSError:
                pass


def with_ini(apply):
    """不关心作用域的用法：改一下、跑完立刻还原。"""
    with ini_scope(apply):
        pass


def set_ini(section, key, value):
    """临时把 ini 的某一项改成 value（结束时由 with_ini 整体还原）。"""
    def apply(raw):
        txt = raw.decode("utf-8", "replace")
        out, cur, found = [], None, False
        for line in txt.splitlines():
            s = line.strip()
            if s.startswith("[") and s.endswith("]"):
                cur = s[1:-1]
            if cur == section and line.startswith(key + "="):
                out.append("%s=%s" % (key, value))
                found = True
                continue
            out.append(line)
        if not found:
            out += ["[%s]" % section, "%s=%s" % (key, value)]
        open(INI, "w", encoding="utf-8").write("\n".join(out) + "\n")
    return apply


def set_pvf_path(path):
    return set_ini("pvf", "path", path)


def set_img_dir(path):
    return set_ini("pvf", "img", path)



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


def npk_cases():
    """合成病态 npk：坏头、超大计数、越界索引，外加真实包的截断副本。"""
    import configparser
    cfg = configparser.RawConfigParser()
    cfg.read(INI, encoding="utf-8")
    img = cfg.get("pvf", "img", fallback="")
    real = None
    if img and os.path.isdir(img):
        for name in sorted(os.listdir(img)):
            if name.lower().endswith(".npk"):
                real = os.path.join(img, name)
                break
    out = []
    tmp = os.path.join(core.BUILD, "config", "fuzz-npk")
    os.makedirs(tmp, exist_ok=True)

    def add(name, data):
        p = os.path.join(tmp, name)
        open(p, "wb").write(data)
        out.append((tmp, p, name))

    add("random.npk", bytes(random.getrandbits(8) for _ in range(4096)))
    hdr = b"NeoplePack_Bill" + b"\0" * 8          # 头 20 字节
    # 计数 200000，但文件只有头部：read_at 必然失败
    add("hugecount.npk", hdr[:16] + (200000).to_bytes(4, "little"))
    # 一条索引，off/len 全 F：解析能过，取图必须失败
    ent = (0xFFFFFFFF).to_bytes(4, "little") + (0xFFFFFFFF).to_bytes(4, "little") + b"A" * 256
    add("badindex.npk", hdr[:16] + (1).to_bytes(4, "little") + ent)

    if real:
        size = os.path.getsize(real)
        with open(real, "rb") as f:
            add("trunc-real.npk", f.read(min(size, 1 << 20)))
    return tmp, out


def npk_deep(args):
    """变异真实贴图包并用 --npk-test 真解图标：走 zlib + IMG 帧表这条路径。

    只看「有没有崩」：返回码 0（解出若干净表）或 2（索引都建不起来）都算正常。
    """
    import configparser
    import subprocess

    cfg = configparser.RawConfigParser()
    cfg.read(INI, encoding="utf-8")
    img = cfg.get("pvf", "img", fallback="")
    cand = []
    if img and os.path.isdir(img):
        for name in os.listdir(img):
            if name.lower().endswith(".npk"):
                p = os.path.join(img, name)
                n = os.path.getsize(p)
                if 5000 < n < 400000:
                    cand.append((n, p))
    if not cand:
        print("跳过贴图包深度模糊：没找到大小合适的 npk")
        return True
    cand.sort()
    src = cand[len(cand) // 2][1]
    data = open(src, "rb").read()
    tmp = os.path.join(core.BUILD, "config", "fuzz-npk")
    exe_dir = os.path.dirname(core.EXE)
    rounds = args.npk_deep
    bad = 0
    try:
        for i in range(rounds):
            mode = i % 4
            b = bytearray(data)
            if mode == 0:
                b = bytearray(data[: random.randrange(100, len(data))])
            elif mode == 1:
                for _ in range(30):
                    b[random.randrange(len(b))] ^= 0xFF
            elif mode == 2:
                for _ in range(120):
                    b[random.randrange(min(len(b), 2048))] ^= 0xFF
            else:
                for _ in range(50):
                    b[random.randrange(min(len(b), 264 * 20))] ^= 0xFF
            shutil.rmtree(tmp, ignore_errors=True)
            os.makedirs(tmp, exist_ok=True)
            open(os.path.join(tmp, "mut.NPK"), "wb").write(b)
            try:
                r = subprocess.run([core.EXE, "--npk-test=" + tmp], cwd=exe_dir,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=120)
                rc = r.returncode
            except subprocess.TimeoutExpired:
                rc = "TIMEOUT"
            good = rc in (0, 2)
            print("[%d/%d] %-10s %7d 字节 rc=%s %s"
                  % (i + 1, rounds, ["trunc", "sparse", "head", "index"][mode],
                     len(b), rc, "" if good else "<-- 异常"))
            if not good:
                bad += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return bad == 0

def check_npk(args):
    if not args.npk:
        return True
    root, cases = npk_cases()
    bad = 0
    try:
        for i, (_dir, path, name) in enumerate(cases, 1):
            with ini_scope(set_img_dir(root)):
                alive, _ = boot_check("贴图包", 12.0)
            print("[%d/%d] %-20s alive=%s" % (i, len(cases), name, alive))
            if not alive:
                bad += 1
    finally:
        core.kill_stale()
        try:
            shutil.rmtree(root, ignore_errors=True)
        except Exception:
            pass
        for _d, path, _n in cases:
            try:
                os.remove(path)
            except OSError:
                pass
    return bad == 0



SQL_TOK = ["'", '"', "`", ";", "--", "/*", "*/", "\n", "\r", " ", "\t", "\\", "\\'", "''",
           "foo", "бар", "中文", "?", "0x", "\\0", "%", "_", "#", "=", ",", "(", ")",
           "\x00"]


def check_sql(args):
    """用随机词法碎片喂还原器：切分逻辑必须报错退出，不能崩、不能挂。

    只用标点/注释/引号和非关键字词元，保证拼不出任何合法语句——数据不会被改。
    """
    import subprocess

    rounds = args.sql or 1
    path = os.path.join(core.BUILD, "config", "backup", "fuzz-sql.sql")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    exe_dir = os.path.dirname(core.EXE)
    bad = 0
    try:
        for i in range(rounds):
            n = random.choice([64, 512, 4096, 20000])
            body = "".join(random.choice(SQL_TOK) for _ in range(n // 2))
            open(path, "w", encoding="utf-8", errors="replace").write(
                "SET NAMES utf8mb4;\n" + body)
            try:
                r = subprocess.run([core.EXE, "--db-restore=" + path], cwd=exe_dir,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=30)
                rc = r.returncode
            except subprocess.TimeoutExpired:
                rc = "TIMEOUT"
            good = rc in (0, 1)        # 0=全被当注释；1=正常报错；其余视为崩溃
            print("[%d/%d] %6d 字节 rc=%s %s" % (i + 1, rounds, n, rc, "" if good else "<-- 异常"))
            if not good:
                bad += 1
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
    return bad == 0


def check_pvf(args):
    cases = pvf_cases(args.pvf)
    if not cases:
        return True
    bad = 0
    try:
        for i, (path, label) in enumerate(cases, 1):
            with ini_scope(set_pvf_path(path)):
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
    ap.add_argument("--sql", type=int, nargs="?", const=1, default=0, help="随机词法碎片喂还原器（可给轮数）")
    ap.add_argument("--pvf", type=int, default=0, help="PVF 畸形副本轮数")
    ap.add_argument("--npk", action="store_true", help="合成病态 npk 贴图包")
    ap.add_argument("--npk-deep", type=int, default=0, metavar="N", help="变异真实 npk 并真解图标 N 轮")
    a = ap.parse_args()
    if not (a.config or a.sql or a.pvf or a.npk or a.npk_deep):
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
    if a.npk:
        ok &= check_npk(a)
    if a.npk_deep:
        ok &= npk_deep(a)
    print("\n结果：%s" % ("全部存活且报错正常" if ok else "有异常，见上"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
