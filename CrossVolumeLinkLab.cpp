#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/FileOps.h"
#include "platform/PathUtil.h"

using namespace ze::platform;

namespace {

int checks = 0;
int failures = 0;

void Expect(const char* name, bool ok) {
    ++checks;
    if (ok) {
        std::printf("  [PASS] %s\n", name);
    } else {
        ++failures;
        std::printf("  [FAIL] %s\n", name);
    }
}

bool WriteText(const std::wstring& path, const char* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(strlen(text));
    const bool ok = WriteFile(h, text, size, &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

std::string ReadText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buffer[512]{};
    DWORD read = 0;
    const bool ok = ReadFile(h, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(h);
    return ok ? std::string(buffer, buffer + read) : std::string();
}

DWORD VolumeSerial(const std::wstring& path) {
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH)) return 0;
    DWORD serial = 0;
    if (!GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr,
                               nullptr, 0)) {
        return 0;
    }
    return serial;
}

std::wstring UniqueName(const wchar_t* prefix) {
    return std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
}

std::wstring VolumeRootArgument(wchar_t* value) {
    std::wstring root = value ? value : L"";
    while (!root.empty() && root.back() == L'"') root.pop_back();
    for (wchar_t& c : root) if (c == L'/') c = L'\\';
    if (root.size() == 2 && root[1] == L':') root.push_back(L'\\');
    return root;
}

bool CreateProbeDirectory(const std::wstring& path) {
    if (CreateDirectoryPath(path, nullptr)) return true;
    SetLastError(ERROR_SUCCESS);
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::printf("Usage: CrossVolumeLinkLab.exe <first-volume-root> <second-volume-root>\n");
        std::printf("Example: CrossVolumeLinkLab.exe C:\\ D:\\\n");
        return 2;
    }

    const std::wstring firstRoot = VolumeRootArgument(argv[1]);
    const std::wstring secondRoot = VolumeRootArgument(argv[2]);
    const DWORD firstSerial = VolumeSerial(firstRoot);
    const DWORD secondSerial = VolumeSerial(secondRoot);
    if (firstSerial == 0 || secondSerial == 0 || firstSerial == secondSerial) {
        std::printf("The two roots must be on distinct writable volumes.\n");
        return 2;
    }

    const std::wstring first =
        JoinPath(firstRoot, UniqueName(L"ZExplorerCrossLink-A-"));
    const std::wstring second =
        JoinPath(secondRoot, UniqueName(L"ZExplorerCrossLink-B-"));
    OpResult cleanup;
    DeletePermanent(first, &cleanup);
    DeletePermanent(second, &cleanup);
    const bool firstCreated = CreateProbeDirectory(first);
    const DWORD firstError = firstCreated ? ERROR_SUCCESS : GetLastError();
    const bool secondCreated = CreateProbeDirectory(second);
    const DWORD secondError = secondCreated ? ERROR_SUCCESS : GetLastError();
    if (!firstCreated || !secondCreated) {
        std::printf("Unable to create probe directories.\n");
        std::printf("first=[%ls] created=%d error=%lu\n", first.c_str(),
                    firstCreated ? 1 : 0, firstError);
        std::printf("second=[%ls] created=%d error=%lu\n", second.c_str(),
                    secondCreated ? 1 : 0, secondError);
        return 2;
    }
    std::printf("First volume : %ls\nSecond volume: %ls\n\n", first.c_str(), second.c_str());

    std::printf("[Hard links]\n");
    {
        const std::wstring original = JoinPath(first, L"hard-original.txt");
        const std::wstring retained = JoinPath(first, L"hard-retained.txt");
        const std::wstring moved = JoinPath(second, L"hard-moved.txt");
        std::wstring error;
        Expect("write hard-link source", WriteText(original, "hard-source"));
        Expect("create same-volume hard link",
               CreateHardLinkPath(retained, original, &error));
        Expect("hard link cannot be created across volumes",
               !CreateHardLinkPath(JoinPath(second, L"hard-illegal.txt"),
                                   original, &error));
        OpResult result;
        MoveTree(original, moved, nullptr, &result);
        Expect("cross-volume move of one hard-link name succeeds", result.Ok());
        Expect("moved source name disappears",
               GetFileAttributesW(original.c_str()) == INVALID_FILE_ATTRIBUTES);
        Expect("retained hard-link name still reads source",
               ReadText(retained) == "hard-source");
        Expect("destination file starts with source content",
               ReadText(moved) == "hard-source");
        Expect("change destination independently", WriteText(moved, "destination-only"));
        Expect("retained source name is unchanged by destination edit",
               ReadText(retained) == "hard-source");
    }

    std::printf("\n[File symbolic links]\n");
    {
        const std::wstring target = JoinPath(first, L"symbolic-target.txt");
        const std::wstring link = JoinPath(first, L"symbolic-link.txt");
        const std::wstring moved = JoinPath(second, L"symbolic-moved.txt");
        std::wstring error;
        Expect("write symbolic-link target",
               WriteText(target, "symbolic-target-content"));
        if (CreateSymbolicLinkPath(link, target, false, &error)) {
            OpResult result;
            MoveTree(link, moved, nullptr, &result);
            Expect("cross-volume symbolic-link move succeeds", result.Ok());
            Expect("source symbolic link disappears",
                   GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES);
            FileLinkInfo info;
            Expect("destination remains a symbolic link",
                   QueryFileLink(moved, &info, &error) &&
                       info.kind == FileLinkKind::SymbolicFile);
            Expect("destination link reaches original target",
                   ReadText(moved) == "symbolic-target-content");
            DeletePermanent(moved, &cleanup);
            Expect("deleting moved link preserves target",
                   ReadText(target) == "symbolic-target-content");
        } else {
            std::printf("  [SKIP] symbolic link unavailable: %ls\n", error.c_str());
        }
    }

    std::printf("\n[Directory junctions]\n");
    {
        const std::wstring target = JoinPath(first, L"junction-target");
        const std::wstring link = JoinPath(first, L"junction-link");
        const std::wstring moved = JoinPath(second, L"junction-moved");
        std::wstring error;
        Expect("create junction target directory", CreateDirectoryPath(target, nullptr));
        Expect("write junction target sentinel",
               WriteText(JoinPath(target, L"sentinel.txt"), "junction-sentinel"));
        if (CreateJunctionPath(link, target, &error)) {
            OpResult result;
            MoveTree(link, moved, nullptr, &result);
            Expect("cross-volume junction move succeeds", result.Ok());
            Expect("source junction disappears",
                   GetFileAttributesW(link.c_str()) == INVALID_FILE_ATTRIBUTES);
            FileLinkInfo info;
            Expect("destination remains a junction",
                   QueryFileLink(moved, &info, &error) &&
                       info.kind == FileLinkKind::Junction);
            Expect("junction points back to original volume target",
                   ReadText(JoinPath(moved, L"sentinel.txt")) == "junction-sentinel");
            Expect("target directory is not copied to destination volume",
                   GetFileAttributesW(JoinPath(second, L"sentinel.txt").c_str()) ==
                       INVALID_FILE_ATTRIBUTES);
            DeletePermanent(moved, &cleanup);
            Expect("deleting moved junction preserves target",
                   ReadText(JoinPath(target, L"sentinel.txt")) == "junction-sentinel");
        } else {
            std::printf("  [SKIP] junction unavailable: %ls\n", error.c_str());
        }
    }

    DeletePermanent(first, &cleanup);
    DeletePermanent(second, &cleanup);
    std::printf("\nChecks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
