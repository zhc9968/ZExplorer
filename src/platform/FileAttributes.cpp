#include "platform/FileAttributes.h"

#include "platform/PathUtil.h"

#include <winioctl.h>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace ze::platform {

namespace {

constexpr DWORD kEditableAttributeMask =
    FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
    FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
    FILE_ATTRIBUTE_PINNED | FILE_ATTRIBUTE_UNPINNED;
constexpr DWORD kMaxZoneBytes = 64u * 1024u;

std::wstring ZonePath(const std::wstring& path) {
    return path + L":Zone.Identifier";
}

void SetError(std::wstring* error, const wchar_t* text, DWORD code) {
    if (!error) return;
    *error = text;
    if (code != 0) *error += L"：" + FormatWin32Error(code);
}

} // namespace

bool ReadFileAttributes(const std::wstring& path, FileAttributesInfo* out,
                        std::wstring* error) {
    if (!out) return false;
    const DWORD raw = GetFileAttributesW(path.c_str());
    if (raw == INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"无法读取文件属性", GetLastError());
        return false;
    }

    *out = {};
    out->raw = raw;
    out->readOnly = (raw & FILE_ATTRIBUTE_READONLY) != 0;
    out->hidden = (raw & FILE_ATTRIBUTE_HIDDEN) != 0;
    out->system = (raw & FILE_ATTRIBUTE_SYSTEM) != 0;
    out->archive = (raw & FILE_ATTRIBUTE_ARCHIVE) != 0;
    out->normal = (raw & FILE_ATTRIBUTE_NORMAL) != 0;
    out->temporary = (raw & FILE_ATTRIBUTE_TEMPORARY) != 0;
    out->directory = (raw & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->device = (raw & FILE_ATTRIBUTE_DEVICE) != 0;
    out->reparsePoint = (raw & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out->compressed = (raw & FILE_ATTRIBUTE_COMPRESSED) != 0;
    out->encrypted = (raw & FILE_ATTRIBUTE_ENCRYPTED) != 0;
    out->integrityStream = (raw & FILE_ATTRIBUTE_INTEGRITY_STREAM) != 0;
    out->virtualFile = (raw & FILE_ATTRIBUTE_VIRTUAL) != 0;
    out->noScrubData = (raw & FILE_ATTRIBUTE_NO_SCRUB_DATA) != 0;
    out->ea = (raw & FILE_ATTRIBUTE_EA) != 0;
    out->pinned = (raw & FILE_ATTRIBUTE_PINNED) != 0;
    out->unpinned = (raw & FILE_ATTRIBUTE_UNPINNED) != 0;
    out->offline = (raw & FILE_ATTRIBUTE_OFFLINE) != 0;
    out->sparse = (raw & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    out->notContentIndexed = (raw & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0;
    return true;
}

bool SetBasicFileAttributes(const std::wstring& path, bool readOnly, bool hidden,
                            bool system, bool archive, std::wstring* error) {
    FileAttributesInfo currentInfo;
    if (!ReadFileAttributes(path, &currentInfo, error)) return false;
    return SetEditableFileAttributes(
        path, readOnly, hidden, system, archive,
        currentInfo.temporary, currentInfo.notContentIndexed,
        currentInfo.pinned, currentInfo.unpinned, error);
}

bool SetEditableFileAttributes(const std::wstring& path, bool readOnly, bool hidden,
                               bool system, bool archive, bool temporary,
                               bool notContentIndexed, bool pinned, bool unpinned,
                               std::wstring* error) {
    const DWORD current = GetFileAttributesW(path.c_str());
    if (current == INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"无法读取文件属性", GetLastError());
        return false;
    }

    DWORD desired = current & ~kEditableAttributeMask;
    if (readOnly) desired |= FILE_ATTRIBUTE_READONLY;
    if (hidden) desired |= FILE_ATTRIBUTE_HIDDEN;
    if (system) desired |= FILE_ATTRIBUTE_SYSTEM;
    if (archive) desired |= FILE_ATTRIBUTE_ARCHIVE;
    if (temporary) desired |= FILE_ATTRIBUTE_TEMPORARY;
    if (notContentIndexed) desired |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if (pinned) desired |= FILE_ATTRIBUTE_PINNED;
    if (unpinned) desired |= FILE_ATTRIBUTE_UNPINNED;

    if (!SetFileAttributesW(path.c_str(), desired)) {
        SetError(error, L"无法保存文件属性", GetLastError());
        return false;
    }
    return true;
}

bool SetFileCompression(const std::wstring& path, bool compressed,
                        std::wstring* error) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"无法读取文件属性", GetLastError());
        return false;
    }
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"无法打开文件设置压缩属性", GetLastError());
        return false;
    }
    USHORT format = compressed ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(file, FSCTL_SET_COMPRESSION, &format,
                                    sizeof(format), nullptr, 0, &returned, nullptr);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        SetError(error, L"设置压缩属性失败", code);
        return false;
    }
    return true;
}

