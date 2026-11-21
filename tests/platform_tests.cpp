// ============================================================================
// tests/platform_tests.cpp —— platform/ 与 fs/ 的 headless 自检
//
// 为什么能这么测：AGENTS.md §2 规定 platform/ 与 fs/ 只依赖 Win32 + STL。
// 本文件因此**不包含 pch.h、不链接 ZufyUI、不建窗口**，可在 CI 里直接跑。
//
//   退出码 0 = 全过；非 0 = 有断言失败。
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "platform/PathUtil.h"
#include "platform/ShellActions.h"
#include "platform/FileOps.h"
#include "platform/FileAttributes.h"
#include "platform/NtfsStreams.h"
#include "platform/FileLock.h"
#include "platform/DirectoryWatcher.h"
#include "platform/WinEKeyState.h"
#include "fs/DirectoryModel.h"

using namespace ze;
using namespace ze::platform;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (expr) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        }                                                                  \
    } while (0)

#define CHECK_EQ_W(actual, expected)                                       \
    do {                                                                   \
        const std::wstring a_ = (actual);                                   \
        const std::wstring e_ = (expected);                                 \
        if (a_ == e_) { ++g_pass; }                                        \
        else {                                                             \
            ++g_fail;                                                      \
            std::printf("  [FAIL] %s:%d  got=[%ls] want=[%ls]\n",          \
                        __FILE__, __LINE__, a_.c_str(), e_.c_str());       \
        }                                                                  \
    } while (0)

static void TestPathUtil() {
    std::printf("[PathUtil]\n");

    CHECK_EQ_W(NormalizePath(L"C:\\a\\b\\.."), L"C:\\a");
    CHECK_EQ_W(NormalizePath(L"C:\\a\\.\\b\\\\c"), L"C:\\a\\b\\c");
    CHECK_EQ_W(NormalizePath(L"C:\\a\\b\\"), L"C:\\a\\b");
    CHECK_EQ_W(NormalizePath(L"C:\\"), L"C:\\");

    CHECK_EQ_W(ToExtendedPath(L"C:\\a"), L"\\\\?\\C:\\a");
    CHECK_EQ_W(ToExtendedPath(L"\\\\?\\C:\\a"), L"\\\\?\\C:\\a");
    CHECK_EQ_W(ToExtendedPath(L"\\\\server\\share\\x"), L"\\\\?\\UNC\\server\\share\\x");
    CHECK_EQ_W(StripExtendedPath(L"\\\\?\\C:\\a"), L"C:\\a");
    CHECK_EQ_W(StripExtendedPath(L"\\\\?\\UNC\\srv\\sh\\x"), L"\\\\srv\\sh\\x");
    CHECK_EQ_W(StripExtendedPath(L"C:\\a"), L"C:\\a");

    CHECK_EQ_W(JoinPath(L"C:\\a", L"b"), L"C:\\a\\b");
    CHECK_EQ_W(JoinPath(L"C:\\a\\", L"b"), L"C:\\a\\b");

    CHECK_EQ_W(ParentPath(L"C:\\a\\b"), L"C:\\a");
    CHECK_EQ_W(ParentPath(L"C:\\a"), L"C:\\");
    CHECK_EQ_W(ParentPath(L"C:\\"), L"C:\\");

    CHECK_EQ_W(FileNameOf(L"C:\\a\\b.txt"), L"b.txt");
    CHECK_EQ_W(FileNameOf(L"C:\\a\\"), L"a");
    CHECK_EQ_W(ExtensionOf(L"C:\\a\\B.TXT"), L".txt");
    CHECK_EQ_W(ExtensionOf(L"C:\\a\\noext"), L"");
    CHECK_EQ_W(ExtensionOf(L"C:\\a\\.gitignore"), L"");
    CHECK(IsExecutablePath(L"C:\\a\\setup.exe"));
    CHECK(IsExecutablePath(L"C:\\a\\SETUP.EXE"));
    CHECK(IsExecutablePath(L"C:\\a\\script.cmd"));
    CHECK(IsExecutablePath(L"C:\\a\\task.ps1"));
    CHECK(!IsExecutablePath(L"C:\\a\\readme.txt"));
    CHECK(!IsExecutablePath(L"C:\\a\\noext"));
    CHECK(IsKnownFileExtension(L"C:\\a\\readme.txt"));
    CHECK(!IsKnownFileExtension(L"C:\\a\\file.zx_unknown_ext"));

    CHECK(IsRootPath(L"C:\\"));
    CHECK(!IsRootPath(L"C:\\a"));

    const std::wstring expandedTemp = ExpandEnvironmentVariables(L"%TEMP%");
    CHECK(!expandedTemp.empty());
    CHECK(expandedTemp.find(L"%TEMP%") == std::wstring::npos);
    CHECK_EQ_W(ExpandEnvironmentVariables(L"%ZEXPLORER_UNKNOWN_VAR%"),
               L"%ZEXPLORER_UNKNOWN_VAR%");
    CHECK(PathExists(expandedTemp));
    CHECK(IsDirectoryPath(expandedTemp));
    const std::wstring system32 =
        ExpandEnvironmentVariables(L"%SystemRoot%\\System32");
    CHECK(system32.find(L"%SystemRoot%") == std::wstring::npos);
    CHECK(PathExists(system32));
    CHECK(IsDirectoryPath(system32));

    CHECK_EQ_W(FormatSize(0), L"");
    CHECK_EQ_W(FormatSize(512), L"512 B");
    CHECK_EQ_W(FormatSize(1024), L"1.0 KB");
    CHECK_EQ_W(FormatSize(1024ull * 1024 * 3 / 2), L"1.5 MB");
}

