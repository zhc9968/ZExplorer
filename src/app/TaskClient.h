#pragma once
// ============================================================================
// app/TaskClient.h —— UI 进程侧的任务客户端
//
// 铁律（这一版重写的核心）：
//   1. **UI 线程永不发起管道调用**。连接/发送/接收全在一个独立 IO 线程上做，
//      UI 线程只读一份内存快照（拿锁 + 拷贝），不可能卡住界面。
//   2. 提交任务是"入队即返回"，不等后台回执。
//   3. 后台做完后由下一次轮询看到，无需反向通知。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace ze::app {

struct TaskInfo {
    int id = 0;
    std::uint32_t originPid = 0;
    std::wstring originPath;
    int state = 0;        // 0 排队 1 运行 2 完成 3 失败 4 取消
    float ratio = 0.0f;
    std::wstring title;
    std::wstring current;
    int issueCount = 0;
    std::wstring firstIssue;
    std::wstring issueDetails;
    std::wstring op;
    std::wstring targetDir;
    std::uint32_t doneFiles = 0;
    std::uint32_t totalFiles = 0;
};

class TaskClient {
public:
    // 启动/停止后台 IO 线程（进程启动一次、退出一次）。
    static void Start();
    static void Stop();

    // 确保后台进程在跑（不在就拉起自己的一份 --host）。
    // 会阻塞，但只在启动后台那一瞬间；由 IO 线程调用，不占 UI 线程。
    static bool EnsureHost();

    // 入队，立即返回。true = 已排进 IO 队列。
    static bool Submit(const std::wstring& op, const std::wstring& target,
                       const std::vector<std::wstring>& sources, int* id = nullptr);
    static bool ShowTask(int id);
    static bool CancelTask(int id);
    static bool OpenNewWindow(const std::wstring& path);
    static bool OpenSettings();
    static bool OpenAbout();

    // 读最近一次快照（不阻塞）。true = 拿到过有效数据。
    static bool Query(std::vector<TaskInfo>* out);
    static bool HostReady();

private:
    static void IoLoop();
};

} // namespace ze::app
