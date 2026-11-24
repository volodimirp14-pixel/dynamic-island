#pragma once
#include <string>

// Starting and stopping DynamicIsland.exe from the Launcher.
namespace di::launcher {
std::wstring IslandExePath();  // DynamicIsland.exe next to the Launcher
bool IsIslandRunning();
// Starts the island. Returns false (with a Russian message) if it could not be started.
bool StartIsland(std::wstring* error);
// Asks the island to quit and waits up to `timeoutMs` for it to finish.
// Returns false if it is still running afterwards.
bool StopIsland(unsigned timeoutMs);
// Last resort when it does not answer: ends the process.
bool KillIsland();
}  // namespace di::launcher
