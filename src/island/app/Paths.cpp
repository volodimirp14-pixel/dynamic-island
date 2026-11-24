#include "Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace di::paths {
namespace {
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        out = raw;
    }
    CoTaskMemFree(raw);
    return out;
}
}  // namespace

std::wstring ConfigDir() {
    return KnownFolder(FOLDERID_RoamingAppData) + L"\\DynamicIsland";
}

std::wstring LogDir() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\DynamicIsland\\logs";
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    const int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS) {
        const DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    return false;
}
}  // namespace di::paths
