#pragma once
#include <windows.h>

#include <functional>
#include <string>

// The island's command endpoint: a hidden message-only window that receives
// commands (see shared/Ipc.h) from the Launcher and from a second DynamicIsland.exe.
namespace di::control {
struct Handlers {
    std::function<void()> reload;
    std::function<void()> quit;
    std::function<void()> show;
    std::function<void(int)> progress;
    std::function<void()> endSession;  // Windows is shutting down / logging off
    std::function<unsigned()> accent;
    std::function<void(const std::wstring&)> timer;  // "timer <25|50|break|pause|stop>"  // "accent": live accent 0x01RRGGBB (0 = none)
};

// Creates the window on the calling thread (which must run a message loop).
HWND Create(const Handlers& handlers);
}  // namespace di::control
