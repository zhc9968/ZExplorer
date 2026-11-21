#pragma once
// ============================================================================
// ipc/Pipe.h —— 命名管道：一次连接一次请求/响应
//
// 客户端（UI 进程）用 CallHost()；服务端（Host 进程）用 PipeServer。
// 只用 Win32 + STL，不使用 PCH、不依赖 ZufyUI。
// ============================================================================

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ipc/Protocol.h"

namespace ze::ipc {

// 发一条消息给 Host，并同步等回复。Host 没起来时返回 false。
bool CallHost(Msg type, const std::vector<std::wstring>& args,
              std::vector<std::wstring>* reply, DWORD timeoutMs = 3000);

// Host 不可用时的快速探测。
bool HostAlive();

class PipeServer {
public:
    // handler：收到一条消息 -> 返回回复参数（第一项约定为 "1"/"0" 表示成功/失败）
    using Handler = std::function<std::vector<std::wstring>(Msg, const std::vector<std::wstring>&)>;

    PipeServer() = default;
    ~PipeServer() { Stop(); }

    bool Start(Handler handler);
    void Stop();

private:
    void Loop();

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
};

} // namespace ze::ipc
