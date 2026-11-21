#include "pch.h"
#include "app/TaskClient.h"

#include "ipc/Pipe.h"
#include "platform/PathUtil.h"

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>

namespace ze::app {

namespace {

struct Request {
    ipc::Msg type;
    std::vector<std::wstring> args;
};

std::mutex g_reqMutex;
std::condition_variable g_reqCv;
std::deque<Request> g_requests;

std::thread g_ioThread;
std::atomic<bool> g_running{ false };
std::atomic<bool> g_hostReady{ false };

// 快照：IO 线程写、UI 线程读
std::mutex g_snapMutex;
std::vector<TaskInfo> g_snapshot;
bool g_snapshotValid = false;

bool Enqueue(Request req) {
    {
        std::lock_guard<std::mutex> lock(g_reqMutex);
        g_requests.push_back(std::move(req));
    }
    g_reqCv.notify_one();
    return true;
}

std::wstring CurrentProcessPath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size()
        ? std::wstring(path.data(), length)
        : std::wstring();
}

std::vector<std::wstring> SplitTabs(const std::wstring& line) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find(L'\t', start);
        if (tab == std::wstring::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

// 解析一行 QueryTasks 结果：
// id, state, ratio, title, current, issueCount, issue, done, total,
// originPid, originPath, op, targetDir, issueDetails
bool ParseLine(const std::wstring& line, TaskInfo* out) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4) return false;

    out->id = _wtoi(fields[0].c_str());
    out->state = _wtoi(fields[1].c_str());
    out->ratio = static_cast<float>(_wtof(fields[2].c_str()));
    out->title = fields[3];
    out->current = fields.size() > 4 ? fields[4] : L"";
    out->issueCount = fields.size() > 5 ? _wtoi(fields[5].c_str()) : 0;
    out->firstIssue = fields.size() > 6 ? fields[6] : L"";
    out->doneFiles = fields.size() > 7
        ? static_cast<std::uint32_t>(_wtoi(fields[7].c_str()))
        : 0;
    out->totalFiles = fields.size() > 8
        ? static_cast<std::uint32_t>(_wtoi(fields[8].c_str()))
        : 0;
    out->originPid = fields.size() > 9
        ? static_cast<DWORD>(_wtoi(fields[9].c_str()))
        : 0;
    out->originPath = fields.size() > 10 ? fields[10] : L"";
    out->op = fields.size() > 11 ? fields[11] : L"";
    out->targetDir = fields.size() > 12 ? fields[12] : L"";
    out->issueDetails = fields.size() > 13 ? fields[13] : L"";
    return true;
}

void PublishSnapshot(std::vector<TaskInfo>&& tasks) {
    std::lock_guard<std::mutex> lock(g_snapMutex);
    g_snapshot = std::move(tasks);
    g_snapshotValid = true;
}

} // namespace

void TaskClient::Start() {
    if (g_running.exchange(true)) return;
    g_ioThread = std::thread([]() { IoLoop(); });
}

void TaskClient::Stop() {
    if (!g_running.exchange(false)) return;
    g_reqCv.notify_all();
    if (g_ioThread.joinable()) g_ioThread.join();
}

void TaskClient::IoLoop() {
    using Clock = std::chrono::steady_clock;
    auto lastQuery = Clock::now() - std::chrono::seconds(1);

    while (g_running.load()) {
        Request req;
        bool hasRequest = false;
        {
            std::unique_lock<std::mutex> lock(g_reqMutex);
            g_reqCv.wait_for(lock, std::chrono::milliseconds(120), []() {
                return !g_requests.empty() || !g_running.load();
            });
            if (!g_requests.empty()) {
                req = std::move(g_requests.front());
                g_requests.pop_front();
                hasRequest = true;
            }
        }
        if (!g_running.load()) break;

        if (hasRequest) {
            std::vector<std::wstring> reply;
            if (!ipc::CallHost(req.type, req.args, &reply)) {
                g_hostReady = false;
                EnsureHost();
                continue;   // 后台刚起来，这条请求丢弃（重试逻辑由调用方重新提交）
            }
            g_hostReady = true;
        }

        // 定期拉一次任务快照（永远在 IO 线程上，不碰 UI 线程）
        const auto now = Clock::now();
        if (now - lastQuery < std::chrono::milliseconds(250)) continue;
        lastQuery = now;

        std::vector<std::wstring> lines;
        if (!ipc::CallHost(ipc::Msg::QueryTasks, {}, &lines)) {
            g_hostReady = false;
            EnsureHost();
            continue;
        }
        g_hostReady = true;

        std::vector<TaskInfo> parsed;
        parsed.reserve(lines.size());
        for (const auto& line : lines) {
            TaskInfo info;
            if (ParseLine(line, &info)) parsed.push_back(std::move(info));
        }
        PublishSnapshot(std::move(parsed));
    }
}

bool TaskClient::EnsureHost() {
    if (ipc::HostAlive()) return true;

    wchar_t self[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;

    const std::wstring exe = self;
    std::wstring cmd = L"\"" + exe + L"\" --host";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (int i = 0; i < 60; ++i) {
        if (ipc::HostAlive()) return true;
        Sleep(50);
    }
    return false;
}

bool TaskClient::Submit(const std::wstring& op, const std::wstring& target,
                        const std::vector<std::wstring>& sources, int* id) {
    if (id) *id = 0;
    std::vector<std::wstring> args;
    args.reserve(sources.size() + 4);
    args.push_back(std::to_wstring(GetCurrentProcessId()));
    args.push_back(CurrentProcessPath());
    args.push_back(op);
    args.push_back(target);
    for (const auto& s : sources) args.push_back(s);
    return Enqueue({ ipc::Msg::SubmitTaskFrom, std::move(args) });
}

bool TaskClient::ShowTask(int id) {
    return Enqueue({ ipc::Msg::ShowTask, { std::to_wstring(id) } });
}

bool TaskClient::CancelTask(int id) {
    return Enqueue({ ipc::Msg::CancelTask, { std::to_wstring(id) } });
}

bool TaskClient::OpenNewWindow(const std::wstring& path) {
    return Enqueue({ ipc::Msg::NewWindow, { path } });
}

bool TaskClient::OpenSettings() {
    return Enqueue({ ipc::Msg::ShowSettings, {} });
}

bool TaskClient::OpenAbout() {
    return Enqueue({ ipc::Msg::ShowAbout, {} });
}

bool TaskClient::Query(std::vector<TaskInfo>* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_snapMutex);
    *out = g_snapshot;
    return g_snapshotValid;
}

bool TaskClient::HostReady() {
    return g_hostReady.load();
}

} // namespace ze::app
