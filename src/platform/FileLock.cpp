#include "platform/FileLock.h"

#include "platform/PathUtil.h"

#include <restartmanager.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "Rstrtmgr.lib")

namespace ze::platform {

namespace {
using NtStatus = LONG;
constexpr NtStatus kStatusInfoLengthMismatch = static_cast<NtStatus>(0xC0000004L);
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr size_t kInitialHandleBufferBytes = 4u * 1024u * 1024u;
constexpr size_t kMaximumHandleBufferBytes = 512u * 1024u * 1024u;

struct SystemHandleEntryEx {
    void* object = nullptr;
    ULONG_PTR processId = 0;
    ULONG_PTR handleValue = 0;
    ULONG grantedAccess = 0;
    USHORT creatorBackTraceIndex = 0;
    USHORT objectTypeIndex = 0;
    ULONG handleAttributes = 0;
    ULONG reserved = 0;
};

struct SystemHandleInformationEx {
    ULONG_PTR handleCount = 0;
    ULONG_PTR reserved = 0;
    SystemHandleEntryEx handles[1];
};

using NtQuerySystemInformationFn = NtStatus(NTAPI*)(
    ULONG, void*, ULONG, ULONG*);

struct FileIdentifier {
    DWORD volumeSerial = 0;
    ULONGLONG fileIndex = 0;
    bool valid = false;
};

struct TargetProbe {
    HANDLE handle = INVALID_HANDLE_VALUE;
    FileIdentifier identifier;
    void* object = nullptr;
    USHORT objectTypeIndex = 0;
    bool entryFound = false;
};

struct ProcessHandles {
    std::unordered_map<DWORD, HANDLE> opened;
    std::set<DWORD> restricted;

    HANDLE Get(DWORD pid) {
        auto it = opened.find(pid);
        if (it != opened.end()) return it->second;
        HANDLE process = OpenProcess(
            PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        opened.emplace(pid, process);
        if (!process) restricted.insert(pid);
        return process;
    }

    ~ProcessHandles() {
        for (const auto& item : opened) {
            if (item.second) CloseHandle(item.second);
        }
    }
};

void SetError(std::wstring* error, const std::wstring& text) {
    if (error) *error = text;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    std::vector<wchar_t> path(32768);
    DWORD size = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    return ok ? std::wstring(path.data(), size) : std::wstring();
}

FileIdentifier IdentifierForHandle(HANDLE file) {
    FileIdentifier id;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file, &info)) {
        id.volumeSerial = info.dwVolumeSerialNumber;
        id.fileIndex = (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) |
                       info.nFileIndexLow;
        id.valid = true;
    }
    return id;
}

HANDLE OpenReadShared(const std::wstring& path, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, flags, nullptr);
}

TargetProbe ProbeTarget(const std::wstring& path) {
    TargetProbe probe;
    probe.handle = OpenReadShared(path, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS);
    if (probe.handle == INVALID_HANDLE_VALUE) {
        probe.handle = CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    }
    if (probe.handle != INVALID_HANDLE_VALUE)
        probe.identifier = IdentifierForHandle(probe.handle);
    return probe;
}

HANDLE OpenTypeProbeFile() {
    std::vector<wchar_t> module(32768);
    const DWORD size = GetModuleFileNameW(nullptr, module.data(),
                                         static_cast<DWORD>(module.size()));
    if (size == 0 || size >= module.size()) return INVALID_HANDLE_VALUE;
    return OpenReadShared(std::wstring(module.data(), size));
}

void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                              nullptr, nullptr);
    }
    CloseHandle(token);
}

bool QuerySystemHandles(std::vector<unsigned char>* buffer, std::wstring* error) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        SetError(error, L"无法加载 ntdll.dll");
        return false;
    }
    auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!query) {
        SetError(error, L"系统不支持 NtQuerySystemInformation");
        return false;
    }

    buffer->assign(kInitialHandleBufferBytes, 0);
    ULONG needed = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const ULONG size = static_cast<ULONG>((std::min)(
            buffer->size(), kMaximumHandleBufferBytes));
        const NtStatus status = query(kSystemExtendedHandleInformation,
                                      buffer->data(), size, &needed);
        if (status == 0) return true;
        if (status != kStatusInfoLengthMismatch) {
            wchar_t statusText[32]{};
            swprintf_s(statusText, L"0x%08X", static_cast<unsigned>(status));
            SetError(error, L"读取系统句柄表失败：" + std::wstring(statusText));
            return false;
        }

        size_t next = static_cast<size_t>(needed) + 64u * 1024u;
        next = (std::max)(next, buffer->size() * 2);
        if (next <= buffer->size() || next > kMaximumHandleBufferBytes) {
            SetError(error, L"系统句柄表过大，无法完成扫描");
            return false;
        }
        buffer->resize(next);
    }
    SetError(error, L"读取系统句柄表时缓冲区持续变化");
    return false;
}

