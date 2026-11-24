#pragma once
#include <windows.h>

namespace di {
// Owns the per-session instance mutex for the island's lifetime.
class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    // false when another island already runs in this Windows session.
    bool IsFirst() const { return first_; }

private:
    HANDLE mutex_ = nullptr;
    bool first_ = false;
};

// true while the old Windhawk mod version of the island is running.
bool IsWindhawkIslandRunning();
}  // namespace di
