#pragma once
// Launcher / command line -> island commands. The island owns a hidden
// message-only window of this class; a client finds it and sends WM_COPYDATA
// with dwData == kCommandMagic and a UTF-16 command string (no terminator needed).
//
// Commands:  reload | quit | show | progress <0..100 | -1>
//            timer <25 | 50 | break | pause | stop>
//            accent  -> returns the live accent as 0x01RRGGBB (TRUE when unknown)
namespace di::ipc {
inline constexpr const wchar_t* kControlWindowClass = L"DynamicIsland.Control";
inline constexpr unsigned long kCommandMagic = 0x44490001;  // 'DI' 0001
}  // namespace di::ipc
