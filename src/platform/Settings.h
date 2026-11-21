#pragma once
// ============================================================================
// platform/Settings.h —— 用户设置（存 HKCU\Software\ZExplorer）
//
// 只用 Win32 + STL，不依赖 ZufyUI，可在无窗口进程里读写。
// ============================================================================

#include <windows.h>
#include <string>
#include <vector>

namespace ze::platform {

enum class StartupMode : int {
    ThisPc = 0,
    LastFolder = 1,
    SpecifiedPath = 2
};

struct AppSettings {
    bool autoElevate = false;     // 启动时自动请求管理员权限
    bool pauseOnError = false;    // 出错时暂停等待处理
    bool focusOnError = true;     // 出错时切到该页签并前置窗口
    bool executableViaShell = true; // EXE/BAT/CMD/PS1 等由当前 UI 的 ShellExecute 打开
    bool showHiddenFiles = false; // 显示隐藏项与系统项
    bool hideKnownExtensions = false; // 隐藏 Shell 已知扩展名

    StartupMode startupMode = StartupMode::ThisPc;
    std::wstring startupPath;
    std::wstring lastFolder;
    int sortKey = 0;              // 0=名称，1=大小，2=类型，3=修改日期
    bool sortAscending = true;
    bool confirmRecycleDelete = true;

    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;
    int windowWidth = 1200;
    int windowHeight = 760;
    float mainSplitRatio = 0.22f;
    float quickSplitRatio = 0.32f;
    float column0 = 380.0f;
    float column1 = 110.0f;
    float column2 = 170.0f;
    float column3 = 180.0f;

    int maxConcurrentTasks = 4;
    int taskShowDelayMs = 350;
    int taskDoneLingerMs = 1200;
    int taskRetentionMinutes = 15;
    bool beepOnError = false;
    bool autoStartHost = false;
    bool shellContextMenu = false;
    bool defaultFileManager = false;
    bool winEShortcut = false;
};

AppSettings LoadSettings();
bool SaveSettings(const AppSettings& settings, std::wstring* error = nullptr);
void ResetSettings();

bool SetAutoStartHost(bool enabled, std::wstring* error = nullptr);
bool SetShellContextMenu(bool enabled, std::wstring* error = nullptr);
bool SetDefaultFileManager(bool enabled, std::wstring* error = nullptr);

bool IsElevated();

// ZExplorer-owned quick access pins. Stored as REG_MULTI_SZ under
// HKCU\Software\ZExplorer\QuickAccessPins. This is independent from Windows
// Explorer's own Quick Access data.
std::vector<std::wstring> LoadQuickAccessPins();
bool SaveQuickAccessPins(const std::vector<std::wstring>& pins,
                         std::wstring* error = nullptr);
bool IsQuickAccessPinned(const std::wstring& path);
bool PinQuickAccess(const std::wstring& path, std::wstring* error = nullptr);
bool UnpinQuickAccess(const std::wstring& path, std::wstring* error = nullptr);

} // namespace ze::platform
