"""One-off port: windhawk/dynamic-island-custom.wh.cpp -> src/island/IslandCore.cpp.
Replaces every Windhawk API use with the standalone equivalents. Kept in the
repo so the transformation can be reviewed and re-run."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit(f"expected {count} x {old[:70]!r}, found {n}")
    s = s.replace(old, new)

# 1. Header: drop the Windhawk metadata, README and YAML settings blocks.
start = s.index("#define WIN32_LEAN_AND_MEAN")
s = s[start:]
rep("#define WIN32_LEAN_AND_MEAN\n#define NOMINMAX\n", """// Dynamic Island -- island core (standalone; ported from the Windhawk mod
// "dynamic-island-for-windows-custom" 1.4.0-custom.20). Settings come from
// settings.json (see settings/Settings.h), logs from log/Log.h; the entry
// point and lifecycle live in main.cpp / the di::island API at the bottom.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
""")
rep("using Microsoft::WRL::ComPtr;\n", """#include "Island.h"
#include "../shared/Version.h"
#include "app/Paths.h"
#include "log/Log.h"
#include "settings/Settings.h"

using Microsoft::WRL::ComPtr;
""")

# 2. Windhawk API -> standalone.
s = re.sub(r"\bWh_Log\(", "DI_LOG(", s)
s = re.sub(r"\bWh_GetIntSetting\(", "di::settings::GetInt(", s)
s = re.sub(r"\bWh_GetIntValue\(", "di::state::GetInt(", s)
s = re.sub(r"\bWh_SetIntValue\(", "di::state::SetInt(", s)
rep("""    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring result = value ? value : L"";
    Wh_FreeStringSetting(value);
    return result;""", """    return di::settings::GetString(name);""")
rep("""    wchar_t buffer[2048] = {};
    Wh_GetStringValue(kBackupValue, buffer, ARRAYSIZE(buffer));
    std::wstring all = buffer;""", """    const std::wstring all = di::state::GetString(kBackupValue);""")
rep("Wh_SetStringValue(kBackupValue, all.c_str());", "di::state::SetString(kBackupValue, all);")

# 3. Own window class name.
rep('constexpr wchar_t kWindowClass[] = L"Windhawk.DynamicIslandForWindows";',
    'constexpr wchar_t kWindowClass[] = L"DynamicIsland.Island";')

# 4. Game detection: our Launcher is never "a game" (was: Windhawk's window).
rep("""           lowerTitle.find(L"windhawk") != std::wstring::npos ||
           base == L"explorer.exe" ||
           base == L"windhawk.exe";""", """           base == L"explorer.exe" ||
           base == L"dynamicislandlauncher.exe";""")

# 5. ProgressPercent: set by the "progress" command instead of Windhawk storage.
rep("""    const int progress = di::state::GetInt(L"ProgressPercent", -1);""",
    """    const int progress = g_progressPercent.load();""")
rep("""    di::state::SetInt(L"ProgressPercent", -1);""", """    g_progressPercent.store(-1);""")
rep("constexpr wchar_t kWindowClass[]", """// Progress ring percent (0..100, -1 = none), set by `DynamicIsland.exe --progress N`
// or the Launcher. Replaces the Windhawk value "ProgressPercent" other tools wrote.
std::atomic<int> g_progressPercent{-1};

constexpr wchar_t kWindowClass[]""")

# 6. Context menu: "Open Windhawk settings" -> open settings.json (the Launcher replaces this in stage 3).
rep('AppendMenuW(menu, MF_STRING, 9, L"Open Windhawk settings");', 'AppendMenuW(menu, MF_STRING, 9, L"Настройки (settings.json)");')
a = s.index("        case 9: {\n            // Launch the Windhawk UI")
b = s.index("        case kShapeMenuIdBase:\n", a)
s = s[:a] + """        case 9: {
            // Opens settings.json in Notepad; changes apply as soon as it is saved
            // (SettingsWatcher). Stage 3 replaces this with the Launcher.
            const std::wstring path = di::settings::FilePath();
            HINSTANCE result = ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                             (L"\\"" + path + L"\\"").c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32) {
                DI_LOG(L"Failed to open %s.", path.c_str());
            }
            break;
        }
""" + s[b:]

# 7. Lifecycle: the Windhawk tool-mod entry points become the di::island API.
a = s.index("BOOL WhTool_ModInit() {")
s = s[:a] + """// ── Lifecycle (called from main.cpp) ─────────────────────────────────────────
namespace di::island {

bool Start() {
    // Custom fork: island Do Not Disturb survives restarts / reboots.
    g_quietMode = di::state::GetInt(L"QuietMode", 0) != 0;
    g_quietHideAt = 0.0;
    g_quietCollapsed = g_quietMode.load();  // already on at start: hidden, no animation
    LoadSettings();

    if (!StartThreads()) {
        StopThreads();
        return false;
    }

    g_layoutDirty = true;
    DI_LOG(L"Dynamic Island %s initialized.", di::kAppVersion);
    return true;
}

void ApplySettings() {
    LoadSettings();
}

void SetProgress(int percent) {
    g_progressPercent.store(percent < 0 ? -1 : std::min(percent, 100));
    g_layoutDirty = true;
    TriggerNudge();
}

void Stop() {
    static std::atomic<bool> s_stopped{false};
    if (s_stopped.exchange(true)) {
        return;
    }
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    StopThreads();
    // Custom fork: with the island off nothing would show Telegram messages, so
    // give Telegram its Windows banners back. Re-applied on the next start.
    TelegramBanner::RestoreAll();
    DI_LOG(L"Dynamic Island stopped.");
}

}  // namespace di::island
"""
open(dst, "w", encoding="utf-8", newline="\n").write(s)
left = re.findall(r"\bWh_\w+|WH_MOD_ID|WhTool_\w+", s)
print("remaining Windhawk identifiers:", sorted(set(left)))
print("lines:", s.count("\n"))
