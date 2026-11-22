# 状态

最后更新：2026-11-22 · 阶段：**v0.1.0 发布** · 构建：绿（282 单卷 / 320 跨卷）

## v0.1.0 发布收口（2026-11-22）

- 新增 `VERSION`、`CHANGELOG.md` 和 `docs/RELEASING.md`，版本与 `app.rc`
  由打包脚本自动校验。
- 新增 `tools/package_release.ps1`：只打包主程序、两个诊断工具、README、
  CHANGELOG、许可证和第三方声明，并生成 SHA-256。
- 新增 GitHub Actions：
  - `ci.yml`：`main` 推送、Pull Request 和手动触发时构建并跑平台测试；
  - `release.yml`：`v*` 标签校验版本、构建、测试、打包并发布 GitHub Release。
- 本地发布验收：
  - x64 Release 构建通过；
  - 单卷平台测试 **282 passed, 0 failed**；
  - 跨卷平台测试 **320 passed, 0 failed**；
  - 跨卷链接样例 **23 checks, 0 failures**；
  - 发布包 SHA-256 与清单一致。

## 设置持久化与系统集成（2026-11-21）

### 回收站 Explorer 规格页（18:10）

- 删除临时 `Label` 列表实现，改为常驻 `TableView`；右键不再经过已失效的临时控件命中链。
- 列为：名称、原位置、删除日期、大小、类型；支持多选、框选、勾选、双击和选择统计。
- 项目右键直接构造 Recycle Bin Shell 命名空间的 `IContextMenu`，还原/剪切/删除/属性均使用 Explorer 原生命令。
- `RecycleBinItem` 保存相对 PIDL，并读取 `System.Recycle.DeletedFrom`、`System.Recycle.DateDeleted`、类型和大小。
- 工具栏提供还原选中项、删除选中项、刷新、清空回收站和打开系统回收站。
- 设置页“维护”分组新增“一键重置所有设置”。
- 设置窗口改为按分类使用 `TabView`：启动与浏览、文件列表、任务与后台、系统与维护；
  卡片保留在对应 Tab 内，不再把每个卡片拆成独立页签。
- 设置改为草稿模型：底部“保存并关闭”统一提交；保存失败会重建当前设置页并用
  NotificationDrawer 显示完整错误，不再关闭窗口。
- 回收站面包屑特判 `::recyclebin`，地址栏编辑显示“回收站”并可重新跳转；
  不再生成空的父路径段。
- 导航树懒加载使用与文件列表相同的 `showHiddenFiles`，切换后重载已展开树节点。
- 回收站图标按原始扩展名走 `SHGFI_USEFILEATTRIBUTES`，缓存 key 与图标源一致。
- 顶部新增搜索框：输入后 220ms 自动递归搜索当前目录及子目录；搜索在后台线程，
  使用代际号丢弃过期结果；Enter 强制立即搜索，Esc 清空。
- 搜索框已移到第二层操作栏右侧，第二层分三段排版：左侧新建、中间文件操作、
  右侧查看/搜索/更多。
- 新增第二层操作栏：新建默认文件 + 右侧下拉选择文件/文件夹，剪切/复制/粘贴/
  重命名/删除、查看、更多菜单。
- “更多”菜单提供固定快捷访问、全部选择、全部取消、反向选择、设置、关于。
- 设置保存整体移到后台线程；开关和输入只修改草稿。NotificationDrawer 正确挂为
  设置窗口根布局，避免错误的内容宿主影响悬停/下拉响应。
- 设置 TabView 动画缩短到 80ms，设置根布局由 NotificationDrawer 真正接管，
  修复悬停动画和 ComboBox 弹出卡顿。
- 首页改为响应式 GridLayout：按客户区宽度自动 2/3 列；常用位置放入有最大高度、
  内部滚动的 Expander；驱动器独立成 2/3 列网格，卡片限制最大宽度。
- 首页卡片去掉强调色按钮，标题/路径/容量文字居中；整张 ClickableCard 悬停并点击导航。
- 新建使用 ZufyUI 自带下拉按钮：主键新建文件，箭头下拉选择文件/文件夹，
  选择后写回按钮文字并立即打开创建弹窗。
- 第二行最终布局：新建、选项、分隔符、文件操作、分隔符、自适应搜索框；“查看”与
  更多操作合并到“选项”菜单。
- 首页和左侧快速访问统一排序：用户自定义固定项在最前，再放桌面/下载等系统位置，
  最后回收站；相同路径去重。
- 首页位置卡改为横向结构：左侧图标，右侧加粗大字名称 + 下方对应目录；
  驱动器卡使用相同结构。
- 首页卡片悬停改用主题 `Selection` 冷色高亮；目录文字使用首尾保留、中间 `...`
  的测量式裁剪，避免尾部溢出。
- 删除首页“按窗口宽度自动排为 2–3 列”的说明文字；快捷访问程序固定组首项为
  “此电脑”。驱动器显示可用量和总量。

本轮把 `src/platform/Settings.{h,cpp}` 从“已存字段”接到真实行为：

后续补丁（17:10）：

- 用 `IShellFolder + IContextMenu` 枚举 Explorer 实际生成的菜单，定位到
  `Directory\Background\shell\...\MultiSelectModel=Single` 会让 Shell 丢弃背景菜单项。
- 按老 `RightClickManager` 的扫描范围补齐：`DesktopBackground`、`Directory\Background`、
  `Directory`、`Drive`；显示名读取方式与老项目一致使用 `MUIVerb` / 默认值。
- 背景菜单不再写 `MultiSelectModel`；文件夹项和驱动器项仍保留 `Single`。
- 右键注册覆盖桌面背景、目录背景、目录项、驱动器四种位置。
- `SetShellContextMenu` 逐个位置应用，单个位置失败不会阻止其他位置注册。
- Win+E 改为纯状态机 `platform/WinEKeyState`，只根据钩子收到的事件维护 Win/Ctrl/Alt
  状态，不再查询可能残留的 `GetAsyncKeyState`；E 仍被拦截，但真实 Win-up 必须放行。
- Win-up 前注入 Left Ctrl down/up 作为 Menu Mask，阻止开始菜单弹出；这样系统异步
  Win 状态会归零，不会再让后续 D 变成 Win+D。
- 数值型设置改为 ZufyUI 原生 `NumberBox`：范围、步进、错误态、滚轮、上下/清除按钮齐全。

| 分组 | 已接线 |
|---|---|
| 启动与浏览 | 此电脑 / 上次目录 / 指定路径；环境变量路径；默认排序键与方向；窗口位置尺寸；主分栏；快捷访问分栏；四列宽度；上次目录 |
| 文件列表 | 隐藏/系统文件；隐藏已知扩展名；ShellExecute 启动；仅回收站删除确认可关闭，永久删除始终确认 |
| 任务与后台 | 最大并发、弹出阈值、完成停留、历史保留、错误暂停、错误前置、错误提示音；改并发上限后唤醒等待线程 |
| 系统集成 | 开机启动、资源管理器右键、文件夹默认打开方式、Win+E；全部显式开关且默认关闭 |
| 托盘 | 设置、关于、一键重置所有设置；重置同时撤销自启、Shell 菜单、默认打开方式并清空应用设置与快捷访问固定项 |

可靠性细节：

- 资源管理器右键菜单改为正确结构：父键写 `MUIVerb` / `Icon`，`command` 子键写实际命令；目录背景用 `%V`，目录项用 `%1`。
- “接管文件夹默认打开方式”会先备份当前用户的 Directory/Folder/Drive 打开命令，关闭或重置时恢复原值及 `REG_SZ` / `REG_EXPAND_SZ` 类型。
- 窗口位置按物理屏幕像素保存；宽高按 `Window::Create` 使用的系统 DPI 反算为 DIP；读取时限制最低 720×480 并把离屏坐标回退为系统默认。
- 浏览状态定时检查并在有变化时落盘，不再只依赖关闭信号；自检模式禁止写用户设置。
- 新增无界面控制入口：`ZExplorer.exe --show-settings`、`ZExplorer.exe --show-about`。

验证：

- x64 Release 构建通过，平台测试 **187 passed, 0 failed**。
- 设置往返、自动启动、一键重置、Shell 菜单键结构、默认文件管理器备份/恢复均由临时注册表探针验证并在结束后恢复用户原值。
- 桌面实测设置窗口可滚动、开关/下拉可用，关于页正常；`上次目录` 启动恢复 `C:\project\ZufyUI`；1200×760 窗口在关闭重启后保持。

## 库基线与本次升级

- 上一版快照：`ZufyUI@a4bab7c`（v1.29.2）。
- **当前快照：ZufyUI v1.30.0（`7759891` + 横向菜单行收尾修改）**，
  已按 `git ls-files` 从库工作区重新导出 65 个 tracked 文件到
  `third_party/ZufyUI/`；未带入未跟踪的 `SystemDump/`。
- 这次升级直接解决了 P0 里最难受的两个问题：

| 新 API | 解决了什么 |
|---|---|
| `TableView::SetCellLabelProvider(rows, cols, provider)` | 行按视口虚拟化 + **每格一个真 Label**（可 `SetText`/`SetImage`）→ 文件图标终于能画，且不再需要 5000 行上限 |
| `TableView::SetAsyncCellLabelProvider(...)` | （备用）异步取格内容 |
| `TreeView::SetLazyLoader(loader)` | 展开时才回调加载，顺序正确（先加子节点再建可见列表），不再需要"收起-展开"的土办法 |
| `TreeView::SetNodeLabelProvider(provider)` | 节点 Label 可设 Image → 树的文件夹**真图标** |
| `ListView::SetItemLabelProvider/ SetAsyncLabelProvider` | 单列虚拟列表的同等能力 |
| `TreeView::BeginBatch/EndBatch` | 批量增删公开了（此前 `BeginUpdate/EndUpdate` 是 private） |

## 已完成

- 独立工程 `C:\project\ZExplorer`，**未触碰** `C:\project\ZufyUI` 工作区。
- 工程骨架：`ZExplorer.vcxproj`（C++20、`/bigobj`、`/MT`、PCH、Unicode、x64）、`ZExplorer.slnx`、
  `app.manifest`（asInvoker + longPathAware + ComCtl v6）、`build.ps1`、`.gitignore`、`AGENTS.md`、`PLAN.md`。
