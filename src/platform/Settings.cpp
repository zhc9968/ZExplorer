// 本文件不使用 PCH、不依赖 ZufyUI。
#include "platform/Settings.h"

#include "platform/PathUtil.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ze::platform {

namespace {

constexpr wchar_t kKeyPath[] = L"Software\\ZExplorer";
constexpr wchar_t kValueAutoElevate[] = L"AutoElevate";
constexpr wchar_t kValuePauseOnError[] = L"PauseOnError";
constexpr wchar_t kValueFocusOnError[] = L"FocusOnError";
constexpr wchar_t kValueExecutableViaShell[] = L"ExecutableViaShell";
constexpr wchar_t kValueShowHiddenFiles[] = L"ShowHiddenFiles";
constexpr wchar_t kValueHideKnownExtensions[] = L"HideKnownExtensions";
constexpr wchar_t kValueQuickAccess[] = L"QuickAccessPins";
constexpr wchar_t kValueStartupMode[] = L"StartupMode";
constexpr wchar_t kValueStartupPath[] = L"StartupPath";
constexpr wchar_t kValueLastFolder[] = L"LastFolder";
constexpr wchar_t kValueSortKey[] = L"SortKey";
constexpr wchar_t kValueSortAscending[] = L"SortAscending";
constexpr wchar_t kValueConfirmRecycleDelete[] = L"ConfirmRecycleDelete";
constexpr wchar_t kValueWindowX[] = L"WindowX";
constexpr wchar_t kValueWindowY[] = L"WindowY";
constexpr wchar_t kValueWindowWidth[] = L"WindowWidth";
constexpr wchar_t kValueWindowHeight[] = L"WindowHeight";
constexpr wchar_t kValueMainSplitRatio[] = L"MainSplitRatio";
constexpr wchar_t kValueQuickSplitRatio[] = L"QuickSplitRatio";
constexpr wchar_t kValueColumn0[] = L"Column0";
constexpr wchar_t kValueColumn1[] = L"Column1";
constexpr wchar_t kValueColumn2[] = L"Column2";
constexpr wchar_t kValueColumn3[] = L"Column3";
constexpr wchar_t kValueMaxConcurrent[] = L"MaxConcurrent";
constexpr wchar_t kValueTaskShowDelay[] = L"TaskShowDelayMs";
constexpr wchar_t kValueTaskDoneLinger[] = L"TaskDoneLingerMs";
constexpr wchar_t kValueTaskRetention[] = L"TaskRetentionMinutes";
constexpr wchar_t kValueBeepOnError[] = L"BeepOnError";
constexpr wchar_t kValueAutoStartHost[] = L"AutoStartHost";
constexpr wchar_t kValueShellContextMenu[] = L"ShellContextMenu";
constexpr wchar_t kValueDefaultFileManager[] = L"DefaultFileManager";
constexpr wchar_t kValueWinEShortcut[] = L"WinEShortcut";
struct ShellMenuSpec {
    const wchar_t* root;
    const wchar_t* argument;
    bool multiSelectModel;
};
constexpr ShellMenuSpec kShellMenuSpecs[] = {
    { L"Software\\Classes\\DesktopBackground\\Shell\\ZExplorer.Open",
      L"\"%V\"", false },
    { L"Software\\Classes\\Directory\\Background\\shell\\ZExplorer.Open",
      L"\"%V\"", false },
    { L"Software\\Classes\\*\\shell\\ZExplorer.Open",
      L"\"%1\"", true },
    { L"Software\\Classes\\Directory\\shell\\ZExplorer.Open",
      L"\"%1\"", true },
    { L"Software\\Classes\\Drive\\shell\\ZExplorer.Open",
      L"\"%1\"", true }
};
constexpr wchar_t kLegacyShellMenuRoots[][64] = {
    L"Software\\Classes\\DesktopBackground\\Shell\\ZExplorer",
    L"Software\\Classes\\Directory\\Background\\shell\\ZExplorer",
    L"Software\\Classes\\Directory\\shell\\ZExplorer",
    L"Software\\Classes\\Drive\\shell\\ZExplorer"
};
constexpr wchar_t kFileManagerBackup[] =
    L"Software\\ZExplorer\\Backup\\DefaultFileManager";
constexpr wchar_t kFileManagerClasses[][64] = {
    L"Software\\Classes\\Directory\\shell\\open\\command",
    L"Software\\Classes\\Folder\\shell\\open\\command",
    L"Software\\Classes\\Drive\\shell\\open\\command"
};
constexpr wchar_t kFileManagerBackupLabels[][32] = {
    L"Directory", L"Folder", L"Drive"
};

bool ReadDefaultValue(HKEY root, const wchar_t* subkey, bool* exists,
                      std::wstring* value, bool* expand = nullptr) {
    if (exists) *exists = false;
    if (value) value->clear();
    if (expand) *expand = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return true;
    }
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return false;
    }
    std::wstring data(bytes / sizeof(wchar_t) + 1, L'\0');
    result = RegQueryValueExW(
        key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(data.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;
    data.resize(bytes / sizeof(wchar_t));
    while (!data.empty() && data.back() == L'\0') data.pop_back();
    if (exists) *exists = true;
    if (value) *value = std::move(data);
    if (expand) *expand = type == REG_EXPAND_SZ;
    return true;
}

bool WriteDefaultValue(HKEY root, const wchar_t* subkey,
                       const std::wstring& value, bool expand = false) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(
        key, nullptr, 0, expand ? REG_EXPAND_SZ : REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void DeleteDefaultValue(HKEY root, const wchar_t* subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, nullptr);
    RegCloseKey(key);
}

std::wstring BackupField(const wchar_t* label, const wchar_t* field) {
    return std::wstring(label) + L"." + field;
}

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    return type == REG_DWORD ? value : fallback;
}

