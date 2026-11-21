#include "platform/TaskbarProgress.h"

#include <shobjidl.h>

namespace ze::platform {

namespace {

ITaskbarList3* g_taskbar = nullptr;
HWND g_window = nullptr;

ITaskbarList3* Taskbar() {
    if (g_taskbar) return g_taskbar;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_taskbar)))) {
        g_taskbar = nullptr;
        return nullptr;
    }
    if (FAILED(g_taskbar->HrInit())) {
        g_taskbar->Release();
        g_taskbar = nullptr;
    }
    return g_taskbar;
}

} // namespace

void SetTaskbarProgress(HWND window, bool active, double ratio) {
    if (!window) return;
    ITaskbarList3* taskbar = Taskbar();
    if (!taskbar) return;

    if (g_window && g_window != window) {
        taskbar->SetProgressState(g_window, TBPF_NOPROGRESS);
    }
    g_window = window;
    if (!active) {
        taskbar->SetProgressState(window, TBPF_NOPROGRESS);
        return;
    }

    ratio = ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
    taskbar->SetProgressState(window, TBPF_NORMAL);
    taskbar->SetProgressValue(window, static_cast<ULONGLONG>(ratio * 1000.0), 1000);
}

void ClearTaskbarProgress(HWND window) {
    if (!window) return;
    if (g_taskbar) g_taskbar->SetProgressState(window, TBPF_NOPROGRESS);
    if (g_window == window) g_window = nullptr;
}

} // namespace ze::platform
