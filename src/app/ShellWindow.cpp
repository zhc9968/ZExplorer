#include "pch.h"
#include "app/ShellWindow.h"

#include "ui/ExplorerPane.h"
#include "platform/PathUtil.h"
#include "platform/Settings.h"
#include "platform/ShellActions.h"
#include "platform/WindowSecurity.h"

using namespace ZufyUI;

namespace ze::app {

namespace {

// 写 UTF-8 (带 BOM) 文件。GUI 程序没有控制台，自检结果必须落盘。
bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string utf8;
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        utf8.resize(static_cast<size_t>(need));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            utf8.data(), need, nullptr, nullptr);
    }

    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    WriteFile(h, bom, 3, &written, nullptr);
    if (!utf8.empty()) WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return true;
}

// 让**应用自己**把自己的窗口截图存成 PNG（只截本窗口矩形，不抓桌面）。
// 这样自检就能在没有任何外部截屏的情况下验证"到底画出来没有"。
bool CaptureWindowToPng(HWND hwnd, const std::wstring& pngPath, std::wstring* error) {
    if (!hwnd) { if (error) *error = L"no hwnd"; return false; }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) { if (error) *error = L"GetWindowRect failed"; return false; }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { if (error) *error = L"empty rect"; return false; }

    HDC screen = GetDC(nullptr);
    if (!screen) { if (error) *error = L"GetDC failed"; return false; }

    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(mem); ReleaseDC(nullptr, screen);
        if (error) *error = L"CreateDIBSection failed";
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);

    // 1) 首选 PrintWindow(FULLCONTENT)。DComp 窗口可能不支持，失败再退回屏幕位块。
    BOOL ok = PrintWindow(hwnd, mem, 0x00000002 /*PW_RENDERFULLCONTENT*/);
    bool usedPrintWindow = (ok != FALSE);
    if (!ok) {
        ok = BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        DeleteObject(bmp);
        if (error) *error = L"PrintWindow/BitBlt failed";
        return false;
    }

    // GDI 给的 32bpp DIB alpha 通道通常是 0，直接转 Image 会全透明 → 先补成不透明。
    auto* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 255;

    bool saved = false;
    if (auto img = Image::FromHBITMAP(bmp, /*premultipliedAlpha*/ false)) {
        saved = img->Save(pngPath, Image::Format::Png);
    }
    DeleteObject(bmp);

    if (!saved && error) *error = L"Image::Save failed";
    if (error && saved) *error = usedPrintWindow ? L"via PrintWindow" : L"via BitBlt";
    return saved;
}

