#include "platform/NtfsStreams.h"

#include "platform/PathUtil.h"

#include <algorithm>
#include <vector>

namespace ze::platform {

namespace {

constexpr std::size_t kCopyBufferBytes = 1024u * 1024u;

void SetError(std::wstring* error, const wchar_t* text, DWORD code = 0) {
    if (!error) return;
    *error = text;
    if (code != 0) *error += L"：" + FormatWin32Error(code);
}

std::wstring StreamPath(const std::wstring& basePath, const std::wstring& name) {
    return ToExtendedPath(basePath) + L":" + name;
}

void AddIssue(OpResult* result, const std::wstring& path,
              const std::wstring& target, const std::wstring& message) {
    if (result) result->issues.push_back(OpIssue{ path, message, target, 0 });
}

bool BaseFileReady(const std::wstring& basePath, std::wstring* error) {
    const DWORD attr = GetFileAttributesW(basePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"无法访问基础文件", GetLastError());
        return false;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        SetError(error, L"目录不支持 NTFS 命名数据流");
        return false;
    }
    return true;
}

struct ReadOnlyGuard {
    std::wstring path;
    DWORD original = INVALID_FILE_ATTRIBUTES;
    bool changed = false;

    bool Prepare(std::wstring* error) {
        original = GetFileAttributesW(path.c_str());
        if (original == INVALID_FILE_ATTRIBUTES) {
            SetError(error, L"无法读取基础文件属性", GetLastError());
            return false;
        }
        if ((original & FILE_ATTRIBUTE_READONLY) == 0) return true;
        if (!SetFileAttributesW(path.c_str(), original & ~FILE_ATTRIBUTE_READONLY)) {
            SetError(error, L"无法临时清除只读属性", GetLastError());
            return false;
        }
        changed = true;
        return true;
    }

    bool Restore(std::wstring* error) {
        if (!changed) return true;
        if (SetFileAttributesW(path.c_str(), original)) return true;
        SetError(error, L"数据流已处理，但恢复只读属性失败", GetLastError());
        return false;
    }
};

bool PumpCopy(HANDLE source, HANDLE target, std::uint64_t total,
              const std::wstring& current, const ProgressFn& progress,
              OpResult* result) {
    std::vector<char> buffer(kCopyBufferBytes);
    OpProgress state;
    state.totalBytes = total;
    state.totalFiles = 1;
    state.current = current;

    if (progress && !progress(state)) {
        if (result) result->canceled = true;
        return false;
    }

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            AddIssue(result, current, L"", L"读取数据流失败：" + FormatWin32Error(GetLastError()));
            return false;
        }
        if (read == 0) break;

        DWORD written = 0;
        if (!WriteFile(target, buffer.data(), read, &written, nullptr) || written != read) {
            AddIssue(result, current, L"", L"写入数据流失败：" + FormatWin32Error(GetLastError()));
            return false;
        }
        state.doneBytes += written;
        if (progress && !progress(state)) {
            if (result) result->canceled = true;
            return false;
        }
    }

    state.doneFiles = 1;
    state.doneBytes = total;
    if (progress) progress(state);
    if (result) ++result->okCount;
    return true;
}

} // namespace