std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

float ReadScaledFloat(HKEY key, const wchar_t* name, float fallback) {
    const DWORD raw = ReadDword(key, name, static_cast<DWORD>(std::lround(fallback * 1000.0f)));
    return static_cast<float>(raw) / 1000.0f;
}

} // namespace

AppSettings LoadSettings() {
    AppSettings settings;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return settings;   // 没写过 = 全用默认值
    }

    settings.autoElevate = ReadDword(key, kValueAutoElevate, 0) != 0;
    settings.pauseOnError = ReadDword(key, kValuePauseOnError, 0) != 0;
    settings.focusOnError = ReadDword(key, kValueFocusOnError, 1) != 0;
    settings.executableViaShell =
        ReadDword(key, kValueExecutableViaShell, 1) != 0;
    settings.showHiddenFiles =
        ReadDword(key, kValueShowHiddenFiles, 0) != 0;
    settings.hideKnownExtensions =
        ReadDword(key, kValueHideKnownExtensions, 0) != 0;
    settings.startupMode = static_cast<StartupMode>(
        ReadDword(key, kValueStartupMode, 0));
    settings.startupPath = ReadString(key, kValueStartupPath);
    settings.lastFolder = ReadString(key, kValueLastFolder);
    settings.sortKey = static_cast<int>(ReadDword(key, kValueSortKey, 0));
    settings.sortAscending = ReadDword(key, kValueSortAscending, 1) != 0;
    settings.confirmRecycleDelete =
        ReadDword(key, kValueConfirmRecycleDelete, 1) != 0;
    settings.windowX = static_cast<int>(ReadDword(key, kValueWindowX, CW_USEDEFAULT));
    settings.windowY = static_cast<int>(ReadDword(key, kValueWindowY, CW_USEDEFAULT));
    settings.windowWidth = static_cast<int>(ReadDword(key, kValueWindowWidth, 1200));
    settings.windowHeight = static_cast<int>(ReadDword(key, kValueWindowHeight, 760));
    settings.mainSplitRatio = ReadScaledFloat(key, kValueMainSplitRatio, 0.22f);
    settings.quickSplitRatio = ReadScaledFloat(key, kValueQuickSplitRatio, 0.32f);
    settings.column0 = ReadScaledFloat(key, kValueColumn0, 380.0f);
    settings.column1 = ReadScaledFloat(key, kValueColumn1, 110.0f);
    settings.column2 = ReadScaledFloat(key, kValueColumn2, 170.0f);
    settings.column3 = ReadScaledFloat(key, kValueColumn3, 180.0f);
    settings.maxConcurrentTasks = static_cast<int>(ReadDword(key, kValueMaxConcurrent, 4));
    settings.taskShowDelayMs = static_cast<int>(ReadDword(key, kValueTaskShowDelay, 350));
    settings.taskDoneLingerMs = static_cast<int>(ReadDword(key, kValueTaskDoneLinger, 1200));
    settings.taskRetentionMinutes = static_cast<int>(ReadDword(key, kValueTaskRetention, 15));
    settings.beepOnError = ReadDword(key, kValueBeepOnError, 0) != 0;
    settings.autoStartHost = ReadDword(key, kValueAutoStartHost, 0) != 0;
    settings.shellContextMenu = ReadDword(key, kValueShellContextMenu, 0) != 0;
    settings.defaultFileManager = ReadDword(key, kValueDefaultFileManager, 0) != 0;
    settings.winEShortcut = ReadDword(key, kValueWinEShortcut, 0) != 0;
    RegCloseKey(key);
    return settings;
}

