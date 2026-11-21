<p align="center">
  <img src="docs/icon-256.png" width="112" alt="ZExplorer icon">
</p>

<h1 align="center">ZExplorer</h1>

<p align="center">
  Windows 高级文件操作外壳与 ZufyUI 旗舰压力测试项目
</p>

<p align="center">
  <a href="https://github.com/zhc9968/ZExplorer">GitHub</a>
  ·
  <a href="https://gitee.com/zhc9968/ZExplorer">Gitee</a>
  ·
  <a href="https://github.com/zhc9968/ZufyUI">ZufyUI</a>
</p>

## 项目简介

ZExplorer 是一个面向高级用户的 Windows 文件管理器。它保留资源管理器的基本交互习惯，
同时把 Windows 有、但普通界面不容易触达的能力集中到同一个工具里，例如 NTFS 文件流、
文件与文件夹占用分析、远程句柄关闭、文件属性编辑、原生 Shell 菜单和可审查的后台任务。

ZExplorer 同时是 [ZufyUI](https://github.com/zhc9968/ZufyUI) 的主要真实业务压力测试场。

## 当前能力

- Explorer 风格地址栏、导航树、文件列表、表头排序和多选。
- 响应式首页：常用位置与驱动器按窗口宽度自动排为 2–3 列。
- 用户自定义快捷访问优先显示，并同步到首页。
- 当前目录与子目录递归搜索，搜索和目录读取在后台线程执行。
- 复制、移动、删除、永久删除、重命名和新建，支持逐项错误处理、重试和忽略。
- 后台任务队列、并发上限、任务历史、速度/进度和取消。
- 文件与文件夹占用分析、进程定位、句柄关闭和结束占用进程。
- NTFS 数据流查看、创建、重命名、编辑、导入、导出和删除。
- 文件属性、扩展属性、Zone.Identifier 等高级信息。
- 回收站表格视图、原生项目菜单、还原、永久删除和清空。
- 系统右键菜单、文件夹默认打开方式与 Win+E 的可选集成。
- 支持浅色/深色主题、底部通知抽屉和无障碍控件树。

## 架构

同一个 `ZExplorer.exe` 承担前台 UI 和后台 Host 两种角色：

```text
ZExplorer.exe                  ZExplorer.exe --host
┌──────────────────────┐       ┌──────────────────────────┐
│ UI / ExplorerPane    │       │ 托盘、任务队列、并发控制   │
│ TaskClient IO thread │──────▶│ 文件操作、占用分析、句柄   │
│ ASCII/Unicode UI     │ IPC   │ 可审查任务与错误决策       │
└──────────────────────┘       └──────────────────────────┘
```

主要设计约束：

- UI 线程不直接执行耗时文件操作。
- `src/platform/` 与 `src/fs/` 只依赖 Win32/STL，可在无 UI 进程中测试。
- 高风险操作进入任务系统，并携带发起任务的 UI 进程信息。
- 系统和 Shell 接管均为显式开关，默认关闭。

## 构建

### 环境

- Windows 10/11 x64
- Visual Studio 2022/2026，安装“使用 C++ 的桌面开发”
- Windows 10/11 SDK

### 编译与测试

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

脚本会：

1. 检查 UTF-8 BOM。
2. 结束占用输出文件的旧实例。
3. 构建 x64 Release。
4. 构建独立文件占用实验程序。
5. 运行无窗口平台测试。

输出：

```text
x64\Release\ZExplorer.exe
x64\Release\FileLockLab.exe
```

## 运行参数

```text
ZExplorer.exe                         前台文件管理器
ZExplorer.exe --host                  后台托盘与任务进程
ZExplorer.exe --show-settings         打开后台设置页
ZExplorer.exe --show-about            打开关于页
ZExplorer.exe --selftest=<report>     自检并输出报告/截图
```

## 验证

当前平台测试由 `tools/run_tests.ps1` 独立编译，不链接 ZufyUI，用于验证分层规则。

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1
```

最近基线：

- x64 Release 构建通过。
- 平台测试 216 passed, 0 failed。

详细开发记录见 [docs/STATUS.md](docs/STATUS.md)，交接信息见 [HANDOFF.md](HANDOFF.md)。

## 来源与第三方组件

- UI 框架来源：[github.com/zhc9968/ZufyUI](https://github.com/zhc9968/ZufyUI)
- 当前仓库内 `third_party/ZufyUI/` 是 ZufyUI 的固定快照，便于可复现构建。
- ZufyUI 及其快照采用 MIT License。
- 更完整的第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

本项目采用 [MIT License](LICENSE)。

Copyright (c) 2026 zhc9968
