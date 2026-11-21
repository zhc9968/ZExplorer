#pragma once

#include <memory>
#include <string>

namespace ZufyUI {
class Button;
class Drawer;
class Label;
class TextEdit;
class UIElement;
} // namespace ZufyUI

namespace ze::ui {

enum class NoticeKind { Info, Warning, Error };

// Application-owned bottom notification surface. The drawer is modal, closes
// on Escape or a scrim click, and is reused for front-end and origin-bound
// background-task failures.
class NotificationDrawer {
public:
    explicit NotificationDrawer(std::shared_ptr<ZufyUI::UIElement> content);

    std::shared_ptr<ZufyUI::Drawer> Root() const { return drawer_; }

    void Show(const std::wstring& title, const std::wstring& body,
              NoticeKind kind = NoticeKind::Warning);
    void Hide();
    bool IsOpen() const;

private:
    std::shared_ptr<ZufyUI::Drawer> drawer_;
    std::shared_ptr<ZufyUI::Label> icon_;
    std::shared_ptr<ZufyUI::Label> title_;
    std::shared_ptr<ZufyUI::TextEdit> body_;
};

} // namespace ze::ui