- 平台层（无 ZufyUI 依赖、无 PCH、可 headless 测）：
  - `platform/PathUtil`：规范化 / `\\?\` 前缀 / 父目录 / 扩展名 / 大小与时间格式化 / 错误文本
  - `platform/ShellActions`：默认程序打开、属性对话框、驱动器枚举、`RelaunchElevated()`（UAC 重启）
  - `fs/DirectoryModel`：`FindFirstFileExW(LARGE_FETCH)` 快照、自然排序、目录优先、类型名按扩展名缓存
  - `fs/ShellIcons`：`HICON` + 缓存（目录用通用图标，文件默认取自身图标）
- UI 层：
  - `ui/FileListView`（151 行）：`TableView` + `SetCellLabelProvider` 四列（名称/大小/类型/修改日期），
    名称列带图标；按扩展名缓存 Image（`.exe/.lnk/.ico` 按路径缓存，上限 500）。
  - `ui/ExplorerPane`：工具栏（后退/前进/上一级/地址栏 TextBox/刷新/提权）、左树（懒加载 + 文件夹图标 +
    占位子节点显示展开箭头）、提示条、列表；目录读取走 `RunAsync` 并用**代际号丢弃过期结果**。
  - `app/ShellWindow`：自定义标题栏 + 状态栏 + 单页内容；包含 `--selftest`（含**自截图**）。
- 测试：`tests/platform_tests.cpp`（37 断言），`tools/run_tests.ps1` 单独编译（**不链接 ZufyUI**，
  以此证明分层规则），已接入 `build.ps1` 第 4 步。

## 验证手段（重要）

`ZExplorer.exe --selftest=<report.txt>`：
1. 起窗口、跑一次真实目录加载；
2. **应用自己截自己的窗口**（`PrintWindow(PW_RENDERFULLCONTENT)`，失败退回本窗口矩形 `BitBlt`），
   存 `<report>.png`；
3. 把控件树、数据统计、DPI、每个关键元素的 DIP 矩形写进报告，然后自动退出。

这样验收**不需要外部截屏**，也不会碰到用户桌面上的其它窗口（此前外部截屏误抓到过用户的在线课堂画面）。

## 本次踩到并已解决的坑

1. **manifest 让链接失败**：`mt.exe` 报 `c1010070` —— XML 注释里不能出现 `--`（我写了 `(--elevated)`）；
   另外 manifest 含中文且无 BOM 也会解析失败，已改为纯 ASCII。
2. **窗口全透明**：`SetBackdrop(Backdrop::Mica, 0x00000000)` 在宿主背景不可用时得到全透明窗口，
   改为 `Backdrop::None` + 主题底色。
3. **窗口不显示**：`Window::Create` 不显示窗口，必须显式 `Show()`（官方 demo 亦如此）。
4. **`/Fo` 尾部反斜杠转义引号**：`cl` 报 D8036，目录改正斜杠。
5. **列表上方巨大空白**：`GridLayout` 的 `rowSpan` 会把子元素高度**均摊**给所跨的行
   （`hPerRow = childHeight / rowSpan`），我用它让树跨行 → 提示条那一行凭空涨了一半高度。
   改为嵌套 `ColumnBox`，空白归零。
6. **树不显示子节点**：先 `ExpandNode` 再异步 `AddChild` 不会重画；现在用库的 `SetLazyLoader`
   （回调发生在 `BuildVisibleList` 之前），顺序天然正确。
7. **自绘列表的教训**：曾因 TableView 不画富单元格 Image 而手写了一个行绘制控件（约 260 行）。
   用户指出这是"把框架的活搬回应用"（滚动/命中/键盘/无障碍全要自己养）。库更新后**已全部删除**，
   `FileListView.cpp` 从 430 行降到 151 行。

## 界面第三轮（2026-11-15 傍晚）

- **左栏折叠是"空间不足才折"**：去掉启动时的 `CollapseFirst()`，改为
  `SetAutoCollapseOnInsufficientSpace(true)` + `SetInsufficientSpaceDefaultSide(2)`（保留右侧列表）。
- **面包屑只做"每个方框各自省略"**：删掉"整条超过 3 段就折叠成 `…`"的逻辑（那是我理解错了），
  现在所有段都显示，单段超 180 DIP 时在自己内部做"首…尾"省略。
- **段宽按控件自身字体量**：早先用 `th.font` 量，比 Label 实际字体窄 → 框偏窄被二次省略成 `253…`。
- **按钮底色改用主题 `ThemeRole::Selection`**（可用态淡蓝，悬停/按下在同色上压深）。
- **失焦恢复面包屑**：库的 `TextBox` 不会因为"点到别的控件"收到 `OnBlur`，改为监听
  `UIZSignals::GlobalMouseDown`，点在地址栏矩形外就主动 `Blur()`。
- 自检扩充到 6 步（正常 → 编辑 → 编辑截图 → 模拟外部点击 → 失焦截图 → 深路径截图），
  并输出三段状态，**不再在 UI 线程 Sleep**（那会把投递到 UI 线程的回调饿死）。

### 本轮验证

```
地址栏状态 : crumb visible=0, edit focused=1, segments=8
失焦状态   : crumb visible=1, edit focused=0, segments=8     ← 点外面能恢复
深路径状态 : crumb visible=1, edit focused=0, segments=14    ← 六段全显示，无整条折叠
结果       : PASS
```

## 第一批真功能：文件操作（2026-11-15 晚）

新增 `platform/FileOps`（纯 Win32、无 PCH、可 headless 测）：

| 能力 | 实现要点 |
|---|---|
| `CopyTree` / `MoveTree` | 递归；保留时间戳；`\\?\` 长路径；同卷移动走 `MoveFileW`，跨卷退化为复制+删除；带进度回调，返回 false 即取消 |
| `DeleteToRecycleBin` | `SHFileOperationW` + `FOF_ALLOWUNDO`（进回收站） |
| `DeletePermanent` | 递归删除，自动去只读属性 |
| `RenamePath` / `CreateDirectoryPath` / `CreateEmptyFile` | —— |
| `MakeUniquePath` | 冲突时生成"名称 (2).ext" |
| `SetClipboardFiles` / `GetClipboardFiles` | `CF_HDROP` + `Preferred DropEffect` → **与资源管理器双向互通**（本程序 Ctrl+C，资源管理器 Ctrl+V 可粘贴） |

UI 接线：
- 右键菜单（`SetContextMenuFactory`）：打开 / 复制 / 剪切 / 重命名 / 删除 / 永久删除 / 属性 /（空白处）粘贴、新建文件夹、新建文本文档、刷新
- 快捷键（`Window::AddKeyHandler`）：F2 重命名、F5 刷新、Delete 回收站、Shift+Delete 永久删除、Ctrl+C/X/V、Backspace 上级
- 重命名用 `MessageBox` + `SetUserContent(TextBox)` 组了个输入框（库没有 InputDialog）

测试：`[FileOps]` 一组用例覆盖 新建/复制/取消/重命名/永久删除，**平台层共 54 条断言全过**。

### 已知缺口（下一轮）

1. **复制/粘贴是同步的** —— 大文件会卡住 UI。下一步：`RunAsync` + 进度对话框 + 取消。
2. **没有多选** —— `TableView` 只有 Cell/Row/Column/None 四种 SelectionMode，没有 Extended/Multi（`ListView` 有）。操作目前只作用于"当前选中一项"。
3. 冲突策略只有"自动改名"，还没有覆盖/跳过/全部应用。
4. 拖放（拖入/拖出）未接。

## 架构升级：后台进程（2026-11-15 夜）

**所有文件操作移出 UI 进程**。**同一个 exe 两种角色**：

```
ZExplorer.exe            前台 UI
ZExplorer.exe --host     后台（托盘 + 任务队列 + 管道服务），由前台按需拉起
```

```
ZExplorer.exe (UI)                        ZExplorer.exe --host (后台)
  ExplorerPane ──SubmitTask──▶ 命名管道 ──▶ PipeServer ──▶ TaskManager(worker)
                                              ├─ 托盘图标
                                              └─ 每个任务一个进度窗
