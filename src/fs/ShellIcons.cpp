// 本文件**不使用 PCH、不包含任何 ZufyUI 头**（见 AGENTS.md §2）。
#include "fs/ShellIcons.h"

#include "platform/PathUtil.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>

namespace ze::fs {

using namespace ze::platform;

ShellIcons& ShellIcons::Instance() {
    static ShellIcons inst;
    return inst;
}

ShellIcons::~ShellIcons() { Clear(); }

std::wstring ShellIcons::MakeKey(bool isDir, const std::wstring& path,
                                 bool preferFileIcon, IconSize size) {
    wchar_t sz[8];
    swprintf_s(sz, L"%d", static_cast<int>(size));

    std::wstring key;
    key.reserve(64);
    key += isDir ? L"d|" : L"f|";
    key += preferFileIcon ? L"1|" : L"0|";
    key += sz;
    key += L"|";

    if (!preferFileIcon) {
        // 通用图标只看扩展名，避免不同目录下的同名扩展重复占缓存
        key += ExtensionOf(path);
    } else {
        key += path;
    }
    return key;
}

HICON ShellIcons::Get(bool isDir, const std::wstring& path,
                      bool preferFileIcon, IconSize size) {
    if (path.empty()) return nullptr;

    const std::wstring key = MakeKey(isDir, path, preferFileIcon, size);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
    }

    SHFILEINFOW sfi{};
    UINT flags = SHGFI_ICON;
    switch (size) {
    case IconSize::Large:      flags |= SHGFI_LARGEICON; break;
    case IconSize::ExtraLarge: flags |= SHGFI_SHELLICONSIZE; break;
    case IconSize::Small:
    default:                   flags |= SHGFI_SMALLICON; break;
    }

    // 交给 Shell 的路径必须是常规形式（它不认 \\?\）。
    const std::wstring plain = StripExtendedPath(path);

    HICON icon = nullptr;
    if (preferFileIcon) {
        // 真实图标：慢，且会触碰磁盘；只在需要时才用。
        if (SHGetFileInfoW(plain.c_str(), 0, &sfi, sizeof(sfi), flags) != 0)
            icon = sfi.hIcon;
    } else {
        // 按扩展名取通用图标：不碰磁盘。
        const DWORD attrs = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        std::wstring probe;
        if (isDir) {
            // 关键：这里**不能**用真实路径。SHGFI_USEFILEATTRIBUTES 仍然会看路径字符串，
            // 拿 "C:\\" 去问会得到"驱动器图标"，再被缓存成所有目录的图标（曾经全是 C: 图标）。
            probe = L"folder";
        } else {
            const std::wstring ext = ExtensionOf(plain);
            probe = ext.empty() ? L"file" : (L"file" + ext);
        }
        if (SHGetFileInfoW(probe.c_str(), attrs, &sfi, sizeof(sfi),
                           flags | SHGFI_USEFILEATTRIBUTES) != 0)
            icon = sfi.hIcon;
    }

    if (!icon) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        // 并发下别的线程已先建好：丢弃自己这份，返回缓存里的
        DestroyIcon(icon);
        return it->second;
    }
    cache_.emplace(key, icon);
    return icon;
}

void ShellIcons::Clear() {
    std::unordered_map<std::wstring, HICON> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(cache_);
    }
    for (auto& kv : doomed) {
        if (kv.second) DestroyIcon(kv.second);
    }
}

size_t ShellIcons::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

} // namespace ze::fs
