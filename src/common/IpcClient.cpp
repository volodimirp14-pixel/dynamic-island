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
}  // namespace di::ipc
