#include "pch.h"
#include "host/HostApp.h"

#include "ipc/Pipe.h"
#include "host/TaskWindow.h"
#include "platform/FileOps.h"
#include "platform/FileLock.h"
#include "platform/NtfsStreams.h"
#include "platform/Settings.h"
#include "platform/PathUtil.h"
#include "platform/ShellActions.h"
#include "platform/WindowSecurity.h"
#include "platform/WinEKeyState.h"
#include "ui/NotificationDrawer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

using namespace ZufyUI;
using namespace ze::platform;

namespace ze::host {

namespace {

enum class TaskState : int { Queued = 0, Running = 1, Done = 2, Failed = 3, Canceled = 4,
                             Awaiting = 5 };   // 等待用户对冲突做决定

struct Task {
    int id = 0;
    DWORD originPid = 0;
    std::wstring originPath;
    std::wstring op;
    std::wstring title;
    std::wstring targetDir;
    std::vector<std::wstring> sources;

    std::atomic<int> state{ static_cast<int>(TaskState::Queued) };
    std::atomic<std::uint64_t> doneBytes{ 0 }, totalBytes{ 0 };
    std::atomic<std::uint32_t> doneFiles{ 0 }, totalFiles{ 0 };
    std::atomic<bool> cancel{ false };
    std::atomic<bool> dismissed{ false };   // 用户关掉了页签 = 放弃这个任务
    std::atomic<ULONGLONG> startedAt{ 0 };
    std::atomic<ULONGLONG> finishedAt{ 0 };
    std::atomic<double> speed{ 0.0 };

    mutable std::mutex textMutex;
    std::wstring current;
    std::vector<OpIssue> issues;


    void SetCurrent(const std::wstring& s) {
        std::lock_guard<std::mutex> lock(textMutex);
        current = s;
    }
    std::wstring GetCurrent() const {
        std::lock_guard<std::mutex> lock(textMutex);
        return current;
    }
    void AddIssues(const std::vector<OpIssue>& in) {
        if (in.empty()) return;
        std::lock_guard<std::mutex> lock(textMutex);
        for (const auto& i : in) issues.push_back(i);
    }
    // 冲突策略：0 = 还没决定（逐个记卡片）；1 = 全部覆盖；2 = 全部跳过
    std::atomic<int> conflictPolicy{ 0 };

    // 等待决策的冲突项。**属于同一个任务**，点覆盖/跳过不会产生新任务。
    struct PendingItem {
        std::wstring src;
        std::wstring dst;
        int decision = 0;   // 0 未定 1 覆盖 2 跳过
    };
    std::vector<PendingItem> pending;

