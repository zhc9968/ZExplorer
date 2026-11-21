#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ze::platform {

struct FileHandleReference {
    ULONG_PTR value = 0;
    DWORD grantedAccess = 0;
    bool closable = false;
};

struct LockingProcess {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
    std::wstring applicationType;
    std::wstring accessSummary;
    std::vector<FileHandleReference> handles;
    bool foundByHandleScan = false;
    bool foundByRestartManager = false;

    size_t ClosableHandleCount() const {
        size_t count = 0;
        for (const auto& handle : handles) {
            if (handle.closable) ++count;
        }
        return count;
    }
};

struct FileLockReport {
    bool handleScanCompleted = false;
    bool restartManagerCompleted = false;
    bool deleteProbeAttempted = false;
    bool deleteOpenSucceeded = false;
    DWORD deleteProbeError = ERROR_SUCCESS;
    unsigned restrictedProcessCount = 0;
    std::wstring warning;
};

bool EnumerateFileLocks(const std::wstring& path,
                        std::vector<LockingProcess>* processes,
                        std::wstring* error = nullptr,
                        FileLockReport* report = nullptr,
                        bool includeCurrentProcess = false);

bool TerminateProcessById(DWORD pid, std::wstring* error = nullptr);

bool CloseRemoteFileHandle(DWORD pid, ULONG_PTR handle,
                           std::wstring* error = nullptr);

} // namespace ze::platform
