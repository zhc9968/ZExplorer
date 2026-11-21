# ZufyUI 自定义菜单与横向菜单行需求

状态：待实现  
目标版本：ZufyUI 1.30+  
使用方：ZExplorer 右键菜单、托盘菜单、后续自定义 flyout  
约束：在 `C:\project\ZufyUI` 源码中实现并测试；不得直接修改
`ZExplorer/third_party/ZufyUI/`。完成后重新导出快照。

---

## 1. 目标

把当前只支持纵向单列的 `Menu` 扩展为可组合菜单，使一个菜单可以同时包含：

- 传统纵向命令项；
- `2 / 3 / 4` 个命令横向排列的一行；
- 每个横向命令独立支持图标、文字、禁用、危险色、快捷键、子菜单；
- 图标可在文字左侧，也可在文字上方；
- 分隔线和分组不破坏横向行；
- 原有 `Menu::AddItem / AddSeparator / AddSubmenu / ShowAtCursor` 调用保持兼容。

必须继续使用现有安全策略：菜单项回调不能在绘制或窗口消息栈中直接销毁菜单；
回调继续投递到 UI 线程执行。

---

## 2. 当前库能力

现状：

- `MenuItem::icon` 是 `std::shared_ptr<Label>`，已支持图标。
- `MenuItem::Type` 支持 `Normal / Separator / Submenu`。
- 子菜单本身是 `Menu`，子菜单项也能有图标。
- `MenuItem` 已支持 `enabled / danger / shortcut / checkable / checked / radio /
  isDefault / bgColor / textColor / font`。
- `Menu` 仍是单列 `std::vector<std::shared_ptr<MenuItem>> items`。
- `MenuWindow` 的测量、绘制、命中、键盘移动全部按单列假设实现。

结论：图标能力已有；横向菜单行没有，必须在 `MenuItem / Menu / MenuWindow` 三层补。

---

## 3. 建议 API

### 3.1 类型扩展

```cpp
enum class MenuRowLayout {
    Inline,      // 图标在文字左侧
    IconTop      // 图标在上、文字在下
};

enum class MenuRowWidth {
    Equal,       // 每列等宽（默认）
    ContentFit   // 各列按内容测宽，再统一拉伸到整行
};

struct MenuRowOptions {
    int columns = 0;                    // 0 = children.size()
    float gap = 4.0f;
    float minItemWidth = 72.0f;
    float maxItemWidth = 180.0f;
    float inlineHeight = 32.0f;
    float iconTopHeight = 64.0f;
    float iconSize = 18.0f;
    float iconTopSize = 24.0f;
    MenuRowLayout layout = MenuRowLayout::Inline;
    MenuRowWidth width = MenuRowWidth::Equal;
    bool showShortcuts = false;         // Compact 横排默认不展示快捷键
};
```

`MenuItem::Type` 增加：

```cpp
enum class Type { Normal, Separator, Submenu, Row };
```

`MenuItem` 增加：

```cpp
std::vector<std::shared_ptr<MenuItem>> children; // Type::Row 时使用
MenuRowOptions rowOptions;
```

### 3.2 创建接口

```cpp
std::shared_ptr<MenuItem> MakeItem(
    const std::wstring& text,
    std::function<void()> callback = nullptr,
    int id = 0);

void AddRow(
    std::vector<std::shared_ptr<MenuItem>> children,
    MenuRowOptions options = {});
```

兼容要求：现有 `AddItem` 继续向 `items` 添加普通项，不要求调用方迁移。

### 3.3 图标便捷接口

继续支持已有 `MenuItem::icon`，同时增加便捷方法：

```cpp
void SetGlyphIcon(Icon icon, float size = 0.0f);
void SetImageIcon(std::shared_ptr<Image> image, float width, float height);
```

每个菜单项必须拥有自己的 `Label`，不能复用同一个被其他菜单项持有的可视元素。

---

## 4. 布局规则

### 4.1 纵向结构

`Menu::items` 仍表示纵向顶层条目。`Type::Row` 是其中一个纵向条目，内部再横向排
`children`。

```text
Menu
├─ Normal
├─ Row (打开 / 打开方式 / 打开文件位置)
├─ Separator
├─ Row (新建文件夹 / 新建文本文档)
├─ Normal
└─ Row (复制 / 剪切 / 删除 / 永久删除)
```

### 4.2 Row 尺寸

- `columns` 默认等于子项数量，最多建议 5。
- `Equal`：整行可用宽度均分。
- `ContentFit`：先测每项最小内容宽，再把剩余宽度均分给各列。
- 单列宽度夹在 `minItemWidth / maxItemWidth` 内。
- 空间不足时文字末尾省略号，禁止菜单窗口按长文本无限增宽。
- 行高度由布局和图标尺寸决定：
  - Inline 默认 32 DIP；
  - IconTop 默认 64 DIP。