bool SaveSettings(const AppSettings& settings, std::wstring* error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0,
                                  KEY_WRITE, nullptr, &key, &disposition);
    if (result != ERROR_SUCCESS) {
        if (error) *error = L"无法打开设置存储：" + FormatWin32Error(result);
        return false;
    }

    LONG firstError = ERROR_SUCCESS;
    auto noteError = [&](LONG code) {
        if (firstError == ERROR_SUCCESS && code != ERROR_SUCCESS) firstError = code;
    };
    auto writeDword = [key, &noteError](const wchar_t* name, DWORD value) {
        noteError(RegSetValueExW(key, name, 0, REG_DWORD,
                                 reinterpret_cast<const BYTE*>(&value),
                                 sizeof(value)));
    };

    writeDword(kValueAutoElevate, settings.autoElevate ? 1u : 0u);
    writeDword(kValuePauseOnError, settings.pauseOnError ? 1u : 0u);
    writeDword(kValueFocusOnError, settings.focusOnError ? 1u : 0u);
    writeDword(kValueExecutableViaShell,
               settings.executableViaShell ? 1u : 0u);
    writeDword(kValueShowHiddenFiles, settings.showHiddenFiles ? 1u : 0u);
    writeDword(kValueHideKnownExtensions,
               settings.hideKnownExtensions ? 1u : 0u);
    auto writeString = [key, &noteError](const wchar_t* name,
                                         const std::wstring& value) {
        noteError(RegSetValueExW(
            key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
    };
    writeDword(kValueStartupMode, static_cast<DWORD>(settings.startupMode));
    writeString(kValueStartupPath, settings.startupPath);
    writeString(kValueLastFolder, settings.lastFolder);
    writeDword(kValueSortKey, static_cast<DWORD>(std::clamp(settings.sortKey, 0, 3)));
    writeDword(kValueSortAscending, settings.sortAscending ? 1u : 0u);
    writeDword(kValueConfirmRecycleDelete, settings.confirmRecycleDelete ? 1u : 0u);
    writeDword(kValueWindowX, static_cast<DWORD>(settings.windowX));
    writeDword(kValueWindowY, static_cast<DWORD>(settings.windowY));
    writeDword(kValueWindowWidth, static_cast<DWORD>((std::max)(320, settings.windowWidth)));
    writeDword(kValueWindowHeight, static_cast<DWORD>((std::max)(240, settings.windowHeight)));
    writeDword(kValueMainSplitRatio, static_cast<DWORD>(std::lround(
        std::clamp(settings.mainSplitRatio, 0.0f, 1.0f) * 1000.0f)));
    writeDword(kValueQuickSplitRatio, static_cast<DWORD>(std::lround(
        std::clamp(settings.quickSplitRatio, 0.0f, 1.0f) * 1000.0f)));
    writeDword(kValueColumn0, static_cast<DWORD>(std::lround((std::max)(0.0f, settings.column0) * 1000.0f)));
    writeDword(kValueColumn1, static_cast<DWORD>(std::lround((std::max)(0.0f, settings.column1) * 1000.0f)));
    writeDword(kValueColumn2, static_cast<DWORD>(std::lround((std::max)(0.0f, settings.column2) * 1000.0f)));
    writeDword(kValueColumn3, static_cast<DWORD>(std::lround((std::max)(0.0f, settings.column3) * 1000.0f)));
    writeDword(kValueMaxConcurrent, static_cast<DWORD>(std::clamp(settings.maxConcurrentTasks, 1, 16)));
    writeDword(kValueTaskShowDelay, static_cast<DWORD>(std::clamp(settings.taskShowDelayMs, 0, 10000)));
    writeDword(kValueTaskDoneLinger, static_cast<DWORD>(std::clamp(settings.taskDoneLingerMs, 0, 60000)));
    writeDword(kValueTaskRetention, static_cast<DWORD>(std::clamp(settings.taskRetentionMinutes, 1, 1440)));
    writeDword(kValueBeepOnError, settings.beepOnError ? 1u : 0u);
    writeDword(kValueAutoStartHost, settings.autoStartHost ? 1u : 0u);
    writeDword(kValueShellContextMenu, settings.shellContextMenu ? 1u : 0u);
    writeDword(kValueDefaultFileManager, settings.defaultFileManager ? 1u : 0u);
    writeDword(kValueWinEShortcut, settings.winEShortcut ? 1u : 0u);
    RegCloseKey(key);
    if (firstError != ERROR_SUCCESS) {
        if (error) *error = L"无法写入设置存储：" + FormatWin32Error(firstError);
        return false;
    }
    return true;
}

void ResetSettings() {
    SetDefaultFileManager(false);
    SetAutoStartHost(false);
    SetShellContextMenu(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, kKeyPath);
}

bool SetAutoStartHost(bool enabled, std::wstring* error) {
    wchar_t exe[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (length == 0 || length >= ARRAYSIZE(exe)) {
        if (error) *error = L"无法取得程序路径";
        return false;
    }

    HKEY key = nullptr;
    const wchar_t* runPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, runPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        if (error) *error = L"无法打开开机启动项";
        return false;
    }
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + std::wstring(exe) + L"\" --host";
        result = RegSetValueExW(key, L"ZExplorerHost", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"ZExplorerHost");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        if (error) *error = L"无法更新开机启动项：" + FormatWin32Error(result);
        return false;
    }
    return true;
}

