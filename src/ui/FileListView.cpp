#include "pch.h"
#include "ui/FileListView.h"

#include "fs/ShellIcons.h"
#include "platform/PathUtil.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace ZufyUI;

namespace ze::ui {

namespace {

constexpr float kIconDpi = 16.0f;
constexpr size_t kPerFileIconLimit = 2000;   // 目录/exe/lnk 这类"每项一个图标"的缓存上限

// ---------------------------------------------------------------------------
// EagerFileListView —— TableView + 新版的单元格 Label provider
//
// 库在 4ad47dd 之后提供了 `SetCellLabelProvider(rows, cols, provider)`：
// 行按视口虚拟化，每格给一个真 Label，可以 SetText / SetImage。
// 于是图标、四列、滚动、选中、键盘导航全部由 TableView 负责，应用层只填内容。
// （此前因为 TableView 不画富单元格里的 Image，才临时自绘过一个控件；已删除。）
// ---------------------------------------------------------------------------
class DragTableView final : public TableView {
public:
    void SetDragPathsProvider(
        std::function<std::vector<std::wstring>()> provider) {
        dragPathsProvider_ = std::move(provider);
    }

    void OnMouseDown(float x, float y) override {
        previousChecked_ = GetCheckedRows();
        const int previousCurrent = GetCurrentRow();
        const CellHit hit = HitTestCellAt(x, y);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        TableView::OnMouseDown(x, y);
        if (shift) {
            // Shift+click is additive selection. Restore checked rows because
            // the current-cell update may synchronize away the previous
            // selection. Shift+drag remains a marquee selection.
            for (int row : previousChecked_) SetRowChecked(row, true);
            const int current = GetCurrentRow();
            if (current >= 0) SetRowChecked(current, true);
            dragArmed_ = false;
            return;
        }
        const int current = GetCurrentRow();
        const bool wasSelected =
            hit.row >= 0 && current >= 0 &&
            (hit.row == previousCurrent ||
             std::find(previousChecked_.begin(), previousChecked_.end(), hit.row) !=
                 previousChecked_.end());
        if (!dragPathsProvider_ || current < 0 || !wasSelected)
            return;

        // Plain drag is the direct drag-out gesture. Normal selection
        // marquee is reserved for Shift+drag.
        for (int row : previousChecked_) SetRowChecked(row, true);
        dragArmed_ = true;
        dragMoved_ = false;
        dragStartX_ = x;
        dragStartY_ = y;
    }

    void OnMouseMove(float x, float y) override {
        if (dragArmed_ && !dragMoved_ &&
            (fabs(x - dragStartX_) + fabs(y - dragStartY_) > 6.0f)) {
            dragMoved_ = true;
            dragArmed_ = false;
            const auto paths = dragPathsProvider_
                ? dragPathsProvider_()
                : std::vector<std::wstring>{};
            if (!paths.empty()) {
                // Close TableView's press/marquee state before entering the
                // OLE modal loop; otherwise the next drag would still be armed
                // as a marquee.
                TableView::OnMouseUp(x, y);
                DragDataBuilder builder;
                builder.AddFiles(paths);
                if (auto* window = GetWindow())
                    window->BeginDrag(builder, DROPEFFECT_COPY, this);
            }
            return;
        }
        TableView::OnMouseMove(x, y);
    }

