# tools

开发机用的两件事：本地编译、回归测试。发布产物由 `.github/workflows/build.yml` 生成，与本目录无关。

## 编译

```
tools\build.bat
```

产物落在项目目录之外：`<项目根>\..\_build\DNF_GM_C\`，可用 `DNF_GM_BUILD` 改。
zig 的查找顺序：环境变量 `ZIG` → `PATH` → `C:\zig\zig.exe` → `%LOCALAPPDATA%\zig\zig.exe`。

告警即错误，开关与 CI 完全一致；任一处新增告警，两边都会红。

## 回归

```
python tools\regress.py                  # 默认组
python tools\regress.py --group manual   # 需要人工环境的组
python tools\regress.py --group all
python tools\regress.py --list           # 用例清单与前置条件
python tools\regress.py --only gm.       # 名字前缀过滤
```

前置：Python 3 + `pip install pillow pymysql`；一个能连上的测试库；已编译的 exe。
数据库参数读产物目录下的 `config/settings.ini`（程序自己写的），读不到用内置默认值。

用例会真的启动程序、投递消息、读控件文本与矩形，或直连库断言。写库的用例自己快照、自己在
`finally` 里还原，不依赖执行顺序。

### 结构

| 文件 | 内容 |
|---|---|
| `regress.py` | 入口 |
| `regress/core.py` | 窗口驱动、数据库连接、用例注册与执行 |
| `regress/cases_ui.py` | 布局、页签、日志页、对话框、悬浮提示 |
| `regress/cases_gm.py` | GM 页字段、写队列、一键操作、泡点开关 |
| `regress/cases_item.py` | 物品页、发放队列、任务、查询浏览 |
| `regress/cases_char.py` | 改名、克隆、等级职业、全服扫描 |
| `regress/cases_mail.py` | 邮件、封停、在线状态 |
| `regress/cases_db.py` | 连接生命周期、字符集、事务、自检 |
| `regress/cases_env.py` | 连续取码、编号悬浮卡（需要真实输入桌面与 PVF） |

### 分组

- `always`：自还原、可复现、不需要真实输入桌面。
- `manual`：需要真实鼠标/键盘/前台窗口，或依赖特定库状态与 PVF、ImagePacks2。

`needs` 声明前置（`gui` / `db` / `pvf` / `desktop`），不满足时用例标 `skip` 并给出原因，
不计为通过，也不计为失败。

## 写用例

- 一个用例一个函数：`@core.case("域.名字", needs=("gui", "db"), group="always", desc="一句话")`。
- 断言用 `ctx.check(tag, got, want)` / `ctx.ok(tag, cond)`；失败自动计入该用例。
- 写库前快照、`finally` 里还原；角色编号用 `ctx.char_no()`，不要写死。
- 用例之间不共享状态，也不假设库的初始行数。
- 控件编号是 `1000 + page*100 + n`（`core.PID`），界面改了要同步。
- 跨进程读文本只用 `WM_GETTEXT`（Edit/Static/ListBox）；列表只读行数，不读单元格文本。

## 模糊测试

```
python tools\fuzz.py --config     # 损坏 settings.ini
python tools\fuzz.py --pvf 5      # 截断/变异真实 PVF 的副本，跑 5 轮
python tools\fuzz.py --npk        # 合成病态 npk（坏头/超大计数/越界索引）
python tools\fuzz.py --npk-deep 20  # 变异真实 npk 并真解图标 20 轮（zlib + IMG 帧表）
python tools\fuzz.py --sql 20     # 20 轮随机词法碎片喂还原器（只报警不崩）
```

判据只有一条：**报错但活着**。每轮都会备份并还原 `settings.ini`，PVF/NPK 用真实文件的副本。

## 自检

```
DNF_GM.exe --selftest
```

不建窗口、不连库，只跑纯函数：编码、MD5/SHA1、zlib 往返与校验、物品 blob 打包解包、
PVF 解密互逆。结果同时写 `config/selftest.txt`，退出码非 0 表示有失败项；CI 每次构建都会跑。


