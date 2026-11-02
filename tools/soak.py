# -*- coding: utf-8 -*-
"""长时间随机操作压测：随机切页、点列表行、偶尔断开重连，盯三件事。

    python tools\\soak.py --sec 480

判据：
  1. 进程活着，且没有新的崩溃转储；
  2. GDI / USER 句柄在首轮建页后不随时间增长；
  3. 工作集内存稳定（允许 PVF/NPK 缓存涨一点）。

退出码 0 = 全部通过。
"""
import argparse
import ctypes
import glob
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from regress import core          # noqa: E402

u = core.u
k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi


class PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def sample(proc):
    h = k32.OpenProcess(0x1000, False, proc.pid)
    gdi = u.GetGuiResources(h, 0)
    usr = u.GetGuiResources(h, 1)
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), ctypes.sizeof(PMC))
    k32.CloseHandle(h)
    return gdi, usr, pmc.WorkingSetSize // (1024 * 1024)


def main():
    ap = argparse.ArgumentParser(description="随机操作压测")
    ap.add_argument("--sec", type=int, default=480, help="持续秒数")
    ap.add_argument("--seed", type=int, default=0, help="随机种子（0=每次不同）")
    a = ap.parse_args()
    if a.seed:
        random.seed(a.seed)

    crash_dir = os.path.join(core.BUILD, "config", "crash")
    before = set(glob.glob(os.path.join(crash_dir, "*.dmp")))
    core.kill_stale()
    p, m = core.boot()
    samples = []
    iters = 0
    try:
        lv = u.GetDlgItem(m, 113)
        if not core.wait_until(lambda: core.rows(lv) > 0, 25.0, 0.2):
            print("启动后没有读到角色列表")
            return 1
        core.click_at(lv, 40, 40, wait=1.5)
        t0 = time.time()
        while time.time() - t0 < a.sec:
            iters += 1
            core.switch(m, random.randrange(12), wait=random.uniform(0.15, 0.4))
            page = None
            for h in core.kids(m):
                if core.cls_of(h) == "DnfPage" and u.IsWindowVisible(h):
                    page = h
                    break
            if page:
                for w in core.kids(page):
                    if core.cls_of(w) == "SysListView32":
                        core.click_at(w, 40, 40, wait=0.1)
                        break
            if iters % 20 == 0:                       # 偶尔断开再连，覆盖会话边界
                core.press(u.GetDlgItem(m, 105), 0.3)
                time.sleep(1.0)
                core.press(u.GetDlgItem(m, 104), 1.0)
                core.click_at(lv, 40, 40, wait=1.0)
            if iters % 5 == 0:
                samples.append(sample(p))
            if p.poll() is not None:
                print("第 %d 轮时进程退出，退出码 %s" % (iters, p.returncode))
                return 1
    finally:
        try:
            p.kill()
        except Exception:
            pass
        core.kill_stale()

    new_dumps = set(glob.glob(os.path.join(crash_dir, "*.dmp"))) - before
    print("轮次: %d  采样: %d" % (iters, len(samples)))
    ok = True
    if new_dumps:
        print("!! 出现新的崩溃转储: %s" % [os.path.basename(x) for x in new_dumps])
        ok = False
    if len(samples) >= 4:
        half = len(samples) // 2
        early = samples[:half]
        late = samples[half:]
        for idx, name in ((0, "GDI"), (1, "USER"), (2, "内存MB")):
            e = max(s[idx] for s in early)
            l = max(s[idx] for s in late)
            print("  %-6s 前半峰值=%-5d 后半峰值=%-5d" % (name, e, l))
            if l > e + 40:
                print("!! %s 后半段继续增长" % name)
                ok = False
        print("  内存首尾: %dMB → %dMB" % (samples[0][2], samples[-1][2]))
    print("结果：%s" % ("通过" if ok else "有异常"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
