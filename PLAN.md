# ZExplorer 计划

> 系列目标：**弥合「Windows 功能面」与「用户接触面」之间的差距**。Windows 提供了能力，用户却够不到——
> 右键菜单只增不减、ADS/Zone.Identifier 藏在 `dir /r` 里、长路径默认关着、占用解锁只能靠第三方工具。
> ZExplorer 是这一系列的第一个应用，也是 ZufyUI 唯一的旗舰压力测试场。
>
> 状态：**P0 进行中**。本文是范围的唯一权威；范围变更需用户确认。

---

## 1. 技术基线（已锁定）

| 项 | 值 |
|---|---|
| UI 库 | ZufyUI **1.29.2 快照**（`git @ a4bab7c`，2026-11-20），只读副本在 `third_party/ZufyUI/` |
| 进程模型 | 单 exe，`asInvoker` 启动，提权走显式 `--elevated` 重启 |
| 平台 | Windows 10 1809+ / Windows 11，x64 |
| 语言 | C++20（`/std:c++20`），MSVC，静态 CRT（`/MT`） |
| 构建 | MSBuild + `ZExplorer.vcxproj` + `build.ps1`（BOM 守卫 → 杀占用 → 编译） |
| 内核态 | **不涉及**（不写 `.sys`，不碰 PPL） |
| 分发 | Inno Setup 安装包（P5） |

> 库的工作区正在被并行修改，因此**锁定为快照**。库有更新时，由用户决定何时重新导出快照。

## 2. 范围

**做**：地址栏/面包屑、导航树、多标签、详细信息视图（+ 后续大图标）、历史、复制/移动/删除/重命名/新建、拖放（内部/外部/拖出）、操作队列与冲突策略、搜索、右键菜单（先原生、后自绘）、快速访问、按目录记忆视图、深浅色、高 DPI、长路径、属性编辑器（ADS/Zone.Identifier）、占用查询入口。

**不做**（需用户确认才可变）：云盘/网络协议实现（走 shell namespace）、文件系统驱动、MFT 索引搜索、压缩包内浏览、双窗格。

## 3. 架构

```
src/
├─ main.cpp                  wWinMain：单实例 → 提权路由 → 主题/DPI → ShellWindow
├─ app/ShellWindow.*         窗口 chrome：标题栏 / 标签栏 / 命令栏 / 状态栏
├─ ui/ExplorerPane.*         单标签内容：地址栏 + 分裂视图（树 | 列表）
├─ ui/FileListView.*         列表视图接口 + 当前 Eager 实现（回退写法）
├─ fs/DirectoryModel.*       取数：枚举 / 排序 / 过滤 → Snapshot（唯一数据入口）
├─ fs/ShellIcons.*           Shell 图标与类型名（异步 + LRU 缓存）
└─ platform/PathUtil.*       路径规范化 / 显示 / 长路径（纯 Win32，可 headless 测）
```

**铁律**（详见 `AGENTS.md`）：
1. `platform/` 只依赖 Win32 + STL，可被无窗口测试覆盖。
2. UI 层不得直接遍历文件系统或调用 Shell，一律经 `fs/`。

**数据源与视图分离**（当前阶段核心约束）：
虚拟化数据源与池化元素尚未就绪，列表先用 `EagerFileListView`（打开目录全量灌入并设上限）。
将来接入库的 `VirtualList` 时，只新增 `VirtualFileListView` 实现，`DirectoryModel` 与页面逻辑不动。

## 4. UI 规格（要点）

窗口默认 1200×760，最小 720×480，全部 DIP。

| 区域 | 高度 | 说明 |
|---|---|---|
| 自定义标题栏 | 32 | `DefaultTitleBar` |
| 标签栏 | 36 | 多标签，中键关闭（P3） |
| 命令栏 | 40 | 导航组 / 新建 ▾ / 剪贴板 / 编辑 / 排序 ▾ 视图 ▾ / 搜索框 |
| 地址栏 | 32 | 面包屑 ⟷ 输入态（`Ctrl+L`），溢出折叠为 `»` |
| 主体 | fill | 左树 220（min 160） + `SplitView` + 列表 |
| 状态栏 | 26 | 项目数 / 选中数+大小 / 可用空间 / 操作进行中 |

列表列（详细信息视图）：`名称`（fill，min 220）· `修改日期` 150 · `类型` 140 · `大小` 90（右对齐）。行高 28，图标 20。

**必须实现的状态**：加载中、空目录、拒绝访问（带"以管理员重启并返回此处"）、路径不存在、部分失败。

## 5. 阶段计划

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| **P0** | 仓库/构建/CI、空壳窗口、三栏布局、`DirectoryModel` 全量加载、列表显示四列、双击导航、状态栏 | `build.ps1` 通过；能浏览本地目录 | 1.5 d |
| **P1** | 虚拟化接入（等库）、列排序、图标、树懒加载、面包屑、历史、多标签、空/错误态 | 10 万项目录滚动 ≥58fps | 4.5 d |
| **P2** | 选择/框选、复制剪切粘贴、删除回收站、重命名、新建、拖放三向、操作队列、进度、冲突策略 | 复制 5GB 可暂停/取消/重试 | 5.5 d |
| **P3** | 右键菜单（原生→自绘）、快捷键全集、快速访问、按目录记忆视图、搜索、撤销 | 20 项日常操作对照系统资源管理器 | 3.5 d |
| **P4** | 属性编辑器（ADS/Zone.Identifier）、占用查询、符号链接角标、大图标视图 | 差异化清单逐项可用 | 3 d |
| **P5** | Inno 安装包、图标、深浅色、DPI、长路径、性能回归 | 装→用→升→卸 四步干净 | 2 d |

