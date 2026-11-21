// 本文件**不使用 PCH、不包含任何 ZufyUI 头**（见 AGENTS.md §2）。
#include "platform/ShellActions.h"

#include "platform/PathUtil.h"

#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <propkey.h>
#include <propsys.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "oleaut32.lib")

namespace ze::platform {

namespace {

void SetShellError(std::wstring* error, const wchar_t* action) {
    if (!error) return;
    const DWORD code = GetLastError();
    *error = std::wstring(action) + L"：" + FormatWin32Error(code);
}

void SplitCommandLine(const std::wstring& input,
                      std::wstring* executable,
                      std::wstring* arguments) {
    executable->clear();
    arguments->clear();
    size_t i = 0;
    while (i < input.size() && iswspace(input[i])) ++i;
    if (i >= input.size()) return;

    if (input[i] == L'"') {
        ++i;
        const size_t start = i;
        while (i < input.size() && input[i] != L'"') ++i;
        *executable = input.substr(start, i - start);
        if (i < input.size()) ++i;
    } else {
        const size_t start = i;
        while (i < input.size() && !iswspace(input[i])) ++i;
        *executable = input.substr(start, i - start);
    }
    while (i < input.size() && iswspace(input[i])) ++i;
    if (i < input.size()) *arguments = input.substr(i);
}

thread_local IContextMenu2* g_activeShellMenu2 = nullptr;
thread_local IContextMenu3* g_activeShellMenu3 = nullptr;

LRESULT CALLBACK NativeMenuHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM ||
        msg == WM_MEASUREITEM || msg == WM_MENUCHAR) {
        LRESULT menuResult = 0;
        if (g_activeShellMenu3 &&
            g_activeShellMenu3->HandleMenuMsg2(
                msg, wParam, lParam, &menuResult) == S_OK) {
            return 1;
        }
        if (g_activeShellMenu2 &&
            g_activeShellMenu2->HandleMenuMsg(msg, wParam, lParam) == S_OK) {
            return 1;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateNativeMenuHost(HWND owner) {
    static std::once_flag registration;
    static const wchar_t* className = L"ZExplorer.NativeShellMenuHost";
    std::call_once(registration, []() {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = NativeMenuHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);
    });
    return CreateWindowExW(0, className, L"", WS_POPUP,
                           0, 0, 0, 0, owner, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

std::wstring QueryCanonicalVerb(IContextMenu* context, UINT offset) {
    if (!context) return {};
    wchar_t verb[512]{};
    if (FAILED(context->GetCommandString(
            offset, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb),
            ARRAYSIZE(verb)))) {
        return {};
    }
    verb[ARRAYSIZE(verb) - 1] = L'\0';
    return verb;
}

HRESULT InvokeShellCommand(IContextMenu* context, HWND owner, UINT offset,
                           const std::wstring& canonicalVerb) {
    if (!context || !owner) return E_INVALIDARG;

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = owner;
    invoke.nShow = SW_SHOWNORMAL;

    // Canonical verbs are stable across Shell extensions and avoid depending
    // on the command-id window chosen by QueryContextMenu. Some dynamic
    // commands do not expose a verb, so retain the ordinal as a fallback.
    if (!canonicalVerb.empty()) {
        invoke.lpVerbW = canonicalVerb.c_str();
        const HRESULT hr = context->InvokeCommand(
            reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        if (SUCCEEDED(hr)) return hr;
    }

    invoke.lpVerb = MAKEINTRESOURCEA(offset);
    invoke.lpVerbW = MAKEINTRESOURCEW(offset);
    return context->InvokeCommand(
        reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
}

bool RunNativeContextMenu(HWND owner, IContextMenu* context, bool show,
                          const std::wstring& requiredVerb,
                          std::wstring* error) {
    if (!owner || !context) {
        if (error) *error = L"原生菜单对象无效";
        return false;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        if (error) *error = L"无法创建原生菜单";
        return false;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x6FFF;
    const HRESULT query = context->QueryContextMenu(
        menu, 0, kFirstCommand, kLastCommand,
        CMF_NORMAL | CMF_EXTENDEDVERBS);
    if (FAILED(query)) {
        DestroyMenu(menu);
        if (error) *error = L"Shell 无法生成回收站菜单";
        return false;
    }

    IContextMenu2* context2 = nullptr;
    IContextMenu3* context3 = nullptr;
    context->QueryInterface(IID_IContextMenu3,
                            reinterpret_cast<void**>(&context3));
    if (context3) {
        context3->QueryInterface(IID_IContextMenu2,
                                 reinterpret_cast<void**>(&context2));
    } else {
        context->QueryInterface(IID_IContextMenu2,
                                reinterpret_cast<void**>(&context2));
    }

    if (!show) {
        UINT matchedOffset = UINT_MAX;
        const UINT commandCount =
            static_cast<UINT>(GetMenuItemCount(menu));
        for (UINT position = 0; position < commandCount; ++position) {
            const UINT commandId = GetMenuItemID(menu, position);
            if (commandId < kFirstCommand || commandId > kLastCommand)
                continue;
            const UINT offset = commandId - kFirstCommand;
            if (_wcsicmp(QueryCanonicalVerb(context, offset).c_str(),
                         requiredVerb.c_str()) == 0) {
                matchedOffset = offset;
                break;
            }
        }
        bool ok = false;
        if (matchedOffset != UINT_MAX) {
            ok = SUCCEEDED(InvokeShellCommand(
                context, owner, matchedOffset, requiredVerb));
        }
        if (!ok && error) {
            *error = L"回收站项目不支持命令：" + requiredVerb;
        }
        if (context2) context2->Release();
        if (context3) context3->Release();
        DestroyMenu(menu);
        return ok;
    }

    HWND host = CreateNativeMenuHost(owner);
    if (!host) {
        if (context2) context2->Release();
        if (context3) context3->Release();
        DestroyMenu(menu);
        if (error) *error = L"无法创建原生菜单宿主窗口";
        return false;
    }

    POINT point{};
    GetCursorPos(&point);
    g_activeShellMenu2 = context2;
    g_activeShellMenu3 = context3;
    SetForegroundWindow(host);
    const int command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTBUTTON |
            TPM_TOPALIGN | TPM_LEFTALIGN,
        point.x, point.y, host, nullptr);
    SetForegroundWindow(owner);
    PostMessageW(owner, WM_NULL, 0, 0);
    g_activeShellMenu3 = nullptr;
    g_activeShellMenu2 = nullptr;

    bool invoked = true;
    if (command >= static_cast<int>(kFirstCommand)) {
        const UINT offset =
            static_cast<UINT>(command - static_cast<int>(kFirstCommand));
        invoked = SUCCEEDED(InvokeShellCommand(
            context, owner, offset, QueryCanonicalVerb(context, offset)));
        if (!invoked && error) *error = L"执行 Shell 菜单命令失败";
    }

    if (context2) context2->Release();
    if (context3) context3->Release();
    DestroyWindow(host);
    DestroyMenu(menu);
    return invoked;
}

PROPERTYKEY PropertyKeyFromName(const wchar_t* name) {
    PROPERTYKEY key{};
    PSGetPropertyKeyFromName(name, &key);
    return key;
}

std::wstring GetFolderPropertyString(
    IShellFolder2* folder, PCUITEMID_CHILD child,
    REFPROPERTYKEY key) {
    if (!folder) return {};
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(folder->GetDetailsEx(child, &key, &value))) {
        if (value.vt == VT_LPWSTR || value.vt == VT_BSTR) {
            result = value.bstrVal;
        }
    }
    VariantClear(&value);
    return result;
}

FILETIME GetFolderPropertyTime(
    IShellFolder2* folder, PCUITEMID_CHILD child,
    REFPROPERTYKEY key) {
    if (!folder) return {};
    VARIANT value;
    VariantInit(&value);
    FILETIME result{};
    if (SUCCEEDED(folder->GetDetailsEx(child, &key, &value))) {
        if (value.vt == VT_DATE) {
            SYSTEMTIME systemTime{};
            if (VariantTimeToSystemTime(value.date, &systemTime))
                SystemTimeToFileTime(&systemTime, &result);
        } else if (value.vt == VT_UI8 || value.vt == VT_I8) {
            ULARGE_INTEGER raw{};
            raw.QuadPart = value.ullVal;
            result.dwLowDateTime = raw.LowPart;
            result.dwHighDateTime = raw.HighPart;
        }
    }
    VariantClear(&value);
    return result;
}

std::uint64_t GetFolderPropertyUInt64(
    IShellFolder2* folder, PCUITEMID_CHILD child,
    REFPROPERTYKEY key) {
    if (!folder) return 0;
    VARIANT value;
    VariantInit(&value);
    std::uint64_t result = 0;
    if (SUCCEEDED(folder->GetDetailsEx(child, &key, &value))) {
        switch (value.vt) {
        case VT_UI8: result = value.ullVal; break;
        case VT_UI4: result = value.ulVal; break;
        case VT_I8: result = static_cast<std::uint64_t>(value.llVal); break;
        case VT_I4: result = static_cast<std::uint64_t>(value.lVal); break;
        default: break;
        }
    }
    VariantClear(&value);
    return result;
}

bool ResolveRecycleBinFolder(IShellFolder** folder,
                             PIDLIST_ABSOLUTE* bin) {
    if (!folder || !bin) return false;
    *folder = nullptr;
    *bin = nullptr;
    if (FAILED(SHGetFolderLocation(nullptr, CSIDL_BITBUCKET, nullptr, 0, bin)) ||
        !*bin) {
        return false;
    }
    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) {
        CoTaskMemFree(*bin);
        *bin = nullptr;
        return false;
    }
    const HRESULT hr = desktop->BindToObject(
        *bin, nullptr, IID_IShellFolder, reinterpret_cast<void**>(folder));
    desktop->Release();
    if (FAILED(hr) || !*folder) {
        CoTaskMemFree(*bin);
        *bin = nullptr;
        return false;
    }
    return true;
}

IContextMenu* CreateRecycleBinContextMenu(
    HWND owner, const std::vector<const RecycleBinItem*>& items) {
    if (items.empty()) return nullptr;
    IShellFolder* folder = nullptr;
    PIDLIST_ABSOLUTE bin = nullptr;
    if (!ResolveRecycleBinFolder(&folder, &bin)) return nullptr;

    std::vector<PCUITEMID_CHILD> children;
    children.reserve(items.size());
    for (const RecycleBinItem* item : items) {
        if (!item || item->relativePidl.empty()) continue;
        children.push_back(reinterpret_cast<PCUITEMID_CHILD>(
            const_cast<std::uint8_t*>(item->relativePidl.data())));
    }
    IContextMenu* context = nullptr;
    if (!children.empty()) {
        folder->GetUIObjectOf(
            owner, static_cast<UINT>(children.size()), children.data(),
            IID_IContextMenu, nullptr, reinterpret_cast<void**>(&context));
    }
    folder->Release();
    CoTaskMemFree(bin);
    return context;
}

} // namespace

bool OpenPath(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        if (error) *error = L"打开路径为空";
        return false;
    }

    const std::wstring plain = StripExtendedPath(path);
    const std::wstring workingDirectory =
        IsDirectoryPath(plain) ? plain : ParentPath(plain);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    // Let the Shell show its own association/error UI. Suppressing it made
    // unassociated files look like a silent no-op when double-clicked.
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = plain.c_str();
    sei.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        SetShellError(error, L"ShellExecuteEx(open) 失败");
        return false;
    }
    return true;
}

