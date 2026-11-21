#pragma once
// ============================================================================
// platform/ShellActions.h —— 交给 Shell 的动作（打开 / 属性 / 驱动器枚举）
//
// 为什么单独一个文件：AGENTS.md §2 规定 UI 层不得直接调 Shell/Win32。
// 打开文件、显示属性、枚举驱动器都是 OS 行为，收在这里。
// 本文件同样只依赖 Win32 + STL，不使用 PCH、不依赖 ZufyUI。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ze::platform {

// 用默认程序打开（文件或文件夹）。失败返回 false。
bool OpenPath(const std::wstring& path, std::wstring* error = nullptr);

// 在系统资源管理器中定位：文件 -> 打开父目录并选中文件；
// 文件夹 -> 直接打开该文件夹。
bool OpenContainingFolder(const std::wstring& path,
                          std::wstring* error = nullptr);

// 打开系统"打开方式"选择器。
bool OpenWith(const std::wstring& path, std::wstring* error = nullptr);

// 在指定工作目录执行 Shell 命令/程序。支持 PATH 中的 cmd.exe、powershell.exe
// 以及带参数命令行；不等待子进程退出。
bool LaunchCommand(const std::wstring& commandLine,
                   const std::wstring& workingDirectory,
                   std::wstring* error = nullptr);

// Show the native Windows Shell context menu for one filesystem target at
// the current cursor position.
bool ShowShellContextMenu(HWND owner, const std::wstring& path);

// 打开系统"属性"对话框。
bool ShowProperties(const std::wstring& path,
                    std::wstring* error = nullptr);

// 取某个路径的 Shell 图标（真实路径，不加 SHGFI_USEFILEATTRIBUTES，
// 所以"驱动器根"会拿到磁盘图标而不是文件夹图标）。返回的 HICON 由调用方 DestroyIcon。
HICON GetShellIconForPath(const std::wstring& path, bool large);

// 从 exe 资源加载应用图标（失败回退到系统默认图标）。用完不必 DestroyIcon。
HICON LoadAppIcon(int cx, int cy);

// 以管理员身份重新启动本程序（UAC）。返回 false 表示用户取消或启动失败。
bool RelaunchElevated();

struct DriveInfo {
    std::wstring root;      // "C:\"
    std::wstring label;     // 卷标，可能为空
    std::wstring display;   // "本地磁盘 (C:)" 之类的展示名
    std::wstring fsType;    // NTFS / exFAT / FAT32 ...
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
    bool ready = false;     // 介质已就绪（可读取容量）
    UINT type = 0;          // DRIVE_FIXED / DRIVE_REMOVABLE / DRIVE_REMOTE ...
};

struct UserFolderInfo {
    std::wstring title;
    std::wstring path;
};

struct RecycleBinInfo {
    bool available = false;
    std::uint64_t itemCount = 0;
    std::uint64_t totalBytes = 0;
};

struct RecycleBinItem {
    std::wstring name;
    std::wstring wrapperPath;     // C:\$Recycle.Bin\...\$R...
    std::wstring originalPath;    // 删除前所在路径
    std::wstring typeName;
    FILETIME deletedAt{};
    std::uint64_t size = 0;
    bool isDir = false;
    std::vector<std::uint8_t> relativePidl;
};

// 枚举可用驱动器（跳过 CD-ROM 空驱动器与未挂载的）。
std::vector<DriveInfo> LogicalDrives();
std::vector<UserFolderInfo> UserFolders();
RecycleBinInfo QueryRecycleBin();
std::vector<RecycleBinItem> QueryRecycleBinItems();
bool ShowRecycleBinContextMenu(
    HWND owner, const std::vector<const RecycleBinItem*>& items);
bool InvokeRecycleBinVerb(
    HWND owner, const std::vector<const RecycleBinItem*>& items,
    const std::wstring& verb, std::wstring* error = nullptr);
bool EmptyRecycleBin(std::wstring* error = nullptr);
bool OpenRecycleBin();

} // namespace ze::platform
