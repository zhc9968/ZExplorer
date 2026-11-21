#pragma once

#include <windows.h>

namespace ze::platform {

// Updates the Windows taskbar progress for one UI window. Intended for the UI
// thread; a missing/unsupported taskbar is treated as a no-op.
void SetTaskbarProgress(HWND window, bool active, double ratio);
void ClearTaskbarProgress(HWND window);

} // namespace ze::platform
