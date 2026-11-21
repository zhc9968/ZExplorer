#pragma once
// ============================================================================
// platform/FileAttributes.h - file attribute and NTFS stream access.
//
// Win32 + STL only. No UI/ZufyUI dependency so this layer can be tested
// without creating a window.
// ============================================================================

#include <windows.h>

#include <cstdint>
#include <string>

namespace ze::platform {

struct FileAttributesInfo {
    std::uint32_t raw = 0;
    bool readOnly = false;
    bool hidden = false;
    bool system = false;
    bool archive = false;
    bool normal = false;
    bool temporary = false;
    bool directory = false;
    bool device = false;
    bool reparsePoint = false;
    bool compressed = false;
    bool encrypted = false;
    bool integrityStream = false;
    bool virtualFile = false;
    bool noScrubData = false;
    bool ea = false;
    bool pinned = false;
    bool unpinned = false;
    bool offline = false;
    bool sparse = false;
    bool notContentIndexed = false;
};

enum class ZoneIdentifierStatus {
    Missing,
    Ok,
    Error,
};

struct ZoneIdentifier {
    ZoneIdentifierStatus status = ZoneIdentifierStatus::Missing;
    std::wstring content;
    DWORD errorCode = 0;
};

bool ReadFileAttributes(const std::wstring& path, FileAttributesInfo* out,
                        std::wstring* error = nullptr);

// Only common, safely writable attributes are changed. All other existing
// attribute bits are preserved.
bool SetBasicFileAttributes(const std::wstring& path, bool readOnly, bool hidden,
                            bool system, bool archive, std::wstring* error = nullptr);

bool SetEditableFileAttributes(const std::wstring& path, bool readOnly, bool hidden,
                               bool system, bool archive, bool temporary,
                               bool notContentIndexed, bool pinned, bool unpinned,
                               std::wstring* error = nullptr);

bool SetFileCompression(const std::wstring& path, bool compressed,
                        std::wstring* error = nullptr);
bool SetFileEncryption(const std::wstring& path, bool encrypted,
                       std::wstring* error = nullptr);

ZoneIdentifier ReadZoneIdentifier(const std::wstring& path);

// Empty content deletes the stream. Non-empty content is written as UTF-8.
bool WriteZoneIdentifier(const std::wstring& path, const std::wstring& content,
                         std::wstring* error = nullptr);

} // namespace ze::platform
