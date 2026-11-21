// 本文件**不使用 PCH、不包含任何 ZufyUI 头**（见 AGENTS.md §2）。
#include "platform/PathUtil.h"

#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace ze::platform {

namespace {

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// 根长度：C:\ -> 3 ;; \\server\share\ -> 长度 ;; 无法判定返回 0。
size_t RootLength(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') {
        size_t n = 2;
        while (n < p.size() && IsSeparator(p[n])) ++n;
        return n; // "C:" 或 "C:\"
    }
    if (p.size() >= 2 && IsSeparator(p[0]) && IsSeparator(p[1])) {
        // UNC：\\server\share
        size_t i = 2, slash = 0;
        while (i < p.size()) {
            if (IsSeparator(p[i])) {
                if (++slash == 2) {
                    ++i;
                    while (i < p.size() && IsSeparator(p[i])) ++i;
                    return i;
                }
            }
            ++i;
        }
        return p.size();
    }
    return 0;
}

} // namespace

std::wstring ExpandEnvironmentVariables(const std::wstring& path) {
    if (path.empty()) return path;

    DWORD required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (required == 0) return path;

    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        path.c_str(), expanded.data(), required);
    if (written == 0 || written > required) return path;
    if (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
    return expanded;
}

bool PathExists(const std::wstring& path) {
    if (path.empty()) return false;
    return GetFileAttributesW(ToExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectoryPath(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(ToExtendedPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return path;

    // 交给系统做权威规范化（处理 . / ..、长短名、重复分隔符）。
    // 注意：GetFullPathNameW 不认 \\?\ 前缀，先剥掉。
    std::wstring plain = StripExtendedPath(path);

    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(plain.c_str(), ARRAYSIZE(buf), buf, nullptr);
    if (n == 0 || n >= ARRAYSIZE(buf)) return path;

    std::wstring out(buf, n);

    // GetFullPathNameW 对根目录返回 "C:\"，对普通路径不会带尾部反斜杠；
    // 但用户可能传了 "C:\dir\"，这里统一去掉尾部（根除外）。
    const size_t rootLen = RootLength(out);
    while (out.size() > rootLen && IsSeparator(out.back())) out.pop_back();

    return out;
}

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;      // 已有前缀
    if (path.rfind(L"\\\\.\\", 0) == 0) return path;      // 设备路径

    if (path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1])) {
        // UNC -> \\?\UNC\server\share\...
        std::wstring rest = path.substr(2);
        return L"\\\\?\\UNC\\" + rest;
    }
    return L"\\\\?\\" + path;
}

std::wstring StripExtendedPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return L"\\\\" + path.substr(8);
    }
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path.substr(4);
    }
    return path;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (IsSeparator(dir.back())) return dir + name;
    return dir + L"\\" + name;
}

std::wstring ParentPath(const std::wstring& path) {
    if (path.empty()) return path;
    const size_t rootLen = RootLength(path);
    if (path.size() <= rootLen) return path;

    size_t pos = path.size();
    while (pos > rootLen && !IsSeparator(path[pos - 1])) --pos;
    if (pos <= rootLen) return path.substr(0, rootLen);

    // 去掉结尾的分隔符（但根保留）
    size_t end = pos;
    while (end > rootLen && IsSeparator(path[end - 1])) --end;
    if (end <= rootLen) return path.substr(0, rootLen);
    return path.substr(0, end);
}

std::wstring FileNameOf(const std::wstring& path) {
    if (path.empty()) return path;
    const size_t rootLen = RootLength(path);
    if (path.size() <= rootLen) return path;

    size_t end = path.size();
    while (end > rootLen && IsSeparator(path[end - 1])) --end;

    size_t pos = end;
    while (pos > rootLen && !IsSeparator(path[pos - 1])) --pos;

    std::wstring name = path.substr(pos, end - pos);
    return name.empty() ? path : name;
}

std::wstring ExtensionOf(const std::wstring& path) {
    const std::wstring name = FileNameOf(path);
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsExecutablePath(const std::wstring& path) {
    if (path.empty() || IsDirectoryPath(path)) return false;
    const std::wstring ext = ExtensionOf(path);
    return ext == L".exe" || ext == L".com" || ext == L".bat" ||
           ext == L".cmd" || ext == L".ps1" || ext == L".scr" ||
           ext == L".msi";
}

bool IsKnownFileExtension(const std::wstring& path) {
    const std::wstring ext = ExtensionOf(path);
    if (ext.empty()) return false;

    auto keyExists = [&](HKEY root, const wchar_t* prefix) {
        std::wstring keyPath = std::wstring(prefix) + ext;
        HKEY key = nullptr;
        const LONG result = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_READ, &key);
        if (result == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
        return false;
    };

    return keyExists(HKEY_CURRENT_USER, L"Software\\Classes\\") ||
           keyExists(HKEY_CLASSES_ROOT, L"");
}

bool IsRootPath(const std::wstring& path) {
    if (path.empty()) return false;
    return path.size() <= RootLength(path);
}

std::wstring FormatSize(uint64_t bytes) {
    if (bytes == 0) return {};
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) { v /= 1024.0; ++unit; }

    wchar_t buf[64];
    if (unit == 0) {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    } else {
        swprintf_s(buf, L"%.1f %s", v, units[unit]);
    }
    return buf;
}

std::wstring FormatFileTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return {};

    FILETIME local{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return {};

    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return {};

    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring FormatWin32Error(DWORD error) {
    LPWSTR msg = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msg), 0, nullptr);

    std::wstring out;
    if (n != 0 && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (msg) LocalFree(msg);

    if (out.empty()) out = L"未知错误";
    return out + L" (0x" + [&] {
        wchar_t b[16]; swprintf_s(b, L"%08X", error); return std::wstring(b);
    }() + L")";
}

} // namespace ze::platform