    void OnMouseUp(float x, float y) override {
        dragArmed_ = false;
        dragMoved_ = false;
        TableView::OnMouseUp(x, y);
    }

private:
    std::function<std::vector<std::wstring>()> dragPathsProvider_;
    std::vector<int> previousChecked_;
    bool dragArmed_ = false;
    bool dragMoved_ = false;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;
};

class EagerFileListView final : public FileListView {
public:
    EagerFileListView() {
        table_ = std::make_shared<DragTableView>();
        table_->SetSelectionMode(TableView::SelectionMode::Row);

        // 多选：框选 + 勾选都打开，并让两者同步（框选会带上勾选状态）
        table_->SetMarqueeEnabled(true);
        table_->SetCheckable(true);
        table_->SetMarqueeCheckSync(true);

        // 勾选与选中双向靠近：点击/键盘改变选中时，把对应行也勾上
        // （拖动框选由 SetMarqueeCheckSync 负责，这条补上点击与键盘路径）
        // 点击行 -> 同时勾上（TableView 没有批量接口，只能对单行做；
        // 大面积选择交给 SetMarqueeCheckSync，逐行 SetRowChecked 会 O(n²) 卡死）
        table_->SelectionChangedCells.connect([this](std::vector<std::pair<int, int>> cells) {
            if (cells.size() == 1) table_->SetRowChecked(cells.front().first, true);
            if (onSelect_) onSelect_(selectedRow_);
        });

        table_->Connect(table_->CellDoubleClicked, [this](int row, int) {
            if (row >= 0 && row < static_cast<int>(entries_.size()) && onOpen_) onOpen_(row);
        });

        // 当前行可能由点击、键盘、框选或程序恢复改变；统一在这里同步给页面。
        table_->Connect(table_->CurrentCellChanged, [this](int row, int) {
            selectedRow_ = (row >= 0 && row < static_cast<int>(entries_.size())) ? row : -1;
            if (onSelect_) onSelect_(selectedRow_);
        });
        table_->Connect(table_->HeaderClicked, [this](int col) {
            if (onSortChanged_) onSortChanged_(col);
        });
    }

    std::shared_ptr<UIElement> Widget() override { return table_; }

    void SetEntries(const std::vector<fs::FileEntry>& entries) override {
        // A refresh must not undo a live header drag. Capture the real current
        // widths before the provider recreates the column model.
        if (table_ && table_->GetColumnCount() == 4) {
            for (int i = 0; i < 4; ++i)
                columnWidths_[static_cast<size_t>(i)] =
                    table_->GetColumnWidth(i);
        }

        // 排序/刷新只改变行顺序。先按完整路径记下选择，再在新顺序中恢复，
        // 避免 TableView 把旧行号直接映射到另一个文件。
        std::vector<std::wstring> selectedPaths;
        for (int row : SelectedRows()) {
            if (row >= 0 && row < static_cast<int>(entries_.size()))
                selectedPaths.push_back(entries_[static_cast<size_t>(row)].fullPath);
        }
        const int oldCurrent = table_->GetCurrentRow();
        const std::wstring primaryPath =
            (oldCurrent >= 0 && oldCurrent < static_cast<int>(entries_.size()))
                ? entries_[static_cast<size_t>(oldCurrent)].fullPath
                : std::wstring();
        const auto checkedBefore = table_->GetCheckedRows();

        table_->ClearSelection();
        for (int row : checkedBefore) table_->SetRowChecked(row, false);

        entries_ = entries;

        // 虚拟化：行数直接给全量，不再有 5000 行上限
        table_->SetCellLabelProvider(static_cast<int>(entries_.size()), 4,
            [this](int row, int col, Label& lb) { FillCell(row, col, lb); });

        // provider 会重设行列数，表头/列宽放在它后面设置
        ApplyHeaderLabels();
        for (int i = 0; i < 4; ++i)
            table_->SetColumnWidth(i, columnWidths_[static_cast<size_t>(i)]);
        table_->SetColumnAlignment(1, TextHAlign::Right);   // 大小右对齐

        if (!selectedPaths.empty()) {
            std::unordered_map<std::wstring, int> indexByPath;
            for (int row = 0; row < static_cast<int>(entries_.size()); ++row)
                indexByPath.emplace(entries_[static_cast<size_t>(row)].fullPath, row);

            int primary = -1;
            if (!primaryPath.empty()) {
                auto it = indexByPath.find(primaryPath);
                if (it != indexByPath.end()) primary = it->second;
            }
            if (primary < 0) {
                auto it = indexByPath.find(selectedPaths.front());
                if (it != indexByPath.end()) primary = it->second;
            }

            for (const auto& path : selectedPaths) {
                auto it = indexByPath.find(path);
                if (it != indexByPath.end()) table_->SetRowChecked(it->second, true);
            }
            if (primary >= 0) {
                table_->SelectRow(primary);
                table_->SetRowChecked(primary, true);
                table_->ScrollToCell(primary, 0);
            }
        }
    }

