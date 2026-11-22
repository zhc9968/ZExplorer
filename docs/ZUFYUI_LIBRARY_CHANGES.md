# ZufyUI 库改动与 Hack 总账

最后更新：2026-11-22

本文件是 ZExplorer 对 `C:\project\ZufyUI` 所做修改的统一账本。以后修改 UI 库时直接修改并在这里追加记录，不再等待确认；但“为什么改、改了什么、影响是什么、怎么验证”必须同步写清楚。

## 基线与当前状态

- 上游基线：`ZufyUI@a4bab7c`，v1.29.2。
- 已提交的库改动：`7759891`、`00f25e3`，版本升为 v1.30.0。
- 当前尚未提交的库改动：`ZDataViewer.h`、`tests/zufyui_tests.cpp`。
- ZExplorer 使用的库快照：`third_party/ZufyUI/`。
- 当前验证：ZufyUI 自检 7491 项通过，ZExplorer 平台测试 216 项通过。

这里所说的“Hack”是库能力尚未正式抽象时，应用需求倒逼库增加的补丁或扩展。不是所有改动都应该永久保留；每项都记录了推荐状态。

## 文件级改动索引

| 文件 | 改动 |
|---|---|
| `ZufyUI.h` | 菜单横向行数据模型、菜单行布局/绘制/命中/子菜单/tooltip、Label 图标接口、文本宽度测量、菜单栏贴边 |
| `ZufyUIWidgets.h` | `Label` 图标绘制/着色/透明度、动态文字缓存失效、`MeasureHeightForWidth`、`MenuItem` 图标工厂 |
| `ZufyUI.cpp` | “右键菜单行”演示页与 30 行压力菜单 |
| `ZDataViewer.h` | `TableView` 精确单元格命中、无滚动恢复选择、滚动偏移读写 |
| `tests/zufyui_tests.cpp` | 横向菜单 API 和 `SelectRowNoScroll` 回归测试 |

## 1. 横向右键菜单行（v1.30.0，已提交）

### 目的

ZExplorer 需要把“打开、复制、剪切、粘贴、重命名、删除”等命令做成图标和文字横向并排的菜单行。原来的 `Menu` 只支持纵向普通项，因此库增加了 `MenuItem::Type::Row`、行布局参数和子项命中。

### 数据模型

文件：`ZufyUI.h`

```cpp
enum class MenuRowLayout { Inline, IconTop };
enum class MenuRowWidth { Equal, ContentFit };

struct MenuRowOptions {
    int columns = 0;
    float gap = 4.0f;
    float minItemWidth = 72.0f;
    float maxItemWidth = 180.0f;
    float inlineHeight = 34.0f;
    float iconTopHeight = 56.0f;
    float iconSize = 18.0f;
    float iconTopSize = 22.0f;
    MenuRowLayout layout = MenuRowLayout::Inline;
    MenuRowWidth width = MenuRowWidth::Equal;
    bool showShortcuts = false;
};
```

`MenuItem` 从 `Normal / Separator / Submenu` 扩展为四种类型：

```cpp
enum class Type { Normal, Separator, Submenu, Row };
std::vector<std::shared_ptr<MenuItem>> children;
MenuRowOptions rowOptions;
```

`Menu` 新增：

```cpp
static std::shared_ptr<MenuItem> MakeItem(
    const std::wstring& text,
    std::function<void()> callback = nullptr,
    int id = 0);

static std::shared_ptr<MenuItem> MakeSeparator();

void AddRow(
    std::vector<std::shared_ptr<MenuItem>> children,
    MenuRowOptions options = {});
```

### 菜单行布局

文件：`ZufyUI.h`，`MenuWindow::BuildLayout()`。

关键逻辑：

```cpp
const float normalRoom = max(0.0f, rowInner - gaps - sepTotal);
if (rowOptions.width == MenuRowWidth::Equal && normalCount > 0) {
    float each = normalRoom / (float)normalCount;
    // 每格均分
}
else if (normalCount > 0 && normalSum < normalRoom) {
    // ContentFit：按内容宽度分配，再用剩余空间均摊
}
else if (normalCount > 0 && normalSum > normalRoom) {
    // 空间不足时按比例压缩
}
```

