#pragma once
// ============================================================================
// ui/ExplorerPane.h —— 一个标签页的内容：地址栏 / 导航树 / 文件列表 / 提示条
//
// 分层：本层只调 fs/ 与 platform/ 的接口，不直接碰 Win32 句柄（AGENTS.md §2）。
// 所有耗时动作（枚举目录）走 RunAsync，回调在 UI 线程执行。
// ============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fs/DirectoryModel.h"
#include "platform/DirectoryWatcher.h"
#include "platform/ShellActions.h"
#include "ui/NotificationDrawer.h"

namespace ZufyUI {
class UIElement;
class Window;
class GridLayout;
class ColumnBox;
class RowBox;
class Card;
class SplitView;
class Label;
class Button;
class TextBox;
class TreeView;
class ListView;
class TableView;
class Timer;
class Menu;
class Image;
struct TreeNode;
class MessageBox;
struct DragEventArgs;
} // namespace ZufyUI

namespace ze::ui {

class FileListView;

class ExplorerPane : public std::enable_shared_from_this<ExplorerPane> {
public:
    static std::shared_ptr<ExplorerPane> Create();

    ~ExplorerPane();

    std::shared_ptr<ZufyUI::UIElement> Root() const;

    // 导航。addHistory=false 用于"后退/前进"内部跳转。
    void NavigateTo(const std::wstring& path, bool addHistory = true);
    void GoBack();
    void GoForward();
    void GoUp();
    void Refresh();

    const std::wstring& CurrentPath() const { return currentPath_; }
    // 从外部 Shell 入口打开文件位置：文件进入父目录并选中，目录直接导航。
    void OpenPathLocation(const std::wstring& path) {
        OpenLocationInZExplorer(path);
    }

    // 自检用：程序化进入/退出地址栏编辑态。
    void FocusAddress();
    void BlurAddress();
    void ApplyPersistentSettings();
    bool SavePersistentSettings(std::wstring* error = nullptr);
    void ReportWarning(const std::wstring& title, const std::wstring& text);

    // 由 ShellWindow 在 SetRootLayout 之后调用：注册快捷键等需要窗口的操作。
    void AttachToWindow(ZufyUI::Window* window);

    // 页面级回调（由 ShellWindow 挂接，用于标题与状态栏）。
    std::function<void(const std::wstring&)> OnNavigated;
    std::function<void(const std::wstring&)> OnStatus;
    // 一次目录读取完成后的统计（总数 / 目录数 / 文件数 / 耗时 ms）。
    std::function<void(size_t, size_t, size_t, double)> OnLoaded;
    // 当前选中项数与文件大小合计（目录不计大小）。
    std::function<void(size_t, uint64_t)> OnSelectionChanged;

    // 自检用。
    size_t EntryCount() const { return entries_.size(); }
    int ListRowCount() const;
    int ListIconCount() const;
    std::wstring IconDiagnostics() const;
    std::wstring TreeDiagnostics() const;
    std::wstring AddressDiagnostics() const;
    size_t TreeRootCount() const { return treeRootCount_; }
    size_t TreeNodeCount() const;
    std::wstring DumpLayout() const;

private:
    ExplorerPane();

    void Build();
    void BuildDriveTree();

    // 树：懒加载（展开时回调）
    void LoadTreeNode(const std::shared_ptr<ZufyUI::TreeNode>& node);
    void RefreshTreeVisibility();

    // 地址栏：未聚焦时显示面包屑，聚焦时显示可编辑路径并全选
    void RebuildBreadcrumb(const std::wstring& path);
    void ScheduleBreadcrumbRebuild();
    void EnterAddressEditMode();
    void LeaveAddressEditMode();
    void FocusAddressEditor();
    void BlurAddressEditor();
    void CommitAddressInput();

    // 左树跟随导航：展开到该深度 + 选中 + 滚动到可见
    void SyncTreeToPath(const std::wstring& path);
    void ScrollTreeToNode(const std::shared_ptr<ZufyUI::TreeNode>& node);