    void AddPending(const std::wstring& src, const std::wstring& dst) {
        std::lock_guard<std::mutex> lock(textMutex);
        for (auto& p : pending) {
            if (p.src == src && p.dst == dst) return;
        }
        pending.push_back({ src, dst, 0 });
    }
    void SetDecision(const std::wstring& src, int decision) {
        std::lock_guard<std::mutex> lock(textMutex);
        for (auto& p : pending) {
            if (p.src == src) p.decision = decision;
        }
    }
    void SetAllDecisions(int decision) {
        std::lock_guard<std::mutex> lock(textMutex);
        for (auto& p : pending) {
            if (p.decision == 0) p.decision = decision;
        }
        conflictPolicy = (decision == 1) ? 1 : 2;
    }
    size_t PendingCount() const {
        std::lock_guard<std::mutex> lock(textMutex);
        return pending.size();
    }
    std::vector<PendingItem> TakeDecided() {
        std::lock_guard<std::mutex> lock(textMutex);
        std::vector<PendingItem> out;
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->decision != 0) {
                out.push_back(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    // 错误清单被清空后，任务状态要跟着回到"完成"（否则会一直显示"有 0 项失败"）
    void RecomputeState() {
        const int st = state.load();
        if (st == static_cast<int>(TaskState::Running) || st == static_cast<int>(TaskState::Queued))
            return;
        if (cancel.load()) { state = static_cast<int>(TaskState::Canceled); return; }
        state = GetIssues().empty() ? static_cast<int>(TaskState::Done)
                                    : static_cast<int>(TaskState::Failed);
        finishedAt = GetTickCount64();
    }

    void RemoveIssuesByKind(int kind) {
        std::lock_guard<std::mutex> lock(textMutex);
        issues.erase(std::remove_if(issues.begin(), issues.end(),
                         [kind](const OpIssue& i) { return i.kind == kind; }),
                     issues.end());
    }

    void RemoveIssueByPath(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(textMutex);
        issues.erase(std::remove_if(issues.begin(), issues.end(),
                         [&](const OpIssue& i) { return i.path == path; }),
                     issues.end());
    }
    void RemoveIssue(size_t index) {
        std::lock_guard<std::mutex> lock(textMutex);
        if (index < issues.size()) issues.erase(issues.begin() + index);
    }
    size_t IssueCount() const {
        std::lock_guard<std::mutex> lock(textMutex);
        return issues.size();
    }
    std::vector<OpIssue> GetIssues() const {
        std::lock_guard<std::mutex> lock(textMutex);
        return issues;
    }
    float Ratio() const {
        const auto total = totalBytes.load();
        if (total > 0) return static_cast<float>(static_cast<double>(doneBytes.load()) / total);
        const auto tf = totalFiles.load();
        if (tf > 0) return static_cast<float>(static_cast<double>(doneFiles.load()) / tf);
        return 0.0f;
    }
};


std::wstring OpTitle(const std::wstring& op) {
    if (op == L"copy") return L"复制";
    if (op == L"cut") return L"剪切";
    if (op == L"paste") return L"粘贴";
    if (op == L"delete") return L"删除";
    if (op == L"deleteperm") return L"永久删除";
    if (op == L"rename") return L"重命名";
    if (op == L"newfolder") return L"新建文件夹";
    if (op == L"newfile") return L"新建文件";
    if (op == L"streamimport") return L"导入数据流";
    if (op == L"streamexport") return L"导出数据流";
    if (op == L"streamdelete") return L"删除数据流";
    if (op == L"streamhide") return L"隐藏到数据流";
    if (op == L"streamwrite") return L"编辑数据流";
    if (op == L"streamrename") return L"重命名数据流";
    if (op == L"streamcreate") return L"创建数据流";
    if (op == L"killprocess") return L"结束占用进程";
    if (op == L"closehandle") return L"关闭文件句柄";
    if (op == L"copyover") return L"覆盖复制";
    if (op == L"launch") return L"启动";
    return op;
}

bool CanRetryIssue(const std::wstring& op) {
    return op == L"copy" || op == L"cut" || op == L"paste" ||
           op == L"copyover" || op == L"delete" || op == L"deleteperm" ||
           op == L"rename" || op == L"newfolder" || op == L"newfile";
}

std::wstring TaskTitle(const std::wstring& op,
                       const std::wstring& targetDir,
                       const std::vector<std::wstring>& sources) {
    if (op == L"closehandle" && !sources.empty()) {
        const DWORD pid = static_cast<DWORD>(_wtoi(sources.front().c_str()));
        return L"关闭文件句柄：PID " + std::to_wstring(pid) + L" · " +
               std::to_wstring(sources.size() - 1) + L" 个句柄";
    }
    if (op == L"killprocess" && !sources.empty()) {
        const DWORD pid = static_cast<DWORD>(_wtoi(sources.front().c_str()));
        return L"结束占用进程：PID " + std::to_wstring(pid);
    }
    std::wstring title = OpTitle(op) + L"：" +
                         (sources.empty() ? FileNameOf(targetDir)
                                          : FileNameOf(sources.front()));
    if (sources.size() > 1)
        title += L" 等 " + std::to_wstring(sources.size()) + L" 项";
    return title;
}

HICON IconForPath(const std::wstring& path) {
    SHFILEINFOW sfi{};
    if (!path.empty() && SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                                        SHGFI_ICON | SHGFI_SMALLICON) != 0) {
        return sfi.hIcon;
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

std::wstring FormatSpeed(double bytesPerSec) {
    if (bytesPerSec <= 0.0) return L"";
    wchar_t buf[64];
    if (bytesPerSec > 1024.0 * 1024.0)
        swprintf_s(buf, L"%.1f MB/s", bytesPerSec / (1024.0 * 1024.0));
    else
        swprintf_s(buf, L"%.0f KB/s", bytesPerSec / 1024.0);
    return buf;
}

bool LaunchNewUiProcess() {
    wchar_t self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
    std::wstring command =
        L"\"" + std::wstring(self) + L"\" --force-current-process";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::atomic<bool> g_winEHookEnabled{ false };
ze::platform::WinEKeyState g_winEKeyState;
HHOOK g_winEHook = nullptr;

void SendWinMenuMask() {
    INPUT events[2]{};
    events[0].type = INPUT_KEYBOARD;
    events[0].ki.wVk = VK_LCONTROL;
    events[0].ki.dwExtraInfo = 0x5A455049;
    events[1] = events[0];
    events[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, events, sizeof(INPUT));
}

LRESULT CALLBACK WinEKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION || !g_winEHookEnabled.load())
        return CallNextHookEx(g_winEHook, code, wParam, lParam);

    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (!info)
        return CallNextHookEx(g_winEHook, code, wParam, lParam);
    if ((info->flags & LLKHF_INJECTED) != 0)
        return CallNextHookEx(g_winEHook, code, wParam, lParam);

    const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if (!keyDown && !keyUp)
        return CallNextHookEx(g_winEHook, code, wParam, lParam);

    const auto action = g_winEKeyState.Handle(info->vkCode, keyDown, keyUp);
    if (action == ze::platform::WinEKeyAction::Launch) {
        LaunchNewUiProcess();
    }
    if (action == ze::platform::WinEKeyAction::MaskAndPass) {
        // The current Win-up has not been forwarded yet, so the OS still sees
        // Win down while this Ctrl tap is injected. That masks Start menu
        // activation; the real Win-up is then passed through, keeping OS key
        // state consistent for later Win+D/Win+L/Win+other shortcuts.
        SendWinMenuMask();
        return CallNextHookEx(g_winEHook, code, wParam, lParam);
    }
    if (action != ze::platform::WinEKeyAction::Pass) {
        return 1;
    }

    return CallNextHookEx(g_winEHook, code, wParam, lParam);
}

bool SetWinEHookEnabled(bool enabled, std::wstring* error) {
    g_winEHookEnabled = enabled;
    if (!enabled) {
        g_winEKeyState.Reset();
        if (g_winEHook) {
            UnhookWindowsHookEx(g_winEHook);
            g_winEHook = nullptr;
        }
        return true;
    }
    if (g_winEHook) return true;
    g_winEHook = SetWindowsHookExW(WH_KEYBOARD_LL, WinEKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
    if (!g_winEHook) {
        if (error) *error = L"无法安装 Win+E 键盘钩子：" +
                            FormatWin32Error(GetLastError());
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
struct HostApp::Impl {
    std::shared_ptr<Window> mainWin;
    TrayIcon tray;
    std::shared_ptr<Menu> trayMenu;
    std::shared_ptr<Window> settingsWin;
    std::shared_ptr<ui::NotificationDrawer> settingsDrawer;
    std::shared_ptr<Window> aboutWin;
    std::atomic<bool> pauseOnError{ false };    // 发生错误时暂停等待
    std::atomic<bool> focusOnError{ true };     // 发生错误时切到该页签并前置
    std::atomic<bool> autoElevate{ false };     // 启动时自动请求管理员权限
    std::atomic<bool> executableViaShell{ true }; // UI 可直接 ShellExecute 可执行文件
    std::atomic<bool> showHiddenFiles{ false };
    std::atomic<bool> hideKnownExtensions{ false };
    std::atomic<int> maxConcurrentTasks{ 4 };
    std::atomic<int> taskShowDelayMs{ 350 };
    std::atomic<int> taskDoneLingerMs{ 1200 };
    std::atomic<int> taskRetentionMinutes{ 15 };
    std::atomic<bool> beepOnError{ false };
    std::atomic<bool> confirmRecycleDelete{ true };
    std::atomic<bool> autoStartHost{ false };
    std::atomic<bool> shellContextMenu{ false };
    std::atomic<bool> defaultFileManager{ false };
    std::atomic<bool> winEShortcut{ false };



    std::vector<std::shared_ptr<Task>> tasks;
    TaskWindow taskUI;                            // 所有任务窗 UI 都在这个模块里
    std::mutex taskMutex;
    std::vector<std::thread> runners;
    std::mutex runnerMutex;
    std::mutex slotMutex;
    std::condition_variable slotCv;
    int activeSlots = 0;
    std::atomic<bool> quit{ false };
    std::atomic<bool> running{ true };
    std::atomic<int> nextId{ 1 };
    ipc::PipeServer server;
    std::shared_ptr<Timer> pump;

    // 管道线程绝不直接碰 UI 对象：请求先入队，由 UI 线程的泵执行
    std::mutex uiMutex;
    std::vector<std::function<void()>> uiTasks;

    // QueryTasks 走这份缓存：管道线程只做一次 vector 拷贝
    std::mutex snapMutex;
    std::vector<std::wstring> snapCache;
    void PostUi(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(uiMutex);
            uiTasks.push_back(std::move(fn));
        }
    }

    void DrainUi() {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(uiMutex);
            pending.swap(uiTasks);
        }
        for (auto& fn : pending) fn();
    }

    void RebuildSnapshot() {
        std::vector<std::wstring> out;
        for (auto& t : Snapshot()) {
            const auto issues = t->GetIssues();
            wchar_t head[96]{};
            swprintf_s(head, L"%d\t%d\t%.3f", t->id, t->state.load(), t->Ratio());
            std::wstring line = head;
            line += L"\t" + t->title;
            line += L"\t" + t->GetCurrent();
            line += L"\t" + std::to_wstring(issues.size());
            line += L"\t" + (issues.empty() ? std::wstring()
                                            : issues.front().message);
            line += L"\t" + std::to_wstring(t->doneFiles.load());
            line += L"\t" + std::to_wstring(t->totalFiles.load());
            line += L"\t" + std::to_wstring(t->originPid);
            line += L"\t" + t->originPath;
            line += L"\t" + t->op;
            line += L"\t" + t->targetDir;
            std::wstring details;
            for (const auto& issue : issues) {
                std::wstring one;
                if (!issue.path.empty()) one += issue.path;
                if (!issue.target.empty()) one += L"  →  " + issue.target;
                if (!issue.message.empty()) {
                    if (!one.empty()) one += L"\n";
                    one += L"错误：" + issue.message;
                }
                for (wchar_t& ch : one) {
                    if (ch == L'\t') ch = L' ';
                }
                if (!details.empty()) details += L"\n\n";
                details += one;
            }
            line += L"\t" + details;
            out.emplace_back(std::move(line));
        }
        std::lock_guard<std::mutex> lock(snapMutex);
        snapCache.swap(out);
    }

    std::shared_ptr<Task> FindTask(int id) {
        std::lock_guard<std::mutex> lock(taskMutex);
        for (auto& t : tasks) if (t->id == id) return t;
        return nullptr;
    }
    std::vector<std::shared_ptr<Task>> Snapshot() {
        std::lock_guard<std::mutex> lock(taskMutex);
        return tasks;
    }

    void PruneHistory() {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(taskMutex);
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
            [this, now](const std::shared_ptr<Task>& task) {
                if (!task) return true;
                const int state = task->state.load();
                const bool terminal = state == static_cast<int>(TaskState::Done) ||
                                      state == static_cast<int>(TaskState::Failed) ||
                                      state == static_cast<int>(TaskState::Canceled);
                const ULONGLONG finished = task->finishedAt.load();
                return terminal && finished != 0 &&
                       now - finished >=
                           static_cast<ULONGLONG>(taskRetentionMinutes.load()) *
                               60ull * 1000ull;
            }), tasks.end());
    }

    void Enqueue(const std::wstring& op, const std::wstring& targetDir,
                 const std::vector<std::wstring>& sources, int* outId,
                 DWORD originPid = 0,
                 const std::wstring& originPath = std::wstring()) {
        auto task = std::make_shared<Task>();
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            task->id = nextId.fetch_add(1);
            task->originPid = originPid;
            task->originPath = originPath;
            task->op = op;
            task->targetDir = targetDir;
            task->sources = sources;
            task->title = TaskTitle(op, targetDir, sources);
            if (outId) *outId = task->id;
            tasks.push_back(task);
        }

        // 并发上限：其余等待空位
        std::lock_guard<std::mutex> lock(runnerMutex);
        runners.emplace_back([this, task]() {
            {
                std::unique_lock<std::mutex> slot(slotMutex);
                slotCv.wait(slot, [this]() {
                    return activeSlots < maxConcurrentTasks.load() || quit.load();
                });
                if (quit.load()) {
                    task->state = static_cast<int>(TaskState::Canceled);
                    task->finishedAt = GetTickCount64();
                    return;
                }
                ++activeSlots;
            }

            RunTask(task);

            {
                std::lock_guard<std::mutex> slot(slotMutex);
                --activeSlots;
            }
            slotCv.notify_one();
        });
    }
    void RunTask(const std::shared_ptr<Task>& t) {
        t->state = static_cast<int>(TaskState::Running);
        t->startedAt = GetTickCount64();

        // 聚合进度：每个源单独跑 CopyTree 会把计数器重置成"这一个文件"，
        // 所以这里自己累计。total 先算好；done 用"已完成部分的基数 + 本项实际完成量"。
        // 注意：只累加**真正成功**的字节/文件数，失败项不算完成。
        std::uint64_t aggBytes = 0, aggTotalBytes = 0;
        std::uint32_t aggFiles = 0, aggTotalFiles = 0;
        std::uint64_t lastDoneBytes = 0;
        std::uint32_t lastDoneFiles = 0;

        auto addSourceToTotal = [&](const std::wstring& path) {
            uint64_t b = 0;
            uint32_t f = 0;
            MeasureTreeStats(path, &b, &f);
            aggTotalBytes += b;
            aggTotalFiles += f;
            t->totalBytes = aggTotalBytes;
            t->totalFiles = aggTotalFiles;
        };
        auto commitSource = [&]() {
            aggBytes += lastDoneBytes;
            aggFiles += lastDoneFiles;
            lastDoneBytes = 0;
            lastDoneFiles = 0;
            t->doneBytes = aggBytes;
            t->doneFiles = aggFiles;
        };

        auto progress = [&t, &aggBytes, &aggFiles, &lastDoneBytes, &lastDoneFiles](
                            const OpProgress& p) {
            lastDoneBytes = p.doneBytes;
            lastDoneFiles = p.doneFiles;
            t->doneBytes = aggBytes + p.doneBytes;
            t->doneFiles = aggFiles + p.doneFiles;
            t->SetCurrent(p.current);

            const ULONGLONG ms = GetTickCount64() - t->startedAt.load();
            if (ms > 200) t->speed = static_cast<double>(t->doneBytes.load()) * 1000.0 / static_cast<double>(ms);
            return !t->cancel.load();
        };

        CopyOptions opts;
        opts.onFile = [&t, this](FileEventKind kind, const std::wstring& path, std::uint64_t size,
                                 std::uint64_t processed) {


            // 设置项：发生错误时暂停等待处理。只有开关打开时才停，
            // 默认是"静默继续"，把错误留给卡片。
            if (kind == FileEventKind::Failed && pauseOnError.load()) {
                t->SetCurrent(L"已暂停：等待处理错误（可在页签里重试或忽略）");
                const size_t count = t->IssueCount();
                while (!t->cancel.load() && !quit.load() && t->IssueCount() == count) Sleep(50);
            }
        };
        // 冲突：按任务级策略处理。
        //   已选"全部覆盖/全部跳过" -> 后续冲突不再算错误，直接照办；
        //   还没选 -> 记一张卡片（带源和目标的完整路径），本次先跳过。
        opts.onConflict = [this, &t](const std::wstring& src, const std::wstring& dst) {
            const int policy = t->conflictPolicy.load();
            if (policy == 1) return ConflictAction::Overwrite;
            if (policy == 2) return ConflictAction::Skip;

            t->AddIssues({ OpIssue{ src, L"目标已存在同名文件", dst, 1 } });
            t->AddPending(src, dst);          // 决策在本任务内完成，不另起任务
            return ConflictAction::Skip;
        };

        const std::wstring op = t->op;
        OpResult result;

        if (op == L"copy" || op == L"cut" || op == L"copyover") {
            if (t->targetDir.empty()) {
                std::wstring err;
                if (!SetClipboardFiles(t->sources, op == L"cut", &err))
                    result.issues.push_back({ t->sources.empty() ? L"" : t->sources.front(), err });
                else
                    result.okCount = static_cast<std::uint32_t>(t->sources.size());
            } else {
                for (const auto& src : t->sources) {
                    if (t->cancel.load()) { result.canceled = true; break; }
                    addSourceToTotal(src);
                    // copyover 的 targetDir 是**完整目标文件路径**（冲突卡片里记下的那个），
                    // 不能再用 targetDir + 文件名拼接，否则子目录里的冲突会被覆盖到上一层。
                    const std::wstring dst = (op == L"copyover")
                                                 ? t->targetDir
                                                 : JoinPath(t->targetDir, FileNameOf(src));   // 不自动改名：交给冲突策略
                    if (op == L"cut") MoveTree(src, dst, progress, &result, opts);
                    else {
                        CopyOptions use = opts;
                        if (op == L"copyover") {
                            use.onConflict = [](const std::wstring&, const std::wstring&) {
                                return ConflictAction::Overwrite;
                            };
                        }
                        CopyTree(src, dst, progress, &result, use);
                    }
                    commitSource();
                }
            }
        } else if (op == L"paste") {
            std::vector<std::wstring> srcs;
            bool cut = false;
            if (!GetClipboardFiles(&srcs, &cut)) {
                result.issues.push_back({ t->targetDir, L"剪贴板里没有文件" });
            } else {
                for (const auto& src : srcs) {
                    if (t->cancel.load()) { result.canceled = true; break; }
                    addSourceToTotal(src);
                    const std::wstring dst = JoinPath(t->targetDir, FileNameOf(src));   // 不自动改名：交给冲突策略
                    if (cut) MoveTree(src, dst, progress, &result, opts);
                    else CopyTree(src, dst, progress, &result, opts);
                    commitSource();
                }
            }
        } else if (op == L"delete" || op == L"deleteperm") {
            t->SetCurrent(L"正在删除…");
            if (op == L"delete") {
                DeleteToRecycleBin(t->sources, &result);
            } else {
                for (const auto& s : t->sources) {
                    if (t->cancel.load()) { result.canceled = true; break; }
                    DeletePermanent(s, &result);
                }
            }
        } else if (op == L"streamimport" || op == L"streamexport" ||
                   op == L"streamdelete" || op == L"streamhide" ||
                   op == L"streamwrite" || op == L"streamrename" ||
                   op == L"streamcreate") {
            if (t->sources.empty()) {
                result.issues.push_back({ t->targetDir, L"缺少数据流参数" });
            } else {
                const std::wstring& streamName = t->sources[0];
                if (op == L"streamcreate") {
                    t->SetCurrent(L"正在创建数据流…");
                    std::wstring error;
                    if (WriteAlternateStreamText(t->targetDir, streamName,
                                                 L"", false, &error)) {
                        ++result.okCount;
                    } else {
                        result.issues.push_back({ t->targetDir + L":" + streamName, error });
                    }
                } else if (op == L"streamwrite") {
                    if (t->sources.size() < 2) {
                        result.issues.push_back({ t->targetDir, L"缺少数据流内容" });
                    } else {
                        t->SetCurrent(L"正在写入数据流…");
                        std::wstring error;
                        if (WriteAlternateStreamText(t->targetDir, streamName,
                                                     t->sources[1], true, &error)) {
                            ++result.okCount;
                        } else {
                            result.issues.push_back({ t->targetDir + L":" + streamName, error });
                        }
                    }
                } else if (op == L"streamrename") {
                    if (t->sources.size() < 2) {
                        result.issues.push_back({ t->targetDir, L"缺少新数据流名称" });
                    } else {
                        t->SetCurrent(L"正在重命名数据流…");
                        RenameAlternateStream(t->targetDir, streamName, t->sources[1],
                                              progress, &result);
                    }
                } else if (op == L"streamimport" || op == L"streamhide") {
                    if (t->sources.size() < 2) {
                        result.issues.push_back({ t->targetDir, L"缺少导入源文件" });
                    } else {
                        const std::wstring& source = t->sources[1];
                        t->SetCurrent(L"正在导入数据流…");
                        const bool overwrite = t->sources.size() >= 3 &&
                                               t->sources[2] == L"1";
                        CopyFileToAlternateStream(t->targetDir, streamName, source,
                                                  overwrite, progress, &result);
                        if (result.Ok() && op == L"streamhide") {
                            t->SetCurrent(L"正在把源文件移入回收站…");
                            DeleteToRecycleBin({ source }, &result);
                        }
                    }
                } else if (op == L"streamexport") {
                    if (t->sources.size() < 2) {
                        result.issues.push_back({ t->targetDir, L"缺少导出目标文件" });
                    } else {
                        t->SetCurrent(L"正在导出数据流…");
                        CopyAlternateStreamToFile(t->targetDir, streamName, t->sources[1],
                                                  true, progress, &result);
                    }
                } else {
                    t->SetCurrent(L"正在删除数据流…");
                    std::wstring error;
                    if (DeleteAlternateStream(t->targetDir, streamName, &error)) ++result.okCount;
                    else result.issues.push_back({ t->targetDir + L":" + streamName, error });
                }
            }
        } else if (op == L"rename") {
            if (t->sources.size() >= 2) {
                std::wstring err;
                if (RenamePath(t->sources[0], t->sources[1], &err)) ++result.okCount;
                else result.issues.push_back({ t->sources[0], err });
            }
        } else if (op == L"killprocess") {
            if (t->sources.empty()) {
                result.issues.push_back({ L"进程 PID", L"缺少进程 PID" });
            } else {
                wchar_t* end = nullptr;
                const unsigned long long parsed =
                    wcstoull(t->sources[0].c_str(), &end, 10);
                const bool validPid = end != t->sources[0].c_str() &&
                                      *end == L'\0' && parsed > 0 &&
                                      parsed <= MAXDWORD;
                const DWORD pid = validPid ? static_cast<DWORD>(parsed) : 0;
                std::wstring error;
                if (!validPid) {
                    result.issues.push_back({ L"进程 PID", L"进程 PID 无效" });
                } else if (TerminateProcessById(pid, &error)) ++result.okCount;
                else result.issues.push_back({ L"PID " + std::to_wstring(pid), error });
            }
        } else if (op == L"closehandle") {
            if (t->sources.size() < 2) {
                result.issues.push_back(
                    { L"进程 PID / 文件句柄", L"缺少进程 PID 或句柄值" });
            } else {
                wchar_t* pidEnd = nullptr;
                const unsigned long long parsedPid =
                    wcstoull(t->sources[0].c_str(), &pidEnd, 10);
                const bool validPid = pidEnd != t->sources[0].c_str() &&
                                      *pidEnd == L'\0' && parsedPid > 0 &&
                                      parsedPid <= MAXDWORD;
                const DWORD pid =
                    validPid ? static_cast<DWORD>(parsedPid) : 0;
                if (!validPid) {
                    result.issues.push_back({ L"进程 PID", L"进程 PID 无效" });
                } else {
                    for (size_t i = 1; i < t->sources.size(); ++i) {
                        wchar_t* end = nullptr;
                        const ULONG_PTR handle = static_cast<ULONG_PTR>(
                            _wcstoui64(t->sources[i].c_str(), &end, 10));
                        if (handle == 0 || end == t->sources[i].c_str() ||
                            *end != L'\0') {
                            result.issues.push_back(
                                { L"PID " + std::to_wstring(pid),
                                  L"文件句柄值无效" });
                            continue;
                        }
                        std::wstring error;
                        if (CloseRemoteFileHandle(pid, handle, &error)) {
                            ++result.okCount;
                        } else {
                            wchar_t handleText[32]{};
                            swprintf_s(handleText, L"0x%llX",
                                       static_cast<unsigned long long>(handle));
                            result.issues.push_back(
                                { L"PID " + std::to_wstring(pid) +
                                      L" · 句柄 " + handleText,
                                  error });
                        }
                    }
                }
            }
        } else if (op == L"launch") {
            if (t->targetDir.empty()) {
                result.issues.push_back({ L"启动目标", L"启动路径为空" });
            } else {
                t->SetCurrent(L"正在启动…");
                std::wstring error;
                const std::wstring commandLine =
                    L"\"" + StripExtendedPath(t->targetDir) + L"\"";
                if (LaunchCommand(commandLine, ParentPath(t->targetDir), &error)) {
                    ++result.okCount;
                } else {
                    result.issues.push_back({
                        t->targetDir,
                        error.empty() ? L"后台进程无法启动目标" : error
                    });
                }
            }
        } else if (op == L"newfolder") {
            if (CreateDirectoryPath(t->targetDir, nullptr)) ++result.okCount;
            else result.issues.push_back({ t->targetDir, L"新建文件夹失败" });
        } else if (op == L"newfile") {
            if (CreateEmptyFile(t->targetDir, nullptr)) ++result.okCount;
            else result.issues.push_back({ t->targetDir, L"新建文件失败" });
        } else {
            result.issues.push_back({ L"", L"未知操作：" + op });
        }

        t->AddIssues(result.issues);

        // 普通错误（占用/拒绝访问/找不到…）也要能"重试 / 忽略"：
        // 把它们同样登记成待决项，任务停在这里等，决策就地执行。
        for (const auto& issue : result.issues) {
            if (issue.kind != 1 && CanRetryIssue(op))
                t->AddPending(issue.path, issue.target);
        }

        // 有冲突在等决策：任务不算结束，就停在这里等（**同一个任务**）。
        // 用户点覆盖/跳过只是给它一个决定，进度条继续走，不会冒出第二个任务。
        while (!t->cancel.load() && !quit.load() && t->PendingCount() > 0) {
            t->state = static_cast<int>(TaskState::Awaiting);
            auto decided = t->TakeDecided();
            for (auto& p : decided) {
                // Remove the old issue first. If the retry fails, the new issue
                // is added back below instead of being accidentally erased by
                // RemoveIssueByPath() after AddIssues().
                t->RemoveIssueByPath(p.src);
                if (p.decision != 1) continue;   // 忽略：只消费原问题

                OpResult again;
                const std::wstring retryOp = t->op;
                std::wstring retryTarget = p.dst;
                if (retryTarget.empty() && !t->targetDir.empty()) {
                    retryTarget = JoinPath(t->targetDir, FileNameOf(p.src));
                }

                if (retryOp == L"delete") {
                    DeleteToRecycleBin({ p.src }, &again);
                } else if (retryOp == L"deleteperm") {
                    DeletePermanent(p.src, &again);
                } else if (retryOp == L"rename") {
                    std::wstring error;
                    if (RenamePath(p.src, retryTarget, &error)) ++again.okCount;
                    else again.issues.push_back({ p.src, error });
                } else if (retryOp == L"newfolder") {
                    std::wstring error;
                    if (CreateDirectoryPath(p.src, &error)) ++again.okCount;
                    else again.issues.push_back({ p.src, error });
                } else if (retryOp == L"newfile") {
                    std::wstring error;
                    if (CreateEmptyFile(p.src, &error)) ++again.okCount;
                    else again.issues.push_back({ p.src, error });
                } else if (retryOp == L"copy" || retryOp == L"copyover" ||
                           retryOp == L"paste" || retryOp == L"cut") {
                    CopyOptions use;
                    use.onConflict = [](const std::wstring&, const std::wstring&) {
                        return ConflictAction::Overwrite;
                    };
                    if (retryOp == L"cut") {
                        MoveTree(p.src, retryTarget, progress, &again, use);
                    } else {
                        CopyTree(p.src, retryTarget, progress, &again, use);
                    }
                } else {
                    again.issues.push_back({ p.src, L"该任务类型不支持重试" });
                }

                if (!again.issues.empty()) {
                    t->AddIssues(again.issues);
                    for (const auto& issue : again.issues) {
                        if (issue.kind != 1 && CanRetryIssue(retryOp))
                            t->AddPending(issue.path, issue.target);
                    }
                } else if (retryOp == L"copy" || retryOp == L"copyover" ||
                           retryOp == L"paste" || retryOp == L"cut") {
                    ++aggFiles;     // 这个文件真的补齐了
                    t->doneFiles = aggFiles;
                    aggBytes += lastDoneBytes;
                    t->doneBytes = aggBytes;
                }
                lastDoneBytes = 0;
                lastDoneFiles = 0;
            }
            if (decided.empty()) Sleep(50);
        }

        const bool hasIssues = !t->GetIssues().empty();
        if (!result.canceled && !t->cancel.load() && !hasIssues) {
            // A successful terminal task must expose a stable final counter even
            // if the last operation did not emit its own progress callback.
            t->doneFiles = t->totalFiles.load();
            t->doneBytes = t->totalBytes.load();
        }
        if (result.canceled || t->cancel.load()) t->state = static_cast<int>(TaskState::Canceled);
        else if (hasIssues) t->state = static_cast<int>(TaskState::Failed);
        else t->state = static_cast<int>(TaskState::Done);
        t->finishedAt = GetTickCount64();
    }
    // 延迟删除的统一入口（只在 UI 泵里调用）
    static void OpenWithOurApp(const std::wstring& filePath) {
        wchar_t self[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;

        const std::wstring cmd =
            L"\"" + std::wstring(self) + L"\" --force-current-process \"" +
            ParentPath(filePath) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(self, const_cast<LPWSTR>(cmd.c_str()), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    std::wstring ApplyStoredSettings() {
        const AppSettings stored = LoadSettings();
        pauseOnError = stored.pauseOnError;
        focusOnError = stored.focusOnError;
        autoElevate = stored.autoElevate;
        executableViaShell = stored.executableViaShell;
        showHiddenFiles = stored.showHiddenFiles;
        hideKnownExtensions = stored.hideKnownExtensions;
        maxConcurrentTasks = std::clamp(stored.maxConcurrentTasks, 1, 16);
        taskShowDelayMs = std::clamp(stored.taskShowDelayMs, 0, 10000);
        taskDoneLingerMs = std::clamp(stored.taskDoneLingerMs, 0, 60000);
        taskRetentionMinutes = std::clamp(stored.taskRetentionMinutes, 1, 1440);
        beepOnError = stored.beepOnError;
        confirmRecycleDelete = stored.confirmRecycleDelete;
        autoStartHost = stored.autoStartHost;
        shellContextMenu = stored.shellContextMenu;
        defaultFileManager = stored.defaultFileManager;
        winEShortcut = stored.winEShortcut;

        taskUI.SetTiming(
            static_cast<std::uint64_t>(taskShowDelayMs.load()),
            static_cast<std::uint64_t>(taskDoneLingerMs.load()),
            beepOnError.load());
        slotCv.notify_all();

        std::wstring registrationError;
        if (!SetAutoStartHost(autoStartHost.load(), &registrationError) ||
            !SetShellContextMenu(shellContextMenu.load(), &registrationError) ||
            !SetDefaultFileManager(defaultFileManager.load(), &registrationError)) {
            return registrationError;
        }

        std::wstring hookError;
        if (!SetWinEHookEnabled(winEShortcut.load(), &hookError)) {
            winEShortcut = false;
            AppSettings fixed = stored;
            fixed.winEShortcut = false;
            SaveSettings(fixed);
            return hookError;
        }
        return {};
    }

    void ResetAllSettings() {
        SetWinEHookEnabled(false, nullptr);
        ResetSettings();
        const std::wstring warning = ApplyStoredSettings();
        if (settingsWin) {
            settingsWin->Close();
            settingsWin.reset();
        }
        if (!warning.empty()) {
            tray.ShowBalloon(L"设置已重置，但有一个开关未启用", warning,
                             TrayIcon::BalloonIcon::Warning);
        } else {
            tray.ShowBalloon(L"设置已重置",
                             L"启动方式、排序、布局、任务参数、开机启动、Shell 集成和快捷访问固定项均已恢复默认。",
                             TrayIcon::BalloonIcon::Info);
        }
    }

    void ReopenSettingsWithError(const std::wstring& title,
                                 const std::wstring& error) {
        auto old = settingsWin;
        settingsWin.reset();
        settingsDrawer.reset();
        ShowSettings();
        if (settingsDrawer)
            settingsDrawer->Show(title, error, ui::NoticeKind::Error);
        if (old) old->Close();
    }

    void ShowAbout() {
        if (aboutWin && aboutWin->IsValid()) {
            aboutWin->Show();
            aboutWin->ShowActivate(Window::ActivateMode::All);
            return;
        }

        auto w = std::make_shared<Window>();
        if (!w->Create(560, 500, L"关于 ZExplorer")) return;
        ze::platform::AllowLowerIntegrityMessages(w->GetHwnd());
        w->SetMinSize(480, 400);
        w->SetIcon(ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CXSMICON)),
                   ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CXSMICON)));

