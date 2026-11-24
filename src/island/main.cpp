// Dynamic Island -- standalone entry point (no Windhawk).
//
//   DynamicIsland.exe                 start the island (or bring a hidden one back)
//   DynamicIsland.exe --quit          stop the running island
//   DynamicIsland.exe --reload        make it re-read settings.json
//   DynamicIsland.exe --progress N    progress ring 0..100 (-1 hides it)
//   DynamicIsland.exe --diagnose      the stage 2a check window
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "common/Version.h"
#include "Island.h"
#include "app/Control.h"
#include "common/IpcClient.h"
#include "common/Edition.h"
#include "common/Security.h"
#include "common/Paths.h"
#include "app/SingleInstance.h"
#include "diag/Diagnostics.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "settings/SettingsWatcher.h"

namespace {
void Message(const wchar_t* text, UINT icon) {
    MessageBoxW(nullptr, text, di::kAppName, MB_OK | icon | MB_SETFOREGROUND);
}

// Returns the command to forward to a running island, or "" to start the island.
std::wstring CommandFromArgs(bool* diagnose) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--quit") cmd = L"quit";
        else if (a == L"--reload") cmd = L"reload";
        else if (a == L"--show") cmd = L"show";
        else if (a == L"--diagnose") *diagnose = true;
        else if (a == L"--progress" && i + 1 < argc) cmd = L"progress " + std::to_wstring(_wtoi(argv[++i]));
    }
    if (argv) LocalFree(argv);
    return cmd;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring logDir = di::paths::LogDir();
    di::paths::EnsureDir(logDir);
    di::log::Init(logDir, L"island");
    di::security::HardenProcess(/*disableExtensionPoints=*/true);
    DI_LOG(L"[Edition] %s", di::kEditionNameW);

    bool diagnose = false;
    const std::wstring command = CommandFromArgs(&diagnose);
    if (diagnose) {
        const di::diag::Report report = di::diag::Run();
        Message(report.text.c_str(), report.notificationsOk ? MB_ICONINFORMATION : MB_ICONWARNING);
        return 0;
    }
    if (!command.empty()) {
        if (!di::ipc::Send(command)) {
            Message(L"Dynamic Island не запущен.", MB_ICONINFORMATION);
            return 1;
        }
        return 0;
    }

    DI_LOG(L"Dynamic Island %s starting (pid %lu).", di::kAppVersion, GetCurrentProcessId());

    di::SingleInstance instance;
    if (!instance.IsFirst()) {
        // Double-click on the shortcut while it runs: just bring it back if hidden.
        if (di::ipc::Send(L"show")) {
            DI_LOG(L"Already running; asked it to show itself.");
            return 0;
        }
        // The mutex is held by a process that does not answer commands: an
        // older build (e.g. the 0.1.0 check window still open) or a hung island.
        DI_LOG(L"Another DynamicIsland.exe holds the instance lock but does not answer; not starting.");
        Message(L"Уже запущена другая копия DynamicIsland.exe, которая не отвечает "
                L"(например, старая проверочная версия с открытым окном).\n\n"
                L"Закройте её: Диспетчер задач (Ctrl+Shift+Esc) → «DynamicIsland.exe» → «Снять задачу», "
                L"и запустите программу снова.",
                MB_ICONWARNING);
        return 1;
    }

    if (di::IsWindhawkIslandRunning()) {
        DI_LOG(L"The Windhawk mod version of the island is running; not starting a second island.");
        Message(L"Сейчас работает версия Dynamic Island для Windhawk.\n\n"
                L"Два острова одновременно будут мешать друг другу (в том числе с уведомлениями Telegram и Discord).\n"
                L"Выключите мод «Dynamic Island for Windows (Custom)» в Windhawk и запустите программу снова.",
                MB_ICONWARNING);
        return 1;
    }

    if (!di::settings::Init()) {
        Message(L"Не удалось загрузить настройки (повреждена программа?). Подробности в логе.", MB_ICONERROR);
        return 1;
    }
    di::state::Init();

    if (!di::island::Start()) {
        DI_LOG(L"Island failed to start.");
        di::island::Stop();
        Message(L"Dynamic Island не смог запуститься. Подробности в логе.", MB_ICONERROR);
        return 1;
    }

    di::control::Handlers handlers;
    handlers.reload = [] {
        if (di::settings::Reload()) di::island::ApplySettings();
    };
    handlers.quit = [] { PostQuitMessage(0); };
    handlers.show = [] { di::island::Show(); };
    handlers.progress = [](int p) { di::island::SetProgress(p); };
    handlers.accent = [] { return di::island::CurrentAccent(); };
    handlers.timer = [](const std::wstring& a) { di::island::TimerCommand(a); };
    handlers.endSession = [] {
        DI_LOG(L"Windows is ending the session.");
        di::settings::StopWatcher();
        di::island::Stop();
    };
    HWND control = di::control::Create(handlers);

    // settings.json saved (Launcher or a text editor) -> ask the main thread to reload.
    di::settings::StartWatcher([control] {
        if (control) PostMessageW(control, WM_APP, 0, 0);
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == control && msg.message == WM_APP) {
            handlers.reload();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DI_LOG(L"Shutting down.");
    di::settings::StopWatcher();
    di::island::Stop();
    if (control) DestroyWindow(control);
    // Like the Windhawk version: end the process here, so no global WinRT/COM
    // object is torn down by static destructors after its apartment is gone.
    ExitProcess(0);
}