---

## 6. TrustedInstaller 接入方案（暂缓实现）

> 结论：老项目里真正的实现位于 `pc_easy2/powerrun/main.cpp`。
> `pc_easy2/exp/main.cpp` 中的同名函数是空壳，不能从那里迁移。

### 6.1 安全边界

UI 进程永远不打开或复制 TrustedInstaller 令牌。调用链固定为：

```text
ZExplorer UI
  -> ZExplorer Host（任务与参数校验）
  -> ZBroker（管理员常驻、鉴权、审计）
  -> 一次性 TI Worker（复制 TrustedInstaller 主令牌后启动）
  -> 执行单一 typed file operation
```

默认不开放“任意命令以 TrustedInstaller 运行”。ZBroker 面向 ZExplorer 的接口只接受
白名单文件操作：`ti-delete`、`ti-rename`、`ti-move`、`ti-set-attributes`、
`ti-take-ownership`。每个请求必须携带操作类型、规范化路径、发起 UI 的 PID/路径、
任务 ID 和用户确认结果。

### 6.2 令牌与进程

1. 启用 `SeDebugPrivilege`、`SeImpersonatePrivilege`、
   `SeAssignPrimaryTokenPrivilege`、`SeIncreaseQuotaPrivilege`。
2. 确保 `TrustedInstaller` 服务运行；记录原状态，不为了提权无条件停止服务。
3. 等待 `SERVICE_RUNNING`，避免启动阶段竞态。
4. 打开服务进程令牌并复制为 `TokenPrimary`。
5. GUI PowerRun 需要设置 `TokenSessionId`；文件操作 Worker 保持无窗口会话。
6. 用 `CreateProcessAsUserW` 启动一次性 Worker，及时关闭全部临时令牌和进程句柄。
7. Worker 返回结构化结果，Broker 写审计后退出。

### 6.3 老实现中已知的问题

- 令牌复制成了 `TokenImpersonation`，却传给要求主令牌的
  `CreateProcessWithTokenW`。
- 没有设置 `TokenSessionId`，GUI 命令可能落到不可见会话。
- 每次提权先停止 `TrustedInstaller` 服务，影响系统更新。
- 只包装任意命令行，没有 typed file op、参数白名单、结构化结果和审计。
- `exp/main.cpp` 的同名函数为空，旧版本实际调用不到 PowerRun 的真实实现。

### 6.4 分阶段交付

| 阶段 | 内容 |
|---|---|
| TI-0 | Broker 内实现令牌探测、服务启动和一次性 Worker，验证 `whoami /user` 的 SID |
| TI-1 | 只开放 `take-ownership` 与 `set-attributes` |
| TI-2 | ZExplorer 接入 typed delete/rename/move，强制用户确认与审计 |
| TI-3 | 独立 PowerRun 界面，可显式选择 Administrator / SYSTEM / TrustedInstaller |

## 7. 已确认边界

- 不做老 Qt 日志查看器；当前任务/错误管理机制替代它。
- 多标签页暂缓，不作为下一阶段前置条件。
- TrustedInstaller 后续实现，必须先完成 Broker typed-op 设计。

## 8. 对 ZufyUI 的需求（当前状态）

> 原则：只要求通用 UI 能力，**不要求任何文件系统/Shell 集成**。

| # | 需求 | 状态 | 影响 |
|---|---|---|---|
| R1′ | **`VirtualList` 控件**：`SetItemCount` / `SetRowHeight` / `DrawRow(i,rt,rect)` / `GetScrollOffset` / `ScrollToIndex` / 内置单选多选与键盘导航 | **待做**（并行开发中） | 阻塞 P1；P0 用 Eager 回退 |
| R2 | ~~`TableView` 虚拟化~~ | **已删除** | 四列由 `DrawRow` 自绘，`TableView` 不改 |
| R3 | `TreeView` 异步节点（占位 + 加载回调） | 待做，可降级为同步单层枚举 | 影响 `C:\` 与网络路径 |
| R4 | `RunAsync<T>` + `AsyncCancelToken` | ✅ **已有**（快照内 `ZufyUI.h:921-1095`） | — |
| R5 | `ProgressDialog` / 任务面板容器 | 待做 | P2 传输队列 |
| R6 | `SettingsStore`（JSON / `%APPDATA%`） | 待做 | 视图状态持久化 |
| R7 | 框选（marquee） | 待做，或随 R1′ 内置 | P2 |
| R9 | `Image` 的 `HBITMAP` 通道保留 alpha | 待确认（`WICBitmapIgnoreAlpha` 会丢透明） | 图标边缘 |
| R10 | `Menu` 横向菜单行：每项独立图标、Inline/IconTop、2-5 列、键盘/UIA/DPI | **待实现**，完整规格见 `docs/MENU_REQUIREMENTS.md` | ZExplorer 右键菜单艺术重构 |

## 7. 风险

| 风险 | 对策 |
|---|---|
| R1′ 延期 | P0 的 Eager 实现保证"能跑"；P1 才需要虚拟化 |
| 长路径与 Shell 混用 | 内部用 `\\?\`，交给 Shell 前转回常规路径 |
| `ReadDirectoryChangesW` 拖慢根目录 | 只监听当前目录（不递归），树节点单层监听 |
| 删除误操作 | 一律走回收站；永久删除二次确认 |
| 无 EV 签名 | 安装包附 SHA256 与说明（P5） |

## 8. 待用户确认

1. 属性编辑器（ADS/Zone.Identifier）放在这里还是独立项目。
2. 免责声明形式（强制同意 / 首页须知）。
3. 库快照的更新节奏。
