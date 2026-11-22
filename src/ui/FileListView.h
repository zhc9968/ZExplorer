#pragma once
// ============================================================================
// ui/FileListView.h —— 文件列表的视图接口
//
// 为什么要接口：虚拟化视图（ZufyUI VirtualList / 池化元素）尚未就绪，
// 当前用「打开目录时全量灌入 + 上限」的回退实现（EagerFileListView）。
// 将来接入虚拟化时**只新增一个实现**，ExplorerPane 与 DirectoryModel 都不动。
// 见 AGENTS.md §3。
// ============================================================================

#include <functional>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "fs/DirectoryModel.h"

namespace ZufyUI { class UIElement; }

namespace ze::ui {

class FileListView {
public:
    virtual ~FileListView() = default;

    // 交给上层挂进布局的元素。
    virtual std::shared_ptr<ZufyUI::UIElement> Widget() = 0;

    // 用一份快照填充列表。
    virtual void SetEntries(const std::vector<fs::FileEntry>& entries) = 0;

    // 空目录提示。
    virtual void SetEmptyText(const std::wstring& text) = 0;

    // 当前选中行（-1 = 无）。
    virtual int SelectedIndex() const = 0;

    // 多选：框选 + 勾选的并集（升序、去重）。
    virtual std::vector<int> SelectedRows() const = 0;
    virtual void SelectAll() = 0;
    virtual void ClearSelection() = 0;
    virtual void InvertSelection() = 0;
    virtual bool SelectPath(const std::wstring& path) = 0;

    // 实际灌入的行数（用于自检与状态栏）。
    virtual int RowCount() const = 0;

    // 成功创建的行图标数量（自检用：为 0 说明图标链路断了）。
    virtual int IconCount() const = 0;

    // 自检用：第一个图标的实际像素尺寸（0x0 说明解码失败）。
    virtual std::wstring IconDiagnostics() const = 0;

    // onOpen  : 双击 / 回车（传行号）
    // onSelect: 选中变化（传行号，-1 表示取消选中）
    virtual void SetCallbacks(std::function<void(int)> onOpen,
                              std::function<void(int)> onSelect) = 0;

    // 表头点击请求排序（传列号）；实际排序由页面层交给 DirectoryModel。
    virtual void SetSortChanged(std::function<void(int)> onSortChanged) = 0;
    virtual void SetSortIndicator(int column, bool ascending) = 0;

    // 视图显示选项与拖出源。
    virtual void SetHideKnownExtensions(bool hide) = 0;
    virtual void SetDragPathsProvider(
        std::function<std::vector<std::wstring>()> provider) = 0;
    virtual void SetColumnWidths(const std::array<float, 4>& widths) = 0;
    virtual std::array<float, 4> GetColumnWidths() const = 0;
};

// 回退实现：全量灌入，超过上限的部分只在末尾提示。
constexpr size_t kEagerRowLimit = 5000;

std::shared_ptr<FileListView> MakeEagerFileListView();

} // namespace ze::ui
