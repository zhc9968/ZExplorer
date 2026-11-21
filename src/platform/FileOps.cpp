// 本文件**不使用 PCH、不包含任何 ZufyUI 头**（见 AGENTS.md §2）。
#include "platform/FileOps.h"

#include "platform/PathUtil.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace ze::platform {

namespace {

// SHFileOperation 的错误码 -> 人话
std::wstring FormatRecycleError(int rc) {
    switch (rc) {
    case 2:  return L"找不到文件（可能已经被删除或移动）";
    case 5:  return L"拒绝访问：文件只读或被占用";
    case 7:  return L"操作被系统拒绝（权限不足）";
    case 32: return L"文件正在被其他程序使用";
    case 81: return L"无法读取源文件";
    case 115: return L"目标目录不允许改名或删除";
    case 122: return L"路径过长或名称非法";
    default: return L"删除到回收站失败（代码 " + std::to_wstring(rc) + L"）";
    }
}

void SetError(std::wstring* error, const std::wstring& text) {
    if (error) *error = text;
}

void SetLastErrorText(std::wstring* error, const std::wstring& what) {
    if (!error) return;
    *error = what + L"：" + FormatWin32Error(GetLastError());
}

// 递归统计（文件数 / 字节数），用于进度分母。
bool MeasureTree(const std::wstring& path, uint64_t* bytes, uint32_t* files) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToExtendedPath(path) + L"\\*").c_str(),
                                FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = JoinPath(path, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            MeasureTree(child, bytes, files);
        } else {
            *bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            ++(*files);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return true;
}

struct CopyCtx {
    const ProgressFn* progress = nullptr;
    OpProgress state;
    bool cancelled = false;
    OpResult* result = nullptr;
    CopyOptions options;
    int policy = 0;              // 0 询问 1 全覆盖 2 全跳过

    void Fail(const std::wstring& path, const std::wstring& msg) { Fail(path, msg, 0); }
    void Fail(const std::wstring& path, const std::wstring& msg, int kind,
              const std::wstring& target = L"") {
        if (result) result->issues.push_back({ path, msg, target, kind });
        if (options.onFile) options.onFile(FileEventKind::Failed, path, 0, 0);
    }
    void AddBytes(std::uint64_t) {}
};

