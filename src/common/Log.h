#pragma once
#include <string>

// Replacement for Windhawk's Wh_Log: a UTF-8 log file with size-based rotation
// (island.log, island.1.log, island.2.log; 5 MB each) plus OutputDebugString.
// Thread-safe. Printf-style format with wide strings, like Wh_Log.
namespace di::log {
void Init(const std::wstring& dir, const wchar_t* fileBaseName);
void Write(const wchar_t* format, ...);
std::wstring FilePath();
}  // namespace di::log

#define DI_LOG(...) ::di::log::Write(__VA_ARGS__)
