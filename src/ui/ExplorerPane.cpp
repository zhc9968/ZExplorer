#include "pch.h"
#include "ui/ExplorerPane.h"

#include "fs/ShellIcons.h"
#include "app/TaskClient.h"
#include "platform/FileAttributes.h"
#include "platform/FileLock.h"
#include "platform/FileOps.h"
#include "platform/NtfsStreams.h"
#include "platform/PathUtil.h"
#include "platform/Settings.h"
#include "platform/ShellActions.h"
#include "platform/Shortcuts.h"
#include "platform/TaskbarProgress.h"
#include "ui/FileListView.h"

#include <cmath>

using namespace ZufyUI;
using namespace ze::platform;

namespace ze::ui {

namespace {

constexpr float kToolbarHeight = 36.0f;
constexpr float kIconDpi = 16.0f;
constexpr float kCrumbHeight = 32.0f;
constexpr float kCrumbSegMaxWidth = 180.0f;   // 面包屑单段文字宽度上限

std::wstring NodeText(const std::shared_ptr<TreeNode>& n) {
    if (!n || n->columns.empty() || !n->columns[0]) return L"";
    return n->columns[0]->GetText();
}

// fg 以自身 alpha 覆盖到 bg 上，返回不透明色。
// 用途：把"半透明强调色"变成实色底——直接用带透明度的色做悬停底色会在插值时
// 经过半透明深色（看起来就是"先变深再变浅"）。
Color BlendOver(Color fg, Color bg) {
    const float a = fg.a;
    return Color(fg.r * a + bg.r * (1.0f - a),
                 fg.g * a + bg.g * (1.0f - a),
                 fg.b * a + bg.b * (1.0f - a),
                 1.0f);
}

float MeasureTextWidth(const std::wstring& text, const FontSpec& spec) {
    if (text.empty()) return 0.0f;
    FontManager& fm = FontManager::Instance();
    IDWriteTextFormat* fmt = fm.GetFormat(spec);
    if (!fmt) return 0.0f;
    auto layout = fm.GetRawLayout(text, fmt, 8192.0f, 128.0f, /*forceNoWrap*/ true);
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.widthIncludingTrailingWhitespace > 0.0f ? m.widthIncludingTrailingWhitespace : m.width;
}

// 超宽时"首…尾"折叠（中间省略），保证不超 maxWidth。
std::wstring ElideMiddle(const std::wstring& text, const FontSpec& spec, float maxWidth) {
    if (text.empty()) return text;
    if (MeasureTextWidth(text, spec) <= maxWidth) return text;

    const std::wstring ell = L"...";
    for (size_t keep = text.size(); keep >= 1; --keep) {
        const size_t head = (keep + 1) / 2;
        const size_t tail = keep / 2;
        std::wstring cand = text.substr(0, head) + ell;
        if (tail > 0) cand += text.substr(text.size() - tail);
        if (MeasureTextWidth(cand, spec) <= maxWidth) return cand;
    }
    return ell;
}

std::wstring LocalTimestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
               now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return buf;
}

std::wstring CurrentProcessPath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length != 0 && length < path.size()
        ? std::wstring(path.data(), length)
        : L"<未知进程路径>";
}

std::wstring ExceptionText(const std::exception_ptr& exception) {
    if (!exception) return L"<没有异常对象>";
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& e) {
        const char* text = e.what();
        if (!text || !*text) return L"<异常没有文本>";
        int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        UINT codePage = CP_UTF8;
        if (length <= 0) {
            codePage = CP_ACP;
            length = MultiByteToWideChar(codePage, 0, text, -1, nullptr, 0);
        }
        if (length <= 1) return L"<异常文本无法解码>";
        std::wstring out(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(codePage, 0, text, -1, out.data(), length);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    } catch (...) {
        return L"<未知的非标准异常>";
    }
}

// 常驻的「任务」按钮：平时就是个按钮；有任务时底部叠一条进度。
// 尺寸固定（96×32），所以有/无任务时工具栏不会跳。
class TaskProgressButton : public Button {
public:
    explicit TaskProgressButton(const std::wstring& text) : Button(text) {}

    void SetProgress(float p) {
        p = (std::max)(0.0f, (std::min)(1.0f, p));
        if (p != progress_) { progress_ = p; RequestRepaint(); }
    }

    void Draw(ID2D1RenderTarget* rt) override {
        Button::Draw(rt);
        if (!rt || progress_ <= 0.0f) return;

        const auto r = GetArrangedRect();
        D2D1_COLOR_F col =
            ThemeManager::AppTheme().Get(ThemeRole::TextSecondary).ToD2D();
        col.a = 0.35f;
        if (!brush_) rt->CreateSolidColorBrush(col, brush_.GetAddressOf());
        else brush_->SetColor(col);
        if (brush_) {
            rt->FillRectangle(D2D1::RectF(r.x + 2.0f, r.y + r.height - 5.0f,
                                          r.x + 2.0f + (r.width - 4.0f) * progress_,
                                          r.y + r.height - 2.0f), brush_.Get());
        }
    }

    void ReleaseDeviceResources() override {
        Button::ReleaseDeviceResources();
        brush_.Reset();
    }

private:
    float progress_ = 0.0f;
    ComPtr<ID2D1SolidColorBrush> brush_;
};
// 只加焦点钩子、不做任何绘制：地址栏要在"聚焦/失焦"两种呈现间切换。
class AddressTextBox : public TextBox {
public:
    std::function<void(bool)> FocusChanged;

    void OnFocus() override {
        TextBox::OnFocus();
        if (FocusChanged) FocusChanged(true);
    }
    void OnBlur() override {
        TextBox::OnBlur();
        if (FocusChanged) FocusChanged(false);
    }
};

class AddressCrumbCard : public Card {
public:
    std::function<void()> OnBlankClick;

    void OnMouseDown(float x, float y) override {
        if (OnBlankClick) OnBlankClick();
        Card::OnMouseDown(x, y);
    }
};

class QuickAccessSplit : public SplitView {
public:
    void ArrangeOverride(const Rect& finalRect) override {
        const float minFirst = max(1.0f, finalRect.height / 12.0f);
        if (minFirst != appliedMinFirst_) {
            appliedMinFirst_ = minFirst;
            SetMinFirst(minFirst);
        }
        SplitView::ArrangeOverride(finalRect);
    }

private:
    float appliedMinFirst_ = -1.0f;
};

std::shared_ptr<MenuItem> MakeMenuAction(
    const std::wstring& text, Icon icon, std::function<void()> callback,
    bool enabled = true, bool danger = false,
    const std::wstring& shortcut = std::wstring()) {
    auto item = Menu::MakeItem(text, std::move(callback));
    item->SetGlyphIcon(icon, 18.0f);
    item->enabled = enabled;
    item->danger = danger;
    item->shortcut = shortcut;
    return item;
}

MenuRowOptions InlineRowOptions(float minItemWidth = 72.0f,
                                float maxItemWidth = 180.0f) {
    MenuRowOptions options;
    options.layout = MenuRowLayout::Inline;
    options.width = MenuRowWidth::ContentFit;
    options.gap = 6.0f;
    options.minItemWidth = minItemWidth;
    options.maxItemWidth = maxItemWidth;
    options.inlineHeight = 34.0f;
    options.iconSize = 16.0f;
    options.showShortcuts = false;
    return options;
}

} // namespace

class RecycleTableView final : public TableView {
public:
    std::function<bool(int)> OnNativeItemMenu;

    bool OnContextMenu(float x, float y) override {
        TableView::OnContextMenu(x, y);
        const int row = GetContextRow();
        if (row >= 0 && OnNativeItemMenu && OnNativeItemMenu(row))
            return true;
        return false;
    }
};

class ClickableCard final : public Card {
public:
    std::function<void()> OnClick;

    HCURSOR GetPointerCursorAt(float, float, bool) const override {
        return LoadCursor(nullptr, IDC_HAND);
    }

    UIElement* HitTest(float x, float y) override {
        return visible_ && arrangedRect_.Contains(x, y) ? this : nullptr;
    }

