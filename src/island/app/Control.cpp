#include "Control.h"

#include <cwchar>

#include "common/Ipc.h"
#include "common/Log.h"

namespace di::control {
namespace {
Handlers g_handlers;

void Dispatch(const std::wstring& cmd) {
    DI_LOG(L"[Control] command: %s", cmd.c_str());
    if (cmd == L"reload") {
        if (g_handlers.reload) g_handlers.reload();
    } else if (cmd == L"quit") {
        if (g_handlers.quit) g_handlers.quit();
    } else if (cmd == L"show") {
        if (g_handlers.show) g_handlers.show();
    } else if (cmd.rfind(L"progress ", 0) == 0) {
        if (g_handlers.progress) g_handlers.progress(_wtoi(cmd.c_str() + 9));
    } else {
        DI_LOG(L"[Control] unknown command ignored");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!cds || cds->dwData != ipc::kCommandMagic || !cds->lpData || cds->cbData > 4096) return FALSE;
            Dispatch(std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)));
            return TRUE;
        }
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam && g_handlers.endSession) g_handlers.endSession();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}  // namespace

HWND Create(const Handlers& handlers) {
    g_handlers = handlers;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = ipc::kControlWindowClass;
    RegisterClassExW(&wc);
    // Message-only windows do not get WM_ENDSESSION, so this is a hidden
    // top-level window instead (never shown, no taskbar button).
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, ipc::kControlWindowClass, L"Dynamic Island Control", WS_POPUP, 0, 0,
                                0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) DI_LOG(L"[Control] CreateWindowEx failed (%lu)", GetLastError());
    return hwnd;
}

}  // namespace di::control
