// Dynamic Island Launcher -- settings and control window.
//
// A plain Win32 window hosting WebView2 (Edge). The UI is one HTML page
// (src/launcher/ui/index.html, embedded as a resource) that talks to this file
// through chrome.webview.postMessage: it asks for the schema and values,
// changes settings, starts/stops the island. Settings are written straight to
// settings.json; the island notices the change by itself and is also told to
// reload. Closing the Launcher never affects the running island.
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <WebView2.h>

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <thread>

#include "Autostart.h"
#include "IslandProcess.h"
#include "common/IpcClient.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Resources.h"
#include "common/Settings.h"
#include "common/Utf.h"
#include "common/Version.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {
constexpr const wchar_t* kWindowClass = L"DynamicIsland.Launcher";
constexpr const wchar_t* kLauncherMutex = L"Local\\DynamicIsland.Launcher";
constexpr UINT WM_APP_POST_JSON = WM_APP + 1;  // lParam: new std::string (JSON) to send to the page

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

// Thread-safe: hands the JSON to the UI thread, which owns the WebView.
void PostJson(const json& j) {
    auto* text = new std::string(j.dump());
    if (!PostMessageW(g_hwnd, WM_APP_POST_JSON, 0, reinterpret_cast<LPARAM>(text))) delete text;
}

json StatusJson() {
    json j = {{"type", "status"},
              {"running", di::launcher::IsIslandRunning()},
              {"autostart", di::launcher::autostart::IsEnabled()},
              {"accent", nullptr}};
    const int rgb = di::ipc::QueryAccent();  // the island's live accent (album art while media plays)
    if (rgb >= 0) {
        char hex[8];
        snprintf(hex, sizeof(hex), "#%06X", rgb);
        j["accent"] = hex;
    }
    return j;
}

void Open(const std::wstring& target, const wchar_t* args = nullptr) {
    ShellExecuteW(g_hwnd, L"open", target.c_str(), args, nullptr, SW_SHOWNORMAL);
}

// Runs slow island operations off the UI thread, then reports the new status.
template <typename Fn>
void RunAsync(Fn fn) {
    std::thread([fn]() {
        json result = fn();
        PostJson(result);
        PostJson(StatusJson());
    }).detach();
}

void HandleMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "init") {
        json init = {{"type", "init"},
                     {"version", di::WideToUtf8(di::kAppVersion)},
                     {"schema", json::parse(di::settings::SchemaText())},
                     {"values", json::parse(di::settings::ValuesJson())},
                     {"settingsPath", di::WideToUtf8(di::settings::FilePath())},
                     {"problem", di::WideToUtf8(di::settings::LastProblem())},
                     {"islandFound", GetFileAttributesW(di::launcher::IslandExePath().c_str()) != INVALID_FILE_ATTRIBUTES}};
        PostJson(init);
        PostJson(StatusJson());
    } else if (type == "status") {
        PostJson(StatusJson());
    } else if (type == "set") {
        const std::string key = m.value("key", "");
        std::string error;
        const bool ok = m.contains("value") && di::settings::SetValue(key, m["value"].dump(), &error);
        if (ok) di::ipc::Send(L"reload");  // the island also watches the file; this is just faster
        PostJson({{"type", "saved"}, {"key", key}, {"ok", ok}, {"error", error},
                  {"values", json::parse(di::settings::ValuesJson())}});
    } else if (type == "reset") {
        const bool ok = di::settings::ResetToDefaults();
        if (ok) di::ipc::Send(L"reload");
        PostJson({{"type", "reset"}, {"ok", ok}, {"values", json::parse(di::settings::ValuesJson())}});
    } else if (type == "start") {
        RunAsync([] {
            std::wstring error;
            const bool ok = di::launcher::StartIsland(&error);
            return json{{"type", "action"}, {"action", "start"}, {"ok", ok}, {"error", di::WideToUtf8(error)}};
        });
    } else if (type == "stop") {
        RunAsync([] {
            const bool ok = di::launcher::StopIsland(6000);
            return json{{"type", "action"}, {"action", "stop"}, {"ok", ok}};
        });
    } else if (type == "restart") {
        RunAsync([] {
            std::wstring error;
            bool ok = di::launcher::StopIsland(6000);
            if (ok) ok = di::launcher::StartIsland(&error);
            else error = L"Остров не закрылся за 6 секунд.";
            return json{{"type", "action"}, {"action", "restart"}, {"ok", ok}, {"error", di::WideToUtf8(error)}};
        });
    } else if (type == "kill") {
        RunAsync([] { return json{{"type", "action"}, {"action", "kill"}, {"ok", di::launcher::KillIsland()}}; });
    } else if (type == "autostart") {
        const bool ok = di::launcher::autostart::Set(m.value("enabled", false), di::launcher::IslandExePath());
        PostJson({{"type", "action"}, {"action", "autostart"}, {"ok", ok}});
        PostJson(StatusJson());
    } else if (type == "open") {
        const std::string what = m.value("what", "");
        if (what == "logs") Open(di::paths::LogDir());
        else if (what == "settingsFolder") Open(di::paths::ConfigDir());
        else if (what == "settingsFile") Open(L"notepad.exe", (L"\"" + di::settings::FilePath() + L"\"").c_str());
        else if (what == "repo") Open(L"https://github.com/volodimirp14-pixel/dynamic-island");
    }
}

