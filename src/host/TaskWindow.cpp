#include "pch.h"
#include "host/TaskWindow.h"

#include "platform/PathUtil.h"
#include "platform/ShellActions.h"
#include "platform/WindowSecurity.h"

#include <algorithm>
#include <set>

using namespace ZufyUI;
using ze::platform::FileNameOf;

namespace ze::host {

namespace {

constexpr size_t kMaxIssueCards = 100;
constexpr size_t kMaxHomeCards = 40;
constexpr int kHomeTabId = -1;

bool MouseDown() { return (GetKeyState(VK_LBUTTON) & 0x8000) != 0; }

std::wstring OpTitle(const std::wstring& op) {
    if (op == L"copy") return L"复制";
    if (op == L"copyover") return L"覆盖复制";
    if (op == L"cut") return L"剪切";
    if (op == L"paste") return L"粘贴";
    if (op == L"delete") return L"删除";
    if (op == L"deleteperm") return L"永久删除";
    if (op == L"rename") return L"重命名";
    if (op == L"newfolder") return L"新建文件夹";
    if (op == L"newfile") return L"新建文件";
    if (op == L"streamimport") return L"导入数据流";
    if (op == L"streamexport") return L"导出数据流";
    if (op == L"streamdelete") return L"删除数据流";
    if (op == L"streamhide") return L"隐藏到数据流";
    if (op == L"streamwrite") return L"编辑数据流";
    if (op == L"streamrename") return L"重命名数据流";
    if (op == L"killprocess") return L"结束占用进程";
    if (op == L"closehandle") return L"关闭文件句柄";
    if (op == L"streamcreate") return L"创建数据流";
    return op;
}

const wchar_t* StateText(int state) {
    switch (state) {
    case 0: return L"排队";
    case 1: return L"进行中";
    case 2: return L"已完成";
    case 3: return L"部分失败";
    default: return L"已取消";
    }
}

std::wstring FormatDuration(int ms) {
    if (ms < 0) ms = 0;
    const int seconds = ms / 1000;
    if (seconds < 60) return std::to_wstring(seconds) + L" 秒";
    const int minutes = seconds / 60;
    const int restSeconds = seconds % 60;
    if (minutes < 60) {
        wchar_t buf[32]{};
        swprintf_s(buf, L"%d 分 %02d 秒", minutes, restSeconds);
        return buf;
    }
    wchar_t buf[48]{};
    swprintf_s(buf, L"%d 时 %02d 分", minutes / 60, minutes % 60);
    return buf;
}

std::wstring FormatSpeed(double bytesPerSecond) {
    if (bytesPerSecond <= 0.0) return {};
    wchar_t buf[32]{};
    if (bytesPerSecond >= 1024.0 * 1024.0 * 1024.0)
        swprintf_s(buf, L"%.2f GB/s", bytesPerSecond / (1024.0 * 1024.0 * 1024.0));
    else if (bytesPerSecond >= 1024.0 * 1024.0)
        swprintf_s(buf, L"%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));
    else
        swprintf_s(buf, L"%.0f KB/s", bytesPerSecond / 1024.0);
    return buf;
}

std::wstring TaskStatsText(const TaskItem& item) {
    const float ratio = item.state == 2 ? 1.0f : item.ratio;
    wchar_t head[96]{};
    swprintf_s(head, L"%d%% · %u/%u 个文件",
               static_cast<int>(ratio * 100.0f), item.doneFiles, item.totalFiles);
    std::wstring text = head;

    if ((item.state == 0 || item.state == 1) && item.speedBytes > 0.0) {
        const std::wstring speed = FormatSpeed(item.speedBytes);
        if (!speed.empty()) text += L" · " + speed;
    }

    if (item.state == 0) {
        text += L" · 等待执行";
    } else {
        text += L" · 已用 " + FormatDuration(item.elapsedMs);
        if (item.state == 1 && ratio >= 0.02f && item.elapsedMs > 0) {
            const int remaining = static_cast<int>(
                static_cast<double>(item.elapsedMs) * (1.0 - ratio) / ratio);
            text += L" · 剩余约 " + FormatDuration(remaining);
        } else if (item.state == 2) {
            text += L" · 已完成";
        } else if (item.state == 3) {
            text += L" · 部分失败";
        } else if (item.state == 4) {
            text += L" · 已取消";
        } else if (item.state == 5) {
            text += L" · 等待处理";
        }
    }
    return text;
}

} // namespace

void TaskWindow::SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

void TaskWindow::SetTiming(std::uint64_t showDelayMs,
                           std::uint64_t doneLingerMs,
                           bool beepOnError) {
    showDelayMs_ = showDelayMs;
    doneLingerMs_ = doneLingerMs;
    beepOnError_ = beepOnError;
}

bool TaskWindow::HasWindow() const { return win_ && win_->IsValid(); }

void TaskWindow::EnsureWindow() {
    if (win_ && win_->IsValid()) return;

    win_ = std::make_shared<Window>();
    if (!win_->Create(760, 520, L"ZExplorer 任务")) return;
    ze::platform::AllowLowerIntegrityMessages(win_->GetHwnd());
    win_->SetMinSize(560, 360);
    win_->SetIcon(ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CXSMICON)), ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CXSMICON)));

    auto root = win_->GetRootColumnBox();
    root->SetMargin(Thickness(12, 10, 12, 10));

    tabs_ = std::make_shared<TabView>();
    tabs_->SetFillWidth(true);
    tabs_->SetFillHeight(true);
    root->AddChild(tabs_);

    homeList_ = std::make_shared<ColumnBox>();
    homeList_->SetSpacing(12.0f);
    homeList_->SetFillWidth(true);

    homeScroll_ = std::make_shared<ScrollViewer>();
    homeScroll_->SetContent(homeList_);
    homeScroll_->SetFillWidth(true);
    homeScroll_->SetFillHeight(true);
    tabs_->AddTab(L"操作", homeScroll_, /*closable*/ false, kHomeTabId);   // 首页不可关闭

    // 点页签的 × -> 只登记，删除推迟到下一拍（绝不在事件回调里删自己）
    tabs_->TabCloseRequested.connect([this](int index) {
        if (!tabs_) return;
        const int taskId = tabs_->GetTabId(index);
        if (taskId == kHomeTabId) return;
        if (hooks_.onCancel) hooks_.onCancel(taskId);
        RemoveTab(taskId, /*deferred*/ true);
    });

    win_->Connect(win_->Closing, [w = win_](bool* cancel) {
        if (cancel) *cancel = true;
        w->Hide();                                // 关窗口 = 隐藏，任务继续
    });

    win_->Show();
}