bool SetShellContextMenu(bool enabled, std::wstring* error) {
    wchar_t exe[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (length == 0 || length >= ARRAYSIZE(exe)) {
        if (error) *error = L"无法取得程序路径";
        return false;
    }

    for (const wchar_t* legacy : kLegacyShellMenuRoots)
        RegDeleteTreeW(HKEY_CURRENT_USER, legacy);

    if (!enabled) {
        for (const auto& spec : kShellMenuSpecs)
            RegDeleteTreeW(HKEY_CURRENT_USER, spec.root);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT,
                       nullptr, nullptr);
        return true;
    }

    std::wstring firstError;
    auto rememberError = [&](std::wstring message) {
        if (firstError.empty()) firstError = std::move(message);
    };

    for (const auto& spec : kShellMenuSpecs) {
        RegDeleteTreeW(HKEY_CURRENT_USER, spec.root);

        HKEY rootKey = nullptr;
        const LONG rootResult =
            RegCreateKeyExW(HKEY_CURRENT_USER, spec.root, 0, nullptr, 0, KEY_WRITE,
                            nullptr, &rootKey, nullptr);
        if (rootResult != ERROR_SUCCESS) {
            rememberError(L"无法注册资源管理器菜单：" + std::wstring(spec.root) +
                          L"\n" +
                          FormatWin32Error(static_cast<DWORD>(rootResult)));
            continue;
        }
        auto writeText = [rootKey](const wchar_t* name, const std::wstring& value) {
            return RegSetValueExW(
                rootKey, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
                ERROR_SUCCESS;
        };
        const bool menuOk =
            writeText(L"MUIVerb", L"用 ZExplorer 打开") &&
            writeText(L"Icon", std::wstring(exe) + L",0") &&
            (!spec.multiSelectModel ||
             writeText(L"MultiSelectModel", L"Single"));
        RegCloseKey(rootKey);
        if (!menuOk) {
            rememberError(L"无法写入资源管理器菜单显示信息：" +
                          std::wstring(spec.root));
            continue;
        }

        const std::wstring commandPath =
            std::wstring(spec.root) + L"\\command";
        HKEY commandKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, commandPath.c_str(), 0, nullptr, 0,
                            KEY_WRITE, nullptr, &commandKey, nullptr) !=
            ERROR_SUCCESS) {
            rememberError(L"无法注册资源管理器菜单命令：" + commandPath);
            continue;
        }
        const std::wstring command =
            L"\"" + std::wstring(exe) + L"\" " + spec.argument;
        const LONG result = RegSetValueExW(
            commandKey, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(commandKey);
        if (result != ERROR_SUCCESS) {
            rememberError(L"无法写入资源管理器菜单命令：" +
                          FormatWin32Error(static_cast<DWORD>(result)));
            continue;
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT,
                   nullptr, nullptr);
    if (!firstError.empty()) {
        if (error) *error = firstError;
        return false;
    }
    return true;
}

bool SetDefaultFileManager(bool enabled, std::wstring* error) {
    wchar_t exe[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (length == 0 || length >= ARRAYSIZE(exe)) {
        if (error) *error = L"无法取得程序路径";
        return false;
    }

    HKEY backup = nullptr;
    const bool hasBackup =
        RegOpenKeyExW(HKEY_CURRENT_USER, kFileManagerBackup, 0, KEY_READ,
                      &backup) == ERROR_SUCCESS &&
        ReadDword(backup, L"Saved", 0) != 0;
    if (backup) {
        RegCloseKey(backup);
        backup = nullptr;
    }

    if (!enabled) {
        if (!hasBackup) {
            // Nothing was backed up: do not touch shell defaults. This also makes
            // ResetSettings safe on a machine where this feature was never used.
            return true;
        }

        if (RegOpenKeyExW(HKEY_CURRENT_USER, kFileManagerBackup, 0, KEY_READ,
                          &backup) != ERROR_SUCCESS) {
            if (error) *error = L"无法读取文件管理器接管备份";
            return false;
        }
        bool ok = true;
        for (size_t i = 0; i < std::size(kFileManagerClasses); ++i) {
            const std::wstring existsName =
                BackupField(kFileManagerBackupLabels[i], L"Exists");
            const std::wstring valueName =
                BackupField(kFileManagerBackupLabels[i], L"Value");
            const std::wstring expandName =
                BackupField(kFileManagerBackupLabels[i], L"Expand");
            const bool existed = ReadDword(backup, existsName.c_str(), 0) != 0;
            if (!existed) {
                DeleteDefaultValue(HKEY_CURRENT_USER, kFileManagerClasses[i]);
                continue;
            }
            const std::wstring value = ReadString(backup, valueName.c_str());
            const bool expand =
                ReadDword(backup, expandName.c_str(), 0) != 0;
            if (!WriteDefaultValue(HKEY_CURRENT_USER, kFileManagerClasses[i],
                                   value, expand))
                ok = false;
        }
        RegCloseKey(backup);
        if (!ok) {
            if (error) *error = L"恢复原来的文件夹打开命令失败";
            return false;
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, kFileManagerBackup);
        return true;
    }

    if (!hasBackup) {
        HKEY newBackup = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kFileManagerBackup, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &newBackup, nullptr) !=
            ERROR_SUCCESS) {
            if (error) *error = L"无法创建文件管理器接管备份";
            return false;
        }
        bool ok = true;
        for (size_t i = 0; i < std::size(kFileManagerClasses); ++i) {
            bool existed = false;
            bool expand = false;
            std::wstring value;
            if (!ReadDefaultValue(HKEY_CURRENT_USER, kFileManagerClasses[i],
                                  &existed, &value, &expand)) {
                ok = false;
                break;
            }
            const std::wstring existsName =
                BackupField(kFileManagerBackupLabels[i], L"Exists");
            const DWORD existsValue = existed ? 1u : 0u;
            if (RegSetValueExW(
                    newBackup, existsName.c_str(), 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&existsValue),
                    sizeof(existsValue)) != ERROR_SUCCESS) {
                ok = false;
                break;
            }
            if (existed) {
                const std::wstring valueName =
                    BackupField(kFileManagerBackupLabels[i], L"Value");
                const std::wstring expandName =
                    BackupField(kFileManagerBackupLabels[i], L"Expand");
                const DWORD expandValue = expand ? 1u : 0u;
                if (RegSetValueExW(
                        newBackup, valueName.c_str(), 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) !=
                        ERROR_SUCCESS ||
                    RegSetValueExW(
                        newBackup, expandName.c_str(), 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&expandValue),
                        sizeof(expandValue)) != ERROR_SUCCESS) {
                    ok = false;
                    break;
                }
            }
        }
        const DWORD saved = ok ? 1u : 0u;
        if (ok) {
            RegSetValueExW(newBackup, L"Saved", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&saved), sizeof(saved));
        }
        RegCloseKey(newBackup);
        if (!ok) {
            RegDeleteTreeW(HKEY_CURRENT_USER, kFileManagerBackup);
            if (error) *error = L"备份当前文件夹打开命令失败";
            return false;
        }
    }

    const std::wstring command = L"\"" + std::wstring(exe) + L"\" \"%1\"";
    for (const wchar_t* classPath : kFileManagerClasses) {
        if (!WriteDefaultValue(HKEY_CURRENT_USER, classPath, command)) {
            if (error) *error = L"写入文件夹默认打开命令失败";
            return false;
        }
    }
    return true;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                                        &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::vector<std::wstring> LoadQuickAccessPins() {
    std::vector<std::wstring> pins;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return pins;

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, kValueQuickAccess, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        (type != REG_MULTI_SZ && type != REG_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return pins;
    }

    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, kValueQuickAccess, nullptr, &type,
                         reinterpret_cast<LPBYTE>(data.data()), &bytes) ==
        ERROR_SUCCESS) {
        const size_t count = bytes / sizeof(wchar_t);
        size_t start = 0;
        for (size_t i = 0; i < count; ++i) {
            if (data[i] != L'\0') continue;
            if (i > start) pins.emplace_back(data.data() + start, i - start);
            start = i + 1;
        }
    }
    RegCloseKey(key);
    return pins;
}

