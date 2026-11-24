#include "Diagnostics.h"

#include <windows.h>

#include <chrono>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

#include "common/Version.h"
#include "common/Paths.h"
#include "../app/SingleInstance.h"
#include "common/Log.h"

namespace di::diag {
namespace {
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

std::wstring WindowsBuild() {
    // RtlGetVersion is not affected by compatibility shims, unlike GetVersionEx.
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v = {sizeof(v)};
    if (!fn || fn(&v) != 0) return L"?";
    return std::to_wstring(v.dwMajorVersion) + L"." + std::to_wstring(v.dwMinorVersion) + L"." +
           std::to_wstring(v.dwBuildNumber);
}

std::wstring DpiMode() {
    const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return L"PerMonitorV2 (правильно)";
    switch (GetAwarenessFromDpiAwarenessContext(ctx)) {
        case DPI_AWARENESS_PER_MONITOR_AWARE: return L"PerMonitor v1 (не то, что нужно)";
        case DPI_AWARENESS_SYSTEM_AWARE: return L"System (ОШИБКА: остров будет размыт)";
        case DPI_AWARENESS_UNAWARE: return L"Unaware (ОШИБКА: манифест не применился)";
        default: return L"неизвестно";
    }
}

struct NotifResult {
    int access = -1;          // 0 unspecified, 1 allowed, 2 denied, -1 error/timeout
    int count = -1;           // toasts currently in the notification centre
    std::wstring error;
};

// Same calls and timeouts as the Windhawk mod (custom.3): RequestAccessAsync can
// hang in an unpackaged process, so every wait is bounded.
NotifResult ProbeNotifications() {
    NotifResult r;
    std::thread worker([&r] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto listener = UserNotificationListener::Current();
            auto access = listener.GetAccessStatus();
            if (access != UserNotificationListenerAccessStatus::Allowed) {
                auto request = listener.RequestAccessAsync();
                if (request.wait_for(std::chrono::seconds(10)) == winrt::Windows::Foundation::AsyncStatus::Completed) {
                    access = request.GetResults();
                } else {
                    try { request.Cancel(); } catch (...) {}
                    r.error = L"Windows не ответила на запрос доступа за 10 с";
                }
            }
            r.access = static_cast<int>(access);
            if (access == UserNotificationListenerAccessStatus::Allowed) {
                auto op = listener.GetNotificationsAsync(NotificationKinds::Toast);
                if (op.wait_for(std::chrono::seconds(5)) == winrt::Windows::Foundation::AsyncStatus::Completed) {
                    r.count = static_cast<int>(op.GetResults().Size());
                } else {
                    try { op.Cancel(); } catch (...) {}
                    r.error = L"список уведомлений не пришёл за 5 с";
                }
            }
        } catch (const winrt::hresult_error& e) {
            r.error = L"WinRT 0x" + [](uint32_t v) {
                wchar_t b[16];
                swprintf_s(b, L"%08X", v);
                return std::wstring(b);
            }(static_cast<uint32_t>(e.code().value)) + L": " + std::wstring(e.message());
        } catch (...) {
            r.error = L"неизвестная ошибка";
        }
        winrt::uninit_apartment();
    });
    worker.join();
    return r;
}
}  // namespace

Report Run() {
    Report rep;
    std::wstring& t = rep.text;
    t += L"Dynamic Island " + std::wstring(kAppVersion) + L" — проверка (этап 2a)\n\n";
    t += L"Windows: " + WindowsBuild() + L"\n";

    UINT dpi = GetDpiForSystem();
    t += L"Масштаб экрана (DPI): " + DpiMode() + L", " + std::to_wstring(dpi * 100 / 96) + L"%\n";

    const std::wstring cfg = paths::ConfigDir();
    t += L"Папка настроек: " + cfg + (paths::EnsureDir(cfg) ? L" — OK\n" : L" — НЕ создаётся!\n");
    t += L"Лог: " + log::FilePath() + L"\n";

    t += IsWindhawkIslandRunning() ? L"Мод Windhawk с островом: ЗАПУЩЕН (во время перехода это нормально)\n"
                                   : L"Мод Windhawk с островом: не запущен\n";

    const NotifResult n = ProbeNotifications();
    t += L"\nДоступ к уведомлениям: ";
    switch (n.access) {
        case 1: t += L"1 — РАЗРЕШЁН ✔"; rep.notificationsOk = true; break;
        case 2: t += L"2 — ЗАПРЕЩЁН (Параметры → Конфиденциальность → Уведомления → «Разрешить приложениям доступ к уведомлениям»)"; break;
        case 0: t += L"0 — не решено"; break;
        default: t += L"нет ответа"; break;
    }
    t += L"\n";
    if (n.count >= 0) t += L"Уведомлений в центре уведомлений сейчас: " + std::to_wstring(n.count) + L"\n";
    if (!n.error.empty()) t += L"Ошибка: " + n.error + L"\n";

    t += rep.notificationsOk ? L"\nИтог: программа без Windhawk может читать уведомления Telegram и Discord."
                             : L"\nИтог: доступа к уведомлениям нет — пришлите этот текст.";

    // Log line by line so the log stays readable.
    size_t start = 0;
    while (start < t.size()) {
        size_t end = t.find(L'\n', start);
        if (end == std::wstring::npos) end = t.size();
        if (end > start) DI_LOG(L"[Diag] %s", t.substr(start, end - start).c_str());
        start = end + 1;
    }
    return rep;
}
}  // namespace di::diag