    void OnMouseDown(float, float) override { pressed_ = true; }
    void OnMouseUp(float x, float y) override {
        const bool fire = pressed_ && arrangedRect_.Contains(x, y);
        pressed_ = false;
        if (fire && OnClick) OnClick();
    }
    void OnPointerCancel() override { pressed_ = false; }

private:
    bool pressed_ = false;
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
ExplorerPane::ExplorerPane() = default;
ExplorerPane::~ExplorerPane() {
    StopDirectoryWatcher();
    if (window_) platform::ClearTaskbarProgress(window_->GetHwnd());
}

std::shared_ptr<UIElement> ExplorerPane::Root() const {
    if (notificationDrawer_) return notificationDrawer_->Root();
    return root_;
}

std::shared_ptr<ExplorerPane> ExplorerPane::Create() {
    auto pane = std::shared_ptr<ExplorerPane>(new ExplorerPane());
    pane->Build();
    pane->BuildDriveTree();
    return pane;
}

std::shared_ptr<Image> ExplorerPane::FolderIcon() {
    if (!folderIcon_) {
        HICON h = fs::ShellIcons::Instance().Get(/*isDir*/ true, L"folder", /*preferFileIcon*/ false,
                                                 fs::IconSize::Small);
        if (h) folderIcon_ = Image::FromHICON(h);
    }
    return folderIcon_;
}

std::shared_ptr<Image> ExplorerPane::TreeNodeIcon(const std::wstring& path) {
    if (path.empty()) return FolderIcon();
    if (auto it = treeIconCache_.find(path); it != treeIconCache_.end()) return it->second;
    if (treeIconCache_.size() > 2000) return FolderIcon();

    const bool isDir = IsDirectoryPath(path);
    HICON h = fs::ShellIcons::Instance().Get(isDir, path, /*preferFileIcon*/ true,
                                             fs::IconSize::Small);
    std::shared_ptr<Image> img = h ? Image::FromHICON(h) : nullptr;
    if (!img && isDir) img = FolderIcon();
    treeIconCache_.emplace(path, img);
    return img;
}

void ExplorerPane::Build() {
    const Theme& th = ThemeManager::AppTheme();
    const Color transparent(0.0f, 0.0f, 0.0f, 0.0f);

    root_ = std::make_shared<ColumnBox>();
    root_->SetMargin(Thickness(16, 12, 16, 12));
    root_->SetSpacing(8.0f);

    // ---- 工具栏：[任务][‹][›][˄][地址栏占满剩余空间] ----
    toolbar_ = std::make_shared<RowBox>();
    toolbar_->SetSpacing(6.0f);
    toolbar_->SetHeight(kToolbarHeight);

    // 工具栏按钮：底色用主题的"选中色"（可用=淡蓝），悬停/按下在同一色上压深
    const Color selColor = th.Get(ThemeRole::Selection);
    const Color navNormal = selColor;
    const Color navHover = BlendOver(Color(0, 0, 0, 0.06f), selColor);
    const Color navPressed = BlendOver(Color(0, 0, 0, 0.12f), selColor);

    auto makeIconButton = [this, navNormal, navHover, navPressed, th](Icon icon, float w,
                                                                      const std::wstring& tip,
                                                                      std::function<void()> onClick) {
        auto b = std::make_shared<Button>(L"");
        b->SetIcon(icon, 14.0f);
        b->SetWidth(w);
        b->SetHeight(32.0f);
        b->SetCornerRadius(6.0f);
        b->SetColors(navNormal, navHover, navPressed);
        // SetColors 之后必须显式给图标着色，否则字形跟着背景色一起看不见
        b->SetIconColor(th.Get(ThemeRole::Text));
        if (!tip.empty()) b->SetToolTip(tip);
        b->Connect(b->Clicked, std::move(onClick));
        return b;
    };
    auto styleSoftButton = [navNormal, navHover, navPressed, th](
                               const std::shared_ptr<Button>& button) {
        button->SetColors(navNormal, navHover, navPressed);
        button->SetTextColor(th.Get(ThemeRole::Text));
        button->SetIconColor(th.Get(ThemeRole::Text));
    };

    backBtn_ = makeIconButton(Icon::ChevronLeft, 34.0f, L"后退", [this]() { GoBack(); });
    forwardBtn_ = makeIconButton(Icon::ChevronRight, 34.0f, L"前进", [this]() { GoForward(); });
    upBtn_ = makeIconButton(Icon::ChevronUp, 34.0f, L"上一级", [this]() { GoUp(); });

    // ---- 地址栏：TextBox 与面包屑叠在同一格 ----
    addressHost_ = std::make_shared<GridLayout>();
    addressHost_->SetColumnCount(1);
    addressHost_->SetRowCount(1);
    addressHost_->SetFillWidth(true);
    addressHost_->SetHorizontalStretchWeight(1.0f);

    auto box = std::make_shared<AddressTextBox>();
    box->SetPlaceholder(L"输入路径后回车");
    box->SetWidth(0.0f);
    box->SetMinWidth(0.0f);
    box->SetHeight(kCrumbHeight);
    box->SetFillWidth(true);
    box->FocusChanged = [this](bool focused) {
        if (focused) EnterAddressEditMode();
        else LeaveAddressEditMode();
    };
    box->Connect(box->ReturnPressed, [this]() { CommitAddressInput(); });
    addressBox_ = box;

    // 面包屑容器（未聚焦时显示，盖在 TextBox 上面）
    auto crumb = std::make_shared<AddressCrumbCard>();
    crumb_ = crumb;
    crumb_->SetHeight(kCrumbHeight);
    crumb_->SetPadding(3.0f);
    crumb_->SetCornerRadius(6.0f);
    crumb_->SetBackgroundColor(th.Get(ThemeRole::Surface));
    crumb_->SetBorderColor(th.Get(ThemeRole::Border));
    crumb_->SetFillWidth(true);
    crumb_->SetBleed(4.0f);   // 出血：卡片圆角/描边不被裁
    crumb->OnBlankClick = [this]() {
        FocusAddressEditor();
    };

    crumbRow_ = std::make_shared<RowBox>();
    crumbRow_->SetSpacing(2.0f);
    crumb_->SetContent(crumbRow_);

    addressHost_->AddChild(addressBox_, 0, 0);
    addressHost_->AddChild(crumb_, 0, 0);    // 后加 = 画在上面

    // 焦点离开时恢复面包屑：TextBox 不会因为"点到别的控件"就收到 OnBlur，
    // 所以监听窗口级鼠标按下，点在地址栏外就主动失焦。
    root_->Connect(UIZSignals::GlobalMouseDown, [this](Window* w, float x, float y) {
        if (!addressBox_ || !addressHost_ || !addressBox_->IsFocused()) return;
        if (w != addressHost_->GetWindow()) return;

        const Rect r = addressHost_->GetArrangedRect();
        const bool inside = (x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height);
        if (!inside) BlurAddressEditor();
    });

    searchBox_ = std::make_shared<TextBox>();
    searchBox_->SetWidth(0.0f);       // 不携带固定基准宽度，由 RowBox 分配剩余空间
    searchBox_->SetMinWidth(0.0f);
    searchBox_->SetHeight(32.0f);
    searchBox_->SetPlaceholder(L"在当前目录中搜索");
    searchBox_->SetImeEnabled(true);
    searchBox_->Connect(searchBox_->TextChanged,
                        [this](const std::wstring& text) {
        if (searchUpdating_) return;
        if (text.empty()) {
            ClearSearch();
            return;
        }
        ++searchGeneration_;
        if (searchDebounceTimer_) {
            searchDebounceTimer_->Stop();
            searchDebounceTimer_->Start(220);
        }
        ShowHint(L"正在搜索当前目录和子目录…");
    });
    searchBox_->Connect(searchBox_->ReturnPressed, [this]() {
        if (searchDebounceTimer_) searchDebounceTimer_->Stop();
        StartRecursiveSearch();
    });

    // ---- 「任务」按钮：常驻、固定尺寸；有任务时底部显示总进度 ----
    auto taskButton = std::make_shared<TaskProgressButton>(L"任务");
    styleSoftButton(taskButton);
    taskButton->SetWidth(96.0f);
    taskButton->SetHeight(32.0f);
    taskButton->SetToolTip(L"任务列表（点击查看/取消）");
    taskButton->Connect(taskButton->Clicked, [this]() { ShowTaskMenu(); });
    taskBtn_ = taskButton;
    toolbar_->AddChild(taskBtn_);

    toolbar_->AddChild(backBtn_);
    toolbar_->AddChild(forwardBtn_);
    toolbar_->AddChild(upBtn_);
    toolbar_->AddChild(addressHost_);

    // ---- 第二层：新建 / 常用文件操作 / 查看 / 更多 ----
    actionBar_ = std::make_shared<RowBox>();
    actionBar_->SetSpacing(6.0f);
    actionBar_->SetHeight(40.0f);

    auto newButton = std::make_shared<Button>(L"新建");
    styleSoftButton(newButton);
    newButton->SetIcon(Icon::Add, 15.0f);
    newButton->SetWidth(140.0f);
    newButton->SetHeight(34.0f);
    newButton->SetToolTip(L"新建文件；右侧箭头选择文件或文件夹");
    newButton->SetDropItems({ L"新建文件", L"新建文件夹" }, 0);
    newButton->SetDropWriteBack(true);
    newButton->SetDropOpenOnClick(false);
    auto createFromDropSelection = [this, newButton]() {
        if (newButton->GetDropSelectedIndex() == 1) DoNewFolder();
        else DoNewTextFile();
    };
    newButton->Connect(newButton->Clicked, createFromDropSelection);
    newButton->DropSelected.connect(
        [this](int selected) {
            if (selected == 1) DoNewFolder();
            else DoNewTextFile();
        });
    actionBar_->AddChild(newButton);

    auto optionButton = std::make_shared<Button>(L"选项");
    styleSoftButton(optionButton);
    optionButton->SetIcon(Icon::Settings, 15.0f);
    optionButton->SetWidth(82.0f);
    optionButton->SetHeight(34.0f);
    optionButton->SetToolTip(L"查看与页面选项");
    optionButton->Connect(optionButton->Clicked, [this]() {
        auto menu = std::make_shared<Menu>();
        auto iconLast = [&](Icon icon) {
            if (!menu->items.empty())
                menu->items.back()->SetGlyphIcon(icon, 16.0f);
        };
        const AppSettings view = LoadSettings();
        menu->AddCheckItemCb(
            L"显示隐藏和系统文件", view.showHiddenFiles,
            [this, view](bool on) mutable {
                UpdateViewOption(on, view.hideKnownExtensions);
            });
        iconLast(Icon::View);
        menu->AddCheckItemCb(
            L"隐藏已知扩展名", view.hideKnownExtensions,
            [this, view](bool on) mutable {
                UpdateViewOption(view.showHiddenFiles, on);
            });
        iconLast(Icon::File);
        menu->AddSeparator();
        const bool canPin = currentPath_ != L"::thispc" &&
                            currentPath_ != L"::recyclebin" &&
                            !currentPath_.empty();
        menu->AddItem(L"固定到快速访问",
                      [this]() { ToggleQuickAccessPin(currentPath_); });
        iconLast(Icon::Pin);
        menu->items.back()->enabled = canPin;
        menu->AddItem(L"全部选择", [this]() { DoSelectAll(); });
        iconLast(Icon::Check);
        menu->AddItem(L"全部取消", [this]() { DoSelectNone(); });
        iconLast(Icon::Cancel);
        menu->AddItem(L"反向选择", [this]() { DoInvertSelection(); });
        iconLast(Icon::Sync);
        menu->AddSeparator();
        menu->AddItem(L"设置", []() { app::TaskClient::OpenSettings(); });
        iconLast(Icon::Settings);
        menu->AddItem(L"关于", []() { app::TaskClient::OpenAbout(); });
        iconLast(Icon::Info);
        menu->ShowAtCursor();
    });
    actionBar_->AddChild(optionButton);

    auto makeActionSeparator = []() {
        auto separator = std::make_shared<Label>(L"");
        separator->SetWidth(1.0f);
        separator->SetHeight(22.0f);
        separator->SetBackgroundColor(
            ThemeManager::AppTheme().Get(ThemeRole::Divider));
        return separator;
    };
    actionBar_->AddChild(makeActionSeparator());
    actionBar_->AddChild(makeIconButton(Icon::Cut, 34.0f, L"剪切", [this]() { DoCopy(true); }));
    actionBar_->AddChild(makeIconButton(Icon::Copy, 34.0f, L"复制", [this]() { DoCopy(false); }));
    actionBar_->AddChild(makeIconButton(Icon::Paste, 34.0f, L"粘贴", [this]() { DoPaste(); }));
    actionBar_->AddChild(makeIconButton(Icon::Edit, 34.0f, L"重命名", [this]() { DoRename(); }));
    actionBar_->AddChild(makeIconButton(Icon::Delete, 34.0f, L"删除", [this]() { DoDelete(false); }));
    actionBar_->AddChild(makeIconButton(
        Icon::Open, 34.0f, L"在当前位置打开新窗口",
        [this]() { LaunchNewWindowHere(); }));
    actionBar_->AddChild(makeActionSeparator());
    searchBox_->SetFillWidth(true);
    searchBox_->SetHorizontalStretchWeight(1.0f);
    searchBox_->SetHeight(34.0f);
    actionBar_->AddChild(searchBox_);

    // ---- 主体：SplitView（左树 | 右列）----
    split_ = std::make_shared<SplitView>();
    split_->SetFillHeight(true);
    split_->SetMinFirst(160.0f);
    split_->SetMinSecond(320.0f);
    split_->SetSplitRatio(0.22f);
    split_->SetSplitterWidth(6.0f);
    split_->SetCollapseEnabled(true);
    // 只有"两侧最小宽度放不下"时才折叠，并保留左侧（列表）——不是启动就折叠
    split_->SetAutoCollapseOnInsufficientSpace(true);
    split_->SetInsufficientSpaceDefaultSide(2);

    tree_ = std::make_shared<TreeView>();
    tree_->SetColumnCount(1);
    tree_->SetHeaderVisible(false);
    tree_->SetRowHeight(26.0f);
    tree_->SetIndent(16.0f);

    tree_->Connect(tree_->SelectionChanged, [this](std::shared_ptr<TreeNode> node) {
        if (suppressTreeSync_) return;      // 程序化同步触发的选中，不要再导航一次
        if (!node) return;
        if (node->tooltip.empty()) return;
        if (_wcsicmp(node->tooltip.c_str(), currentPath_.c_str()) == 0) return;   // 已经是当前目录
        NavigateTo(node->tooltip);
    });

    tree_->SetNodeLabelProvider([this](const std::shared_ptr<TreeNode>& node, int col, Label& lb) {
        if (!node || col != 0) return;
        lb.SetTextFast(NodeText(node));
        lb.SetTextOverflow(Label::TextOverflow::Ellipsis);
        lb.SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
        if (auto img = TreeNodeIcon(node->tooltip)) {
            lb.SetImage(img);
            lb.SetIconSize(kIconDpi, kIconDpi);
            lb.SetIconSpacing(6.0f);
        }
    });

    tree_->SetLazyLoader([this](std::shared_ptr<TreeNode> node) { LoadTreeNode(node); });

    // 左栏上半部：ZExplorer 自有快捷访问。为空时整个上半区及分隔条隐藏，
    // 目录树直接占满左栏。
    quickList_ = std::make_shared<ListView>();
    quickList_->SetItemHeight(28.0f);
    quickList_->SetSelectionMode(ListView::SelectionMode::Single);
    quickList_->SetEmptyText(L"没有快捷访问项目");
    quickList_->Connect(quickList_->ItemClicked,
                        [this](int index) { OpenQuickAccessItem(index, false); });
    quickList_->Connect(quickList_->ItemDoubleClicked,
                        [this](int index) { OpenQuickAccessItem(index, true); });
    quickList_->Connect(quickList_->ItemRightClicked,
                        [this](int index) { quickList_->SetSelectedIndex(index); });
    quickList_->SetContextMenuFactory([this]() {
        const int index = quickList_->GetContextRow();
        if (index < 0 || index >= static_cast<int>(quickEntries_.size()))
            return std::make_shared<Menu>();
        const auto& entry = quickEntries_[static_cast<size_t>(index)];
        if (entry.path == L"::recyclebin") {
            auto menu = std::make_shared<Menu>();
            menu->AddItem(L"打开回收站", [this]() { NavigateTo(L"::recyclebin"); });
            menu->AddItem(L"在资源管理器中打开", []() { OpenRecycleBin(); });
            return menu;
        }
        return BuildTargetContextMenu(entry.path);
    });

    quickPane_ = std::make_shared<ColumnBox>();
    quickPane_->SetSpacing(4.0f);
    quickPane_->SetFillWidth(true);
    quickPane_->SetFillHeight(true);
    auto quickTitle = std::make_shared<Label>(L"快速访问");
    quickTitle->SetHeight(24.0f);
    quickTitle->SetTextColor(ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
    quickTitle->SetPadding(Thickness(8.0f, 0.0f, 8.0f, 0.0f));
    quickPane_->AddChild(quickTitle);
    quickList_->SetFillHeight(true);
    quickPane_->AddChild(quickList_);

    leftStack_ = std::make_shared<QuickAccessSplit>();
    leftStack_->SetOrientation(SplitView::Orientation::Horizontal); // 上/下
    leftStack_->SetFillHeight(true);
    leftStack_->SetSplitterWidth(6.0f);
    leftStack_->SetMinFirst(1.0f);
    leftStack_->SetMinSecond(160.0f);
    leftStack_->SetSplitRatio(0.32f);
    leftStack_->SetCollapseEnabled(true);
    leftStack_->SetFirst(quickPane_);
    leftStack_->SetSecond(tree_);
    leftStack_->SplitChanged.connect([this](float ratio) {
        if (ratio > 0.5f && leftStack_) leftStack_->SetSplitRatio(0.5f);
    });

    // 右列：提示条 + 列表
    rightCol_ = std::make_shared<ColumnBox>();
    rightCol_->SetSpacing(6.0f);
    rightCol_->SetFillHeight(true);

    hint_ = std::make_shared<Label>(L"");
    hint_->SetVisible(false);

    list_ = MakeEagerFileListView();
    list_->SetEmptyText(L"此文件夹为空");
    const AppSettings initialView = LoadSettings();
    list_->SetHideKnownExtensions(initialView.hideKnownExtensions);
    list_->SetDragPathsProvider([this]() { return SelectedPaths(); });
    list_->SetCallbacks([this](int index) { OpenIndex(index); },
                        [this](int index) { SelectIndex(index); });
    list_->SetSortChanged([this](int column) { SortByColumn(column); });
    list_->SetSortIndicator(0, true);

    rightCol_->AddChild(hint_);
    auto listWidget = list_->Widget();
    listWidget->SetFillHeight(true);
    listWidget->SetDropTargetEnabled(true);
    listWidget->Connect(listWidget->DragEnter, [this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    listWidget->Connect(listWidget->DragOver, [this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    listWidget->Connect(listWidget->DragLeave, [](DragEventArgs&) {});
    listWidget->Connect(listWidget->Drop, [this](DragEventArgs& e) {
        HandleExternalDrag(e, true);
    });

    // 右键菜单：**一个统一菜单**，没选中时把与选中相关的项置灰
    listWidget->SetContextMenuFactory([this]() {
        auto menu = std::make_shared<Menu>();
        const auto paths = SelectedPaths();
        const bool hasSelection = !paths.empty();
        const bool singleSelection = paths.size() == 1;
        const fs::FileEntry* selected = SelectedEntry();
        const bool singleFile = singleSelection && selected && !selected->isDir;
        const std::wstring pinPath = singleSelection ? paths.front() : L"";
        const bool pinned = !pinPath.empty() && IsQuickAccessPinned(pinPath);
        const std::wstring nativeTarget =
            hasSelection ? paths.front()
                         : (currentPath_ == L"::thispc" ? std::wstring()
                                                       : currentPath_);

        auto open = MakeMenuAction(L"打开", Icon::Open, [this]() { DoOpenSelected(); },
                                   hasSelection, false, L"Enter");
        open->isDefault = true;
        auto openWith = MakeMenuAction(L"打开方式…", Icon::More,
                                       [this]() { DoOpenWith(); }, singleFile, false,
                                       L"Ctrl+O");
        auto openLocation = MakeMenuAction(L"打开文件位置", Icon::Folder,
                                           [this]() { DoOpenLocation(); }, hasSelection);
        std::vector<std::shared_ptr<MenuItem>> openRow =
            { open, openWith, openLocation };
        std::vector<std::shared_ptr<MenuItem>> shortcutLocationRow;
        if (singleSelection && IsShortcutPath(paths.front())) {
            ShortcutInfo shortcut;
            if (ResolveShortcut(paths.front(), &shortcut, nullptr) &&
                !shortcut.targetPath.empty() &&
                (shortcut.kind == ShortcutTargetKind::File ||
                 shortcut.kind == ShortcutTargetKind::Directory)) {
                const std::wstring target = shortcut.targetPath;
                const std::wstring label =
                    shortcut.kind == ShortcutTargetKind::Directory
                        ? L"打开目标文件夹"
                        : L"打开目标位置";
                shortcutLocationRow.push_back(MakeMenuAction(
                    label, Icon::Link, [this, target]() {
                        OpenLocationInZExplorer(target);
                    }, true));
            }
        }
        menu->AddRow(std::move(openRow), InlineRowOptions(112.0f, 220.0f));
        for (const auto& action : shortcutLocationRow)
            menu->AddRow({ action }, InlineRowOptions(220.0f, 320.0f));
        menu->AddSeparator();

        auto newFolder = MakeMenuAction(L"新建文件夹", Icon::Folder,
                                        [this]() { DoNewFolder(); }, true, false,
                                        L"Ctrl+Shift+N");
        auto newText = MakeMenuAction(L"新建文本文档", Icon::File,
                                      [this]() { DoNewTextFile(); }, true);
        menu->AddRow({ newFolder, newText }, InlineRowOptions(128.0f, 220.0f));
        menu->AddSeparator();

        auto copy = MakeMenuAction(L"复制", Icon::Copy, [this]() { DoCopy(false); },
                                   hasSelection, false, L"Ctrl+C");
        auto cut = MakeMenuAction(L"剪切", Icon::Cut, [this]() { DoCopy(true); },
                                  hasSelection, false, L"Ctrl+X");
        auto paste = MakeMenuAction(L"粘贴", Icon::Paste,
                                    [this]() { DoPaste(); }, true, false, L"Ctrl+V");
        auto rename = MakeMenuAction(L"重命名", Icon::Edit,
                                     [this]() { DoRename(); }, hasSelection, false, L"F2");
        auto deleteItem = MakeMenuAction(L"删除", Icon::Delete,
                                         [this]() { DoDelete(false); }, hasSelection, false,
                                         L"Delete");
        auto deletePerm = MakeMenuAction(L"永久删除", Icon::Delete,
                                         [this]() { DoDelete(true); }, hasSelection, true,
                                         L"Shift+Delete");
        menu->AddRow({ copy, cut, paste, rename },
                     InlineRowOptions(82.0f, 180.0f));
        menu->AddRow({ deleteItem, deletePerm },
                     InlineRowOptions(100.0f, 180.0f));
        menu->AddSeparator();

        auto systemProperties = MakeMenuAction(
            L"系统属性面板", Icon::Settings, [this]() { DoProperties(); },
            hasSelection, false, L"Alt+Enter");
        auto editAttributes = MakeMenuAction(
            L"编辑文件属性…", Icon::Edit, [this]() { DoEditAttributes(); },
            singleSelection);
        menu->AddRow({ systemProperties, editAttributes },
                     InlineRowOptions(140.0f, 220.0f));
        menu->AddSeparator();

        auto pin = MakeMenuAction(
            pinned ? L"从快捷访问取消固定" : L"固定到快捷访问", Icon::Pin,
            [this, pinPath]() { ToggleQuickAccessPin(pinPath); }, singleSelection);
        auto copyPath = MakeMenuAction(L"复制路径", Icon::Link,
                                       [this]() { DoCopyPath(); }, hasSelection, false,
                                       L"Ctrl+Shift+C");
        auto nativeMenu = MakeMenuAction(
            L"系统右键菜单…", Icon::More, [this, nativeTarget]() {
                ShowNativeContextMenu(nativeTarget);
            }, !nativeTarget.empty());
        auto streams = MakeMenuAction(L"NTFS 文件流…", Icon::Attach,
                                      [this]() { DoManageStreams(); }, singleFile);
        auto unlock = MakeMenuAction(L"解除文件/文件夹占用…", Icon::Unlock,
                                     [this]() { DoUnlockFile(); }, hasSelection);
        menu->items.insert(menu->items.end(),
                           { pin, copyPath, nativeMenu, streams, unlock });
        return menu;
    });

    rightCol_->AddChild(listWidget);

    split_->SetFirst(tree_);
    split_->SetSecond(rightCol_);

    root_->AddChild(toolbar_);
    root_->AddChild(actionBar_);
    root_->AddChild(split_);

    // "此电脑"页面：默认首页，驱动器磁贴
    drivesList_ = std::make_shared<ColumnBox>();
    drivesList_->SetSpacing(10.0f);
    drivesList_->SetFillWidth(true);
    drivesScroll_ = std::make_shared<ScrollViewer>();
    drivesScroll_->SetContent(drivesList_);
    drivesScroll_->SetFillWidth(true);
    drivesScroll_->SetFillHeight(true);
    drivesScroll_->SetVisible(false);
    root_->AddChild(drivesScroll_);

    // 回收站是独立的数据页，不再用临时 Label 列表。TableView 提供真实的
    // 行命中、多选和稳定生命周期，右键直接走 Recycle Bin Shell 命名空间。
    recyclePage_ = std::make_shared<ColumnBox>();
    recyclePage_->SetSpacing(8.0f);
    recyclePage_->SetFillWidth(true);
    recyclePage_->SetFillHeight(true);
    recyclePage_->SetVisible(false);
    root_->AddChild(recyclePage_);

    auto recycleHeader = std::make_shared<RowBox>();
    recycleHeader->SetSpacing(10.0f);
    recycleHeader->SetHeight(30.0f);
    auto recycleTitle = std::make_shared<Label>(L"回收站");
    recycleTitle->SetFontSize(18.0f);
    recycleTitle->SetFillWidth(true);
    recycleHeader->AddChild(recycleTitle);
    recycleSummary_ = std::make_shared<Label>(L"");
    recycleSummary_->SetTextColor(
        ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
    recycleHeader->AddChild(recycleSummary_);
    recyclePage_->AddChild(recycleHeader);

    auto recycleActions = std::make_shared<RowBox>();
    recycleActions->SetSpacing(8.0f);
    recycleActions->SetHeight(32.0f);
    auto addRecycleAction = [&](const wchar_t* text,
                                std::function<void()> action,
                                bool danger = false) {
        auto button = std::make_shared<Button>(text);
        button->SetHeight(30.0f);
        button->SetFillWidth(true);
        if (danger) {
            const Color red =
                ThemeManager::AppTheme().Get(ThemeRole::Danger);
            button->SetColors(red, Color::Lerp(red, Color::FromArgb(255, 0, 0, 0), 0.10f),
                              Color::Lerp(red, Color::FromArgb(255, 0, 0, 0), 0.20f));
        }
        button->Connect(button->Clicked, std::move(action));
        recycleActions->AddChild(button);
    };
    addRecycleAction(L"还原选中项",
                     [this]() { InvokeSelectedRecycleVerb(L"undelete"); });
    addRecycleAction(L"删除选中项",
                     [this]() { InvokeSelectedRecycleVerb(L"delete"); },
                     true);
    addRecycleAction(L"刷新", [this]() { ShowRecycleBinPage(); });
    addRecycleAction(L"清空回收站", [this]() {
        MessageBox confirm(window_, L"清空回收站",
                           L"确定清空回收站吗？此操作不可恢复。",
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;
        std::wstring error;
        if (!EmptyRecycleBin(&error)) {
            Notify(L"清空回收站失败", error, NoticeKind::Error);
            return;
        }
        ShowHint(L"回收站已清空");
        ShowRecycleBinPage();
    }, true);
    addRecycleAction(L"系统回收站", []() { OpenRecycleBin(); });
    recyclePage_->AddChild(recycleActions);

    auto table = std::make_shared<RecycleTableView>();
    table->SetColumnCount(5);
    table->SetHorizontalHeaderLabels(
        { L"名称", L"原位置", L"删除日期", L"大小", L"类型" });
    table->SetColumnWidth(0, 240.0f);
    table->SetColumnWidth(1, 280.0f);
    table->SetColumnWidth(2, 190.0f);
    table->SetColumnWidth(3, 100.0f);
    table->SetColumnWidth(4, 180.0f);
    table->SetSelectionMode(TableView::SelectionMode::Row);
    table->SetMarqueeEnabled(true);
    table->SetCheckable(true);
    table->SetMarqueeCheckSync(true);
    table->SetFillWidth(true);
    table->SetFillHeight(true);
    table->SetCellLabelProvider(0, 5, [](int, int, Label&) {});
    table->OnNativeItemMenu = [this](int row) {
        ShowRecycleBinItemMenu(row);
        return true;
    };
    table->CellDoubleClicked.connect([this](int row, int) {
        auto selected = SelectedRecycleRows();
        if (std::find(selected.begin(), selected.end(), row) == selected.end()) {
            if (recycleTable_) recycleTable_->ClearSelection();
            if (recycleTable_) recycleTable_->SelectRow(row);
        }
        InvokeSelectedRecycleVerb(L"open");
    });
    table->SelectionChangedCells.connect([this](std::vector<std::pair<int, int>> cells) {
        if (cells.size() == 1 && recycleTable_)
            recycleTable_->SetRowChecked(cells.front().first, true);
        const size_t count = SelectedRecycleRows().size();
        uint64_t bytes = 0;
        for (int row : SelectedRecycleRows())
            if (row >= 0 && row < static_cast<int>(recycleItems_.size()))
                bytes += recycleItems_[static_cast<size_t>(row)].size;
        if (OnSelectionChanged)
            OnSelectionChanged(count, bytes);
        if (recycleSummary_) {
            recycleSummary_->SetText(
                count == 0 ? std::wstring()
                           : L"已选 " + std::to_wstring(count) + L" 项");
        }
    });
    recycleTable_ = table;
    recyclePage_->AddChild(table);

    RefreshQuickAccess();
    notificationDrawer_ = std::make_shared<NotificationDrawer>(root_);
    // The drawer's modal scrim is above the file list. Make the drawer itself
    // an OLE fallback target so dragging from Explorer still works while a
    // notification is visible.
    notificationDrawer_->Root()->SetDropTargetEnabled(true);
    notificationDrawer_->Root()->DragEnter.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    notificationDrawer_->Root()->DragOver.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    notificationDrawer_->Root()->Drop.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, true);
    });
}

// ---------------------------------------------------------------------------
// 地址栏
// ---------------------------------------------------------------------------
void ExplorerPane::FocusAddress() {
    FocusAddressEditor();
}

void ExplorerPane::BlurAddress() {
    BlurAddressEditor();
}

void ExplorerPane::FocusAddressEditor() {
    if (!addressBox_) return;
    if (window_) window_->FocusElement(addressBox_.get());
    else addressBox_->Focus();
}

void ExplorerPane::BlurAddressEditor() {
    if (!addressBox_) return;
    if (window_ && window_->GetFocusedElement() == addressBox_.get())
        window_->FocusElement(nullptr);
    else if (addressBox_->IsFocused())
        addressBox_->Blur();
}

void ExplorerPane::EnterAddressEditMode() {
    if (crumb_) crumb_->SetVisible(false);

    if (addressBox_) {
        // 进入编辑态：显示完整路径并全选
        addressBox_->SetText(
            currentPath_ == L"::thispc" ? L"此电脑" :
            currentPath_ == L"::recyclebin" ? L"回收站" :
            currentPath_);
        const int n = static_cast<int>(addressBox_->GetText().size());
        addressBox_->SetSelection(0, n);
    }
}

void ExplorerPane::LeaveAddressEditMode() {
    if (crumb_) {
        crumb_->SetVisible(true);
        crumb_->RequestRepaint();
    }
    RebuildBreadcrumb(currentPath_);
    if (addressHost_) addressHost_->RequestRepaint();
}

void ExplorerPane::CommitAddressInput() {
    if (!addressBox_) return;

    std::wstring input = addressBox_->GetText();
    const size_t first = input.find_first_not_of(L" \t\r\n\"");
    const size_t last = input.find_last_not_of(L" \t\r\n\"");
    if (first == std::wstring::npos) {
        BlurAddressEditor();
        return;
    }
    input = input.substr(first, last - first + 1);
    if (input.empty()) {
        BlurAddressEditor();
        return;
    }

    BlurAddressEditor();   // Enter 后先恢复面包屑；命令启动不会刷新当前目录

    if (_wcsicmp(input.c_str(), L"此电脑") == 0 || input == L"::thispc") {
        NavigateTo(L"::thispc");
        return;
    }
    if (_wcsicmp(input.c_str(), L"回收站") == 0 ||
        input == L"::recyclebin") {
        NavigateTo(L"::recyclebin");
        return;
    }

    const std::wstring expanded = ExpandEnvironmentVariables(input);
    const bool rooted =
        (expanded.size() >= 2 && expanded[1] == L':') ||
        (!expanded.empty() && (expanded.front() == L'\\' || expanded.front() == L'/')) ||
        expanded.rfind(L"\\\\", 0) == 0;

    std::wstring candidate = expanded;
    if (!rooted && currentPath_ != L"::thispc" && !currentPath_.empty())
        candidate = JoinPath(currentPath_, expanded);
    candidate = NormalizePath(candidate);

    if (PathExists(candidate)) {
        if (IsDirectoryPath(candidate)) {
            NavigateTo(candidate);
        } else {
            OpenFilePath(candidate);
        }
        return;
    }

    if (rooted) {
        Notify(L"找不到该位置", expanded, NoticeKind::Warning);
        return;
    }

    const std::wstring workingDirectory =
        currentPath_ == L"::thispc" ? std::wstring() : currentPath_;
    std::wstring launchError;
    if (!LaunchCommand(expanded, workingDirectory, &launchError)) {
        if (expanded.find(L'\\') != std::wstring::npos ||
            expanded.find(L'/') != std::wstring::npos) {
            Notify(L"找不到该位置",
                   expanded + L"\n启动尝试返回：" +
                       (launchError.empty() ? L"系统没有返回详细原因" : launchError),
                   NoticeKind::Warning);
        } else {
            Notify(L"无法跳转或启动",
                   L"输入：" + input + L"\n原因：" +
                       (launchError.empty() ? L"系统没有返回详细原因" : launchError),
                   NoticeKind::Warning);
        }
        return;
    }
    ShowHint(L"已启动：" + input);
    if (OnStatus) OnStatus(L"已启动：" + input);
}

void ExplorerPane::RebuildBreadcrumb(const std::wstring& path) {
    if (!crumbRow_) return;
    crumbRow_->ClearChildren();
    if (path.empty()) return;

    const Theme& th = ThemeManager::AppTheme();
    const Color surface = th.Get(ThemeRole::Surface);
    const Color hover = th.Get(ThemeRole::ControlHover);
    const Color pressed = th.Get(ThemeRole::ControlPressed);
    const Color textColor = th.Get(ThemeRole::Text);
    const Color secondary = th.Get(ThemeRole::TextSecondary);

    // 从根到当前目录。
    std::vector<std::wstring> chain;
    if (path == L"::thispc" || path == L"::recyclebin") {
        chain.push_back(path);
    } else {
        for (std::wstring cur = path;;) {
            chain.push_back(cur);
            const std::wstring parent = ParentPath(cur);
            if (parent == cur) break;
            cur = parent;
        }
        std::reverse(chain.begin(), chain.end());
    }

    struct CrumbNode {
        std::wstring text;
        std::wstring target;
        bool current = false;
    };
    std::vector<CrumbNode> nodes;
    nodes.reserve(chain.size());
    for (size_t i = 0; i < chain.size(); ++i) {
        CrumbNode node;
        node.target = chain[i];
        node.current = (i + 1 == chain.size());
        if (path == L"::thispc") node.text = L"此电脑";
        else if (path == L"::recyclebin") node.text = L"回收站";
        else node.text = (i == 0) ? chain[i]
                                  : fs::DirectoryModel::DisplayNameOf(chain[i]);
        nodes.push_back(std::move(node));
    }

    auto makeSeparator = [&]() {
        auto sep = std::make_shared<Label>(L"\u203A");
        sep->SetTextColor(secondary);
        sep->SetAlignment(Label::HAlign::Center, Label::VAlign::Center);
        sep->SetWidth(12.0f);
        sep->SetHeight(24.0f);
        return sep;
    };

    auto makeSegment = [&](const CrumbNode& node, float widthBudget) {
        constexpr float kPadX = 8.0f;        // 左右内边距
        constexpr float kSlack = 4.0f;       // 余量，避免边界处被二次省略
        auto b = std::make_shared<Button>(node.text);
        const FontSpec spec = b->GetLabel()->GetEffectiveFontSpec();
        const float maxButtonWidth =
            max(32.0f, min(widthBudget, kCrumbSegMaxWidth + kPadX * 2.0f + kSlack));
        const float textCap = max(4.0f, maxButtonWidth - kPadX * 2.0f - kSlack);
        const std::wstring shown = ElideMiddle(node.text, spec, textCap);
        b->SetText(shown);
        const float measuredW =
            b->GetLabel()->Measure(Size(FLT_MAX, FLT_MAX)).width;
        const float textW = min(
            max(MeasureTextWidth(shown, spec),
                max(measuredW, shown.size() * max(4.0f, spec.size * 0.4f))),
            textCap);
        b->SetPadding(kPadX);
        b->SetWidth(min(maxButtonWidth, textW + kPadX * 2.0f + kSlack));
        b->SetHeight(26.0f);
        b->SetCornerRadius(4.0f);
        b->SetColors(node.current ? hover : surface, hover, pressed);
        b->SetTextColor(textColor);
        if (node.current) {
            b->SetToolTip(L"点击编辑地址");
            b->Connect(b->Clicked, [this]() { FocusAddressEditor(); });
        } else {
            const std::wstring target = node.target;
            b->Connect(b->Clicked, [this, target]() { NavigateTo(target); });
        }
        return b;
    };

    auto pc = std::make_shared<Label>(L"");
    pc->SetIcon(Icon::Home, 14.0f);
    pc->SetWidth(26.0f);
    pc->SetHeight(24.0f);
    pc->SetAlignment(Label::HAlign::Center, Label::VAlign::Center);
    crumbRow_->AddChild(pc);

    constexpr float kHomeWidth = 26.0f;
    constexpr float kSeparatorWidth = 12.0f;
    constexpr float kSpacing = 2.0f;
    constexpr float kEllipsisWidth = 30.0f;
    float available = 0.0f;
    const float toolbarWidth = toolbar_->GetArrangedRect().width;
    if (toolbarWidth > 10.0f) {
        const float fixedWidth = taskBtn_->GetWidth() + backBtn_->GetWidth() +
                                 forwardBtn_->GetWidth() + upBtn_->GetWidth() +
                                 toolbar_->GetSpacing() * 4.0f;
        available = max(0.0f, toolbarWidth - fixedWidth - 8.0f);
    } else if (addressHost_->GetArrangedRect().width > 10.0f) {
        available = addressHost_->GetArrangedRect().width - 8.0f;
    } else if (crumb_->GetArrangedRect().width > 10.0f) {
        available = crumb_->GetArrangedRect().width - 6.0f;
    } else if (crumbRow_->GetArrangedRect().width > 10.0f) {
        available = crumbRow_->GetArrangedRect().width;
    } else {
        available = 320.0f;
    }
    available = max(0.0f, available - kSpacing);   // 给右侧隐形编辑区保留格间距

    std::vector<std::shared_ptr<Button>> fullSegments;
    fullSegments.reserve(nodes.size());
    std::vector<float> fullWidths = { kHomeWidth, kSeparatorWidth };
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto segment = makeSegment(nodes[i], kCrumbSegMaxWidth + 20.0f);
        fullWidths.push_back(segment->GetWidth());
        if (i + 1 < nodes.size()) fullWidths.push_back(kSeparatorWidth);
        fullSegments.push_back(std::move(segment));
    }
    float fullWidth = kSpacing * (float)(fullWidths.size() - 1);
    for (float width : fullWidths) fullWidth += width;

    crumbRow_->AddChild(makeSeparator());
    if (fullWidth <= available || nodes.size() == 1) {
        for (size_t i = 0; i < fullSegments.size(); ++i) {
            crumbRow_->AddChild(fullSegments[i]);
            if (i + 1 < fullSegments.size()) crumbRow_->AddChild(makeSeparator());
        }
    } else {
        // 根盘符固定在左侧。
        const float rootBudget = max(
            40.0f, min(kCrumbSegMaxWidth + 20.0f,
                       available - kHomeWidth - kSeparatorWidth * 2.0f -
                           kEllipsisWidth - kSpacing * 6.0f - 28.0f));
        auto root = makeSegment(nodes.front(), rootBudget);
        crumbRow_->AddChild(root);
        crumbRow_->AddChild(makeSeparator());

        auto ellipsis = std::make_shared<Button>(L"...");
        ellipsis->SetWidth(kEllipsisWidth);
        ellipsis->SetHeight(26.0f);
        ellipsis->SetCornerRadius(4.0f);
        ellipsis->SetColors(surface, hover, pressed);
        ellipsis->SetTextColor(textColor);
        ellipsis->SetToolTip(L"显示完整路径");
        ellipsis->Connect(ellipsis->Clicked, [this, nodes]() {
            auto menu = std::make_shared<Menu>();
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto& node = nodes[i];
                auto item = Menu::MakeItem(node.text, [this, target = node.target]() {
                    NavigateTo(target);
                });
                item->SetGlyphIcon(i == 0 ? Icon::Home : Icon::Folder, 16.0f);
                item->isDefault = node.current;
                menu->items.push_back(std::move(item));
            }
            menu->ShowAtCursor();
        });
        crumbRow_->AddChild(ellipsis);

        float used = kHomeWidth + kSeparatorWidth + root->GetWidth() +
                     kSeparatorWidth + kEllipsisWidth + kSpacing * 4.0f;
        std::vector<size_t> tailIndices;
        for (size_t i = nodes.size(); i-- > 1;) {
            auto candidate = makeSegment(nodes[i], kCrumbSegMaxWidth + 20.0f);
            const float needed =
                kSeparatorWidth + candidate->GetWidth() + kSpacing * 2.0f;
            if (used + needed <= available) {
                used += needed;
                tailIndices.push_back(i);
                continue;
            }
            if (tailIndices.empty()) {
                const float budget =
                    available - used - kSeparatorWidth - kSpacing * 2.0f;
                if (budget >= 28.0f) {
                    crumbRow_->AddChild(makeSeparator());
                    crumbRow_->AddChild(makeSegment(nodes[i], budget));
                }
            }
            break;
        }
        std::reverse(tailIndices.begin(), tailIndices.end());
        for (size_t index : tailIndices) {
            crumbRow_->AddChild(makeSeparator());
            crumbRow_->AddChild(makeSegment(nodes[index], kCrumbSegMaxWidth + 20.0f));
        }
    }

    // 右侧空白：点一下进入编辑态（全选）
    auto blank = std::make_shared<Button>(L"");
    blank->SetHeight(24.0f);
    blank->SetFillWidth(true);
    blank->SetColors(surface, surface, surface);   // 视觉上不可见
    blank->Connect(blank->Clicked, [this]() { FocusAddressEditor(); });
    crumbRow_->AddChild(blank);
}

void ExplorerPane::ScheduleBreadcrumbRebuild() {
    if (!window_ || currentPath_.empty()) return;
    if (!crumbRebuildTimer_) {
        crumbRebuildTimer_ = window_->CreateTimer(40);
        crumbRebuildTimer_->Tick.connect([this]() {
            if (crumbRebuildTimer_) crumbRebuildTimer_->Stop();
            if (addressBox_ && addressBox_->IsFocused()) return;
            RebuildBreadcrumb(currentPath_);
        });
    }
    crumbRebuildTimer_->Stop();
    crumbRebuildTimer_->Start(40);
}

// ---------------------------------------------------------------------------
// 树
// ---------------------------------------------------------------------------
void ExplorerPane::LoadTreeNode(const std::shared_ptr<TreeNode>& node) {
    if (!node) return;
    const std::wstring path = node->tooltip;
    if (path.empty()) return;

    tree_->BeginBatch();
    tree_->RemoveChildren(node);

    size_t added = 0;
    const auto snapshot = fs::DirectoryModel::Load(
        path, LoadSettings().showHiddenFiles);
    if (snapshot.Ok()) {
        for (const auto& e : snapshot.entries) {
            if (!e.isDir) continue;
            auto child = tree_->AddChild(node, e.name);
            if (!child) continue;
            child->tooltip = e.fullPath;
            // 先乐观地认为它有子目录（让箭头出现）；真加载出空时再改回 false
            tree_->SetNodeHasChildren(child, true);
            ++added;
        }
    }
    // 空目录：去掉箭头，免得点开一场空
    if (added == 0) tree_->SetNodeHasChildren(node, false);
    tree_->EndBatch();
}

void ExplorerPane::RefreshTreeVisibility() {
    if (!tree_) return;
    suppressTreeSync_ = true;
    for (const auto& root : roots_) {
        tree_->RemoveChildren(root);
        tree_->SetNodeHasChildren(root, true);
        if (root && root->expanded) LoadTreeNode(root);
    }
    suppressTreeSync_ = false;
    if (!currentPath_.empty() &&
        currentPath_.rfind(L"::", 0) != 0) {
        SyncTreeToPath(currentPath_);
    }
}

void ExplorerPane::BuildDriveTree() {
    const auto drives = LogicalDrives();
    std::shared_ptr<TreeNode> firstRoot;

    for (const auto& d : drives) {
        auto node = tree_->AddRoot(d.display);
        node->tooltip = d.root;
        tree_->SetNodeHasChildren(node, true);
        roots_.push_back(node);
        if (!firstRoot) firstRoot = node;
    }
    treeRootCount_ = drives.size();
    firstRoot_ = firstRoot;

    if (firstRoot) tree_->ExpandNode(firstRoot, true);
}

void ExplorerPane::RefreshQuickAccess() {
    if (!quickList_ || !leftStack_ || !split_ || !tree_) return;

    quickEntries_.clear();
    const auto folders = UserFolders();
    const auto stored = LoadQuickAccessPins();
    quickPins_.clear();
    std::vector<std::wstring> seen;
    for (const auto& raw : stored) {
        if (!PathExists(raw)) continue;
        const std::wstring path = NormalizePath(StripExtendedPath(raw));
        const bool duplicate = std::any_of(
            seen.begin(), seen.end(), [&](const std::wstring& item) {
                return _wcsicmp(item.c_str(), path.c_str()) == 0;
            });
        if (duplicate) continue;
        quickPins_.push_back(path);
        seen.push_back(path);

        std::wstring title = IsRootPath(path) ? path : FileNameOf(path);
        for (const auto& folder : folders) {
            if (_wcsicmp(folder.path.c_str(), path.c_str()) == 0) {
                title = folder.title;
                break;
            }
        }
        quickEntries_.push_back({ title, path, true, false });
    }
    if (quickPins_.size() != stored.size())
        SaveQuickAccessPins(quickPins_, nullptr);

    quickEntries_.push_back({ L"此电脑", L"::thispc", false, true });
    for (const auto& folder : folders) {
        const bool duplicate = std::any_of(
            seen.begin(), seen.end(), [&](const std::wstring& path) {
                return _wcsicmp(path.c_str(), folder.path.c_str()) == 0;
            });
        if (duplicate) continue;
        quickEntries_.push_back({ folder.title, folder.path, false, true });
        seen.push_back(folder.path);
    }
    quickEntries_.push_back({ L"回收站", L"::recyclebin", false, true });

    auto snapshot = std::make_shared<const std::vector<QuickAccessEntry>>(quickEntries_);
    quickList_->SetItemLabelProvider(
        static_cast<int>(snapshot->size()),
        [this, snapshot](int index, Label& label) {
            if (index < 0 || index >= static_cast<int>(snapshot->size())) return;
            const auto& entry = (*snapshot)[static_cast<size_t>(index)];
            label.SetTextFast(entry.title);
            label.SetTextOverflow(Label::TextOverflow::Ellipsis);
            label.SetToolTip(entry.path);
            label.SetImage(nullptr);
            if (auto icon = entry.path == L"::recyclebin"
                                ? nullptr
                                : TreeNodeIcon(entry.path)) {
                label.SetImage(icon);
                label.SetIconSize(16.0f, 16.0f);
                label.SetIconSpacing(6.0f);
            } else {
                label.SetIcon(entry.path == L"::recyclebin" ? Icon::Delete : Icon::Folder,
                              16.0f);
            }
        });

    const bool hasItems = !quickEntries_.empty();
    quickPane_->SetVisible(hasItems);
    if (!hasItems) {
        if (split_->GetFirst() != tree_) {
            leftStack_->SetSecond(nullptr);
            split_->SetFirst(tree_);
        }
        return;
    }

    if (split_->GetFirst() != leftStack_) {
        split_->SetFirst(nullptr);
        leftStack_->SetSecond(tree_);
        split_->SetFirst(leftStack_);
    }
    if (leftStack_->GetSplitRatio() > 0.5f)
        leftStack_->SetSplitRatio(0.5f);
}

void ExplorerPane::ToggleQuickAccessPin(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring error;
    const bool pinned = IsQuickAccessPinned(path);
    const bool ok = pinned ? UnpinQuickAccess(path, &error)
                           : PinQuickAccess(path, &error);
    if (!ok) {
        Notify(L"快捷访问操作失败", error, NoticeKind::Error);
        return;
    }
    RefreshQuickAccess();
    ShowHint(pinned ? L"已从快捷访问取消固定" : L"已固定到快捷访问");
}

void ExplorerPane::OpenQuickAccessItem(int index, bool openTarget) {
    if (index < 0 || index >= static_cast<int>(quickEntries_.size())) return;
    const auto& entry = quickEntries_[static_cast<size_t>(index)];
    const std::wstring path = entry.path;
    if (path == L"::recyclebin") {
        NavigateTo(openTarget ? L"::recyclebin" : L"::thispc");
        return;
    }
    if (path == L"::thispc") {
        NavigateTo(L"::thispc");
        return;
    }
    if (!PathExists(path)) {
        RefreshQuickAccess();
        return;
    }

    if (openTarget) {
        if (IsDirectoryPath(path)) NavigateTo(path);
        else OpenFilePath(path);
        return;
    }

    const std::wstring parent = ParentPath(path);
    NavigateTo(IsRootPath(path) ? L"::thispc" : parent);
}

void ExplorerPane::LaunchNewWindowHere() {
    const std::wstring path = currentPath_ == L"::thispc" ? L"" : currentPath_;
    if (!app::TaskClient::OpenNewWindow(path))
        Notify(L"无法打开新窗口", L"后台进程请求入队失败。", NoticeKind::Error);
    else
        Notify(L"已请求打开新窗口", path, NoticeKind::Info);
}

void ExplorerPane::ShowNativeContextMenu(const std::wstring& path) {
    if (!window_) return;
    const std::wstring target =
        path.empty() && currentPath_ != L"::thispc" ? currentPath_ : path;
    if (target.empty() || !ShowShellContextMenu(window_->GetHwnd(), target)) {
        Notify(L"无法打开系统右键菜单", target, NoticeKind::Error);
    }
}

std::shared_ptr<Menu> ExplorerPane::BuildTargetContextMenu(
    const std::wstring& path) {
    auto menu = std::make_shared<Menu>();
    const bool exists = PathExists(path);
    const bool isDir = IsDirectoryPath(path);
    const bool pinned = IsQuickAccessPinned(path);

    auto open = MakeMenuAction(L"打开", Icon::Open, [this, path, isDir]() {
        if (isDir) NavigateTo(path);
        else OpenFilePath(path);
    }, exists);
    open->isDefault = true;
    auto openWith = MakeMenuAction(L"打开方式…", Icon::More, [this, path, isDir]() {
        if (isDir) return;
        std::wstring error;
        if (!OpenWith(path, &error)) {
            Notify(L"打开方式",
                   L"目标：" + path + L"\n原因：" +
                       (error.empty() ? L"系统没有完成打开方式选择" : error),
                   NoticeKind::Error);
        }
    }, exists && !isDir);
    auto openLocation = MakeMenuAction(L"打开文件位置", Icon::Folder, [this, path]() {
        std::wstring error;
        if (!OpenContainingFolder(path, &error)) {
            Notify(L"无法打开文件位置",
                   L"目标：" + path + L"\n原因：" +
                       (error.empty() ? L"系统没有返回详细原因" : error),
                   NoticeKind::Error);
        }
    }, exists);
    std::vector<std::shared_ptr<MenuItem>> openRow =
        { open, openWith, openLocation };
    std::vector<std::shared_ptr<MenuItem>> shortcutLocationRow;
    if (IsShortcutPath(path)) {
        ShortcutInfo shortcut;
        if (ResolveShortcut(path, &shortcut, nullptr) &&
            !shortcut.targetPath.empty() &&
            (shortcut.kind == ShortcutTargetKind::File ||
             shortcut.kind == ShortcutTargetKind::Directory)) {
            const std::wstring target = shortcut.targetPath;
            const std::wstring label =
                shortcut.kind == ShortcutTargetKind::Directory
                    ? L"打开目标文件夹"
                    : L"打开目标位置";
            shortcutLocationRow.push_back(MakeMenuAction(
                label, Icon::Link,
                [this, target]() { OpenLocationInZExplorer(target); }, true));
        }
    }
    menu->AddRow(std::move(openRow), InlineRowOptions(112.0f, 220.0f));
    for (const auto& action : shortcutLocationRow)
        menu->AddRow({ action }, InlineRowOptions(220.0f, 320.0f));
    menu->AddSeparator();

    auto copy = MakeMenuAction(L"复制", Icon::Copy, [this, path]() {
        if (!app::TaskClient::Submit(L"copy", L"", { path }))
            Notify(L"任务服务不可用", L"ZExplorer 后台进程没有响应。", NoticeKind::Error);
        else ShowHint(L"已复制 1 项");
    }, exists);
    auto cut = MakeMenuAction(L"剪切", Icon::Cut, [this, path]() {
        if (!app::TaskClient::Submit(L"cut", L"", { path }))
            Notify(L"任务服务不可用", L"ZExplorer 后台进程没有响应。", NoticeKind::Error);
        else ShowHint(L"已剪切 1 项");
    }, exists);
    auto rename = MakeMenuAction(L"重命名", Icon::Edit, [this, path]() {
        std::wstring name = FileNameOf(path);
        if (!PromptName(L"重命名", name, &name) || name == FileNameOf(path)) return;
        const std::wstring destination = JoinPath(ParentPath(path), name);
        if (!app::TaskClient::Submit(L"rename", L"", { path, destination }))
            Notify(L"重命名失败", L"后台进程不可用", NoticeKind::Error);
        else ScheduleRefresh();
    }, exists, false, L"F2");
    auto deleteItem = MakeMenuAction(L"删除", Icon::Delete, [this, path]() {
        MessageBox confirm(window_, L"删除",
                           L"确定把「" + FileNameOf(path) + L"」移到回收站吗？",
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;
        if (!app::TaskClient::Submit(L"delete", L"", { path }))
            Notify(L"删除失败", L"后台进程不可用", NoticeKind::Error);
        else ScheduleRefresh();
    }, exists);
    auto deletePerm = MakeMenuAction(L"永久删除", Icon::Delete, [this, path]() {
        MessageBox confirm(window_, L"永久删除",
                           L"确定永久删除「" + FileNameOf(path) + L"」吗？",
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;
        if (!app::TaskClient::Submit(L"deleteperm", L"", { path }))
            Notify(L"永久删除失败", L"后台进程不可用", NoticeKind::Error);
        else ScheduleRefresh();
    }, exists, true);
    menu->AddRow({ copy, cut, rename }, InlineRowOptions(82.0f, 180.0f));
    menu->AddRow({ deleteItem, deletePerm },
                 InlineRowOptions(100.0f, 180.0f));
    menu->AddSeparator();

    auto systemProperties = MakeMenuAction(L"系统属性面板", Icon::Settings, [this, path]() {
        std::wstring error;
        if (!ShowProperties(path, &error)) {
            Notify(L"无法打开系统属性",
                   L"目标：" + path + L"\n原因：" +
                       (error.empty() ? L"系统没有返回详细原因" : error),
                   NoticeKind::Error);
        }
    }, exists);
    auto editAttributes = MakeMenuAction(
        L"编辑文件属性…", Icon::Edit,
        [this, path]() { DoEditAttributesForPath(path); }, exists);
    menu->AddRow({ systemProperties, editAttributes },
                 InlineRowOptions(140.0f, 220.0f));
    menu->AddSeparator();

    auto pin = MakeMenuAction(
        pinned ? L"从快捷访问取消固定" : L"固定到快捷访问", Icon::Pin,
        [this, path]() { ToggleQuickAccessPin(path); }, exists);
    auto copyPath = MakeMenuAction(
        L"复制路径", Icon::Link,
        [path]() { SetClipboardText(path, nullptr); }, exists, false, L"Ctrl+Shift+C");
    auto nativeMenu = MakeMenuAction(
        L"系统右键菜单…", Icon::More,
        [this, path]() { ShowNativeContextMenu(path); }, exists);
    auto streams = MakeMenuAction(
        L"NTFS 文件流…", Icon::Attach,
        [this, path]() { DoManageStreamsForPath(path); }, exists && !isDir);
    auto unlock = MakeMenuAction(
        L"解除文件/文件夹占用…", Icon::Unlock,
        [this, path]() { DoUnlockFileForPath(path); }, exists);
    menu->items.insert(menu->items.end(),
                       { pin, copyPath, nativeMenu, streams, unlock });
    return menu;
}

// 让左树跟上当前目录：展开到该深度 → 选中 → 滚到可见。
void ExplorerPane::SyncTreeToPath(const std::wstring& path) {
    if (!tree_ || path.empty()) return;

    const std::wstring norm = NormalizePath(path);

    // 1) 找包含该路径的根（最长前缀），例如盘符根目录。
    //    注意：注释结尾不要留反斜杠 —— C++ 的行拼接会把它后面那行一起注释掉。
    std::shared_ptr<TreeNode> node;
    size_t bestLen = 0;
    for (const auto& r : roots_) {
        const std::wstring& rp = r->tooltip;
        if (rp.empty() || rp.size() > norm.size()) continue;
        if (_wcsnicmp(norm.c_str(), rp.c_str(), rp.size()) != 0) continue;
        if (rp.size() > bestLen) { node = r; bestLen = rp.size(); }
    }
    if (!node) return;

    // 2) 逐级下钻（必要时展开，展开会触发懒加载把子节点填进来）
    std::wstring cur = NormalizePath(node->tooltip);
    while (cur.size() < norm.size()) {
        std::wstring rest = norm.substr(cur.size());
        while (!rest.empty() && (rest[0] == L'\\' || rest[0] == L'/')) rest.erase(0, 1);
        if (rest.empty()) break;

        const size_t slash = rest.find_first_of(L"\\/");
        const std::wstring seg = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);

        if (!node->expanded) tree_->ExpandNode(node, true);   // 懒加载在这里发生

        std::shared_ptr<TreeNode> next;
        for (const auto& c : node->children) {
            if (_wcsicmp(NodeText(c).c_str(), seg.c_str()) == 0) { next = c; break; }
        }
        if (!next) break;   // 该层没这个节点（被过滤/尚未列出），到此为止

        node = next;
        cur = node->tooltip.empty() ? JoinPath(cur, seg) : NormalizePath(node->tooltip);
    }

    // 3) 选中 + 滚到可见（期间抑制 SelectionChanged 回灌）
    suppressTreeSync_ = true;
    tree_->SetSelectedNode(node);
    ScrollTreeToNode(node);
    suppressTreeSync_ = false;

    // 布局/可见列表可能这一拍还没稳定，40ms 后再对一次
    pendingScrollNode_ = node;
    if (auto* w = tree_->GetWindow()) {
        if (!syncScrollTimer_) {
            syncScrollTimer_ = w->CreateTimer(40);
            syncScrollTimer_->Tick.connect([this]() {
                if (syncScrollTimer_) syncScrollTimer_->Stop();
                if (tree_ && pendingScrollNode_) {
                    suppressTreeSync_ = true;
                    tree_->SetSelectedNode(pendingScrollNode_);
                    ScrollTreeToNode(pendingScrollNode_);
                    suppressTreeSync_ = false;
                }
            });
        }
        syncScrollTimer_->Start(40);
    }
}

// 自己算索引再设偏移：TreeView::ScrollToNode 只设平滑滚动目标，
// 在"刚插入大量子节点、maxScrollY_ 还没更新"的时序下会被 clamp 截小。
void ExplorerPane::ScrollTreeToNode(const std::shared_ptr<TreeNode>& node) {
    if (!tree_ || !node) return;

    const auto nodes = tree_->GetVisibleNodes();
    int idx = -1;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (nodes[static_cast<size_t>(i)].get() == node.get()) { idx = i; break; }
    }
    if (idx < 0) return;

    const float rowH = 26.0f;                       // 与 SetRowHeight 一致
    const float viewport = tree_->GetArrangedRect().height;
    if (viewport <= rowH) return;

    const float top = static_cast<float>(idx) * rowH;
    const float bottom = top + rowH;
    const float visibleTop = tree_->GetScrollOffsetY();
    const float visibleBottom = visibleTop + viewport;

    // 目标已经完整处于视口内时保持现有滚动位置。只有在上方/下方不可见时，
    // 才做最小幅度滚动，避免每次同步都把它硬拽到顶部或底部。
    if (top >= visibleTop && bottom <= visibleBottom) return;

    const float target = (top < visibleTop) ? top : (bottom - viewport);
    tree_->SetScrollOffsetY(max(0.0f, target));
}

size_t ExplorerPane::TreeNodeCount() const {
    if (!firstRoot_) return 0;
    size_t n = 0;
    std::function<void(const std::shared_ptr<TreeNode>&)> walk =
        [&](const std::shared_ptr<TreeNode>& node) {
            if (!node) return;
            ++n;
            for (const auto& c : node->children) walk(c);
        };
    walk(firstRoot_);
    return n;
}

std::wstring ExplorerPane::TreeDiagnostics() const {
    if (!firstRoot_) return L"no root";
    std::wstring selected = L"<none>";
    if (tree_) {
        if (auto sel = tree_->GetSelectedNode()) {
            selected = NodeText(sel);
            if (selected.empty()) selected = L"<empty>";
        }
    }
    wchar_t buf[320];
    swprintf_s(buf, L"root children=%zu expanded=%d nodes=%zu selected=[%ls]",
               firstRoot_->children.size(), firstRoot_->expanded ? 1 : 0, TreeNodeCount(),
               selected.c_str());
    return buf;
}

std::wstring ExplorerPane::AddressDiagnostics() const {
    wchar_t buf[256];
    swprintf_s(buf, L"crumb visible=%d, edit focused=%d, segments=%d",
               crumb_ && crumb_->IsVisible() ? 1 : 0,
               addressBox_ && addressBox_->IsFocused() ? 1 : 0,
               crumbRow_ ? static_cast<int>(crumbRow_->GetChildren().size()) : 0);
    return buf;
}

void ExplorerPane::HideDrivesPage() {
    if (!showingDrives_ && !showingRecycleBin_) return;
    showingDrives_ = false;
    showingRecycleBin_ = false;
    if (drivesScroll_) drivesScroll_->SetVisible(false);
    if (recyclePage_) recyclePage_->SetVisible(false);
    if (split_) split_->SetVisible(true);
}

void ExplorerPane::ShowDrivesPage() {
    showingDrives_ = true;
    showingRecycleBin_ = false;
    if (split_) split_->SetVisible(false);
    if (recyclePage_) recyclePage_->SetVisible(false);
    if (!drivesScroll_ || !drivesList_) return;
    if (searchBox_) {
        searchBox_->SetPlaceholder(L"此电脑不支持递归搜索");
        searchBox_->SetEnabled(false);
    }
    drivesScroll_->SetVisible(true);

    drivesList_->ClearChildren();
    if (OnSelectionChanged) OnSelectionChanged(0, 0);

    const Theme& homeTheme = ThemeManager::AppTheme();
    const float clientWidth =
        window_ ? window_->GetClientWidthDip() : 1000.0f;
    const int columns = clientWidth >= 900.0f ? 3 : 2;
    drivesColumns_ = columns;
    const float cardWidth = std::clamp(
        (clientWidth - 80.0f) / static_cast<float>(columns) - 10.0f,
        180.0f, 280.0f);

    auto styleCard = [&](const std::shared_ptr<ClickableCard>& card) {
        card->SetWidth(cardWidth);
        card->SetMaxWidth(280.0f);
        card->SetHeight(82.0f);
        card->SetFillWidth(false);
        card->SetPadding(10.0f);
        card->SetCornerRadius(10.0f);
        card->SetContentAlignment(
            LayoutAlignment::Stretch, LayoutAlignment::Center);
        card->SetBackgroundColor(homeTheme.Get(ThemeRole::Surface));
        card->SetBorderColor(homeTheme.Get(ThemeRole::Border));
        card->SetHoverBackgroundColor(BlendOver(
            homeTheme.Get(ThemeRole::Selection),
            homeTheme.Get(ThemeRole::Surface)));
        card->SetHoverBorderColor(homeTheme.Get(ThemeRole::Accent));
        card->SetHoverAnimationSpeed(14.0f);
    };

    auto makeLocationCard = [this, &styleCard, &homeTheme, cardWidth](
                                const std::wstring& title,
                                const std::wstring& path,
                                bool recycle) {
        auto body = std::make_shared<ColumnBox>();
        body->SetSpacing(0.0f);
        body->SetFillWidth(true);
        auto row = std::make_shared<RowBox>();
        row->SetSpacing(10.0f);
        row->SetFillWidth(true);
        auto iconLabel = std::make_shared<Label>(L"");
        iconLabel->SetWidth(36.0f);
        iconLabel->SetHeight(36.0f);
        iconLabel->SetAlignment(Label::HAlign::Center, Label::VAlign::Center);
        if (recycle) {
            iconLabel->SetIcon(Icon::Delete, 24.0f);
        } else if (HICON h = GetShellIconForPath(path, true)) {
            if (auto icon = Image::FromHICON(h)) {
                iconLabel->SetImage(icon);
                iconLabel->SetIconSize(26.0f, 26.0f);
            }
            DestroyIcon(h);
        }
        row->AddChild(iconLabel);
        auto textCol = std::make_shared<ColumnBox>();
        textCol->SetSpacing(2.0f);
        textCol->SetFillWidth(true);
        textCol->SetFillHeight(true);
        auto titleLabel = std::make_shared<Label>(title);
        titleLabel->SetFillWidth(true);
        titleLabel->SetHeight(26.0f);
        titleLabel->SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
        titleLabel->SetFontSize(16.0f);
        titleLabel->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD);
        textCol->AddChild(titleLabel);
        const float pathWidth = (std::max)(60.0f, cardWidth - 86.0f);
        std::wstring locationText = recycle ? L"查看和清空" : path;
        if (!recycle) {
            locationText = ElideMiddle(
                locationText, homeTheme.font, pathWidth);
        }
        auto location = std::make_shared<Label>(locationText);
        location->SetFillWidth(true);
        location->SetHeight(20.0f);
        location->SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
        location->SetTextOverflow(Label::TextOverflow::Ellipsis);
        location->SetTextColor(homeTheme.Get(ThemeRole::TextSecondary));
        textCol->AddChild(location);
        row->AddChild(textCol);
        body->AddChild(row);

        auto card = std::make_shared<ClickableCard>();
        styleCard(card);
        card->SetContent(body);
        card->SetToolTip(recycle ? L"回收站" : path);
        card->OnClick = [this, path, recycle]() {
            if (recycle) {
                NavigateTo(L"::recyclebin");
            } else if (IsDirectoryPath(path)) {
                NavigateTo(path);
            } else {
                OpenFilePath(path);
            }
        };
        return card;
    };

    auto commonGrid = std::make_shared<GridLayout>();
    commonGrid->SetColumnCount(columns);
    commonGrid->SetSpacing(10.0f, 10.0f);
    for (int c = 0; c < columns; ++c)
        commonGrid->SetColumnStretch(c, 1.0f);

    std::vector<std::shared_ptr<UIElement>> commonCards;
    std::vector<std::wstring> seenHomePaths;
    const auto homeFolders = UserFolders();
    for (const auto& raw : LoadQuickAccessPins()) {
        if (!PathExists(raw)) continue;
        const std::wstring path = NormalizePath(StripExtendedPath(raw));
        if (std::any_of(seenHomePaths.begin(), seenHomePaths.end(),
                        [&](const std::wstring& item) {
                            return _wcsicmp(item.c_str(), path.c_str()) == 0;
                        })) {
            continue;
        }
        std::wstring title = IsRootPath(path) ? path : FileNameOf(path);
        for (const auto& folder : homeFolders) {
            if (_wcsicmp(folder.path.c_str(), path.c_str()) == 0) {
                title = folder.title;
                break;
            }
        }
        seenHomePaths.push_back(path);
        commonCards.push_back(makeLocationCard(title, path, false));
    }
    for (const auto& folder : homeFolders) {
        const bool duplicate =
            std::any_of(seenHomePaths.begin(), seenHomePaths.end(),
                        [&](const std::wstring& path) {
                            return _wcsicmp(path.c_str(),
                                            folder.path.c_str()) == 0;
                        });
        if (duplicate) continue;
        seenHomePaths.push_back(folder.path);
        commonCards.push_back(
            makeLocationCard(folder.title, folder.path, false));
    }
    commonCards.push_back(
        makeLocationCard(L"回收站", L"::recyclebin", true));

    const int commonRows = static_cast<int>(
        (commonCards.size() + columns - 1) / columns);
    commonGrid->SetRowCount(commonRows);
    for (int i = 0; i < static_cast<int>(commonCards.size()); ++i) {
        commonGrid->AddChild(
            commonCards[static_cast<size_t>(i)],
            i / columns, i % columns, 1, 1,
            GridLayout::Alignment::Center,
            GridLayout::Alignment::Center);
    }

    auto commonScroll = std::make_shared<ScrollViewer>();
    commonScroll->SetContent(commonGrid);
    commonScroll->SetHeight(
        (std::min)(230.0f, 20.0f + commonRows * 94.0f));
    commonScroll->SetMaxHeight(230.0f);
    commonScroll->SetFillWidth(true);
    auto commonExpander = std::make_shared<Expander>(L"常用位置");
    commonExpander->SetExpanded(true);
    commonExpander->SetContent(commonScroll);
    commonExpander->SetMaxHeight(320.0f);
    drivesList_->AddChild(commonExpander);

    auto drivesTitle = std::make_shared<Label>(L"驱动器和设备");
    drivesTitle->SetHeight(26.0f);
    drivesTitle->SetTextColor(homeTheme.Get(ThemeRole::TextSecondary));
    drivesList_->AddChild(drivesTitle);

    auto driveGrid = std::make_shared<GridLayout>();
    driveGrid->SetColumnCount(columns);
    driveGrid->SetSpacing(10.0f, 10.0f);
    for (int c = 0; c < columns; ++c)
        driveGrid->SetColumnStretch(c, 1.0f);

    const auto drives = LogicalDrives();
    const int driveRows = static_cast<int>(
        (drives.size() + columns - 1) / columns);
    driveGrid->SetRowCount(driveRows);
    for (size_t i = 0; i < drives.size(); ++i) {
        const auto& drive = drives[i];
        auto body = std::make_shared<ColumnBox>();
        body->SetSpacing(0.0f);
        body->SetFillWidth(true);
        auto row = std::make_shared<RowBox>();
        row->SetSpacing(10.0f);
        row->SetFillWidth(true);
        auto iconLabel = std::make_shared<Label>(L"");
        iconLabel->SetWidth(36.0f);
        iconLabel->SetHeight(36.0f);
        iconLabel->SetAlignment(Label::HAlign::Center, Label::VAlign::Center);
        if (HICON h = GetShellIconForPath(drive.root, true)) {
            if (auto icon = Image::FromHICON(h)) {
                iconLabel->SetImage(icon);
                iconLabel->SetIconSize(26.0f, 26.0f);
            }
            DestroyIcon(h);
        }
        row->AddChild(iconLabel);
        auto textCol = std::make_shared<ColumnBox>();
        textCol->SetSpacing(3.0f);
        textCol->SetFillWidth(true);
        auto name = std::make_shared<Label>(drive.display);
        name->SetFillWidth(true);
        name->SetHeight(24.0f);
        name->SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
        name->SetFontSize(16.0f);
        name->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD);
        textCol->AddChild(name);
        auto bar = std::make_shared<ProgressBar>();
        bar->SetHeight(9.0f);
        const double usedRatio =
            (drive.ready && drive.totalBytes > 0)
                ? 1.0 - static_cast<double>(drive.freeBytes) /
                            static_cast<double>(drive.totalBytes)
                : 0.0;
        bar->SetValue(static_cast<float>(usedRatio));
        textCol->AddChild(bar);
        auto capacity = std::make_shared<Label>(
            drive.ready
                ? (L"可用 " + FormatSize(drive.freeBytes) +
                   L" / 共 " + FormatSize(drive.totalBytes))
                : (drive.root + L" · 未就绪"));
        capacity->SetFillWidth(true);
        capacity->SetHeight(19.0f);
        capacity->SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
        capacity->SetFontSize(12.0f);
        capacity->SetTextColor(homeTheme.Get(ThemeRole::TextSecondary));
        textCol->AddChild(capacity);
        row->AddChild(textCol);
        body->AddChild(row);

        auto card = std::make_shared<ClickableCard>();
        styleCard(card);
        card->SetHeight(96.0f);
        card->SetContent(body);
        card->SetToolTip(drive.root);
        const std::wstring root = drive.root;
        card->OnClick = [this, root]() { NavigateTo(root); };
        driveGrid->AddChild(
            card, static_cast<int>(i) / columns,
            static_cast<int>(i) % columns, 1, 1,
            GridLayout::Alignment::Center,
            GridLayout::Alignment::Center);
    }
    drivesList_->AddChild(driveGrid);

    addressBox_->SetText(L"此电脑");
    RebuildBreadcrumb(L"::thispc");
}

void ExplorerPane::ShowRecycleBinPage() {
    showingDrives_ = false;
    showingRecycleBin_ = true;
    if (split_) split_->SetVisible(false);
    if (drivesScroll_) drivesScroll_->SetVisible(false);
    if (!recyclePage_ || !recycleTable_) return;
    if (searchBox_) {
        searchBox_->SetPlaceholder(L"回收站暂不支持搜索");
        searchBox_->SetEnabled(false);
    }
    recyclePage_->SetVisible(true);
    if (OnSelectionChanged) OnSelectionChanged(0, 0);

    const RecycleBinInfo info = QueryRecycleBin();
    if (recycleSummary_) {
        recycleSummary_->SetText(
            info.available
                ? (std::to_wstring(info.itemCount) + L" 项 · " +
                   FormatSize(info.totalBytes))
                : std::wstring(L"回收站不可用"));
    }
    recycleTable_->SetCellLabelProvider(
        0, 5, [](int, int, Label&) {});

    auto self = shared_from_this();
    RunAsync<std::vector<RecycleBinItem>>(
        AsyncOwner{ self },
        [](AsyncCancelToken, AsyncProgress) { return QueryRecycleBinItems(); },
        [self](std::vector<RecycleBinItem> items) {
            if (!self->showingRecycleBin_) return;
            self->recycleItems_ = std::move(items);
            self->recycleIconCache_.clear();
            if (self->recycleSummary_) {
                uint64_t totalBytes = 0;
                for (const auto& item : self->recycleItems_)
                    totalBytes += item.size;
                self->recycleSummary_->SetText(
                    std::to_wstring(self->recycleItems_.size()) + L" 项" +
                    (totalBytes ? L" · " + FormatSize(totalBytes)
                                : std::wstring()));
            }
            if (!self->recycleTable_) return;
            const int rowCount =
                static_cast<int>(self->recycleItems_.size());
            auto* pane = self.get();
            self->recycleTable_->SetCellLabelProvider(
                rowCount, 5, [pane](int row, int col, Label& label) {
                    if (row < 0 ||
                        row >= static_cast<int>(pane->recycleItems_.size()))
                        return;
                    const auto& item =
                        pane->recycleItems_[static_cast<size_t>(row)];
                    label.SetTextOverflow(Label::TextOverflow::Ellipsis);
                    switch (col) {
                    case 0: {
                        label.SetTextFast(item.name);
                        label.SetAlignment(Label::HAlign::Left,
                                           Label::VAlign::Center);
                        const std::wstring key =
                            item.isDir ? L"<directory>"
                                       : platform::ExtensionOf(item.name);
                        auto found = pane->recycleIconCache_.find(key);
                        std::shared_ptr<Image> image;
                        if (found != pane->recycleIconCache_.end()) {
                            image = found->second;
                        } else {
                            HICON icon = fs::ShellIcons::Instance().Get(
                                item.isDir, item.name, false,
                                fs::IconSize::Small);
                            if (icon) image = Image::FromHICON(icon);
                            pane->recycleIconCache_.emplace(key, image);
                        }
                        if (image) {
                            label.SetImage(image);
                            label.SetIconSize(kIconDpi, kIconDpi);
                            label.SetIconSpacing(6.0f);
                        }
                        break;
                    }
                    case 1:
                        label.SetTextFast(item.originalPath);
                        break;
                    case 2:
                        label.SetTextFast(item.deletedAt.dwLowDateTime ||
                                                  item.deletedAt.dwHighDateTime
                                              ? FormatFileTime(item.deletedAt)
                                              : std::wstring());
                        break;
                    case 3:
                        label.SetTextFast(item.isDir ? std::wstring()
                                                    : FormatSize(item.size));
                        break;
                    case 4:
                        label.SetTextFast(item.typeName);
                        break;
                    default:
                        break;
                    }
                });
            self->recycleTable_->SetColumnAlignment(3, TextHAlign::Right);
        },
        [self](std::exception_ptr) {
            if (!self->showingRecycleBin_) return;
            self->recycleItems_.clear();
            if (self->recycleSummary_)
                self->recycleSummary_->SetText(L"读取回收站内容失败");
            if (self->recycleTable_)
                self->recycleTable_->SetCellLabelProvider(
                    0, 5, [](int, int, Label&) {});
        });

    addressBox_->SetText(L"回收站");
    RebuildBreadcrumb(L"回收站");
}

std::vector<int> ExplorerPane::SelectedRecycleRows() const {
    std::vector<int> rows;
    if (!recycleTable_) return rows;
    std::unordered_set<int> seen;
    for (int row : recycleTable_->GetSelectedRows())
        if (row >= 0 && row < static_cast<int>(recycleItems_.size()) &&
            seen.insert(row).second)
            rows.push_back(row);
    for (int row : recycleTable_->GetCheckedRows())
        if (row >= 0 && row < static_cast<int>(recycleItems_.size()) &&
            seen.insert(row).second)
            rows.push_back(row);
    std::sort(rows.begin(), rows.end());
    return rows;
}

std::vector<const platform::RecycleBinItem*>
ExplorerPane::SelectedRecycleItems() const {
    std::vector<const platform::RecycleBinItem*> items;
    for (int row : SelectedRecycleRows())
        items.push_back(&recycleItems_[static_cast<size_t>(row)]);
    return items;
}

void ExplorerPane::ScheduleRecycleBinRefresh() {
    if (!window_) {
        if (showingRecycleBin_) ShowRecycleBinPage();
        return;
    }
    if (!recycleRefreshTimer_) {
        recycleRefreshTimer_ = window_->CreateTimer(250);
        recycleRefreshTimer_->Tick.connect([this]() {
            if (recycleRefreshTimer_) recycleRefreshTimer_->Stop();
            if (showingRecycleBin_) ShowRecycleBinPage();
        });
    }
    recycleRefreshTimer_->Start(250);
}

void ExplorerPane::InvokeSelectedRecycleVerb(const std::wstring& verb) {
    const auto items = SelectedRecycleItems();
    if (items.empty()) return;
    std::wstring error;
    const bool ok = platform::InvokeRecycleBinVerb(
        window_ ? window_->GetHwnd() : nullptr, items, verb, &error);
    if (!ok) {
        Notify(L"回收站操作失败",
               (error.empty() ? L"无法执行系统回收站命令" : error) +
                   L"\n\n命令：" + verb,
               NoticeKind::Error);
        return;
    }
    ScheduleRecycleBinRefresh();
}

void ExplorerPane::ShowRecycleBinItemMenu(int row) {
    if (row < 0 || row >= static_cast<int>(recycleItems_.size()) || !recycleTable_)
        return;
    auto selected = SelectedRecycleRows();
    if (std::find(selected.begin(), selected.end(), row) == selected.end()) {
        for (int checked : recycleTable_->GetCheckedRows())
            recycleTable_->SetRowChecked(checked, false);
        recycleTable_->ClearSelection();
        recycleTable_->SelectRow(row);
        recycleTable_->SetRowChecked(row, true);
    }
    const auto items = SelectedRecycleItems();
    if (items.empty()) return;
    platform::ShowRecycleBinContextMenu(
        window_ ? window_->GetHwnd() : nullptr, items);
    ScheduleRecycleBinRefresh();
}

// ---------------------------------------------------------------------------
// 导航
// ---------------------------------------------------------------------------
void ExplorerPane::NavigateTo(const std::wstring& path, bool addHistory) {
    if (path.empty()) return;
    const std::wstring target =
        path == L"此电脑" ? L"::thispc" :
        path == L"回收站" ? L"::recyclebin" : path;
    if (!pendingSelectPath_.empty() &&
        _wcsicmp(target.c_str(), ParentPath(pendingSelectPath_).c_str()) != 0) {
        pendingSelectPath_.clear();
    }

    if (target != currentPath_ && searchBox_)
        ClearSearch(false);

    // "此电脑"是正式导航位置，不是临时显示状态：它要进入历史，
    // 并且必须取消尚未完成的目录读取，避免旧结果稍后把页面覆盖掉。
    if (target == L"::thispc") {
        StopDirectoryWatcher();
        ++loadGen_;
        if (addHistory) {
            if (historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int>(history_.size()))
                history_.resize(static_cast<size_t>(historyIndex_) + 1);
            if (history_.empty() || history_.back() != target) {
                history_.push_back(target);
                historyIndex_ = static_cast<int>(history_.size()) - 1;
            }
        }
        currentPath_ = target;
        entries_.clear();
        if (list_) list_->SetEntries({});
        ShowDrivesPage();
        UpdateNavButtons();
        if (OnNavigated) OnNavigated(target);
        if (OnStatus) OnStatus(L"此电脑");
        return;
    }

    if (target == L"::recyclebin") {
        StopDirectoryWatcher();
        ++loadGen_;
        if (addHistory) {
            if (historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int>(history_.size()))
                history_.resize(static_cast<size_t>(historyIndex_) + 1);
            if (history_.empty() || history_.back() != target) {
                history_.push_back(target);
                historyIndex_ = static_cast<int>(history_.size()) - 1;
            }
        }
        currentPath_ = target;
        entries_.clear();
        if (list_) list_->SetEntries({});
        ShowRecycleBinPage();
        UpdateNavButtons();
        if (OnNavigated) OnNavigated(target);
        if (OnStatus) OnStatus(L"回收站");
        return;
    }
    HideDrivesPage();

    const std::wstring normalized = NormalizePath(target);

    if (addHistory) {
        if (historyIndex_ >= 0 && historyIndex_ + 1 < static_cast<int>(history_.size()))
            history_.resize(static_cast<size_t>(historyIndex_) + 1);

        if (history_.empty() || history_.back() != normalized) {
            history_.push_back(normalized);
            historyIndex_ = static_cast<int>(history_.size()) - 1;
        }
    }

    BeginLoad(normalized);
    UpdateNavButtons();
}

void ExplorerPane::GoBack() {
    if (historyIndex_ <= 0) return;
    --historyIndex_;
    NavigateTo(history_[static_cast<size_t>(historyIndex_)], false);
}

void ExplorerPane::GoForward() {
    if (historyIndex_ + 1 >= static_cast<int>(history_.size())) return;
    ++historyIndex_;
    NavigateTo(history_[static_cast<size_t>(historyIndex_)], false);
}

void ExplorerPane::GoUp() {
    if (currentPath_.empty() || currentPath_ == L"::thispc") return;
    if (currentPath_ == L"::recyclebin") {
        NavigateTo(L"::thispc");
        return;
    }
    const std::wstring parent = ParentPath(currentPath_);
    if (parent == currentPath_) {
        // 已经在盘符根目录了：上一级就是"此电脑"
        NavigateTo(L"::thispc");
        return;
    }
    NavigateTo(parent);
}

void ExplorerPane::Refresh() {
    if (currentPath_ == L"::thispc") {
        ShowDrivesPage();
        return;
    }
    if (currentPath_ == L"::recyclebin") {
        ShowRecycleBinPage();
        return;
    }
    if (!currentPath_.empty()) BeginLoad(currentPath_);
}

void ExplorerPane::UpdateNavButtons() {
    if (backBtn_) backBtn_->SetEnabled(historyIndex_ > 0);
    if (forwardBtn_) forwardBtn_->SetEnabled(historyIndex_ + 1 < static_cast<int>(history_.size()));
}

// ---------------------------------------------------------------------------
// 异步读取
// ---------------------------------------------------------------------------
void ExplorerPane::BeginLoad(const std::wstring& path) {
    const uint64_t gen = ++loadGen_;
    auto self = shared_from_this();
    const bool showHidden = LoadSettings().showHiddenFiles;

    ShowHint(L"");

    RunAsync<fs::Snapshot>(
        AsyncOwner{ self },
        [path, showHidden](AsyncCancelToken, AsyncProgress) {
            return fs::DirectoryModel::Load(path, showHidden);
        },
        [self, gen](fs::Snapshot snapshot) {
            if (gen != self->loadGen_) return;
            self->ApplySnapshot(snapshot);
        },
        [self, gen, path](std::exception_ptr exception) {
            if (gen != self->loadGen_) return;
            self->Notify(L"读取失败",
                         L"目录：" + path + L"\n异常：" +
                             ExceptionText(exception),
                         NoticeKind::Error);
            if (self->OnStatus) self->OnStatus(L"读取失败");
        });
}

void ExplorerPane::ApplySnapshot(const fs::Snapshot& snap) {
    currentPath_ = snap.path;
    sourceEntries_ = snap.entries;
    entries_ = sourceEntries_;
    fs::DirectoryModel::Sort(entries_, sortKey_, sortOrder_);

    addressBox_->SetText(snap.path);
    if (addressBox_->IsFocused()) {
        addressBox_->SetSelection(
            0, static_cast<int>(addressBox_->GetText().size()));
    }
    RebuildBreadcrumb(snap.path);
    ScheduleBreadcrumbRebuild();

    if (snap.accessDenied) {
        pendingSelectPath_.clear();
        list_->SetEntries({});
        Notify(L"没有访问权限",
               L"无法读取 " + snap.path + L"。可以尝试「提权」后重试。",
               NoticeKind::Warning);
        if (OnStatus) OnStatus(L"拒绝访问");
        return;
    }
    if (snap.notFound) {
        pendingSelectPath_.clear();
        list_->SetEntries({});
        Notify(L"找不到该位置",
               snap.path + L"\n可能已被移动、删除，或驱动器未就绪。",
               NoticeKind::Warning);
        if (OnStatus) OnStatus(L"路径不存在");
        return;
    }
    if (!snap.Ok()) {
        Notify(L"部分内容无法读取",
               FormatWin32Error(snap.errorCode), NoticeKind::Warning);
    } else {
        ShowHint(L"");
    }

    if (searchBox_ && !searchBox_->GetText().empty())
        StartRecursiveSearch();
    else
        list_->SetEntries(entries_);
    ApplyPendingPathSelection();
    if (searchBox_) {
        searchBox_->SetEnabled(true);
        const std::wstring folderName =
            fs::DirectoryModel::DisplayNameOf(snap.path);
        searchBox_->SetPlaceholder(L"在 " + folderName + L" 中搜索");
    }

    // 左右联动：树展开到该深度 + 选中 + 滚动到可见
    SyncTreeToPath(snap.path);

    if (OnNavigated) OnNavigated(snap.path);
    if (OnLoaded) OnLoaded(entries_.size(), snap.DirCount(), snap.FileCount(), snap.elapsedMs);
    UpdateSelectionSummary();
    StartDirectoryWatcher(snap.path);
}

void ExplorerPane::SortByColumn(int column) {
    fs::SortKey key = fs::SortKey::Name;
    switch (column) {
    case 0: key = fs::SortKey::Name; break;
    case 1: key = fs::SortKey::Size; break;
    case 2: key = fs::SortKey::Type; break;
    case 3: key = fs::SortKey::Modified; break;
    default: return;
    }

    if (key == sortKey_) {
        sortOrder_ = (sortOrder_ == fs::SortOrder::Ascending)
            ? fs::SortOrder::Descending
            : fs::SortOrder::Ascending;
    } else {
        sortKey_ = key;
        sortOrder_ = fs::SortOrder::Ascending;
    }

    fs::DirectoryModel::Sort(entries_, sortKey_, sortOrder_);
    if (list_) {
        list_->SetEntries(entries_);
        list_->SetSortIndicator(column, sortOrder_ == fs::SortOrder::Ascending);
    }
    UpdateSelectionSummary();
}

void ExplorerPane::ApplyViewOptions() {
    const AppSettings settings = LoadSettings();
    if (list_) list_->SetHideKnownExtensions(settings.hideKnownExtensions);
    if (currentPath_ == L"::thispc") ShowDrivesPage();
    else if (!currentPath_.empty()) BeginLoad(currentPath_);
}

void ExplorerPane::UpdateViewOption(bool showHidden, bool hideExtensions) {
    AppSettings settings = LoadSettings();
    settings.showHiddenFiles = showHidden;
    settings.hideKnownExtensions = hideExtensions;
    SaveSettings(settings);
    ApplyViewOptions();
    RefreshTreeVisibility();
}

void ExplorerPane::StartRecursiveSearch() {
    if (!searchBox_ || !list_) return;
    std::wstring query = searchBox_->GetText();
    const size_t first = query.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        ClearSearch();
        return;
    }
    const size_t last = query.find_last_not_of(L" \t\r\n");
    query = query.substr(first, last - first + 1);
    if (currentPath_.empty() || currentPath_ == L"::thispc" ||
        currentPath_ == L"::recyclebin") {
        return;
    }