```

- 启动流程：UI 先 `TaskClient::EnsureHost()` —— 管道不在就 `CreateProcess(自己, "--host")`，
  最多等 3 秒到管道就绪；后台自己也会做单实例判断（已在跑就直接退出）。
- 协议：`src/ipc/Protocol.h`（uint32 type + uint32 bytes + UTF-16 参数表）
- 管道：`src/ipc/Pipe.h/.cpp`（一次连接一次请求/响应，`\\.\pipe\ZExplorer.Host`）
- 客户端：`src/app/TaskClient`（`Submit / Query / ShowTask / CancelTask / EnsureHost`）
- 后台：`src/host/HostApp` —— 任务队列 + worker 线程 + 托盘（显示任务窗口 / 退出）
  + **每个任务一个进度窗口**（标题栏图标用源项目的 Shell 图标，标题 `ZExplorer — <操作>：<名字>`，
  进度条 + 当前文件 + 取消按钮；`Closing` 时改成隐藏，任务继续）
- UI 侧操作全部改为 `app::TaskClient::Submit(...)`：复制/剪切（写剪贴板也在后台做）、粘贴、
  删除/永久删除、重命名、新建文件夹/文件。提交后 700ms 自动刷新目录。

> 早先我把它做成了两个 exe（`ZExplorer.Host.exe`），按用户指正改回**同一个文件**，
> 独立的 `ZExplorer.Host.vcxproj` 与 `src/host/main.cpp` 已删除。

### 端到端验证（`tools/ipc_probe.ps1` 直接发协议帧）

```
启动前  pipe=False
启动 UI pipe=True              ← UI 自动拉起了自己的 --host 实例
submit newfolder : 1 | 1  created : True
submit newfile   : 1 | 2  file created : True
tasks: 1 Done 新建文件夹 / 2 Done 新建文件
```

> 注：本开发环境里派生进程只有 `C:\project` 可写（写 `%TEMP%`/`Documents` 会 ACCESS_DENIED），
> 这是沙箱限制；上面用 `C:\project` 下的路径验证了完整链路。

### 还没做（下一轮）

1. **总体进度条 + 下拉任务列表**（主窗口右侧）
2. **多选**（`TableView` 无 Extended/Multi 模式）
3. 后台的**设置窗口**
4. 任务完成时 UI 主动刷新（现在固定 700ms 刷一次）

## 多选（2026-11-15 夜，更正）

> 我之前说"`TableView` 没有多选"是**错的**：只看了 `SelectionMode` 枚举（只有 Cell/Row/Column/None）
> 就下结论。实际上 `TableView` 的多选走的是另一条路：

| 能力 | API |
|---|---|
| 框选 | `SetMarqueeEnabled(true)` |
| 勾选 | `SetCheckable(true)` / `SetRowChecked` / `GetCheckedRows` |
| 两者同步 | `SetMarqueeCheckSync(true)`（框选会带上勾选状态） |
| 取选中 | `GetSelectedRows()` / `GetSelectedCells()` |
| 变更通知 | `SelectionChangedCells` / `ItemCheckStateChanged` |

接线：
- `ui/FileListView` 里三个开关全开；`SelectedRows()` 返回 **框选 ∪ 勾选**（去重升序）
- `ui/ExplorerPane::SelectedPaths()` 把行号映射成完整路径；空时回退"当前行"
- 复制/剪切/删除/永久删除 都改成**多选批量**提交给后台（确认框显示"这 N 项"）
- 重命名仍是单项操作；**树只用于查看与导航，不参与操作**

## 交互补全（2026-11-15 深夜）

- **右键菜单补全**（所有操作都在菜单里，快捷键是同一批函数的别名）：
  - 选中项：打开 / 复制 / 剪切 / 粘贴 / 重命名 / 删除 / 永久删除 / 全选 / 属性
  - 空白处：粘贴 / 新建文件夹 / 新建文本文档 / 全选 / 刷新
- **快捷键全集**：Enter 打开、Alt+Enter 属性、F2 重命名、F5 刷新、Delete / Shift+Delete、
  Ctrl+C / X / V / A、Ctrl+Shift+N 新建文件夹、Backspace 上级
- **主窗口也加了托盘**（`TrayIcon`：单击/双击显示主窗口，菜单：显示主窗口 / 退出）
- **任务窗口延迟弹出**：后台任务在 0.35 秒后才建窗口；**0.35 秒内跑完的完全不弹**
  （失败的任务仍然弹，否则错误会被吞掉）
- **新建先问名字**：新建文件夹 / 文本文档都会先弹输入框（`MessageBox + TextBox`，与重命名共用
  `PromptName`），确认后才把任务发给后台
- 自检增强：`Ping` 回复里带上**当前任务窗口数**，用来验证"快速任务不弹窗"

### 验证

```
ping before : 1 | pong | 0
submit      : 1 | 1
created     : True
ping after  : 1 | pong | 0     ← 快速任务完成，窗口数仍为 0（没有打扰用户）
```

## 一次性补齐（2026-11-16 凌晨）

| 需求 | 实现 |
|---|---|
| 新建要"确定/取消" | `PromptName` 用 `MessageBox(OK\|Cancel) + SetUserContent(TextBox)`，正文 "请输入名称："（之前正文为空，按钮区看着像没有） |
| 勾选 ↔ 选中同步（含拖动） | 拖动由 `SetMarqueeCheckSync(true)`；点击/键盘由 `SelectionChangedCells` → `SetRowChecked(row,true)` 补上 |
| 右键菜单不分开 | **一个统一菜单**：打开/属性/复制/剪切/粘贴/重命名/删除/永久删除/新建文件夹/新建文本文档/全选/刷新；没选中时把"选中相关"项 `enabled=false` 置灰（永久删除标 `danger`） |
| 弹窗完成自动关闭 | 任务窗口完成后再停 0.8 秒自动关闭；`autoClose` 标志让 `Closing` 放行（平时手动关闭仍只是隐藏） |
| 总体进度条 + 下拉 | 工具栏右侧：`ProgressBar`（运行中任务的平均进度）+「任务 N」按钮；点开 `Menu` 列出每个任务 `标题 42% 状态`，点条目调 `ShowTask(id)` |
| 任务完成反向刷新 | 600ms 轮询 `QueryTasks`，`pending` 从 >0 变 0 时自动 `Refresh()` |
| 设置窗口占位 | 后台托盘菜单「设置…」打开一个 460×260 空壳窗口，里面写明以后要放哪些项 |

验证：`build.ps1` 全绿（54 断言），`--selftest` PASS，工具栏几何未变（进度条/任务按钮在无任务时隐藏）。

## 文件操作细化（2026-11-16 凌晨 2）

- **平台层改为"逐项继续 + 错误清单"**：`OpResult { canceled, okCount, issues[] }`。
  单个文件失败只记一条 `OpIssue{path,message}` 并继续，不再整批中断。
- **后台并发**：每个任务一个线程（不再排队）；任务状态 Done / Failed / Canceled 三态。
- **任务窗口（详细）**：标题 + 当前文件 + 进度条 + `已完成/总数 · 速度` +
  **错误清单列表**（`文件名 — 原因`）+「重试失败项」+「取消」。
- **完成自动关闭**（0.8s），失败不自动关（要让人看到错误）。
- **UI 任务按钮**：`TaskProgressButton`（`Button` 子类，自绘底部进度条），固定 96×32 常驻；
  有任务时 `任务 N` + 进度，无任务时就是普通按钮；点击弹出任务下拉。
- **下拉菜单**：不显示已完成任务；失败项前加 `⚠` 并带"失败 N 项：原因"；点条目打开对应任务窗。
- **多选卡死修复**：`SelectionChangedCells` 只对**单行**做勾选同步（`TableView` 没有批量接口，
  逐行 `SetRowChecked` 是 O(n²)），大面积选择交给 `SetMarqueeCheckSync`。

### 崩溃（未定位，如实记录）

19:49 与 19:43 各有一份 dump：
`%LOCALAPPDATA%\CrashDumps\ZExplorer2.exe.{516,15816}.dmp`，
以及 `x64\Release\ZExplorer.exe.12672.dmp`（498 MB，来自那个杀不掉的实例）。

离线解析（无符号）得到：

```
Access violation - code c0000005
mov rbx,qword ptr [rax+rbx*8+8]     rax=0000000000000000  rbx=6
#0 ZExplorer+0x25cf5  ← #1 +0x26875  ← #2 +0x24f14  ← ... ← BaseThreadInitThunk
```

= 空指针 + 下标 6 的数组访问，发生在**线程入口**起的调用链上。
**Release 构建默认不带 PDB，栈无法符号化**，所以结论是「**未定位**」——不做猜测。
为下一次能定位，已把 Relase 的 `GenerateDebugInformation` 打开（vcxproj）。

### 已知问题（需要你动手）

## 卡死根因与重构（2026-11-16 凌晨 3）

### 根因（两个，都是跨线程）

1. **关任务窗口会卡住前台**
   管道线程里直接执行 `win->Show() / ShowActivate()` —— 那是 UI 对象。
   同时前台 UI 线程正在 `CallHost()` 同步等回复；后台 UI 线程若在忙（或窗口正在响应），
   两边互等 → 前台卡死。

2. **任务多时 UI 卡**
   前台每 600ms 在 **UI 线程** 上同步 `QueryTasks`；后台把 N 个任务的字符串全量序列化，
   任务越多越慢，UI 线程卡在 `ReadFile` 上。

### 修复（结构性，不是加延时）

**前台 `app/TaskClient`**
- 新增独立 **IO 线程**：所有 `CallHost`（提交/查询/显示/取消）都在这条线程上执行。
- 提交 = 入队即返回；查询 = 读一份内存快照（`mutex` + 拷贝），**UI 线程零阻塞**。
- 快照由 IO 线程每 250ms 刷新一次。

**后台 `host/HostApp`**
- 管道线程**不再碰任何 UI 对象**：`ShowTask` 变成 `PostUi(lambda)` 入队，
  由 UI 线程的 100ms 泵 `DrainUi()` 执行。
- `QueryTasks` 只返回 `snapCache`（泵每拍重建一次），请求路径上没有逐任务加锁与格式化。

### 验证

```
build.ps1        -> 53 断言全过
--selftest       -> PASS
连发任务后 ping  -> 1 | pong | 0 | 0   （后台仍可响应）
```

## 任务窗口改为单窗口 + 页签（2026-11-16 凌晨 4）

每个任务开一个窗口太占显存，改成**一个窗口 + 每个任务一个页签**：

- `std::shared_ptr<Window> taskWin` + `TabView tabs`，全局只创建一次，没有页签时 `Hide()`（不销毁，避免反复建 DComp 设备）
- 每个页签内容：当前文件、进度条、`已完成/总数 · 速度`、错误清单、重试失败项、取消
- 页签可手动关闭（`TabCloseRequested`）；**任务本身继续跑**
- 完成后页签 1.2 秒自动收起；收起完最后一个页签 → 窗口隐藏
- 关窗口仍是隐藏，不影响任务
- 0.35 秒规则不变：运行超过 0.35 秒才建页签，快速任务完全不打扰

### 验证

```
build.ps1        -> 53 断言全过
400MB 单文件复制 -> 后台执行完成，前台进程存活、可响应
```

> 页签窗口没能在 1.5 秒的采样点上抓到（400MB 在本机 SSD 上比 0.35 秒还快，
> 抓到时窗口已经收起），这条需要人工确认一次视觉效果。

### 仍未做

1. 冲突策略（覆盖/跳过/全部应用）
2. 每个文件的独立进度行
3. 并发上限（现在每任务一线程）

## 待上报给库（ZufyUI）的问题

1. **`GridLayout` 的 `rowSpan` 语义容易踩**：跨行元素会抬高它跨过的每一行。建议 `pitfalls.md` 补一条。
2. **`TreeView` 没有"是否有子节点"的 API**：现在只能用"占位子节点"换展开箭头，
   代价是空文件夹也会显示 ▶。建议加 `SetNodeHasChildren(node, bool)` 或让 `SetLazyLoader` 隐含可展开。
3. **`Window::Create` 不显示窗口**：README 最小示例建议点明需要 `Show()`。
4. `docs/pitfalls.md` 4.11 已过期（`Window::SizeChanged`/`DpiChanged` 实际存在）。

## 界面第二轮（2026-11-15 下午）

- **左树与列表之间换成 `SplitView`**：`SetMinFirst(160)` / `SetMinSecond(320)` / 分割条 6 DIP，
  默认 `CollapseFirst()`（左边缘有小把手可展开）。
- **地址栏改成两态**：
  - 未聚焦：显示面包屑 `🏠 › C:\ › Users › [25342]`（当前段是高亮圆角块，段可点击导航，
    超过 3 段时中间折叠成 `…`，点开是菜单；右侧空白点一下进入编辑态）；
  - 聚焦：面包屑整块隐藏，`TextBox` 显示完整路径并**全选**（`SetSelection(0, n)`）。
  - 聚焦/失焦靠 `UIElement::OnFocus/OnBlur` 的钩子（只做事件，不做绘制）。
- **左侧三个导航按钮统一成同族箭头** `‹ › ˄`（字体里没有"粗的向上箭头"，只有 ChevronUp）。
- 自检现在会拍**两张**图：正常态与地址栏编辑态，并输出 `地址栏状态: crumb visible=0, edit focused=1`。

### 这一轮踩到的坑

- **`Button::SetColors()` 会把字形的颜色一起改掉**：设了透明底之后图标直接看不见
  （`SetColors(transparent, hover, pressed)` 后必须补 `SetIconColor(主题文字色)`）。
  一开始我以为是图标字体缺 `E70E`/`E72C`，用 GDI `GetGlyphIndicesW` 验证过两个字体都**有**这些码点，
  才回头怀疑着色。

## 下一步

1. 表头点击排序（`TableView::HeaderClicked` 已可用）；
2. 地址栏面包屑 + 下拉历史；
3. 写操作（剪切/复制/粘贴/删除/重命名/新建）与操作队列；
4. 树补「快速访问」固定项（桌面/下载/文档）。

## 并发/冲突/每文件进度（2026-11-16 凌晨 5）

- **并发上限**：同时最多 4 个任务，其余等空位（`slotCv`，退出时唤醒）
- **冲突策略**：`CopyOptions::onConflict` -> 后台弹非模态对话框（覆盖/跳过/全部覆盖/全部跳过/取消任务），
  worker 轮询等答案，UI 线程不被阻塞；`CopyCtx::policy` 记住"全部"决策
- **每文件进度**：分块复制（1 MB）+ `FileEventFn` -> 任务页签里的文件列表，`✔ 完成 / ✖ 失败 / … 45%`
- **修掉一个真 bug**：`RunTask` 里的 `MakeUniquePath` 会把同名文件自动改名，
  导致冲突策略永远不触发；改为直接拼目标路径
- **修掉一个真 bug**：0 字节文件不进分块循环，取消对它无效；现在每个条目开始前都问一次

### 验证

```
53 断言全过 / build.ps1 绿 / selftest PASS
冲突路径：提交同名复制 -> ping 显示 页签=1 进行中=1（任务停在对话框等待）
```

### 未解决

- `CancelTask` 对这个等待中的任务返回"没有这个任务"（`FindTask` 没找到 id=1），
  但 QueryTasks 列表里明明有 id=1。**未定位**，下一步查。

## 修掉"取消找不到任务"（2026-11-16 凌晨 6）

**根因：后台起了两个实例。**

- `RunHostMode()` 只用 `HostAlive()`（管道是否存在）判断单实例，有竞态：
  两个进程可能同时通过检查，然后各自 `CreateNamedPipe` 成功
  （`PIPE_UNLIMITED_INSTANCES`），同一个管道名后面挂了两个后台。
- 客户端连到哪个实例是随机的：提交进了 A，取消被 B 处理 -> B 的 `tasks` 是空的，
  于是回 "没有这个任务"。
- 诊断输出直接暴露了这一点：`id=1 | count=0 | have=`，而同时 `QueryTasks` 能列出该任务。

**修复**：改用命名互斥体做单实例

```cpp
static HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\ZExplorer.Host.Singleton");
if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
```

**顺带修**：冲突等待循环没看 `cancel` 标志，取消后 worker 仍卡在对话框上。
现在 `RequestConflict(task, ...)` 在等待中检查 `task->cancel`，命中就收起对话框并按"跳过"返回。

### 验证

```
启动 UI            -> 进程数 2（UI + 1 个后台）
同名复制(冲突)      -> 页签=1 进行中=1（停在对话框）
cancel             -> 返回 1
任务状态            -> 4（已取消）
目标文件内容        -> existing（没有偷偷覆盖）
build.ps1          -> 53 断言全过
--selftest         -> PASS
```

## 崩溃定位 + 首页设计（2026-11-16 凌晨 7）

### 崩溃根因（带符号定位，非猜测）

`x64\Release\ZExplorer.exe.21916.dmp`，Release 已开 PDB：

```
Access violation c0000005
  ZufyUI::ListView::Clear+0xe        ← this == nullptr (rcx=0)
  ze::host::HostApp::Impl::UpdateTabs+0x6c4