void TaskWindow::ShowAndFocus() {
    EnsureWindow();
    if (!win_ || !win_->IsValid()) return;
    win_->Show();
    win_->ShowActivate(Window::ActivateMode::All);
}

void TaskWindow::ShowTask(int taskId) {
    EnsureWindow();
    if (!win_ || !win_->IsValid() || !tabs_) return;
    const int idx = tabs_->FindTabById(taskId);
    if (idx >= 0) tabs_->SetSelectedIndex(idx);
    win_->Show();
    win_->ShowActivate(Window::ActivateMode::All);
}

std::wstring TaskWindow::TabDiagnostics() const {
    std::wstring ids;
    std::set<int> seen;
    int duplicates = 0;
    if (tabs_) {
        for (int i = 0; i < tabs_->GetTabCount(); ++i) {
            const int id = tabs_->GetTabId(i);
            if (!ids.empty()) ids += L",";
            ids += std::to_wstring(id);
            if (!seen.insert(id).second) ++duplicates;
        }
    }
    const int selectedIndex = tabs_ ? tabs_->GetSelectedIndex() : -1;
    const int selectedId = (tabs_ && selectedIndex >= 0) ? tabs_->GetTabId(selectedIndex) : -999;
    wchar_t buf[256];
    swprintf_s(buf, L"tabs=%d selected=%d home=%d ids=[%ls] pending=%zu duplicate=%d",
               tabs_ ? tabs_->GetTabCount() : 0, selectedId, kHomeTabId,
               ids.c_str(), pendingRemove_.size(), duplicates);
    return buf;
}