    const uint64_t generation = ++searchGeneration_;
    const std::wstring root = currentPath_;
    const bool showHidden = LoadSettings().showHiddenFiles;
    auto self = shared_from_this();
    ShowHint(L"正在搜索子目录…");
    if (OnStatus) OnStatus(L"正在搜索“" + query + L"”…");
    RunAsync<std::vector<fs::FileEntry>>(
        AsyncOwner{ self },
        [root, query, showHidden](AsyncCancelToken token, AsyncProgress progress) {
            return fs::DirectoryModel::Search(
                root, query, showHidden,
                [token]() { return token.IsCancellationRequested(); },
                [progress](size_t visited) mutable {
                    progress.Report(0.0f, L"已扫描 " +
                        std::to_wstring(visited) + L" 个目录");
                });
        },
        [self, generation, query](std::vector<fs::FileEntry> results) {
            if (generation != self->searchGeneration_ ||
                !self->list_) return;
            self->entries_ = std::move(results);
            fs::DirectoryModel::Sort(
                self->entries_, self->sortKey_, self->sortOrder_);
            self->list_->SetEntries(self->entries_);
            self->ShowHint(
                L"子目录搜索“" + query + L"”： " +
                std::to_wstring(self->entries_.size()) + L" 项");
            if (self->OnStatus)
                self->OnStatus(L"搜索结果 " +
                               std::to_wstring(self->entries_.size()));
        },
        [self, generation](std::exception_ptr) {
            if (generation == self->searchGeneration_)
                self->Notify(L"搜索失败",
                             L"递归搜索目录时发生错误。",
                             NoticeKind::Error);
        });
}