// 分块复制：每块 1 MB，边拷边回调 -> UI 能看到单个大文件的百分比
bool CopyFileChunked(const std::wstring& src, const std::wstring& dst, CopyCtx& ctx) {
    HANDLE in = CreateFileW(ToExtendedPath(src).c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        ctx.Fail(src, L"打开失败：" + FormatWin32Error(GetLastError()), 0, dst);
        return false;
    }

    HANDLE out = CreateFileW(ToExtendedPath(dst).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        // 目标是只读文件：Windows 的复制会先去掉只读，这里照做
        const DWORD attrs = GetFileAttributesW(ToExtendedPath(dst).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(ToExtendedPath(dst).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            out = CreateFileW(ToExtendedPath(dst).c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (out == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
        // A shell/drag source can carry a path whose parent is reported as
        // transiently missing. Make the destination directory explicit and
        // retry once instead of reporting a misleading copy failure.
        CreateDirectoryPath(ParentPath(dst), nullptr);
        out = CreateFileW(ToExtendedPath(dst).c_str(), GENERIC_WRITE, 0, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        CloseHandle(in);
        ctx.Fail(src, L"创建目标失败：" + FormatWin32Error(err), 0, dst);
        return false;
    }

    std::vector<char> buffer(1024 * 1024);
    uint64_t copied = 0;
    bool ok = true;

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ctx.Fail(src, L"读取失败：" + FormatWin32Error(GetLastError()), 0, dst);
            ok = false;
            break;
        }
        if (read == 0) break;

        DWORD written = 0;
        if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
            ctx.Fail(src, L"写入失败：" + FormatWin32Error(GetLastError()), 0, dst);
            ok = false;
            break;
        }

        copied += read;
        ctx.state.doneBytes += read;
        ctx.AddBytes(read);

        if (ctx.options.onFile) {
            ctx.options.onFile(FileEventKind::Progress, src, ctx.state.totalBytes, copied);
        }
        if (ctx.progress && *ctx.progress) {
            if (!(*ctx.progress)(ctx.state)) { ctx.cancelled = true; ok = false; break; }
        }
    }

    CloseHandle(in);
    CloseHandle(out);

    if (!ok) {
        DeleteFileW(ToExtendedPath(dst).c_str());   // 半截文件不留
        return false;
    }

    // 保留时间戳
    HANDLE hSrc = CreateFileW(ToExtendedPath(src).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSrc != INVALID_HANDLE_VALUE) {
        FILETIME create{}, access{}, write{};
        if (GetFileTime(hSrc, &create, &access, &write)) {
            HANDLE hDst = CreateFileW(ToExtendedPath(dst).c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hDst != INVALID_HANDLE_VALUE) {
                SetFileTime(hDst, &create, &access, &write);
                CloseHandle(hDst);
            }
        }
        CloseHandle(hSrc);
    }
    return true;
}

// 目标已存在 -> 问策略（Ask 由 options.onConflict 决定）
bool ResolveConflict(const std::wstring& src, const std::wstring& dst, CopyCtx& ctx, bool* skip) {
    *skip = false;
    if (GetFileAttributesW(ToExtendedPath(dst).c_str()) == INVALID_FILE_ATTRIBUTES) return true;

    if (ctx.policy == 1) return true;          // 全部覆盖
    if (ctx.policy == 2) {                                  // 全部跳过
        *skip = true;
        return true;
    }

    ConflictAction action = ConflictAction::Overwrite;
    if (ctx.options.onConflict) action = ctx.options.onConflict(src, dst);

    switch (action) {
    case ConflictAction::Overwrite: return true;
    case ConflictAction::Skip:
        *skip = true;
        return true;
    case ConflictAction::OverwriteAll: ctx.policy = 1; return true;
    case ConflictAction::SkipAll:
        ctx.policy = 2;
        *skip = true;
        return true;
    }
    return true;
}



bool CopyRecursive(const std::wstring& src, const std::wstring& dst, CopyCtx& ctx) {
    if (ctx.cancelled) return false;

    // 每个条目开始前问一次是否取消：0 字节文件不会进分块循环，
    // 只在循环里检查会让取消对它们失效。
    if (ctx.progress && *ctx.progress && !(*ctx.progress)(ctx.state)) {
        ctx.cancelled = true;
        return false;
    }

    const DWORD attrs = GetFileAttributesW(ToExtendedPath(src).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        ctx.Fail(src, L"源不存在：" + FormatWin32Error(GetLastError()), 0, dst);
        return true;
    }

    // 符号链接 / 联接点：递归进去可能成环（例如 junction 指回上级目录），跳过并记录
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        ctx.Fail(src, L"跳过符号链接 / 联接点（避免递归成环）", 0, dst);
        return true;
    }

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        bool skip = false;
        ResolveConflict(src, dst, ctx, &skip);
        if (skip) return true;
        ctx.state.current = src;
        if (ctx.progress && *ctx.progress && !(*ctx.progress)(ctx.state)) {
            ctx.cancelled = true;
            return false;
        }
        if (ctx.options.onFile) {
            uint64_t size = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(ToExtendedPath(src).c_str(), GetFileExInfoStandard, &fad)) {
                size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            }
            ctx.options.onFile(FileEventKind::Start, src, size, 0);
        }

        if (!CopyFileChunked(src, dst, ctx)) return true;   // 已记录失败，继续

        if (ctx.result) ++ctx.result->okCount;
        ++ctx.state.doneFiles;
        ctx.state.current = src;
        if (ctx.options.onFile) ctx.options.onFile(FileEventKind::Done, src, 0, 0);
        return true;
    }

    if (!CreateDirectoryW(ToExtendedPath(dst).c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        ctx.Fail(dst, L"创建目录失败：" + FormatWin32Error(GetLastError()));
        return true;
    }

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToExtendedPath(src) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        ctx.Fail(src, L"枚举失败：" + FormatWin32Error(GetLastError()));
        return true;
    }

    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!CopyRecursive(JoinPath(src, name), JoinPath(dst, name), ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return !ctx.cancelled;
}
bool DeleteRecursive(const std::wstring& path, std::wstring* error) {
    const DWORD attrs = GetFileAttributesW(ToExtendedPath(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;

    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (attrs & FILE_ATTRIBUTE_READONLY) {
            SetFileAttributesW(ToExtendedPath(path).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
        if (DeleteFileW(ToExtendedPath(path).c_str())) return true;
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        SetLastErrorText(error, L"删除失败 " + path);
        return false;
    }

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToExtendedPath(path) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (!DeleteRecursive(JoinPath(path, name), error)) { FindClose(h); return false; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (RemoveDirectoryW(ToExtendedPath(path).c_str())) return true;
    SetLastErrorText(error, L"删除目录失败 " + path);
    return false;
}

} // namespace
void MeasureTreeStats(const std::wstring& path, std::uint64_t* bytes, std::uint32_t* files) {
    if (bytes) *bytes = 0;
    if (files) *files = 0;
    const DWORD attrs = GetFileAttributesW(ToExtendedPath(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(ToExtendedPath(path).c_str(), GetFileExInfoStandard, &fad)) {
            if (bytes) *bytes = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (files) *files = 1;
        }
        return;
    }
    MeasureTree(path, bytes, files);
}

void CopyTree(const std::wstring& src, const std::wstring& dst,
              const ProgressFn& progress, OpResult* result, const CopyOptions& options) {
    // 目标在源目录内部（例如把 C:\a 复制到 C:\a\b）会边复制边枚举，无限递归。
    // 这种情况必须直接拒绝。
    const std::wstring srcNorm = ToExtendedPath(src);
    const std::wstring dstNorm = ToExtendedPath(dst);
    // 注意必须带分隔符比较：否则 "...\sub" 会把 "...\sub_copy" 误判成子目录
    const std::wstring srcPrefix = srcNorm + L'\\';
    if (dstNorm.size() > srcPrefix.size() &&
        _wcsnicmp(dstNorm.c_str(), srcPrefix.c_str(), srcPrefix.size()) == 0) {
        if (result) result->issues.push_back({ dst, L"目标在源目录内部，已拒绝（会无限递归）", L"", 0 });
        return;
    }

    CopyCtx ctx;
    ctx.progress = &progress;
    ctx.result = result;
    ctx.options = options;
    MeasureTree(src, &ctx.state.totalBytes, &ctx.state.totalFiles);

    CopyRecursive(src, dst, ctx);
    if (!ctx.cancelled && (!result || result->issues.empty())) {
        ctx.state.doneFiles = ctx.state.totalFiles;
        ctx.state.doneBytes = ctx.state.totalBytes;
    }
    ctx.state.current = src;
    if (ctx.progress && *ctx.progress) (*ctx.progress)(ctx.state);
    if (result) result->canceled = ctx.cancelled;
}

void MoveTree(const std::wstring& src, const std::wstring& dst,
              const ProgressFn& progress, OpResult* result, const CopyOptions& options) {
    if (MoveFileW(ToExtendedPath(src).c_str(), ToExtendedPath(dst).c_str())) {
        if (result) ++result->okCount;
        return;
    }

    // 跨卷或目标已存在：复制 + 删除源
    CopyTree(src, dst, progress, result, options);
    if (result && (result->canceled || !result->issues.empty())) return;

    OpResult del;
    DeletePermanent(src, &del);
    if (result) {
        result->canceled = result->canceled || del.canceled;
        for (auto& issue : del.issues) result->issues.push_back(issue);
    }
}

// ---------------------------------------------------------------------------
// 删除
// ---------------------------------------------------------------------------
void DeleteToRecycleBin(const std::vector<std::wstring>& paths, OpResult* result) {
    if (paths.empty()) return;

    // SHFileOperationW 需要"双空结尾"的路径串
    std::wstring buffer;
    for (const auto& p : paths) {
        buffer += StripExtendedPath(p);
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int rc = SHFileOperationW(&op);
    if (rc == 0 && !op.fAnyOperationsAborted) {
        if (result) result->okCount += static_cast<std::uint32_t>(paths.size());
        return;
    }

    if (op.fAnyOperationsAborted) {
        if (result) result->canceled = true;
        return;
    }

    // 批量失败时不知道是哪一个——逐个重试，把错误精确归到具体文件上
    for (const auto& p : paths) {
        std::wstring buffer = StripExtendedPath(p);
        buffer.push_back(L'\0');
        buffer.push_back(L'\0');

        SHFILEOPSTRUCTW one{};
        one.wFunc = FO_DELETE;
        one.pFrom = buffer.c_str();
        one.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

        const int oneRc = SHFileOperationW(&one);
        if (!result) continue;
        if (oneRc == 0 && !one.fAnyOperationsAborted) {
            ++result->okCount;
        } else if (one.fAnyOperationsAborted) {
            result->canceled = true;
        } else {
            result->issues.push_back({ p, FormatRecycleError(oneRc), L"", 0 });
        }
    }
}

void DeletePermanent(const std::wstring& path, OpResult* result) {
    std::wstring error;
    if (DeleteRecursive(path, &error)) {
        if (result) ++result->okCount;
        return;
    }
    if (result) result->issues.push_back({ path, error });
}
bool RenamePath(const std::wstring& from, const std::wstring& to, std::wstring* error) {
    if (MoveFileW(ToExtendedPath(from).c_str(), ToExtendedPath(to).c_str())) return true;
    SetLastErrorText(error, L"重命名失败 " + from + L" -> " + to);
    return false;
}

bool CreateDirectoryPath(const std::wstring& path, std::wstring* error) {
    if (CreateDirectoryW(ToExtendedPath(path).c_str(), nullptr)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;
    SetLastErrorText(error, L"新建文件夹失败 " + path);
    return false;
}

bool CreateEmptyFile(const std::wstring& path, std::wstring* error) {
    HANDLE h = CreateFileW(ToExtendedPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SetLastErrorText(error, L"新建文件失败 " + path);
        return false;
    }
    CloseHandle(h);
    return true;
}

std::wstring MakeUniquePath(const std::wstring& desired) {
    if (GetFileAttributesW(ToExtendedPath(desired).c_str()) == INVALID_FILE_ATTRIBUTES)
        return desired;

    const std::wstring parent = ParentPath(desired);
    const std::wstring name = FileNameOf(desired);
    const size_t dot = name.find_last_of(L'.');
    const bool hasExt = (dot != std::wstring::npos && dot > 0);
    const std::wstring stem = hasExt ? name.substr(0, dot) : name;
    const std::wstring ext = hasExt ? name.substr(dot) : L"";

    for (int i = 2; i < 1000; ++i) {
        const std::wstring candidate = JoinPath(parent, stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (GetFileAttributesW(ToExtendedPath(candidate).c_str()) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return desired;
}

// ---------------------------------------------------------------------------
// 剪贴板
// ---------------------------------------------------------------------------
namespace {
constexpr UINT kPreferredDropEffect = 0;

UINT PreferredDropEffectFormat() {
    static const UINT fmt = RegisterClipboardFormatW(L"Preferred DropEffect");
    return fmt;
}
} // namespace

bool SetClipboardFiles(const std::vector<std::wstring>& paths, bool cut, std::wstring* error) {
    if (paths.empty()) return false;

    // DROPFILES + 双空结尾的宽字符路径串
    std::wstring list;
    for (const auto& p : paths) {
        list += StripExtendedPath(p);
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    const size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) { SetError(error, L"GlobalAlloc 失败"); return false; }

    auto* drop = static_cast<DROPFILES*>(GlobalLock(mem));
    if (!drop) { GlobalFree(mem); SetError(error, L"GlobalLock 失败"); return false; }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES), list.data(),
           list.size() * sizeof(wchar_t));
    GlobalUnlock(mem);

    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (effect) {
        if (auto* p = static_cast<DWORD*>(GlobalLock(effect))) {
            *p = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(effect);
        }
    }

    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        if (effect) GlobalFree(effect);
        SetError(error, L"打开剪贴板失败");
        return false;
    }
    EmptyClipboard();

    bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
    if (ok && effect) {
        SetClipboardData(PreferredDropEffectFormat(), effect);   // 所有权转移给系统
        effect = nullptr;
    }
    CloseClipboard();

    if (!ok) {
        GlobalFree(mem);
        if (effect) GlobalFree(effect);
        SetError(error, L"写入剪贴板失败");
        return false;
    }
    return true;
}

bool GetClipboardFiles(std::vector<std::wstring>* paths, bool* cut) {
    if (!paths) return false;
    paths->clear();
    if (cut) *cut = false;

    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (hDrop) {
        const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring buf(len + 1, L'\0');
            if (DragQueryFileW(hDrop, i, buf.data(), len + 1) > 0) {
                buf.resize(wcslen(buf.c_str()));
                paths->push_back(buf);
            }
        }
        ok = !paths->empty();
    }

    if (cut) {
        HANDLE data = GetClipboardData(PreferredDropEffectFormat());
        if (auto* p = data ? static_cast<DWORD*>(GlobalLock(data)) : nullptr) {
            *cut = (*p & DROPEFFECT_MOVE) != 0;
            GlobalUnlock(data);
        }
    }

    CloseClipboard();
    return ok;
}

bool ClipboardHasFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

bool SetClipboardText(const std::wstring& text, std::wstring* error) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) {
        SetError(error, L"GlobalAlloc 失败");
        return false;
    }
    auto* dest = static_cast<wchar_t*>(GlobalLock(mem));
    if (!dest) {
        GlobalFree(mem);
        SetError(error, L"GlobalLock 失败");
        return false;
    }
    memcpy(dest, text.c_str(), bytes);
    GlobalUnlock(mem);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(mem);
        SetError(error, L"打开剪贴板失败");
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) {
        GlobalFree(mem);
        SetError(error, L"写入文本剪贴板失败");
    }
    return ok;
}

bool IsCtrlDown() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool IsShiftDown() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }
bool IsAltDown() { return (GetKeyState(VK_MENU) & 0x8000) != 0; }

} // namespace ze::platform
