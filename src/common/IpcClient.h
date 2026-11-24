#pragma once
#include <string>

// Client side of the island's command window (see Ipc.h). Used by the Launcher
// and by a second DynamicIsland.exe started with --quit/--reload/--show/--progress.
namespace di::ipc {
bool IsIslandRunning();
// Returns false when no island is running or it did not answer within 3 s.
bool Send(const std::wstring& command);
}  // namespace di::ipc