void ExplorerPane::ClearSearch(bool restoreEntries) {
    ++searchGeneration_;
    if (searchDebounceTimer_) searchDebounceTimer_->Stop();
    if (searchBox_) {
        searchUpdating_ = true;
        searchBox_->SetText(L"");
        searchUpdating_ = false;
    }
    if (!restoreEntries || !list_) return;
    entries_ = sourceEntries_;
    fs::DirectoryModel::Sort(entries_, sortKey_, sortOrder_);
    list_->SetEntries(entries_);
    UpdateSelectionSummary();
    ShowHint(L"");
    if (!currentPath_.empty() &&
        currentPath_.rfind(L"::", 0) != 0 &&
        OnStatus) {
        OnStatus(L"就绪");
    }
}

void ExplorerPane::ApplyPersistentSettings() {
    const AppSettings settings = LoadSettings();
    sortKey_ = static_cast<fs::SortKey>(std::clamp(settings.sortKey, 0, 3));
    sortOrder_ = settings.sortAscending ? fs::SortOrder::Ascending
                                        : fs::SortOrder::Descending;
    if (list_) {
        list_->SetSortIndicator(static_cast<int>(sortKey_),
                                settings.sortAscending);
        list_->SetColumnWidths({ settings.column0, settings.column1,
                                 settings.column2, settings.column3 });
        list_->SetHideKnownExtensions(settings.hideKnownExtensions);
    }
    if (split_) split_->SetSplitRatio(std::clamp(settings.mainSplitRatio, 0.15f, 0.60f));
    if (leftStack_) {
        leftStack_->SetSplitRatio(std::clamp(settings.quickSplitRatio, 0.08f, 0.50f));
    }
}

