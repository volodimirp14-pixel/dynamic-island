#pragma once
#include <string>
#include <string_view>

// Standalone 0.9: process hardening and input limits shared by both programs.
namespace di::security {

// Call first thing in wWinMain.
//  - DLLs are loaded only from System32 and the program folder (never from
//    the current directory or PATH), which defeats DLL planting.
//  - Images from network shares or low-integrity files cannot be loaded.
//  - extensionPoints: also refuse legacy injection points (AppInit DLLs,
//    global window-hook DLLs, Winsock LSPs) -- for the island, which needs none.
//  - Heap corruption terminates the process instead of being exploitable.
void HardenProcess(bool disableExtensionPoints);

// JSON from disk or from the Launcher page is parsed only if it is small and
// not absurdly nested (a deeply nested file would overflow the parser's stack).
bool JsonLooksSafe(std::string_view text, size_t maxBytes, int maxDepth = 64);

// App user model ids are used as registry key names and in "shell:AppsFolder\..."
// paths; only the characters real ids use are accepted.
bool IsSafeAumid(std::wstring_view aumid);

// Cuts text from outside (messages, web data) to a sane length.
std::wstring Clip(std::wstring text, size_t maxChars);

}  // namespace di::security