void TaskWindow::RemoveTab(int taskId, bool deferred) {
    if (deferred) {
        if (std::find(pendingRemove_.begin(), pendingRemove_.end(), taskId) == pendingRemove_.end())
            pendingRemove_.push_back(taskId);
        return;
    }
    pendingRemove_.erase(std::remove(pendingRemove_.begin(), pendingRemove_.end(), taskId),
                         pendingRemove_.end());
    if (tabs_) tabs_->RemoveTabById(taskId);
    tabStates_.erase(taskId);
    homeHash_ = 0;
}

void TaskWindow::EnsureTab(const TaskItem& item) {
    EnsureWindow();
    if (!tabs_) return;
    if (tabStates_.count(item.id)) return;

    TabState tab;
    tab.taskId = item.id;

    auto page = std::make_shared<ColumnBox>();
    page->SetMargin(Thickness(14, 12, 14, 12));
    page->SetSpacing(10.0f);

    tab.current = std::make_shared<Label>(L"准备中…");
    tab.current->SetTextOverflow(Label::TextOverflow::Ellipsis);
    page->AddChild(tab.current);

    tab.bar = std::make_shared<ProgressBar>();
    tab.bar->SetHeight(10.0f);
    tab.bar->SetValue(0.0f);
    page->AddChild(tab.bar);

    auto row = std::make_shared<RowBox>();
    row->SetSpacing(10.0f);
    row->SetHeight(28.0f);

    tab.percent = std::make_shared<Label>(L"0%");
    tab.percent->SetWidth(56.0f);
    row->AddChild(tab.percent);

    tab.stats = std::make_shared<Label>(L"");
    tab.stats->SetFillWidth(true);
    row->AddChild(tab.stats);

    tab.cancel = std::make_shared<Button>(L"取消");
    tab.cancel->SetWidth(72.0f);
    tab.cancel->SetHeight(26.0f);
    const int taskId = item.id;
    tab.cancel->Connect(tab.cancel->Clicked, [this, taskId]() {
        if (hooks_.onCancel) hooks_.onCancel(taskId);
        RemoveTab(taskId, /*deferred*/ true);
    });
    row->AddChild(tab.cancel);
    page->AddChild(row);

    tab.list = std::make_shared<ColumnBox>();
    tab.list->SetSpacing(12.0f);
    tab.list->SetFillWidth(true);

    tab.scroll = std::make_shared<ScrollViewer>();
    tab.scroll->SetContent(tab.list);
    tab.scroll->SetFillWidth(true);
    tab.scroll->SetFillHeight(true);
    page->AddChild(tab.scroll);

    const int tabIndex = tabs_->AddTab(item.title, page, /*closable*/ true, item.id);
    tabStates_[item.id] = std::move(tab);
    tabs_->SetSelectedIndex(tabIndex);
    homeHash_ = 0;
}

