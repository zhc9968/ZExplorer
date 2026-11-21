#include "platform/WindowSecurity.h"

#include "platform/PathUtil.h"

namespace ze::platform {

bool AllowLowerIntegrityMessage(HWND window, UINT message,
                                std::wstring* error) {
    if (!window || !IsWindow(window)) {
        if (error) *error = L"窗口句柄无效";
        return false;
    }
    if (ChangeWindowMessageFilterEx(window, message, MSGFLT_ALLOW, nullptr))
        return true;
    if (error) {
        *error = L"无法放行跨完整性消息 0x" + std::to_wstring(message) +
                 L"：" + FormatWin32Error(GetLastError());
    }
    return false;
}

bool AllowLowerIntegrityMessages(HWND window, std::wstring* error) {
    // WM_COPYGLOBALDATA (0x0049) is the undocumented companion message used by
    // shell drag/drop across integrity levels. WM_DROPFILES covers classic
    // CF_HDROP drops; WM_COPYDATA covers explicit app-to-app payloads.
    constexpr UINT kMessages[] = {
        WM_DROPFILES,
        WM_COPYDATA,
        0x0049
    };
    for (UINT message : kMessages) {
        if (!AllowLowerIntegrityMessage(window, message, error)) return false;
    }
    return true;
}

} // namespace ze::platform
