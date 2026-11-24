#pragma once
#include <string>

// "Start with Windows": HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
// Per user, no administrator rights; also visible in Task Manager > Startup apps.
namespace di::launcher::autostart {
bool IsEnabled();
bool Set(bool enabled, const std::wstring& exePath);
}  // namespace di::launcher::autostart