```

反汇编定位到调用点 `mov rcx,[r12+48h]; call ListView::Clear`。`TaskTab` 里
`errors` 字段偏移正是 `0x48`——我之前把"错误列表"改成卡片时**删掉了 ListView 的创建，
却漏掉了 UpdateTabs 里的 `tab->errors->Clear()`**，于是每拍都拿空指针清列表，后台当场死。

修复：删掉残余的 `tab->errors` 使用；详情页签的错误改成卡片渲染。

### 任务窗改版

- **首页（不可关闭）**：`操作` 页签，滚动区域里是各个操作的卡片
  - 进行中优先排序（进行中 → 排队 → 失败 → 取消 → 完成）
  - 卡片：操作描述（`复制：a.txt → D:\dst`）+ 进度条
  - 有失败项：进度条下加"该操作需要引起注意（N 项失败）" + `查看` 按钮（切到对应页签并前置窗口）
- **没有任务时不再隐藏窗口**：停在首页（用户明确要求"不能直接关掉"）
- 页签索引统一加首页偏移（关闭/完成自动收起/取消 三处）
- 详情页签错误改为卡片：左侧操作与错误说明，右侧 `重试` / `忽略`
- 后台托盘「打开新的操作窗口」= 启动一个新的**主 UI 进程**（不带 `--host`），不是任务窗

### 验证

```
锁定文件的复制 -> 任务 state=3，1 项失败，消息完整
host alive     -> True（错误路径不再杀后台）
主 UI 启动      -> 进程数 2（UI + 单实例 host）
build.ps1      -> 53 断言全过
--selftest     -> PASS
```

## 设置语义修正（2026-11-16 凌晨 8）

用户指出："遇到同名文件时询问不就是冲突吗" —— 对的，我多开了一个开关。已合并：

- `发生错误时暂停等待处理` **开** -> 冲突弹窗等待；**关**（默认）-> 不弹窗，按「默认冲突策略」处理
- 新增设置：`默认冲突策略（不询问时）` = 覆盖 / 跳过（默认**跳过**，避免静默覆盖用户数据）
- 被跳过的同名文件会**留痕**：`目标已存在同名文件，已跳过` 进错误清单，首页卡片显示"需要引起注意"，可点重试
- `暂停等待` 现在真的会暂停：`onFile(Failed)` 里阻塞该任务的 worker，直到用户重试/忽略或取消任务

### 验证

```
提交同名复制（暂停关闭）
tasks  : 1  3  复制：a.txt   1  目标已存在同名文件，已跳过
dst    : existing            <- 没有偷偷覆盖
host   : True
build  : 53 断言全过
```

## 删掉冲突弹窗（2026-11-16 凌晨 9）—— 我的理解错误

用户要的是：**任务页签里，进度条下面一个滚动区域，每个失败文件一张卡片，右侧给动作**。
我却自己加了一个 `覆盖/跳过/全部覆盖/全部跳过/取消任务` 的模态弹窗，这是我擅自加的，语义完全不同。

已删除：
- `ShowConflictDialog` / `RequestConflict` / `conflictWin` 全部移除
- `CopyOptions::onConflict` 现在固定返回 `Skip`，**永远不等待任何确认**
- 冲突不再单独设"默认策略"设置项

现在的行为：
- 目标同名 -> 跳过该文件，记一条 `OpIssue{kind=1, "目标已存在同名文件"}`
- 任务页签滚动区域里生成一张卡片：左侧 `复制：<路径>` + 错误说明，右侧动作
  - `kind=1`（同名冲突）-> **覆盖** / 忽略
  - 其他错误 -> **重试** / 忽略
- `覆盖` 会以 `copyover` 操作重新提交这一个文件（强制覆盖），不会重复整批

### 验证（真实管道调用）

```
1) copy      -> dst = existing   任务 state=3，错误清单 1 条："目标已存在同名文件"
2) copyover  -> dst = hello      任务 state=2（完成）
3) host alive -> True
```

## 关页签崩溃：在事件回调里删除自己（2026-11-16 凌晨 10）

用户定位准确：`TabCloseRequested` 是 `TabView` 在**自己的点击处理里**发出的，
我在回调里直接 `RemoveTab(index)`，把"正在处理点击的那个页签对象"销毁了 -> `c0000005`。
取消按钮同理：在 Button 的 `Clicked` 回调里销毁按钮自己。

修复：所有删除统一走 `RemoveTaskTab(taskId)`，并且**只在 UI 队列里执行**：

```cpp
tabs->TabCloseRequested.connect([this](int index) {
    const int taskId = ...;
    if (auto task = FindTask(taskId)) task->cancel = true;
    PostUi([this, taskId]() { RemoveTaskTab(taskId); });   // 事件派发结束后再删
});
```

- 首页卡片的「查看」只新增页签（不销毁），保持同步
- 卡片上的重试/忽略只改数据，重建发生在下一拍泵里（此时不在事件回调中）

> 用户给的 `ZExplorer.exe.7896.dmp` 无法符号化：exe 校验和不匹配（dump 来自改动前的二进制），
> 所以这里以代码结构确认修复，不做栈断言。

### 验证

```
build.ps1 -> 53 断言全过；exe 1370 KB
```

## 库反馈（ZufyUI）：这次踩到的、值得改的（2026-11-16）

### 1. 销毁"正在派发事件的元素"没有任何保护

`TabView` 在自己的点击处理里同步发 `TabCloseRequested(i)`；我照常理在回调里
`tabs->RemoveTab(i)`，就把正在处理这次点击的页签销毁了 -> `c0000005`。
`Button::Clicked` 里销毁按钮自己同理。

建议：`ZSignal::Fire` 期间对监听者做一次"自毁保护"（延迟析构/引用计数），
或在 `RemoveTab`/`RemoveChild` 里检测"正在派发"并推迟到派发结束。

**复现**：`Tabs->TabCloseRequested.connect([&](int i){ tabs->RemoveTab(i); })`，点 × 即崩。

### 2. 列表只能整体重建，没有增量接口

`ColumnBox`/`StackPanel` 只有 `ClearChildren()`，**没有 `RemoveChild`**；
`TableView` 没有 `BeginUpdate/EndUpdate`（`ListView` 有，`TableView` 没有）。

后果：卡片列表"忽略一项"只能全清重建。418 项时每次重建 ~2500 个元素，
UI 直接卡死，内存冲到 800MB。

建议：`RemoveChild(index)`、`TableView::BeginBatch/EndBatch`。

### 3. `TabView` 没有"按 id 找页签"

只有 `RemoveTab(index)`，没有 `IndexOfTab(id)` 或稳定的 tab handle，
业务侧只能自己维护 index 映射（首页固定页签后，三处索引都要加偏移，很容易错）。

### 4. `Card` 是单子元素容器

`Card` 只有 `SetContent()`，没有 `AddChild`；做"标题+说明+按钮行"需要再套一层 `ColumnBox`。

## 我自己的问题（不是库的锅）

- `ShowTask` 在管道线程里直接调 `win->Show()` —— 跨线程动 UI，卡死前台
- `UpdateTabs` 里的 `tab->errors->Clear()` 引用了早已不创建的字段 —— 空指针崩溃
- 用 PowerShell 正则/行号拼接改源码，静默失败和切坏大括号 —— 后面一律用 edit 工具 + 唯一锚点

## 冲突卡片细化（2026-11-16 中午）

- `OpIssue` 增加 `target` 字段：冲突卡片能同时显示**源文件**和**目标文件**完整路径
- 卡片结构：`[文件名按钮]` + `操作：粘贴` + `源文件：...` + `目标文件：...` + 错误说明 + 动作行
  - 文件名按钮点击 -> `explorer /select,<path>` 开一个新窗口并选中该文件
- 冲突卡片动作：`覆盖` / `跳过` / `全部覆盖` / `全部跳过`
  - `全部覆盖` / `全部跳过` 只作用于**本任务**：设置 `Task::conflictPolicy`(1/2)，
    并立刻 `RemoveIssuesByKind(1)` 把列表里同类卡片一起清掉（已经做了决定就不算错误）
  - 之后的冲突不再产生卡片：`onConflict` 直接按 policy 返回 Overwrite/Skip
- 卡片一律**按路径**匹配（不再用下标），只追加不重建

### 验证

```
5 个同名冲突 -> state=3，5 条 issue，页签 1 个
host 内存 149.8 MB，进程存活
build.ps1 -> 53 断言全过，exe 1380 KB
```

> 图标没加：按用户给的另一条路走了「文件名变成按钮、点击开新窗口」。

## 任务窗重写（2026-11-15 深夜）—— 新库 + 新模块

### 库已更新并重新导出

`C:\project\ZufyUI` 工作区（未提交）-> `third_party/ZufyUI`，46 个文件。
新增的关键接口：

```cpp
void RemoveChild(const std::shared_ptr<UIElement>& child);   // ZufyUI.h:2599
void RemoveChildAt(int index);
void InsertChild(int index, std::shared_ptr<UIElement> child);
```

这就是"删一张卡片不用全量重建"的基础。

### 新模块 `src/host/TaskWindow.{h,cpp}`（418 行，从零写）

- `struct TaskItem`：纯数据快照（id/op/state/ratio/issues/elapsedMs...），HostApp 只负责填
- `class TaskWindow`：`Update(items)` 每拍刷新，`Hooks` 把 6 个动作回传给 HostApp
- 卡片**增量**：`AddChild` 新增、`RemoveChild` 删除，**全程没有 ClearChildren 重建**（首页除外，上限 40 张）
- 鼠标按下期间不做结构改动；页签关闭走 `pendingRemove_`，下一拍才真正删除
- 首页固定不可关闭；无任务时窗口不消失

### HostApp 里删掉的旧代码

`RemoveTaskTab / TabIndexOf / EnsureTaskWindow / EnsureTab / UpdateHome / UpdateTabs`、
`TaskTab` 结构、`taskWin/tabs/homeScroll/homeList/homeTabIndex/tabsState` 成员
（约 430 行），全部由新模块取代。HostApp 现在只做：执行任务 + IPC + 把快照喂给 TaskWindow。

### 验证

```
build.ps1        -> 53 断言全过，exe 1391 KB
--selftest       -> PASS
3 个同名冲突      -> state=3、3 条 issue、页签 1 个、目标文件保持 o1（跳过）
快速完成的复制    -> 页签 0 个（0.35 秒规则生效）
host 内存        -> 146 MB（有任务窗）/ 53 MB（无任务窗）
```

## 进度与异常完善（2026-11-16）

### 计数（用户报的 0-0）

- 原因：每个源单独跑 `CopyTree`，进度回调把任务计数器重置成"这一个文件"
- 修复：聚合累计 `aggBytes/aggFiles`，`total` 用 `MeasureTreeStats` 预计算，
  `done = 已完成基数 + 本项实际完成量`，**只累加真正成功的字节/文件**
- 解冲突后成功覆盖某个文件 -> `++aggFiles`，进度立刻反映
- `QueryTasks` 增加第 8/9 个字段（doneFiles / totalFiles），方便自检

### 速度

任务页签恢复显示 `N/M 个文件 · X MB/s`。

### 危险场景（平台层，已加断言）

| 场景 | 处理 |
|---|---|
| 目标在源目录内部（`C:\a` -> `C:\a\b`） | **直接拒绝**，否则边复制边枚举会无限递归 |
| 前缀误判（`sub` vs `sub_copy`） | 带分隔符比较，已加测试 |
| 符号链接 / 联接点 | 跳过并记录（避免成环） |
| 目标只读 | 去掉只读属性后覆盖（和资源管理器一致） |
| 回收站批量失败 | 逐个重试归因，错误码翻成人话（占用/拒绝访问/找不到/路径非法） |
| 删除不存在的文件 | 幂等，不报错 |

```
build.ps1 -> 55 断言全过 -> exe 1396 KB
```

### 还可以继续做（未做）

1. 任务页签显示用时 / 剩余时间估算
2. 首页历史卡片补上"完成时间 / 结果"
3. 已完成任务卡片定时回收（长会话内存）

## 交互修正（2026-11-18）

| 用户反馈 | 处理 |
|---|---|
| 复制为什么也显示成一个任务 | Ctrl+C/X 只写剪贴板，**不再建任务**：`SubmitTask` 里 target 为空直接写剪贴板返回 `id=0`。实测：`1 | 0`，任务数 0 |
| 进度更新不够快 | 泵 100ms -> **50ms**；统计行恢复 `N/M 个文件 · X MB/s` |
| 出错/开始复制要弹窗并前置 | 新页签创建时 `ShowAndFocus()`；状态进入 3(出错)/5(等决策) 时也 `ShowAndFocus()` 并选中该页签 |
| 永久删除的重试/忽略没反应 | 根因：删除类任务**跑完就结束了**，没有待决项，按钮改了决定也没人消费。现在非冲突错误也登记为待决项，任务停在 state=5。实测 `RAW[1 5 ... 删除失败 ... 0x00000020]` |
| 全部覆盖/全部忽略没用 | 同上，`SetAllDecisions` 现在有等待方消费 |
| 卡片对不齐 | 卡片内容 `SetFillWidth(true)`，路径前缀列宽 76 统一 |
| 源文件按钮打开的是默认目录 | 初始路径导航改为**窗口起来后 80ms 再导航**（原来被首屏加载覆盖） |

## 此电脑 / 设置持久化 / 输入对话框（2026-11-18 晚）

### 输入对话框没有按钮（真 bug）

`MessageBox::SetUserContent` 的注释写着：**"SetUserContent 后不再放任何按钮，全部交给用户"**。
所以我那个"重命名/新建"对话框当然只有一个输入框。

改成自己搭的模态窗口（`Window::RunModal`）：标签 + TextBox + **确定 / 取消** 两个按钮。
`DoNewFolder` / `DoNewTextFile` / `DoRename` 全部走 `PromptName`。

### 任务首页"鬼畜"

根因：首页重建的 hash 里带了 `ratio`，进度每变一次就 `ClearChildren` 整页重建。
现在 hash 只含 `id/state/issueCount`，进度条改为**就地更新**（`homeBars_` 映射），不再重建。

统计行改成 `45% · 12/36 个文件 · 3.1 MB/s`，跟着进度条一起动。

### "此电脑"默认页（驱动器磁贴）

- 启动默认导航到 `::thispc`
- 磁贴：`[驱动器名按钮]` + 使用率进度条 + `X 可用，共 Y`
- 点磁贴进入盘符；在盘符根目录按"上一级"回到此电脑
- 平台层 `LogicalDrives()` 补齐 `fsType/totalBytes/freeBytes/ready`

### 设置持久化 + 自动提权

- 新增 `platform/Settings.{h,cpp}`：`HKCU\Software\ZExplorer` 下 `AutoElevate / PauseOnError / FocusOnError`
- 设置窗口三项开关，改动即写入注册表；启动时读取
- `AutoElevate` 打开且当前不是管理员 -> 启动时 `RelaunchElevated()` 提权重启
  （提权后的前台进程再拉起后台，后台继承管理员权限，所有文件操作都在这个后台里跑）

```
build.ps1 -> 55 断言全过 -> exe 1411 KB
--selftest -> PASS
```

### 构建进程与免提权模式（2026-11-20）

- 默认 `build.ps1` 会先结束所有 `ZExplorer` 实例，再更新正式输出。
- 如果实例处于高权限、当前进程无权限结束，则**不触发 UAC**，改用
  `build.ps1 -FalseKill`（等价 `-NoKill`）：保留运行实例，并排输出到
  `tmp_fast/no-kill-<时间>/`，同时照常运行平台测试。

## 退出崩溃 / 开关控件 / 应用图标（2026-11-18 夜）

### 托盘"退出"崩溃（库的静态析构顺序）

dump `ZExplorer.exe.5764.dmp`：

```
c0000005
  std::_Hash<...>::_Find_last+0xe
  ZufyUI::detail::ReleaseAccessibility+0xd5
  ZufyUI::Window::~Window+0xb5
  ZufyUI::MenuWindow::scalar deleting destructor
  ZufyUI::MenuWindow::StandaloneHolder::s 的 atexit 析构