- 所有尺寸按 DPI 缩放；禁用在布局阶段固定，hover 不得改变菜单总体尺寸。

### 4.3 图标

- Inline：图标 18 DIP，左内边距 10，图标与文字间距 8。
- IconTop：图标 24 DIP 居中，文字在下方居中，最多两行省略。
- 无图标时保留该视觉槽位，避免同组按钮文字跳动。
- Submenu 项右侧显示箭头；Arrow 不占用图标槽。
- disabled 图标、文字统一使用禁用色；danger 文字与图标使用 Danger 色。

---

## 5. 交互与键盘

菜单内部坐标需从“单索引”升级为“顶层条目 + 子项索引”。

- 鼠标 hover/pressed 精确到 Row 的具体子项。
- 上下键跨纵向条目移动。
- Row 内左右键移动列。
- 从 Row 上/下移动时，尽量保持相近列索引。
- Home/End 跳到首个/末个可激活项。
- Enter/Space 激活。
- 右键子菜单箭头键按平台习惯打开。
- Esc 关闭整棵菜单树。
- 禁用项跳过。
- `isDefault` 只允许普通项或 Row 子项使用，同菜单只取第一个有效默认项。
- 子菜单锚点必须按“目标子项矩形”计算，不能按整行顶部计算。

---

## 6. 绘制与主题

必须使用当前 `ThemeRole`：

- 背景：`Surface`
- hover：`ControlHover`
- pressed：`ControlPressed`
- text：`Text`
- shortcut：`TextSecondary`
- disabled：`TextDisabled`
- separator：`Divider`
- danger：`Danger`
- focus ring：`FocusRing`

视觉要求：

- Row 中每个子项独立自己的 hover 圆角背景。
- 不允许整行一个大 hover 块。
- 分隔线与当前菜单左右 padding 对齐。
- 阴影、圆角、弹出层级沿用 `MenuWindowBase`。
- 不允许改变原菜单项危险色和自定义背景色语义。
- Focus ring 只在键盘导航时显示。

---

## 7. 可用性与无障碍

- UIA 角色仍为 `MenuItem`。
- Row 本身暴露为 Group 或 Container，不暴露为可点击命令。
- 每个子项暴露 Name、Enabled、Default、Checked、ExpandCollapse、Invoke。
- 屏幕阅读器读顺序按视觉顺序：从上到下、行内从左到右。
- 工具提示显示完整文字和快捷键。
- 高对比度主题下，图标、焦点框、禁用态不能只靠颜色区分。

---

## 8. 兼容要求

必须保证：

- 所有旧 `Menu` API 行为不变。
- 托盘菜单、MenuBar、ComboBox 下拉、任务菜单不受影响。
- `Menu::ItemSelected(int id)` 对 Row 子项继续触发一次。
- `MenuItem::Clicked` 只触发一次，并在当前菜单关闭后的 UI 任务中执行。
- 子菜单可以继续为传统纵向菜单。
- 不引入第三方 UI 库。

---

## 9. ZExplorer 目标分组

右键菜单后续按以下结构落地：

1. 打开组，一行 3 项：
   - 打开
   - 打开方式
   - 打开文件位置
2. 新建组，一行 2 项：
   - 新建文件夹
   - 新建文本文档
3. 编辑组，一行 4 项：
   - 复制
   - 剪切
   - 删除
   - 永久删除（danger）
4. 属性组，一行 2 项：
   - 系统属性面板
   - 编辑文件属性

其他功能（复制路径、NTFS 文件流、解除占用）继续作为传统纵向项或放入后续子菜单。
每一行内每个菜单项都必须独立支持图标。

---

## 10. 测试矩阵

必须新增至少以下自动化测试：

1. 2/3/4/5 列测量宽度与高度。
2. Equal 与 ContentFit 两种宽度模式。
3. Inline 与 IconTop 两种图标布局。
4. 每个 Row 子项独立 hover / pressed / disabled / danger。
5. 命中测试准确落到子项。
6. 上下左右键导航与列索引保持。
7. Row 子项子菜单的锚点坐标。
8. Row 子项回调只触发一次。
9. 旧单列菜单回归：键盘、快捷键、勾选、子菜单。
10. 100 / 125 / 150 / 200% DPI 布局截图。
11. 深色、浅色、高对比度主题截图。
12. 菜单关闭后的回调安全，无 use-after-free。

验收结果需提供：

- 自动测试输出；
- 每种行布局截图；
- API 示例代码；
- 兼容性说明；
- 导出到 ZExplorer 后的完整构建结果。