const SystemHandleEntryEx* FindHandleEntry(const SystemHandleInformationEx& info,
                                           HANDLE handle) {
    const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(handle);
    const DWORD currentPid = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < info.handleCount; ++i) {
        const auto& entry = info.handles[i];
        if (entry.processId == currentPid && entry.handleValue == raw)
            return &entry;
    }
    return nullptr;
}

std::wstring NormalizeForCompare(const std::wstring& path) {
    std::wstring result = NormalizePath(StripExtendedPath(path));
    for (wchar_t& c : result) c = static_cast<wchar_t>(std::towlower(c));
    return result;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() &&
           NormalizeForCompare(left) == NormalizeForCompare(right);
}

std::wstring DuplicatedHandlePath(HANDLE duplicate) {
    if (GetFileType(duplicate) != FILE_TYPE_DISK) return {};
    std::vector<wchar_t> path(32768);
    const DWORD length = GetFinalPathNameByHandleW(
        duplicate, path.data(), static_cast<DWORD>(path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= path.size()) return {};
    return std::wstring(path.data(), length);
}

bool DuplicatedHandleMatches(HANDLE duplicate, const TargetProbe& target,
                             const std::wstring& path) {
    if (GetFileType(duplicate) != FILE_TYPE_DISK) return false;
    if (target.identifier.valid) {
        const FileIdentifier id = IdentifierForHandle(duplicate);
        if (id.valid && id.volumeSerial == target.identifier.volumeSerial &&
            id.fileIndex == target.identifier.fileIndex) {
            return true;
        }
    }
    return SamePath(DuplicatedHandlePath(duplicate), path);
}

std::wstring DescribeAccess(DWORD access) {
    if (access == 0) return L"查询元数据";

    std::vector<std::wstring> parts;
    if (access & (FILE_READ_DATA | FILE_READ_ATTRIBUTES)) parts.push_back(L"读取");
    if (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES))
        parts.push_back(L"写入");
    if (access & DELETE) parts.push_back(L"删除");
    if (access & (READ_CONTROL | WRITE_DAC | WRITE_OWNER)) parts.push_back(L"安全信息");
    if (access & SYNCHRONIZE) parts.push_back(L"同步");
    if (parts.empty()) parts.push_back(L"其他访问");

    std::wstring result;
    for (const auto& part : parts) {
        if (!result.empty()) result += L" / ";
        result += part;
    }
    return result;
}

LockingProcess& EnsureProcess(std::unordered_map<DWORD, LockingProcess>* byPid,
                              DWORD pid) {
    auto [it, inserted] = byPid->try_emplace(pid);
    LockingProcess& item = it->second;
    if (inserted) {
        item.pid = pid;
        item.path = ProcessPath(pid);
        item.name = item.path.empty() ? L"进程 " + std::to_wstring(pid)
                                      : FileNameOf(item.path);
        item.applicationType = pid == 4 ? L"系统进程" : L"全系统句柄扫描";
    }
    return item;
}

void AddHandleIfMissing(LockingProcess* process, const SystemHandleEntryEx& entry,
                        bool closable) {
    for (const auto& existing : process->handles) {
        if (existing.value == entry.handleValue) return;
    }
    FileHandleReference handle;
    handle.value = static_cast<ULONG_PTR>(entry.handleValue);
    handle.grantedAccess = entry.grantedAccess;
    handle.closable = closable;
    process->handles.push_back(handle);
}

void FinalizeHandles(std::vector<LockingProcess>* processes) {
    for (auto& process : *processes) {
        std::sort(process.handles.begin(), process.handles.end(),
                  [](const FileHandleReference& a, const FileHandleReference& b) {
                      return a.value < b.value;
                  });
        DWORD combinedAccess = 0;
        for (const auto& handle : process.handles)
            combinedAccess |= handle.grantedAccess;
        process.accessSummary = process.handles.empty()
            ? L"仅进程级占用信息"
            : DescribeAccess(combinedAccess);
    }
}

