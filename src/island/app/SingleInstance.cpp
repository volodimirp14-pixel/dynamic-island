#include "SingleInstance.h"

#include "../../shared/Version.h"

namespace di {
SingleInstance::SingleInstance() {
    mutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    first_ = mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_) {
        if (first_) ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
}

bool IsWindhawkIslandRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kWindhawkModMutex);
    if (h) {
        CloseHandle(h);
        return true;
    }
    // Access denied also means it exists.
    return GetLastError() == ERROR_ACCESS_DENIED;
}
}  // namespace di