bool ExplorerPane::SavePersistentSettings(std::wstring* error) {
    const AppSettings before = LoadSettings();
    AppSettings settings = before;
    settings.sortKey = static_cast<int>(sortKey_);
    settings.sortAscending = sortOrder_ == fs::SortOrder::Ascending;
    settings.mainSplitRatio = split_ ? split_->GetSplitRatio() : settings.mainSplitRatio;
    settings.quickSplitRatio = leftStack_ ? leftStack_->GetSplitRatio()
                                          : settings.quickSplitRatio;
    if (list_) {
        const auto widths = list_->GetColumnWidths();
        settings.column0 = widths[0];
        settings.column1 = widths[1];
        settings.column2 = widths[2];
        settings.column3 = widths[3];
    }
    if (!currentPath_.empty() && currentPath_.rfind(L"::", 0) != 0) {
        settings.lastFolder = currentPath_;
    } else if (currentPath_ == L"::thispc") {
        settings.lastFolder = currentPath_;
    }
    const bool changed =
        settings.sortKey != before.sortKey ||
        settings.sortAscending != before.sortAscending ||
        std::fabs(settings.mainSplitRatio - before.mainSplitRatio) > 0.0005f ||
        std::fabs(settings.quickSplitRatio - before.quickSplitRatio) > 0.0005f ||
        std::fabs(settings.column0 - before.column0) > 0.5f ||
        std::fabs(settings.column1 - before.column1) > 0.5f ||
        std::fabs(settings.column2 - before.column2) > 0.5f ||
        std::fabs(settings.column3 - before.column3) > 0.5f ||
        settings.lastFolder != before.lastFolder;
    if (!changed) return true;
    return SaveSettings(settings, error);
}

void ExplorerPane::ReportWarning(const std::wstring& title,
                                 const std::wstring& text) {
    Notify(title, text, NoticeKind::Warning);
}

void ExplorerPane::StartDirectoryWatcher(const std::wstring& path) {
    if (!directoryWatcher_) {
        directoryWatcher_ = std::make_unique<platform::DirectoryWatcher>();
    } else {
        directoryWatcher_->Stop();
    }
    if (!path.empty()) directoryWatcher_->Start(path);
}

void ExplorerPane::StopDirectoryWatcher() {
    if (directoryWatcher_) directoryWatcher_->Stop();
}

void ExplorerPane::HandleExternalDrag(DragEventArgs& e, bool drop) {
    const auto files = e.data.GetFiles();
    if (currentPath_ == L"::thispc" || currentPath_.empty() || files.empty()) {
        e.effect = DROPEFFECT_NONE;
        return;
    }

    const bool move = (e.keyState & MK_SHIFT) != 0 &&
                      (e.keyState & MK_CONTROL) == 0;
    if (!drop) {
        const DWORD preferred = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        e.effect = (e.allowed & preferred) != 0
                       ? preferred
                       : (e.allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE));
        return;
    }

    const std::wstring target = NormalizePath(currentPath_);
    std::vector<std::wstring> accepted;
    accepted.reserve(files.size());
    for (const auto& file : files) {
        const std::wstring parent =
            NormalizePath(ParentPath(StripExtendedPath(file)));
        if (_wcsicmp(parent.c_str(), target.c_str()) != 0)
            accepted.push_back(file);
    }
    if (accepted.empty()) {
        ShowHint(L"同文件夹项目已忽略");
        e.effect = DROPEFFECT_NONE;
        return;
    }

    if (!app::TaskClient::Submit(move ? L"cut" : L"copy", currentPath_, accepted)) {
        Notify(L"无法接收拖入文件",
               L"拖入请求没有进入后台任务队列。\n目标：" + currentPath_,
               NoticeKind::Error);
        e.effect = DROPEFFECT_NONE;
        return;
    }
    ShowHint(L"已接收到 " + std::to_wstring(accepted.size()) + L" 项");
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
void ExplorerPane::OpenIndex(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    const auto& e = entries_[static_cast<size_t>(index)];

    if (e.isDir) {
        NavigateTo(e.fullPath);
    } else {
        OpenFilePath(e.fullPath);
    }
}

bool ExplorerPane::OpenFilePath(const std::wstring& path) {
    if (path.empty() || !PathExists(path)) {
        Notify(L"无法打开文件",
               L"目标不存在或路径为空：\n" + path,
               NoticeKind::Error);
        return false;
    }

    if (IsShortcutPath(path)) {
        ShortcutInfo shortcut;
        if (ResolveShortcut(path, &shortcut, nullptr) &&
            shortcut.kind == ShortcutTargetKind::Directory &&
            PathExists(shortcut.targetPath)) {
            NavigateTo(shortcut.targetPath);
            if (OnStatus)
                OnStatus(L"已打开快捷方式目标：" + shortcut.targetPath);
            return true;
        }
    }

    const AppSettings settings = LoadSettings();
    if (!settings.executableViaShell && IsExecutablePath(path)) {
        if (!app::TaskClient::Submit(L"launch", path, {})) {
            Notify(L"无法请求后台启动",
                   L"启动请求没有进入任务队列。\n目标：" + path,
                   NoticeKind::Error);
            return false;
        }
        if (OnStatus) OnStatus(L"已交给后台进程启动：" + FileNameOf(path));
        return true;
    }

    std::wstring error;
    if (!OpenPath(path, &error)) {
        Notify(L"无法打开文件",
               L"目标：" + path + L"\n原因：" +
                   (error.empty() ? L"ShellExecuteEx 未返回详细原因" : error),
               NoticeKind::Error);
        return false;
    }
    return true;
}

void ExplorerPane::OpenLocationInZExplorer(const std::wstring& path) {
    if (path.empty() || !PathExists(path)) {
        Notify(L"无法打开所在位置", L"目标不存在：\n" + path,
               NoticeKind::Error);
        return;
    }
    const std::wstring normalized = NormalizePath(path);
    if (IsDirectoryPath(normalized)) {
        pendingSelectPath_.clear();
        NavigateTo(normalized);
        return;
    }

    const std::wstring parent = ParentPath(normalized);
    pendingSelectPath_ = normalized;
    if (searchBox_ && !searchBox_->GetText().empty())
        ClearSearch(true);
    if (_wcsicmp(currentPath_.c_str(), parent.c_str()) == 0) {
        ApplyPendingPathSelection();
    } else {
        NavigateTo(parent);
    }
}

void ExplorerPane::ApplyPendingPathSelection() {
    if (pendingSelectPath_.empty() || !list_) return;
    const std::wstring parent = ParentPath(pendingSelectPath_);
    if (_wcsicmp(currentPath_.c_str(), parent.c_str()) != 0) return;
    const std::wstring selected = pendingSelectPath_;
    if (list_->SelectPath(selected)) {
        pendingSelectPath_.clear();
        UpdateSelectionSummary();
        if (OnStatus)
            OnStatus(L"已选中：" + FileNameOf(selected));
    } else {
        pendingSelectPath_.clear();
    }
}

void ExplorerPane::SelectIndex(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        if (OnStatus) OnStatus(L"未选择");
        UpdateSelectionSummary();
        return;
    }
    if (OnStatus) OnStatus(L"选中：" + entries_[static_cast<size_t>(index)].name);
    UpdateSelectionSummary();
}

void ExplorerPane::UpdateSelectionSummary() {
    if (!OnSelectionChanged) return;
    if (!list_) {
        OnSelectionChanged(0, 0);
        return;
    }

    auto rows = list_->SelectedRows();
    if (rows.empty()) {
        const int current = list_->SelectedIndex();
        if (current >= 0 && current < static_cast<int>(entries_.size()))
            rows.push_back(current);
    }

    uint64_t bytes = 0;
    for (int row : rows) {
        if (row >= 0 && row < static_cast<int>(entries_.size())) {
            const auto& entry = entries_[static_cast<size_t>(row)];
            if (!entry.isDir) bytes += entry.size;
        }
    }
    OnSelectionChanged(rows.size(), bytes);
}

void ExplorerPane::ShowHint(const std::wstring& text) {
    if (!hint_) return;
    if (text.empty()) {
        hint_->SetVisible(false);
        return;
    }
    hint_->SetVisible(false);
    if (OnStatus) OnStatus(text);
}

// ---------------------------------------------------------------------------
// 文件操作
// ---------------------------------------------------------------------------
const fs::FileEntry* ExplorerPane::SelectedEntry() const {
    if (!list_) return nullptr;
    const int idx = list_->SelectedIndex();
    if (idx < 0 || idx >= static_cast<int>(entries_.size())) return nullptr;
    return &entries_[static_cast<size_t>(idx)];
}

// 多选：框选 + 勾选的并集；没有多选时退回"当前行"
std::vector<std::wstring> ExplorerPane::SelectedPaths() const {
    std::vector<std::wstring> out;
    if (!list_) return out;

    for (int row : list_->SelectedRows()) {
        if (row >= 0 && row < static_cast<int>(entries_.size()))
            out.push_back(entries_[static_cast<size_t>(row)].fullPath);
    }
    if (out.empty()) {
        if (const auto* e = SelectedEntry()) out.push_back(e->fullPath);
    }
    return out;
}

