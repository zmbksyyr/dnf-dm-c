# -*- coding: utf-8 -*-
"""回归入口。

    python tools\\regress.py                 # 默认组（自还原、可复现）
    python tools\\regress.py --group manual   # 需要人工环境的组
    python tools\\regress.py --group all
    python tools\\regress.py --list           # 列用例与前置条件
    python tools\\regress.py --only gm.       # 按名字前缀过滤

退出码：全部通过 0，任一失败 1，跳过不算失败。
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from regress import core          # noqa: E402
from regress import cases          # noqa: E402  用例在此汇总注册

if __name__ == "__main__":
    sys.exit(core.main())
