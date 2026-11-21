# ZExplorer 交接文档（HANDOFF）

> 最后更新：2026-11-21
> 这份文档是**唯一完整交接材料**：项目目的、硬规则、架构、已实现/未实现的功能、
> 用户提出过的全部需求、已知问题、库反馈、构建与验证方式、以及踩过的坑。
> 其它文档：`PLAN.md`（范围与阶段）、`AGENTS.md`（硬规则）、`docs/STATUS.md`（按时间的过程日志）、
> `docs/LIBRARY_FEEDBACK.md`（给 ZufyUI 的反馈清单）。

---

## 0. 这份项目的**真实目的**（很重要，别只当文件管理器做）

三层目的，按重要性：

1. **给 ZufyUI 做真实压力测试，把库的 bug 逼出来。**
   ZExplorer 是 ZufyUI 唯一的旗舰应用。库作者（就是用户本人）需要真实业务去暴露
   API 缺口、生命周期问题、并发问题。所以：
   - 遇到库的问题**不要就地改 `third_party/`**，写成 `docs/LIBRARY_FEEDBACK.md` 里的条目（带最小复现/dump 栈）。
   - 用库的方式要"较真"：能证明是库的问题就写清楚，不要用业务代码糊过去；
     万不得已的绕法要注明"这是绕开库 bug"，并在反馈里登记。
2. **弥合「Windows 功能面」与「用户接触面」之间的差距。**
   Windows 有能力但用户够不到：右键菜单只增不减、ADS/Zone.Identifier 藏在 `dir /r`、
   长路径默认关闭、占用解锁要装第三方工具……ZExplorer 是这条产品线的第一个应用。
3. **交付一个能日常用的文件管理器**：单 exe、自带后台进程、操作可取消可重试、
   所有写操作都由后台进程执行。

> 用户的耐心在"同一个操作里出现 4 个以上 bug"时会被耗尽。要求：
> 每个改动**自己先把边界情况想完**（空列表、失败、取消、并发、重复点击、路径特殊字符），
> 不要等用户报一个修一个。

---

## 1. 硬规则（违反会被骂，且会把项目搞坏）

来自 `AGENTS.md`，摘要 + 实战补充：

1. **不得修改 `third_party/ZufyUI/`**。它是只读快照。库更新时由用户说"重新复制一下"，
   用 `git -C C:\project\ZufyUI ls-files` 重新导出（见 §9）。
2. **不得擅自缩小范围**。范围以用户为准；要写"不做"必须先得到确认。
3. **不得自动 `git commit` / `git push`**。
4. **不得用机制掩盖 bug**。先定位根因；定位不到就写"未定位"，不要加延时/重试/上限糊过去。
5. **编码规则**：
   - `.h` / `.cpp` 必须 **UTF-8 带 BOM**（`build.ps1` 会拦，缺 BOM 直接失败）。
   - `.md` 必须 UTF-8 **无 BOM**。
   - `build.ps1` 与 `tools/*.ps1` **必须纯 ASCII 无 BOM**（PowerShell 5.1 会按 GBK 解析中文而崩）。
6. **分层铁律**：
   - `src/platform/` 与 `src/fs/` 只依赖 Win32 + STL，**不 include ZufyUI、不建窗口**，必须能被 headless 测试。
   - `src/ui/`、`src/app/`、`src/host/` 不得直接做文件系统写操作或调 Shell（先加到 platform 层）。
7. **改代码用 `edit` 工具 + 唯一锚点**。不要用 PowerShell 正则批量替换或按行号拼接——
   本项目已经因此两次把文件切坏（正则静默失败 / 大括号丢失）。
8. **UI 线程不得被阻塞 >50ms**：所有 IPC、复制、删除都不能在 UI 线程同步做。
9. **不要截图桌面**。只能截我们自己的窗口（`PrintWindow` 抓不到 D2D 内容，见 §9）。

---

## 2. 技术基线