原因：

- `Equal` 适合固定宽度的操作行。
- `ContentFit` 适合中文菜单，避免“打开方式…”被均分成“打开方...”。
- 行内允许插入 `MenuItem::Type::Separator`，绘制成竖向分隔线。

推荐状态：保留。这是通用菜单能力，不应视为永久 Hack。

## 2. 菜单项图标与文字绘制能力（v1.30.0，已提交）

文件：`ZufyUI.h`、`ZufyUIWidgets.h`

`UIElement` 新增用于把控件当作菜单图标的接口：

```cpp
virtual void DrawAsIcon(ID2D1RenderTarget*, const Rect&) {}
virtual void SetIconTint(Color) {}
virtual void SetIconOpacity(float) {}
```

`Label` 实现这些接口，支持系统字形图标、图片图标、着色和透明度：

```cpp
void DrawAsIcon(ID2D1RenderTarget* rt, const Rect& r) override {
    if (!rt || !visible_) return;
    Measure(Size(max(1.0f, r.width), max(1.0f, r.height)));
    Arrange(r);
    Draw(rt);
}

void SetIconTint(Color c) override {
    SetTextColor(c);
    SetIconColor(c);
}

void SetIconOpacity(float opacity) override {
    iconOpacity_ = clamp(opacity, 0.0f, 1.0f);
    RequestRepaint();
}
```

`MenuItem` 增加：

```cpp
void MenuItem::SetGlyphIcon(Icon icon, float size = 0.0f);
void MenuItem::SetImageIcon(
    std::shared_ptr<Image> image,
    float width = 0.0f,
    float height = 0.0f);
```

原因：

- 菜单需要系统图标，同时要兼容禁用态、危险项、主题色。
- 横向菜单不能用只有纵向菜单尺寸假设的固定 icon slot。

推荐状态：保留。

## 3. 横向菜单命中、悬停和子菜单

文件：`ZufyUI.h`，`MenuWindow`

新增行/子项二维命中：

```cpp
struct HitPos {
    int top = -1;
    int child = -1;
    bool valid() const { return top >= 0; }
};

HitPos HitTestCell(int x, int y);
```

行内子菜单使用真实单元格位置打开，而不是按“前面有几个普通项 × 固定行高”推算：

```cpp
D2D1_RECT_F cell = CellRect(index, childIndex);
int px = rc.left + ShadowPx() + MulDiv((int)ceil(cell.right + 2.0f), Dpi(), 96);
int py = rc.top + ShadowPx() + MulDiv((int)floor(cell.top), Dpi(), 96);
```

原因：

- 横排、图标在上、等高不等宽三种布局混用后，旧的一维索引无法定位真正的子项。
- 子菜单、快捷键、tooltip 都需要知道 `(top, child)`，而不只是顶层行号。

推荐状态：保留。

## 4. 文本测量与菜单/横向布局修正（v1.30.0，已提交）

文件：`ZufyUI.h`、`ZufyUIWidgets.h`

新增不改变布局脏状态的文本高度测量：

```cpp
virtual float MeasureHeightForWidth(float width) {
    return desiredSize_.height;
}
```

`Label` 对自动换行文本实现测量：

```cpp
float MeasureHeightForWidth(float width) override {
    if (text_.empty() || !richRuns_.empty() ||
        overflow_ != TextOverflow::Wrap)
        return UIElement::MeasureHeightForWidth(width);

    // 用最终交叉轴宽度测量文字高度，但不触发子控件重新布局
    return padH + max(max(iconH, metrics.height), childrenH);
}
```

原因：

- 行布局在已知最终宽度后需要准确高度，原实现会让换行 Label 继续沿用旧高度。
- 不能在 Arrange 阶段再次 `Measure()` 把布局缓存打脏，否则会重复测量、抖动。

