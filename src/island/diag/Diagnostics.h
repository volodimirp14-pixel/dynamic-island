#pragma once
#include <string>

// Stage 2a check of everything the standalone island needs from Windows that
// Windhawk used to provide or that might behave differently outside it.
namespace di::diag {
struct Report {
    std::wstring text;  // human-readable, Russian (shown to the user and logged)
    bool notificationsOk = false;
};
Report Run();
}  // namespace di::diag