    void SetEmptyText(const std::wstring& text) override { emptyText_ = text; }

    int SelectedIndex() const override { return selectedRow_; }

    std::vector<int> SelectedRows() const override {
        std::vector<int> rows;
        if (!table_) return rows;

        std::unordered_set<int> seen;
        for (int r : table_->GetSelectedRows()) if (seen.insert(r).second) rows.push_back(r);
        for (int r : table_->GetCheckedRows()) if (seen.insert(r).second) rows.push_back(r);

        std::sort(rows.begin(), rows.end());
        return rows;
    }
    int RowCount() const override { return static_cast<int>(entries_.size()); }

    void SelectAll() override { if (table_) table_->SelectAll(); }

    void ClearSelection() override {
        if (!table_) return;
        for (int row : table_->GetCheckedRows()) table_->SetRowChecked(row, false);
        table_->ClearSelection();
        selectedRow_ = -1;
    }

    void InvertSelection() override {
        if (!table_) return;
        const auto previous = SelectedRows();
        std::unordered_set<int> old(previous.begin(), previous.end());
        for (int row : table_->GetCheckedRows()) table_->SetRowChecked(row, false);
        table_->ClearSelection();
        for (int row = 0; row < RowCount(); ++row)
            if (!old.count(row)) table_->SetRowChecked(row, true);
        selectedRow_ = -1;
    }

    int IconCount() const override {
        int n = 0;
        for (const auto& kv : iconCache_) if (kv.second) ++n;
        return n;
    }

    std::wstring IconDiagnostics() const override {
        if (!firstIcon_) return L"none";
        wchar_t buf[128];
        swprintf_s(buf, L"%dx%d null=%d", firstIcon_->Width(), firstIcon_->Height(),
                   firstIcon_->IsNull() ? 1 : 0);
        return buf;
    }

    void SetCallbacks(std::function<void(int)> onOpen,
                      std::function<void(int)> onSelect) override {
        onOpen_ = std::move(onOpen);
        onSelect_ = std::move(onSelect);
    }

    void SetSortChanged(std::function<void(int)> onSortChanged) override {
        onSortChanged_ = std::move(onSortChanged);
    }

    void SetSortIndicator(int column, bool ascending) override {
        sortColumn_ = column;
        sortAscending_ = ascending;
        ApplyHeaderLabels();
    }

    void SetHideKnownExtensions(bool hide) override {
        if (hideKnownExtensions_ == hide) return;
        hideKnownExtensions_ = hide;
        if (!entries_.empty()) SetEntries(entries_);
        else if (table_) table_->RequestRepaint();
    }

    void SetDragPathsProvider(
        std::function<std::vector<std::wstring>()> provider) override {
        dragPathsProvider_ = std::move(provider);
        if (table_) table_->SetDragPathsProvider(dragPathsProvider_);
    }

    void SetColumnWidths(const std::array<float, 4>& widths) override {
        if (!table_) return;
        for (int i = 0; i < 4; ++i) {
            const size_t index = static_cast<size_t>(i);
            columnWidths_[index] = (std::max)(TableCaptionMinWidth, widths[index]);
            table_->SetColumnWidth(i, columnWidths_[index]);
        }
    }

