#include "platform/FileLock.h"
#include "platform/PathUtil.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace ze::platform;

namespace {

constexpr DWORD kShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

struct Mode {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* behavior;
};

const Mode kModes[] = {
    { L"exclusive", L"独占读写",
      L"拒绝所有其他共享；Restart Manager 与句柄扫描都应能找到。" },
    { L"readonly-noshare", L"只读独占",
      L"只请求读取，但拒绝共享；删除会被共享冲突阻止。" },
    { L"readonly-no-delete", L"只读且不共享删除",
      L"允许其他读写，但未授予 FILE_SHARE_DELETE；只读句柄也会阻止删除。" },
    { L"writer-no-delete", L"读写且不共享删除",
      L"允许别人读取，但写者拒绝删除共享。" },
    { L"delete-friendly", L"允许删除的只读引用",
      L"句柄仍存在，但共享模式允许删除；用于验证“有句柄但可删”。" },
    { L"mapped", L"内存映射",
      L"关闭文件句柄后保留 Section + View；占用来自映射对象。" },
    { L"region-lock", L"字节区域锁",
      L"允许删除共享，同时用 LockFileEx 锁住整个文件区域。" },
    { L"child-process", L"跨进程子进程持有",
      L"由隐藏子进程持有只读且不共享删除的句柄。" },
};

std::wstring ModulePath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length ? std::wstring(path.data(), length) : std::wstring();
}

