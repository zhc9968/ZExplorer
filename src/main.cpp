// ============================================================================
// main.cpp —— 入口（**同一个 exe 两种角色**）
//
//   ZExplorer.exe            前台 UI
//   ZExplorer.exe --host     后台进程（托盘 + 任务执行），由前台按需拉起
//
// 前台启动时会先看后台在不在，不在就拉起自己的一份 --host。
//
// 其它命令行：
//   --selftest=<path>        自检：写报告后自动退出（用于无桌面/CI 验证）
// ============================================================================

#include "pch.h"
#include "app/ShellWindow.h"
#include "app/TaskClient.h"
#include "host/HostApp.h"
#include "ipc/Pipe.h"
#include "platform/Settings.h"
#include "platform/ShellActions.h"

namespace {

// 解析 --selftest[=path]。返回报告路径，空串表示不是自检模式。
std::wstring ParseSelfTestArg() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring report;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const std::wstring prefix = L"--selftest";
        if (arg.rfind(prefix, 0) != 0) continue;

        if (arg.size() > prefix.size() && arg[prefix.size()] == L'=') {
            report = arg.substr(prefix.size() + 1);
        } else {
            wchar_t tmp[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmp);
            report = std::wstring(tmp) + L"zexplorer-selftest.txt";
        }
        break;
    }
    LocalFree(argv);
    return report;
}

bool HasArg(const wchar_t* wanted) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], wanted) == 0) { found = true; break; }
    }
    LocalFree(argv);
    return found;
}

// 后台模式：托盘 + 任务队列 + 管道服务
int RunHostMode() {
    // 单实例：命名互斥体。只用"管道是否存在"判断会有竞态——两个进程可能同时
    // 通过检查，然后各自 CreateNamedPipe 成功（PIPE_UNLIMITED_INSTANCES），
    // 于是同一个管道名后面挂了两个后台，请求会被随机分到其中一个。
    static HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\ZExplorer.Host.Singleton");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ZufyUI::ThemeManager::SetFollowSystem(true);

    auto& app = ze::host::HostApp::Instance();
    const bool ok = app.Start();
    if (ok) app.Run();

    if (SUCCEEDED(hrCom)) CoUninitialize();

    // 不返回：库里有静态对象（MenuWindow::StandaloneHolder）在静态析构阶段
    // 会去访问已经销毁的 UIA 表 -> 退出时 c0000005。改用 ExitProcess 跳过 C++ 静态析构。
    // （这是库的问题，已记在 docs/STATUS.md；这里只是绕开）
    ExitProcess(ok ? 0 : 1);
}

// 第一个不以 -- 开头的参数当作"启动时要打开的路径"。
std::wstring ParseInitialPath() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring path;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.empty() || arg[0] == L'-') continue;
        path = arg;
        break;
    }
    LocalFree(argv);
    return path;
}

int RunUiMode() {
    // 设置里开了"自动请求提升权限"且当前不是管理员 -> 提权重启自己
    const auto settings = ze::platform::LoadSettings();
    if (settings.autoElevate && !ze::platform::IsElevated() &&
        ParseSelfTestArg().empty() && !HasArg(L"--no-elevate")) {
        if (ze::platform::RelaunchElevated()) return 0;
    }

    // 启动 IO 线程：所有管道调用都在它上面，UI 线程永不阻塞
    ze::app::TaskClient::Start();

    // 先看后台在不在，没有就拉起自己的一份 --host（在 IO 线程做）
    ze::app::TaskClient::EnsureHost();

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ZufyUI::ThemeManager::SetFollowSystem(true);

    int exitCode = 0;
    {
        auto shell = ze::app::CreateAppShell();
        if (!shell->Create(ParseSelfTestArg(), ParseInitialPath())) {
            exitCode = 1;
        } else {
            shell->Run();
        }
    }   // shell 在 CoUninitialize 之前析构，确保 UI 对象先释放

    ze::app::TaskClient::Stop();

    if (SUCCEEDED(hrCom)) CoUninitialize();
    return exitCode;
}

int RunHostWindowCommand(ze::ipc::Msg type) {
    if (!ze::ipc::HostAlive() && !ze::app::TaskClient::EnsureHost()) return 1;
    std::vector<std::wstring> reply;
    if (!ze::ipc::CallHost(type, {}, &reply) || reply.empty()) return 1;
    return reply.front() == L"1" ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    if (HasArg(L"--host")) return RunHostMode();
    if (HasArg(L"--show-settings"))
        return RunHostWindowCommand(ze::ipc::Msg::ShowSettings);
    if (HasArg(L"--show-about"))
        return RunHostWindowCommand(ze::ipc::Msg::ShowAbout);
    return RunUiMode();
}