    void BeginLoad(const std::wstring& path);
    void ApplySnapshot(const fs::Snapshot& snapshot);
    void SortByColumn(int column);
    void ApplyViewOptions();
    void UpdateViewOption(bool showHidden, bool hideExtensions);
    void StartRecursiveSearch();
    void ClearSearch(bool restoreEntries = true);
    void StartDirectoryWatcher(const std::wstring& path);
    void StopDirectoryWatcher();
    void HandleExternalDrag(ZufyUI::DragEventArgs& e, bool drop);

    void OpenIndex(int index);
    void SelectIndex(int index);
    bool OpenFilePath(const std::wstring& path);
    void OpenLocationInZExplorer(const std::wstring& path);
    void ApplyPendingPathSelection();
    void UpdateSelectionSummary();

    // ---- 文件操作 ----
    const fs::FileEntry* SelectedEntry() const;
    std::vector<std::wstring> SelectedPaths() const;   // 多选（框选+勾选并集）
    void DoOpenSelected();
    void DoOpenLocation();
    void DoOpenWith();
    void DoCopy(bool cut);
    void DoCopyPath();
    void DoPaste();
    void DoDelete(bool permanent);
    void DoRename();
    void DoNewFolder();
    void DoNewTextFile();
    void DoSelectAll();
    void DoSelectNone();
    void DoInvertSelection();
    void DoProperties();
    void DoEditAttributes();
    void DoManageStreams();
    void DoUnlockFile();
    void DoEditAttributesForPath(const std::wstring& path);
    void DoManageStreamsForPath(const std::wstring& path);
    void DoUnlockFileForPath(const std::wstring& path);
    void RefreshQuickAccess();
    void ToggleQuickAccessPin(const std::wstring& path);
    void OpenQuickAccessItem(int index, bool openTarget);
    void LaunchNewWindowHere();
    void ShowNativeContextMenu(const std::wstring& path);
    std::shared_ptr<ZufyUI::Menu> BuildTargetContextMenu(const std::wstring& path);
    // 通用"输入名字"对话框（新建/重命名共用）
    bool PromptName(const std::wstring& title, const std::wstring& initial, std::wstring* out,
                    ZufyUI::Window* owner = nullptr);
    void ShowMessage(const std::wstring& title, const std::wstring& text,
                     ZufyUI::MessageBox::Icon icon);
    void Notify(const std::wstring& title, const std::wstring& text,
                NoticeKind kind = NoticeKind::Warning);
    void ScheduleRefresh();   // 任务在后台执行，稍后刷新目录
    void UpdateTaskStrip();   // 轮询后台任务，更新总进度条与"任务"按钮
    void ShowTaskMenu();      // 点"任务"弹出的下拉
    void ShowHint(const std::wstring& text);   // 空串 = 隐藏
    void UpdateNavButtons();                   // 按历史栈启用/禁用前进后退
    std::shared_ptr<ZufyUI::Image> FolderIcon();
    // 树节点图标：按路径取真实图标（桌面/下载/文档有专属图标），带缓存与上限。
    std::shared_ptr<ZufyUI::Image> TreeNodeIcon(const std::wstring& path);