bool OpenContainingFolder(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        if (error) *error = L"打开位置为空";
        return false;
    }
    if (!PathExists(path)) {
        if (error) *error = L"目标不存在：" + path;
        return false;
    }
    if (IsDirectoryPath(path)) return OpenPath(path, error);

    PIDLIST_ABSOLUTE item = ILCreateFromPathW(StripExtendedPath(path).c_str());
    HRESULT firstError = E_FAIL;
    if (item) {
        const HRESULT hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
        if (SUCCEEDED(hr)) return true;
        firstError = hr;
    }

    const std::wstring parameters = L"/select,\"" + StripExtendedPath(path) + L"\"";
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"open", L"explorer.exe", parameters.c_str(),
        nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) return true;
    if (error) {
        *error = L"打开并选中目标失败：SHOpenFolderAndSelectItems=0x";
        wchar_t code[16]{};
        swprintf_s(code, L"%08X", static_cast<unsigned>(firstError));
        *error += code;
        *error += L"，Explorer=" + FormatWin32Error(GetLastError());
    }
    return false;
}

bool OpenWith(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        if (error) *error = L"打开方式路径为空";
        return false;
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"openas";
    const std::wstring plain = StripExtendedPath(path);
    sei.lpFile = plain.c_str();
    const std::wstring directory = ParentPath(plain);
    sei.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        SetShellError(error, L"ShellExecuteEx(openas) 失败");
        return false;
    }
    return true;
}

