#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace ze::platform {

// Watches one directory with ReadDirectoryChangesW. The worker only records a
// dirty/debounce timestamp; UI code calls Poll() on its own timer thread.
class DirectoryWatcher {
public:
    DirectoryWatcher() = default;
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    bool Start(const std::wstring& directory);
    void Stop();

    // Returns true once after the debounce interval has elapsed.
    bool Poll();
    bool IsWatching() const { return running_.load(); }

private:
    void WorkerLoop();

    std::wstring directory_;
    HANDLE directoryHandle_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    OVERLAPPED overlapped_{};
    std::vector<BYTE> buffer_;
    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> startupOk_{ false };
    std::atomic<bool> dirty_{ false };
    std::atomic<ULONGLONG> changedAt_{ 0 };
};

} // namespace ze::platform