bool ListAlternateStreams(const std::wstring& basePath,
                          std::vector<AlternateStream>* streams,
                          std::wstring* error) {
    if (!streams) return false;
    streams->clear();
    if (!BaseFileReady(basePath, error)) return false;

    WIN32_FIND_STREAM_DATA data{};
    HANDLE find = FindFirstStreamW(ToExtendedPath(basePath).c_str(),
                                   FindStreamInfoStandard, &data, 0);
    if (find == INVALID_HANDLE_VALUE) {
        SetError(error, L"无法枚举数据流", GetLastError());
        return false;
    }

    do {
        std::wstring raw = data.cStreamName;
        AlternateStream item;
        item.size = static_cast<std::uint64_t>(data.StreamSize.QuadPart);
        if (raw == L"::$DATA") {
            item.isDefault = true;
        } else {
            if (!raw.empty() && raw.front() == L':') raw.erase(raw.begin());
            const std::wstring suffix = L":$DATA";
            if (raw.size() >= suffix.size() &&
                raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
                raw.resize(raw.size() - suffix.size());
            }
            item.name = std::move(raw);
        }
        streams->push_back(std::move(item));
    } while (FindNextStreamW(find, &data));

    const DWORD last = GetLastError();
    FindClose(find);
    if (last != ERROR_HANDLE_EOF && last != ERROR_SUCCESS) {
        SetError(error, L"枚举数据流未完整完成", last);
        return false;
    }

    std::sort(streams->begin(), streams->end(), [](const AlternateStream& a,
                                                   const AlternateStream& b) {
        if (a.isDefault != b.isDefault) return a.isDefault;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

bool ValidateAlternateStreamName(const std::wstring& name, std::wstring* error) {
    if (name.empty()) {
        SetError(error, L"数据流名称不能为空");
        return false;
    }
    if (name.size() > 255) {
        SetError(error, L"数据流名称过长");
        return false;
    }
    if (name == L"." || name == L"..") {
        SetError(error, L"数据流名称无效");
        return false;
    }
    for (wchar_t c : name) {
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' ||
            c == L'<' || c == L'>' || c == L'"' || c == L'|' ||
            c == L'?' || c == L'*') {
            SetError(error, L"数据流名称包含非法字符");
            return false;
        }
    }
    return true;
}

bool CopyFileToAlternateStream(const std::wstring& basePath,
                               const std::wstring& streamName,
                               const std::wstring& sourceFile,
                               bool overwrite,
                               const ProgressFn& progress,
                               OpResult* result) {
    if (!ValidateAlternateStreamName(streamName, nullptr)) {
        AddIssue(result, basePath, L"", L"数据流名称无效");
        return false;
    }
    if (!BaseFileReady(basePath, nullptr)) {
        AddIssue(result, basePath, L"", L"基础文件不可用");
        return false;
    }

    const DWORD sourceAttr = GetFileAttributesW(sourceFile.c_str());
    if (sourceAttr == INVALID_FILE_ATTRIBUTES || (sourceAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        AddIssue(result, sourceFile, L"", L"请选择普通文件作为导入源");
        return false;
    }

    const std::wstring stream = StreamPath(basePath, streamName);
    if (!overwrite && GetFileAttributesW(stream.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AddIssue(result, sourceFile, basePath + L":" + streamName, L"同名数据流已存在");
        return false;
    }

    ReadOnlyGuard guard{ basePath };
    std::wstring error;
    if (!guard.Prepare(&error)) {
        AddIssue(result, basePath, L"", error);
        return false;
    }

    HANDLE src = CreateFileW(ToExtendedPath(sourceFile).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE dst = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
        if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
        guard.Restore(nullptr);
        AddIssue(result, sourceFile, basePath + L":" + streamName,
                 L"无法打开文件或数据流：" + FormatWin32Error(code));
        return false;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(src, &size);
    const bool ok = PumpCopy(src, dst, static_cast<std::uint64_t>(size.QuadPart),
                             basePath + L":" + streamName, progress, result);
    CloseHandle(src);
    CloseHandle(dst);
    if (!ok && !(result && result->canceled)) DeleteFileW(stream.c_str());
    if (result && result->canceled) DeleteFileW(stream.c_str());
    guard.Restore(nullptr);
    return ok;
}

bool CopyAlternateStreamToFile(const std::wstring& basePath,
                               const std::wstring& streamName,
                               const std::wstring& destinationFile,
                               bool overwrite,
                               const ProgressFn& progress,
                               OpResult* result) {
    if (!ValidateAlternateStreamName(streamName, nullptr) || !BaseFileReady(basePath, nullptr)) {
        AddIssue(result, basePath, L"", L"数据流或基础文件无效");
        return false;
    }
    if (!overwrite && GetFileAttributesW(destinationFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AddIssue(result, basePath + L":" + streamName, destinationFile, L"目标文件已存在");
        return false;
    }

    const std::wstring stream = StreamPath(basePath, streamName);
    HANDLE src = CreateFileW(stream.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const std::wstring temporary = MakeUniquePath(destinationFile + L".zexplorer-tmp");
    HANDLE dst = CreateFileW(ToExtendedPath(temporary).c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
        if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
        DeleteFileW(ToExtendedPath(temporary).c_str());
        AddIssue(result, basePath + L":" + streamName, destinationFile,
                 L"无法打开数据流或目标文件：" + FormatWin32Error(code));
        return false;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(src, &size);
    const bool ok = PumpCopy(src, dst, static_cast<std::uint64_t>(size.QuadPart),
                             basePath + L":" + streamName, progress, result);
    CloseHandle(src);
    CloseHandle(dst);
    if (!ok) {
        DeleteFileW(ToExtendedPath(temporary).c_str());
        return false;
    }

    if (!MoveFileExW(ToExtendedPath(temporary).c_str(),
                     ToExtendedPath(destinationFile).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(ToExtendedPath(temporary).c_str());
        if (result && result->okCount > 0) --result->okCount;
        AddIssue(result, basePath + L":" + streamName, destinationFile,
                 L"无法替换导出目标文件：" + FormatWin32Error(code));
        return false;
    }
    return true;
}

bool DeleteAlternateStream(const std::wstring& basePath,
                           const std::wstring& streamName,
                           std::wstring* error) {
    if (!ValidateAlternateStreamName(streamName, error) || !BaseFileReady(basePath, error))
        return false;

    ReadOnlyGuard guard{ basePath };
    if (!guard.Prepare(error)) return false;
    const std::wstring stream = StreamPath(basePath, streamName);
    if (!DeleteFileW(stream.c_str())) {
        const DWORD code = GetLastError();
        guard.Restore(nullptr);
        SetError(error, L"删除数据流失败", code);
        return false;
    }
    return guard.Restore(error);
}

bool ReadAlternateStreamText(const std::wstring& basePath,
                             const std::wstring& streamName,
                             std::wstring* text,
                             std::wstring* error) {
    if (!text || !ValidateAlternateStreamName(streamName, error) ||
        !BaseFileReady(basePath, error)) {
        return false;
    }
    const std::wstring stream = StreamPath(basePath, streamName);
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"无法打开数据流", GetLastError());
        return false;
    }
    std::vector<char> bytes(64u * 1024u);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                             &read, nullptr);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        SetError(error, L"读取数据流失败", code);
        return false;
    }
    if (read == bytes.size()) {
        SetError(error, L"数据流超过 64KB，请用导出功能处理");
        return false;
    }

    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    bytes.data(), static_cast<int>(read), nullptr, 0);
    UINT cp = CP_UTF8;
    if (chars <= 0) {
        cp = CP_ACP;
        chars = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(read), nullptr, 0);
    }
    if (chars < 0) {
        SetError(error, L"数据流文本解码失败");
        return false;
    }
    text->assign(static_cast<size_t>(chars), L'\0');
    if (chars > 0 && MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(read),
                                         text->data(), chars) <= 0) {
        SetError(error, L"数据流文本解码失败", GetLastError());
        return false;
    }
    return true;
}

bool WriteAlternateStreamText(const std::wstring& basePath,
                              const std::wstring& streamName,
                              const std::wstring& text,
                              bool overwrite,
                              std::wstring* error) {
    if (!ValidateAlternateStreamName(streamName, error) || !BaseFileReady(basePath, error))
        return false;
    const std::wstring stream = StreamPath(basePath, streamName);
    if (!overwrite && GetFileAttributesW(stream.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"同名数据流已存在");
        return false;
    }
    ReadOnlyGuard guard{ basePath };
    if (!guard.Prepare(error)) return false;

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes < 0) {
        guard.Restore(nullptr);
        SetError(error, L"数据流文本编码失败", GetLastError());
        return false;
    }
    std::vector<char> utf8(static_cast<size_t>(bytes));
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            utf8.data(), bytes, nullptr, nullptr);
    }
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        guard.Restore(nullptr);
        SetError(error, L"创建数据流失败", code);
        return false;
    }
    DWORD written = 0;
    const BOOL ok = bytes == 0 ||
        (WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
                   &written, nullptr) && written == utf8.size());
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(stream.c_str());
        guard.Restore(nullptr);
        SetError(error, L"写入数据流失败", code);
        return false;
    }
    return guard.Restore(error);
}