void MergeRestartManager(const std::wstring& path,
                         std::unordered_map<DWORD, LockingProcess>* byPid,
                         FileLockReport* report,
                         std::wstring* warning) {
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    DWORD code = RmStartSession(&session, 0, key);
    if (code != ERROR_SUCCESS) {
        if (warning)
            *warning += L"Restart Manager 启动失败：" + FormatWin32Error(code) + L"。";
        return;
    }

    const wchar_t* files[] = { path.c_str() };
    code = RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr);
    if (code != ERROR_SUCCESS) {
        RmEndSession(session);
        if (warning)
            *warning += L"Restart Manager 注册失败：" + FormatWin32Error(code) + L"。";
        return;
    }

    std::vector<RM_PROCESS_INFO> infos;
    bool completed = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        code = RmGetList(session, &needed, &count, nullptr, &reasons);
        if (code == ERROR_SUCCESS && needed == 0) {
            completed = true;
            break;
        }
        if (code != ERROR_MORE_DATA) break;

        infos.resize(needed);
        count = needed;
        code = RmGetList(session, &needed, &count, infos.data(), &reasons);
        if (code == ERROR_SUCCESS) {
            infos.resize(count);
            completed = true;
            break;
        }
        if (code != ERROR_MORE_DATA) {
            infos.clear();
            break;
        }
    }
    RmEndSession(session);

    if (!completed) {
        if (warning)
            *warning += L"Restart Manager 查询失败：" + FormatWin32Error(code) + L"。";
        return;
    }
    if (report) report->restartManagerCompleted = true;

    for (const RM_PROCESS_INFO& info : infos) {
        LockingProcess& process = EnsureProcess(byPid, info.Process.dwProcessId);
        process.foundByRestartManager = true;
        if (info.strAppName[0]) process.name = info.strAppName;
        std::wstring rmType = L"Restart Manager";
        switch (info.ApplicationType) {
        case RmCritical: rmType = L"不可安全重启"; break;
        case RmExplorer: rmType = L"资源管理器"; break;
        case RmConsole: rmType = L"控制台程序"; break;
        case RmMainWindow: rmType = L"窗口程序"; break;
        case RmOtherWindow: rmType = L"其他窗口"; break;
        case RmService: rmType = L"服务"; break;
        default: break;
        }
        process.applicationType = process.foundByHandleScan
            ? L"句柄扫描 / " + rmType
            : rmType;
    }
}

bool ScanAllFileHandles(const std::wstring& path,
                        std::unordered_map<DWORD, LockingProcess>* byPid,
                        FileLockReport* report,
                        std::wstring* warning,
                        bool includeCurrentProcess) {
    TargetProbe target = ProbeTarget(path);
    HANDLE typeProbe = target.handle;
    if (typeProbe == INVALID_HANDLE_VALUE)
        typeProbe = OpenTypeProbeFile();
    if (typeProbe == INVALID_HANDLE_VALUE) {
        if (warning) *warning += L"无法创建句柄类型探针。";
        return false;
    }

    EnableDebugPrivilege();
    std::vector<unsigned char> buffer;
    std::wstring scanError;
    if (!QuerySystemHandles(&buffer, &scanError)) {
        if (target.handle != INVALID_HANDLE_VALUE) CloseHandle(target.handle);
        if (typeProbe != target.handle) CloseHandle(typeProbe);
        if (warning) *warning += scanError + L"。";
        return false;
    }

    const auto* info = reinterpret_cast<const SystemHandleInformationEx*>(buffer.data());
    const SystemHandleEntryEx* typeEntry = FindHandleEntry(*info, typeProbe);
    if (!typeEntry) {
        if (target.handle != INVALID_HANDLE_VALUE) CloseHandle(target.handle);
        if (typeProbe != target.handle) CloseHandle(typeProbe);
        if (warning) *warning += L"系统句柄表中找不到文件类型探针。";
        return false;
    }

    target.objectTypeIndex = typeEntry->objectTypeIndex;
    if (target.handle != INVALID_HANDLE_VALUE) {
        const SystemHandleEntryEx* targetEntry = FindHandleEntry(*info, target.handle);
        if (targetEntry) {
            target.object = targetEntry->object;
            target.entryFound = true;
        }
    }

    const ULONG_PTR currentPid = GetCurrentProcessId();
    const ULONG_PTR targetProbeValue = reinterpret_cast<ULONG_PTR>(target.handle);
    const ULONG_PTR typeProbeValue = reinterpret_cast<ULONG_PTR>(typeProbe);
    ProcessHandles processHandles;

    for (ULONG_PTR i = 0; i < info->handleCount; ++i) {
        const auto& entry = info->handles[i];
        if (entry.objectTypeIndex != target.objectTypeIndex) continue;
        if (entry.processId == currentPid &&
            entry.handleValue == targetProbeValue) {
            continue;
        }
        if (entry.processId == currentPid &&
            entry.handleValue == typeProbeValue) {
            continue;
        }

        // Object pointers are sufficient on some Windows builds, but the
        // kernel may emit process-relative or obfuscated values on others.
        // Always verify non-matches by duplicating the handle and comparing
        // file identity/final path.
        bool matches = target.object != nullptr && entry.object == target.object;
        if (!matches) {
            HANDLE process = processHandles.Get(static_cast<DWORD>(entry.processId));
            if (!process) continue;
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(process,
                                 reinterpret_cast<HANDLE>(entry.handleValue),
                                 GetCurrentProcess(), &duplicate, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS) || !duplicate) {
                continue;
            }
            matches = DuplicatedHandleMatches(duplicate, target, path);
            CloseHandle(duplicate);
        }

        if (!matches) continue;
        const DWORD pid = static_cast<DWORD>(entry.processId);
        LockingProcess& process = EnsureProcess(byPid, pid);
        process.foundByHandleScan = true;
        const bool closable = pid != 0 && pid != 4 &&
                              (includeCurrentProcess || pid != currentPid);
        AddHandleIfMissing(&process, entry, closable);
    }

    if (target.handle != INVALID_HANDLE_VALUE) CloseHandle(target.handle);
    if (typeProbe != target.handle) CloseHandle(typeProbe);
    if (report) {
        report->handleScanCompleted = true;
        report->restrictedProcessCount =
            static_cast<unsigned>(processHandles.restricted.size());
    }
    return true;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> module(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    return length && length < module.size()
        ? std::wstring(module.data(), length)
        : std::wstring();
}