void TaskWindow::SyncIssues(TabState& tab, const TaskItem& item) {
    if (!tab.list) return;

    std::set<std::wstring> wanted;
    for (const auto& issue : item.issues) wanted.insert(issue.path);

    // 1) 已经不存在的错误：**只隐藏，不销毁**。
    //    销毁一张卡片等于销毁它里面的按钮，而库里的 hover/press 还持有裸指针，
    //    点几次"跳过"就会野指针崩溃。隐藏是安全的，数量也有上限。
    for (auto& [path, card] : tab.cards) {
        card->SetVisible(wanted.count(path) > 0);
    }

    // 2) 新的错误：插一张卡片（追加是安全的）
    for (const auto& issue : item.issues) {
        if (tab.cards.count(issue.path)) continue;
        if (tab.cards.size() >= kMaxIssueCards) break;

        const int taskId = item.id;
        const std::wstring path = issue.path;
        const std::wstring target = issue.target;
        const int kind = issue.kind;

        auto col = std::make_shared<ColumnBox>();
        col->SetSpacing(6.0f);
        col->SetFillWidth(true);
        col->AddChild(std::make_shared<Label>(L"操作：" + OpTitle(item.op)));

        auto pathRow = [&](const wchar_t* prefix, const std::wstring& p) {
            auto row = std::make_shared<RowBox>();
            row->SetSpacing(8.0f);
            row->SetHeight(28.0f);
            auto label = std::make_shared<Label>(prefix);
            label->SetWidth(76.0f);
            row->AddChild(label);
            auto btn = std::make_shared<Button>(p);
            btn->SetFillWidth(true);
            btn->SetHeight(28.0f);
            btn->Connect(btn->Clicked, [this, p]() {
                if (hooks_.onOpenPath) hooks_.onOpenPath(p);
            });
            row->AddChild(btn);
            col->AddChild(row);
        };
        pathRow(L"源文件", path);
        if (!target.empty()) pathRow(L"目标文件", target);

        auto msg = std::make_shared<Label>(issue.message);
        msg->SetTextOverflow(Label::TextOverflow::Ellipsis);
        col->AddChild(msg);

        auto actions = std::make_shared<RowBox>();
        actions->SetSpacing(8.0f);
        actions->SetHeight(26.0f);

        auto addAction = [&](const wchar_t* text, float width, std::function<void()> fn) {
            auto b = std::make_shared<Button>(text);
            b->SetWidth(width);
            b->SetHeight(26.0f);
            b->Connect(b->Clicked, std::move(fn));
            actions->AddChild(b);
        };

        const bool streamOp = item.op == L"streamimport" || item.op == L"streamexport" ||
                              item.op == L"streamdelete" || item.op == L"streamhide" ||
                              item.op == L"streamwrite" || item.op == L"streamrename" ||
                              item.op == L"killprocess" || item.op == L"closehandle";
        if (streamOp) {
            // Stream errors are reported per task. Retrying needs a changed name or
            // path, so the generic copy retry path must not be exposed.
        } else if (kind == 1) {
            addAction(L"覆盖", 64.0f, [this, taskId, path, target]() {
                if (hooks_.onOverwrite) hooks_.onOverwrite(taskId, path, target);
            });
            addAction(L"跳过", 64.0f, [this, taskId, path]() {
                if (hooks_.onSkip) hooks_.onSkip(taskId, path);
            });
            addAction(L"全部覆盖", 88.0f, [this, taskId]() {
                if (hooks_.onAllOverwrite) hooks_.onAllOverwrite(taskId);
            });
            addAction(L"全部跳过", 88.0f, [this, taskId]() {
                if (hooks_.onAllSkip) hooks_.onAllSkip(taskId);
            });
        } else {
            addAction(L"重试", 64.0f, [this, taskId, path, target]() {
                if (hooks_.onOverwrite) hooks_.onOverwrite(taskId, path, target);
            });
            addAction(L"忽略", 64.0f, [this, taskId, path]() {
                if (hooks_.onSkip) hooks_.onSkip(taskId, path);
            });
            addAction(L"全部重试", 88.0f, [this, taskId]() {
                if (hooks_.onAllRetry) hooks_.onAllRetry(taskId);
            });
            addAction(L"全部忽略", 88.0f, [this, taskId]() {
                if (hooks_.onAllIgnore) hooks_.onAllIgnore(taskId);
            });
        }

        col->AddChild(actions);

        auto card = std::make_shared<Card>();
        card->SetContent(col);
        card->SetFillWidth(true);
        tab.list->AddChild(card);
        tab.cards[path] = card;
    }
}