        auto root = w->GetRootColumnBox();
        root->SetMargin(Thickness(18, 16, 18, 16));
        root->SetSpacing(12.0f);

        auto titleRow = std::make_shared<RowBox>();
        titleRow->SetSpacing(12.0f);
        titleRow->SetHeight(38.0f);
        auto title = std::make_shared<Label>(L"ZExplorer");
        title->SetFillWidth(true);
        titleRow->AddChild(title);
        auto version = std::make_shared<Label>(L"0.1.0 · x64");
        version->SetTextColor(
            ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
        titleRow->AddChild(version);
        root->AddChild(titleRow);

        auto scroll = std::make_shared<ScrollViewer>();
        scroll->SetFillWidth(true);
        scroll->SetFillHeight(true);
        root->AddChild(scroll);
        auto content = std::make_shared<ColumnBox>();
        content->SetSpacing(12.0f);
        content->SetFillWidth(true);
        scroll->SetContent(content);

        auto addCard = [&](const wchar_t* heading, const wchar_t* body) {
            auto col = std::make_shared<ColumnBox>();
            col->SetSpacing(7.0f);
            col->SetFillWidth(true);
            col->AddChild(std::make_shared<Label>(heading));
            auto text = std::make_shared<Label>(body);
            text->SetTextOverflow(Label::TextOverflow::Wrap);
            text->SetMaxLines(0);
            text->SetFillWidth(true);
            text->SetTextColor(
                ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
            col->AddChild(text);
            auto card = std::make_shared<Card>();
            card->SetContent(col);
            card->SetFillWidth(true);
            content->AddChild(card);
        };

        addCard(L"产品定位",
                L"面向高级用户的 Windows 文件操作外壳。保留资源管理器习惯，同时集中提供文件流、占用分析、权限与属性等深层能力。");
        addCard(L"运行方式",
                L"前台窗口只负责交互；文件操作、提权启动和高风险句柄处理由后台进程执行。命令先进入可审查的任务队列，再按并发与错误策略运行。");
        addCard(L"当前集成",
                L"后台托盘、任务历史、Shell 上下文菜单、文件夹默认打开方式、Win+E 接管和开机启动均为显式开关，默认关闭。接管设置只写当前用户作用域。");

        auto actions = std::make_shared<RowBox>();
        actions->SetSpacing(10.0f);
        actions->SetHeight(34.0f);
        auto settings = std::make_shared<Button>(L"打开设置");
        settings->SetWidth(108.0f);
        settings->SetHeight(32.0f);
        settings->Connect(settings->Clicked, [this]() { ShowSettings(); });
        actions->AddChild(settings);
        auto close = std::make_shared<Button>(L"关闭");
        close->SetWidth(88.0f);
        close->SetHeight(32.0f);
        close->Connect(close->Clicked, [w]() { w->Close(); });
        actions->AddChild(close);
        root->AddChild(actions);

        w->Show();
        w->ShowActivate(Window::ActivateMode::All);
        aboutWin = w;
    }

    void ShowSettings() {
        if (settingsWin && settingsWin->IsValid()) {
            settingsWin->Show();
            settingsWin->ShowActivate(Window::ActivateMode::All);
            return;
        }
        auto w = std::make_shared<Window>();
        if (!w->Create(720, 760, L"ZExplorer 设置")) return;
        ze::platform::AllowLowerIntegrityMessages(w->GetHwnd());
        w->SetMinSize(600, 500);
        w->SetIcon(ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CXSMICON)), ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CXSMICON)));