| 项 | 值 |
|---|---|
| UI 库 | ZufyUI **1.30.0**（`7759891` + 横向菜单行收尾修改），只读副本 `third_party/ZufyUI/`（右键菜单横向行、逐项图标、Inline/IconTop、Equal/ContentFit） |
| 进程模型 | **同一个 exe 两种角色**：`ZExplorer.exe`（前台 UI）/ `ZExplorer.exe --host`（后台） |
| 平台 | Windows 10 1809+ / Windows 11，x64 |
| 语言 | C++20，MSVC，静态 CRT（/MT） |
| 构建 | MSBuild + `ZExplorer.vcxproj` + `build.ps1`（BOM 守卫 → 杀占用 → 编译 → 平台测试） |
| 内核态 | **无**（不写 .sys，不碰 PPL） |
| 分发 | Inno Setup 安装包（未做） |
| 清单 | `app.manifest`（PerMonitorV2、longPathAware）；`app.rc`（图标 101 + VERSIONINFO） |

### 2026-11-21 设置批次

- `platform/Settings` 现在承载启动方式、默认排序、窗口/分栏/列宽、任务调度参数和系统集成开关。
- 托盘提供设置、关于、一键重置；设置窗口使用滚动分组卡片。
- Shell 右键菜单、文件夹默认打开方式均可安全撤销；默认打开方式开启前会备份原命令。
- 目录背景菜单禁止使用 `MultiSelectModel=Single`，否则 Shell 会静默丢弃该菜单项；
  现已覆盖桌面背景、目录背景、文件夹和驱动器，并通过 `IContextMenu` 枚举验证。
- 菜单扫描范围沿用 `C:\project\RightClickManager\registryhelper.cpp` 的经典位置集合。
- Win+E 由后台低级键盘钩子接管，显式开启；钩子安装失败会把设置回退为关闭。
- Win+E 使用 `platform/WinEKeyState.h` 事件状态机，禁止用 `GetAsyncKeyState` 拼状态；
  回归测试覆盖“Win+E 后单独按 E 不得再次启动”。
- Win-up 必须放行：先发送 Ctrl Menu Mask，再 `CallNextHookEx`。吞掉 Win-up 会残留
  Win-down 状态，导致后续单独按 D 被解释成 Win+D。
- 数值设置采用滑杆 + ZufyUI `NumberBox`，保留范围、步进、错误态和滚轮/步进按钮。
- 回收站页面使用 `TableView` + Recycle Bin 命名空间的相对 PIDL；右键是 Explorer 原生
  `IContextMenu`，不再使用会被异步刷新销毁的临时 Label 行。
- 设置窗口按分类使用 `TabView`（启动与浏览 / 文件列表 / 任务与后台 / 系统与维护），
  一键重置位于“系统与维护”页。
- 设置修改只写草稿，底部“保存并关闭”统一提交；失败会重建设置页并用抽屉显示错误。
- 回收站地址栏使用专用 `::recyclebin` 标识，树和列表共享隐藏文件设置。
- 搜索输入自动递归子目录（220ms 防抖），Enter 立即搜索，全部在后台线程完成。
- 搜索框位于第二层操作栏右侧；操作栏三段布局为新建 / 文件操作 / 查看与搜索。
- 首页使用动态 2/3 列 GridLayout；常用位置在带内部滚动的 Expander 中，驱动器独立网格。
- 首页卡片整卡悬停/点击，文字居中，无强调色按钮。
- 自定义固定在首页和左侧快速访问中都排在系统常用位置之前，并按路径去重。
- 首页卡片悬停使用主题 Selection 色；目录用中间省略，首尾都保留。
- 快捷访问顺序为自定义固定项 → 此电脑 → 系统常用目录 → 回收站；驱动器显示
  可用量/总量。
- Explorer 第二层操作栏依赖现有文件操作函数；New 主按钮默认新建文本文档，
  右侧下拉选择文件/文件夹。
- 浏览状态采用“只在变化时写注册表”的持久化，自检模式不会覆盖用户设置。
- 控制系统入口：`ZExplorer.exe --show-settings`、`ZExplorer.exe --show-about`。

---

## 3. 架构（当前真实状态）