// ---------------------------------------------------------------------------
// 外壳：自定义标题栏 + 状态栏 + 一个 ExplorerPane
//
// 布局说明：窗口级 chrome（标题栏/状态栏）由 ZufyUI 自己排（不参与根布局），
// 根布局只有 ExplorerPane 这一块，所以直接 SetRootLayout。
// ---------------------------------------------------------------------------
class AppShellImpl final : public IAppShell {
public:
    bool Create(const std::wstring& selfTestReport,
                const std::wstring& initialPath) override {
        const ze::platform::AppSettings savedSettings = ze::platform::LoadSettings();
        int savedX = savedSettings.windowX;
        int savedY = savedSettings.windowY;
        int savedW = std::clamp(savedSettings.windowWidth, 720, 6000);
        int savedH = std::clamp(savedSettings.windowHeight, 480, 4000);
        if (static_cast<unsigned>(savedX) != CW_USEDEFAULT ||
            static_cast<unsigned>(savedY) != CW_USEDEFAULT) {
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (savedX < vx || savedY < vy ||
                savedX >= vx + vw - 80 || savedY >= vy + vh - 60) {
                savedX = CW_USEDEFAULT;
                savedY = CW_USEDEFAULT;
            }
        }
        if (!win_.Create(savedW, savedH, L"ZExplorer",
                         savedX, savedY)) return false;
        ze::platform::AllowLowerIntegrityMessages(win_.GetHwnd());

        win_.SetMinSize(720, 480);
        win_.SetIcon(ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CXSMICON)), ze::platform::LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CXSMICON)));   // 标题栏/任务栏图标

        titleBar_ = std::make_shared<DefaultTitleBar>();
        auto pin = std::make_shared<CaptionButton>(CaptionButton::Kind::Pin);
        pin->SetButtonWidth(42.0f);
        pin->SetToolTip(L"固定到最前 / 取消固定");
        pin->SetGlyphColor(ThemeManager::AppTheme().Get(ThemeRole::Text));
        auto alwaysOnTop = std::make_shared<bool>(false);
        pin->Connect(pin->Clicked, [this, pin, alwaysOnTop]() {
            *alwaysOnTop = !*alwaysOnTop;
            SetWindowPos(win_.GetHwnd(),
                         *alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            pin->SetActiveState(*alwaysOnTop);
        });
        titleBar_->AddCustomButton(pin);
        win_.SetCustomTitleBar(titleBar_);

        // ---- 状态栏 ----
        status_ = std::make_shared<StatusBar>();
        pItems_ = status_->AddPanel(L"");
        pSel_ = status_->AddPanel(L"");
        pInfo_ = status_->AddPanel(L"", true);
        pState_ = status_->AddPanel(L"就绪", true);
        win_.SetStatusBar(status_);

        // ---- 内容 ----
        pane_ = ui::ExplorerPane::Create();
        pane_->OnNavigated = [this](const std::wstring& path) { ApplyNavigated(path); };
        pane_->OnStatus = [this](const std::wstring& text) {
            status_->SetPanelText(pState_, text);
        };
        pane_->OnLoaded = [this](size_t total, size_t dirs, size_t files, double ms) {
            wchar_t buf[192];
            swprintf_s(buf, L"%zu 个项目（%zu 个文件夹，%zu 个文件）· 读取 %.1f ms",
                       total, dirs, files, ms);
            status_->SetPanelText(pInfo_, buf);
            status_->SetPanelText(pSel_, L"");
        };
        pane_->OnSelectionChanged = [this](size_t count, uint64_t bytes) {
            if (count == 0) {
                status_->SetPanelText(pSel_, L"");
                return;
            }
            std::wstring text = L"已选 " + std::to_wstring(count) + L" 项";
            if (bytes > 0) text += L" · " + ze::platform::FormatSize(bytes);
            status_->SetPanelText(pSel_, text);
        };
        pane_->ApplyPersistentSettings();

        auto rootLayout = std::dynamic_pointer_cast<Layout>(pane_->Root());
        if (!rootLayout) return false;
        win_.SetRootLayout(rootLayout);
        pane_->AttachToWindow(&win_);   // 注册快捷键等需要窗口的东西
        win_.Connect(win_.Closing, [this](bool* cancel) {
            if (cancel && *cancel) return;
            // Self-test is a diagnostic launch, not a user session: never let it
            // overwrite the user's last path/window/layout state.
            if (reportPath_.empty()) {
                PersistPaneSettings();
                SaveWindowState();
            }
        });
        // 只在窗口起来后导航一次。普通模式默认进"此电脑"；命令行路径和
        // 自检目录走同一个入口，避免先加载用户目录再被目标位置覆盖。
        {
            std::wstring target = initialPath;
            if (target.empty()) {
                if (selfTestReport.empty()) {
                    switch (savedSettings.startupMode) {
                    case ze::platform::StartupMode::LastFolder:
                        target = savedSettings.lastFolder;
                        break;
                    case ze::platform::StartupMode::SpecifiedPath:
                        target = savedSettings.startupPath;
                        break;
                    case ze::platform::StartupMode::ThisPc:
                    default:
                        target = L"::thispc";
                        break;
                    }
                    if (target.empty() ||
                        (target != L"::thispc" &&
                         !ze::platform::PathExists(target))) {
                        target = L"::thispc";
                    }
                } else {
                    wchar_t home[MAX_PATH]{};
                    if (SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home) == S_OK &&
                        home[0] != L'\0') {
                        target = home;
                    } else {
                        target = L"C:\\";
                    }
                }
            }

            auto nav = win_.CreateTimer(80);
            nav->Tick.connect([this, target, nav]() {
                nav->Stop();
                if (ze::platform::PathExists(target) &&
                    !ze::platform::IsDirectoryPath(target)) {
                    pane_->OpenPathLocation(target);
                } else {
                    pane_->NavigateTo(target);
                }
            });
            nav->Start(80);
        }

        // ---- 显示 ----
        win_.Show();
        persistTimer_ = win_.CreateTimer(1000);
        persistTimer_->Tick.connect([this]() {
            if (!reportPath_.empty()) return;
            PersistPaneSettings();
            SaveWindowState();
        });
        // 在库提供正式的“透明容器/控件表面”API 前，保持可读的实心窗口底。
        win_.SetBackdrop(Backdrop::None);
        win_.SetBackgroundColor(ThemeManager::AppTheme().Get(ThemeRole::Backdrop));


        // ---- 自检模式 ----
        if (!selfTestReport.empty()) {
            reportPath_ = selfTestReport;
            // 自检分步执行：每一次 tick 只做一件事，避免在 UI 线程 Sleep 把
            // RunAsync 的回调饿死（回调是投递到 UI 线程的）。
            selfTestTimer_ = win_.CreateTimer(350);
            selfTestTimer_->Tick.connect([this]() {
                HWND hwnd = win_.GetHwnd();
                if (!hwnd) return;

                std::wstring pngPath = reportPath_;
                const size_t dot = pngPath.find_last_of(L'.');
                if (dot != std::wstring::npos) pngPath = pngPath.substr(0, dot) + L".png";
                auto withSuffix = [&](const wchar_t* suffix) {
                    const size_t d = pngPath.find_last_of(L'.');
                    return (d != std::wstring::npos) ? pngPath.substr(0, d) + suffix
                                                     : pngPath + suffix;
                };

                switch (selftestStep_) {
                case 0: {
                    SetForegroundWindow(hwnd);
                    std::wstring note;
                    if (CaptureWindowToPng(hwnd, pngPath, &note)) { capturePngPath_ = pngPath; captureNote_ = note; }
                    break;
                }
                case 1: {
                    if (pane_) pane_->FocusAddress();          // 进入编辑态：面包屑应隐藏 + 全选
                    break;
                }
                case 2: {
                    std::wstring note;
                    if (CaptureWindowToPng(hwnd, withSuffix(L"-edit.png"), &note)) editPngPath_ = withSuffix(L"-edit.png");
                    if (pane_) addressDiag_ = pane_->AddressDiagnostics();

                    // 模拟"点到地址栏外面"：窗口级鼠标按下应让地址栏失焦、面包屑恢复
                    UIZSignals::GlobalMouseDown.Fire(&win_, 600.0f, 400.0f);
                    break;
                }
                case 3: {
                    std::wstring note;
                    if (CaptureWindowToPng(hwnd, withSuffix(L"-blur.png"), &note)) blurPngPath_ = withSuffix(L"-blur.png");
                    if (pane_) {
                        blurDiag_ = pane_->AddressDiagnostics();
                        pane_->NavigateTo(L"C:\\Windows\\System32\\drivers");
                    }
                    break;
                }
                case 4: {
                    std::wstring note;
                    if (CaptureWindowToPng(hwnd, withSuffix(L"-deep.png"), &note)) deepPngPath_ = withSuffix(L"-deep.png");
                    if (pane_) deepDiag_ = pane_->AddressDiagnostics();
                    break;
                }
                case 5: {
                    if (selfTestTimer_) selfTestTimer_->Stop();
                    WriteSelfTestReport();
                    win_.Close();
                    break;
                }                default:
                    break;
                }
                ++selftestStep_;
            });
            selfTestTimer_->Start(350);
        }
        return true;
    }

    void Run() override { win_.Run(); }