static void TestDirectoryModel() {
    std::printf("[DirectoryModel]\n");

    // 约定：测试从仓库根目录运行（build.ps1 会 cd 过去）。
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const std::wstring srcDir = JoinPath(cwd, L"src");

    if (GetFileAttributesW(srcDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  [SKIP] src/ 不存在，请从仓库根目录运行测试 (cwd=%ls)\n", cwd);
        return;
    }

    auto snap = fs::DirectoryModel::Load(srcDir, true);
    CHECK(snap.Ok());
    CHECK(!snap.accessDenied);
    CHECK(!snap.notFound);
    CHECK(snap.entries.size() >= 3);          // app / fs / platform / ui 至少 3 个
    CHECK(snap.DirCount() >= 3);
    CHECK(!snap.path.empty());

    // 默认名称升序：目录在前。
    bool seenFile = false;
    bool orderOk = true;
    for (const auto& e : snap.entries) {
        if (!e.isDir) seenFile = true;
        else if (seenFile) orderOk = false;
    }
    CHECK(orderOk);

    // 按大小降序：目录作为无大小分组在最前，文件之间按大小降序。
    auto bySize = snap;
    fs::DirectoryModel::Sort(bySize, fs::SortKey::Size, fs::SortOrder::Descending);
    bool dirsFirst = true;
    bool sizeOrderOk = true;
    bool seenFileInSize = false;
    uint64_t previousFileSize = UINT64_MAX;
    for (const auto& e : bySize.entries) {
        if (!e.isDir) {
            seenFileInSize = true;
            if (e.size > previousFileSize) sizeOrderOk = false;
            previousFileSize = e.size;
        } else if (seenFileInSize) {
            dirsFirst = false;
        }
    }
    CHECK(dirsFirst);
    CHECK(sizeOrderOk);

    // 独立排序模型（不依赖当前仓库里恰好有哪些文件）：
    // 自然排序应把 file2 排在 file10 前；降序只翻转同一组内的文件，目录仍在前。
    std::vector<fs::FileEntry> sortable;
    auto addEntry = [&](const wchar_t* name, bool isDir, uint64_t size,
                        uint32_t modifiedLow = 0) {
        fs::FileEntry e;
        e.name = name;
        e.fullPath = JoinPath(srcDir, name);
        e.isDir = isDir;
        e.size = size;
        e.typeName = isDir ? L"文件夹" : L"文本文档";
        e.modified.dwLowDateTime = modifiedLow;
        sortable.push_back(std::move(e));
    };
    addEntry(L"file10.txt", false, 10, 100);
    addEntry(L"dir10", true, 0, 30);
    addEntry(L"file2.txt", false, 20, 20);
    addEntry(L"dir2", true, 0, 10);

    fs::DirectoryModel::Sort(sortable, fs::SortKey::Name, fs::SortOrder::Ascending);
    CHECK(sortable.size() == 4);
    CHECK(sortable[0].name == L"dir2");
    CHECK(sortable[1].name == L"dir10");
    CHECK(sortable[2].name == L"file2.txt");
    CHECK(sortable[3].name == L"file10.txt");

    fs::DirectoryModel::Sort(sortable, fs::SortKey::Size, fs::SortOrder::Descending);
    CHECK(sortable[0].name == L"dir10");
    CHECK(sortable[1].name == L"dir2");
    CHECK(sortable[2].name == L"file2.txt");
    CHECK(sortable[3].name == L"file10.txt");

    fs::DirectoryModel::Sort(sortable, fs::SortKey::Type, fs::SortOrder::Ascending);
    CHECK(sortable[0].isDir);
    CHECK(sortable[1].isDir);
    CHECK(!sortable[2].isDir);
    CHECK(!sortable[3].isDir);

    fs::DirectoryModel::Sort(sortable, fs::SortKey::Modified, fs::SortOrder::Descending);
    CHECK(sortable[0].name == L"file10.txt");
    CHECK(sortable[1].name == L"dir10");
    CHECK(sortable[2].name == L"file2.txt");
    CHECK(sortable[3].name == L"dir2");

    // 不存在的路径
    auto missing = fs::DirectoryModel::Load(JoinPath(srcDir, L"__no_such_dir__"), false);
    CHECK(missing.notFound);
    CHECK(missing.entries.empty());

    // 相对路径应被规范化成绝对路径
    auto rel = fs::DirectoryModel::Load(L"src", true);
    CHECK(IsRootPath(rel.path) || rel.path.find(L":\\") != std::wstring::npos);
}

static void TestDirectoryWatcher() {
    std::printf("[DirectoryWatcher]\n");
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        CHECK(false);
        return;
    }
    const std::wstring dir = JoinPath(
        temp, L"ZExplorer-watch-" + std::to_wstring(GetCurrentProcessId()));
    CreateDirectoryPath(dir, nullptr);
    DirectoryWatcher watcher;
    CHECK(watcher.Start(dir));
    const std::wstring file = JoinPath(dir, L"watch-probe.txt");
    CHECK(CreateEmptyFile(file, nullptr));

    bool changed = false;
    for (int i = 0; i < 30 && !changed; ++i) {
        Sleep(50);
        changed = watcher.Poll();
    }
    CHECK(changed);
    watcher.Stop();
    OpResult cleanup;
    DeletePermanent(dir, &cleanup);
    CHECK(cleanup.issues.empty());
}

