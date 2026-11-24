#pragma once
// Standalone 0.11: two editions are built from the same code.
//   Pro   (DI_EDITION_PRO=1) -- everything.
//   Trial (DI_EDITION_PRO=0) -- the base: clock, music player, volume, brightness,
//                               battery (plus position, size, theme, animations,
//                               Do Not Disturb, hotkeys).
// Which settings are Pro-only is written in settings_schema.json ("pro": true, or
// "proOptions" for single choices of a setting). The settings layer enforces it, so
// the island needs no checks of its own: a Pro-only feature reads as switched off.
#ifndef DI_EDITION_PRO
#define DI_EDITION_PRO 1
#endif

namespace di {
inline constexpr bool kIsPro = DI_EDITION_PRO != 0;
inline constexpr const wchar_t* kEditionNameW = kIsPro ? L"Pro" : L"Trial";
inline constexpr const char* kEditionIdA = kIsPro ? "pro" : "trial";
}  // namespace di