        auto root = w->GetRootColumnBox();
        root->SetMargin(Thickness(18, 16, 18, 16));
        root->SetSpacing(12);

        auto heading = std::make_shared<RowBox>();
        heading->SetSpacing(12.0f);
        heading->SetHeight(34.0f);
        auto title = std::make_shared<Label>(L"设置");
        title->SetFillWidth(true);
        heading->AddChild(title);
        auto savedHint = std::make_shared<Label>(L"修改后点击底部保存");
        savedHint->SetTextColor(ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
        heading->AddChild(savedHint);
        root->AddChild(heading);

        auto tabs = std::make_shared<TabView>();
        tabs->SetFillWidth(true);
        tabs->SetFillHeight(true);
        root->AddChild(tabs);

        std::shared_ptr<ColumnBox> activeContent;
        auto addTabPage = [&](const wchar_t* titleText) {
            auto pageContent = std::make_shared<ColumnBox>();
            pageContent->SetSpacing(12.0f);
            pageContent->SetFillWidth(true);
            auto pageScroll = std::make_shared<ScrollViewer>();
            pageScroll->SetFillWidth(true);
            pageScroll->SetFillHeight(true);
            pageScroll->SetContent(pageContent);
            tabs->AddTab(titleText, pageScroll);
            return pageContent;
        };
        const auto browsePage = addTabPage(L"启动与浏览");
        const auto filesPage = addTabPage(L"文件列表");
        const auto tasksPage = addTabPage(L"任务与后台");
        const auto systemPage = addTabPage(L"系统与维护");
        const auto draft = std::make_shared<AppSettings>(LoadSettings());
        const AppSettings original = *draft;

        auto addSection = [&](const wchar_t* sectionTitle,
                              const wchar_t* description) {
            auto inner = std::make_shared<ColumnBox>();
            inner->SetSpacing(8.0f);
            inner->SetFillWidth(true);
            inner->AddChild(std::make_shared<Label>(sectionTitle));
            if (description && *description) {
                auto desc = std::make_shared<Label>(description);
                desc->SetTextOverflow(Label::TextOverflow::Wrap);
                desc->SetMaxLines(0);
                desc->SetFillWidth(true);
                desc->SetTextColor(
                    ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
                inner->AddChild(desc);
            }
            auto card = std::make_shared<Card>();
            card->SetContent(inner);
            card->SetFillWidth(true);
            activeContent->AddChild(card);
            return inner;
        };

        auto addToggle = [&](const std::shared_ptr<ColumnBox>& parent,
                             const wchar_t* text,
                             bool AppSettings::* member) {
            auto row = std::make_shared<RowBox>();
            row->SetSpacing(10.0f);
            row->SetHeight(34.0f);

            auto label = std::make_shared<Label>(text);
            label->SetFillWidth(true);
            row->AddChild(label);

            auto button = std::make_shared<ToggleSwitch>(draft.get()->*member);
            button->SetWidth(52.0f);
            button->SetHeight(28.0f);
            button->Toggled.connect([draft, member](bool on) {
                draft.get()->*member = on;
            });
            row->AddChild(button);
            parent->AddChild(row);
            return button;
        };

        auto addSlider = [&](const std::shared_ptr<ColumnBox>& parent,
                             const wchar_t* text,
                             int AppSettings::* member,
                             int minValue, int maxValue, int step) {
            auto row = std::make_shared<RowBox>();
            row->SetSpacing(10.0f);
            row->SetHeight(34.0f);
            auto label = std::make_shared<Label>(text);
            label->SetFillWidth(true);
            row->AddChild(label);
            auto slider = std::make_shared<Slider>();
            slider->SetWidth(190.0f);
            slider->SetRange(static_cast<float>(minValue), static_cast<float>(maxValue));
            slider->SetStep(static_cast<float>((std::max)(1, step)));
            slider->SetValue(static_cast<float>(
                std::clamp(draft.get()->*member, minValue, maxValue)));
            auto number = std::make_shared<NumberBox>();
            number->SetWidth(118.0f);
            number->SetHeight(32.0f);
            number->SetRange(static_cast<double>(minValue),
                             static_cast<double>(maxValue));
            number->SetStep(static_cast<double>((std::max)(1, step)));
            number->SetDecimals(0);
            number->SetSpinButtons(true);
            number->SetDefaultValue(static_cast<double>(
                std::clamp(draft.get()->*member, minValue, maxValue)));
            number->SetValue(static_cast<double>(
                std::clamp(draft.get()->*member, minValue, maxValue)), false);
            row->AddChild(slider);
            row->AddChild(number);
            parent->AddChild(row);

            auto updating = std::make_shared<bool>(false);
            slider->ValueChanged.connect(
                [number, draft, member, updating](float v) {
                    if (*updating) return;
                    *updating = true;
                    const int n = static_cast<int>(std::lround(v));
                    draft.get()->*member = n;
                    number->SetValue(static_cast<double>(n), false);
                    *updating = false;
                });
            number->ValueChanged.connect(
                [slider, draft, member, updating](double v) {
                if (*updating) return;
                *updating = true;
                const int n = static_cast<int>(std::lround(v));
                draft.get()->*member = n;
                slider->SetValue(static_cast<float>(n));
                *updating = false;
            });
        };

        auto addCombo = [&](const std::shared_ptr<ColumnBox>& parent,
                            const wchar_t* text,
                            const std::vector<std::wstring>& items,
                            int selected,
                            std::function<void(int)> onChanged) {
            auto row = std::make_shared<RowBox>();
            row->SetSpacing(10.0f);
            row->SetHeight(34.0f);
            auto label = std::make_shared<Label>(text);
            label->SetFillWidth(true);
            row->AddChild(label);
            auto combo = std::make_shared<ComboBox>();
            combo->SetWidth(240.0f);
            combo->SetHeight(30.0f);
            combo->SetItems(items);
            combo->SetSelectedIndex(
                std::clamp(selected, 0, static_cast<int>(items.size()) - 1));
            combo->SelectionChanged.connect(std::move(onChanged));
            row->AddChild(combo);
            parent->AddChild(row);
            return combo;
        };

        auto applyRuntime = [this](const AppSettings& target) {
            autoElevate = target.autoElevate;
            pauseOnError = target.pauseOnError;
            focusOnError = target.focusOnError;
            executableViaShell = target.executableViaShell;
            showHiddenFiles = target.showHiddenFiles;
            hideKnownExtensions = target.hideKnownExtensions;
            maxConcurrentTasks = target.maxConcurrentTasks;
            taskShowDelayMs = target.taskShowDelayMs;
            taskDoneLingerMs = target.taskDoneLingerMs;
            taskRetentionMinutes = target.taskRetentionMinutes;
            beepOnError = target.beepOnError;
            confirmRecycleDelete = target.confirmRecycleDelete;
            autoStartHost = target.autoStartHost;
            shellContextMenu = target.shellContextMenu;
            defaultFileManager = target.defaultFileManager;
            winEShortcut = target.winEShortcut;
            taskUI.SetTiming(
                static_cast<std::uint64_t>(taskShowDelayMs.load()),
                static_cast<std::uint64_t>(taskDoneLingerMs.load()),
                beepOnError.load());
            slotCv.notify_all();
        };

        const AppSettings& initial = *draft;
        activeContent = browsePage;
        const auto launchSection = addSection(
            L"启动与浏览",
            L"命令行或资源管理器传入的路径优先于这里的启动位置。排序、窗口位置、主分栏、快捷访问分栏和四列宽度都会自动记住。");
        addCombo(launchSection, L"启动时打开",
                 { L"此电脑", L"上次关闭时的目录", L"指定路径" },
                 static_cast<int>(initial.startupMode),
                 [draft](int selected) {
                     draft->startupMode = static_cast<StartupMode>(
                         std::clamp(selected, 0, 2));
                 });

        auto pathRow = std::make_shared<RowBox>();
        pathRow->SetSpacing(10.0f);
        pathRow->SetHeight(36.0f);
        auto pathLabel = std::make_shared<Label>(L"指定路径");
        pathLabel->SetWidth(140.0f);
        pathRow->AddChild(pathLabel);
        auto pathBox = std::make_shared<TextBox>();
        pathBox->SetPlaceholder(L"%TEMP%\\ 或 C:\\project");
        pathBox->SetText(initial.startupPath);
        pathBox->SetFillWidth(true);
        pathBox->SetHeight(32.0f);
        pathRow->AddChild(pathBox);
        launchSection->AddChild(pathRow);
        auto pathHint = std::make_shared<Label>(
            L"支持环境变量；按 Enter 或移开焦点后保存。");
        pathHint->SetTextColor(
            ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
        launchSection->AddChild(pathHint);
        auto commitPath = [draft, pathBox, pathHint]() {
            const std::wstring raw = pathBox->GetText();
            const std::wstring expanded = ExpandEnvironmentVariables(raw);
            if (!raw.empty() && !PathExists(expanded)) {
                pathBox->SetError(true);
                pathHint->SetText(L"该路径当前不存在；已保留文本，启动时仍会回退到“此电脑”。");
                return;
            }
            pathBox->SetError(false);
            pathHint->SetText(
                raw.empty() ? L"指定路径为空时，启动会回退到“此电脑”。"
                            : L"已保存为启动路径：" + expanded);
            draft->startupPath = raw;
        };
        pathBox->Connect(pathBox->ReturnPressed, commitPath);
        pathBox->Connect(pathBox->Blurred, commitPath);

        addCombo(launchSection, L"默认排序",
                 { L"名称", L"大小", L"类型", L"修改日期" },
                 initial.sortKey,
                 [draft](int selected) {
                     draft->sortKey = std::clamp(selected, 0, 3);
                 });
        addCombo(launchSection, L"排序方向", { L"升序", L"降序" },
                 initial.sortAscending ? 0 : 1,
                 [draft](int selected) {
                     draft->sortAscending = selected == 0;
                 });

        activeContent = filesPage;
        const auto browseSection = addSection(
            L"文件列表",
            L"删除确认只控制“移到回收站”；永久删除始终要求确认。");
        addToggle(browseSection, L"显示隐藏和系统文件",
                  &AppSettings::showHiddenFiles);
        addToggle(browseSection, L"隐藏已知扩展名",
                  &AppSettings::hideKnownExtensions);
        addToggle(browseSection, L"打开可执行文件时使用 ShellExecute",
                  &AppSettings::executableViaShell);
        auto launchHint = std::make_shared<Label>(
            L"默认开启。关闭后，EXE/COM/BAT/CMD/PS1/SCR/MSI 会交给后台进程，按后台进程的权限启动。");
        launchHint->SetTextOverflow(Label::TextOverflow::Wrap);
        launchHint->SetMaxLines(0);
        launchHint->SetTextColor(ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
        launchHint->SetFillWidth(true);
        browseSection->AddChild(launchHint);
        addToggle(browseSection, L"加入回收站前确认",
                  &AppSettings::confirmRecycleDelete);

        activeContent = tasksPage;
        const auto taskSection = addSection(
            L"任务与后台",
            L"这些参数直接控制任务队列、任务页签出现时机和历史清理。");
        addSlider(taskSection, L"最大并发任务数",
                  &AppSettings::maxConcurrentTasks, 1, 16, 1);
        addSlider(taskSection, L"任务页签弹出阈值（毫秒）",
                  &AppSettings::taskShowDelayMs, 0, 2000, 50);
        addSlider(taskSection, L"完成页签停留时间（毫秒）",
                  &AppSettings::taskDoneLingerMs, 0, 5000, 100);
        addSlider(taskSection, L"任务历史保留（分钟）",
                  &AppSettings::taskRetentionMinutes, 1, 1440, 1);
        addToggle(taskSection, L"发生错误时暂停等待处理",
                  &AppSettings::pauseOnError);
        addToggle(taskSection, L"发生错误时切换到任务页签并前置窗口",
                  &AppSettings::focusOnError);
        addToggle(taskSection, L"出错时播放系统提示音",
                  &AppSettings::beepOnError);

        activeContent = systemPage;
        const auto systemSection = addSection(
            L"系统集成",
            L"所有接管项默认关闭。启用后只写入当前用户注册表；关闭会恢复原来的文件夹打开命令。Windows 11 的经典菜单项位于右键菜单的“显示更多选项”中。");
        addToggle(systemSection, L"启动时自动请求管理员权限（下次启动生效）",
                  &AppSettings::autoElevate);
        addToggle(systemSection, L"开机启动后台",
                  &AppSettings::autoStartHost);
        addToggle(systemSection, L"资源管理器右键“用 ZExplorer 打开”",
                  &AppSettings::shellContextMenu);
        addToggle(systemSection, L"接管文件夹默认打开方式",
                  &AppSettings::defaultFileManager);
        addToggle(systemSection, L"后台运行时用 Win+E 打开新窗口",
                  &AppSettings::winEShortcut);

        const auto maintenanceSection = addSection(
            L"维护",
            L"重置会恢复所有设置默认值，撤销开机启动、资源管理器菜单和文件夹接管，并清除 ZExplorer 快捷访问固定项。");
        auto resetAll = std::make_shared<Button>(L"一键重置所有设置");
        resetAll->SetHeight(34.0f);
        resetAll->SetWidth(220.0f);
        const Color danger =
            ThemeManager::AppTheme().Get(ThemeRole::Danger);
        resetAll->SetColors(
            danger,
            Color::Lerp(danger, Color::FromArgb(255, 0, 0, 0), 0.10f),
            Color::Lerp(danger, Color::FromArgb(255, 0, 0, 0), 0.20f));
        resetAll->Connect(resetAll->Clicked, [this]() { ResetAllSettings(); });
        maintenanceSection->AddChild(resetAll);

        auto footer = std::make_shared<RowBox>();
        footer->SetSpacing(10.0f);
        footer->SetHeight(36.0f);
        auto cancel = std::make_shared<Button>(L"取消");
        cancel->SetWidth(92.0f);
        cancel->SetHeight(34.0f);
        cancel->Connect(cancel->Clicked, [this]() {
            if (settingsWin) settingsWin->Close();
        });
        footer->AddChild(cancel);
        auto save = std::make_shared<Button>(L"保存并关闭");
        save->SetWidth(132.0f);
        save->SetHeight(34.0f);
        save->Connect(save->Clicked, [this, w, draft, original, applyRuntime, save]() {
            save->SetEnabled(false);
            const AppSettings target = *draft;
            const bool winEChanged =
                target.winEShortcut != original.winEShortcut;
            if (winEChanged) {
                std::wstring hookError;
                if (!SetWinEHookEnabled(target.winEShortcut, &hookError)) {
                    save->SetEnabled(true);
                    PostUi([this, hookError]() {
                        ReopenSettingsWithError(L"设置保存失败", hookError);
                    });
                    return;
                }
            }

            RunAsync<std::wstring>(
                AsyncOwner{ w },
                [target, original](AsyncCancelToken token,
                                   AsyncProgress) -> std::wstring {
                    token.ThrowIfCancellationRequested();
                    if (target.startupMode == StartupMode::SpecifiedPath &&
                        !target.startupPath.empty() &&
                        !PathExists(ExpandEnvironmentVariables(
                            target.startupPath))) {
                        return L"指定启动路径不存在：\n" +
                               target.startupPath;
                    }

                    auto revertIntegrations = [&]() {
                        SetDefaultFileManager(
                            original.defaultFileManager, nullptr);
                        SetShellContextMenu(original.shellContextMenu, nullptr);
                        SetAutoStartHost(original.autoStartHost, nullptr);
                    };
                    std::wstring stepError;
                    if (!SetAutoStartHost(target.autoStartHost, &stepError) ||
                        !SetShellContextMenu(target.shellContextMenu,
                                             &stepError) ||
                        !SetDefaultFileManager(target.defaultFileManager,
                                               &stepError)) {
                        revertIntegrations();
                        return stepError;
                    }
                    if (!SaveSettings(target, &stepError)) {
                        revertIntegrations();
                        return stepError;
                    }
                    return {};
                },
                [this, target, original, winEChanged,
                 applyRuntime](std::wstring error) {
                    if (error.empty()) {
                        applyRuntime(target);
                        if (settingsWin) settingsWin->Close();
                        return;
                    }
                    if (winEChanged)
                        SetWinEHookEnabled(original.winEShortcut, nullptr);
                    PostUi([this, error]() {
                        ReopenSettingsWithError(L"设置保存失败", error);
                    });
                },
                [this, original, winEChanged](std::exception_ptr) {
                    if (winEChanged)
                        SetWinEHookEnabled(original.winEShortcut, nullptr);
                    PostUi([this]() {
                        ReopenSettingsWithError(
                            L"设置保存失败",
                            L"后台保存设置时发生异常。");
                    });
                });
        });
        footer->AddChild(save);
        root->AddChild(footer);

        settingsDrawer = std::make_shared<ui::NotificationDrawer>(root);
        if (auto drawerLayout =
                std::dynamic_pointer_cast<Layout>(settingsDrawer->Root())) {
            w->SetRootLayout(drawerLayout);
        }
        w->Show();
        w->ShowActivate(Window::ActivateMode::All);
        settingsWin = w;
    }

    std::vector<std::wstring> Handle(ipc::Msg type, const std::vector<std::wstring>& args) {
        switch (type) {
        case ipc::Msg::Ping: {
            int pending = 0;
            for (auto& t : Snapshot()) if (t->state.load() <= 1) ++pending;
            return { L"1", L"pong", std::to_wstring(taskUI.TabCount()), std::to_wstring(pending),
                     taskUI.TabDiagnostics() };
        }

        case ipc::Msg::SubmitTask: {
            if (args.size() < 2) return { L"0", L"参数不足" };

            // 只写剪贴板（Ctrl+C / Ctrl+X）不是"操作"，不建任务、不在任务窗里出现
            const std::wstring op = args[0];
            const std::wstring target = args[1];
            if ((op == L"copy" || op == L"cut") && target.empty()) {
                std::vector<std::wstring> paths(args.begin() + 2, args.end());
                std::wstring err;
                if (!SetClipboardFiles(paths, op == L"cut", &err)) return { L"0", err };
                return { L"1", L"0" };      // id=0 表示"不是任务"
            }

            int id = 0;
            Enqueue(op, target,
                    std::vector<std::wstring>(args.begin() + 2, args.end()), &id);
            return { L"1", std::to_wstring(id) };
        }

        case ipc::Msg::SubmitTaskFrom: {
            if (args.size() < 4) return { L"0", L"参数不足" };

            wchar_t* end = nullptr;
            const unsigned long long parsedPid =
                wcstoull(args[0].c_str(), &end, 10);
            const DWORD originPid =
                end != args[0].c_str() && *end == L'\0' &&
                    parsedPid <= MAXDWORD
                ? static_cast<DWORD>(parsedPid)
                : 0;
            const std::wstring& originPath = args[1];
            const std::wstring op = args[2];
            const std::wstring target = args[3];

            if ((op == L"copy" || op == L"cut") && target.empty()) {
                std::vector<std::wstring> paths(args.begin() + 4, args.end());
                std::wstring err;
                if (!SetClipboardFiles(paths, op == L"cut", &err))
                    return { L"0", err };
                return { L"1", L"0" };
            }

            int id = 0;
            Enqueue(op, target,
                    std::vector<std::wstring>(args.begin() + 4, args.end()),
                    &id, originPid, originPath);
            return { L"1", std::to_wstring(id) };
        }

        case ipc::Msg::NewWindow: {
            const std::wstring path = args.empty() ? std::wstring() : args[0];
            if (!path.empty() && path != L"::thispc" && !PathExists(path))
                return { L"0", L"目录不存在" };

            wchar_t self[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
                return { L"0", L"无法取得程序路径" };

            std::wstring command =
                L"\"" + std::wstring(self) + L"\" --force-current-process";
            if (!path.empty() && path != L"::thispc")
                command += L" \"" + path + L"\"";
            std::vector<wchar_t> mutableCommand(command.begin(), command.end());
            mutableCommand.push_back(L'\0');

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            const DWORD flags = CREATE_UNICODE_ENVIRONMENT;
            if (!CreateProcessW(
                    self, mutableCommand.data(), nullptr, nullptr, FALSE, flags,
                    nullptr,
                    (!path.empty() && IsDirectoryPath(path)) ? path.c_str() : nullptr,
                    &si, &pi)) {
                return { L"0", L"无法创建新窗口" };
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return { L"1", L"已创建新窗口" };
        }

        case ipc::Msg::QueryTasks: {
            std::lock_guard<std::mutex> lock(snapMutex);
            return snapCache;
        }

        case ipc::Msg::ShowTask: {
            if (args.empty()) return { L"0", L"缺少 id" };
            const int id = _wtoi(args[0].c_str());
            PostUi([this, id]() { taskUI.ShowTask(id); });
            return { L"1" };
        }

        case ipc::Msg::ShowSettings:
            PostUi([this]() { ShowSettings(); });
            return { L"1" };

        case ipc::Msg::ShowAbout:
            PostUi([this]() { ShowAbout(); });
            return { L"1" };

        case ipc::Msg::CancelTask: {
            if (args.empty()) return { L"0", L"缺少 id" };
            const int id = _wtoi(args[0].c_str());
            auto all = Snapshot();
            if (auto t = FindTask(id)) { t->cancel = true; return { L"1" }; }
            // 诊断：把收到的 id 与现有任务一起返回
            std::wstring ids;
            for (auto& x : all) ids += std::to_wstring(x->id) + L",";
            return { L"0", L"没有这个任务", L"id=" + std::to_wstring(id), L"count=" + std::to_wstring(all.size()), L"have=" + ids };
        }

        case ipc::Msg::QuitHost:
            HostApp::Instance().RequestQuit();
            return { L"1" };

        default:
            return { L"0", L"未知消息" };
        }
    }

    void BuildTray() {
        trayMenu = std::make_shared<Menu>();
        trayMenu->AddItem(L"显示任务窗口", [this]() { taskUI.ShowAndFocus(); });
        trayMenu->AddItem(L"打开新的操作窗口", [this]() {
            LaunchNewUiProcess();
        });
        trayMenu->AddItem(L"设置…", [this]() { ShowSettings(); });
        trayMenu->AddItem(L"关于 ZExplorer…", [this]() { ShowAbout(); });
        trayMenu->AddItem(L"重置所有设置", [this]() { ResetAllSettings(); });
        trayMenu->AddSeparator();
        trayMenu->AddItem(L"退出 ZExplorer 后台", [this]() { HostApp::Instance().RequestQuit(); });

        HICON icon = ze::platform::LoadAppIcon(16, 16);
        tray.Add(icon, L"ZExplorer 后台进程");
        tray.SetMenu(trayMenu);
        tray.DoubleClicked.connect([this]() { taskUI.ShowAndFocus(); });
    }
};

// ---------------------------------------------------------------------------
HostApp& HostApp::Instance() {
    static HostApp inst;
    return inst;
}

HostApp::HostApp() : impl_(std::make_unique<Impl>()) {}
HostApp::~HostApp() = default;

bool HostApp::Start() {
    impl_->mainWin = std::make_shared<Window>();
    if (!impl_->mainWin->Create(320, 200, L"ZExplorer 后台")) return false;
    ze::platform::AllowLowerIntegrityMessages(impl_->mainWin->GetHwnd());
    impl_->BuildTray();
    const std::wstring settingsWarning = impl_->ApplyStoredSettings();
    if (!settingsWarning.empty()) {
        impl_->tray.ShowBalloon(L"Win+E 接管未启用", settingsWarning,
                                TrayIcon::BalloonIcon::Warning);
    }

    impl_->server.Start([this](ipc::Msg t, const std::vector<std::wstring>& a) {
        return impl_->Handle(t, a);
    });

    // 任务窗的动作回调：UI 只发意图，真正的执行仍然在 HostApp
    TaskWindow::Hooks hooks;
    hooks.onCancel = [this](int id) {
        if (auto t = impl_->FindTask(id)) { t->cancel = true; t->dismissed = true; }
    };
    hooks.onOverwrite = [this](int id, const std::wstring& src, const std::wstring&) {
        if (auto t = impl_->FindTask(id)) t->SetDecision(src, 1);
    };
    hooks.onSkip = [this](int id, const std::wstring& src) {
        if (auto t = impl_->FindTask(id)) t->SetDecision(src, 2);
    };
    hooks.onAllOverwrite = [this](int id) {
        if (auto t = impl_->FindTask(id)) t->SetAllDecisions(1);
    };
    hooks.onAllSkip = [this](int id) {
        if (auto t = impl_->FindTask(id)) t->SetAllDecisions(2);
    };
    hooks.onAllRetry = [this](int id) {
        if (auto t = impl_->FindTask(id)) t->SetAllDecisions(1);
    };
    hooks.onAllIgnore = [this](int id) {
        if (auto t = impl_->FindTask(id)) t->SetAllDecisions(2);
    };
    hooks.onOpenPath = [](const std::wstring& path) { Impl::OpenWithOurApp(path); };
    impl_->taskUI.SetHooks(std::move(hooks));
    impl_->pump = impl_->mainWin->CreateTimer(50);
    impl_->pump->Tick.connect([this]() {
        impl_->DrainUi();
        impl_->PruneHistory();
        impl_->RebuildSnapshot();

        // 任务快照 -> 纯数据 TaskItem，交给任务窗模块去画
        std::vector<TaskItem> items;
        const ULONGLONG now = GetTickCount64();
        for (auto& t : impl_->Snapshot()) {
            TaskItem item;
            item.id = t->id;
            item.originPid = t->originPid;
            item.originPath = t->originPath;
            item.op = t->op;
            item.title = t->title;
            item.targetDir = t->targetDir;
            item.state = t->state.load();
            item.ratio = t->Ratio();
            item.doneFiles = t->doneFiles.load();
            item.totalFiles = t->totalFiles.load();
            const ULONGLONG started = t->startedAt.load();
            const ULONGLONG finished = t->finishedAt.load();
            const ULONGLONG end = finished ? finished : now;
            item.elapsedMs = started && end >= started
                ? static_cast<int>(end - started) : 0;
            item.speedBytes = t->speed.load();
            item.currentFile = t->GetCurrent();
            item.sources = t->sources;
            item.dismissed = t->dismissed.load();
            for (const auto& issue : t->GetIssues()) {
                item.issues.push_back({ issue.path, issue.target, issue.message, issue.kind });
            }
            items.push_back(std::move(item));
        }
        impl_->taskUI.Update(items);
    });
    impl_->pump->Start(50);
    return true;
}

void HostApp::Run() {
    if (impl_->mainWin) impl_->mainWin->Run();
}

void HostApp::RequestQuit() {
    if (impl_->quit.exchange(true)) return;
    for (auto& t : impl_->Snapshot()) t->cancel = true;

    impl_->slotCv.notify_all();
    SetWinEHookEnabled(false, nullptr);

    // Never join workers in the menu callback or pipe-server callback. A task may
    // still be unwinding, and blocking here makes the tray "exit" appear dead.
    // Queue shutdown on the UI thread, then terminate the message loop. The host
    // process exits through ExitProcess to avoid ZufyUI's static destruction bug.
    impl_->PostUi([this]() {
        impl_->server.Stop();
        if (impl_->pump) impl_->pump->Stop();
        PostQuitMessage(0);
    });
}

} // namespace ze::host