static void TestFileOps() {
    std::printf("[FileOps]\n");

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = JoinPath(std::wstring(tmp), L"zexplorer_ops_test");

    OpResult r;
    DeletePermanent(root, &r);                      // 清残留
    CHECK(CreateDirectoryPath(root, nullptr));
    CHECK(CreateDirectoryPath(JoinPath(root, L"sub"), nullptr));
    CHECK(CreateEmptyFile(JoinPath(root, L"a.txt"), nullptr));
    CHECK(CreateEmptyFile(JoinPath(root, L"sub\\b.txt"), nullptr));

    CHECK(FileNameOf(MakeUniquePath(JoinPath(root, L"a.txt"))) == L"a (2).txt");

    // 复制目录（含子文件）
    int progressCalls = 0;
    OpProgress lastProgress;
    const std::wstring copied = JoinPath(root, L"sub_copy");
    OpResult copyResult;
    CopyTree(JoinPath(root, L"sub"), copied,
             [&](const OpProgress& p) {
                 ++progressCalls;
                 lastProgress = p;
                 return true;
             }, &copyResult);
    CHECK(copyResult.issues.empty());
    CHECK(copyResult.okCount > 0);
    CHECK(lastProgress.totalFiles == lastProgress.doneFiles);
    CHECK(lastProgress.totalBytes == lastProgress.doneBytes);
    CHECK(GetFileAttributesW(JoinPath(copied, L"b.txt").c_str()) != INVALID_FILE_ATTRIBUTES);

    // 取消
    OpResult cancelResult;
    CopyTree(JoinPath(root, L"sub"), JoinPath(root, L"sub_copy2"),
             [](const OpProgress&) { return false; }, &cancelResult);
    CHECK(cancelResult.canceled);
    OpResult cleanup;
    DeletePermanent(JoinPath(root, L"sub_copy2"), &cleanup);

    // 单项失败不中断：拷一个不存在的文件 + 一个存在的文件
    OpResult partial;
    CopyTree(JoinPath(root, L"__no_such__"), JoinPath(root, L"nope"), nullptr, &partial);
    CHECK(!partial.issues.empty());                 // 失败被记录
    CopyTree(JoinPath(root, L"a.txt"), JoinPath(root, L"a_copy.txt"), nullptr, &partial);
    CHECK(GetFileAttributesW(JoinPath(root, L"a_copy.txt").c_str()) != INVALID_FILE_ATTRIBUTES);

    // 重命名
    std::wstring err;
    CHECK(RenamePath(JoinPath(root, L"a.txt"), JoinPath(root, L"renamed.txt"), &err));
    CHECK(GetFileAttributesW(JoinPath(root, L"renamed.txt").c_str()) != INVALID_FILE_ATTRIBUTES);

    // 永久删除目录
    OpResult delResult;
    DeletePermanent(copied, &delResult);
    CHECK(delResult.issues.empty());
    CHECK(GetFileAttributesW(copied.c_str()) == INVALID_FILE_ATTRIBUTES);

    // 目标在源目录内部必须拒绝（否则边复制边枚举会无限递归）
    OpResult selfCopy;
    CopyTree(root, JoinPath(root, L"inner"), nullptr, &selfCopy);
    CHECK(!selfCopy.issues.empty());
    CHECK(!selfCopy.issues.empty() && selfCopy.issues.front().message.find(L"内部") != std::wstring::npos);

    OpResult finalDel;
    DeletePermanent(root, &finalDel);
    CHECK(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES);
}