    std::shared_ptr<ZufyUI::ColumnBox> root_;
    std::shared_ptr<NotificationDrawer> notificationDrawer_;
    std::shared_ptr<ZufyUI::SplitView> split_;
    std::shared_ptr<ZufyUI::SplitView> leftStack_;
    std::shared_ptr<ZufyUI::ListView> quickList_;
    std::shared_ptr<ZufyUI::ColumnBox> quickPane_;
    std::vector<std::wstring> quickPins_;
    struct QuickAccessEntry {
        std::wstring title;
        std::wstring path;
        bool pinned = false;
        bool special = false;
    };
    std::vector<QuickAccessEntry> quickEntries_;
    // "此电脑"页面（默认首页）：驱动器磁贴
    std::shared_ptr<ZufyUI::ScrollViewer> drivesScroll_;
    std::shared_ptr<ZufyUI::ColumnBox> drivesList_;
    std::shared_ptr<ZufyUI::Timer> drivesLayoutTimer_;
    int drivesColumns_ = 0;
    std::shared_ptr<ZufyUI::ColumnBox> recyclePage_;
    std::shared_ptr<ZufyUI::Label> recycleSummary_;
    std::shared_ptr<ZufyUI::TableView> recycleTable_;
    std::shared_ptr<ZufyUI::Timer> recycleRefreshTimer_;
    std::vector<platform::RecycleBinItem> recycleItems_;
    std::unordered_map<std::wstring, std::shared_ptr<ZufyUI::Image>>
        recycleIconCache_;
    bool showingDrives_ = false;
    bool showingRecycleBin_ = false;
    void ShowDrivesPage();
    void ShowRecycleBinPage();
    void HideDrivesPage();
    std::vector<int> SelectedRecycleRows() const;
    std::vector<const platform::RecycleBinItem*> SelectedRecycleItems() const;
    void InvokeSelectedRecycleVerb(const std::wstring& verb);
    void ShowRecycleBinItemMenu(int row);
    void ScheduleRecycleBinRefresh();
    std::shared_ptr<ZufyUI::ColumnBox> rightCol_;
    std::shared_ptr<ZufyUI::RowBox> toolbar_;
    std::shared_ptr<ZufyUI::RowBox> actionBar_;
    std::shared_ptr<ZufyUI::TextBox> searchBox_;
    std::shared_ptr<ZufyUI::Timer> searchDebounceTimer_;
    bool searchUpdating_ = false;
    std::shared_ptr<ZufyUI::Label> hint_;

    // 地址栏：GridLayout 1x1，TextBox 与面包屑叠在同一格
    std::shared_ptr<ZufyUI::GridLayout> addressHost_;
    std::shared_ptr<ZufyUI::TextBox> addressBox_;
    std::shared_ptr<ZufyUI::Card> crumb_;
    std::shared_ptr<ZufyUI::RowBox> crumbRow_;
    std::shared_ptr<ZufyUI::Timer> crumbRebuildTimer_;

    std::shared_ptr<ZufyUI::Button> backBtn_;
    std::shared_ptr<ZufyUI::Button> forwardBtn_;
    std::shared_ptr<ZufyUI::Button> upBtn_;
    std::shared_ptr<ZufyUI::ProgressBar> taskBar_;
    std::shared_ptr<ZufyUI::Button> taskBtn_;
    std::shared_ptr<ZufyUI::Timer> taskTimer_;
    int lastPending_ = 0;
    std::unordered_map<int, int> reportedTaskIssues_;
    std::shared_ptr<ZufyUI::TreeView> tree_;
    std::shared_ptr<FileListView> list_;

    std::shared_ptr<ZufyUI::TreeNode> firstRoot_;
    std::vector<std::shared_ptr<ZufyUI::TreeNode>> roots_;
    bool suppressTreeSync_ = false;   // 程序化选中时不要再回灌导航
    // 布局跑完之前 ScrollToNode 的 clamp 会算错，所以下一拍再滚一次
    std::shared_ptr<ZufyUI::Timer> syncScrollTimer_;
    std::shared_ptr<ZufyUI::Timer> refreshTimer_;
    std::shared_ptr<ZufyUI::Timer> watchTimer_;
    std::unique_ptr<platform::DirectoryWatcher> directoryWatcher_;
    std::shared_ptr<ZufyUI::TreeNode> pendingScrollNode_;
    std::shared_ptr<ZufyUI::Image> folderIcon_;
    std::unordered_map<std::wstring, std::shared_ptr<ZufyUI::Image>> treeIconCache_;
    size_t treeRootCount_ = 0;

    std::wstring currentPath_;
    std::wstring pendingSelectPath_;
    std::vector<fs::FileEntry> sourceEntries_;
    std::vector<fs::FileEntry> entries_;
    uint64_t searchGeneration_ = 0;
    ZufyUI::Window* window_ = nullptr;
    fs::SortKey sortKey_ = fs::SortKey::Name;
    fs::SortOrder sortOrder_ = fs::SortOrder::Ascending;

    std::vector<std::wstring> history_;
    int historyIndex_ = -1;

    uint64_t loadGen_ = 0;   // 代际号：丢弃过期的枚举结果
};

} // namespace ze::ui