void TaskWindow::RebuildHome(const std::vector<TaskItem>& tasks) {
    if (!homeList_) return;

    std::vector<const TaskItem*> visible;
    for (const auto& t : tasks) {
        if (t.dismissed) continue;
        visible.push_back(&t);
    }
    auto rank = [](int s) { return s == 1 ? 0 : s == 0 ? 1 : s == 3 ? 2 : s == 4 ? 3 : 4; };
    std::sort(visible.begin(), visible.end(), [&](const TaskItem* a, const TaskItem* b) {
        const int ra = rank(a->state), rb = rank(b->state);
        return ra != rb ? ra < rb : a->id < b->id;
    });

    size_t hash = visible.size();
    for (const auto* t : visible) {
        hash = hash * 131 + static_cast<size_t>(t->id);
        hash = hash * 131 + static_cast<size_t>(t->state);
        hash = hash * 131 + t->issues.size();
        // 注意：不能把 ratio 放进 hash —— 否则进度一变就整页重建，看起来"鬼畜"
    }
    if (hash == homeHash_) return;
    if (MouseDown()) return;                 // 按住鼠标不动结构
    homeHash_ = hash;

    homeList_->ClearChildren();
    homeBars_.clear();
    homeStats_.clear();

    int shown = 0;
    for (const auto* t : visible) {
        if (shown++ >= static_cast<int>(kMaxHomeCards)) break;

        auto col = std::make_shared<ColumnBox>();
        col->SetSpacing(6.0f);

        std::wstring desc = OpTitle(t->op);
        if (!t->sources.empty()) desc += L"：" + FileNameOf(t->sources.front());
        if (!t->targetDir.empty()) desc += L"  →  " + t->targetDir;
        col->AddChild(std::make_shared<Label>(desc));

        auto stats = std::make_shared<Label>(TaskStatsText(*t));
        stats->SetTextOverflow(Label::TextOverflow::Ellipsis);
        col->AddChild(stats);

        auto bar = std::make_shared<ProgressBar>();
        bar->SetHeight(8.0f);
        bar->SetValue(t->state == 2 ? 1.0f : t->ratio);
        col->AddChild(bar);

        if (!t->issues.empty()) {
            auto row = std::make_shared<RowBox>();
            row->SetSpacing(8.0f);
            row->SetHeight(26.0f);
            auto warn = std::make_shared<Label>(
                L"该操作需要引起注意（" + std::to_wstring(t->issues.size()) + L" 项失败）");
            warn->SetFillWidth(true);
            row->AddChild(warn);

            const int taskId = t->id;
            auto open = std::make_shared<Button>(L"查看");
            open->SetWidth(64.0f);
            open->SetHeight(26.0f);
            open->Connect(open->Clicked, [this, taskId]() {
                ShowTask(taskId);
            });
            row->AddChild(open);
            col->AddChild(row);
        }

        auto card = std::make_shared<Card>();
        card->SetContent(col);
        card->SetFillWidth(true);
        homeList_->AddChild(card);
        homeBars_[t->id] = bar;
        homeStats_[t->id] = stats;
    }
}

// 首页卡片的进度条就地更新（不重建），避免整页闪烁
void TaskWindow::UpdateHomeProgress(const std::vector<TaskItem>& tasks) {
    for (const auto& t : tasks) {
        auto it = homeBars_.find(t.id);
        if (it != homeBars_.end() && it->second)
            it->second->SetValue(t.state == 2 ? 1.0f : t.ratio);
        auto stats = homeStats_.find(t.id);
        if (stats != homeStats_.end() && stats->second)
            stats->second->SetText(TaskStatsText(t));
    }
}

