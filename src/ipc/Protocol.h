#pragma once
// ============================================================================
// ipc/Protocol.h —— UI 进程 <-> 后台 Host 进程的消息格式
//
// 帧：uint32 type + uint32 payloadBytes + UTF-16 负载
// 负载：若干以 L'\0' 结尾的字符串（参数表）
// 不依赖 ZufyUI，也不使用 PCH，两边都能编。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ze::ipc {

enum class Msg : std::uint32_t {
    Ping = 1,
    SubmitTask = 2,     // args: [op, targetDir, src1, src2, ...]
    QueryTasks = 3,     // args: []
    ShowTask = 4,       // args: [id]
    CancelTask = 5,     // args: [id]
    QuitHost = 6,       // args: []
    SubmitTaskFrom = 7, // args: [originPid, originPath, op, targetDir, src1, ...]
    NewWindow = 8,      // args: [path]
    ShowSettings = 9,   // args: []
    ShowAbout = 10,     // args: []
    Reply = 100,        // args: [ok(0/1), text...]
};

// op 取值
inline const wchar_t* kOpCopy = L"copy";
inline const wchar_t* kOpCut = L"cut";
inline const wchar_t* kOpPaste = L"paste";
inline const wchar_t* kOpDelete = L"delete";
inline const wchar_t* kOpDeletePerm = L"deleteperm";
inline const wchar_t* kOpRename = L"rename";
inline const wchar_t* kOpNewFolder = L"newfolder";
inline const wchar_t* kOpNewFile = L"newfile";
inline const wchar_t* kOpStreamImport = L"streamimport";
inline const wchar_t* kOpStreamExport = L"streamexport";
inline const wchar_t* kOpStreamDelete = L"streamdelete";
inline const wchar_t* kOpStreamHide = L"streamhide";
inline const wchar_t* kOpStreamWrite = L"streamwrite";
inline const wchar_t* kOpStreamRename = L"streamrename";
inline const wchar_t* kOpStreamCreate = L"streamcreate";
inline const wchar_t* kOpKillProcess = L"killprocess";
inline const wchar_t* kOpCloseHandle = L"closehandle";
inline const wchar_t* kOpLaunch = L"launch";

inline std::wstring PipeName() { return L"\\\\.\\pipe\\ZExplorer.Host"; }

inline std::vector<char> Encode(Msg type, const std::vector<std::wstring>& args) {
    std::wstring joined;
    for (const auto& a : args) {
        joined += a;
        joined.push_back(L'\0');
    }

    const std::size_t payloadBytes = joined.size() * sizeof(wchar_t);
    std::vector<char> out(sizeof(std::uint32_t) * 2 + payloadBytes);
    auto* p = reinterpret_cast<std::uint32_t*>(out.data());
    p[0] = static_cast<std::uint32_t>(type);
    p[1] = static_cast<std::uint32_t>(payloadBytes);
    if (payloadBytes) std::memcpy(out.data() + sizeof(std::uint32_t) * 2, joined.data(), payloadBytes);
    return out;
}

inline bool Decode(const char* data, std::size_t size, Msg* type, std::vector<std::wstring>* args) {
    if (!type || !args) return false;
    if (size < sizeof(std::uint32_t) * 2) return false;

    auto* p = reinterpret_cast<const std::uint32_t*>(data);
    const std::uint32_t t = p[0];
    const std::uint32_t bytes = p[1];
    if (size < sizeof(std::uint32_t) * 2 + bytes) return false;

    *type = static_cast<Msg>(t);
    args->clear();

    const auto* w = reinterpret_cast<const wchar_t*>(data + sizeof(std::uint32_t) * 2);
    const std::size_t count = bytes / sizeof(wchar_t);
    std::wstring cur;
    for (std::size_t i = 0; i < count; ++i) {
        if (w[i] == L'\0') {
            args->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(w[i]);
        }
    }
    return true;
}

} // namespace ze::ipc