推荐状态：保留。

### 菜单栏贴边与行内分隔线修正（00f25e3）

文件：`ZufyUI.h`

菜单栏属于窗口 chrome，下面的普通内容有根布局 margin，但菜单栏不应跟着内缩：

```cpp
// 无自定义标题栏时
menuBar_->Arrange(
    Rect(left, top - rootMargin.top, availWidth, menuBarHeight_));
```

横向行里的 `Separator` 不再参与普通列宽均分，单独保留 10 DIP 的竖向分隔宽度：

```cpp
static float RowDividerWidth() { return 10.0f; }

if (child->type == MenuItem::Type::Separator)
    widths[c] = dividerWidth;
else
    widths[c] = clamp(CellContentWidth(*child),
                      rowOptions.minItemWidth,
                      rowOptions.maxItemWidth);
```

原因：

- 让菜单栏真正贴住客户区左右边缘。
- 防止行内分隔线被当作普通菜单项，导致列宽和命中位置偏移。

推荐状态：保留。

## 5. 菜单行演示和压力测试页（v1.30.0，已提交）

文件：`ZufyUI.cpp`

新增“右键菜单行”页面，覆盖：

- 2/3/4/5/6 列。
- `Inline` 与 `IconTop`。
- `Equal` 与 `ContentFit`。
- 禁用项、danger 项、快捷键、子菜单。
- 行内竖向分隔线。
- 30 行混合压力菜单。

原因：这项库能力不只是 ZExplorer 私有需求，必须有独立演示和回归入口。

推荐状态：保留。

## 6. TableView 精确单元格命中（当前未提交）

文件：`ZufyUI/ZDataViewer.h`

新增：

```cpp
struct CellHit {
    int row = -1;
    int col = -1;
    explicit operator bool() const { return row >= 0 && col >= 0; }
};

CellHit HitTestCellAt(float x, float y) {
    if (!visible_ || !arrangedRect_.Contains(x, y)) return {};
    const float header = headerVisible_ ? headerHeight_ : 0.0f;
    if (y <= arrangedRect_.y + header) return {};
    if (showVerticalScrollBar_ &&
        x >= arrangedRect_.x + arrangedRect_.width - scrollBarWidth_) {
        return {};
    }
    if (showHorizontalScrollBar_ &&
        y >= arrangedRect_.y + arrangedRect_.height - scrollBarWidth_) {
        return {};
    }

    const float relX = LocalToContentX(x);
    const float relY =
        y - arrangedRect_.y - header + Snap(scrollOffsetY_);
    const int vi = RowAtY(relY);
    const int col = GetColumnIndexAtX(relX);
    if (vi < 0 || vi >= static_cast<int>(view_.size()) ||
        col < 0 || col >= colCount_) {
        return {};
    }
    return { view_[static_cast<size_t>(vi)], col };
}
```

原因：

- ZExplorer 需要判断鼠标是否真正按在已选文件行上，只有此时才进入拖出。
- 从空白、表头、滚动条或未选中行开始拖动，应执行框选。
- 应用层用坐标估算会受表头、滚动、行列宽、虚拟行序影响，属于不可靠补丁。

推荐状态：保留，并提交到 ZufyUI。当前代码是精确命中，不是 UI 假象。

## 7. TableView 无滚动恢复选择（当前未提交）

文件：`ZufyUI/ZDataViewer.h`

新增：

```cpp
float GetScrollOffsetY() const;
void SetScrollOffsetY(float v);

void SelectRow(int row) {
    SetSelectionMode(SelectionMode::Row);
    SetCurrentCell(row, 0);
}

void SelectRowNoScroll(int row) {
    SetSelectionMode(SelectionMode::Row);
    SetCurrentCellNoScroll(row, 0);
}

void SetCurrentCell(int row, int col) {
    SetCurrentCellInternal(row, col, true);
}

void SetCurrentCellNoScroll(int row, int col) {
    SetCurrentCellInternal(row, col, false);
}
```