void ExplorerPane::ShowMessage(const std::wstring& title, const std::wstring& text,
                               MessageBox::Icon icon) {
    NoticeKind kind = NoticeKind::Info;
    if (icon == MessageBox::Icon::Warning) kind = NoticeKind::Warning;
    else if (icon == MessageBox::Icon::Error) kind = NoticeKind::Error;
    Notify(title, text, kind);
}

void ExplorerPane::Notify(const std::wstring& title, const std::wstring& text,
                          NoticeKind kind) {
    if (kind == NoticeKind::Info) {
        std::wstring status = text.empty() ? title : title + L"：" + text;
        if (title.empty() || title == L"提示") status = text;
        if (OnStatus && !status.empty()) OnStatus(status);
        return;
    }

    if (notificationDrawer_) {
        std::wstring details = text;
        details += L"\n\n──────── 诊断信息 ────────";
        details += L"\n发生时间：" + LocalTimestamp();
        details += L"\n操作：" + title;
        details += L"\n当前目录：" +
                   (currentPath_.empty() ? std::wstring(L"<未知>") : currentPath_);
        details += L"\n窗口进程：PID " + std::to_wstring(GetCurrentProcessId()) +
                   L" · " + CurrentProcessPath();
        details += L"\n后台任务服务：" +
                   std::wstring(app::TaskClient::HostReady() ? L"已连接"
                                                             : L"未连接或未就绪");

        const auto selected = SelectedPaths();
        if (!selected.empty()) {
            details += L"\n当前选择（" + std::to_wstring(selected.size()) +
                       L" 项）：";
            const size_t shown = (std::min)(selected.size(), size_t{ 20 });
            for (size_t i = 0; i < shown; ++i) {
                details += L"\n  " + std::to_wstring(i + 1) + L". " + selected[i];
            }
            if (selected.size() > shown) {
                details += L"\n  …另有 " +
                           std::to_wstring(selected.size() - shown) + L" 项";
            }
        }
        notificationDrawer_->Show(title, details, kind);
        if (OnStatus) OnStatus(title);
    }
}

void ExplorerPane::DoOpenSelected() {
    if (const auto* e = SelectedEntry()) {
        const int idx = list_->SelectedIndex();
        OpenIndex(idx);
        (void)e;
    }
}

void ExplorerPane::DoOpenLocation() {
    const fs::FileEntry* entry = SelectedEntry();
    if (!entry) return;
    std::wstring error;
    if (!OpenContainingFolder(entry->fullPath, &error)) {
        Notify(L"无法打开文件位置",
               L"目标：" + entry->fullPath + L"\n原因：" +
                   (error.empty() ? L"系统没有返回详细原因" : error),
               NoticeKind::Error);
    }
}

void ExplorerPane::DoOpenWith() {
    const fs::FileEntry* entry = SelectedEntry();
    if (!entry || entry->isDir) return;
    std::wstring error;
    if (!OpenWith(entry->fullPath, &error)) {
        Notify(L"无法打开“打开方式”",
               L"目标：" + entry->fullPath + L"\n原因：" +
                   (error.empty() ? L"系统没有返回详细原因" : error),
               NoticeKind::Error);
    }
}

void ExplorerPane::DoCopy(bool cut) {
    const auto paths = SelectedPaths();
    if (paths.empty()) {
        Notify(L"没有选择项目", L"请先选中要复制或剪切的文件。", NoticeKind::Info);
        return;
    }

    // 剪贴板由**后台进程**写，本进程不碰任何文件系统操作
    if (!app::TaskClient::Submit(cut ? L"cut" : L"copy", L"", paths)) {
        ShowMessage(L"操作失败", L"后台进程不可用", MessageBox::Icon::Error);
        return;
    }

    wchar_t buf[128];
    swprintf_s(buf, cut ? L"已剪切 %zu 项" : L"已复制 %zu 项", paths.size());
    ShowHint(buf);
}

void ExplorerPane::DoCopyPath() {
    const auto paths = SelectedPaths();
    if (paths.empty()) {
        Notify(L"没有选择项目", L"请先选中要复制的路径。", NoticeKind::Info);
        return;
    }

    std::wstring text;
    for (const auto& path : paths) {
        if (!text.empty()) text += L"\r\n";
        text += path;
    }
    std::wstring error;
    if (!SetClipboardText(text, &error)) {
        Notify(L"复制路径失败", error, NoticeKind::Error);
        return;
    }
    ShowHint(paths.size() == 1 ? L"已复制路径"
                              : L"已复制 " + std::to_wstring(paths.size()) +
                                    L" 个路径");
}

void ExplorerPane::DoPaste() {
    if (currentPath_.empty() || currentPath_ == L"::thispc") return;

    if (!app::TaskClient::Submit(L"paste", currentPath_, {})) {
        ShowMessage(L"粘贴失败", L"后台进程不可用（ZExplorer.Host.exe）", MessageBox::Icon::Error);
        return;
    }
    ShowHint(L"已提交粘贴任务");
    ScheduleRefresh();
}

void ExplorerPane::DoDelete(bool permanent) {
    const auto paths = SelectedPaths();
    if (paths.empty() || !window_) return;

    wchar_t msg[256];
    if (paths.size() == 1) {
        swprintf_s(msg, permanent ? L"确定要永久删除「%ls」吗？此操作不可恢复。"
                                  : L"确定要把「%ls」移到回收站吗？",
                   FileNameOf(paths.front()).c_str());
    } else {
        swprintf_s(msg, permanent ? L"确定要永久删除这 %zu 项吗？此操作不可恢复。"
                                  : L"确定要把这 %zu 项移到回收站吗？",
                   paths.size());
    }

    if (LoadSettings().confirmRecycleDelete || permanent) {
        MessageBox confirm(window_, permanent ? L"永久删除" : L"删除", msg,
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, /*blocking*/ true);
        if (confirm.GetResult() != FastButton::Yes) return;
    }

    if (!app::TaskClient::Submit(permanent ? L"deleteperm" : L"delete", L"", paths)) {
        ShowMessage(L"删除失败", L"后台进程不可用", MessageBox::Icon::Error);
        return;
    }
    ShowHint(permanent ? L"已提交永久删除任务" : L"已提交删除任务");
    ScheduleRefresh();
}

void ExplorerPane::DoRename() {
    const auto* e = SelectedEntry();
    if (!e || !window_) return;

    const std::wstring oldPath = e->fullPath;
    const std::wstring oldName = e->name;

    std::wstring newName;
    if (!PromptName(L"重命名", oldName, &newName)) return;
    if (newName == oldName) return;

    const std::wstring newPath = JoinPath(ParentPath(oldPath), newName);
    if (!app::TaskClient::Submit(L"rename", L"", { oldPath, newPath })) {
        ShowMessage(L"重命名失败", L"后台进程不可用", MessageBox::Icon::Error);
        return;
    }
    ShowHint(L"已提交重命名任务");
    ScheduleRefresh();
}

// 通用"输入名字"对话框。
// 注意：MessageBox::SetUserContent 会把预设按钮整排清掉（库的约定），
// 所以这里自己搭一个带 确定/取消 的模态窗口。
bool ExplorerPane::PromptName(const std::wstring& title, const std::wstring& initial,
                              std::wstring* out, Window* owner) {
    Window* modalOwner = owner ? owner : window_;
    if (!modalOwner || !out) return false;

    auto dlg = std::make_shared<Window>();
    if (!dlg->Create(460, 200, title)) return false;
    dlg->SetMinSize(360, 170);

    auto root = dlg->GetRootColumnBox();
    root->SetMargin(Thickness(16, 14, 16, 14));
    root->SetSpacing(12.0f);

    root->AddChild(std::make_shared<Label>(L"请输入名称："));

    auto edit = std::make_shared<TextBox>();
    edit->SetText(initial);
    edit->SetHeight(30.0f);
    root->AddChild(edit);

    auto row = std::make_shared<RowBox>();
    row->SetSpacing(8.0f);
    row->SetHeight(30.0f);

    auto spacer = std::make_shared<Label>(L"");
    spacer->SetFillWidth(true);
    row->AddChild(spacer);

    bool confirmed = false;
    auto okBtn = std::make_shared<Button>(L"确定");
    okBtn->SetWidth(88.0f);
    okBtn->SetHeight(28.0f);
    okBtn->Connect(okBtn->Clicked, [&]() {
        confirmed = true;
        dlg->Close();
    });
    row->AddChild(okBtn);

    auto cancelBtn = std::make_shared<Button>(L"取消");
    cancelBtn->SetWidth(88.0f);
    cancelBtn->SetHeight(28.0f);
    cancelBtn->Connect(cancelBtn->Clicked, [&]() { dlg->Close(); });
    row->AddChild(cancelBtn);

    root->AddChild(row);

    dlg->Show();

    edit->Focus();
    edit->SetSelection(0, static_cast<int>(initial.size()));
    dlg->RunModal(modalOwner);

    const std::wstring text = edit->GetText();
    if (!confirmed || text.empty()) return false;
    *out = text;
    return true;
}

void ExplorerPane::DoNewFolder() {
    if (currentPath_.empty() || currentPath_ == L"::thispc") return;

    std::wstring name;
    if (!PromptName(L"新建文件夹", L"新建文件夹", &name)) return;

    const std::wstring path = MakeUniquePath(JoinPath(currentPath_, name));
    if (!app::TaskClient::Submit(L"newfolder", path, {})) {
        ShowMessage(L"新建失败", L"后台进程不可用", MessageBox::Icon::Error);
        return;
    }
    ScheduleRefresh();
}

void ExplorerPane::DoNewTextFile() {
    if (currentPath_.empty() || currentPath_ == L"::thispc") return;

    std::wstring name;
    if (!PromptName(L"新建文本文档", L"新建文本文档.txt", &name)) return;

    const std::wstring path = MakeUniquePath(JoinPath(currentPath_, name));
    if (!app::TaskClient::Submit(L"newfile", path, {})) {
        ShowMessage(L"新建失败", L"后台进程不可用", MessageBox::Icon::Error);
        return;
    }
    ScheduleRefresh();
}

void ExplorerPane::DoSelectAll() {
    if (currentPath_ == L"::recyclebin") {
        if (recycleTable_) recycleTable_->SelectAll();
        return;
    }
    if (list_) list_->SelectAll();
    UpdateSelectionSummary();
}

void ExplorerPane::DoSelectNone() {
    if (currentPath_ == L"::recyclebin") {
        if (recycleTable_) {
            for (int row : recycleTable_->GetCheckedRows())
                recycleTable_->SetRowChecked(row, false);
            recycleTable_->ClearSelection();
        }
        return;
    }
    if (list_) list_->ClearSelection();
    UpdateSelectionSummary();
}

void ExplorerPane::DoInvertSelection() {
    if (currentPath_ == L"::recyclebin") {
        if (!recycleTable_) return;
        std::unordered_set<int> previous;
        for (int row : SelectedRecycleRows()) previous.insert(row);
        for (int row : recycleTable_->GetCheckedRows())
            recycleTable_->SetRowChecked(row, false);
        recycleTable_->ClearSelection();
        for (int row = 0; row < static_cast<int>(recycleItems_.size()); ++row)
            if (!previous.count(row)) recycleTable_->SetRowChecked(row, true);
        return;
    }
    if (list_) list_->InvertSelection();
    UpdateSelectionSummary();
}

void ExplorerPane::DoProperties() {
    // 属性对话框只支持单个目标
    const auto paths = SelectedPaths();
    if (paths.empty()) return;
    ShowProperties(paths.front());
}

void ExplorerPane::DoEditAttributes() {
    const fs::FileEntry* entry = SelectedEntry();
    if (!entry) return;
    DoEditAttributesForPath(entry->fullPath);
}

void ExplorerPane::DoEditAttributesForPath(const std::wstring& path) {
    FileAttributesInfo attrs;
    std::wstring error;
    if (!ReadFileAttributes(path, &attrs, &error)) {
        ShowMessage(L"读取属性失败", error, MessageBox::Icon::Error);
        return;
    }
    const ZoneIdentifier zone = ReadZoneIdentifier(path);

    auto dlg = std::make_shared<Window>();
    if (!dlg->Create(680, 620, L"编辑文件属性")) return;
    dlg->SetMinSize(560, 480);

    auto root = dlg->GetRootColumnBox();
    root->SetMargin(Thickness(18, 16, 18, 16));
    root->SetSpacing(10.0f);

    auto scroll = std::make_shared<ScrollViewer>();
    scroll->SetFillWidth(true);
    scroll->SetFillHeight(true);
    auto content = std::make_shared<ColumnBox>();
    content->SetSpacing(10.0f);
    content->SetFillWidth(true);
    scroll->SetContent(content);
    root->AddChild(scroll);

    auto pathLabel = std::make_shared<Label>(path);
    pathLabel->SetTextOverflow(Label::TextOverflow::Ellipsis);
    pathLabel->SetToolTip(path);
    content->AddChild(pathLabel);

    FileLinkInfo linkInfo;
    if (QueryFileLink(path, &linkInfo, nullptr)) {
        std::wstring linkText;
        switch (linkInfo.kind) {
        case FileLinkKind::HardLink:
            linkText = L"文件链接：硬链接 · " +
                       std::to_wstring(linkInfo.hardLinkCount) + L" 个名称";
            break;
        case FileLinkKind::SymbolicFile:
            linkText = L"文件链接：符号链接 → " + linkInfo.target;
            break;
        case FileLinkKind::SymbolicDirectory:
            linkText = L"目录链接：符号链接 → " + linkInfo.target;
            break;
        case FileLinkKind::Junction:
            linkText = L"目录链接：Junction → " + linkInfo.target;
            break;
        case FileLinkKind::OtherReparsePoint:
            linkText = L"其他重解析点";
            break;
        case FileLinkKind::None:
            break;
        }
        if (!linkText.empty()) {
            auto linkLabel = std::make_shared<Label>(linkText);
            linkLabel->SetTextOverflow(Label::TextOverflow::Ellipsis);
            linkLabel->SetToolTip(linkText);
            linkLabel->SetTextColor(
                ThemeManager::AppTheme().Get(ThemeRole::TextSecondary));
            content->AddChild(linkLabel);
        }
    }

    content->AddChild(std::make_shared<Label>(L"基本属性"));
    auto addFlag = [&](const std::shared_ptr<ColumnBox>& parent,
                       const wchar_t* text, bool checked, bool enabled = true) {
        auto check = std::make_shared<CheckBox>(checked);
        check->SetEnabled(enabled);
        check->SetAccessibleName(text);
        auto row = std::make_shared<RowBox>();
        row->SetSpacing(6.0f);
        row->SetHeight(24.0f);
        row->AddChild(check);
        auto label = std::make_shared<Label>(text);
        label->SetFillWidth(true);
        label->SetEnabled(enabled);
        row->AddChild(label);
        parent->AddChild(row);
        return check;
    };
    auto basicRow = std::make_shared<RowBox>();
    basicRow->SetSpacing(24.0f);
    auto basicLeft = std::make_shared<ColumnBox>();
    basicLeft->SetSpacing(4.0f);
    basicLeft->SetFillWidth(true);
    auto basicRight = std::make_shared<ColumnBox>();
    basicRight->SetSpacing(4.0f);
    basicRight->SetFillWidth(true);
    basicRow->AddChild(basicLeft);
    basicRow->AddChild(basicRight);
    content->AddChild(basicRow);

    auto readOnly = addFlag(basicLeft, L"只读 (R)", attrs.readOnly);
    auto hidden = addFlag(basicLeft, L"隐藏 (H)", attrs.hidden);
    auto system = addFlag(basicLeft, L"系统 (S)", attrs.system);
    addFlag(basicLeft, L"目录", attrs.directory, false);
    auto archive = addFlag(basicRight, L"存档 (A)", attrs.archive);
    addFlag(basicRight, L"正常", attrs.normal, false);
    auto temporary = addFlag(basicRight, L"临时", attrs.temporary);
    auto compressed = addFlag(basicRight, L"压缩", attrs.compressed);

    auto advancedGroup = std::make_shared<ColumnBox>();
    advancedGroup->SetSpacing(8.0f);
    auto advancedRow = std::make_shared<RowBox>();
    advancedRow->SetSpacing(24.0f);
    auto advancedLeft = std::make_shared<ColumnBox>();
    advancedLeft->SetSpacing(4.0f);
    advancedLeft->SetFillWidth(true);
    auto advancedRight = std::make_shared<ColumnBox>();
    advancedRight->SetSpacing(4.0f);
    advancedRight->SetFillWidth(true);
    advancedRow->AddChild(advancedLeft);
    advancedRow->AddChild(advancedRight);
    advancedGroup->AddChild(advancedRow);
    addFlag(advancedLeft, L"设备", attrs.device, false);
    addFlag(advancedLeft, L"稀疏文件", attrs.sparse, false);
    addFlag(advancedLeft, L"重解析点", attrs.reparsePoint, false);
    addFlag(advancedLeft, L"脱机", attrs.offline, false);
    auto notContentIndexed = addFlag(advancedLeft, L"无内容索引",
                                     attrs.notContentIndexed);
    auto encrypted = addFlag(advancedLeft, L"加密", attrs.encrypted);
    addFlag(advancedRight, L"完整性流", attrs.integrityStream, false);
    addFlag(advancedRight, L"虚拟", attrs.virtualFile, false);
    addFlag(advancedRight, L"无清理数据", attrs.noScrubData, false);
    addFlag(advancedRight, L"扩展属性", attrs.ea, false);
    auto pinned = addFlag(advancedRight, L"固定", attrs.pinned);
    auto unpinned = addFlag(advancedRight, L"取消固定", attrs.unpinned);
    advancedGroup->AddChild(std::make_shared<Label>(
        L"灰显属性由系统/文件系统管理，仅展示，不允许强制写回。"));
    auto advancedCard = std::make_shared<Expander>(
        L"高级属性", L"系统/文件系统管理的标志会灰显");
    advancedCard->SetContent(advancedGroup);
    advancedCard->SetExpanded(false);
    content->AddChild(advancedCard);
    pinned->Connect(pinned->Toggled, [unpinned](bool on) {
        if (on) unpinned->SetChecked(false);
    });
    unpinned->Connect(unpinned->Toggled, [pinned](bool on) {
        if (on) pinned->SetChecked(false);
    });
    compressed->Connect(compressed->Toggled, [encrypted](bool on) {
        if (on) encrypted->SetChecked(false);
    });
    encrypted->Connect(encrypted->Toggled, [compressed](bool on) {
        if (on) compressed->SetChecked(false);
    });

    content->AddChild(std::make_shared<Label>(L"Zone.Identifier（下载来源/安全警告）"));
    auto zoneCombo = std::make_shared<ComboBox>();
    zoneCombo->AddItem(L"不设置（删除数据流）");
    zoneCombo->AddItem(L"本地计算机 (ZoneId=0)");
    zoneCombo->AddItem(L"可信站点 (ZoneId=2)");
    zoneCombo->AddItem(L"互联网 (ZoneId=3)");
    zoneCombo->AddItem(L"受限站点 (ZoneId=4)");
    zoneCombo->AddItem(L"自定义内容");
    zoneCombo->SetFillWidth(true);
    zoneCombo->SetHeight(32.0f);

    int zoneChoice = 0;
    if (zone.status == ZoneIdentifierStatus::Ok) {
        if (zone.content.find(L"ZoneId=0") != std::wstring::npos) zoneChoice = 1;
        else if (zone.content.find(L"ZoneId=2") != std::wstring::npos) zoneChoice = 2;
        else if (zone.content.find(L"ZoneId=3") != std::wstring::npos) zoneChoice = 3;
        else if (zone.content.find(L"ZoneId=4") != std::wstring::npos) zoneChoice = 4;
        else zoneChoice = 5;
    }
    zoneCombo->SetSelectedIndex(zoneChoice);
    content->AddChild(zoneCombo);

    auto zoneEdit = std::make_shared<TextEdit>();
    zoneEdit->SetPlaceholder(L"[ZoneTransfer]\r\nZoneId=3");
    zoneEdit->SetText(zone.status == ZoneIdentifierStatus::Ok ? zone.content : std::wstring());
    zoneEdit->SetHeight(150.0f);
    zoneEdit->SetFillWidth(true);
    zoneEdit->SetEnabled(zoneChoice == 5);
    content->AddChild(zoneEdit);

    if (attrs.directory) {
        zoneCombo->SetEnabled(false);
        zoneEdit->SetEnabled(false);
        content->AddChild(std::make_shared<Label>(L"目录不支持 Zone.Identifier 编辑。"));
    }
    zoneCombo->Connect(zoneCombo->SelectionChanged, [zoneEdit, attrs](int index) {
        zoneEdit->SetEnabled(!attrs.directory && index == 5);
    });

    if (!attrs.directory && zone.status == ZoneIdentifierStatus::Error) {
        auto warning = std::make_shared<Label>(
            L"读取 Zone.Identifier 失败：" + FormatWin32Error(zone.errorCode));
        content->AddChild(warning);
    }

    auto buttons = std::make_shared<RowBox>();
    buttons->SetSpacing(8.0f);
    buttons->SetHeight(30.0f);
    auto spacer = std::make_shared<Label>(L"");
    spacer->SetFillWidth(true);
    buttons->AddChild(spacer);

    bool confirmed = false;
    auto ok = std::make_shared<Button>(L"保存");
    ok->SetWidth(88.0f);
    ok->SetHeight(28.0f);
    ok->Connect(ok->Clicked, [&]() {
        confirmed = true;
        dlg->Close();
    });
    buttons->AddChild(ok);

    auto cancel = std::make_shared<Button>(L"取消");
    cancel->SetWidth(88.0f);
    cancel->SetHeight(28.0f);
    cancel->Connect(cancel->Clicked, [&]() { dlg->Close(); });
    buttons->AddChild(cancel);
    root->AddChild(buttons);

    dlg->Show();
    dlg->RunModal(window_);
    if (!confirmed) return;

    std::wstring saveError;
    // Temporarily clear read-only while changing compression/EFS, then apply the
    // final editable attribute set below.
    if (!SetEditableFileAttributes(path, false, hidden->IsChecked(), system->IsChecked(),
                                   archive->IsChecked(), temporary->IsChecked(),
                                   notContentIndexed->IsChecked(), pinned->IsChecked(),
                                   unpinned->IsChecked(), &saveError)) {
        ShowMessage(L"保存失败", saveError, MessageBox::Icon::Error);
        return;
    }
    if (compressed->IsChecked() != attrs.compressed &&
        !SetFileCompression(path, compressed->IsChecked(), &saveError)) {
        ShowMessage(L"保存压缩属性失败", saveError, MessageBox::Icon::Error);
        return;
    }
    if (encrypted->IsChecked() != attrs.encrypted &&
        !SetFileEncryption(path, encrypted->IsChecked(), &saveError)) {
        ShowMessage(L"保存加密属性失败", saveError, MessageBox::Icon::Error);
        return;
    }
    if (!SetEditableFileAttributes(path, readOnly->IsChecked(), hidden->IsChecked(),
                                   system->IsChecked(), archive->IsChecked(),
                                   temporary->IsChecked(), notContentIndexed->IsChecked(),
                                   pinned->IsChecked(), unpinned->IsChecked(), &saveError)) {
        ShowMessage(L"保存失败", saveError, MessageBox::Icon::Error);
        return;
    }

    if (!attrs.directory) {
        std::wstring content;
        switch (zoneCombo->GetSelectedIndex()) {
        case 1: content = L"[ZoneTransfer]\r\nZoneId=0\r\n"; break;
        case 2: content = L"[ZoneTransfer]\r\nZoneId=2\r\n"; break;
        case 3: content = L"[ZoneTransfer]\r\nZoneId=3\r\n"; break;
        case 4: content = L"[ZoneTransfer]\r\nZoneId=4\r\n"; break;
        case 5: content = zoneEdit->GetText(); break;
        default: break;
        }
        if (!WriteZoneIdentifier(path, content, &saveError)) {
            ShowMessage(L"保存失败", saveError, MessageBox::Icon::Error);
            return;
        }
    }

    ShowHint(L"文件属性已保存");
    Refresh();
}