std::wstring QuoteArgument(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool WriteInitialText(const std::wstring& path, const Mode& mode) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, kShareAll, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::wstring modeId = mode.id;
    const std::string text =
        std::string("ZExplorer file-lock laboratory\r\nmode-utf8-bytes: ") +
        std::to_string(modeId.size()) +
        "\r\nClose the console or press Enter to release.\r\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                              &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

struct Holder {
    Mode mode{};
    std::wstring path;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    void* view = nullptr;
    HANDLE job = nullptr;
    DWORD childPid = 0;
    bool regionLocked = false;
    OVERLAPPED overlap{};

    ~Holder() {
        if (regionLocked && file != INVALID_HANDLE_VALUE) {
            UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &overlap);
            regionLocked = false;
        }
        if (job) {
            TerminateJobObject(job, 0);
            CloseHandle(job);
        }
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool LaunchChildHolder(Holder* holder) {
    std::wstring exe = ModulePath();
    if (exe.empty()) return false;

    holder->job = CreateJobObjectW(nullptr, nullptr);
    if (!holder->job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(holder->job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(holder->job);
        holder->job = nullptr;
        return false;
    }

    std::wstring command = QuoteArgument(exe) + L" --hold-child=" +
                           QuoteArgument(holder->path);
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
        &startup, &process);
    if (!created) {
        CloseHandle(holder->job);
        holder->job = nullptr;
        return false;
    }
    AssignProcessToJobObject(holder->job, process.hProcess);
    ResumeThread(process.hThread);
    holder->childPid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool HoldMode(const Mode& mode, const std::wstring& directory, Holder* holder) {
    holder->mode = mode;
    holder->path = JoinPath(directory,
        L"ZExplorer-lock-" + std::wstring(mode.id) + L"-" +
        std::to_wstring(GetCurrentProcessId()) + L".txt");
    if (!WriteInitialText(holder->path, mode)) return false;

    const std::wstring id = mode.id;
    if (id == L"exclusive") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (id == L"readonly-noshare") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (id == L"readonly-no-delete") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (id == L"writer-no-delete") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (id == L"delete-friendly") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ, kShareAll, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    } else if (id == L"mapped") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ, kShareAll, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (holder->file != INVALID_HANDLE_VALUE) {
            holder->mapping = CreateFileMappingW(
                holder->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (holder->mapping)
                holder->view = MapViewOfFile(
                    holder->mapping, FILE_MAP_READ, 0, 0, 0);
            if (holder->view) {
                CloseHandle(holder->file);
                holder->file = INVALID_HANDLE_VALUE;
            }
        }
    } else if (id == L"region-lock") {
        holder->file = CreateFileW(
            holder->path.c_str(), GENERIC_READ | GENERIC_WRITE, kShareAll,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (holder->file != INVALID_HANDLE_VALUE) {
            holder->regionLocked =
                LockFileEx(holder->file, LOCKFILE_EXCLUSIVE_LOCK, 0,
                           MAXDWORD, MAXDWORD, &holder->overlap) != FALSE;
        }
    } else if (id == L"child-process") {
        return LaunchChildHolder(holder);
    }
    return holder->file != INVALID_HANDLE_VALUE ||
           holder->mapping != nullptr || holder->childPid != 0;
}

void PrintReport(const Holder& holder) {
    std::vector<LockingProcess> processes;
    std::wstring error;
    FileLockReport report;
    const bool ok = EnumerateFileLocks(
        holder.path, &processes, &error, &report, /*includeCurrentProcess*/ true);

    std::wcout << L"  文件: " << holder.path << L"\n";
    std::wcout << L"  行为: " << holder.mode.behavior << L"\n";
    if (holder.childPid) std::wcout << L"  持有子进程 PID: " << holder.childPid << L"\n";
    if (!ok) {
        std::wcout << L"  检测失败: " << error << L"\n\n";
        return;
    }

    size_t handleCount = 0;
    for (const auto& process : processes) handleCount += process.handles.size();
    std::wcout << L"  检测结果: " << processes.size() << L" 个进程 / "
               << handleCount << L" 个具体句柄\n";
    for (const auto& process : processes) {
        std::wcout << L"    PID " << process.pid << L"  " << process.name
                   << L"  [" << process.applicationType << L"]  "
                   << process.handles.size() << L" 个句柄  "
                   << process.accessSummary << L"\n";
        for (const auto& handle : process.handles) {
            wchar_t value[32]{};
            swprintf_s(value, L"0x%llX",
                       static_cast<unsigned long long>(handle.value));
            std::wcout << L"      句柄 " << value
                       << (handle.closable ? L"（可关闭）" : L"（只显示）") << L"\n";
        }
    }
    if (report.deleteProbeAttempted) {
        if (report.deleteOpenSucceeded) {
            std::wcout << L"  删除探测: 可打开 DELETE 句柄，共享不阻止删除\n";
        } else if (report.deleteProbeError == ERROR_SHARING_VIOLATION) {
            std::wcout << L"  删除探测: 共享冲突，当前有句柄阻止删除\n";
        } else {
            std::wcout << L"  删除探测: " << FormatWin32Error(report.deleteProbeError)
                       << L"\n";
        }
    }
    if (!report.warning.empty())
        std::wcout << L"  提示: " << report.warning << L"\n";
    std::wcout << L"\n";
}

void PrintModes() {
    std::wcout << L"ZExplorer 文件占用实验室\n\n";
    for (size_t i = 0; i < std::size(kModes); ++i) {
        std::wcout << L"  " << (i + 1) << L". " << kModes[i].title
                   << L" (" << kModes[i].id << L")\n";
    }
    std::wcout << L"  0. 同时运行全部模式\n"
               << L"  q. 退出\n\n";
}

int HoldChild(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 2;
    WaitForSingleObject(GetCurrentProcess(), INFINITE);
    CloseHandle(file);
    return 0;
}

bool Parse(const std::wstring& argument, const wchar_t* prefix,
           std::wstring* value) {
    const size_t length = wcslen(prefix);
    if (argument.rfind(prefix, 0) != 0) return false;
    *value = argument.substr(length);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stdin), _O_U8TEXT);
    SetConsoleTitleW(L"ZExplorer 文件占用实验室");

    std::wstring directory = ParentPath(ModulePath());
    std::wstring requestedMode;
    bool all = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring value;
        const std::wstring arg = argv[i];
        if (Parse(arg, L"--hold-child=", &value)) return HoldChild(value);
        if (Parse(arg, L"--path=", &value)) directory = value;
        else if (Parse(arg, L"--mode=", &value)) requestedMode = value;
        else if (arg == L"--all") all = true;
        else if (arg == L"--help" || arg == L"-h") {
            PrintModes();
            return 0;
        }
    }

    if (!directory.empty()) CreateDirectoryW(directory.c_str(), nullptr);
    std::wcout << L"测试文件目录: " << directory << L"\n"
               << L"保持此控制台打开；关闭控制台会由系统释放所有句柄。\n\n";

    std::vector<const Mode*> selected;
    if (all) {
        for (const auto& mode : kModes) selected.push_back(&mode);
    } else if (!requestedMode.empty()) {
        auto it = std::find_if(std::begin(kModes), std::end(kModes),
                              [&](const Mode& mode) {
                                  return requestedMode == mode.id;
                              });
        if (it == std::end(kModes)) {
            std::wcerr << L"未知模式: " << requestedMode << L"\n";
            PrintModes();
            return 2;
        }
        selected.push_back(&*it);
    } else {
        PrintModes();
        std::wcout << L"选择模式: ";
        std::wstring input;
        std::getline(std::wcin, input);
        if (input == L"q" || input == L"Q") return 0;
        if (input == L"0") {
            for (const auto& mode : kModes) selected.push_back(&mode);
        } else {
            const int index = _wtoi(input.c_str());
            if (index < 1 || index > static_cast<int>(std::size(kModes))) {
                std::wcerr << L"无效选择。\n";
                return 2;
            }
            selected.push_back(&kModes[index - 1]);
        }
    }

    std::vector<std::unique_ptr<Holder>> holders;
    for (const Mode* mode : selected) {
        auto holder = std::make_unique<Holder>();
        if (!HoldMode(*mode, directory, holder.get())) {
            std::wcerr << L"创建占用失败: " << mode->title
                       << L"，错误: " << FormatWin32Error(GetLastError()) << L"\n";
            return 3;
        }
        holders.push_back(std::move(holder));
    }

    std::wcout << L"\n当前测试程序 PID: " << GetCurrentProcessId() << L"\n\n";
    for (const auto& holder : holders) PrintReport(*holder);

    std::wcout << L"按 Enter 释放并退出；也可以直接关闭此控制台。";
    std::wstring line;
    std::getline(std::wcin, line);
    return 0;
}