bool LaunchCommand(const std::wstring& commandLine,
                   const std::wstring& workingDirectory,
                   std::wstring* error) {
    if (commandLine.empty()) {
        if (error) *error = L"命令行不能为空";
        return false;
    }
    std::wstring executable;
    std::wstring arguments;
    SplitCommandLine(commandLine, &executable, &arguments);
    if (executable.empty()) {
        if (error) *error = L"无法从命令行解析可执行文件：" + commandLine;
        return false;
    }

    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = executable.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        SetShellError(error, L"ShellExecuteEx(launch) 失败");
        return false;
    }
    return true;
}

bool ShowShellContextMenu(HWND owner, const std::wstring& path) {
    if (!owner || path.empty()) return false;

    PIDLIST_ABSOLUTE absolute = ILCreateFromPathW(StripExtendedPath(path).c_str());
    if (!absolute) return false;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IContextMenu* context = nullptr;
    HRESULT hr = SHBindToParent(absolute, IID_IShellFolder,
                                reinterpret_cast<void**>(&parent), &child);
    if (SUCCEEDED(hr) && parent) {
        hr = parent->GetUIObjectOf(
            owner, 1, &child, IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(&context));
    }
    if (FAILED(hr) || !context) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                StripExtendedPath(path).c_str(), nullptr,
                IID_IShellItem, reinterpret_cast<void**>(&item))) &&
            item) {
            item->BindToHandler(nullptr, BHID_SFUIObject, IID_IContextMenu,
                                reinterpret_cast<void**>(&context));
            item->Release();
        }
    }
    if (parent) parent->Release();
    ILFree(absolute);
    if (!context) return false;

    const bool invoked = RunNativeContextMenu(owner, context, true, L"", nullptr);
    context->Release();
    return invoked;
}

