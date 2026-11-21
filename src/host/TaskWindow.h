#pragma once
// ============================================================================
// host/TaskWindow.h —— 任务窗（只有这一个窗口）
//
// 职责边界：
//   HostApp 负责"执行任务"，把任务快照整理成 TaskItem 交给这里；
//   本模块只负责"把快照画出来"，不碰文件系统、不碰管道。
//
// 设计要点（都是为了不再出现之前的崩溃/卡死）：
//   1. 卡片**增量更新**：新增插入、消失用 RemoveChild 摘掉，永不整体重建。
//   2. 鼠标按下期间不做任何结构改动（推迟到下一拍），避免销毁"正在派发的元素"。
//   3. 页签关闭 = 取消任务，且删除动作**投递到 UI 队列**，不在事件回调里删自己。
//   4. 首页固定不可关闭；没有任务时停在首页，窗口不消失。
// ============================================================================

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ZufyUI {
class Window;
class TabView;
class ScrollViewer;
class ColumnBox;
class Card;
class Label;
class Button;
class ProgressBar;
} // namespace ZufyUI

namespace ze::host {

struct UiIssue {
    std::wstring path;      // 源文件
    std::wstring target;    // 目标文件（冲突时才有）
    std::wstring message;
    int kind = 0;           // 0 普通错误；1 目标已存在
};

struct TaskItem {
    int id = 0;
    DWORD originPid = 0;          // 发起任务的 ZExplorer UI 进程
    std::wstring originPath;      // 发起进程镜像路径
    std::wstring op;
    std::wstring title;
    std::wstring targetDir;
    int state = 0;          // 0 排队 1 运行 2 完成 3 失败 4 取消
    float ratio = 0.0f;
    std::uint32_t doneFiles = 0;
    std::uint32_t totalFiles = 0;
    int elapsedMs = 0;                    // 已运行时长（0.35 秒规则用）
    double speedBytes = 0.0;              // 字节/秒，0 表示不显示
    std::wstring currentFile;
    std::vector<std::wstring> sources;    // 首页卡片显示"从哪到哪"
    bool dismissed = false;
    std::vector<UiIssue> issues;
};

class TaskWindow {
public:
    struct Hooks {
        std::function<void(int)> onCancel;
        std::function<void(int, const std::wstring& src, const std::wstring& dst)> onOverwrite;
        std::function<void(int, const std::wstring& src)> onSkip;
        std::function<void(int)> onAllOverwrite;
        std::function<void(int)> onAllSkip;
        std::function<void(int)> onAllRetry;
        std::function<void(int)> onAllIgnore;
        std::function<void(const std::wstring& path)> onOpenPath;
    };

    void SetHooks(Hooks hooks);
    void SetTiming(std::uint64_t showDelayMs, std::uint64_t doneLingerMs,
                   bool beepOnError);
    void Update(const std::vector<TaskItem>& tasks);
    void ShowAndFocus();
    void ShowTask(int taskId);          // 切到指定任务的页签并前置窗口
    int TabCount() const { return static_cast<int>(tabStates_.size()); }
    std::wstring TabDiagnostics() const;
    bool HasWindow() const;

private:
    struct TabState {
        int taskId = 0;
        std::shared_ptr<ZufyUI::Label> current;
        std::shared_ptr<ZufyUI::Label> stats;
        std::shared_ptr<ZufyUI::Label> percent;
        std::shared_ptr<ZufyUI::ProgressBar> bar;
        std::shared_ptr<ZufyUI::Button> cancel;
        std::shared_ptr<ZufyUI::ScrollViewer> scroll;
        std::shared_ptr<ZufyUI::ColumnBox> list;
        std::map<std::wstring, std::shared_ptr<ZufyUI::Card>> cards;  // 按源路径索引
        std::uint64_t doneAt = 0;
        size_t lastIssueCount = 0;      // 用来判断"出现了新错误" -> 闪任务栏
        int lastState = -1;             // 状态变化（-> 出错/等决策）时弹窗
    };

    void EnsureWindow();
    void EnsureTab(const TaskItem& item);
    void SyncIssues(TabState& tab, const TaskItem& item);
    void RebuildHome(const std::vector<TaskItem>& tasks);
    void UpdateHomeProgress(const std::vector<TaskItem>& tasks);
    void RemoveTab(int taskId, bool deferred);

    std::shared_ptr<ZufyUI::Window> win_;
    std::shared_ptr<ZufyUI::TabView> tabs_;
    std::shared_ptr<ZufyUI::ScrollViewer> homeScroll_;
    std::shared_ptr<ZufyUI::ColumnBox> homeList_;
    std::map<int, TabState> tabStates_;
    std::map<int, std::shared_ptr<ZufyUI::ProgressBar>> homeBars_;   // 首页进度条就地更新
    std::map<int, std::shared_ptr<ZufyUI::Label>> homeStats_;        // 首页统计行就地更新
    std::vector<int> pendingRemove_;     // 延后删除的页签（避免在事件回调里删自己）
    size_t homeHash_ = 0;
    std::uint64_t showDelayMs_ = 350;
    std::uint64_t doneLingerMs_ = 1200;
    bool beepOnError_ = false;
    Hooks hooks_;
    bool closing_ = false;
};

} // namespace ze::host