void ExplorerPane::DoManageStreams() {
    const fs::FileEntry* entry = SelectedEntry();
    if (!entry || entry->isDir) return;
    DoManageStreamsForPath(entry->fullPath);
}

void ExplorerPane::DoManageStreamsForPath(const std::wstring& basePath) {
    auto dlg = std::make_shared<Window>();
    if (!dlg->Create(820, 600, L"NTFS 文件流 - " + FileNameOf(basePath))) return;
    dlg->SetMinSize(640, 460);

    auto root = dlg->GetRootColumnBox();
    root->SetMargin(Thickness(18, 16, 18, 16));
    root->SetSpacing(10.0f);

    auto pathLabel = std::make_shared<Label>(basePath);
    pathLabel->SetTextOverflow(Label::TextOverflow::Ellipsis);
    pathLabel->SetToolTip(basePath);
    root->AddChild(pathLabel);

    auto countLabel = std::make_shared<Label>(L"正在读取数据流…");
    root->AddChild(countLabel);

    auto streamList = std::make_shared<ListView>();
    streamList->SetFillHeight(true);
    streamList->SetItemHeight(28.0f);
    streamList->SetSelectionMode(ListView::SelectionMode::Single);
    streamList->SetEmptyText(L"没有可显示的数据流");
    root->AddChild(streamList);

    auto importBtn = std::make_shared<Button>(L"导入文件到流…");
    auto editBtn = std::make_shared<Button>(L"编辑/创建文本流…");
    auto exportBtn = std::make_shared<Button>(L"导出选中流…");
    auto deleteBtn = std::make_shared<Button>(L"删除选中流");
    auto renameBtn = std::make_shared<Button>(L"重命名流…");
    auto createBtn = std::make_shared<Button>(L"创建文件流");
    auto refreshBtn = std::make_shared<Button>(L"刷新");
    for (auto& button : { importBtn, editBtn, exportBtn, deleteBtn, renameBtn,
                          createBtn, refreshBtn }) {
        button->SetHeight(28.0f);
    }
    importBtn->SetWidth(116.0f);
    editBtn->SetWidth(132.0f);
    exportBtn->SetWidth(106.0f);
    deleteBtn->SetWidth(96.0f);
    renameBtn->SetWidth(96.0f);
    createBtn->SetWidth(100.0f);
    refreshBtn->SetWidth(64.0f);
    exportBtn->SetEnabled(false);
    deleteBtn->SetEnabled(false);
    renameBtn->SetEnabled(false);

    auto actions = std::make_shared<RowBox>();
    actions->SetSpacing(8.0f);
    actions->SetHeight(30.0f);
    actions->AddChild(createBtn);
    actions->AddChild(refreshBtn);
    auto actionSpacer = std::make_shared<Label>(L"");
    actionSpacer->SetFillWidth(true);
    actions->AddChild(actionSpacer);
    auto closeBtn = std::make_shared<Button>(L"关闭");
    closeBtn->SetWidth(80.0f);
    closeBtn->SetHeight(28.0f);
    actions->AddChild(closeBtn);
    root->AddChild(actions);

    auto streams = std::make_shared<std::vector<AlternateStream>>();
    auto selectedName = std::make_shared<std::wstring>();
    auto loadGen = std::make_shared<std::uint64_t>(0);
    auto refresh = std::make_shared<std::function<void()>>();
    auto updateButtons = std::make_shared<std::function<void()>>();
    auto streamMessage = [dlg](const std::wstring& title, const std::wstring& text,
                               MessageBox::Icon icon) {
        MessageBox box(dlg.get(), title, text, icon, FastButton::OK, true);
        (void)box;
    };

    *updateButtons = [streamList, streams, selectedName, exportBtn, deleteBtn, renameBtn]() {
        int index = streamList->GetSelectedIndex();
        bool named = index >= 0 && index < static_cast<int>(streams->size()) &&
                     !(*streams)[static_cast<size_t>(index)].isDefault;
        if (named) *selectedName = (*streams)[static_cast<size_t>(index)].name;
        else selectedName->clear();
        exportBtn->SetEnabled(named);
        deleteBtn->SetEnabled(named);
        renameBtn->SetEnabled(named);
    };

    *refresh = [dlg, basePath, streamList, countLabel, streams, selectedName, loadGen, updateButtons]() {
        const std::uint64_t gen = ++*loadGen;
        struct ListResult {
            bool ok = false;
            std::wstring error;
            std::vector<AlternateStream> streams;
        };
        RunAsync<ListResult>(
            AsyncOwner{ dlg },
            [basePath](AsyncCancelToken, AsyncProgress) {
                ListResult result;
                result.ok = ListAlternateStreams(basePath, &result.streams, &result.error);
                return result;
            },
            [gen, loadGen, streamList, countLabel, streams, selectedName, updateButtons](ListResult result) {
                if (gen != *loadGen) return;
                if (!result.ok) {
                    streams->clear();
                    auto empty = std::make_shared<const std::vector<AlternateStream>>();
                    streamList->SetItemProvider(0, [empty](int) { return std::wstring(); });
                    countLabel->SetText(L"读取数据流失败：" + result.error);
                    (*updateButtons)();
                    return;
                }

                *streams = result.streams;
                size_t namedCount = 0;
                std::uint64_t namedBytes = 0;
                for (const auto& stream : *streams) {
                    if (stream.isDefault) continue;
                    ++namedCount;
                    namedBytes += stream.size;
                }
                std::wstring size = FormatSize(namedBytes);
                if (size.empty()) size = L"0 B";
                countLabel->SetText(L"数据流 " + std::to_wstring(streams->size()) +
                                    L" 个（命名流 " + std::to_wstring(namedCount) +
                                    L" 个），命名流共 " + size);

                auto snapshot = std::make_shared<const std::vector<AlternateStream>>(*streams);
                streamList->SetItemProvider(
                    static_cast<int>(snapshot->size()),
                    [snapshot](int index) {
                        if (index < 0 || index >= static_cast<int>(snapshot->size()))
                            return std::wstring();
                        const auto& stream = (*snapshot)[static_cast<size_t>(index)];
                        std::wstring bytes = FormatSize(stream.size);
                        if (bytes.empty()) bytes = L"0 B";
                        return stream.isDefault
                            ? (L"  主数据流 (:$DATA)    " + bytes)
                            : (L"  :" + stream.name + L"    " + bytes);
                    });

                int select = -1;
                for (int i = 0; i < static_cast<int>(streams->size()); ++i) {
                    if (!selectedName->empty() &&
                        (*streams)[static_cast<size_t>(i)].name == *selectedName) {
                        select = i;
                        break;
                    }
                }
                if (select < 0 && !streams->empty()) select = 0;
                if (select >= 0) streamList->SetSelectedIndex(select);
                (*updateButtons)();
            },
            [gen, loadGen, countLabel, updateButtons](std::exception_ptr) {
                if (gen != *loadGen) return;
                countLabel->SetText(L"读取数据流失败");
                (*updateButtons)();
            });
    };

    streamList->Connect(streamList->SelectionChanged, [updateButtons](int) {
        (*updateButtons)();
    });

    importBtn->Connect(importBtn->Clicked, [this, dlg, basePath, selectedName,
                                            countLabel, streamMessage]() {
        const std::wstring streamName = *selectedName;
        if (streamName.empty()) return;
        FileDialogOptions options;
        options.title = L"选择要导入 NTFS 数据流的文件";
        auto picked = FileDialog::OpenOne(dlg->GetHwnd(), options);
        if (!picked) return;
        std::wstring error;
        if (!ValidateAlternateStreamName(streamName, &error)) {
            streamMessage(L"名称无效", error, MessageBox::Icon::Warning);
            return;
        }

        MessageBox overwrite(dlg.get(), L"导入文件内容",
                             L"导入会覆盖数据流 :" + streamName + L" 的现有内容，继续吗？",
                             MessageBox::Icon::Warning,
                             FastButton::Yes | FastButton::No, true);
        if (overwrite.GetResult() != FastButton::Yes) return;

        MessageBox question(dlg.get(), L"隐藏源文件",
                            L"导入成功后，是否把源文件移到回收站？\n\n"
                            L"选择“是”后普通目录中将不再看到该源文件，内容会藏在数据流中。",
                            MessageBox::Icon::Warning, FastButton::Yes | FastButton::No, true);
        const bool hideSource = question.GetResult() == FastButton::Yes;
        const std::wstring op = hideSource ? L"streamhide" : L"streamimport";
        if (!app::TaskClient::Submit(op, basePath, { streamName, *picked, L"1" })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
            return;
        }
        countLabel->SetText(hideSource ? L"已提交隐藏到数据流的任务…" : L"已提交导入数据流的任务…");
    });

    createBtn->Connect(createBtn->Clicked,
                       [this, dlg, basePath, streams, selectedName,
                        countLabel, streamMessage]() {
        std::wstring name = L"新数据流";
        if (!PromptName(L"创建文件流", name, &name, dlg.get())) return;
        std::wstring error;
        if (!ValidateAlternateStreamName(name, &error)) {
            streamMessage(L"名称无效", error, MessageBox::Icon::Warning);
            return;
        }
        for (const auto& stream : *streams) {
            if (!stream.isDefault && _wcsicmp(stream.name.c_str(), name.c_str()) == 0) {
                streamMessage(L"数据流已存在", L"该名称已有数据流，请换一个名称。",
                              MessageBox::Icon::Warning);
                return;
            }
        }
        if (!app::TaskClient::Submit(L"streamcreate", basePath, { name })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
            return;
        }
        *selectedName = name;
        countLabel->SetText(L"已提交创建文件流任务…");
    });

    editBtn->Connect(editBtn->Clicked, [this, dlg, basePath, streamList, streams,
                                         selectedName, countLabel, streamMessage]() {
        const int index = streamList->GetSelectedIndex();
        const bool hasSelected =
            (index >= 0 && index < static_cast<int>(streams->size()) &&
             !(*streams)[static_cast<size_t>(index)].isDefault);
        const std::wstring originalName =
            hasSelected ? (*streams)[static_cast<size_t>(index)].name : std::wstring();

        std::wstring initialName = originalName;
        std::wstring initialText;
        if (hasSelected) {
            std::wstring error;
            if (!ReadAlternateStreamText(basePath, originalName, &initialText, &error)) {
                streamMessage(L"无法快速编辑", error, MessageBox::Icon::Warning);
                return;
            }
        }

        auto editor = std::make_shared<Window>();
        if (!editor->Create(680, 520, L"编辑 NTFS 文本流")) return;
        auto editorRoot = editor->GetRootColumnBox();
        editorRoot->SetMargin(Thickness(16, 14, 16, 14));
        editorRoot->SetSpacing(8.0f);
        editorRoot->AddChild(std::make_shared<Label>(L"数据流名称（不含冒号）"));
        auto nameBox = std::make_shared<TextBox>();
        nameBox->SetText(initialName);
        nameBox->SetHeight(30.0f);
        editorRoot->AddChild(nameBox);
        editorRoot->AddChild(std::make_shared<Label>(L"文本内容（最多 24KB）"));
        auto contentBox = std::make_shared<TextEdit>();
        contentBox->SetText(initialText);
        contentBox->SetFillHeight(true);
        contentBox->SetFillWidth(true);
        editorRoot->AddChild(contentBox);

        auto row = std::make_shared<RowBox>();
        row->SetSpacing(8.0f);
        row->SetHeight(30.0f);
        auto spacer = std::make_shared<Label>(L"");
        spacer->SetFillWidth(true);
        row->AddChild(spacer);
        bool save = false;
        auto saveBtn = std::make_shared<Button>(L"保存");
        saveBtn->SetWidth(84.0f);
        saveBtn->SetHeight(28.0f);
        saveBtn->Connect(saveBtn->Clicked, [&]() { save = true; editor->Close(); });
        row->AddChild(saveBtn);
        auto cancelBtn = std::make_shared<Button>(L"取消");
        cancelBtn->SetWidth(84.0f);
        cancelBtn->SetHeight(28.0f);
        cancelBtn->Connect(cancelBtn->Clicked, [&]() { editor->Close(); });
        row->AddChild(cancelBtn);
        editorRoot->AddChild(row);

        editor->Show();
        editor->RunModal(dlg.get());
        if (!save) return;

        std::wstring name = nameBox->GetText();
        std::wstring text = contentBox->GetText();
        std::wstring error;
        if (!ValidateAlternateStreamName(name, &error)) {
            streamMessage(L"名称无效", error, MessageBox::Icon::Warning);
            return;
        }
        if (text.size() > 24u * 1024u) {
            streamMessage(L"内容过大", L"快速编辑最多支持 24KB，请使用导入/导出功能。",
                          MessageBox::Icon::Warning);
            return;
        }
        for (const auto& stream : *streams) {
            if (!stream.isDefault && _wcsicmp(stream.name.c_str(), name.c_str()) == 0 &&
                (!hasSelected || _wcsicmp(originalName.c_str(), name.c_str()) != 0)) {
                streamMessage(L"数据流已存在", L"该名称已有数据流，请换一个名称。",
                              MessageBox::Icon::Warning);
                return;
            }
        }
        if (!app::TaskClient::Submit(L"streamwrite", basePath, { name, text })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
            return;
        }
        *selectedName = name;
        countLabel->SetText(L"已提交文本流写入任务…");
    });

    exportBtn->Connect(exportBtn->Clicked, [this, dlg, basePath, streamList, streams, streamMessage]() {
        const int index = streamList->GetSelectedIndex();
        if (index < 0 || index >= static_cast<int>(streams->size())) return;
        if ((*streams)[static_cast<size_t>(index)].isDefault) return;
        const std::wstring streamName = (*streams)[static_cast<size_t>(index)].name;

        FileDialogOptions options;
        options.title = L"导出 NTFS 数据流";
        options.defaultFileName = streamName;
        auto destination = FileDialog::SaveOne(dlg->GetHwnd(), options);
        if (!destination) return;
        if (!app::TaskClient::Submit(L"streamexport", basePath,
                                     { streamName, *destination })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
        }
    });

    deleteBtn->Connect(deleteBtn->Clicked, [this, dlg, basePath, streamList, streams, streamMessage]() {
        const int index = streamList->GetSelectedIndex();
        if (index < 0 || index >= static_cast<int>(streams->size())) return;
        if ((*streams)[static_cast<size_t>(index)].isDefault) return;
        const std::wstring streamName = (*streams)[static_cast<size_t>(index)].name;

        MessageBox confirm(dlg.get(), L"删除数据流",
                           L"确定删除数据流 :" + streamName + L" 吗？此操作不可恢复。",
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;
        if (!app::TaskClient::Submit(L"streamdelete", basePath, { streamName })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
        }
    });

    renameBtn->Connect(renameBtn->Clicked, [this, dlg, basePath, streamList, streams,
                                             selectedName, countLabel, streamMessage]() {
        const int index = streamList->GetSelectedIndex();
        if (index < 0 || index >= static_cast<int>(streams->size())) return;
        if ((*streams)[static_cast<size_t>(index)].isDefault) return;
        const std::wstring oldName = (*streams)[static_cast<size_t>(index)].name;

        std::wstring newName = oldName;
        if (!PromptName(L"重命名数据流", oldName, &newName, dlg.get())) return;
        std::wstring error;
        if (!ValidateAlternateStreamName(newName, &error)) {
            streamMessage(L"名称无效", error, MessageBox::Icon::Warning);
            return;
        }
        for (const auto& other : *streams) {
            if (!other.isDefault && _wcsicmp(other.name.c_str(), newName.c_str()) == 0) {
                streamMessage(L"数据流已存在", L"该名称已有数据流，请换一个名称。",
                              MessageBox::Icon::Warning);
                return;
            }
        }
        if (!app::TaskClient::Submit(L"streamrename", basePath,
                                     { oldName, newName })) {
            streamMessage(L"提交失败", L"后台进程不可用", MessageBox::Icon::Error);
            return;
        }
        *selectedName = newName;
        countLabel->SetText(L"已提交数据流重命名任务…");
    });

    streamList->SetContextMenuFactory(
        [streamList, streams, renameBtn, deleteBtn, editBtn, importBtn, exportBtn]() {
            const int index = streamList->GetSelectedIndex();
            const bool named =
                index >= 0 && index < static_cast<int>(streams->size()) &&
                !(*streams)[static_cast<size_t>(index)].isDefault;
            auto menu = std::make_shared<Menu>();
            auto add = [&](const wchar_t* text, const std::shared_ptr<Button>& button,
                           bool danger = false) {
                menu->AddItem(text, [button]() { button->Clicked(); });
                auto item = menu->items.back();
                item->enabled = named;
                item->danger = danger;
            };
            add(L"重命名", renameBtn);
            add(L"删除", deleteBtn, true);
            menu->AddSeparator();
            add(L"文本编辑", editBtn);
            add(L"导入文件内容…", importBtn);
            add(L"导出为文件…", exportBtn);
            return menu;
        });

    refreshBtn->Connect(refreshBtn->Clicked, [refresh]() { (*refresh)(); });
    closeBtn->Connect(closeBtn->Clicked, [dlg]() { dlg->Close(); });
    auto timer = dlg->CreateTimer(900);
    timer->Tick.connect([refresh]() { (*refresh)(); });
    timer->Start(900);

    (*refresh)();
    dlg->Show();
    dlg->RunModal(window_);
    Refresh();
}

void ExplorerPane::DoUnlockFile() {
    const fs::FileEntry* entry = SelectedEntry();
    if (!entry) return;
    DoUnlockFileForPath(entry->fullPath);
}

void ExplorerPane::DoUnlockFileForPath(const std::wstring& path) {
    auto dlg = std::make_shared<Window>();
    if (!dlg->Create(800, 520, L"解除占用 - " + FileNameOf(path))) return;
    dlg->SetMinSize(620, 400);
    auto root = dlg->GetRootColumnBox();
    root->SetMargin(Thickness(18, 16, 18, 16));
    root->SetSpacing(10.0f);
    auto pathLabel = std::make_shared<Label>(path);
    pathLabel->SetTextOverflow(Label::TextOverflow::Ellipsis);
    pathLabel->SetToolTip(path);
    root->AddChild(pathLabel);
    auto status = std::make_shared<Label>(L"正在扫描全系统文件句柄，并核对 Restart Manager…");
    status->SetTextOverflow(Label::TextOverflow::Wrap);
    status->SetMaxLines(3);
    root->AddChild(status);

    auto list = std::make_shared<ListView>();
    list->SetFillHeight(true);
    list->SetItemHeight(28.0f);
    list->SetSelectionMode(ListView::SelectionMode::Single);
    list->SetEmptyText(L"没有检测到占用进程");
    root->AddChild(list);
    auto selection = std::make_shared<Label>(L"未选择进程");
    root->AddChild(selection);

    auto actions = std::make_shared<RowBox>();
    actions->SetSpacing(8.0f);
    actions->SetHeight(30.0f);
    auto refreshBtn = std::make_shared<Button>(L"刷新");
    refreshBtn->SetWidth(76.0f);
    refreshBtn->SetHeight(28.0f);
    auto killBtn = std::make_shared<Button>(L"结束选中进程");
    killBtn->SetWidth(120.0f);
    killBtn->SetHeight(28.0f);
    killBtn->SetEnabled(false);
    auto closeHandleBtn = std::make_shared<Button>(L"关闭全部句柄");
    closeHandleBtn->SetWidth(120.0f);
    closeHandleBtn->SetHeight(28.0f);
    closeHandleBtn->SetEnabled(false);
    actions->AddChild(refreshBtn);
    actions->AddChild(closeHandleBtn);
    actions->AddChild(killBtn);
    auto spacer = std::make_shared<Label>(L"");
    spacer->SetFillWidth(true);
    actions->AddChild(spacer);
    auto closeBtn = std::make_shared<Button>(L"关闭");
    closeBtn->SetWidth(80.0f);
    closeBtn->SetHeight(28.0f);
    actions->AddChild(closeBtn);
    root->AddChild(actions);

    struct LockResult {
        bool ok = false;
        std::wstring error;
        std::vector<LockingProcess> items;
        FileLockReport report;
    };
    auto processes = std::make_shared<std::vector<LockingProcess>>();
    auto selectedPid = std::make_shared<DWORD>(0);
    auto selectedName = std::make_shared<std::wstring>();
    auto selectedProcess = std::make_shared<std::optional<LockingProcess>>();
    auto rmOnlySeen = std::make_shared<std::unordered_map<DWORD, unsigned>>();
    auto loadGen = std::make_shared<std::uint64_t>(0);
    auto refresh = std::make_shared<std::function<void()>>();

    auto updateSelection = [list, processes, selectedPid, selectedName,
                            selectedProcess, killBtn, closeHandleBtn, selection]() {
        const int index = list->GetSelectedIndex();
        if (index < 0 || index >= static_cast<int>(processes->size())) {
            *selectedPid = 0;
            selectedName->clear();
            selectedProcess->reset();
            killBtn->SetEnabled(false);
            closeHandleBtn->SetEnabled(false);
            selection->SetText(L"未选择进程");
            return;
        }
        const auto& process = (*processes)[static_cast<size_t>(index)];
        *selectedPid = process.pid;
        *selectedName = process.name;
        *selectedProcess = process;
        killBtn->SetEnabled(process.pid != 0 && process.pid != 4 &&
                            process.pid != GetCurrentProcessId());
        closeHandleBtn->SetEnabled(process.ClosableHandleCount() > 0);
        selection->SetText(L"选中：" + process.name + L" · PID " +
                           std::to_wstring(process.pid) + L" · " +
                           std::to_wstring(process.handles.size()) + L" 个句柄 · " +
                           process.accessSummary);
    };
    list->Connect(list->SelectionChanged, [updateSelection](int) { updateSelection(); });

    *refresh = [dlg, path, list, status, processes, selectedPid, selectedProcess,
                rmOnlySeen, loadGen, updateSelection]() {
        const std::uint64_t gen = ++*loadGen;
        RunAsync<LockResult>(
            AsyncOwner{ dlg },
            [path](AsyncCancelToken, AsyncProgress) {
                LockResult result;
                result.ok = EnumerateFileLocks(path, &result.items, &result.error,
                                               &result.report);
                return result;
            },
            [gen, loadGen, list, status, processes, selectedPid, selectedProcess,
             rmOnlySeen, updateSelection](LockResult result) {
                if (gen != *loadGen) return;
                if (!result.ok) {
                    status->SetText(L"检查失败：" + result.error);
                    processes->clear();
                    selectedProcess->reset();
                    rmOnlySeen->clear();
                    list->SetItemProvider(0, [](int) { return std::wstring(); });
                    updateSelection();
                    return;
                }

                // Restart Manager can emit short-lived processes while it is
                // opening/registering resources. A process with no concrete
                // handle must survive two polls before it enters the list.
                std::unordered_map<DWORD, unsigned> nextSeen;
                std::erase_if(result.items, [&](const LockingProcess& process) {
                    if (!process.handles.empty() || !process.foundByRestartManager)
                        return false;
                    const auto it = rmOnlySeen->find(process.pid);
                    const unsigned count =
                        it == rmOnlySeen->end() ? 1u : it->second + 1u;
                    nextSeen[process.pid] = count;
                    return count < 2u;
                });
                *rmOnlySeen = std::move(nextSeen);
                *processes = std::move(result.items);
                size_t handleCount = 0;
                for (const auto& process : *processes)
                    handleCount += process.handles.size();
                std::wstring summary;
                if (result.report.handleScanCompleted) {
                    summary = L"全系统句柄扫描：" +
                              std::to_wstring(processes->size()) + L" 个进程 / " +
                              std::to_wstring(handleCount) + L" 个句柄";
                } else {
                    summary = L"全系统句柄扫描不可用";
                }
                if (result.report.restartManagerCompleted)
                    summary += L" · Restart Manager 已核对";
                if (result.report.deleteProbeAttempted) {
                    if (result.report.deleteOpenSucceeded) {
                        summary += L" · 当前可打开删除句柄";
                    } else if (result.report.deleteProbeError == ERROR_SHARING_VIOLATION) {
                        summary += L" · 共享引用正在阻止删除";
                    } else {
                        summary += L" · 删除探测：" +
                                   FormatWin32Error(result.report.deleteProbeError);
                    }
                }
                if (!result.report.warning.empty())
                    summary += L" · " + result.report.warning;
                status->SetText(summary);
                auto snapshot = std::make_shared<const std::vector<LockingProcess>>(*processes);
                list->SetItemProvider(
                    static_cast<int>(snapshot->size()),
                    [snapshot](int index) {
                        if (index < 0 || index >= static_cast<int>(snapshot->size()))
                            return std::wstring();
                        const auto& process = (*snapshot)[static_cast<size_t>(index)];
                        std::wstring handleText = L"仅 Restart Manager 报告";
                        if (!process.handles.empty()) {
                            wchar_t firstHandle[32]{};
                            swprintf_s(firstHandle, L"0x%llX",
                                       static_cast<unsigned long long>(
                                           process.handles.front().value));
                            handleText = std::to_wstring(process.handles.size()) +
                                         L" 个句柄 · " + process.accessSummary +
                                         L" · " + firstHandle;
                            if (process.handles.size() > 1) handleText += L"…";
                        }
                        return L"PID " + std::to_wstring(process.pid) + L"    " +
                               process.name + L"    [" + process.applicationType + L"]    " +
                               process.path + L"    " + handleText;
                    });
                int select = -1;
                for (int i = 0; i < static_cast<int>(processes->size()); ++i) {
                    if ((*processes)[static_cast<size_t>(i)].pid == *selectedPid) {
                        select = i;
                        break;
                    }
                }
                if (select < 0 && !processes->empty()) select = 0;
                if (select >= 0) list->SetSelectedIndex(select);
                updateSelection();
            },
            [gen, loadGen, status, updateSelection](std::exception_ptr) {
                if (gen != *loadGen) return;
                status->SetText(L"检查占用进程失败");
                updateSelection();
            });
    };

    auto submitCloseHandles =
        std::make_shared<std::function<void(const LockingProcess&,
                                             const std::vector<ULONG_PTR>&)>>();
    *submitCloseHandles = [dlg, status](const LockingProcess& process,
                                        const std::vector<ULONG_PTR>& handles) {
        if (process.pid == 0 || process.pid == 4 || handles.empty()) {
            MessageBox box(dlg.get(), L"句柄已失效",
                           L"当前选择没有可关闭的进程 PID 或文件句柄。\n"
                           L"请刷新列表后重新选择。",
                           MessageBox::Icon::Warning, FastButton::OK, true);
            (void)box;
            return;
        }

        std::wstring message = L"确定关闭进程 " + process.name + L"（PID " +
                               std::to_wstring(process.pid) + L"）持有的 " +
                               std::to_wstring(handles.size()) + L" 个文件句柄吗？";
        if (handles.size() == 1) {
            wchar_t handleText[32]{};
            swprintf_s(handleText, L"0x%llX",
                       static_cast<unsigned long long>(handles.front()));
            message += L"\n\n句柄 " + std::wstring(handleText);
        }
        MessageBox confirm(dlg.get(), L"关闭远程文件句柄", message,
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;

        std::vector<std::wstring> args;
        args.reserve(handles.size() + 1);
        args.push_back(std::to_wstring(process.pid));
        for (ULONG_PTR handle : handles) {
            if (handle != 0) {
                args.push_back(std::to_wstring(
                    static_cast<unsigned long long>(handle)));
            }
        }
        if (args.size() < 2) return;
        if (!app::TaskClient::Submit(L"closehandle", L"", args)) {
            MessageBox box(dlg.get(), L"提交失败", L"后台进程不可用",
                           MessageBox::Icon::Error, FastButton::OK, true);
            (void)box;
            return;
        }
        status->SetText(L"已提交关闭 " + std::to_wstring(args.size() - 1) +
                        L" 个文件句柄…");
    };

    auto submitKillProcess =
        std::make_shared<std::function<void(const LockingProcess&)>>();
    *submitKillProcess = [dlg, status](const LockingProcess& process) {
        if (process.pid == 0 || process.pid == 4 ||
            process.pid == GetCurrentProcessId()) {
            return;
        }
        MessageBox confirm(dlg.get(), L"结束占用进程",
                           L"确定结束 " + process.name + L"（PID " +
                           std::to_wstring(process.pid) + L"）吗？\n\n"
                           L"可能导致该程序未保存的数据丢失。",
                           MessageBox::Icon::Warning,
                           FastButton::Yes | FastButton::No, true);
        if (confirm.GetResult() != FastButton::Yes) return;
        if (!app::TaskClient::Submit(L"killprocess", L"",
                                     { std::to_wstring(process.pid) })) {
            MessageBox box(dlg.get(), L"提交失败", L"后台进程不可用",
                           MessageBox::Icon::Error, FastButton::OK, true);
            (void)box;
            return;
        }
        status->SetText(L"已提交结束进程任务…");
    };

    list->Connect(list->ItemRightClicked,
                  [list](int row) { list->SetSelectedIndex(row); });
    list->SetContextMenuFactory(
        [list, processes, submitCloseHandles, submitKillProcess]() {
            auto menu = std::make_shared<Menu>();
            const int row = list->GetContextRow();
            if (row < 0 || row >= static_cast<int>(processes->size())) {
                menu->AddItem(L"没有可操作的占用进程", []() {});
                menu->items.back()->enabled = false;
                return menu;
            }

            const LockingProcess process = (*processes)[static_cast<size_t>(row)];
            auto add = [&](const std::wstring& text,
                           std::function<void()> action,
                           bool enabled, bool danger = false) {
                menu->AddItem(text, std::move(action));
                auto item = menu->items.back();
                item->enabled = enabled;
                item->danger = danger;
            };

            std::vector<ULONG_PTR> closableHandles;
            for (const auto& handle : process.handles) {
                if (!handle.closable || handle.value == 0) continue;
                closableHandles.push_back(handle.value);
                wchar_t handleText[32]{};
                swprintf_s(handleText, L"0x%llX",
                           static_cast<unsigned long long>(handle.value));
                add(L"关闭句柄 " + std::wstring(handleText),
                    [submitCloseHandles, process, value = handle.value]() {
                        (*submitCloseHandles)(process, { value });
                    },
                    true);
            }
            if (closableHandles.size() > 1) {
                menu->AddSeparator();
                add(L"关闭此进程的全部 " +
                        std::to_wstring(closableHandles.size()) + L" 个句柄",
                    [submitCloseHandles, process, closableHandles]() {
                        (*submitCloseHandles)(process, closableHandles);
                    },
                    true);
            }
            menu->AddSeparator();
            add(L"结束此进程",
                [submitKillProcess, process]() { (*submitKillProcess)(process); },
                process.pid != 0 && process.pid != 4 &&
                    process.pid != GetCurrentProcessId(),
                true);
            return menu;
        });

    refreshBtn->Connect(refreshBtn->Clicked, [refresh]() { (*refresh)(); });
    closeBtn->Connect(closeBtn->Clicked, [dlg]() { dlg->Close(); });
    killBtn->Connect(killBtn->Clicked,
                     [selectedProcess, submitKillProcess]() {
        if (selectedProcess->has_value())
            (*submitKillProcess)(**selectedProcess);
    });
    closeHandleBtn->Connect(
        closeHandleBtn->Clicked,
        [selectedProcess, submitCloseHandles]() {
            if (!selectedProcess->has_value()) return;
            const LockingProcess& process = **selectedProcess;
            std::vector<ULONG_PTR> handles;
            for (const auto& handle : process.handles) {
                if (handle.closable && handle.value != 0)
                    handles.push_back(handle.value);
            }
            (*submitCloseHandles)(process, handles);
        });

    auto timer = dlg->CreateTimer(1000);
    timer->Tick.connect([refresh]() { (*refresh)(); });
    timer->Start(1000);
    (*refresh)();
    dlg->Show();
    dlg->RunModal(window_);
    Refresh();
}

// 任务在后台跑，UI 不能确定何时完成：稍等一拍再刷新目录。
void ExplorerPane::ScheduleRefresh() {
    if (!window_) return;
    if (!refreshTimer_) {
        refreshTimer_ = window_->CreateTimer(700);
        refreshTimer_->Tick.connect([this]() {
            if (refreshTimer_) refreshTimer_->Stop();
            Refresh();
        });
    }
    refreshTimer_->Start(700);
}

void ExplorerPane::UpdateTaskStrip() {
    std::vector<app::TaskInfo> tasks;
    if (!app::TaskClient::Query(&tasks)) {
        if (taskBtn_) {
            if (auto* b = dynamic_cast<TaskProgressButton*>(taskBtn_.get())) b->SetProgress(0.0f);
            taskBtn_->SetText(L"任务");
        }
        if (window_) platform::ClearTaskbarProgress(window_->GetHwnd());
        return;
    }

    int pending = 0;
    float sum = 0.0f;
    for (const auto& t : tasks) {
        if (t.originPid != GetCurrentProcessId()) continue;
        if (t.state <= 1) { ++pending; sum += t.ratio; }
        if ((t.state == 3 || t.state == 5) && t.issueCount > 0) {
            const auto it = reportedTaskIssues_.find(t.id);
            const int reported = it == reportedTaskIssues_.end() ? 0 : it->second;
            if (t.issueCount > reported) {
                std::wstring body = L"任务：" + t.title;
                body += L"\n任务 ID：" + std::to_wstring(t.id);
                body += L"\n状态：" +
                        std::wstring(t.state == 3 ? L"执行失败" : L"等待处理");
                if (!t.op.empty()) body += L"\n操作类型：" + t.op;
                if (!t.targetDir.empty()) body += L"\n目标：" + t.targetDir;
                if (!t.current.empty()) body += L"\n当前位置：" + t.current;
                if (t.originPid != 0) {
                    body += L"\n任务来源 PID：" + std::to_wstring(t.originPid);
                }
                if (!t.originPath.empty()) {
                    body += L"\n任务来源进程：" + t.originPath;
                }
                if (t.totalFiles > 0) {
                    body += L"\n进度：" + std::to_wstring(t.doneFiles) + L" / " +
                            std::to_wstring(t.totalFiles) + L" 个文件";
                }
                body += L"\n问题数量：" + std::to_wstring(t.issueCount);
                if (!t.issueDetails.empty()) {
                    body += L"\n\n──────── 错误详情 ────────\n" + t.issueDetails;
                } else if (!t.firstIssue.empty()) {
                    body += L"\n\n──────── 错误详情 ────────\n" + t.firstIssue;
                }
                if (t.issueCount > 1 && t.issueDetails.empty()) {
                    body += L"\n\n另有 " + std::to_wstring(t.issueCount - 1) +
                            L" 项问题。";
                }
                body += L"\n\n点击工具栏「任务」可以查看可操作详情。";
                Notify(t.state == 3 ? L"任务执行失败" : L"任务需要处理",
                       body, t.state == 3 ? NoticeKind::Error
                                          : NoticeKind::Warning);
                reportedTaskIssues_[t.id] = t.issueCount;
            }
        } else if (t.state == 2 || t.state == 4) {
            reportedTaskIssues_.erase(t.id);
        }
    }

    if (taskBtn_) {
        if (auto* b = dynamic_cast<TaskProgressButton*>(taskBtn_.get())) {
            b->SetProgress(pending > 0 ? (sum / static_cast<float>(pending)) : 0.0f);
        }
        taskBtn_->SetText(pending > 0 ? (L"任务 " + std::to_wstring(pending)) : std::wstring(L"任务"));
    }

    // 全部结束 -> 目录可能变了，刷新一次
    if (lastPending_ > 0 && pending == 0) Refresh();
    lastPending_ = pending;
    if (window_) {
        platform::SetTaskbarProgress(
            window_->GetHwnd(), pending > 0,
            pending > 0 ? static_cast<double>(sum / static_cast<float>(pending)) : 0.0);
    }
}
void ExplorerPane::ShowTaskMenu() {
    std::vector<app::TaskInfo> tasks;
    if (!app::TaskClient::Query(&tasks)) {
        Notify(L"任务服务不可用", L"ZExplorer 后台进程没有响应。",
               NoticeKind::Error);
        return;
    }

    auto menu = std::make_shared<Menu>();
    int shown = 0;
    for (const auto& t : tasks) {
        if (t.originPid != GetCurrentProcessId()) continue;
        if (t.state == 2) continue;          // 已完成的不显示

        wchar_t buf[320];
        if (t.state == 3) {
            swprintf_s(buf, L"⚠ %s  —  失败 %d 项：%s", t.title.c_str(), t.issueCount,
                       t.firstIssue.c_str());
        } else {
            const wchar_t* state = (t.state == 0) ? L"排队" : (t.state == 1) ? L"进行中" : L"已取消";
            swprintf_s(buf, L"%s  —  %s %d%%", t.title.c_str(), state,
                       static_cast<int>(t.ratio * 100.0f));
        }
        const int id = t.id;
        menu->AddItem(buf, [id]() { app::TaskClient::ShowTask(id); });
        ++shown;
    }

    if (shown == 0) {
        menu->AddItem(L"没有进行中的任务", nullptr);
    }
    menu->ShowAtCursor();
}
void ExplorerPane::AttachToWindow(Window* window) {
    if (!window) return;
    window_ = window;
    window->SizeChanged.connect([this](float, float) {
        ScheduleBreadcrumbRebuild();
    });
    window->SetDropTargetEnabled(true);
    window->DragEnter.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    window->DragOver.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, false);
    });
    window->Drop.connect([this](DragEventArgs& e) {
        HandleExternalDrag(e, true);
    });
    // 任务条轮询：后台在跑什么、进度多少，都在这里汇总
    taskTimer_ = window->CreateTimer(600);
    taskTimer_->Tick.connect([this]() { UpdateTaskStrip(); });
    taskTimer_->Start(600);

    searchDebounceTimer_ = window->CreateTimer(220);
    searchDebounceTimer_->Tick.connect([this]() {
        if (searchDebounceTimer_) searchDebounceTimer_->Stop();
        StartRecursiveSearch();
    });

    drivesLayoutTimer_ = window->CreateTimer(120);
    drivesLayoutTimer_->Tick.connect([this]() {
        if (drivesLayoutTimer_) drivesLayoutTimer_->Stop();
        if (currentPath_ != L"::thispc" || !window_) return;
        const int columns =
            window_->GetClientWidthDip() >= 900.0f ? 3 : 2;
        if (columns != drivesColumns_) ShowDrivesPage();
    });
    window->SizeChanged.connect([this](float, float) {
        if (currentPath_ != L"::thispc") return;
        if (drivesLayoutTimer_) drivesLayoutTimer_->Start(120);
    });

    watchTimer_ = window->CreateTimer(250);
    watchTimer_->Tick.connect([this]() {
        if (directoryWatcher_ && directoryWatcher_->Poll()) Refresh();
    });
    watchTimer_->Start(250);

    // 快捷键（地址栏编辑时不拦截，交给 TextBox 自己）
    window->AddKeyHandler([this](int vk) -> bool {
        if (!window_) return false;
        if (addressBox_ && addressBox_->IsFocused()) return false;
        if (searchBox_ && searchBox_->IsFocused()) {
            if (vk == VK_ESCAPE) {
                ClearSearch();
                if (window_) window_->FocusElement(nullptr);
                else if (searchBox_) searchBox_->Blur();
                return true;
            }
            return false;
        }

        const bool ctrl = IsCtrlDown();
        const bool shift = IsShiftDown();
        const bool alt = IsAltDown();

        if (currentPath_ == L"::recyclebin") {
            if (vk == VK_RETURN) {
                InvokeSelectedRecycleVerb(alt ? L"properties" : L"open");
                return true;
            }
            if (vk == VK_F5) {
                ShowRecycleBinPage();
                return true;
            }
            if (vk == VK_DELETE) {
                InvokeSelectedRecycleVerb(L"delete");
                return true;
            }
            if (vk == VK_BACK) {
                GoUp();
                return true;
            }
            if (vk == 'A' && ctrl) {
                if (recycleTable_) recycleTable_->SelectAll();
                return true;
            }
            if (vk == 'C' && ctrl) {
                InvokeSelectedRecycleVerb(L"copy");
                return true;
            }
            if (vk == 'X' && ctrl) {
                InvokeSelectedRecycleVerb(L"cut");
                return true;
            }
            return false;
        }

        if (vk == VK_RETURN) {
            if (alt) DoProperties(); else DoOpenSelected();
            return true;
        }

        switch (vk) {
        case VK_F2: DoRename(); return true;
        case VK_F5: Refresh(); return true;
        case VK_DELETE: DoDelete(shift); return true;
        case VK_BACK: GoUp(); return true;
        case 'C':
            if (ctrl) {
                if (shift) DoCopyPath();
                else DoCopy(false);
                return true;
            }
            break;
        case 'X': if (ctrl) { DoCopy(true); return true; } break;
        case 'V': if (ctrl) { DoPaste(); return true; } break;
        case 'A': if (ctrl) { DoSelectAll(); return true; } break;
        case 'L': if (ctrl) { FocusAddress(); return true; } break;
        case 'N': if (ctrl && shift) { DoNewFolder(); return true; } break;
        case 'O': if (ctrl) { DoOpenWith(); return true; } break;
        default: break;
        }
        return false;
    });
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------
int ExplorerPane::ListRowCount() const {
    return list_ ? list_->RowCount() : 0;
}