bool ShowProperties(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        if (error) *error = L"属性目标为空";
        return false;
    }

    std::wstring plain = StripExtendedPath(path);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"properties";
    sei.lpFile = plain.c_str();
    sei.nShow = SW_SHOW;
    if (!ShellExecuteExW(&sei)) {
        SetShellError(error, L"ShellExecuteEx(properties) 失败");
        return false;
    }
    return true;
}

HICON GetShellIconForPath(const std::wstring& path, bool large) {
    SHFILEINFOW sfi{};
    const UINT flags = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags) == 0) return nullptr;
    return sfi.hIcon;
}
HICON LoadAppIcon(int cx, int cy) {
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                               MAKEINTRESOURCEW(101), IMAGE_ICON, cx, cy, 0));
    if (icon) return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}
bool RelaunchElevated() {
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return false;

    // 把原始命令行参数一起带过去（否则 --selftest / 初始路径会丢）
    std::wstring params;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (!params.empty()) params.push_back(L' ');
            params.push_back(L'"');
            params += argv[i];
            params.push_back(L'"');
        }
        LocalFree(argv);
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

namespace {

std::wstring KnownFolderPath(const KNOWNFOLDERID& id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) ||
        !value) {
        return {};
    }
    const std::wstring path = value;
    CoTaskMemFree(value);
    return path;
}

} // namespace

