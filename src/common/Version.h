#pragma once
// Single place for the app version. Bumped together with docs/CHANGELOG.md.
#define DI_VERSION_MAJOR 0
#define DI_VERSION_MINOR 9
#define DI_VERSION_PATCH 0
#define DI_VERSION_STRING_W L"0.9.0-alpha"
#define DI_VERSION_STRING_A "0.9.0-alpha"

namespace di {
inline constexpr const wchar_t* kAppName = L"Dynamic Island";
inline constexpr const wchar_t* kAppVersion = DI_VERSION_STRING_W;
// Name of the per-session mutex that keeps a single island running.
inline constexpr const wchar_t* kInstanceMutex = L"Local\\DynamicIsland.Instance";
// Mutex the Windhawk mod (dynamic-island-for-windows-custom) holds while running.
inline constexpr const wchar_t* kWindhawkModMutex = L"windhawk-tool-mod_dynamic-island-for-windows-custom";
}  // namespace di
