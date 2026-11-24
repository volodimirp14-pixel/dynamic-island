#pragma once
#include <string>

namespace di::paths {
// %APPDATA%\DynamicIsland          -- settings.json, state.json (kept on update/uninstall by default)
std::wstring ConfigDir();
// %LOCALAPPDATA%\DynamicIsland\logs -- log files
std::wstring LogDir();
// Creates the folder (and parents). Returns false if it does not exist afterwards.
bool EnsureDir(const std::wstring& path);
}  // namespace di::paths