std::vector<DriveInfo> LogicalDrives() {
    std::vector<DriveInfo> out;

    wchar_t buf[512]{};
    const DWORD n = GetLogicalDriveStringsW(ARRAYSIZE(buf) - 1, buf);
    if (n == 0) return out;

    for (const wchar_t* p = buf; *p != L'\0'; p += wcslen(p) + 1) {
        std::wstring root(p);
        const UINT type = GetDriveTypeW(root.c_str());

        // 跳过光驱/未插入介质的驱动器（P0 只列确定可用的）
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_CDROM || type == DRIVE_UNKNOWN)
            continue;

        DriveInfo d;
        d.root = root;
        d.type = type;

        wchar_t label[MAX_PATH]{};
        wchar_t fsName[MAX_PATH]{};
        if (GetVolumeInformationW(root.c_str(), label, MAX_PATH,
                                  nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
            d.label = label;
            d.fsType = fsName;
        }

        // 容量（"此电脑"页面的进度条要用）
        ULARGE_INTEGER freeToCaller{}, total{}, freeTotal{};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeToCaller, &total, &freeTotal)) {
            d.totalBytes = total.QuadPart;
            d.freeBytes = freeTotal.QuadPart;
            d.ready = true;
        }

        // 去掉尾部反斜杠，取 "C:" 形式
        std::wstring letter = root;
        while (!letter.empty() && (letter.back() == L'\\' || letter.back() == L'/'))
            letter.pop_back();

        std::wstring kind;
        switch (type) {
        case DRIVE_FIXED:      kind = L"本地磁盘"; break;
        case DRIVE_REMOVABLE:  kind = L"可移动磁盘"; break;
        case DRIVE_REMOTE:     kind = L"网络驱动器"; break;
        case DRIVE_RAMDISK:    kind = L"内存盘"; break;
        default:               kind = L"驱动器"; break;
        }

        std::wstring display = kind + L" (" + letter + L")";
        if (!d.label.empty()) display = d.label + L" (" + letter + L")";
        d.display = display;

        out.push_back(std::move(d));
    }
    return out;
}

std::vector<UserFolderInfo> UserFolders() {
    const struct {
        const KNOWNFOLDERID* id;
        const wchar_t* title;
    } entries[] = {
        { &FOLDERID_Desktop, L"桌面" },
        { &FOLDERID_Downloads, L"下载" },
        { &FOLDERID_Documents, L"文档" },
        { &FOLDERID_Pictures, L"图片" },
        { &FOLDERID_Music, L"音乐" },
        { &FOLDERID_Videos, L"视频" },
        { &FOLDERID_Libraries, L"库" },
    };

    std::vector<UserFolderInfo> out;
    for (const auto& entry : entries) {
        const std::wstring path = KnownFolderPath(*entry.id);
        if (!path.empty() && PathExists(path))
            out.push_back({ entry.title, path });
    }
    return out;
}

RecycleBinInfo QueryRecycleBin() {
    RecycleBinInfo info;
    SHQUERYRBINFO raw{};
    raw.cbSize = sizeof(raw);
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &raw))) {
        info.available = true;
        info.itemCount = raw.i64NumItems;
        info.totalBytes = raw.i64Size;
    }
    return info;
}

