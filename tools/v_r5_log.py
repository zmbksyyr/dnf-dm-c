# -*- coding: utf-8 -*-
"""R5 / N-3 的配套脚本：v_log.py 那条「倒序一致：否」的**成因确认 + 上限感知判据**。

`tools/v_log.py` 判的是两件事：

    box[0]  == flines[-1]     ← 倒序本身（最新的在最顶）
    box[-1] == flines[0]      ← 「页面第一行是文件第一行」= 要求整份文件都显示

第二条在日志超过显示上限时**物理上不可能成立**：`src/ui_data.c:794` 写死了

    size_t start = b.n > 100000 ? b.n - 100000 : 0;

今天 log-20261101.txt 已 20 万字节 / 4300 多行，页面只剩一千多行。所以
「倒序一致：否」**不是倒序出错**——第一条一直成立——是判据里那条
「整份文件都在页面上」的要求撞上了上限。

顺带量出来两件小事：一是**切片单位是字节不是字符**，UTF-8 一个汉字 3 字节，
100000 字节在中文日志里只有 6 万多个字符、一千三百多行 —— 拿字符去切会算出
两倍多的行数（脚本里踩过一次，写在这里）；二是切点会落在一行中间，页面最后一行
因此是半截的一行（这次截出来的是「…克隆测试2（charac_no=9919）· 19 张表 / 38 行」）。
两条都属既有 Minor，不影响倒序本身。

这个脚本把它拆开验：

  1) 倒序本身：页面第一行 == 文件最后一行
  2) 上限：页面行数 == 文件末尾 100000 字节切出来的行数
  3) 页面最后一行 == 那 100000 字节切片的第一行（切点可能落在一个字中间，
     这一行本来就是半截的 —— 这也是上限带来的观感问题，不是倒序问题）
  4) 页面行数 < 文件行数（说明确实被截了）

建议（给下一轮，测试者不能改既有脚本）：把 v_log.py 的第二条判据改成
「页面最后一行 == 末尾 100000 字节切片的第一行」，并单独 print 一行
「显示上限 100000 字节」。**不要**去提高上限：上限是刻意的，日志页每次
act_ver 抖动都会重写整块文本，把 190KB 全塞进一个 EDIT 只会让每次刷新更重；
而且无论上限提到多少，日志迟早会再超，判据不感知上限就永远是个定时假红。

跑法：
    python tools/v_r5_log.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pages import u, EXE, BUILD, wait_main, switch, ctl, txt  # noqa: E402

PAGE_LOG = 11
L_TEXT = 1000 + 11 * 100 + 1        # 2101
CAP = 100000
LOG = os.path.join(BUILD, "config", "logs", time.strftime("log-%Y%m%d.txt"))

fails = []


def ok(tag, got, want):
    good = got == want
    print("%s %s（得到 %r，期望 %r）" % ("OK  " if good else "FAIL", tag, got, want))
    if not good:
        fails.append(tag)
    return good


def ok_true(tag, cond, detail=""):
    print("%s %s%s" % ("OK  " if cond else "FAIL", tag, (" | " + detail) if detail else ""))
    if not cond:
        fails.append(tag)
    return cond


def main():
    print("EXE =", EXE)
    p = None
    try:
        p = subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
        m = wait_main(p)
        u.PostMessageW(u.GetDlgItem(m, 104), 0x00F5, 0, 0)
        time.sleep(4.0)
        switch(m, PAGE_LOG)
        time.sleep(1.5)
        raw = open(LOG, encoding="utf-8", errors="replace").read()
        flines = [l for l in raw.replace("\r\n", "\n").split("\n") if l.strip()]
        box = [l for l in txt(ctl(m, L_TEXT), 1 << 20).replace("\r\n", "\n").split("\n") if l.strip()]
        print("文件：%d 字节 / %d 行    页面：%d 行 / 约 %d 字节"
              % (len(raw), len(flines), len(box), sum(len(l) + 2 for l in box)))
        ok_true("文件确实超过了 100000 字节上限", len(raw) > CAP,
                "%d > %d" % (len(raw), CAP))
        ok("1) 倒序本身成立（页面第一行 == 文件最后一行）", box[0][:40], flines[-1][:40])
        ok_true("2) 页面被截短了（行数少于文件）", len(box) < len(flines),
                "%d < %d" % (len(box), len(flines)))
        # 3) 按字节切 → 最后一行一定是「半行」。整行里找不到它，但它一定是某一行的尾段。
        #    注意：100000 是**字节**，UTF-8 一个汉字 3 字节，所以切片要按字节切，
        #    按字符切会切出两倍多的行（早先就是这么算错的）。
        braw = raw.encode("utf-8", "replace")
        tailbytes = braw[len(braw) - CAP:]
        tl = [l for l in tailbytes.decode("utf-8", "replace").replace("\r\n", "\n").split("\n")
              if l.strip()]
        # 文件在被测程序读完之后还会继续长（它自己还在往里写），所以只能断言
        # 「页面最后一行属于末尾 100000 字节这个窗口」，不能断言它正好是切片第一行。
        print("    末尾 100000 字节 = %d 行；页面 %d 行" % (len(tl), len(box)))
        print("    页面最后一行：", box[-1][:70])
        whole = set(flines)
        in_tail = [l for l in tl if l.endswith(box[-1])]
        # 切点是不是正好落在行边界上，取决于那 100000 字节落在哪 —— 两种都对，
        # 所以「是不是半行」只作观察，不作断言。
        print("    切点%s" % ("正好落在行边界上（最后一行是完整的一行）"
                             if box[-1] in whole else "落在一行中间（最后一行是半截的）"))
        ok_true("3) 它属于末尾 100000 字节那一截（切点就在这里）", bool(in_tail),
                in_tail[0][:70] if in_tail else "")
        ok_true("3) 页面行数不超过末尾切片的行数", len(box) <= len(tl),
                "%d <= %d" % (len(box), len(tl)))
        # 4) 页面文本量正好停在 100000 字节附近 —— 卡住的就是 ui_data.c 那道
        box_bytes = sum(len(l.encode("utf-8", "replace")) + 2 for l in box)
        ok_true("4) 页面文本 ≈ 100000 字节（正是 ui_data.c:794 那道上限）",
                90000 < box_bytes <= 100500, "%d 字节" % box_bytes)
    finally:
        try:
            if p and p.poll() is None:
                p.kill()
        except Exception:
            pass
    print("\n合计失败 %d 项" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