void TaskWindow::Update(const std::vector<TaskItem>& tasks) {
    if (tasks.empty() && !win_) return;

    // 先把上一拍登记的删除做掉
    if (!MouseDown() && !pendingRemove_.empty()) {
        auto pending = pendingRemove_;
        pendingRemove_.clear();
        for (int id : pending) RemoveTab(id, /*deferred*/ false);
    }

    std::map<int, const TaskItem*> byId;
    for (const auto& t : tasks) byId[t.id] = &t;

    for (const auto& [id, item] : byId) {
        (void)id;
        if (item->dismissed) continue;

        const bool shouldShow = item->state == 1 ? item->elapsedMs >= static_cast<int>(showDelayMs_)
                                                 : (item->state == 3 || item->state == 5);
        if (!shouldShow) continue;
        const bool existed = tabStates_.count(item->id) > 0;
        EnsureTab(*item);
        if (!existed && item->originPid == 0)
            ShowAndFocus();               // 无来源的旧协议任务仍走全局任务窗
    }

    for (auto it = tabStates_.begin(); it != tabStates_.end();) {
        auto found = byId.find(it->first);
        if (found == byId.end()) {
            // Host pruned an old terminal task. Do not leave a stale tab that
            // can no longer show progress or process decisions.
            RemoveTab(it->first, /*deferred*/ true);
            ++it;
            continue;
        }
        const TaskItem& item = *found->second;

        TabState& tab = it->second;
        const float ratio = item.state == 2 ? 1.0f : item.ratio;
        if (tab.bar) tab.bar->SetValue(ratio);
        if (tab.percent) {
            wchar_t buf[32];
            swprintf_s(buf, L"%d%%", static_cast<int>(ratio * 100.0f));
            tab.percent->SetText(buf);
        }
        if (tab.stats) {
            tab.stats->SetText(TaskStatsText(item));
        }
        if (tab.current) {
            if (item.state == 5) {
                tab.current->SetText(L"有 " + std::to_wstring(item.issues.size()) +
                                     L" 项问题等待处理（同一个任务，处理后继续）");
            } else if (item.state == 3) {
                tab.current->SetText(L"有 " + std::to_wstring(item.issues.size()) +
                                     L" 项需要处理（其余已完成）");
            } else if (item.state == 2) {
                tab.current->SetText(L"已完成");
            } else if (item.state == 4) {
                tab.current->SetText(L"已取消");
            } else if (!item.currentFile.empty()) {
                tab.current->SetText(item.currentFile);
            }
        }
        if (tab.cancel) {
            tab.cancel->SetEnabled(item.state != 2 && item.state != 4);
        }

        SyncIssues(tab, item);

        // 状态从"跑着"变成"出错/等决策" -> 弹出来并前置（用户不用自己找）
        if (item.originPid == 0 && tab.lastState != item.state &&
            (item.state == 3 || item.state == 5)) {
            ShowTask(item.id);
        }
        tab.lastState = item.state;

        // 出现新错误 -> 闪任务栏（不管设置如何都要闪）
        if (item.originPid == 0 && item.issues.size() > tab.lastIssueCount) {
            if (beepOnError_) MessageBeep(MB_ICONWARNING);
            if (win_ && win_->IsValid()) {
                FLASHWINFO fi{};
                fi.cbSize = sizeof(fi);
                fi.hwnd = win_->GetHwnd();
                fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
                fi.uCount = 3;
                FlashWindowEx(&fi);
            }
            tab.lastIssueCount = item.issues.size();
        } else if (item.issues.size() < tab.lastIssueCount) {
            tab.lastIssueCount = item.issues.size();
        }

        if (item.state == 2) {
            if (tab.doneAt == 0) tab.doneAt = GetTickCount64();
            if (GetTickCount64() - tab.doneAt >= doneLingerMs_) {
                RemoveTab(it->first, /*deferred*/ true);
            }
        }
        ++it;
    }

    RebuildHome(tasks);
    UpdateHomeProgress(tasks);
}

} // namespace ze::host
