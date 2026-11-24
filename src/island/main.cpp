// Dynamic Island — standalone (no Windhawk). Stage 2a: a diagnostic build that
// checks what the island needs from Windows (DPI awareness from our own
// manifest, single instance, settings/log folders, notification access).
// The island itself is ported in stage 2b.
#include <windows.h>

#include "../shared/Version.h"
#include "app/Paths.h"
#include "app/SingleInstance.h"
#include "diag/Diagnostics.h"
#include "log/Log.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring logDir = di::paths::LogDir();
    di::paths::EnsureDir(logDir);
    di::log::Init(logDir, L"island");
    DI_LOG(L"Dynamic Island %s starting (pid %lu).", di::kAppVersion, GetCurrentProcessId());

    di::SingleInstance instance;
    if (!instance.IsFirst()) {
        DI_LOG(L"Another instance is already running; exiting.");
        MessageBoxW(nullptr, L"Dynamic Island уже запущен.", di::kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const di::diag::Report report = di::diag::Run();
    MessageBoxW(nullptr, report.text.c_str(), L"Dynamic Island — проверка",
                MB_OK | (report.notificationsOk ? MB_ICONINFORMATION : MB_ICONWARNING));
    DI_LOG(L"Diagnostic build finished.");
    return 0;
}