void ProbeDeleteAccess(const std::wstring& path, FileLockReport* report) {
    if (!report) return;
    report->deleteProbeAttempted = true;
    HANDLE file = CreateFileW(
        path.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        report->deleteOpenSucceeded = true;
        report->deleteProbeError = ERROR_SUCCESS;
        CloseHandle(file);
    } else {
        report->deleteOpenSucceeded = false;
        report->deleteProbeError = GetLastError();
    }
}

} // namespace

bool EnumerateFileLocks(const std::wstring& path,
                        std::vector<LockingProcess>* processes,
                        std::wstring* error,
                        FileLockReport* report,
                        bool includeCurrentProcess) {
    if (!processes) return false;
    processes->clear();
    if (error) error->clear();
    if (report) *report = FileLockReport{};

    std::unordered_map<DWORD, LockingProcess> byPid;
    std::wstring warning;
    const bool handleScanOk = ScanAllFileHandles(
        path, &byPid, report, &warning, includeCurrentProcess);
    MergeRestartManager(path, &byPid, report, &warning);
    // The current UI process can legitimately hold its own image or a preview
    // handle. It must never be offered as an unlock target.
    if (!includeCurrentProcess) byPid.erase(GetCurrentProcessId());
    if (!includeCurrentProcess) {
        const std::wstring currentExe = CurrentExecutablePath();
        if (!currentExe.empty()) {
            // Restart Manager can briefly report the UI or its --host sibling
            // while the dialog itself is refreshing resources. They are part
            // of this application, never useful unlock targets, and must not
            // make the result list flicker.
            for (auto it = byPid.begin(); it != byPid.end();) {
                if (!it->second.path.empty() &&
                    SamePath(it->second.path, currentExe)) {
                    it = byPid.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    ProbeDeleteAccess(path, report);
    if (report) report->warning = warning;

    processes->reserve(byPid.size());
    for (auto& item : byPid) processes->push_back(std::move(item.second));
    std::sort(processes->begin(), processes->end(),
              [](const LockingProcess& a, const LockingProcess& b) {
                  return a.pid < b.pid;
              });
    FinalizeHandles(processes);

    const bool rmOk = report && report->restartManagerCompleted;
    if (!handleScanOk && !rmOk) {
        if (error) *error = warning.empty() ? L"无法枚举文件占用" : warning;
        return false;
    }
    return true;
}

bool TerminateProcessById(DWORD pid, std::wstring* error) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        if (error) *error = L"拒绝结束受保护进程";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        if (error) *error = L"无法打开进程：" + FormatWin32Error(GetLastError());
        return false;
    }
    const BOOL ok = TerminateProcess(process, 0);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!ok && error) *error = L"结束进程失败：" + FormatWin32Error(code);
    return ok != FALSE;
}

bool CloseRemoteFileHandle(DWORD pid, ULONG_PTR handle, std::wstring* error) {
    if (pid == 0 || pid == 4 || handle == 0) {
        if (error) *error = L"无效的进程或句柄";
        return false;
    }
    EnableDebugPrivilege();
    HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) {
        if (error) *error = L"无法打开目标进程：" + FormatWin32Error(GetLastError());
        return false;
    }
    HANDLE duplicate = nullptr;
    const BOOL ok = DuplicateHandle(
        process, reinterpret_cast<HANDLE>(handle),
        GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    if (duplicate) CloseHandle(duplicate);
    CloseHandle(process);
    if (!ok && error) *error = L"关闭远程句柄失败：" + FormatWin32Error(code);
    return ok != FALSE;
}

} // namespace ze::platform