static void TestFileAttributes() {
    std::printf("[FileAttributes]\n");

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = JoinPath(std::wstring(tmp), L"zexplorer_attr_test");
    const std::wstring file = JoinPath(root, L"zone.txt");

    OpResult cleanup;
    DeletePermanent(root, &cleanup);
    CHECK(CreateDirectoryPath(root, nullptr));
    CHECK(CreateEmptyFile(file, nullptr));

    platform::FileAttributesInfo info;
    std::wstring error;
    CHECK(platform::ReadFileAttributes(file, &info, &error));
    CHECK(!info.directory);
    CHECK(!info.reparsePoint);

    CHECK(platform::SetBasicFileAttributes(file, true, true, false, true, &error));
    CHECK(platform::ReadFileAttributes(file, &info, &error));
    CHECK(info.readOnly);
    CHECK(info.hidden);
    CHECK(info.archive);

    CHECK(platform::SetEditableFileAttributes(
        file, false, false, false, true, true, true, false, false, &error));
    CHECK(platform::ReadFileAttributes(file, &info, &error));
    CHECK(info.temporary);
    CHECK(info.notContentIndexed);
    CHECK(platform::SetEditableFileAttributes(
        file, false, false, false, true, false, false, false, false, &error));

    const std::wstring zone = L"[ZoneTransfer]\r\nZoneId=3\r\n";
    const bool zoneWritten = platform::WriteZoneIdentifier(file, zone, &error);
    if (!zoneWritten) {
        std::printf("  Zone write failed: errorSize=%zu lastError=%lu text=[%ls]\n",
                    error.size(), GetLastError(), error.c_str());
    }
    CHECK(zoneWritten);
    auto readZone = platform::ReadZoneIdentifier(file);
    CHECK(readZone.status == platform::ZoneIdentifierStatus::Ok);
    CHECK(readZone.content == zone);

    CHECK(platform::WriteZoneIdentifier(file, L"", &error));
    readZone = platform::ReadZoneIdentifier(file);
    CHECK(readZone.status == platform::ZoneIdentifierStatus::Missing);

    platform::SetBasicFileAttributes(file, false, false, false, true, &error);
    OpResult finalCleanup;
    DeletePermanent(root, &finalCleanup);
    CHECK(finalCleanup.issues.empty());
}