内部统一实现：

```cpp
void SetCurrentCellInternal(int row, int col, bool reveal) {
    // 更新 selectedRow_ / selectedCol_、SelectionChanged、
    // CurrentCellChanged、勾选同步和指示条……

    if (reveal) EnsureVisible(row, col);

    UpdateIndicatorTarget();
    InvalidateLayout();
    RequestRepaint();
}
```

原因：

- 原 `SelectRow` 必然调用 `EnsureVisible`。
- ZExplorer 周期性刷新需要恢复选中身份，但用户可能正在查看同一目录的其他位置。
- 简单删除 `ScrollToCell` 不够，因为 `SelectRow` 自身仍会把视口拉回去。
- 普通点击/键盘导航仍使用 `SelectRow`，保留保可视语义。

推荐状态：保留。API 语义明确，已有单元测试。

测试：

```cpp
TableView tv;
tv.SetCellLabelProvider(100, 1, [](int, int, Label&) {});
tv.Arrange(Rect(0, 0, 320.0f, 120.0f));
tv.SetScrollOffsetY(300.0f);
const float before = tv.GetScrollOffsetY();
tv.SelectRowNoScroll(0);
CHECK(tv.GetCurrentRow() == 0);
CHECK(fabs(tv.GetScrollOffsetY() - before) < 0.01f);
```

## 8. Label 动态文字缓存失效（当前未提交）

文件：`ZufyUIWidgets.h`

```cpp
void SetText(const std::wstring& text) {
    bool changed = (text_ != text);
    text_ = text;
    if (!richRuns_.empty()) richRuns_.clear();
    if (changed) InvalidateCache();
    InvalidateLayout();
    RequestRepaint();
    if (changed) TextChanged(text_);
}
```

原因：

- `Label` 默认使用离屏缓存。
- 文本改变时原实现只请求重绘，没有显式让自身缓存失效。
- 当固定宽度 Label 从 `0%` 变成 `7%` 时，控件尺寸不变，旧缓存位图可能继续被复用。
- 窗口 resize 会走尺寸变化路径并清缓存，所以表现为“调整窗口大小后文字立刻刷新”。

验收入口：

```cpp
auto label = std::make_shared<Label>(L"0%");
label->SetWidth(56.0f);
label->Measure(Size(56.0f, 24.0f));
label->Arrange(Rect(0.0f, 0.0f, 56.0f, 24.0f));
label->cacheValid_ = true;
label->SetText(L"7%");
CHECK(!label->DebugCacheValid());
```

推荐状态：保留。修复在库层，任务窗和所有动态 Label 都受益。

## 9. 已回退：应用层强改透明背景

这次尝试不算正式库补丁，已经撤掉。

曾尝试：

```cpp
// ShellWindow.cpp
win_.SetBackdrop(Backdrop::Acrylic);

// FileListView.cpp
table_->SetBackgroundColor(
    Color(surface.r, surface.g, surface.b, 0.72f));
tree_->SetBackgroundColor(
    Color(surface.r, surface.g, surface.b, 0.68f));
quickList_->SetBackgroundColor(
    Color(surface.r, surface.g, surface.b, 0.68f));
```

问题：

- 直接把大控件背景改成半透明，绕过了主题角色和可读性约束。
- `Card` 又会被 ZufyUI 的主题重放钩子重新覆盖为不透明 `SurfaceRaised`，不同控件行为不一致。
- 把根内容换成无背景 `ColumnBox` 会破坏原来的布局/绘制契约，出现整页空白、只剩顶栏和状态栏。
- 直接全透明不是“亚克力”，只是让文字和层级失去背景支撑。

当前处理：恢复 `Backdrop::None`、主题实心底、抽屉 `Card`、列表/树/表格默认表面。等库正式提供透明表面模型后再启用视觉层。

## 正式库需求：透明容器和控件表面

这不是应用层继续硬设 alpha 能妥善解决的问题。ZufyUI 需要提供正式的表面/层级 API。