std::vector<RecycleBinItem> QueryRecycleBinItems() {
    std::vector<RecycleBinItem> out;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellFolder* folder = nullptr;
    PIDLIST_ABSOLUTE bin = nullptr;
    if (!ResolveRecycleBinFolder(&folder, &bin)) {
        if (com == S_OK) CoUninitialize();
        return out;
    }

    IShellFolder2* folder2 = nullptr;
    folder->QueryInterface(IID_IShellFolder2,
                           reinterpret_cast<void**>(&folder2));
    IEnumIDList* enumItems = nullptr;
    folder->EnumObjects(nullptr,
                        SHCONTF_FOLDERS | SHCONTF_NONFOLDERS |
                            SHCONTF_INCLUDEHIDDEN,
                        &enumItems);

    const PROPERTYKEY deletedFrom =
        PropertyKeyFromName(L"System.Recycle.DeletedFrom");
    const PROPERTYKEY deletedAt =
        PropertyKeyFromName(L"System.Recycle.DateDeleted");
    const PROPERTYKEY typeName =
        PropertyKeyFromName(L"System.ItemTypeText");
    const PROPERTYKEY sizeKey =
        PropertyKeyFromName(L"System.Size");

    if (enumItems) {
        LPITEMIDLIST child = nullptr;
        while (enumItems->Next(1, &child, nullptr) == S_OK && child) {
            RecycleBinItem item;
            STRRET value{};
            if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &value))) {
                wchar_t name[32768]{};
                if (SUCCEEDED(StrRetToBufW(&value, child, name,
                                            ARRAYSIZE(name))) &&
                    name[0] != L'\0') {
                    item.name = FileNameOf(name);
                }
            }
            if (item.name.empty()) item.name = L"(未命名项目)";

            const UINT size = ILGetSize(child);
            if (size > 0) {
                item.relativePidl.resize(size);
                std::memcpy(item.relativePidl.data(), child, size);
            }
            item.originalPath =
                GetFolderPropertyString(folder2, child, deletedFrom);
            item.typeName =
                GetFolderPropertyString(folder2, child, typeName);
            item.deletedAt =
                GetFolderPropertyTime(folder2, child, deletedAt);
            item.size = GetFolderPropertyUInt64(folder2, child, sizeKey);

            PCUITEMID_CHILD childConst = child;
            SFGAOF attributes = SFGAO_FOLDER;
            if (SUCCEEDED(folder->GetAttributesOf(1, &childConst, &attributes)))
                item.isDir = (attributes & SFGAO_FOLDER) != 0;

            PIDLIST_ABSOLUTE absolute = ILCombine(bin, child);
            if (absolute) {
                wchar_t wrapper[32768]{};
                if (SHGetPathFromIDListW(absolute, wrapper))
                    item.wrapperPath = wrapper;
                ILFree(absolute);
            }

            if (item.typeName.empty())
                item.typeName = item.isDir ? L"文件夹" : L"文件";
            out.push_back(std::move(item));
            CoTaskMemFree(child);
            child = nullptr;
        }
        enumItems->Release();
    }
    if (folder2) folder2->Release();
    folder->Release();
    CoTaskMemFree(bin);
    if (com == S_OK) CoUninitialize();

    std::sort(out.begin(), out.end(), [](const RecycleBinItem& a,
                                         const RecycleBinItem& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool ShowRecycleBinContextMenu(
    HWND owner, const std::vector<const RecycleBinItem*>& items) {
    IContextMenu* context = CreateRecycleBinContextMenu(owner, items);
    if (!context) {
        if (!items.empty()) MessageBoxW(owner,
            L"无法生成回收站项目菜单，请刷新后重试。",
            L"ZExplorer", MB_OK | MB_ICONWARNING);
        return false;
    }
    const bool result =
        RunNativeContextMenu(owner, context, true, L"", nullptr);
    context->Release();
    return result;
}

bool InvokeRecycleBinVerb(
    HWND owner, const std::vector<const RecycleBinItem*>& items,
    const std::wstring& verb, std::wstring* error) {
    IContextMenu* context = CreateRecycleBinContextMenu(owner, items);
    if (!context) {
        if (error) *error = L"无法创建回收站项目菜单";
        return false;
    }
    const bool result =
        RunNativeContextMenu(owner, context, false, verb, error);
    context->Release();
    return result;
}

bool EmptyRecycleBin(std::wstring* error) {
    const HRESULT hr = SHEmptyRecycleBinW(
        nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI);
    if (SUCCEEDED(hr)) return true;
    if (error) {
        wchar_t code[16]{};
        swprintf_s(code, L"%08X", static_cast<unsigned>(hr));
        *error = L"清空回收站失败 (0x" + std::wstring(code) + L")";
    }
    return false;
}

bool OpenRecycleBin() {
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace ze::platform