    std::array<float, 4> GetColumnWidths() const override {
        std::array<float, 4> widths{};
        if (!table_) return widths;
        for (int i = 0; i < 4; ++i) widths[static_cast<size_t>(i)] = table_->GetColumnWidth(i);
        return widths;
    }

private:
    void ApplyHeaderLabels() {
        if (!table_) return;
        std::vector<std::wstring> labels = { L"名称", L"大小", L"类型", L"修改日期" };
        if (sortColumn_ >= 0 && sortColumn_ < static_cast<int>(labels.size())) {
            labels[static_cast<size_t>(sortColumn_)] += sortAscending_ ? L" ▲" : L" ▼";
        }
        table_->SetHorizontalHeaderLabels(labels);
    }

    void FillCell(int row, int col, Label& lb) {
        if (row < 0 || row >= static_cast<int>(entries_.size())) return;
        const auto& e = entries_[static_cast<size_t>(row)];

        switch (col) {
        case 0: {
            std::wstring display = e.name;
            if (hideKnownExtensions_ && !e.isDir &&
                platform::IsKnownFileExtension(e.name)) {
                const size_t dot = display.find_last_of(L'.');
                if (dot != std::wstring::npos && dot > 0) display.resize(dot);
            }
            lb.SetTextFast(display);
            lb.SetTextOverflow(Label::TextOverflow::Ellipsis);
            lb.SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
            if (auto img = IconFor(e)) {
                lb.SetImage(img);
                lb.SetIconSize(kIconDpi, kIconDpi);
                lb.SetIconSpacing(6.0f);
            }
            break;
        }
        case 1:
            lb.SetTextFast(e.isDir ? std::wstring() : platform::FormatSize(e.size));
            break;
        case 2:
            lb.SetTextFast(e.typeName);
            break;
        case 3:
            lb.SetTextFast(platform::FormatFileTime(e.modified));
            break;
        default:
            break;
        }
    }

    std::shared_ptr<Image> IconFor(const fs::FileEntry& e) {
        const std::wstring ext = e.isDir ? std::wstring() : platform::ExtensionOf(e.name);

        // 目录 / exe / lnk / ico：每个条目有自己的图标（桌面、下载、文档各有专属图标），按路径缓存。
        // 其余文件按扩展名缓存（同类型共用一张，快）。
        const bool perFile = e.isDir || ext == L".exe" || ext == L".lnk" || ext == L".ico";
        const std::wstring key = perFile ? e.fullPath : ext;

        if (auto it = iconCache_.find(key); it != iconCache_.end()) return it->second;

        if (perFile && iconCache_.size() > kPerFileIconLimit) return nullptr;   // 超限降级

        HICON h = fs::ShellIcons::Instance().Get(e.isDir, e.fullPath, /*preferFileIcon*/ perFile,
                                                 fs::IconSize::Small);
        std::shared_ptr<Image> img;
        if (h) img = Image::FromHICON(h);
        if (!firstIcon_ && img) firstIcon_ = img;
        iconCache_.emplace(key, img);
        return img;
    }

    std::shared_ptr<DragTableView> table_;
    std::vector<fs::FileEntry> entries_;
    std::wstring emptyText_;
    int selectedRow_ = -1;
    std::function<void(int)> onOpen_;
    std::function<void(int)> onSelect_;
    std::function<void(int)> onSortChanged_;
    int sortColumn_ = -1;
    bool sortAscending_ = true;
    bool hideKnownExtensions_ = false;
    std::array<float, 4> columnWidths_ = { 380.0f, 110.0f, 170.0f, 180.0f };
    std::function<std::vector<std::wstring>()> dragPathsProvider_;
    std::unordered_map<std::wstring, std::shared_ptr<Image>> iconCache_;
    std::shared_ptr<Image> firstIcon_;
    static constexpr float TableCaptionMinWidth = 40.0f;
};

} // namespace

std::shared_ptr<FileListView> MakeEagerFileListView() {
    return std::make_shared<EagerFileListView>();
}

} // namespace ze::ui
