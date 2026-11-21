// 本文件**不使用 PCH、不包含任何 ZufyUI 头**（见 AGENTS.md §2）。
#include "fs/DirectoryModel.h"

#include "platform/PathUtil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <unordered_map>

namespace ze::fs {

using namespace ze::platform;

// ---------------------------------------------------------------------------
// 本地辅助
// ---------------------------------------------------------------------------
namespace {

double NowMs() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// CompareStringEx 返回 CSTR_LESS_THAN/EQUAL/GREATER_THAN(1/2/3)，统一成 -1/0/1。
int CompareNatural(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                                  SORT_DIGITSASNUMBERS | NORM_LINGUISTIC_CASING,
                                  a.c_str(), static_cast<int>(a.size()),
                                  b.c_str(), static_cast<int>(b.size()),
                                  nullptr, nullptr, 0);
    if (r == CSTR_LESS_THAN) return -1;
    if (r == CSTR_GREATER_THAN) return 1;
    return 0;
}

int CompareFileTime2(const FILETIME& a, const FILETIME& b) {
    const ULARGE_INTEGER ua{ a.dwLowDateTime, a.dwHighDateTime };
    const ULARGE_INTEGER ub{ b.dwLowDateTime, b.dwHighDateTime };
    if (ua.QuadPart < ub.QuadPart) return -1;
    if (ua.QuadPart > ub.QuadPart) return 1;
    return 0;
}

// 类型名缓存：按扩展名（目录用特殊键）缓存 SHGetFileInfoW 的结果。
// 用 SHGFI_USEFILEATTRIBUTES 避免每个文件都去碰磁盘 / 触发网络访问。
std::wstring TypeNameFor(const std::wstring& name, bool isDir,
                         std::unordered_map<std::wstring, std::wstring>& cache) {
    const std::wstring key = isDir ? L"<dir>" : ExtensionOf(name);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    SHFILEINFOW sfi{};
    // 目录：随便给个名字 + DIRECTORY 属性；文件：给扩展名即可。
    std::wstring probe = isDir ? L"folder" : (key.empty() ? L"file" : (L"file" + key));
    const DWORD attrs = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    std::wstring result;
    if (SHGetFileInfoW(probe.c_str(), attrs, &sfi, sizeof(sfi),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0) {
        result = sfi.szTypeName;
    }
    if (result.empty()) result = isDir ? L"文件夹" : L"文件";

    cache.emplace(key, result);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
size_t Snapshot::DirCount() const {
    size_t n = 0;
    for (const auto& e : entries) if (e.isDir) ++n;
    return n;
}

size_t Snapshot::FileCount() const { return entries.size() - DirCount(); }

uint64_t Snapshot::TotalFileBytes() const {
    uint64_t total = 0;
    for (const auto& e : entries) if (!e.isDir) total += e.size;
    return total;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
Snapshot DirectoryModel::Load(const std::wstring& path, bool showHidden) {
    Snapshot snap;
    snap.path = NormalizePath(path);
    const double t0 = NowMs();

    if (snap.path.empty()) {
        snap.notFound = true;
        snap.errorCode = ERROR_PATH_NOT_FOUND;
        return snap;
    }

    const std::wstring wildcard = ToExtendedPath(snap.path) + L"\\*";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        switch (err) {
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            snap.accessDenied = true;
            snap.errorCode = err;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        // 「驱动器不存在」也走"找不到"分支，UI 才能给出"返回上级"
        case ERROR_INVALID_DRIVE:
        case ERROR_NOT_READY:
            snap.notFound = true;
            snap.errorCode = err;
            break;
        case ERROR_NO_MORE_FILES:   // 空目录
            snap.errorCode = 0;
            break;
        default:
            snap.errorCode = err;
            break;
        }
        snap.elapsedMs = NowMs() - t0;
        return snap;
    }

    std::unordered_map<std::wstring, std::wstring> typeCache;
    snap.entries.reserve(64);

    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;

        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isHidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        const bool isSystem = (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
        const bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (!showHidden && (isHidden || isSystem)) {
            ++snap.hiddenSkipped;
            continue;
        }

        FileEntry e;
        e.name = name;
        e.fullPath = JoinPath(snap.path, name);
        e.isDir = isDir;
        e.isHidden = isHidden;
        e.isSystem = isSystem;
        e.isReparse = isReparse;
        e.size = isDir ? 0
                       : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.modified = fd.ftLastWriteTime;
        e.typeName = TypeNameFor(name, isDir, typeCache);

        snap.entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));

    const DWORD enumErr = GetLastError();   // 正常结束为 ERROR_NO_MORE_FILES
    FindClose(h);

    if (enumErr != ERROR_NO_MORE_FILES && enumErr != ERROR_SUCCESS) {
        // 枚举中途失败：保留已读到的项，但把错误报上去，UI 显示"部分项目无法显示"。
        snap.errorCode = enumErr;
    }

    Sort(snap, SortKey::Name, SortOrder::Ascending);
    snap.elapsedMs = NowMs() - t0;
    return snap;
}

std::vector<FileEntry> DirectoryModel::Search(
    const std::wstring& path, const std::wstring& query, bool showHidden,
    const std::function<bool()>& canceled,
    const std::function<void(size_t)>& progress,
    size_t maxResults) {
    std::vector<FileEntry> results;
    if (path.empty() || query.empty() || maxResults == 0) return results;

    std::wstring needle = query;
    if (!needle.empty())
        CharLowerBuffW(needle.data(), static_cast<DWORD>(needle.size()));
    size_t visited = 0;

    std::function<void(const std::wstring&)> visit =
        [&](const std::wstring& folderPath) {
            if ((canceled && canceled()) || results.size() >= maxResults)
                return;
            ++visited;
            if (progress) progress(visited);

            const Snapshot snapshot = Load(folderPath, showHidden);
            if (!snapshot.Ok()) return;
            for (const auto& entry : snapshot.entries) {
                if ((canceled && canceled()) || results.size() >= maxResults)
                    return;
                std::wstring name = entry.name;
                std::wstring type = entry.typeName;
                CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
                CharLowerBuffW(type.data(), static_cast<DWORD>(type.size()));
                if (name.find(needle) != std::wstring::npos ||
                    type.find(needle) != std::wstring::npos) {
                    results.push_back(entry);
                }
                if (entry.isDir && !entry.isReparse)
                    visit(entry.fullPath);
            }
        };

    visit(NormalizePath(path));
    Sort(results, SortKey::Name, SortOrder::Ascending);
    return results;
}

// ---------------------------------------------------------------------------
// Sort
// ---------------------------------------------------------------------------
void DirectoryModel::Sort(std::vector<FileEntry>& entries, SortKey key, SortOrder order) {
    const int mul = (order == SortOrder::Ascending) ? 1 : -1;

    std::stable_sort(entries.begin(), entries.end(),
                     [&](const FileEntry& a, const FileEntry& b) {
        // 名称、大小、类型都把目录作为独立分组放前面；大小对目录没有意义。
        // 修改日期则让目录和文件严格按日期混排。
        if (key != SortKey::Modified && a.isDir != b.isDir) return a.isDir;

        int c = 0;
        switch (key) {
        case SortKey::Name:
            c = CompareNatural(a.name, b.name);
            break;
        case SortKey::Modified:
            c = CompareFileTime2(a.modified, b.modified);
            break;
        case SortKey::Type:
            c = CompareNatural(a.typeName, b.typeName);
            if (c == 0) c = CompareNatural(a.name, b.name);
            break;
        case SortKey::Size:
            if (a.size < b.size) c = -1;
            else if (a.size > b.size) c = 1;
            else c = CompareNatural(a.name, b.name);
            break;
        }
        if (c == 0) c = CompareNatural(a.name, b.name);
        return (c * mul) < 0;
    });
}

void DirectoryModel::Sort(Snapshot& snap, SortKey key, SortOrder order) {
    Sort(snap.entries, key, order);
}

// ---------------------------------------------------------------------------
// DisplayNameOf
// ---------------------------------------------------------------------------
std::wstring DirectoryModel::DisplayNameOf(const std::wstring& path) {
    if (path.empty()) return L"";
    if (IsRootPath(path)) return path;
    return FileNameOf(path);
}

} // namespace ze::fs