```
ZExplorer.exe（前台 UI）                          ZExplorer.exe --host（后台）
┌────────────────────────────┐                   ┌──────────────────────────────┐
│ main.cpp  --host? 分派      │                   │ HostApp：任务队列 + 执行器      │
│ app/ShellWindow 窗口/状态栏 │                   │  - 每个任务一个线程（上限 4）    │
│ app/TaskClient  IO 线程     │  命名管道          │  - FileOps 执行复制/移动/删除    │
│   ├ Submit/Query/ShowTask  │ ───────────────▶  │  - 待决冲突(等待用户决定)        │
│   └ 每 250ms 拉一次快照     │  \\.\pipe\ZExplorer.Host │  - TaskWindow：单窗口+页签  │
│ ui/ExplorerPane 地址栏/树/列表│ ◀───────────────  │  - 托盘图标 + 设置窗口          │
│ fs/DirectoryModel 取数      │                   │  - PipeServer（串行处理请求）   │
│ fs/ShellIcons 图标          │                   └──────────────────────────────┘
└────────────────────────────┘
```

关键文件与行数（2026-11-20）：

| 文件 | 行 | 职责 |
|---|---|---|
| `src/main.cpp` | 138 | `--host` 分派、自动提权、初始路径、自检参数、**退出用 ExitProcess 绕开库静态析构崩溃** |
| `src/app/ShellWindow.cpp` | 365 | 主窗口、状态栏、导航联动、自检状态机（6 步，写报告 + PNG） |
| `src/app/TaskClient.cpp` | 199 | **IO 线程**：所有管道调用都在这里；UI 只读内存快照 |
| `src/ui/ExplorerPane.cpp` | 1257 | 地址栏（TextBox+面包屑叠加）、树、列表、右键菜单、快捷键、**此电脑页**、任务条 |
| `src/ui/FileListView.cpp` | 177 | `TableView` + 单元格标签提供器（名称/大小/类型/日期） |
| `src/fs/DirectoryModel.cpp` | 226 | `FindFirstFileExW(LARGE_FETCH)` + 自然排序 + 目录优先 |
| `src/fs/ShellIcons.cpp` | 120 | HICON 缓存（目录用中性 `"folder"` 探针） |
| `src/host/HostApp.cpp` | 755 | 任务模型、执行器、IPC 处理、托盘、设置、冲突待决 |
| `src/host/TaskWindow.cpp` | 478 | 任务窗：**单窗口 + 首页(不可关) + 每任务页签 + 错误卡片** |
| `src/platform/FileOps.cpp` | 571 | 复制/移动/删除/重命名/新建/剪贴板 + 冲突策略 + 逐项错误 |
| `src/platform/Settings.cpp` | 72 | `HKCU\Software\ZExplorer` 设置持久化 |
| `src/platform/ShellActions.cpp` | 135 | Shell 打开/属性/提权/驱动器枚举/应用图标/路径图标 |
| `src/ipc/Protocol.h`+`Pipe.cpp` | 193 | 长度+类型帧、UTF-16 参数表；一次连接一次请求/响应 |

### IPC 协议（`src/ipc/Protocol.h`）

帧 = `uint32 type` + `uint32 payloadBytes` + UTF-16 参数表（每项以 `\0` 结尾）。

| 消息 | 参数 | 说明 |
|---|---|---|
| `Ping` | — | 回 `1/pong/页签数/待处理任务数`（自检用） |
| `SubmitTask` | `op, target, src...` | `op` 见下；**target 为空 + op=copy/cut = 只写剪贴板，不建任务** |
| `QueryTasks` | — | 每任务一行：`id \t state \t ratio \t title \t current \t issueCount \t firstIssue \t doneFiles \t totalFiles` |
| `ShowTask` | `id` | 切到该任务页签并前置窗口（走 UI 队列，**管道线程不碰 UI**） |
| `CancelTask` | `id` | 设 cancel 标志 |
| `QuitHost` | — | 退出后台 |

