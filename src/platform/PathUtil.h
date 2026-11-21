#pragma once
// ============================================================================
// platform/PathUtil.h —— 路径与格式化工具
//
// 铁律：本文件只依赖 Win32 + STL，不 include 任何 ZufyUI 头、不建窗口、不弹 UI。
// 目的：可被 tests/ 在无窗口进程里直接覆盖。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <string>

namespace ze::platform {

// 规范化为绝对路径：反斜杠、折叠 . / ..、去掉末尾多余分隔符（根目录保留）。
// 失败时返回原字符串。
std::wstring NormalizePath(const std::wstring& path);

// 展开 %TEMP%、%SystemRoot% 等环境变量。未知变量保持原样。
std::wstring ExpandEnvironmentVariables(const std::wstring& path);

// 轻量路径探测。失败或不存在统一返回 false。
bool PathExists(const std::wstring& path);
bool IsDirectoryPath(const std::wstring& path);

// 加上扩展长度前缀（\\?\ 或 \\?\UNC\...）。已带前缀则原样返回。
// 注意：加了前缀后不再支持 . / .. 与相对路径 —— 必须先 NormalizePath。
std::wstring ToExtendedPath(const std::wstring& path);

// 去掉 \\?\ 前缀，用于交给 Shell / 第三方组件。
std::wstring StripExtendedPath(const std::wstring& path);

// 拼接目录与子项名（不重复添加分隔符）。
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);

// 父目录；根目录返回自身。
std::wstring ParentPath(const std::wstring& path);

// 最后一段名称；根目录返回 "C:\" 这类形式。
std::wstring FileNameOf(const std::wstring& path);

// 扩展名（含点，小写）。无扩展名返回空串。
std::wstring ExtensionOf(const std::wstring& path);

// 常见 Windows 可执行入口：EXE/COM/BAT/CMD/PS1/SCR/MSI。
bool IsExecutablePath(const std::wstring& path);

// Shell 已注册的扩展名（有文件类型关联）。用于“隐藏已知扩展名”。
bool IsKnownFileExtension(const std::wstring& path);

bool IsRootPath(const std::wstring& path);

// 人类可读大小："1.2 MB"、"980 KB"、目录传 0 时返回空串。
std::wstring FormatSize(uint64_t bytes);

// 本地时间："2026-11-14 19:03"。FILETIME 为 0 时返回空串。
std::wstring FormatFileTime(const FILETIME& ft);

// 便于日志与错误信息：Win32 错误码 -> 可读描述（已去除尾部换行）。
std::wstring FormatWin32Error(DWORD error);

} // namespace ze::platform
