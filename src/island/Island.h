#pragma once

// The island itself (IslandCore.cpp). Called from main.cpp on the main thread.
namespace di::island {
bool Start();            // load settings, start all threads and the island window
void Stop();             // stop everything, give Telegram/Discord their banners back; idempotent
void ApplySettings();    // re-read the current settings (after settings.json changed)
void Show();             // un-hide a manually hidden island
void SetProgress(int percent);  // progress ring 0..100, -1 hides it
}  // namespace di::island
