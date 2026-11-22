#include "platform/Shortcuts.h"

#include "platform/PathUtil.h"

#include <shobjidl.h>

namespace ze::platform {

namespace {

std::wstring ShortcutString(HRESULT (IShellLinkW::*getter)(wchar_t*, int),
                            IShellLinkW* link) {
    wchar_t buffer[32768]{};
    return SUCCEEDED((link->*getter)(buffer, 32768))
        ? std::wstring(buffer)
        : std::wstring();
}

} // namespace

bool IsShortcutPath(const std::wstring& path) {
    return _wcsicmp(ExtensionOf(path).c_str(), L".lnk") == 0;
}

bool ResolveShortcut(const std::wstring& path, ShortcutInfo* info,
                     std::wstring* error) {
    if (!info) return false;
    *info = {};
    info->shortcutPath = NormalizePath(path);
    if (!IsShortcutPath(path)) {
        if (error) *error = L"不是 .lnk 快捷方式";
        return false;
    }
    info->isShortcut = true;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize =
        SUCCEEDED(hrCom) && hrCom != RPC_E_CHANGED_MODE;

    IShellLinkW* link = nullptr;
    IPersistFile* persist = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr) && link) {
        hr = link->QueryInterface(IID_IPersistFile,
                                  reinterpret_cast<void**>(&persist));
    }
    if (SUCCEEDED(hr) && persist) {
        hr = persist->Load(path.c_str(), STGM_READ);
    }
    if (SUCCEEDED(hr) && link) {
        link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOTRACK |
                                  SLR_NOUPDATE);

        wchar_t target[32768]{};
        WIN32_FIND_DATAW data{};
        if (SUCCEEDED(link->GetPath(target, 32768, &data, 0)) && target[0]) {
            info->targetPath =
                NormalizePath(ExpandEnvironmentVariables(target));
        }
        info->arguments = ShortcutString(&IShellLinkW::GetArguments, link);
        info->workingDirectory =
            ShortcutString(&IShellLinkW::GetWorkingDirectory, link);
        info->description = ShortcutString(&IShellLinkW::GetDescription, link);
    }

    if (persist) persist->Release();
    if (link) link->Release();
    if (uninitialize) CoUninitialize();

    if (FAILED(hr)) {
        if (error) {
            wchar_t code[32]{};
            swprintf_s(code, L"读取快捷方式失败：HRESULT 0x%08X",
                       static_cast<unsigned>(hr));
            *error = code;
        }
        return false;
    }

    if (info->targetPath.empty()) {
        info->kind = ShortcutTargetKind::ShellItem;
    } else if (!PathExists(info->targetPath)) {
        info->kind = ShortcutTargetKind::MissingPath;
    } else if (IsDirectoryPath(info->targetPath)) {
        info->kind = ShortcutTargetKind::Directory;
    } else {
        info->kind = ShortcutTargetKind::File;
    }
    return true;
}

} // namespace ze::platform
