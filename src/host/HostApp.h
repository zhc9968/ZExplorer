#pragma once
// ============================================================================
// host/HostApp.h —— 后台进程：任务队列 + 托盘 + 每个任务一个进度窗口
//
// 所有文件操作都在这里执行，UI 进程只发命令。
// ============================================================================

#include <memory>

namespace ze::host {

class HostApp {
public:
    static HostApp& Instance();

    // 创建隐藏主窗口 + 托盘 + 启动管道服务。
    bool Start();

    // 进入消息循环（阻塞）。
    void Run();

    // 收到 QuitHost 时调用。
    void RequestQuit();

private:
    HostApp();
    ~HostApp();
    HostApp(const HostApp&) = delete;
    HostApp& operator=(const HostApp&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ze::host