`op`：`copy` / `cut` / `paste` / `copyover` / `delete` / `deleteperm` / `rename` /
`newfolder` / `newfile` / `streamimport` / `streamexport` / `streamdelete` /
`streamhide` / `streamwrite` / `streamrename` / `killprocess`。

`state`：0 排队、1 运行、2 完成、3 部分失败、4 取消、**5 等待处理（冲突/错误待决）**。

---

## 4. 已实现的功能（可用）

### 4.1 浏览
- 起始页 = **此电脑**（`::thispc`）：驱动器磁贴（真实卷图标 + 使用率进度条 + `X 可用，共 Y`），点名字进盘符；盘符根按"上一级"回此电脑。
- 地址栏：TextBox 与面包屑（`Card` 叠加）互斥显示，`Ctrl+L` 进编辑态，点外部失焦回到面包屑。
- 左树：懒加载、点击导航、**导航时深度同步（展开→选中→滚动）**；树只读，不参与操作。
- 列表：`TableView` 四列（名称/大小/类型/修改日期，大小右对齐），图标按扩展名/目录缓存。
- 导航：双击进入、Enter 打开、Backspace 上一级、前进/后退历史。

### 4.2 选择
- 单选、`Ctrl+A` 全选、框选（`SetMarqueeEnabled`）、勾选（`SetCheckable`）、两者同步（`SetMarqueeCheckSync`）。
- 多选后所有操作作用于全部选中项（复制/剪切/删除/永久删除）。

### 4.3 文件操作（**全部在后台进程执行**）
- 复制/剪切（写剪贴板 `CF_HDROP` + `Preferred DropEffect`，**与资源管理器互通**）
- 粘贴（读剪贴板；跨卷移动自动退化为复制+删除）
- 删除（回收站 `SHFileOperationW`）、永久删除（递归，自动去只读）
- 重命名、新建文件夹、新建文本文档
- 分块复制（1 MB/块）→ 大文件有进度、可取消、取消会删掉半截目标文件
- **逐项继续**：单个文件失败只记一条 `OpIssue{path, target, message, kind}`，不中断整批
- **冲突策略**：遇到同名文件 → 记一张卡片（源+目标全路径），本任务停在 `state=5` 等决策；
  `覆盖` / `跳过` / `全部覆盖` / `全部跳过` 都是**同一个任务内部**的决策（不产生新任务）
- 安全护栏：目标在源目录内部直接拒绝（防无限递归）、符号链接/联接点跳过、目标只读自动去只读、回收站批量失败逐个归因

### 4.4 任务窗（`src/host/TaskWindow.cpp`）
- **只有一个窗口**，第一个页签是 `操作`（**不可关闭**，历史卡片，进行中优先排序）
- 每个任务一个页签，**直接使用稳定任务 id**；首页固定 id `-1`。查找、选中、删除均走
  `FindTabById` / `RemoveTabById`，不再维护索引与任务 id 的平行数组。
- 页签 × 和"取消"按钮 = 取消任务并关闭页签（**延后删除且去重**，避免在事件回调里删自己）
- 卡片（只增不减 + 隐藏，**永不销毁**）：操作名、源文件按钮、目标文件按钮（点了用**本程序**打开所在目录）、错误说明、动作按钮
- 进度：`45% · 12/36 个文件 · 3.1 MB/s · 已用 5 秒 · 剩余约 4 秒`
- 当前文件在复制开始时就显示，不再一直停在“准备中…”
- 完成/失败/取消后总用时冻结；成功任务保证 `done = total`
- 任务窗**只在 0.35 秒后才弹**（跑得快的任务完全不打扰），失败/等决策时自动弹出并前置
- 出错会**闪任务栏**（`FlashWindowEx`）

### 4.5 设置 / 权限
- 设置窗口（`ToggleSwitch` 真开关）：出错时暂停等待、出错时切页签并前置、启动自动请求管理员
- 设置持久化：`HKCU\Software\ZExplorer` 的 `AutoElevate` / `PauseOnError` / `FocusOnError`
- 自动提权：启动读 `AutoElevate` → `RelaunchElevated()`（**保留命令行参数**）→ 提权后的前台再拉起后台，后台继承管理员权限

