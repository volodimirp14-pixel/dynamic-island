#include "common/IpcClient.h"

#include <windows.h>

#include "common/Ipc.h"

namespace di::ipc {
bool IsIslandRunning() {
    return FindWindowW(kControlWindowClass, nullptr) != nullptr;
}

bool Send(const std::wstring& command) {
    HWND target = FindWindowW(kControlWindowClass, nullptr);
    if (!target) return false;
    COPYDATASTRUCT cds = {};
    cds.dwData = kCommandMagic;
    cds.cbData = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(command.c_str());
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000,
                               &result) != 0 &&
           result == TRUE;
}

int QueryAccent() {
    HWND target = FindWindowW(kControlWindowClass, nullptr);
    if (!target) return -1;
    const std::wstring command = L"accent";
    COPYDATASTRUCT cds = {};
    cds.dwData = kCommandMagic;
    cds.cbData = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(command.c_str());
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 250, &result)) {
        return -1;
    }
    return (result & 0xFF000000u) == 0x01000000u ? static_cast<int>(result & 0xFFFFFFu) : -1;
}
}  // namespace di::ipc
