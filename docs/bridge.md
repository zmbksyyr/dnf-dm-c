# 可选 DP2 桥接

GM 独立连接 MySQL；在线修改通过可选的 DP2 模块调用游戏接口并确认存档。
桥接脚本和配置由 GM 项目管理。

## 配置

在 GM 的“关于 → 桥接”可一并导出 `dnf-gm-online.lua`、`gm-online.private.json` 和简短说明。
源码为 `src/gm_bridge.lua`；构建时嵌入 GM 可执行文件，不要求额外下载文件。

当前验证的服务端是原 i386 game，SHA-256 为
`da417c5e65918f655afcddb6edfdaf2fa1e77b2f208e5aa397f021fa23b79979`。
不能直接用于其他 ELF。需先自行配置匹配的 DP2，使其提供 `luv`、`json`、`ffi`、
`luasql.mysql`、`df.game`、`df.game.mgr.world` 和 `df.game.symbols.core`。
原 Linux 与 Windows 兼容宿主按相同步骤配置：

1. 备份现有 DP2 入口和配置，将导出的脚本放到游戏进程可见的 `/dp2/dnf-gm-online.lua`。
2. 将一并导出的 `gm-online.private.json` 放到 `/dp2/`。数据库连接使用 GM 当前配置，
   核对该地址是否可从游戏服务器访问。`channel` 默认 3011，按游戏 cfg 中的
   `server_group * 1000 + channel_no` 调整。每个 game 使用自身频道。
3. 在 `/dp2/df_game_r.lua` 的初始化末尾加入 `dofile('/dp2/dnf-gm-online.lua')`。
4. 连接账号需要在 `d_taiwan` 创建和读写 `dnf_gm_hosts_v1..v9`、
   `dnf_gm_online_v1..v9` 的权限；无需 root。正常重启 game，模块自动建表。

## 自动判断

GM 通过既有角色轮询检查账号所在频道的最近 5 秒心跳，并同时检查相应队列表存在。
只开放当前版本实际提供的在线功能。部署、升级或失效后无需重连 GM，控件随轮询更新，
保留正在编辑的表单；其他页面不要求选择或配置桥接。

后台提交再次核对当前账号、角色及频道心跳，按钮可用状态不能代替实际提交检查。
无模块、旧模块缺少相应能力、心跳过期或频道不匹配时，在线请求明确失败，提示先退出游戏；
不自动改走 SQL。确认离线后，原数据库修改功能照常使用。

复活币和当前角色识别使用 v1，金币 v2、疲劳 v3、SP/TP v4、QP v5、泡点账单刷新 v6、
装备设置 v7、负重 v8、角色等级 v9。升级时同时更新 GM 与游戏端脚本，加载错误查看 DP2 日志。
保存或到账结果不确定时须先核对，不能重复发放。

## 来源与边界

脚本从 Native 早期实现迁入 GM，迁移时 SHA-256 为
`82d24d4f9b0070a2b40dec288c576032875d99a9520d6db8a6fcd2c44f436e04`，逻辑未变。
实现依据固定原 ELF 动态符号、原函数行为与 DP2/LuaSQL/luv 接口独立完成，
没有搬运逆向源码或补丁表。详细业务和历史双平台测试范围见 [audit.md](audit.md)。