### 4.6 外观
- 应用图标：`tools/make_icon.ps1` 生成 10 个尺寸（16/20/24/32/40/48/64/96/128/256）
  - ≤20px 只画粗体 "Z"，≥24px 画"白文件夹 + 蓝 Z"
  - `app.rc` 里 `101 ICON "app.ico"`，窗口图标按 `GetSystemMetrics(SM_CXICON/SM_CXSMICON)` 取
- 主题跟随系统；窗口 `Backdrop::None` + `ThemeRole::Backdrop`（Mica 在宿主上会全透明，别开）

---

## 5. 验证方式（**必须保留**）

```powershell
cd C:\project\ZExplorer
powershell -ExecutionPolicy Bypass -File build.ps1
# 4 步：BOM 守卫 → 杀占用 → msbuild → 平台测试（tools/run_tests.ps1）

# 现有实例是高权限、不能无 UAC 结束，或希望保留实例：
powershell -ExecutionPolicy Bypass -File build.ps1 -FalseKill
# -FalseKill 是 -NoKill 的别名：不结束进程，输出到 tmp_fast/no-kill-<时间>/
```

当前基线（2026-11-20）：

```
187 passed, 0 failed       # 平台层断言（PathUtil / DirectoryModel / FileOps / FileAttributes / NtfsStreams / FileLock / DirectoryWatcher）
OK -> x64\Release\ZExplorer.exe (约 1.84 MB)
--selftest -> PASS
```

**自检**：`ZExplorer.exe --selftest=<report.txt>` 起一个 6 步计时器状态机，截图 + 写报告后退出。
截图是应用自己 `PrintWindow` 抓的（**抓不到 D2D 客户区，只能抓到标题栏**，所以视觉效果必须人眼看）。

**管道自检脚本**：`tools/ipc_probe.ps1`（用 PowerShell 直接发协议帧，验证提交任务/查询/取消）。

### 注意：启动后台进程的方式

不要用 `Start-Process -WindowStyle Hidden`——`STARTUPINFO` 会让后台第一次 `ShowWindow` 被压成隐藏，
任务窗看不到。用 `Start-Process ... -RedirectStandardOutput/-RedirectStandardError`（否则子进程继承
stdout 句柄，调用方会一直等）。

---

## 6. 用户提出过的**全部需求**（按主题，含状态）

> 这张表是需求真相源。新周期先看这里，别只做最后一轮说的。

### 6.1 UI / 交互
- [x] 左树跟随导航：深度展开 + 选中 + 滚动（含"刚插入子节点时 maxScrollY_ 过期"的绕法）
- [x] 空间不足时**自动折叠左侧**（不是默认折叠）
- [x] 面包屑每一段独立成框，超宽才在自己内部省略
- [x] 浅蓝色用主题选中色
- [x] 焦点离开地址栏要恢复面包屑（含点其它控件失焦）
- [x] 左右两侧选中/滚动同步
- [x] 默认打开**此电脑**，驱动器显示成**小方块磁贴**（图标 + 进度条 + 容量文字）
- [x] “此电脑”进入前进/后退历史；启动不再先加载用户目录再覆盖
- [x] 表头点击排序（名称/大小/类型/修改日期，升降序指示，按路径保持选择）
- [x] 多选：框选 + 勾选 + 同步
- [x] 右键菜单**合成一个**（不分离），没选中时把选中相关项置灰
- [x] 快捷键全集：Enter/Alt+Enter/F2/F5/Delete/Shift+Delete/Ctrl+C·X·V·A/Ctrl+Shift+N/Backspace
- [x] 打开方式（右键 / Ctrl+O）
- [x] 文件属性编辑器：完整基本属性 + `Expander` 折叠高级属性 + 系统管理标志只读展示
- [x] Zone.Identifier：删除、本地/可信/互联网/受限区域、自定义内容
- [x] NTFS 文件流管理器：底部仅“创建文件流 / 刷新 / 关闭”；
  重命名、删除、文本编辑、导入文件内容、导出为文件全部在流列表右键菜单
