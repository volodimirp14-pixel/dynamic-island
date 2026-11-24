#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cwchar>
#include <mutex>

namespace di::log {
namespace {
std::mutex g_mutex;
std::wstring g_path;
constexpr LONGLONG kMaxBytes = 5LL * 1024 * 1024;
constexpr int kKeep = 2;  // island.1.log, island.2.log

std::wstring Rotated(int i) {
    const size_t dot = g_path.rfind(L'.');
    return g_path.substr(0, dot) + L"." + std::to_wstring(i) + g_path.substr(dot);
}

void RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &data)) return;
    const LONGLONG size = (static_cast<LONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size < kMaxBytes) return;
    DeleteFileW(Rotated(kKeep).c_str());
    for (int i = kKeep - 1; i >= 1; --i) MoveFileExW(Rotated(i).c_str(), Rotated(i + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(g_path.c_str(), Rotated(1).c_str(), MOVEFILE_REPLACE_EXISTING);
}
}  // namespace

void Init(const std::wstring& dir, const wchar_t* fileBaseName) {
    std::lock_guard lock(g_mutex);
    g_path = dir + L"\\" + fileBaseName + L".log";
}

std::wstring FilePath() {
    std::lock_guard lock(g_mutex);
    return g_path;
}

void Write(const wchar_t* format, ...) {
    wchar_t message[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME t = {};
    GetLocalTime(&t);
    wchar_t line[2200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), message);
    OutputDebugStringW(line);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string utf8(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);

    std::lock_guard lock(g_mutex);
    if (g_path.empty()) return;
    RotateIfNeeded();
    HANDLE f = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}
}  // namespace di::log
