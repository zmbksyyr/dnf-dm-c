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

### 数据层故障注入与参考项目对照

```
python tools\db_integration.py --build
python tools\compare_item.py --build
```

前者编译无窗口探针，直接调用生产数据层，验证事务中途失败、附件归属、克隆槽位、
备份取消与特殊列、还原会话恢复、SQL 模式及 MySQL 分包边界，以及泡点缺行建账、
重复结算、目标冻结、溢出保护和跨收件人/跨批次故障回滚。需要测试库的建表、
建触发器权限；用例在 finally 中清理临时记录、触发器与自增计数。
只在测试库执行，并与 GUI 回归、游戏服务和其他写入者错开。

连接参数读同一份 settings.ini，或用 `DNF_GM_TEST_HOST`、`DNF_GM_TEST_PORT`、
`DNF_GM_TEST_USER`、`DNF_GM_TEST_PWD` 覆盖；不在命令行参数或日志中输出密码。
探针产物位于 `..\_build\audit\`，`DNF_GM_BUILD` 可指定读取配置的构建目录。

后者不连接数据库，只抽取两个本地 Python 参考项目的 `DnfItemSlot` 类，用固定种子的
160 个物品槽逐字段比较解析结果，再逐字节比较修改后的 61 字节。默认从相邻目录查找
参考项目；可重复传 `--reference 路径/sqlManager2.py` 指定其他副本。

真实 PVF 与贴图对照：

```
python -m pip install --target ..\_build\audit\pydeps zhconv setuptools
python tools\compare_resources.py --build --pvf "路径\Script.pvf" --images "路径\ImagePacks2"
```

此工具通过 `resource_probe.c` 调用生产解析器，只读游戏资源，不连接数据库。
逐项比较文件树元数据，抽样比较解密字节、物品名称、堆叠上限和装备类型，
核对经验表的原始 32 位值，并验证初次与缓存加载结果一致。提供 `--images` 时还会
建立 NPK 索引、解码各类图标并生成物品详情；它不验证游戏内显示与领取。
参考类直接从两个 Python 项目抽取，保留其真实 `zhconv` 简繁转换；C 名称在比较时
转为简体，原文差异单独记录。可重复传 `--reference 路径/pvfReader.py` 指定参考。
报告写入 `..\_build\audit\resources-check.json`，初次加载可能使用已有缓存；
耗时仅供本机观察，不代表跨机器基准。

特殊道具发放的界面回归：

```
python tools\regress.py --group all --only item.special_
```

需要已配置真实 PVF 的测试环境。覆盖时装/宠物/宠物装备拒绝直接入普通背包、
多件时装宠物逐件建立附件，以及后续邮件失败时整批回滚；测试完成后还原数据与自增计数。

泡点的界面回归可单独运行：`python tools\regress.py --group manual --only db.bubble_`，
覆盖定时发放、停用，以及结算等待写锁期间修改配置。仅在线模式保持在线账号下全部
未删除角色的范围；账号奖励去重，界面分别显示到账角色数和账号数。

备份现使用全部目标表的 READ 锁获得跨 InnoDB/MyISAM 的一致数据，期间会阻塞这些表的
写入；任一读写失败或取消都不会覆盖已有目标文件。完整备份和角色备份都检查结束标记。
结束标记用于检测中断，不提供内容真实性或每行完整性的校验。
还原会恢复会话的 autocommit、foreign_key_checks 与 sql_mode；DDL 会隐式提交，
MyISAM 不支持事务，因此含这些操作的还原仍需依靠事前备份恢复。

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

- 长任务（备份 / 还原 / 全服扫描）会先弹**模态进度窗**，主窗口被禁用。用例要在任务跑着的时候
  继续驱动界面，必须先 `core.scan_bg(pid)`（扫描）或 `core.progress_bg(pid)` 点「后台运行」；
  验证取消用 `core.progress_cancel(pid)`；转后台后再点同一个入口按钮会重开进度窗（工具栏没有取消按钮）。
- 一个用例一个函数：`@core.case("域.名字", needs=("gui", "db"), group="always", desc="一句话")`。
- 断言用 `ctx.check(tag, got, want)` / `ctx.ok(tag, cond)`；失败自动计入该用例。
- 写库前快照、`finally` 里还原；角色编号用 `ctx.char_no()`，不要写死。
- 正常克隆用例须有有效源角色槽位；可在原始视图快照保护下调用 `core.seed_test_char_slot`，
  并在 finally 还原整行视图。缺槽位的错误路径由数据层用例单独验证。
- 用例之间不共享状态，也不假设库的初始行数。
- 控件编号是 `1000 + page*100 + n`（`core.PID`），界面改了要同步。
- 跨进程读文本只用 `WM_GETTEXT`（Edit/Static/ListBox）；列表只读行数，不读单元格文本。

## 压测与一致性

```
python tools\soak.py --sec 480        # 随机操作压测：盯崩溃、句柄、内存
python tools\kill_test.py             # 写操作卡住时硬杀进程，验数据没被写一半
```

`soak.py` 判据：进程活着、没有新转储、GDI/USER 句柄与工作集在首轮建页后不再增长。
`kill_test.py` 判据：杀进程后目标列仍是原值（服务端回滚），重启后能正常连库读数据。

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
PVF 解密互逆，以及 UTF-8 边界、转换缓冲区边界、固定/动态 Huffman 流和解压上限。
结果同时写 `config/selftest.txt`，退出码非 0 表示有失败项；CI 每次构建都会跑。