- [x] 创建文件流只弹名称输入框并直接创建空流；默认流/空选择禁用右键操作
- [x] NTFS 文本流快速创建/编辑、数据流重命名
- [x] 解除文件/文件夹占用：全系统句柄独立扫描 + Restart Manager 类型补充 +
  同 PID 多句柄显示/批量关闭 + 删除共享探测 + 结束进程；
  根目录 `FileLockLab.exe` 覆盖独占、共享、映射、区域锁、跨进程模式
- [x] 所有 UI 任务携带发起进程 PID/路径；任务错误按来源窗口进入应用内通知抽屉
- [x] 地址栏整块点击编辑并全选，支持环境变量路径、当前目录命令启动和应用内错误抽屉
- [x] 复制路径（Ctrl+Shift+C，多选按行复制）
- [x] 左树同步仅在目标不可见时最小滚动，不再重定位视口内的节点
- [x] 自有快捷访问树分区，带图标、右键复用目标菜单；上下分栏下限为左栏 1/12，并可继续拖入边缘折叠
- [x] 文件/快捷项右键“系统右键菜单…”，接入 Windows 原生 Shell 菜单（规范动词 + 序号回退）
- [x] 设置“打开可执行文件时使用 ShellExecute”（默认开启）；关闭后交给 Host 按其权限启动
- [x] 错误抽屉改为大尺寸只读文本，汇总全部任务问题、进程/路径/时间和选择信息，可复制
- [x] 右键菜单横向分组：打开 3 项、新建 2 项、编辑 4 项、属性 2 项，横向子项逐项图标
- [x] 普通提示只走底部状态栏；仅 Warning/Error 打开完整通知抽屉
- [x] 任务普通错误支持全部重试/全部忽略；单项重试失败会保留新错误并重新等待处理
- [x] 显示隐藏/系统文件、隐藏已知扩展名选项
- [x] 选中行直接拖动拖出；空白/未选中行拖动框选；Shift+点击叠加多选；外部文件可拖入
- [x] ReadDirectoryChangesW 自动监视与防抖刷新
- [x] 状态栏选中数量和文件大小合计
- [x] 快捷访问内建特殊位置；此电脑常用文件夹/库使用图标卡片；回收站查看与清空
- [x] ITaskbarList3 任务栏进度
- [x] 所有非空提示进入底部通知抽屉
- [x] 新窗口由 Host 通过 IPC 创建，当前 UI 不直接拉起
- [x] 新建文件/文件夹**先弹输入框**（确定/取消）
- [ ] 多标签页浏览（PLAN P1，未做）
- [ ] 大图标视图（未做）
- [ ] 拖放三向（未做）

### 6.2 文件操作
- [x] 所有写操作在**后台进程**执行，UI 只发指令
- [x] 每个任务一个"可取消"的执行单元；并发上限 4
- [x] 冲突：**同一个任务内**决定覆盖/跳过/全部（不要新任务）
- [x] 失败项卡片：源/目标全路径 + 动作（重试/覆盖/跳过/忽略/全部）
- [x] 逐项继续，不因一个失败中断整批
- [x] 进度：总进度条 + `百分比 · 已完/总数 · 速度`
- [x] 删除回收站 / 永久删除，错误消息人话化
- [ ] 拖放、撤销、暂停/继续

### 6.3 任务窗
- [x] 单窗口 + 页签（省显存）
- [x] 首页不可关闭、历史卡片、进行中优先
- [x] 0.35 秒延迟弹窗；出错/等决策自动弹出并前置
- [x] 完成 1.2 秒后自动收起页签
- [x] 关窗只是隐藏，任务继续
- [x] 出错闪任务栏
- [x] 托盘：后台有托盘（显示任务窗口 / 打开新的操作窗口=**新的主 UI 进程** / 设置 / 退出）；**前台不要托盘**
- [ ] 首页卡片显示用时/剩余时间
- [ ] 已完成任务卡片定时回收

