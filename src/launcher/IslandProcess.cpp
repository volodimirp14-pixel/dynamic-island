#include "IslandProcess.h"

#include <windows.h>

#include "common/Ipc.h"
#include "common/IpcClient.h"
#include "common/Log.h"
#include "common/Version.h"

namespace di::launcher {
std::wstring IslandExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"DynamicIsland.exe";
}

bool IsIslandRunning() {
    if (ipc::IsIslandRunning()) return true;
    // Starting up (instance lock taken, command window not created yet).
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);
    if (h) {
        CloseHandle(h);
        return true;
    }
    return false;
}

bool StartIsland(std::wstring* error) {
    if (IsIslandRunning()) return true;
    const std::wstring exe = IslandExePath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = L"Не найден DynamicIsland.exe рядом с Launcher (" + exe + L").";
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        if (error) *error = L"Windows не смогла запустить DynamicIsland.exe (ошибка " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    CloseHandle(pi.hThread);
    // Wait until it either shows its command window (running) or exits (refused to start).
    for (int i = 0; i < 100; ++i) {
        if (ipc::IsIslandRunning()) break;
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            if (error) *error = L"Остров закрылся сразу после запуска (код " + std::to_wstring(code) +
                                L"). Причина — в его сообщении или в логе.";
            CloseHandle(pi.hProcess);
            return false;
        }
    }
    CloseHandle(pi.hProcess);
    DI_LOG(L"[Launcher] island started");
    return true;
}

bool StopIsland(unsigned timeoutMs) {
    if (!IsIslandRunning()) return true;
    ipc::Send(L"quit");
    for (unsigned waited = 0; waited < timeoutMs; waited += 100) {
        if (!IsIslandRunning()) {
            DI_LOG(L"[Launcher] island stopped");
            return true;
        }
        Sleep(100);
    }
    DI_LOG(L"[Launcher] island did not stop within %u ms", timeoutMs);
    return false;
}

bool KillIsland() {
    HWND control = FindWindowW(ipc::kControlWindowClass, nullptr);
    DWORD pid = 0;
    if (control) GetWindowThreadProcessId(control, &pid);
    if (!pid) return !IsIslandRunning();
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!p) return false;
    const bool ok = TerminateProcess(p, 1) && WaitForSingleObject(p, 3000) == WAIT_OBJECT_0;
    CloseHandle(p);
    DI_LOG(L"[Launcher] island process %lu terminated: %d", pid, ok ? 1 : 0);
    return ok;
}
}  // namespace di::launcher
