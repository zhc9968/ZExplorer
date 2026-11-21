#include "platform/DirectoryWatcher.h"

#include "platform/PathUtil.h"

#include <array>

namespace ze::platform {

DirectoryWatcher::~DirectoryWatcher() {
    Stop();
}

bool DirectoryWatcher::Start(const std::wstring& directory) {
    Stop();
    if (directory.empty() || !PathExists(directory)) return false;

    directory_ = NormalizePath(directory);
    directoryHandle_ = CreateFileW(
        ToExtendedPath(directory_).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (directoryHandle_ == INVALID_HANDLE_VALUE) return false;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !readyEvent_) {
        if (stopEvent_) CloseHandle(stopEvent_);
        if (readyEvent_) CloseHandle(readyEvent_);
        stopEvent_ = nullptr;
        readyEvent_ = nullptr;
        CloseHandle(directoryHandle_);
        directoryHandle_ = INVALID_HANDLE_VALUE;
        return false;
    }
    overlapped_ = {};
    overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped_.hEvent) {
        CloseHandle(stopEvent_);
        CloseHandle(readyEvent_);
        stopEvent_ = nullptr;
        readyEvent_ = nullptr;
        CloseHandle(directoryHandle_);
        directoryHandle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    buffer_.resize(64 * 1024);
    dirty_ = false;
    startupOk_ = false;
    running_ = true;
    worker_ = std::thread([this]() { WorkerLoop(); });

    const DWORD ready = WaitForSingleObject(readyEvent_, 1500);
    if (ready != WAIT_OBJECT_0 || !startupOk_.load()) {
        Stop();
        return false;
    }
    return true;
}

void DirectoryWatcher::Stop() {
    if (running_.exchange(false)) {
        if (stopEvent_) SetEvent(stopEvent_);
        if (directoryHandle_ != INVALID_HANDLE_VALUE)
            CancelIoEx(directoryHandle_, &overlapped_);
        if (worker_.joinable()) worker_.join();
    }

    if (overlapped_.hEvent) {
        CloseHandle(overlapped_.hEvent);
        overlapped_.hEvent = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (readyEvent_) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }
    if (directoryHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(directoryHandle_);
        directoryHandle_ = INVALID_HANDLE_VALUE;
    }
    buffer_.clear();
    dirty_ = false;
}

bool DirectoryWatcher::Poll() {
    if (!dirty_.load()) return false;
    const ULONGLONG changed = changedAt_.load();
    if (changed != 0 && GetTickCount64() - changed < 180) return false;
    dirty_ = false;
    return true;
}

void DirectoryWatcher::WorkerLoop() {
    constexpr DWORD kFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE;
    std::array<HANDLE, 2> waits{ overlapped_.hEvent, stopEvent_ };
    bool firstWatch = true;

    while (running_.load()) {
        ResetEvent(overlapped_.hEvent);
        const BOOL issued = ReadDirectoryChangesW(
            directoryHandle_, buffer_.data(), static_cast<DWORD>(buffer_.size()),
            TRUE, kFilter, nullptr, &overlapped_, nullptr);
        const DWORD issueError = issued ? ERROR_SUCCESS : GetLastError();
        const bool armed = issued || issueError == ERROR_IO_PENDING;
        if (!armed) {
            if (firstWatch) {
                startupOk_ = false;
                SetEvent(readyEvent_);
                firstWatch = false;
            }
            if (!running_.load()) break;
            Sleep(80);
            continue;
        }
        if (firstWatch) {
            startupOk_ = true;
            SetEvent(readyEvent_);
            firstWatch = false;
        }

        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()), waits.data(), FALSE, 1000);
        if (wait == WAIT_OBJECT_0 + 1 || !running_.load()) break;
        if (wait != WAIT_OBJECT_0) continue;

        DWORD transferred = 0;
        if (GetOverlappedResult(directoryHandle_, &overlapped_, &transferred, FALSE)) {
            if (transferred > 0) {
                dirty_ = true;
                changedAt_ = GetTickCount64();
            }
        } else if (GetLastError() != ERROR_OPERATION_ABORTED && running_.load()) {
            Sleep(80);
        }
    }
}

} // namespace ze::platform
