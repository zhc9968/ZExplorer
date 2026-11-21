#pragma once
// ============================================================================
// fs/ShellIcons.h —— 文件类型图标（纯 Win32，产出 HICON）
//
// 分层约定：本层**只产出 HICON**，由 ui/ 负责转成 ZufyUI::Image。
// 这样 fs/ 保持零 ZufyUI 依赖，图标缓存也能在无窗口测试里验证。
//
// 所有权：返回的 HICON 归本缓存所有，调用方**不得 DestroyIcon**。
// 进程退出前调 Clear()（或由静态析构）统一释放。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ze::fs {

enum class IconSize { Small = 16, Large = 32, ExtraLarge = 48 };

class ShellIcons {
public:
    static ShellIcons& Instance();

    // 取图标。
    //   isDir          : 目录用通用文件夹图标
    //   preferFileIcon : true 时尝试取该文件自身的图标（会触碰磁盘，慢）；
    //                    false 时按扩展名取通用图标（快，走 SHGFI_USEFILEATTRIBUTES）
    // 失败返回 nullptr。
    HICON Get(bool isDir, const std::wstring& path, bool preferFileIcon, IconSize size);

    // 清空缓存并释放所有 HICON。窗口销毁 / 设备丢失时调用。
    void Clear();

    size_t Count() const;

private:
    ShellIcons() = default;
    ~ShellIcons();
    ShellIcons(const ShellIcons&) = delete;
    ShellIcons& operator=(const ShellIcons&) = delete;

    static std::wstring MakeKey(bool isDir, const std::wstring& path,
                                bool preferFileIcon, IconSize size);

    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, HICON> cache_;
};

} // namespace ze::fs