bool RenameAlternateStream(const std::wstring& basePath,
                           const std::wstring& oldName,
                           const std::wstring& newName,
                           const ProgressFn& progress,
                           OpResult* result) {
    if (!ValidateAlternateStreamName(oldName, nullptr) ||
        !ValidateAlternateStreamName(newName, nullptr) ||
        _wcsicmp(oldName.c_str(), newName.c_str()) == 0 ||
        !BaseFileReady(basePath, nullptr)) {
        AddIssue(result, basePath + L":" + oldName, basePath + L":" + newName,
                 L"数据流名称无效或未变化");
        return false;
    }
    const std::wstring oldStream = StreamPath(basePath, oldName);
    const std::wstring newStream = StreamPath(basePath, newName);
    if (GetFileAttributesW(newStream.c_str()) != INVALID_FILE_ATTRIBUTES) {
        AddIssue(result, basePath + L":" + oldName, basePath + L":" + newName,
                 L"目标数据流已存在");
        return false;
    }

    ReadOnlyGuard guard{ basePath };
    if (!guard.Prepare(nullptr)) {
        AddIssue(result, basePath, L"", L"无法临时清除只读属性");
        return false;
    }
    HANDLE src = CreateFileW(oldStream.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE dst = CreateFileW(newStream.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE || dst == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
        if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);
        DeleteFileW(newStream.c_str());
        guard.Restore(nullptr);
        AddIssue(result, basePath + L":" + oldName, basePath + L":" + newName,
                 L"无法打开数据流：" + FormatWin32Error(code));
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(src, &size);
    const bool copied = PumpCopy(src, dst, static_cast<std::uint64_t>(size.QuadPart),
                                 basePath + L":" + newName, progress, result);
    CloseHandle(src);
    CloseHandle(dst);
    if (!copied || (result && result->canceled)) {
        DeleteFileW(newStream.c_str());
        guard.Restore(nullptr);
        return false;
    }
    if (!DeleteFileW(oldStream.c_str())) {
        const DWORD code = GetLastError();
        DeleteFileW(newStream.c_str());
        guard.Restore(nullptr);
        AddIssue(result, basePath + L":" + oldName, basePath + L":" + newName,
                 L"删除旧数据流失败：" + FormatWin32Error(code));
        return false;
    }
    if (!guard.Restore(nullptr)) {
        AddIssue(result, basePath, L"", L"数据流已重命名，但恢复只读属性失败");
        return false;
    }
    return true;
}

} // namespace ze::platform