```

= 进程退出时，`MenuWindow` 的静态持有者析构，`~Window` 去查 UIA 表，而那张表**已经被销毁**。
是库的静态析构顺序问题（`Window` 析构函数依赖一个更早析构的全局表）。

绕开方式：后台模式在 `Run()` 返回后不返回 C++（不跑静态析构），直接 `ExitProcess(ok?0:1)`。
已在 STATUS 的库反馈里记录。

### 设置改用开关

`ToggleSwitch`（`SetOn/IsOn/Toggled`）替代"开/关"按钮。

### 应用图标

- `tools/make_icon.ps1` 生成 `app.ico`（16/32/48/64/128/256，经典 32bpp DIB 条目）
- `app.rc` 里 `101 ICON "app.ico"` + VERSIONINFO
- `platform::LoadAppIcon(cx,cy)` 从 exe 资源加载，主窗口 / 任务窗 / 设置窗都设了标题栏与任务栏图标
- exe 体积 1418 KB -> 1782 KB（图标内嵌）

```
build.ps1 -> 55 断言全过；--selftest -> PASS
```

### 提权重启丢参数（顺带修）

`RelaunchElevated()` 原来只 `ShellExecuteEx(runas)` 传 exe，**没传命令行参数**，
所以开了"自动提权"后 `--selftest` / 初始路径都会丢（表现为自检不写报告、看起来像启动就退出）。
现在把 `CommandLineToArgvW` 拿到的参数重新拼好放进 `lpParameters`；
另外自检模式跳过自动提权（自检不需要管理员，也不该弹 UAC）。

```
build.ps1 -> 55 断言全过 -> exe 1782 KB（含图标）
--selftest -> PASS
```

## 图标保真 + 驱动器图标（2026-11-18 深夜）

- **驱动器磁贴用错图标**：原来走 `ShellIcons::Get(true, root, false, ...)`，那是
  "文件夹"探针（早年为修驱动器图标 bug 加的），所以 C: 显示成黄色文件夹。
  新增 `platform::GetShellIconForPath(path, large)`（真实路径、不加
  `SHGFI_USEFILEATTRIBUTES`），磁贴改用真实卷图标。
- **图标发虚**：重新设计并生成 10 个尺寸（16/20/24/32/40/48/64/96/128/256）；
  ≤20px 只画粗体 "Z"（不再画文件夹细节），≥24px 画"白文件夹 + 蓝色 Z"。
- 库已重新导出（新增 `AddTab(..., id)` / `GetTabId` / `RemoveTabById`），编译通过。

```
build.ps1 -> 55 断言全过 -> exe 1837 KB
```

## 导航状态与表头排序（2026-11-20）

- **“此电脑”成为正式导航位置**：`::thispc` 现在会取消尚未完成的目录加载、清空旧列表、
  进入前进/后退历史，并从盘符根目录按“上一级”正确返回。
- **启动只导航一次**：移除“先加载用户目录，再定时跳到此电脑/命令行路径”的双重加载。
  普通启动直接进“此电脑”；命令行路径与自检目录也走同一个导航入口，不再互相覆盖。
- **表头排序**：名称 / 大小 / 类型 / 修改日期支持点击排序，再点同列切换升降序；
  文件夹始终在前，当前排序列用 ▲/▼ 标示。
- **排序保持选择**：`FileListView` 在重排前记录选中项的完整路径，重排后按路径恢复
  “当前行 + 勾选集合”，不会把旧行号映射到另一个文件。
- **选择同步修正**：`selectedRow_` 现在跟随 `CurrentCellChanged`，键盘选择和程序恢复
  都会更新页面状态；此前只有鼠标点击路径部分可用。

```
platform tests -> 64 passed, 0 failed
isolated Release build -> OK
--selftest -> PASS
```

### ZufyUI v1.29.1 同步（2026-11-20）

- 重新导出 `ZufyUI@d41ed86`（v1.29.1）到 `third_party/ZufyUI/`。
- 该版本包含 PageHost 反向导航缓动、过渡命中变换、指针/捕获取消和
  非 DWM 窗口显示状态修复。
- ZExplorer 无 API 破坏，正式 Release 链接通过；平台测试仍为 64 项全过，
  `--selftest` PASS。

## 任务窗稳定 ID 重构（2026-11-20）

- 首页页签使用固定 id `-1`；每个任务页签直接使用任务 id。
- 删除 `tabOrder_`、`TabIndex()`、`index + 1` 等平行索引映射。
- `ShowTask`、首页“查看”、错误状态自动前置全部改用 `FindTabById`。
- 页签删除改用 `RemoveTabById`；延后删除列表去重，连续点关闭不会重复排队。
- `Ping` 新增第 5 个字段：`tabs=<含首页总数> selected=<page id> home=-1
  ids=[-1,1,...] pending=<n> duplicate=<n>`。

### 端到端验证

启动独立 Host，创建两个同名冲突复制任务后：

```text
mapping      : tabs=3 selected=2 home=-1 ids=[-1,1,2] pending=0 duplicate=0
show-task-1  : 1
selected     : tabs=3 selected=1 home=-1 ids=[-1,1,2] pending=0 duplicate=0
```

结论：任务页签 id 稳定、无重复，按 id 切换后选中页正确。探针目录和 Host 实例均已清理。

## 老项目能力迁移：属性编辑器 + 打开方式（2026-11-20）

- 旧项目主体已确认是 `C:\project\pc_easy2\exp\main.cpp`，不是只含空壳的
  `widget.cpp/.ui`。
- 新增 `platform/FileAttributes`：安全读写常见文件属性，并支持 NTFS
  `Zone.Identifier` 数据流。
- 只读文件写 ADS 时临时清除只读位，成功后恢复；失败路径也会恢复，避免无意改变权限。
- 右键“编辑文件属性…”：只读 / 隐藏 / 系统 / 存档可编辑；压缩、加密、重解析点、
  稀疏、离线等危险或只读状态只展示，不盲目写回。
- Zone.Identifier 支持删除、ZoneId 0/2/3/4 和自定义内容；目录禁用该区域。
- 旧项目“打开方式”入口迁入：右键“打开方式…”和 `Ctrl+O`。
- 平台测试 64 → **80 项全过**，完整 Release 编译通过，主界面自检 PASS。

## NTFS 文件流管理器（2026-11-20）

右键文件 → “NTFS 文件流…”：

- 枚举默认数据流和全部命名流，显示数量、命名流数量及总大小。
- 导入普通文件到指定流；同名流存在时拒绝静默覆盖。
- 从选中流导出为普通文件，内容逐字节一致。
- 导出先写同目录临时文件，成功后再替换目标；取消不会破坏已有目标文件。
- 删除选中流；主数据流不可删除。
- “隐藏到数据流”：导入完成后把源文件移入回收站；导入失败时保留源文件。
- 文件流名称校验，拒绝路径分隔符、冒号和通配符等非法字符。
- 导入/导出/删除全部交给后台 Host，支持进度与取消；取消导入会删除半截流。
- 只读基础文件写流时临时清只读位，随后恢复。
- 目录不支持命名数据流，右键入口禁用。

新增 `platform/NtfsStreams` 平台测试覆盖：枚举、数量、名称、导入、重复导入拒绝、
取消清理、导出内容一致、取消导出保留旧目标、删除和非法名称。平台断言
80 → **110 项全过**。

## 托盘退出无响应修复（2026-11-20）

根因不是托盘菜单没收到点击，而是 `HostApp::RequestQuit()` 在菜单或管道回调线程里
同步 `join` 所有任务线程；任务仍等待/收尾时，退出回调被阻塞，表现为点击后完全没反应。

修复：

- `quit` 改为幂等 exchange，重复点击不会重复关闭。
- 先发出任务取消并唤醒并发等待，再把关闭动作投递到 UI 线程。
- UI 线程停止 PipeServer 和任务泵，最后 `PostQuitMessage(0)` 结束消息循环。
- 不在退出回调里同步 join worker；进程由现有 `ExitProcess` 路径结束，继续规避
  ZufyUI 静态析构崩溃。

实测新版正式 Host：

```text
quit-reply        : 1
exited-within-4s  : True
```

授权 UAC 后结束旧高权限 Host PID 34920，随后默认 Release 已更新，无残留
`ZExplorer` 进程。

## 任务运行信息与历史回收（2026-11-20）

- 任务页签和首页卡片新增百分比、文件数、速度、已用时间、预计剩余时间及终态。
- 终态任务的 `finishedAt` 固定总耗时，不再随首页刷新持续增长。
- 当前文件在分块复制开始前更新，不再停留在“准备中…”。
- 成功任务结束时幂等补齐 `doneFiles/doneBytes = totalFiles/totalBytes`；
  失败和取消任务不伪造完成数。
- Host 每轮泵清理 15 分钟前的终态任务；TaskWindow 删除已不存在任务的陈旧页签。
- 增加 `--no-elevate` 测试参数，便于无 UAC 的 UI 自动化验收。

### UI 实测

16GB 临时文件复制运行态：

```text
52% · 0/1 个文件 · 1.67 GB/s · 已用 5 秒 · 剩余约 4 秒
当前文件: C:\project\ZExplorer\tmp_ui_copy3\source\large.bin
```

完成态：

```text
100% · 1/1 个文件 · 已用 9 秒 · 已完成
```

已验证任务页签自动收起、窗口隐藏、冲突卡片布局和取消路径；临时测试文件已清理。
平台断言 110 → **112 项全过**。

## 完整属性、文件流编辑与解除占用（2026-11-20）

### 文件属性

- 恢复旧版两列基本属性：只读、隐藏、系统、目录、存档、正常、临时、压缩。
- 高级属性使用 ZufyUI `Expander` 折叠卡片，完整展示设备、稀疏文件、重解析点、脱机、无内容索引、加密、
  完整性流、虚拟、无清理数据、扩展属性、固定、取消固定。
- 常见属性、临时、无内容索引、固定状态可安全编辑；系统/文件系统管理标志灰显，
  不把无效或危险的属性位强写回。
- 压缩通过 `FSCTL_SET_COMPRESSION`；加密/解密通过 EFS API。
- 加密与压缩互斥，固定与取消固定互斥。
- Zone.Identifier 保留完整区域选择和自定义内容编辑。

### NTFS 文件流

- 管理器底部只保留“创建文件流 / 刷新 / 关闭”三个按钮。
- “创建文件流”只询问名称，确定后直接创建一个空流。
- 流列表右键菜单：重命名、删除、文本编辑、导入文件内容、导出为文件。
- 默认数据流和空选择会禁用上述右键操作。
- 文本编辑直接编辑最多 24KB 内容，后台写入。
- 重命名先完整复制到新流，成功后删除旧流；失败保留旧流。
- 导入/导出、文件隐藏、删除、主数据流保护、只读恢复等原有逻辑保留。

### 解除文件占用

- 使用 Restart Manager 枚举占用进程，显示 PID、名称、程序类型和路径。
- 恢复旧项目手工实现的内核句柄扫描：`NtQuerySystemInformation` 枚举、
  `DuplicateHandle` 后比较卷序列与文件索引，定位具体文件句柄。
- 支持 `DUPLICATE_CLOSE_SOURCE` 关闭具体句柄，也支持确认后结束进程。
- 相比旧实现，只扫描 Restart Manager 已命中的 PID，不再硬编码对象类型索引，
  并针对缓冲区增长、重复 PID、句柄打开失败做了防御。
- 普通权限下句柄扫描安全降级；管理员后台下可关闭具体句柄。
- 新增 `platform/FileLock` 无窗口测试。

平台断言 112 → **135 项全过**；正式 Release 构建成功，主界面自检 PASS。

## 文件占用实验室与全系统句柄补强（2026-11-20）

### 解除文件占用

- 句柄扫描不再以 Restart Manager 返回的 PID 为前提。现在每次都会独立枚举全系统句柄，
  Restart Manager 只负责补充“不可安全重启 / 服务 / 窗口程序”等进程类型。
- 目标文件可正常打开时，用文件对象匹配具体句柄；目标被独占而无法打开时，退回
  `DuplicateHandle + GetFinalPathNameByHandleW` 对比规范化路径，不依赖可写句柄。
- 同一 PID 不再只保留一个句柄。列表会显示句柄数量、访问权限和首个句柄值；
  “关闭全部句柄”会一次提交该进程上所有可关闭的文件句柄。
- 新增删除探测：只读但授予 `FILE_SHARE_DELETE` 的引用会显示“可打开 DELETE 句柄”，
  真正因共享模式阻止删除时显示“共享引用正在阻止删除”。
- 受保护进程或普通权限无法复制句柄时会明确降级提示；系统进程仍只允许查看，
  不允许直接结束。
- 状态行改为最多三行自动换行，不再把诊断信息截成一条省略号。

### 根目录测试器

新增 `FileLockLab.cpp`、`FileLockLab.vcxproj` 和根目录可执行文件 `FileLockLab.exe`。
直接运行会显示交互菜单，也可以在命令行指定：

```powershell
.\FileLockLab.exe --mode=delete-friendly
.\FileLockLab.exe --all
```

覆盖的模式：

1. `exclusive`：读写但拒绝全部共享。
2. `readonly-noshare`：只读但拒绝全部共享。
3. `readonly-no-delete`：只读，不授予删除共享。
4. `writer-no-delete`：读写，不允许删除共享。
5. `delete-friendly`：句柄存在，但允许删除。
6. `mapped`：关闭文件句柄后保留 Section + View。
7. `region-lock`：允许删除共享，同时用 `LockFileEx` 锁定整个区域。
8. `child-process`：隐藏子进程跨进程持有文件。

测试文件默认创建在 ZExplorer 根目录，`--path=<目录>` 可改位置。控制台关闭后
系统自动释放句柄；正常按 Enter 退出时测试文件保留，便于复查。

### 验证

- Release 构建包含主程序与 `FileLockLab.exe`。
- 平台断言 135 → **149 项全过**，新增只读可删除、双句柄同 PID、全系统扫描报告断言。
- 高权限实验室实测独占 / 四种共享组合 / 区域锁 / 跨进程模式均返回具体句柄；
  `delete-friendly` 返回“可删除”，`mapped` 返回 0 个文件句柄且不误报删除冲突。

### 占用列表右键操作与空参数修复（2026-11-20）

- 当前 UI 进程从占用列表中强制剔除；扫描目标就是自己的 exe 时，与当前程序同路径的
  UI / Host 兄弟进程也不列出，避免把“自己”当成可解锁对象。
- 占用进程行新增右键菜单：
  - 每个可关闭句柄单独一项，例如“关闭句柄 0x1C4”；
  - 同一 PID 有多个句柄时提供“关闭此进程的全部 N 个句柄”；
  - 提供“结束此进程”。
- 底部按钮与右键菜单都使用**点击前的进程/句柄快照**，不再在定时刷新后按旧行号
  重新取进程，避免出现 `PID 0` 或空参数。
- 后台 `closehandle` 增加 PID、句柄值严格校验；任务标题改为
  “关闭文件句柄：PID x · N 个句柄”，不再显示内部操作名 `closehandle : 0`。
- `EnumerateFileLocks` 增加显式 `includeCurrentProcess` 测试参数；产品 UI 默认排除自身，
  实验室为了展示自持句柄可以显式包含。
- Restart Manager 在注册/查询资源时短暂报告的 ZExplorer UI、`--host` 及其他同程序路径
  进程全部排除，不参与“占用进程”数量。
- 只有 Restart Manager 能证明、但系统句柄扫描拿不到具体句柄的进程，必须连续两次刷新
  都存在才进入列表，避免扫描时一闪而过的临时结果。

平台断言 149 → **153 项全过**；Release 构建成功。

- 全系统里“受保护进程无法复制句柄”的计数继续保留在 `FileLockReport` 诊断字段，
  但不再作为每次扫描的占用警告；该总数不能证明这些进程占用了目标文件。
- `FileLockLab` 显式包含当前进程时，自持句柄会标记为可关闭，测试器可以真实演示
  单句柄关闭；产品 UI 仍默认排除当前进程。

### ZufyUI v1.29.2 同步（2026-11-20）

- 从 `C:\project\ZufyUI@a4bab7c` 复制全部 65 个 tracked 文件到
  `third_party/ZufyUI/`；未复制未跟踪的 `SystemDump/`。
- 逐文件 SHA-256 核对：65/65 一致，目标目录与源库文件集合完全一一对应。
- 本版包含自定义标题栏默认显示标题/图标、Card 默认不播放悬停动画、
  ComboBox 覆盖层优先命中、正常释放捕获不再误判为指针取消、
  数据视图列宽变化失效缓存等修复。

## 资源管理器交互补齐（2026-11-20）

- 文件夹也支持“解除文件/文件夹占用”；目录句柄使用
  `FILE_FLAG_BACKUP_SEMANTICS` 扫描，平台测试覆盖目录独占句柄定位和关闭。
- 双击保持 Windows 资源管理器语义：目录进入浏览，普通文件交给 Shell 默认程序打开。
  Shell 打开不再设置 `SEE_MASK_FLAG_NO_UI`，无文件关联时会显示系统“打开方式”，
  不再表现为静默无反应。
- 右键菜单明确区分：
  - “打开”：直接运行/由系统默认程序打开；
  - “打开文件位置”：文件打开父目录并选中目标文件，文件夹直接打开自身；
  - “打开方式…”：显示系统选择器。
- 地址栏新增常驻编辑图标、当前路径段点击编辑和 `Ctrl+L`；输入后可直接粘贴路径。
- 支持 `%TEMP%`、`%SystemRoot%\System32` 等环境变量展开。
- 地址输入会依次判断绝对/相对目录、文件、命令：
  - 目录：导航；
  - 文件：交给默认程序；
  - `cmd`、`powershell` 或带参数命令：以当前 ZExplorer 目录为工作目录启动，
    不刷新列表、不改变当前路径和已有选择。

Release 构建成功；平台断言 153 → **166 项全过**。

## 任务来源与通知抽屉（2026-11-20）

- IPC 新增 `SubmitTaskFrom`；`TaskClient::Submit` 自动携带发起窗口的 PID 和进程镜像路径。
  Host 任务快照保留 `originPid / originPath`，旧的 `SubmitTask` 协议仍兼容。
- 每个窗口只统计、显示和通知自己 PID 发起的任务；来源任务出错时不再强制弹出全局任务窗。
- 新增可复用底部通知抽屉 `NotificationDrawer`：
  - 自下而上动画、宽内容区、内部滚动；
  - `Esc` 或点击遮罩空白处关闭；
  - 支持 Info / Warning / Error；
  - 主窗口的前端错误和来源任务错误统一进入该抽屉。
- 地址栏移除铅笔按钮；整块面包屑空白区域可点击进入编辑，当前路径段也可点击，
  进入后自动全选。`Ctrl+L` 保留。
- 地址栏不存在命令不再触发 Windows Shell 错误框，统一显示应用内抽屉。

桌面验证：

- 点击地址栏空白可直接进入编辑并全选当前路径。
- 输入不存在的命令后，应用内底部抽屉显示错误，不再弹 Windows 提示。
- `Esc` 和点击遮罩空白都能关闭抽屉。
- IPC 实测来源为 PID `424242` 的任务快照正确携带
  `C:\Demo\ZExplorer.exe`。

### 抽屉外层边框与地址栏布局回归（2026-11-20）

- `NotificationDrawer` 不再把原页面直接作为 `Drawer` 内容。现在外层再包一层 `Card`：
  - Card 负责完整窗口边界、10px 圆角和 8px 内缩；
  - 原 Explorer ColumnBox 的 16px margin 恢复生效；
  - 抽屉打开时仍覆盖整个应用，关闭后主内容不会贴窗口边缘。
- 地址栏当前路径段改为固定测量宽度的按钮，剩余空间只交给不可见空白命中区，
  路径过长时各段按自身文本省略，不再被横向拉伸和均分。
- 地址栏程序化聚焦改为 `Window::FocusElement(addressBox)`，不再是只改 TextBox
  自身 `focused_` 标志；单击面包屑后当次点击就能输入，无需第二次点击。
- 自检布局恢复为工具栏 `x=24 y=52 w=1152`、地址栏 `x=246 y=52 w=818`；
  普通态、编辑态和深路径自截图均通过。

### 21:30 崩溃 dump 初步结论

- Dump：`C:\project\ZExplorer\x64\Release\ZExplorer.exe.28648.dmp`，约 1.34GB。
- 异常：`c0000005` Access Violation，发生在旧临时副本
  `ZExplorerFinalUX-20261120-212717` 的窗口消息回调链上。
- 旧副本 PDB 已被后续 Release 构建覆盖，当前 PDB 与 dump 模块签名不匹配，
  因此不能把具体源码行伪装成已定位。
- 该 dump 对应地址栏/抽屉重构之前的代码。当前版本已替换相关根布局、地址栏点击链和
  通知处理链；如果新版本仍能复现，需要保留新 dump 与同次 PDB 再精确符号化。

## 复制路径与老项目差异收口（2026-11-20）

- 右键菜单新增“复制路径”，快捷键 `Ctrl+Shift+C`。
- 多选时每行一个完整路径写入 `CF_UNICODETEXT` 剪贴板。
- 老项目的日志查看器确认属于另一套 Qt 工具，不迁入 ZExplorer。
- TrustedInstaller 实际实现位于 `pc_easy2/powerrun/main.cpp`；
  接入方案已写入 `PLAN.md`，暂缓编码。

Release 构建成功；平台断言保持 **175 项全过**。

### 排序语义修正（2026-11-20）

- 名称、大小、类型排序都保持目录分组优先；目录没有有效文件大小，
  因此按大小时位于文件之前。
- 修改日期排序时目录和文件严格按日期混排，不再把目录固定在顶部。
- 新增测试覆盖：名称目录优先、大小分组与文件降序、类型目录优先、
  修改日期混排，以及同字段名称回退。

### 属性菜单与左树滚动修正（2026-11-20）

- 右键“属性”改名为“系统属性面板”，行为仍由 Shell 打开 Windows 原生属性；
  “编辑文件属性…”继续打开 ZExplorer 自己的属性编辑器。
- 左树跟随目录时只做最小滚动：
  - 目标节点已经完整位于当前视口内：不改滚动位置；
  - 节点在上方不可见：滚到顶部刚好可见；
  - 节点在下方不可见：滚到底部刚好可见。
- 移除了原先无视当前滚动偏移、把可见节点重新拽到顶部/底部的行为。

### 快捷访问、抽屉提示与新窗口（2026-11-20）

- 新增 ZExplorer 自有快捷访问，独立存储在
  `HKCU\Software\ZExplorer\QuickAccessPins`，不读取 Windows 资源管理器数据。
- 左栏上半区为带 Shell 图标的快捷访问纯列表，下半区为目录树，中间使用上下
  `SplitView`：
  - 快捷访问为空时整个分区和分隔条隐藏；
  - 快捷访问的可拖拽下限是左栏总高度的 **1/12**，不能直接缩得更小；
  - 继续把分隔条拖向边缘会触发原生折叠，松手后收起；
  - 目录树最小高度 160 DIP；
  - 快捷访问高度不超过左栏 50%。
- 文件夹单击到父目录、双击进入；文件单击到所在目录、双击由系统打开。
- 快捷项右键复用目标路径菜单，支持固定/取消固定和完整文件操作。
- 文件列表右键新增“固定到快捷访问”，已固定时切换为“从快捷访问取消固定”。
- 所有非空 `ShowHint` 改走通知抽屉；顶部小 Label 不再显示提示。
- 新增 IPC `NewWindow`：当前 UI 把目录传给 Host，由 Host 启动新的 ZExplorer UI；
  UI 不再直接 `ShellExecute` 新窗口。
- 实测 IPC 返回 `1|已创建新窗口`，同一临时目录出现 2 个独立 UI 进程。
- 右键菜单新增“系统右键菜单…”，通过 `IContextMenu` 打开 Windows 原生 Shell 菜单；
  命令执行优先使用 `IContextMenu::GetCommandString` 返回的规范动词，动态命令回退到菜单序号，
  并转发 `IContextMenu3` 消息。

## 可执行文件启动策略与完整错误抽屉（2026-11-21）

- 设置新增 `ExecutableViaShell`，默认开启：
  - 开启：EXE/COM/BAT/CMD/PS1/SCR/MSI 由当前 UI 进程通过 ShellExecuteEx 打开；
  - 关闭：UI 提交 `launch` 任务，由 Host 用自己的权限和 ShellExecuteEx 启动。
- 启动设置实时从 `HKCU\Software\ZExplorer` 读取，切换后无需重启窗口。
- 通知抽屉高度扩大到 300–620 DIP，正文改为只读 `TextEdit`：
  - 不限制 10 行，可滚动；
  - 可选择并用 `Ctrl+C` 复制完整错误；
  - 前端错误自动附带时间、操作、当前目录、发起进程、后台连接状态和当前选择；
  - 后台任务错误自动附带任务 ID、操作类型、目标、当前文件、进度和全部问题项。

## 右键菜单横向行接线（2026-11-21）

- 主列表右键菜单改为组合式横向布局，子项统一为“图标在左、文字在右”：
  - 打开行 3 项：打开 / 打开方式 / 打开文件位置；
  - 新建行 2 项：新建文件夹 / 新建文本文档；
  - 文件操作行 4 项：复制 / 剪切 / 粘贴 / 重命名；
  - 删除行 2 项：删除 / 永久删除；
  - 属性行 2 项：系统属性面板 / 编辑文件属性。
- 快捷访问目标菜单复用同一套打开、文件操作、删除和属性行。
- 横向单元格改为 `ContentFit`，并按中文图标菜单的实际内容宽度设置最小宽度，
  避免“打开方式…”等文字被截断成“打开方...”。
- “显示隐藏和系统文件”“隐藏已知扩展名”只保留在第二行“选项”菜单；
  “在当前位置打开新窗口”从右键菜单移到第二行图标区；普通右键菜单移除全选和刷新。
- “选项”菜单中的查看开关、固定、选择操作、设置和关于均补充系统图标。
- Release 构建成功；平台断言 **216 项全过**。

### 刷新滚动保持与透明表面回退（2026-11-21）

- 直接启用 `Backdrop::Acrylic` 并强改应用控件的 alpha 会产生全透明/空白界面；
  当前已恢复 `Backdrop::None`、主题实心底和原有 `Card`/`TableView` 表面。
- 正式需求已记录在 [ZUFYUI_LIBRARY_CHANGES.md](ZUFYUI_LIBRARY_CHANGES.md)：
  库需要为容器和数据控件提供主题安全的透明表面模型，而不是由应用层硬设颜色。
- `TableView` 新增 `SelectRowNoScroll` / `SetCurrentCellNoScroll`：程序化恢复
  选择时只更新选择与信号，不调用 `EnsureVisible`。
- 文件列表刷新和排序使用无滚动选择，已选目标不会把用户正在浏览的视口拉回去；
  原有的 `SelectRow` / `SetCurrentCell` 保可视语义不变，显式联动仍可按需使用。
- ZufyUI 新增回归断言覆盖“切换选中行但滚动偏移保持不变”；库自检
  **7491 项通过，0 失败**，ZExplorer 平台测试 **216 项通过，0 失败**。

### 动态任务文字刷新（2026-11-22）

- 任务页签下方文字只在窗口 resize 后刷新，根因是 `Label::SetText` 未让自身
  离屏缓存失效；固定宽度 `0%` / `7%` 标签尺寸不变，旧位图继续被复用。
- 库修复：文本内容真正变化时调用 `InvalidateCache()`，再请求重绘。
- 新增 ZufyUI 回归断言：固定宽度 Label 从 `0%` 改为 `7%` 后缓存必须失效；
  库自检 **7478 项通过，0 失败**。
- 上一轮与本问题无关的进度条 API、重复百分比删除、Junction 统计/删除改动已全部撤回。

### 自适应地址栏与完整路径折叠（2026-11-22）

- 第一行精简为“任务、后退、前进、上一级、地址栏”；刷新和提权按钮移除。
- 地址栏不再按路径内容申请宽度，也不再设置独立最小/最大宽度：固定控件排完后，
  地址栏独占全部剩余宽度。
- 路径完整可见时从根到当前目录正常顺序显示。
- 空间不足时固定显示根盘符、可点击 `...`，再从路径尾部向前选择最多可容纳的目录段。
- 点击 `...` 打开完整目录链菜单，菜单项使用完整段名并可直接导航到对应目录。
- 实测 720 宽窗口显示为 `C:\ › ... › third_party › ZufyUI › ZufyUIDebugger`，
  地址栏宽度随窗口实时重算。
- 搜索框移除应用层 `SetWidth(280)` 固定基准，最小宽度显式设为 0，只使用第二行
  固定按钮之后的剩余空间；地址编辑框同样清除 TextBox 默认宽度基准。720 宽窗口
  实测搜索框不再越过右边界。

### 文件链接与设置动画（2026-11-22）

- 设置页 `TabView` 移除 `0.08` 秒覆盖值，恢复 ZufyUI 的默认 `0.3` 秒过渡。
- 无头链接套件先于业务接入完成，覆盖：
  - 硬链接数量查询、同卷移动、删除单个名称、复制成独立文件；
  - 文件符号链接的统计、复制、删除与目标保护；
  - Junction 的统计、递归搜索、复制、永久删除与目标保护。
- 链接契约：
  - 统计和搜索不进入 Junction/符号链接目标；
  - 复制重建链接本身，不复制目标内容；
  - 永久删除只删除链接目录项；
  - 硬链接复制产生普通独立文件，移动保留底层同一文件关系。
- 已接入 `CopyTree / MoveTree / DeletePermanent / DirectoryModel::Search`
  和文件类型显示；属性编辑器显示硬链接数量或链接目标。
- 平台测试 **264 passed, 0 failed**，其中 Junction 用例实际执行，不再跳过。

### 跨卷链接互操作（2026-11-22）

- `platform_tests` 支持 `-SecondVolumeRoot` / `argv[1]`，第二卷可用时启用完整
  跨卷矩阵；不提供时明确输出 `SKIP`。
- 新增独立控制台样例 `CrossVolumeLinkLab.exe`，参数为两个不同卷或卷内目录：
  `CrossVolumeLinkLab.exe "C:\project" "D:\"`。
