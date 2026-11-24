#pragma once
#include <string>

// The island itself (IslandCore.cpp). Called from main.cpp on the main thread.
namespace di::island {
bool Start();            // load settings, start all threads and the island window
void Stop();             // stop everything, give Telegram/Discord their banners back; idempotent
void ApplySettings();    // re-read the current settings (after settings.json changed)
void Show();             // un-hide a manually hidden island
void SetProgress(int percent);  // progress ring 0..100, -1 hides it
unsigned CurrentAccent();
void TimerCommand(const std::wstring& action);  // focus timer: 25 | 50 | break | pause | stop       // live accent 0x01RRGGBB, 0 = not drawn yet
}  // namespace di::island
