#pragma once
// ============================================================================
// platform/FileOps.h —— 文件操作（复制 / 移动 / 删除 / 重命名 / 新建 / 剪贴板）
//
// 铁律：只依赖 Win32 + STL，不使用 PCH、不 include 任何 ZufyUI 头。
// 这样这些高风险操作能被 tests/ 在无窗口进程里直接覆盖。
//
// 剪贴板用 CF_HDROP + "Preferred DropEffect"，因此**能和系统资源管理器互通**：
// 在本程序里 Ctrl+C，切到资源管理器 Ctrl+V 可以粘贴。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ze::platform {

struct OpProgress {
    uint64_t doneBytes = 0;
    uint64_t totalBytes = 0;
    uint32_t doneFiles = 0;
    uint32_t totalFiles = 0;
    std::wstring current;
};

// 进度回调：返回 false 表示用户取消。
using ProgressFn = std::function<bool(const OpProgress&)>;

// 单项失败：不中断整个任务，收集起来交给 UI 显示/重试。
struct OpIssue {
    std::wstring path;      // 出问题的文件（源）
    std::wstring message;
    std::wstring target;    // 冲突时的目标路径；其它错误为空
    int kind = 0;           // 0 = 普通错误；1 = 目标已存在（可覆盖）
};

// 目标已存在时的决策
enum class ConflictAction { Overwrite, Skip, OverwriteAll, SkipAll };

// 单文件事件：供 UI 显示"每个文件"的进度
enum class FileEventKind { Start, Progress, Done, Failed };
using FileEventFn = std::function<void(FileEventKind, const std::wstring& path,
                                        std::uint64_t size, std::uint64_t processed)>;

struct CopyOptions {
    // 返回 Overwrite / Skip 等；为空表示直接覆盖
    std::function<ConflictAction(const std::wstring& src, const std::wstring& dst)> onConflict;
    FileEventFn onFile;
};

struct OpResult {
    bool canceled = false;
    std::uint32_t okCount = 0;
    std::vector<OpIssue> issues;

    bool Ok() const { return !canceled && issues.empty(); }
};

// 递归统计（文件数 / 字节数），用于给整批操作算进度分母。
void MeasureTreeStats(const std::wstring& path, std::uint64_t* bytes, std::uint32_t* files);

// 递归复制：目录 / 文件都可以；dst 是目标路径（含文件名）。
// 单个文件失败不会中断，失败项进 result->issues。
void CopyTree(const std::wstring& src, const std::wstring& dst,
              const ProgressFn& progress, OpResult* result,
              const CopyOptions& options = {});

// 移动：同卷走 MoveFileW，跨卷退化为复制 + 删除。
void MoveTree(const std::wstring& src, const std::wstring& dst,
              const ProgressFn& progress, OpResult* result,
              const CopyOptions& options = {});

// 删除到回收站（SHFileOperationW + FOF_ALLOWUNDO）。
void DeleteToRecycleBin(const std::vector<std::wstring>& paths, OpResult* result);

// 永久删除（不进回收站）。
void DeletePermanent(const std::wstring& path, OpResult* result);

bool RenamePath(const std::wstring& from, const std::wstring& to, std::wstring* error);

bool CreateDirectoryPath(const std::wstring& path, std::wstring* error);
bool CreateEmptyFile(const std::wstring& path, std::wstring* error);

// 目标已存在时生成 "名称 (2).ext" / "名称 (2)" 这种唯一路径。
std::wstring MakeUniquePath(const std::wstring& desired);

// ---- 剪贴板（文件）----
bool SetClipboardFiles(const std::vector<std::wstring>& paths, bool cut, std::wstring* error);
bool GetClipboardFiles(std::vector<std::wstring>* paths, bool* cut);
bool ClipboardHasFiles();
bool SetClipboardText(const std::wstring& text, std::wstring* error);

// ---- 修饰键（UI 层不该直接调 GetKeyState）----
bool IsCtrlDown();
bool IsShiftDown();
bool IsAltDown();

} // namespace ze::platform
