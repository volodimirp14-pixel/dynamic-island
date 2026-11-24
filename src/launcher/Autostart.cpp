#include "Autostart.h"

#include <windows.h>

#include "common/Log.h"

namespace di::launcher::autostart {
namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr const wchar_t* kValueName = L"DynamicIsland";
}  // namespace

bool IsEnabled() {
    wchar_t data[MAX_PATH * 2] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS) {
        return false;
    }
    // Task Manager's "Disable" leaves the Run value and marks it here (first byte 3 = disabled).
    BYTE approved[12] = {};
    DWORD asize = sizeof(approved);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, approved, &asize) ==
            ERROR_SUCCESS &&
        asize > 0 && (approved[0] & 1)) {
        return false;
    }
    return true;
}

bool Set(bool enabled, const std::wstring& exePath) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    LSTATUS rc;
    if (enabled) {
        const std::wstring value = L"\"" + exePath + L"\"";
        rc = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kValueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    // Clear a "disabled in Task Manager" mark so turning it on here really turns it on.
    HKEY approved = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, KEY_SET_VALUE, &approved) == ERROR_SUCCESS) {
        RegDeleteValueW(approved, kValueName);
        RegCloseKey(approved);
    }
    DI_LOG(L"[Launcher] autostart %s: %s", enabled ? L"on" : L"off", rc == ERROR_SUCCESS ? L"ok" : L"failed");
    return rc == ERROR_SUCCESS;
}
}  // namespace di::launcher::autostart
