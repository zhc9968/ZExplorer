# ZufyUI 反馈（来自 ZExplorer 实战）

> 导出快照：`ZufyUI@d41ed86`（v1.29.1）。
> 下面每条都带**最小复现**或 **dump 证据**，按严重程度排序。

---

## 1. 【崩溃】进程退出时静态析构顺序导致 c0000005

**现象**：后台进程点托盘"退出"，进程退出瞬间崩溃。

**dump 证据**（`ZExplorer.exe.5764.dmp`，Release + PDB）：

```
c0000005
  std::_Hash<Window*, shared_ptr<UiaWindowContext>>::_Find_last
  ZufyUI::detail::ReleaseAccessibility
  ZufyUI::Window::~Window
  ZufyUI::MenuWindow::scalar deleting destructor
  ZufyUI::MenuWindow::StandaloneHolder::s 的 atexit 析构
```

**原因**：`MenuWindow::StandaloneHolder` 是函数内静态对象，atexit 阶段析构时调用
`~Window` → `ReleaseAccessibility`，而它依赖的 UIA 映射表也是静态对象、**已经先被销毁**。

**建议**：UIA 表用 `function-local static` 且永不析构（`new` 出来不 delete），
或 `ReleaseAccessibility` 先判自身是否已销毁。

**我们的绕开方式**：进程退出不返回 C++（直接 `ExitProcess`），跳过静态析构。

---

## 2. 【崩溃】销毁"正在交互的元素"没有保护

**现象**：点卡片上的"跳过"按钮 → 卡片被移除 → 再点一次就 c0000005；
栈落在 `Window::HandleMessage`（派发鼠标消息时访问了已释放元素）。

**原因**：`ZSignal::Fire` 期间没有自毁保护，`RemoveChild`/`ClearChildren` 立刻析构元素，
而窗口内部持有 pressed/hovered 元素的裸指针。

**建议**：
- `Fire` 期间对监听者做延迟析构，或
- `RemoveChild` 检测到"该元素正在派发事件/处于 hover"时推迟到消息循环下一拍。

**我们的绕开方式**：卡片只 `SetVisible(false)`，整个会话不销毁（有数量上限）。

---

## 3. 【易踩】`MessageBox::SetUserContent` 会静默清空所有按钮

`ZufyUIWindowTool.h` 注释写了"SetUserContent 后不再放任何按钮"，但调用方
（`SetUserContent(content)` + `FastButton::OK|Cancel`）编译通过、运行时只剩内容，
很容易误以为"对话框没有按钮"。

**建议**：要么保留按钮行，要么在 API 上加 `SetUserContent(content, keepButtons)`，
或者干脆 assert/警告。

---

## 4. 【易踩】`TabView` 没有按 id 找页签的接口

只有 `RemoveTab(index)`。业务侧加了固定首页后，删除/选中都要手算偏移
（我们踩了两次：删错页签、删完又弹回来）。

**建议**：`int IndexOfTab(const std::wstring& key)` 或给 tab 一个稳定 handle。

---

## 5. 【不便】`Card` 只能单子元素

`Card` 只有 `SetContent(child)`。做"标题 + 说明 + 按钮行"必须再套 `ColumnBox`，
而卡片背景/内边距又作用在那一层，容易和内层间距打架。

**建议**：让 `Card` 也能 `AddChild`（内部自动套一层 ColumnBox）。

---

## 6. 【缺失】`TableView` 没有批量更新

`ListView` 有 `BeginUpdate/EndUpdate`，`TableView` 没有。逐行 `SetRowChecked`
在 480 行时是 O(n²)，实测把 UI 卡死。

**建议**：给 `TableView` 补 `BeginBatch/EndBatch`（或 `BeginUpdate/EndUpdate`）。

---

## 7. 【绘图】`Image` 不是 `UIElement`，图标要套 `Label`

`Image::FromHICON` 产出的是资源对象，不能 `AddChild`，必须
`Label::SetImage + SetIconSize`。能用，但和"加一个图标"的直觉不一致。

**建议**：提供 `IconView : public UIElement`，或在文档里给一段标准写法。

---

## 8. 【细节】`Window::SetIcon` 不按 DPI 选尺寸

我们传 32/16 时，在 192 DPI 下任务栏会缩放导致发糊。
现在改为 `GetSystemMetrics(SM_CXICON/SM_CXSMICON)` 后正常。

**建议**：`SetIcon` 内部按 `GetDpiForWindow` 自动取尺寸，或提供
`SetIconFromResource(id)` 内部走 `LoadIconMetric`。

---

## 附：已经确认修好的

- `ColumnBox/UIElement` 现在有 `RemoveChild / RemoveChildAt / InsertChild`（我们靠它做增量卡片）
- `TreeView::ScrollToNode` 在"刚插入大量子节点"时会被旧的 `maxScrollY_` 夹小（我们用
  `GetVisibleNodes + SetScrollOffsetY` 绕开，建议内部先 `UpdateScrollInfo` 再 clamp）