private:
    void PersistPaneSettings() {
        if (!pane_) return;
        std::wstring error;
        if (pane_->SavePersistentSettings(&error)) {
            lastPersistenceError_.clear();
            return;
        }
        if (!error.empty() && error != lastPersistenceError_) {
            lastPersistenceError_ = error;
            pane_->ReportWarning(
                L"无法保存浏览状态",
                L"目录、排序或布局设置没有写入。\n\n" + error);
        }
    }

    void SaveWindowState() {
        if (!win_.GetHwnd()) return;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(win_.GetHwnd(), &placement)) return;
        const RECT& rect = placement.rcNormalPosition;
        // Window::Create scales width/height with the system DPI, not the
        // monitor DPI. Reverse the exact same transform so the next launch
        // restores the same physical size instead of shrinking on mixed-DPI
        // setups.
        UINT dpi = GetDpiForSystem();
        if (dpi == 0) dpi = 96;
        const ze::platform::AppSettings before = ze::platform::LoadSettings();
        ze::platform::AppSettings settings = before;
        // Window::Create receives x/y as physical screen pixels, while
        // width/height are DIP and are scaled by the library.
        settings.windowX = rect.left;
        settings.windowY = rect.top;
        settings.windowWidth = std::clamp(
            MulDiv(rect.right - rect.left, 96, static_cast<int>(dpi)),
            720, 6000);
        settings.windowHeight = std::clamp(
            MulDiv(rect.bottom - rect.top, 96, static_cast<int>(dpi)),
            480, 4000);
        const bool changed =
            settings.windowX != before.windowX ||
            settings.windowY != before.windowY ||
            settings.windowWidth != before.windowWidth ||
            settings.windowHeight != before.windowHeight;
        if (changed) {
            std::wstring error;
            if (!ze::platform::SaveSettings(settings, &error) &&
                !error.empty() && error != lastPersistenceError_) {
                lastPersistenceError_ = error;
                if (pane_) {
                    pane_->ReportWarning(
                        L"无法保存窗口布局",
                        L"窗口位置或大小没有写入。\n\n" + error);
                }
            }
        }
    }

    void ApplyNavigated(const std::wstring& path) {
        const std::wstring shown =
            path == L"::thispc" ? L"此电脑" :
            path == L"::recyclebin" ? L"回收站" : path;
        win_.SetTitle(L"ZExplorer — " + shown);
        if (titleBar_) titleBar_->SetTitle(L"ZExplorer — " + shown);
        status_->SetPanelText(pItems_, L"当前目录: " + shown);
        if (path == L"::thispc") {
            status_->SetPanelText(pSel_, L"");
            status_->SetPanelText(pInfo_, L"驱动器列表");
        } else if (path == L"::recyclebin") {
            status_->SetPanelText(pSel_, L"");
            status_->SetPanelText(pInfo_, L"回收站");
        }
        if (reportPath_.empty()) PersistPaneSettings();
        if (reportPath_.empty()) SaveWindowState();
    }

    void WriteSelfTestReport() {
        std::wstring out;
        wchar_t buf[512];

        out += L"ZExplorer 自检报告\n";
        out += L"=================\n";

        swprintf_s(buf, L"窗口 HWND      : %p\n", reinterpret_cast<void*>(win_.GetHwnd()));
        out += buf;
        swprintf_s(buf, L"客户区(DIP)    : %.0f x %.0f\n",
                   win_.GetClientWidthDip(), win_.GetClientHeightDip());
        out += buf;
        swprintf_s(buf, L"DPI 缩放       : %.2f\n", win_.GetDpiScale());
        out += buf;

        const auto root = pane_ ? pane_->Root() : nullptr;
        swprintf_s(buf, L"根布局子元素   : %zu\n", root ? root->GetChildren().size() : 0);
        out += buf;

        out += L"导航树根节点   : " + std::to_wstring(pane_ ? pane_->TreeRootCount() : 0) + L"\n";
        out += L"导航树总节点   : " + std::to_wstring(pane_ ? pane_->TreeNodeCount() : 0) + L"\n";
        swprintf_s(buf, L"文件列表行数   : %d\n", pane_ ? pane_->ListRowCount() : 0);
        out += buf;
        swprintf_s(buf, L"行图标数       : %d\n", pane_ ? pane_->ListIconCount() : 0);
        out += buf;
        out += L"图标解码       : " + (pane_ ? pane_->IconDiagnostics() : L"") + L"\n";
        out += L"树状态         : " + (pane_ ? pane_->TreeDiagnostics() : L"") + L"\n";
        out += L"自截图         : " + (capturePngPath_.empty() ? L"失败：" + captureNote_
                                                               : capturePngPath_ + L"（" + captureNote_ + L"）") + L"\n";
        out += L"编辑态截图     : " + (editPngPath_.empty() ? std::wstring(L"失败") : editPngPath_) + L"\n";
        out += L"地址栏状态     : " + addressDiag_ + L"\n";
        out += L"失焦截图       : " + (blurPngPath_.empty() ? std::wstring(L"失败") : blurPngPath_) + L"\n";
        out += L"失焦状态       : " + blurDiag_ + L"\n";
        out += L"深路径截图     : " + (deepPngPath_.empty() ? std::wstring(L"失败") : deepPngPath_) + L"\n";
        out += L"深路径状态     : " + deepDiag_ + L"\n";
        swprintf_s(buf, L"条目数(模型)   : %zu\n", pane_ ? pane_->EntryCount() : 0);
        out += buf;
        out += L"当前路径       : " + (pane_ ? pane_->CurrentPath() : std::wstring()) + L"\n";

        // ---- DPI / 物理尺寸：确认进程是否真的 Per-Monitor V2 ----
        const HWND hwnd = win_.GetHwnd();
        const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
        RECT wr{}, cr{};
        if (hwnd) { GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr); }

        const DPI_AWARENESS aw =
            GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
        const wchar_t* awName = (aw == DPI_AWARENESS_UNAWARE) ? L"UNAWARE"
                             : (aw == DPI_AWARENESS_SYSTEM_AWARE) ? L"SYSTEM_AWARE"
                             : (aw == DPI_AWARENESS_PER_MONITOR_AWARE) ? L"PER_MONITOR_AWARE"
                             : L"?";
        swprintf_s(buf, L"窗口 DPI       : %u（scale %.2f）· 感知级别 %ls\n",
                   dpi, dpi / 96.0, awName);
        out += buf;
        swprintf_s(buf, L"窗口矩形(物理) : %ld x %ld  客户区(物理): %ld x %ld\n",
                   wr.right - wr.left, wr.bottom - wr.top,
                   cr.right - cr.left, cr.bottom - cr.top);
        out += buf;

        // ---- 布局：列出关键元素的排布矩形，用于验证"有没有多余空白" ----
        out += L"\n--- 布局（DIP）---\n";
        out += pane_ ? pane_->DumpLayout() : L"(pane 为空)\n";

        out += L"\n结果: ";
        const bool ok = (win_.GetHwnd() != nullptr)
                     && (pane_ != nullptr)
                     && (pane_->TreeRootCount() > 0)
                     && (pane_->TreeNodeCount() > pane_->TreeRootCount())   // 懒加载已生效
                     && (pane_->ListRowCount() > 0)
                     && (pane_->ListIconCount() > 0);
        out += ok ? L"PASS（窗口/树/列表均已建立并完成一次异步加载）\n"
                  : L"FAIL（窗口或控件树不完整，或列表未加载到数据）\n";

        WriteUtf8File(reportPath_, out);
    }

    Window win_;

    std::shared_ptr<DefaultTitleBar> titleBar_;
    std::shared_ptr<StatusBar> status_;
    std::shared_ptr<ui::ExplorerPane> pane_;
    std::shared_ptr<Timer> selfTestTimer_;
    std::shared_ptr<Timer> persistTimer_;
    int selftestStep_ = 0;
    std::wstring reportPath_;
    std::wstring capturePngPath_;
    std::wstring captureNote_;
    std::wstring editPngPath_;
    std::wstring addressDiag_;
    std::wstring blurPngPath_;
    std::wstring blurDiag_;
    std::wstring deepPngPath_;
    std::wstring deepDiag_;
    std::wstring lastPersistenceError_;

    int pItems_ = 0;
    int pSel_ = 0;
    int pInfo_ = 0;
    int pState_ = 0;
};

} // namespace

std::unique_ptr<IAppShell> CreateAppShell() {
    return std::make_unique<AppShellImpl>();
}

} // namespace ze::app
