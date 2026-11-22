// 本文件不使用 PCH、不依赖 ZufyUI。
#include "ipc/Pipe.h"
#include <sddl.h>

namespace ze::ipc {

namespace {
constexpr DWORD kBufferSize = 64 * 1024;

std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (bytes == 0 ||
        !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid) || !sid) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

bool MakePipeSecurity(SECURITY_ATTRIBUTES* sa, PSECURITY_DESCRIPTOR* descriptor) {
    if (!sa || !descriptor) return false;
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) return false;

    // Only the same user (plus SYSTEM/Administrators) may use the task IPC.
    // A Low mandatory label lets an ordinary, non-elevated launcher connect
    // to the elevated host without weakening the DACL to Everyone.
    const std::wstring sddl =
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + sid + L")"
        L"S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, descriptor, nullptr)) {
        return false;
    }
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = *descriptor;
    return true;
}
}

bool CallHost(Msg type, const std::vector<std::wstring>& args,
              std::vector<std::wstring>* reply, DWORD timeoutMs) {
    HANDLE pipe = CreateFileW(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // 等 Host 处理完（管道本身没有超时，用 WaitNamedPipe 之外再配合调用方超时）
    (void)timeoutMs;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const auto frame = Encode(type, args);
    DWORD written = 0;
    if (!WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr)) {
        CloseHandle(pipe);
        return false;
    }

    std::vector<char> buffer(kBufferSize);
    DWORD read = 0;
    const BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    CloseHandle(pipe);
    if (!ok) return false;

    Msg replyType{};
    std::vector<std::wstring> replyArgs;
    if (!Decode(buffer.data(), read, &replyType, &replyArgs)) return false;

    if (reply) *reply = std::move(replyArgs);
    return true;
}

bool HostAlive() {
    return WaitNamedPipeW(PipeName().c_str(), 0) != FALSE;
}

bool PipeServer::Start(Handler handler) {
    if (running_.load()) return true;
    handler_ = std::move(handler);
    running_.store(true);
    thread_ = std::thread([this]() { Loop(); });
    return true;
}

void PipeServer::Stop() {
    if (!running_.exchange(false)) return;

    // 唤醒阻塞中的 ConnectNamedPipe：自己连一次
    HANDLE wake = CreateFileW(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);

    if (thread_.joinable()) thread_.join();
}

void PipeServer::Loop() {
    while (running_.load()) {
        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!MakePipeSecurity(&sa, &descriptor)) {
            Sleep(100);
            continue;
        }
        HANDLE pipe = CreateNamedPipeW(PipeName().c_str(),
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       kBufferSize, kBufferSize, 0, &sa);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected || !running_.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        std::vector<char> buffer(kBufferSize);
        DWORD read = 0;
        if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            Msg type{};
            std::vector<std::wstring> args;
            std::vector<std::wstring> reply;
            if (Decode(buffer.data(), read, &type, &args) && handler_) {
                reply = handler_(type, args);
            } else {
                reply = { L"0", L"消息解析失败" };
            }

            const auto frame = Encode(Msg::Reply, reply);
            DWORD written = 0;
            WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
            FlushFileBuffers(pipe);
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace ze::ipc