- 跨卷覆盖：普通文件移动、硬链接名称移动、禁止跨卷创建硬链接、文件符号链接
  移动、Junction 移动，以及链接目标保留检查。
- 使用 512 MB expandable NTFS VHD 实测：
  - 全量平台测试 **320 passed, 0 failed**；
  - `CrossVolumeLinkLab` **23 checks, 0 failures**。
- 测试盘保留并挂载为 `Z:\`，宿主文件
  `C:\project\ZExplorer\test-disks\ZExplorer-CrossVolume-512.vhd`，
  `test-disks` 已加入 `.gitignore`；现有物理分区未被修改。

### 快捷方式与动态菜单（2026-11-22）

- 新增 `platform/Shortcuts`，通过 `IShellLinkW + IPersistFile` 解析 `.lnk`
  目标、参数、工作目录和描述。
- 目标分类：文件、目录、Shell 项、目标缺失、非快捷方式。
- 目录快捷方式双击/回车直接在 ZExplorer 内进入目标目录；文件或 Shell 快捷方式
  继续交给 Shell，保留参数、工作目录和快捷方式自身的权限语义。
- 单选快捷方式时，右键菜单在 factory 内动态构造“打开目标位置/目标文件夹”，
  与“打开文件位置”（定位 `.lnk` 自身）并存。
- 这些 ZExplorer 位置动作只做当前窗口内部导航：文件先进入父目录再选中目标，
  目录直接导航；不调用外部资源管理器。
- ZExplorer 自身列表不再为普通文件重复提供“打开所在位置”；普通文件本来就在
  当前目录，只有 `.lnk` 才需要动态目标位置入口。
- Windows 资源管理器 Shell 菜单新增 `Software\Classes\*\shell` 文件类型注册：
  任意文件右键“用 ZExplorer 打开”后进入父目录并选中该文件；目录、驱动器和
  背景入口保持原有行为。
- 主列表和快捷访问菜单本来就使用 `SetContextMenuFactory`，每次右键重新构造；
  快捷方式解析结果和路径按值捕获，菜单对象与回调不持有临时字符串引用。
- 无头测试覆盖文件快捷方式、目录快捷方式、目标缺失、大小写扩展名和跨卷目标。
- 实测 UI 菜单出现独立“打开目标位置”行；目录快捷方式通过回车进入目标目录。
- 单卷平台测试 **282 passed**；挂载 Z: 测试盘后跨卷测试 **320 passed**。

### 后台接续启动与低权限 IPC（2026-11-22）

- 普通入口启动时先探测活动 Host。若已存在，通过 `NewWindow` 让 Host 用自身令牌
  创建新 UI；入口进程收到成功回执后直接退出。
- Host 创建的所有 UI 命令统一带 `--force-current-process`，避免后台拉起的窗口再次
  转交形成递归；该参数也可手动用于双实例和独立进程测试。
- Host 命名管道改为“当前用户 + SYSTEM + Administrators”DACL，并设置低完整性
  标签。Medium 权限入口现在可以连接高权限 Host，不再使用管理员默认 ACL。
- 实测：低权限入口成功退出；新 UI 的父进程为活动 Host；手动
  `--force-current-process` 保持当前进程运行。
- 亚克力透明实验已回退。当前优先保证布局和可读性；透明容器/控件作为正式库需求
  跟踪，不再用无背景布局宿主或半透明硬编码绕过主题系统。

### 通知分流（2026-11-21）

- 普通操作反馈、成功提示和 `Info` 级消息只更新底部状态栏，不再打开通知抽屉。
- 只有 `Warning` / `Error` 进入大尺寸通知抽屉；错误正文继续附带完整诊断与任务问题清单。
- 任务普通错误补齐“全部重试 / 全部忽略”；重试时先消费旧问题，失败的新问题会重新回填并继续等待，不再被旧问题清理误删。

## 文件管理器基础能力补齐（2026-11-21）

- 查看选项：`显示隐藏和系统文件`、`隐藏已知扩展名`，默认关闭，可从列表右键菜单切换并持久化。
- 拖放：只有从已选中行开始普通拖动才通过 OLE 拖出；从空白或未选中行开始拖动执行框选，`Shift`+点击叠加多选。外部文件可拖入当前目录，同文件夹项目自动忽略，其余按 COPY/MOVE 提交后台任务。
- 目录监视：`DirectoryWatcher` 使用 `ReadDirectoryChangesW`，带启动握手和 180ms 防抖；外部变化自动刷新。
- 状态栏：`pSel_` 显示选中项数和文件大小合计。
- 特殊位置：快捷访问固定显示桌面、下载、文档、图片、音乐、视频、库和回收站；此电脑首页对应位置使用带图标的卡片；“回收站”页面可查看统计、打开资源管理器并清空。
- 任务栏进度：通过 `ITaskbarList3` 把当前窗口的后台任务平均进度同步到任务栏。