bool SaveQuickAccessPins(const std::vector<std::wstring>& pins,
                         std::wstring* error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKeyPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, &disposition) != ERROR_SUCCESS) {
        if (error) *error = L"无法打开快捷访问设置";
        return false;
    }

    bool ok = true;
    if (pins.empty()) {
        const LONG result = RegDeleteValueW(key, kValueQuickAccess);
        ok = result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    } else {
        std::wstring data;
        for (const auto& pin : pins) {
            data += pin;
            data.push_back(L'\0');
        }
        data.push_back(L'\0');
        ok = RegSetValueExW(
                 key, kValueQuickAccess, 0, REG_MULTI_SZ,
                 reinterpret_cast<const BYTE*>(data.data()),
                 static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (!ok && error) *error = L"无法保存快捷访问设置";
    return ok;
}

bool IsQuickAccessPinned(const std::wstring& path) {
    const std::wstring normalized = NormalizePath(StripExtendedPath(path));
    const auto pins = LoadQuickAccessPins();
    return std::any_of(pins.begin(), pins.end(), [&](const std::wstring& pin) {
        return _wcsicmp(pin.c_str(), normalized.c_str()) == 0;
    });
}

bool PinQuickAccess(const std::wstring& path, std::wstring* error) {
    if (path.empty() || !PathExists(path)) {
        if (error) *error = L"目标不存在";
        return false;
    }
    const std::wstring normalized = NormalizePath(StripExtendedPath(path));
    auto pins = LoadQuickAccessPins();
    const bool exists = std::any_of(pins.begin(), pins.end(), [&](const std::wstring& pin) {
        return _wcsicmp(pin.c_str(), normalized.c_str()) == 0;
    });
    if (!exists) pins.push_back(normalized);
    return SaveQuickAccessPins(pins, error);
}

bool UnpinQuickAccess(const std::wstring& path, std::wstring* error) {
    const std::wstring normalized = NormalizePath(StripExtendedPath(path));
    auto pins = LoadQuickAccessPins();
    pins.erase(std::remove_if(pins.begin(), pins.end(), [&](const std::wstring& pin) {
        return _wcsicmp(pin.c_str(), normalized.c_str()) == 0;
    }), pins.end());
    return SaveQuickAccessPins(pins, error);
}

} // namespace ze::platform
