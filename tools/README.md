# tools —— 本地编译与回归脚本

这里的两个东西都只服务于**开发机**，不参与发布：

- `build.bat` —— 本地编译，产物落到项目目录之外
- `v_*.py` —— 回归脚本，启动程序、模拟操作、断言结果

发布产物由 `.github/workflows/build.yml` 手动触发构建，与本目录无关。两边的编译参数（`-std=c11 -O2 -DNDEBUG -DUNICODE -D_UNICODE -static -s`）保持一致，改一处记得改另一处。

## 编译

```
tools\build.bat
```

双击或在任意目录下调用都可以——脚本用自身位置推项目根，不写死盘符。

**产物在项目目录之外**：

```
<项目根>\..\_build\DNF_GM_C\
├─ DNF_GM.exe      编译结果（静态链接，可直接拷走运行）
├─ res.obj         中间产物
├─ config\         程序运行时自建（settings.ini / 备份 / 日志 / 缓存）
└─ shots\          回归脚本截的图
```

仓库里因此不会出现 exe、obj、config、截图，`git status` 永远是干净的。

**zig 的查找顺序**：环境变量 `ZIG` → `PATH` 里的 `zig` → `C:\zig\zig.exe` → `%LOCALAPPDATA%\zig\zig.exe`。都没有就报错退出。

**两个可选环境变量**：

| 变量 | 作用 |
|---|---|
| `ZIG` | 指定 zig.exe 的完整路径 |
| `DNF_GM_BUILD` | 改输出目录（默认 `..\_build\DNF_GM_C`）；`pages.py` 会跟着用同一个值 |

程序正在运行时无法覆盖 exe，脚本会直接提示先关掉，而不是丢一句链接失败。

## 回归脚本

「回归」= 改完代码，回头验证原来能用的功能现在还能用。这些脚本不是单元测试，它们**真的启动程序、真的发消息点它**，然后读回控件文本、日志、控件矩形，或直连 MySQL 查库来断言。

```
python tools\v_gmwrite.py
```

跑之前先关掉已经在运行的 `DNF_GM.exe`（脚本会自己起一个，起完自己杀）。

**前置条件**：Python 3 + `pip install pillow pymysql`，以及一个能连上的测试库。数据库参数从 `<产物目录>\config\settings.ini` 读（程序自己写的那个），读不到就用代码里的默认值——改过库密码只需要改配置，脚本不用动。

| 脚本 | 验什么 |
|---|---|
| `v_gmwrite.py` | GM 页改金币 → 失焦 → 落库 → 改回原值 |
| `v_queue.py` | 金币/复活币/点券/等级四个字段连改，一个都不丢 |
| `v_stable.py` | 物品页选中与滚动扛过一次 reload；写库期间界面不把输入框的值顶回去 |
| `v_pollcard.py` | 外部改等级后：左栏卡片跟着变、GM 表单保持快照、物品页选中不丢 |
| `v_pageall.py` | 11 个页签逐个进入并切回，查有没有整块空白（控件没画出来） |
| `v_geom.py` | 量控件矩形，查越界、重叠、两栏底部是否齐平 |
| `v_tab.py` | Tab 键能否走到每个输入框，卡片内顺序是否正确 |
| `v_align.py` | 量 PVF 标签与页签「物品」的左边缘差几像素 |
| `v_selfcheck.py` | 临时把 `taiwan_cain_2nd.letter` 改名，验证启动自检能报出缺表（`finally` 里改回） |
| `v_norow.py` | 造「表里没有这条记录」的场景，验证报错而不是报成功 |

`pages.py` 是公共库：Win32 消息封装、截图、切页签、等主窗口、`db()` 连库。

## 几个约定

1. **会改库，但会复原**。`v_gmwrite` / `v_queue` / `v_stable` / `v_pollcard` 都记录原值并写回；`v_pollcard` 还会把等级改回去。跑完仍建议对一下 `money` / `coin` / `cera` / `lev`。
2. **`v_selfcheck` 会临时改表名**。它把 `letter` 改成 `letter_selfcheck_bak` 再改回来，中途程序会报缺表——这是预期结果。异常退出时表可能留在改名状态，手动改回即可。
3. **不要在 CI 里跑这些**。它们要真实 MySQL、要能创建窗口、而且会改业务数据，都跑在开发机上。
4. **控件 ID 是硬编码的**（`PAGE_ID(page, n) = 1000 + page*100 + n`）。界面改了就跟着改，所以脚本天然短命——只有经得起反复用、改界面也愿意同步维护的才留在这个目录。