static void TestNtfsStreams() {
    std::printf("[NtfsStreams]\n");

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = JoinPath(std::wstring(tmp), L"zexplorer_stream_test");
    const std::wstring base = JoinPath(root, L"host.bin");
    const std::wstring source = JoinPath(root, L"payload.txt");
    const std::wstring exported = JoinPath(root, L"exported.txt");

    OpResult cleanup;
    DeletePermanent(root, &cleanup);
    CHECK(CreateDirectoryPath(root, nullptr));
    CHECK(CreateEmptyFile(base, nullptr));
    {
        HANDLE h = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        const char payload[] = "payload-inside-ntfs-stream";
        DWORD written = 0;
        CHECK(h != INVALID_HANDLE_VALUE &&
              WriteFile(h, payload, sizeof(payload) - 1, &written, nullptr));
        CHECK(written == sizeof(payload) - 1);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    OpResult importResult;
    CHECK(platform::CopyFileToAlternateStream(
        base, L"HiddenPayload", source, false, nullptr, &importResult));
    CHECK(importResult.Ok());

    OpResult duplicateImport;
    CHECK(!platform::CopyFileToAlternateStream(
        base, L"HiddenPayload", source, false, nullptr, &duplicateImport));
    CHECK(!duplicateImport.issues.empty());

    OpResult cancelledImport;
    CHECK(!platform::CopyFileToAlternateStream(
        base, L"CancelledPayload", source, false,
        [](const platform::OpProgress&) { return false; }, &cancelledImport));
    CHECK(cancelledImport.canceled);

    std::vector<platform::AlternateStream> streams;
    std::wstring error;

    CHECK(platform::WriteAlternateStreamText(
        base, L"QuickText", L"quick stream text", false, &error));
    std::wstring streamText;
    CHECK(platform::ReadAlternateStreamText(base, L"QuickText", &streamText, &error));
    CHECK(streamText == L"quick stream text");
    OpResult renameResult;
    CHECK(platform::RenameAlternateStream(
        base, L"QuickText", L"RenamedText", nullptr, &renameResult));
    CHECK(platform::ListAlternateStreams(base, &streams, &error));
    bool sawRenamed = false;
    bool sawOld = false;
    for (const auto& stream : streams) {
        if (stream.name == L"RenamedText") sawRenamed = true;
        if (stream.name == L"QuickText") sawOld = true;
    }
    CHECK(sawRenamed);
    CHECK(!sawOld);

    CHECK(platform::ListAlternateStreams(base, &streams, &error));
    CHECK(streams.size() == 3);
    auto named = std::find_if(streams.begin(), streams.end(), [](const auto& s) {
        return !s.isDefault && s.name == L"HiddenPayload";
    });
    CHECK(named != streams.end());

    OpResult exportResult;
    CHECK(platform::CopyAlternateStreamToFile(
        base, L"HiddenPayload", exported, false, nullptr, &exportResult));
    CHECK(exportResult.Ok());
    CHECK(GetFileAttributesW(exported.c_str()) != INVALID_FILE_ATTRIBUTES);
    OpResult overwriteExport;
    CHECK(platform::CopyAlternateStreamToFile(
        base, L"HiddenPayload", exported, true, nullptr, &overwriteExport));
    CHECK(overwriteExport.Ok());
    OpResult cancelledExport;
    CHECK(!platform::CopyAlternateStreamToFile(
        base, L"HiddenPayload", exported, true,
        [](const platform::OpProgress&) { return false; }, &cancelledExport));
    CHECK(cancelledExport.canceled);
    {
        HANDLE h = CreateFileW(exported.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        char roundTrip[64]{};
        DWORD read = 0;
        CHECK(h != INVALID_HANDLE_VALUE &&
              ReadFile(h, roundTrip, sizeof(roundTrip) - 1, &read, nullptr));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CHECK(std::string(roundTrip, read) == "payload-inside-ntfs-stream");
    }

    CHECK(platform::DeleteAlternateStream(base, L"HiddenPayload", &error));
    CHECK(platform::DeleteAlternateStream(base, L"RenamedText", &error));
    CHECK(platform::ListAlternateStreams(base, &streams, &error));
    CHECK(streams.size() == 1);

    CHECK(!platform::ValidateAlternateStreamName(L"bad:name", &error));
    CHECK(platform::ValidateAlternateStreamName(L"Zone.Identifier", &error));

    OpResult finalCleanup;
    DeletePermanent(root, &finalCleanup);
    CHECK(finalCleanup.issues.empty());
}

static void TestFileLock() {
    std::printf("[FileLock]\n");

    wchar_t selfPath[32768]{};
    const DWORD selfLength = GetModuleFileNameW(
        nullptr, selfPath, static_cast<DWORD>(std::size(selfPath)));
    CHECK(selfLength > 0 && selfLength < std::size(selfPath));
    std::vector<platform::LockingProcess> selfLocks;
    std::wstring selfError;
    if (selfLength > 0) {
        CHECK(platform::EnumerateFileLocks(
            std::wstring(selfPath, selfLength), &selfLocks, &selfError));
        const auto current = std::find_if(
            selfLocks.begin(), selfLocks.end(), [](const auto& item) {
                return item.pid == GetCurrentProcessId();
            });
        CHECK(current == selfLocks.end());
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = JoinPath(std::wstring(tmp), L"zexplorer_lock_test");
    const std::wstring file = JoinPath(root, L"unlocked.txt");
    OpResult cleanup;
    DeletePermanent(root, &cleanup);
    CHECK(CreateDirectoryPath(root, nullptr));
    CHECK(CreateEmptyFile(file, nullptr));

    std::vector<platform::LockingProcess> locks;
    std::wstring error;
    CHECK(platform::EnumerateFileLocks(file, &locks, &error));
    CHECK(locks.empty());
    CHECK(!platform::TerminateProcessById(GetCurrentProcessId(), &error));

    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    platform::FileLockReport report;
    CHECK(platform::EnumerateFileLocks(file, &locks, &error, &report,
                                       /*includeCurrentProcess*/ true));
    CHECK(!locks.empty());
    CHECK(report.handleScanCompleted);
    CHECK(report.deleteProbeAttempted);
    CHECK(!report.deleteOpenSucceeded);
    CHECK(report.deleteProbeError == ERROR_SHARING_VIOLATION);
    auto self = std::find_if(locks.begin(), locks.end(), [](const auto& item) {
        return item.pid == GetCurrentProcessId();
    });
    CHECK(self != locks.end());
    if (self != locks.end() && !self->handles.empty()) {
        CHECK(self->handles.front().closable);
        const ULONG_PTR handle = self->handles.front().value;
        if (handle != 0) {
            CHECK(platform::CloseRemoteFileHandle(
                self->pid, handle, &error));
            locked = INVALID_HANDLE_VALUE; // DUPLICATE_CLOSE_SOURCE invalidated it.
        }
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);

    // A read-only handle with full sharing does not prevent deletion, but it
    // must still be visible in the handle list and closeable.
    HANDLE shared = CreateFileW(
        file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(shared != INVALID_HANDLE_VALUE);
    HANDLE sharedSecond = CreateFileW(
        file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(sharedSecond != INVALID_HANDLE_VALUE);
    CHECK(platform::EnumerateFileLocks(file, &locks, &error, &report,
                                       /*includeCurrentProcess*/ true));
    CHECK(report.handleScanCompleted);
    CHECK(report.deleteProbeAttempted);
    CHECK(report.deleteOpenSucceeded);
    self = std::find_if(locks.begin(), locks.end(), [](const auto& item) {
        return item.pid == GetCurrentProcessId();
    });
    CHECK(self != locks.end());
    if (self != locks.end()) {
        CHECK(self->foundByHandleScan);
        CHECK(self->handles.size() >= 2);
    }
    if (sharedSecond != INVALID_HANDLE_VALUE) CloseHandle(sharedSecond);
    if (shared != INVALID_HANDLE_VALUE) CloseHandle(shared);

    // Directories are unlockable too. Open with FILE_FLAG_BACKUP_SEMANTICS
    // because CreateFile refuses directory handles otherwise.
    const std::wstring folder = JoinPath(root, L"locked-folder");
    CHECK(CreateDirectoryPath(folder, nullptr));
    HANDLE lockedFolder = CreateFileW(
        folder.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(lockedFolder != INVALID_HANDLE_VALUE);
    CHECK(platform::EnumerateFileLocks(
        folder, &locks, &error, &report, /*includeCurrentProcess*/ true));
    self = std::find_if(locks.begin(), locks.end(), [](const auto& item) {
        return item.pid == GetCurrentProcessId();
    });
    CHECK(self != locks.end());
    if (self != locks.end() && !self->handles.empty()) {
        CHECK(platform::CloseRemoteFileHandle(
            self->pid, self->handles.front().value, &error));
        lockedFolder = INVALID_HANDLE_VALUE;
    }
    if (lockedFolder != INVALID_HANDLE_VALUE) CloseHandle(lockedFolder);

    OpResult finalCleanup;
    DeletePermanent(root, &finalCleanup);
    CHECK(finalCleanup.issues.empty());
}

static void TestWinEKeyState() {
    using platform::WinEKeyAction;
    using platform::WinEKeyState;
    std::printf("[WinEKeyState]\n");

    WinEKeyState state;
    CHECK(state.Handle(VK_LWIN, true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle('E', true, false) == WinEKeyAction::Launch);
    CHECK(state.Handle('E', true, false) == WinEKeyAction::Swallow);
    CHECK(state.Handle('E', false, true) == WinEKeyAction::Swallow);
    CHECK(state.Handle(VK_LWIN, false, true) == WinEKeyAction::MaskAndPass);
    // The critical regression: a later plain E must not launch after our
    // swallowed Win-up.
    CHECK(state.Handle('E', true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle('E', false, true) == WinEKeyAction::Pass);

    state.Reset();
    CHECK(state.Handle(VK_RWIN, true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle('E', true, false) == WinEKeyAction::Launch);
    CHECK(state.Handle(VK_RWIN, false, true) == WinEKeyAction::MaskAndPass);
    CHECK(state.Handle('E', false, true) == WinEKeyAction::Swallow);

    state.Reset();
    CHECK(state.Handle(VK_LWIN, true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle(VK_LCONTROL, true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle('E', true, false) == WinEKeyAction::Pass);
    CHECK(state.Handle('E', false, true) == WinEKeyAction::Pass);
    CHECK(state.Handle(VK_LCONTROL, false, true) == WinEKeyAction::Pass);
    CHECK(state.Handle(VK_LWIN, false, true) == WinEKeyAction::Pass);
}

static void TestDirectorySearch() {
    std::printf("[DirectorySearch]\n");
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root =
        platform::JoinPath(std::wstring(tmp), L"zexplorer_search_test");
    OpResult cleanup;
    platform::DeletePermanent(root, &cleanup);
    CHECK(platform::CreateDirectoryPath(root, nullptr));
    const std::wstring one = platform::JoinPath(root, L"one");
    const std::wstring nested = platform::JoinPath(one, L"two");
    CHECK(platform::CreateDirectoryPath(one, nullptr));
    CHECK(platform::CreateDirectoryPath(nested, nullptr));
    CHECK(platform::CreateEmptyFile(
        platform::JoinPath(root, L"needle-top.txt"), nullptr));
    CHECK(platform::CreateEmptyFile(
        platform::JoinPath(nested, L"needle-deep.txt"), nullptr));
    CHECK(platform::CreateEmptyFile(
        platform::JoinPath(nested, L"other.txt"), nullptr));

    const auto results =
        fs::DirectoryModel::Search(root, L"needle", false);
    CHECK_EQ_W(std::to_wstring(results.size()), L"2");
    auto top = std::find_if(results.begin(), results.end(), [](const auto& e) {
        return e.name == L"needle-top.txt";
    });
    auto deep = std::find_if(results.begin(), results.end(), [](const auto& e) {
        return e.name == L"needle-deep.txt";
    });
    CHECK(top != results.end());
    CHECK(deep != results.end());
    OpResult finalCleanup;
    platform::DeletePermanent(root, &finalCleanup);
    CHECK(finalCleanup.issues.empty());
}

static void TestRecycleBinInventory() {
    std::printf("[RecycleBin]\n");
    const platform::RecycleBinInfo info = platform::QueryRecycleBin();
    const auto items = platform::QueryRecycleBinItems();
    std::printf("  available=%d shellCount=%llu items=%zu\n",
                info.available ? 1 : 0,
                static_cast<unsigned long long>(info.itemCount),
                items.size());
    if (!items.empty()) {
        std::printf("  first=[%ls] from=[%ls] type=[%ls] pidl=%zu\n",
                    items.front().name.c_str(),
                    items.front().originalPath.c_str(),
                    items.front().typeName.c_str(),
                    items.front().relativePidl.size());
        CHECK(!items.front().relativePidl.empty());
        CHECK(!items.front().name.empty());
    }
}

int main() {
    std::printf("ZExplorer platform tests\n");
    TestPathUtil();
    TestDirectoryModel();
    TestFileAttributes();
    TestNtfsStreams();
    TestWinEKeyState();
    TestDirectorySearch();
    TestRecycleBinInventory();
    TestFileLock();
    TestFileOps();
    TestDirectoryWatcher();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