### 6.4 设置 / 权限
- [x] 设置用**开关**控件，不用按钮
- [x] 设置存注册表（用户级）
- [x] 自动请求提升权限（启动时读开关 → UAC → 整个程序含后台以管理员运行）
- [ ] 设置里"并发上限 / 默认冲突策略 / 开机自启"

### 6.5 外观
- [x] 应用 logo/图标，放进资源文件，标题栏和任务栏都显示
- [x] 图标要清晰（多尺寸 + 小尺寸简化）
- [ ] 视觉细节（磁贴排版、卡片对齐）用户仍在挑

---

## 7. 已知问题（**未解决，接手请先看**）

| # | 现象 | 现状/线索 |
|---|---|---|
| 1 | 托盘"退出"曾无响应 | `RequestQuit()` 在菜单/管道回调线程同步 join 任务线程，任务未收尾时阻塞 UI 回调。已改为异步投递到 UI 线程：取消任务 → 停 PipeServer/任务泵 → `PostQuitMessage`；进程再由 `ExitProcess` 结束以绕开库静态析构问题 |
| 2 | 图标"发虚" | 已重画 + 10 尺寸 + 按 DPI 取窗口图标。用户仍反馈糊，**未确认是否解决**；下次可尝试 PNG-compressed ICO 或从矢量源（SVG→多尺寸）生成 |
| 3 | 任务窗卡片视觉 | 用户多次说"对不齐/鬼畜"。已把首页卡片改成增量更新（不重建）、卡片 `SetFillWidth`，**未最终确认** |
| 4 | 库的 `SetUserContent` 清空按钮 | 已改用自建模态输入框（`PromptName`） |
| 5 | `third_party` 与新库同步 | 每次用户说"库更新了"要重新导出（§9） |

---

## 8. 库（ZufyUI）反馈摘要

完整版见 `docs/LIBRARY_FEEDBACK.md`（带 dump 栈与最小复现）。要点：

1. **【崩溃】** 进程退出时静态析构顺序：`MenuWindow::StandaloneHolder` → `~Window` → UIA 表已销毁。
2. **【崩溃】** 销毁"正在交互的元素"无保护（`RemoveChild`/`ClearChildren` 立刻析构，内部仍持裸指针）。
3. **【易踩】** `MessageBox::SetUserContent` 会静默清空按钮。
4. **【易踩】** `TabView` 没有按 id 找页签（**最新库已加 `AddTab(...,id)`/`GetTabId`/`RemoveTabById`，尚未在业务里换用**）。
5. **【不便】** `Card` 只能单子元素。
6. **【缺失】** `TableView` 没有批量更新（`ListView` 有 `BeginUpdate`）。
7. **【绘图】** `Image` 不是 `UIElement`，图标要套 `Label`。
8. **【细节】** `Window::SetIcon` 不按 DPI 选尺寸。
9. `TreeView::ScrollToNode` 在刚插入大量子节点时被旧 `maxScrollY_` 夹小（我们用 `GetVisibleNodes + SetScrollOffsetY` 绕开）。

> 已修好：`RemoveChild/RemoveChildAt/InsertChild` 已进库。

---

## 9. 重新导出 ZufyUI 快照

```powershell
$src="C:\project\ZufyUI"; $dst="C:\project\ZExplorer\third_party\ZufyUI"
$files = & git -C $src ls-files
foreach ($rel in $files) {
  if ($rel -like 'SystemDump/*' -or $rel -like 'docs/*' -or $rel -like 'tests/*') { continue }
  if ($rel -like '*.vcxproj*' -or $rel -like '*.sln') { continue }
  $s = Join-Path $src $rel; $d = Join-Path $dst $rel
  if (-not (Test-Path $s)) { continue }
  New-Item -ItemType Directory -Force -Path (Split-Path $d -Parent) | Out-Null
  Copy-Item $s $d -Force
}
```

导出后立刻 `build.ps1` 确认兼容（库的 API 会变，例如 `AddTab` 加了第 4 个参数）。

---

## 10. 环境事实

