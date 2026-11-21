# ZExplorer 开发规则

> 本文件是这个项目的硬规则。**改动代码前先读一遍。**
> 系列背景：pcEasy 系列的目标是补上「Windows 功能面」与「用户接触面」之间的差距；ZExplorer 是这个系列的旗舰应用，也是 ZufyUI 唯一的压力测试场。

---

## 1. 禁止事项（最高优先级）

1. **不得修改 `third_party/ZufyUI/`**。它是从 `ZufyUI@a4bab7c`（v1.29.2 快照）导出的只读副本。
   - 需要库的新能力 → 写进 `PLAN.md` 的「库需求」一节，由库负责人处理。
   - 发现库有 bug → 记录到 `docs/STATUS.md` 的「待上报」小节，附最小复现，不要就地改。
2. **不得擅自缩小范围**。范围由用户定；计划里写「不做」的条目必须有用户的明确确认。
3. **不得自动 `git commit` / `git push`**。等工作区被认可后再由用户决定。
4. **不得用机制掩盖 bug**。先定位根因，再从根因修；定位不到就写「未定位」，不要猜、不要加延时/重试/上限糊过去。

## 2. 两条架构铁律

1. **`src/platform/` 只依赖 Win32 + STL**：不 include 任何 ZufyUI 头、不建窗口、不弹 UI、不打日志到界面。
   - 目的：平台逻辑能被 `tests/` 在**不建窗口**的情况下测。
2. **`src/ui/` 与 `src/app/` 里不得出现裸 Win32 句柄操作**（`HKEY`、`CreateFileW`、`SHGetFileInfo` 等）。
   - 需要新能力就先在 `platform/` 加函数，再在 UI 层调用。

## 3. 数据源与视图分离（当前阶段的核心约束）

虚拟化数据源与池化元素**尚未就绪**，当前采用「回退写法」：

- `fs::DirectoryModel` 负责**取数**（枚举、排序、过滤），对上层只暴露 `Snapshot`。
- `ui::FileListView` 是**视图接口**；当前实现是 `EagerFileListView`（打开目录时全量灌入，并设上限）。
- 将来接入 `VirtualList` 时，只新增一个 `VirtualFileListView` 实现，**不动 `DirectoryModel`、不动页面逻辑**。
- 因此：**任何 UI 代码都不得直接遍历文件系统**，一律经 `DirectoryModel`。

## 4. 编码与构建

- `.h` / `.cpp` 必须是 **UTF-8 带 BOM**（否则 MSVC 按 GBK 解析中文注释会报 `C2143/C2259/C2888`）。
- `.md` 必须是 **UTF-8 无 BOM**。
- `build.ps1` 与 `tools/*.ps1` **必须纯 ASCII、无 BOM**（PowerShell 5.1 会把无 BOM 的中文脚本按 GBK 读而解析失败）。
- 构建：`powershell -ExecutionPolicy Bypass -File build.ps1`（含 BOM 守卫 → 杀占用 → 编译）。

## 5. 性能底线（不是"以后优化"）

- 打开目录：≤200ms 出首屏（本地 SSD、≤5,000 项）。
- 列表滚动：≥58fps。
- 任何操作都不得阻塞 UI 线程超过 50ms；耗时动作一律走 `RunAsync`。

## 6. 交付节奏

- 每个阶段结束更新 `docs/STATUS.md`（当前进度、已知问题、下一步）。
- 每修一个并发/虚拟化相关 bug，必须在 `tests/` 或 `docs/STATUS.md` 留下可复现验证方式；没有验证方式的不算修好。