int ExplorerPane::ListIconCount() const {
    return list_ ? list_->IconCount() : 0;
}

std::wstring ExplorerPane::IconDiagnostics() const {
    return list_ ? list_->IconDiagnostics() : L"no list";
}

std::wstring ExplorerPane::DumpLayout() const {
    auto line = [](const wchar_t* name, const ZufyUI::UIElement* e) -> std::wstring {
        if (!e) return std::wstring(name) + L": <null>\n";
        const auto r = e->GetArrangedRect();
        wchar_t buf[256];
        swprintf_s(buf, L"%-8ls x=%7.1f y=%7.1f w=%7.1f h=%7.1f vis=%d\n",
                   name, r.x, r.y, r.width, r.height, e->IsVisible() ? 1 : 0);
        return buf;
    };

    std::wstring out;
    out += line(L"工具栏", toolbar_.get());
    out += line(L"地址栏", addressHost_.get());
    out += line(L"面包屑", crumb_.get());
    out += line(L"分栏", split_.get());
    out += line(L"左栏堆叠", leftStack_.get());
    out += line(L"快速访问", quickPane_.get());
    out += line(L"导航树", tree_.get());
    out += line(L"右列", rightCol_.get());
    out += line(L"列表", list_ ? list_->Widget().get() : nullptr);
    return out;
}

} // namespace ze::ui
