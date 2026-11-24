#include "SettingsWatcher.h"

#include <windows.h>

#include <thread>

#include "../app/Paths.h"
#include "../log/Log.h"

namespace di::settings {
namespace {
HANDLE g_stop = nullptr;
std::thread g_thread;
}  // namespace

void StartWatcher(std::function<void()> onChange) {
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread([onChange = std::move(onChange)] {
        HANDLE change = FindFirstChangeNotificationW(paths::ConfigDir().c_str(), FALSE,
                                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (change == INVALID_HANDLE_VALUE) {
            DI_LOG(L"[Settings] cannot watch the settings folder (%lu)", GetLastError());
            return;
        }
        HANDLE handles[] = {g_stop, change};
        while (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            // Editors save in several steps; let the burst settle, then reload once.
            while (WaitForSingleObject(g_stop, 300) == WAIT_TIMEOUT) {
                if (!FindNextChangeNotification(change)) break;
                if (WaitForSingleObject(change, 0) != WAIT_OBJECT_0) break;
            }
            if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
            onChange();
            if (!FindNextChangeNotification(change)) break;
        }
        FindCloseChangeNotification(change);
    });
}

void StopWatcher() {
    if (g_stop) SetEvent(g_stop);
    if (g_thread.joinable()) g_thread.join();
    if (g_stop) CloseHandle(g_stop);
    g_stop = nullptr;
}
}  // namespace di::settings
