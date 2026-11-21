#pragma once
// ============================================================================
// app/ShellWindow.h —— 应用外壳的对外接口（刻意保持不透明）
//
// main.cpp 只需要「创建 + 运行」，不需要知道 ZufyUI 的任何类型，
// 因此这里只暴露一个纯虚接口，实现细节全在 .cpp 里。
// ============================================================================

#include <memory>
#include <string>

namespace ze::app {

class IAppShell {
public:
    virtual ~IAppShell() = default;

    // 创建窗口与全部子控件。失败返回 false。
    // selfTestReport 非空时进入自检模式：界面起来后把控件树与数据写入该文件并自动退出。
    // initialPath 非空时，界面起来后直接导航到该目录（用于"用本程序打开某个位置"）。
    virtual bool Create(const std::wstring& selfTestReport = std::wstring(),
                        const std::wstring& initialPath = std::wstring()) = 0;

    // 进入消息循环（阻塞到窗口关闭）。
    virtual void Run() = 0;
};

std::unique_ptr<IAppShell> CreateAppShell();

} // namespace ze::app
