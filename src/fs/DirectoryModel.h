#pragma once
// ============================================================================
// fs/DirectoryModel.h —— 目录取数的唯一入口
//
// 铁律：UI 层不得自行遍历文件系统；所有列表数据都从这里出。
// 目前是「回退写法」：一次枚举出全量 Snapshot（同步）。
// 将来接入 ZufyUI 的 VirtualList 时，只新增一个视图适配器，
// 本文件的分层与接口保持不变（见 AGENTS.md §3）。
//
// 本文件只依赖 Win32 + STL —— 不 include ZufyUI。
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ze::fs {

struct FileEntry {
    std::wstring name;      // 文件名（不含路径）
    std::wstring fullPath;  // 绝对路径（常规形式，无 \\?\ 前缀）
    bool isDir = false;
    bool isHidden = false;
    bool isSystem = false;
    bool isReparse = false; // 符号链接 / junction / 云占位
    uint64_t size = 0;      // 目录恒为 0
    FILETIME modified{};
    std::wstring typeName;  // Shell 类型名（"文本文档"、"文件夹"）
};

// 一次目录读取的结果。失败信息与数据放在一起，UI 层不需要再问系统。
struct Snapshot {
    std::wstring path;               // 规范化后的绝对路径
    std::vector<FileEntry> entries;  // 已排序（默认名称升序、目录在前）
    bool accessDenied = false;       // ERROR_ACCESS_DENIED / ERROR_PRIVILEGE_NOT_HELD
    bool notFound = false;           // ERROR_FILE_NOT_FOUND / PATH_NOT_FOUND
    DWORD errorCode = 0;             // 其余错误
    uint32_t hiddenSkipped = 0;      // 因"不显示隐藏项"而跳过的数量
    double elapsedMs = 0.0;

    bool Ok() const { return errorCode == 0; }
    size_t DirCount() const;
    size_t FileCount() const;
    uint64_t TotalFileBytes() const;
};

enum class SortKey { Name, Modified, Type, Size };
enum class SortOrder { Ascending, Descending };

class DirectoryModel {
public:
    // 读取目录。path 可为常规或 \\?\ 形式；返回的 Snapshot::path 已规范化。
    static Snapshot Load(const std::wstring& path, bool showHidden);

    // 递归搜索当前目录及子目录。回调必须可在工作线程调用。
    static std::vector<FileEntry> Search(
        const std::wstring& path, const std::wstring& query, bool showHidden,
        const std::function<bool()>& canceled = {},
        const std::function<void(size_t)>& progress = {},
        size_t maxResults = 10000);

    // 就地排序：目录恒在前，其余按 key/order。
    static void Sort(std::vector<FileEntry>& entries, SortKey key, SortOrder order);
    static void Sort(Snapshot& snapshot, SortKey key, SortOrder order);

    // 显示名（面包屑用）：根目录返回 "C:\"、UNC 返回 "\\server\share"。
    static std::wstring DisplayNameOf(const std::wstring& path);
};

} // namespace ze::fs
