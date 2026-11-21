#pragma once
// ============================================================================
// platform/WindowSecurity.h —— 跨完整性级别的窗口消息白名单
//
// Windows 不支持“关闭整个 UIPI”。高完整性窗口只能按消息类型放行低完整性
// 发送方。这里只放行文件拖放/结构化数据交换需要的消息，键盘鼠标注入不在
// 白名单内。
// ============================================================================

#include <windows.h>

#include <string>

namespace ze::platform {

bool AllowLowerIntegrityMessage(HWND window, UINT message,
                                std::wstring* error = nullptr);

bool AllowLowerIntegrityMessages(HWND window,
                                 std::wstring* error = nullptr);

} // namespace ze::platform
