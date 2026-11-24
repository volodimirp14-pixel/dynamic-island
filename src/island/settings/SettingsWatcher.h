#pragma once
#include <functional>

// Watches %APPDATA%\DynamicIsland and calls onChange (on its own thread) after
// settings.json was written -- by the Launcher or by hand in a text editor.
namespace di::settings {
void StartWatcher(std::function<void()> onChange);
void StopWatcher();
}  // namespace di::settings
