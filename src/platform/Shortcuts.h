#pragma once

#include <windows.h>
#include <string>

namespace ze::platform {

enum class ShortcutTargetKind {
    NotShortcut,
    ShellItem,
    File,
    Directory,
    MissingPath
};

struct ShortcutInfo {
    bool isShortcut = false;
    ShortcutTargetKind kind = ShortcutTargetKind::NotShortcut;
    std::wstring shortcutPath;
    std::wstring targetPath;
    std::wstring arguments;
    std::wstring workingDirectory;
    std::wstring description;
};

bool IsShortcutPath(const std::wstring& path);
bool ResolveShortcut(const std::wstring& path, ShortcutInfo* info,
                     std::wstring* error = nullptr);

} // namespace ze::platform