- Visual Studio 18（MSBuild 18.9.1），Windows 11 25H2（build 26200），192 DPI。
- 窗口：1200×760 DIP（2400×1520 px @192DPI），最小 720×480。
- 用户目录 `C:\Users\25342`（38 项）。
- 沙箱限制：在 DSH 里派生的子进程有时只能写 `C:\project`；**测试请用 `C:\project` 下的路径**。
- 有时会残留杀不掉的 `ZExplorer` 进程（`Access denied`），会锁住 exe 导致链接失败；
  此时用任务管理器结束，或先编到临时 `TargetName` 验证。

---

## 11. 待办 / 建议顺序

1. 多标签页浏览（PLAN P1）、大图标视图、拖放。
2. 设置补：并发上限、默认冲突策略、开机自启。
3. 继续在真实第三方进程上验证全系统句柄扫描；旧项目参考仍为
   `C:\project\pc_easy2\exp`。
4. Inno Setup 安装包（P5）。

> 老项目能力来源：`pc_easy2\exp\main.cpp` 是旧文件管理器主体；`widget.cpp/.ui`
> 只是空壳。第一批已迁移属性编辑器与 Zone.Identifier，“打开方式”入口同时补齐。
> 老系列其他可评估能力：`autorunManager`（启动项 + 签名校验）、`powerrun`
> （TrustedInstaller 运行）、`sysOptimizer`。

---

## 12. 踩过的坑（照着躲）

| 坑 | 说明 |
|---|---|
| `mt.exe c1010070` | manifest 的 XML 注释里不能出现 `--`；manifest 要 ASCII |
| 窗口全透明 | `SetBackdrop(Mica, 0x00000000)` 在部分宿主全透明；用 `Backdrop::None` + `ThemeRole::Backdrop` |
| 窗口不显示 | `Window::Create` 不显示，必须再 `Show()` |
| `cl D8036` | `/Fo"dir\"` 的转义引号会吃掉后面的参数，改用正斜杠 |
| 未定义 UNICODE | `FindResourceW`/`RegisterClipboardFormatW` 需要 `/DUNICODE /D_UNICODE` |
| 巨大空白 | `GridLayout` 的 `hPerRow = childHeight/rowSpan`，rowSpan=2 会把行撑高 |
| 树子节点不显示 | 异步 add 之后要 `SetLazyLoader` |
| 图标不显示 | `TableView::SetItem(row,col,Label)` 只读文本，图标要用 `SetCellLabelProvider` |
| 所有文件夹都显示 C 盘图标 | `ShellIcons` 的 key 要带路径；目录用中性 `"folder"` 探针 |
| 文字/背景错位 | 自绘坐标是控件相对坐标，`Columns()` 曾返回绝对 x |
| 面包屑过早省略 | 要用 `label->GetEffectiveFontSpec()` 量宽，不是 `th.font` |
| hover 变深再变浅 | 颜色不能用 `Color(0,0,0,0)`（透明黑）插值，要用 `BlendOver` |
| 行拼接 | `//` 注释**不能以 `\` 结尾**（会把下一行拼进注释） |
| `std::min/max` 编译错误 | pch 里 `windows.h` 定义了宏，用 `(std::min)(...)` |
| 自检截图是旧路径 | UI 线程 `Sleep` 会饿死 `RunAsync` 回调，改用计时器状态机 |
| 进程杀不掉 | 用 `taskkill /F` 也不行时（Access denied），只能让用户手动结束或重启 |
| `SetUserContent` | 会把 MessageBox 的按钮整排清掉（库约定），要自建对话框 |

---

## 13. 交接检查清单

- [ ] `build.ps1` 全绿、`--selftest` PASS
- [ ] 读一遍 `docs/STATUS.md` 最近 5 条记录（了解当前进度）
- [ ] 读 `docs/LIBRARY_FEEDBACK.md`（避免重复踩库的坑）
- [ ] 确认 `third_party/ZufyUI` 是否为最新（用户会说"库更新了"）
- [ ] 改代码前先 `read`，用 `edit` 的唯一锚点
- [ ] 每次交付前自问：空列表、失败、取消、重复点击、特殊路径 想过了吗