void ResizeWebView() {
    if (!g_controller) return;
    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    g_controller->put_Bounds(bounds);
}

void FailWebView(HRESULT hr) {
    DI_LOG(L"[Launcher] WebView2 failed: 0x%08X", static_cast<unsigned>(hr));
    const int choice = MessageBoxW(
        g_hwnd,
        L"Для окна Launcher нужен компонент Microsoft Edge WebView2 Runtime, но он не запустился.\n\n"
        L"Открыть страницу загрузки WebView2 Runtime? (Нужен вариант «Evergreen Bootstrapper».)\n\n"
        L"Сам остров работает и без Launcher.",
        L"Dynamic Island Launcher", MB_YESNO | MB_ICONWARNING);
    if (choice == IDYES) Open(L"https://developer.microsoft.com/microsoft-edge/webview2/");
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

void CreateWebView() {
    const std::wstring dataDir = di::paths::LogDir() + L"\\..\\WebView2";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    FailWebView(result);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                FailWebView(result);
                                return S_OK;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // No white flash while the page loads.
                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_controller.As(&c2))) {
                                const bool dark = SystemUsesDarkTheme();
                                c2->put_DefaultBackgroundColor(dark ? COREWEBVIEW2_COLOR{255, 28, 28, 30}
                                                                    : COREWEBVIEW2_COLOR{255, 243, 243, 245});
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
                            settings->put_AreDevToolsEnabled(FALSE);
#endif
                            EventRegistrationToken token;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                                            try {
                                                HandleMessage(json::parse(di::WideToUtf8(raw)));
                                            } catch (const std::exception& ex) {
                                                DI_LOG(L"[Launcher] bad message from page: %s",
                                                       di::Utf8ToWide(ex.what()).c_str());
                                            }
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);
                            // Links (GitHub, WebView2 page) open in the normal browser.
                            g_webview->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                            if (wcsncmp(uri, L"https://", 8) == 0) Open(uri);
                                            CoTaskMemFree(uri);
                                        }
                                        args->put_Handled(TRUE);
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

                            ResizeWebView();
                            const std::string_view html = di::res::LoadText(di::res::kLauncherUi);
                            g_webview->NavigateToString(di::Utf8ToWide(html).c_str());
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) FailWebView(hr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_GETMINMAXINFO: {
            const UINT dpi = GetDpiForWindow(hwnd);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = {MulDiv(760, dpi, 96), MulDiv(520, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_APP_POST_JSON: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lParam));
            if (g_webview) g_webview->PostWebMessageAsJson(di::Utf8ToWide(*text).c_str());
            return 0;
        }
        case WM_SETTINGCHANGE: {
            // Follow a light/dark switch in Windows for the title bar too.
            const BOOL dark = SystemUsesDarkTheme();
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            break;
        }
        case WM_DESTROY:
            if (g_controller) g_controller->Close();
            g_webview.Reset();
            g_controller.Reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // One Launcher at a time: a second start brings the first one forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kLauncherMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    const std::wstring logDir = di::paths::LogDir();
    di::paths::EnsureDir(logDir);
    di::log::Init(logDir, L"launcher");
    DI_LOG(L"Launcher %s starting.", di::kAppVersion);

    if (!di::settings::Init()) {
        MessageBoxW(nullptr, L"Не удалось загрузить настройки. Подробности в логе.", L"Dynamic Island Launcher",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(SystemUsesDarkTheme() ? RGB(28, 28, 30) : RGB(243, 243, 245));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(1040, dpi, 96);
    const int h = MulDiv(720, dpi, 96);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_hwnd = CreateWindowExW(0, kWindowClass, L"Dynamic Island", WS_OVERLAPPEDWINDOW,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                             w, h, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;
    const BOOL dark = SystemUsesDarkTheme();
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    CreateWebView();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    DI_LOG(L"Launcher closed.");
    return 0;
}
