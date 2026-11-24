#include "Control.h"

#include <cwchar>

#include "common/Ipc.h"
#include "common/Log.h"

namespace di::control {
namespace {
Handlers g_handlers;

LRESULT Dispatch(const std::wstring& cmd) {
    if (cmd == L"accent") {  // polled by the Launcher; not logged
        const unsigned rgb = g_handlers.accent ? g_handlers.accent() : 0;
        return rgb ? static_cast<LRESULT>(rgb) : TRUE;
    }
    DI_LOG(L"[Control] command: %s", cmd.c_str());
    if (cmd == L"reload") {
        if (g_handlers.reload) g_handlers.reload();
    } else if (cmd == L"quit") {
        if (g_handlers.quit) g_handlers.quit();
    } else if (cmd == L"show") {
        if (g_handlers.show) g_handlers.show();
    } else if (cmd.rfind(L"timer ", 0) == 0) {
        const std::wstring action = cmd.substr(6);
        if ((action == L"25" || action == L"50" || action == L"break" || action == L"pause" || action == L"stop") &&
            g_handlers.timer) {
            g_handlers.timer(action);
        }
    } else if (cmd.rfind(L"progress ", 0) == 0) {
        // Strict number: anything else (or out of range) is ignored.
        wchar_t* end = nullptr;
        const long value = wcstol(cmd.c_str() + 9, &end, 10);
        if (end && *end == L'\0' && value >= -1 && value <= 100 && g_handlers.progress) {
            g_handlers.progress(static_cast<int>(value));
        }
    } else {
        DI_LOG(L"[Control] unknown command ignored");
    }
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            // Standalone 0.9: only well-formed UTF-16 commands of sane size.
            if (!cds || cds->dwData != ipc::kCommandMagic || !cds->lpData || cds->cbData == 0 ||
                cds->cbData > 256 || (cds->cbData % sizeof(wchar_t)) != 0) {
                return FALSE;
            }
            return Dispatch(std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)));
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
