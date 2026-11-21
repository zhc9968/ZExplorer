#pragma once
// ============================================================================
// pch.h —— 预编译头
//
// 与 ZufyUI 自己的 pch.h 策略**相反**：那边把库头排除在 PCH 外，因为它天天改库。
// 这里库是**固定快照**（third_party/ZufyUI @ 939833a），而我们的代码天天改，
// 所以把库头放进 PCH：改动应用代码时复用 PCH，只剩链接时间。
// ============================================================================

#ifdef _DEBUG
#define ZufyUI_DEBUG
#endif

// 启用 ComCtl v6 视觉样式（文件对话框/属性页需要）
#define ZUFYUI_ENABLE_COMCTL_V6

// ---- Win32 / 系统 ----
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <pathcch.h>
#include <propsys.h>
#include <shobjidl.h>

// ---- STL ----
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ---- ZufyUI（固定快照）----
#pragma warning(push, 0)
#include "ZufyUI.h"
#include "ZufyUIWidgets.h"
#include "ZDataViewer.h"
#include "ZufyUIWindowTool.h"
#include "ZufyUIImages.h"
#include "ZufyUIIcons.h"
#pragma warning(pop)