bool SetFileEncryption(const std::wstring& path, bool encrypted,
                       std::wstring* error) {
    const BOOL ok = encrypted ? EncryptFileW(path.c_str())
                              : DecryptFileW(path.c_str(), 0);
    if (!ok) {
        SetError(error, encrypted ? L"加密文件失败" : L"解密文件失败", GetLastError());
        return false;
    }
    return true;
}

ZoneIdentifier ReadZoneIdentifier(const std::wstring& path) {
    ZoneIdentifier result;
    const std::wstring stream = ZonePath(path);
    HANDLE h = CreateFileW(stream.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        result.errorCode = GetLastError();
        result.status = (result.errorCode == ERROR_FILE_NOT_FOUND ||
                         result.errorCode == ERROR_PATH_NOT_FOUND)
            ? ZoneIdentifierStatus::Missing
            : ZoneIdentifierStatus::Error;
        return result;
    }

    std::vector<char> bytes;
    bytes.resize(4096);
    DWORD total = 0;
    for (;;) {
        if (total == bytes.size()) {
            if (bytes.size() >= kMaxZoneBytes) break;
            bytes.resize((std::min)(bytes.size() * 2, static_cast<size_t>(kMaxZoneBytes)));
        }
        DWORD read = 0;
        if (!ReadFile(h, bytes.data() + total,
                      static_cast<DWORD>(bytes.size() - total), &read, nullptr)) {
            result.errorCode = GetLastError();
            result.status = ZoneIdentifierStatus::Error;
            CloseHandle(h);
            return result;
        }
        if (read == 0) break;
        total += read;
    }
    CloseHandle(h);

    bytes.resize(total);
    if (bytes.empty()) {
        result.status = ZoneIdentifierStatus::Ok;
        return result;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   bytes.data(), static_cast<int>(bytes.size()),
                                   nullptr, 0);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (wlen <= 0) {
        result.errorCode = GetLastError();
        result.status = ZoneIdentifierStatus::Error;
        return result;
    }

    std::wstring text(static_cast<size_t>(wlen), L'\0');
    const UINT cp = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        bytes.data(), static_cast<int>(bytes.size()),
                                        text.data(), wlen) > 0 ? CP_UTF8 : CP_ACP;
    if (MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()),
                            text.data(), wlen) <= 0) {
        result.errorCode = GetLastError();
        result.status = ZoneIdentifierStatus::Error;
        return result;
    }

    result.status = ZoneIdentifierStatus::Ok;
    result.content = std::move(text);
    return result;
}

bool WriteZoneIdentifier(const std::wstring& path, const std::wstring& content,
                         std::wstring* error) {
    const std::wstring stream = ZonePath(path);
    const DWORD originalAttributes = GetFileAttributesW(path.c_str());
    if (originalAttributes == INVALID_FILE_ATTRIBUTES) {
        SetError(error, L"无法读取文件属性", GetLastError());
        return false;
    }

    bool clearedReadOnly = false;
    if (originalAttributes & FILE_ATTRIBUTE_READONLY) {
        if (!SetFileAttributesW(path.c_str(), originalAttributes & ~FILE_ATTRIBUTE_READONLY)) {
            SetError(error, L"无法临时清除只读属性", GetLastError());
            return false;
        }
        clearedReadOnly = true;
    }
    auto restoreReadOnly = [&]() {
        if (!clearedReadOnly) return true;
        if (SetFileAttributesW(path.c_str(), originalAttributes)) return true;
        SetError(error, L"Zone.Identifier 已处理，但恢复只读属性失败", GetLastError());
        return false;
    };

    if (content.empty()) {
        if (DeleteFileW(stream.c_str())) return restoreReadOnly();
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return restoreReadOnly();
        SetError(error, L"无法删除 Zone.Identifier", code);
        restoreReadOnly();
        return false;
    }

    const int need = WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                                         static_cast<int>(content.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        SetError(error, L"无法编码 Zone.Identifier", GetLastError());
        restoreReadOnly();
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), static_cast<int>(content.size()),
                        bytes.data(), need, nullptr, nullptr);

    HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SetError(error, L"无法创建 Zone.Identifier", GetLastError());
        restoreReadOnly();
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok || written != bytes.size()) {
        SetError(error, L"写入 Zone.Identifier 失败", code);
        restoreReadOnly();
        return false;
    }
    return restoreReadOnly();
}

} // namespace ze::platform