### 建议 API

```cpp
enum class SurfaceMode {
    Theme,       // 完全跟随主题
    Opaque,      // 使用主题 RGB，alpha 强制 1
    Translucent, // RGB 跟随主题，alpha 由控件指定
    Transparent  // alpha = 0，不绘制表面
};

struct SurfaceStyle {
    SurfaceMode mode = SurfaceMode::Theme;
    float opacity = 1.0f;
    ThemeRole role = ThemeRole::Surface;
    bool inheritFromParent = false;
};

void SetSurfaceStyle(const SurfaceStyle& style);
```

主题重放时必须遵守：

- `Opaque`：更新 RGB，alpha 保持 1。
- `Translucent`：只更新 RGB，保留应用显式设置的 alpha。
- `Transparent`：主题颜色变化不能把它重新刷成不透明。
- `Theme`：才允许完全按主题默认值覆盖。

### 必须支持的类型

- 容器：`Card`、`Page`、`PageHost`、`Drawer` 内容宿主、`ColumnBox`、`RowBox`、`GridLayout`。
- 数据视图：`ListView`、`TableView`、`TreeView`。
- 数据视图还必须有独立表面项：
  - body background
  - header background
  - alternating row background
  - selection / hover background
  - scrollbar track
- 输入控件：`TextBox`、`TextEdit`、`ComboBox`、`NumberBox`。
- 窗口 chrome：`TitleBar`、`StatusBar`。
- `Button` 至少需要 normal/hover/pressed 三态的 alpha 独立控制。

### Card 特殊要求

`Card` 当前在主题系统里会被强制设置为 `SurfaceRaised`。需要增加“显式透明覆盖优先于主题默认”的粘性标记，但不能让所有 `Card` 默认透明。

建议：

```cpp
card->SetSurfaceStyle({
    .mode = SurfaceMode::Translucent,
    .opacity = 0.72f,
    .role = ThemeRole::Surface
});
```

主题重放后仍保持 72% alpha。

### 组合和可读性要求

- 透明表面只透出窗口背景，不允许穿透上层控件。
- 拖动区、命中测试和无障碍边界不能受 alpha 改变影响。
- 透明控件必须有最小对比度建议；错误抽屉、选择行等区域应保持较高不透明度。
- 每个控件要能独立决定背景，不需要应用层对每个实例手动设色。
- 不能要求应用层把整棵根树改成透明来获得亚克力。

### 验收用例

1. 浅色/深色/系统主题切换后，透明控件 alpha 不被覆盖。
2. `Card`、`TableView`、`TreeView`、`ListView` 同时放在亚克力窗口内。
3. 表头、正文、隔行色、悬停、选中都使用独立 alpha。
4. 透明子控件中的文字、图标、焦点框、IME 输入正常。
5. DPI 改变、设备丢失重建、窗口激活后透明状态保持。
6. 透明根容器不影响布局测量、子元素绘制顺序和 HitTest。

## 非 UI 库改动

以下不属于 ZufyUI Hack，但和本轮启动问题相关：

- `src/main.cpp`：活动 Host 时把启动转交 Host。
- `src/host/HostApp.cpp`：后台拉起 UI 时带 `--force-current-process`，防止递归。
- `src/ipc/Pipe.cpp`：Host 管道改为当前用户 + SYSTEM + Administrators DACL，并设置低完整性标签。

这些修改不应混入 UI 库补丁记录。

## 维护规则

以后修改 UI 库时：

1. 直接修改 `C:\project\ZufyUI`。
2. 在 ZufyUI 跑完整构建和自检。
3. 同步快照到 `C:\project\ZExplorer\third_party\ZufyUI`。
4. 在本文件追加：日期、文件、根因、代码段、影响范围、测试结果。
5. 如果是临时绕行，必须标记“临时”以及撤销条件。
6. 不允许用 alpha、延时、强制布局或颜色覆盖掩盖库的正式表面模型缺失。
