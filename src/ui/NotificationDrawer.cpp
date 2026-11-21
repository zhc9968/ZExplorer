#include "pch.h"
#include "ui/NotificationDrawer.h"

using namespace ZufyUI;

namespace ze::ui {

NotificationDrawer::NotificationDrawer(std::shared_ptr<UIElement> content) {
    const Theme& theme = ThemeManager::AppTheme();

    auto frame = std::make_shared<Card>();
    frame->SetPadding(8.0f);
    frame->SetCornerRadius(10.0f);
    frame->SetBackgroundColor(theme.Get(ThemeRole::Backdrop));
    frame->SetBorderColor(theme.Get(ThemeRole::Border));
    frame->SetHoverAnimationEnabled(false);
    frame->SetContent(std::move(content));

    drawer_ = std::make_shared<Drawer>();
    drawer_->SetContent(frame);
    drawer_->SetSide(DrawerSide::Bottom);
    drawer_->SetMode(DrawerMode::Overlay);
    drawer_->SetPanelSize(400.0f);
    drawer_->SetPanelMinSize(300.0f);
    drawer_->SetPanelMaxSize(620.0f);
    drawer_->SetPanelPadding(Thickness(20.0f, 16.0f, 20.0f, 16.0f));
    drawer_->SetPanelCornerRadius(14.0f);
    drawer_->SetPanelBackgroundColor(theme.Get(ThemeRole::SurfaceRaised));
    drawer_->SetPanelBorderColor(theme.Get(ThemeRole::Border));
    drawer_->SetPanelShadow(true);
    drawer_->SetScrimOpacity(0.28f);
    drawer_->SetModal(true);
    drawer_->SetDismissOnScrimClick(true);
    drawer_->SetDismissOnEscape(true);
    drawer_->SetAnimationDuration(0.22f);
    drawer_->SetAnimationEasing(Easing::EaseOut);

    auto panel = std::make_shared<ColumnBox>();
    panel->SetSpacing(12.0f);
    panel->SetFillWidth(true);
    panel->SetFillHeight(true);

    auto header = std::make_shared<RowBox>();
    header->SetSpacing(10.0f);
    header->SetHeight(34.0f);

    icon_ = std::make_shared<Label>(L"");
    icon_->SetIcon(Icon::Warning, 18.0f);
    icon_->SetIconColor(theme.Get(ThemeRole::Warning));
    icon_->SetWidth(26.0f);
    icon_->SetAlignment(Label::HAlign::Center, Label::VAlign::Center);
    header->AddChild(icon_);

    title_ = std::make_shared<Label>(L"提示");
    title_->SetFillWidth(true);
    title_->SetTextOverflow(Label::TextOverflow::Ellipsis);
    title_->SetAlignment(Label::HAlign::Left, Label::VAlign::Center);
    title_->SetFontSize(18.0f);
    header->AddChild(title_);

    auto close = std::make_shared<Button>(L"");
    close->SetIcon(Icon::Close, 13.0f);
    close->SetWidth(34.0f);
    close->SetHeight(30.0f);
    close->SetCornerRadius(6.0f);
    close->SetColors(theme.Get(ThemeRole::Control),
                     theme.Get(ThemeRole::ControlHover),
                     theme.Get(ThemeRole::ControlPressed));
    close->SetToolTip(L"关闭通知 (Esc)");
    close->Connect(close->Clicked, [this]() { Hide(); });
    header->AddChild(close);
    panel->AddChild(header);

    auto divider = std::make_shared<Label>(L"");
    divider->SetHeight(1.0f);
    divider->SetFillWidth(true);
    divider->SetBackgroundColor(theme.Get(ThemeRole::Divider));
    panel->AddChild(divider);

    body_ = std::make_shared<TextEdit>();
    body_->SetReadOnly(true);
    body_->SetWrapMode(TextEdit::WrapMode::WidgetWidth);
    body_->SetShowLineNumbers(false);
    body_->SetShowCurrentLine(false);
    body_->SetFillWidth(true);
    body_->SetFillHeight(true);
    body_->SetTextColor(theme.Get(ThemeRole::Text));
    body_->SetBackgroundColor(theme.Get(ThemeRole::SurfaceAlt));
    body_->SetBorderColor(theme.Get(ThemeRole::Border));
    body_->SetCornerRadius(6.0f);
    panel->AddChild(body_);

    auto footer = std::make_shared<Label>(L"可拖动选择并用 Ctrl+C 复制完整错误 · Esc 或点击空白处关闭");
    footer->SetHeight(22.0f);
    footer->SetTextColor(theme.Get(ThemeRole::TextSecondary));
    footer->SetFillWidth(true);
    panel->AddChild(footer);

    drawer_->SetPanel(panel);
    drawer_->SetFocusOnOpen(close);
    drawer_->SetOpen(false, false);
}

void NotificationDrawer::Show(const std::wstring& title,
                              const std::wstring& body,
                              NoticeKind kind) {
    const Theme& theme = ThemeManager::AppTheme();
    Icon glyph = Icon::Info;
    ThemeRole color = ThemeRole::Info;
    if (kind == NoticeKind::Warning) {
        glyph = Icon::Warning;
        color = ThemeRole::Warning;
    } else if (kind == NoticeKind::Error) {
        glyph = Icon::Error;
        color = ThemeRole::Danger;
    }
    icon_->SetIcon(glyph, 18.0f);
    icon_->SetIconColor(theme.Get(color));
    title_->SetText(title);
    title_->SetTextColor(theme.Get(color));
    body_->SetPlainText(body);
    drawer_->Open();
}

void NotificationDrawer::Hide() {
    if (drawer_) drawer_->Close();
}

bool NotificationDrawer::IsOpen() const {
    return drawer_ && drawer_->IsOpen();
}

} // namespace ze::ui
