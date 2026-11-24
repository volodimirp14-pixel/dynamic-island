#pragma once
#include <windows.h>

#include <string_view>

// Resource ids shared by DynamicIsland.exe and the Launcher (see res/*.rc).
namespace di::res {
inline constexpr int kSettingsSchema = 101;  // RCDATA: src/shared/settings_schema.json

// Text of an RCDATA resource in this exe (valid for the process lifetime).
inline std::string_view LoadText(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return {};
    HGLOBAL h = LoadResource(nullptr, r);
    const void* p = h ? LockResource(h) : nullptr;
    return p ? std::string_view(static_cast<const char*>(p), SizeofResource(nullptr, r)) : std::string_view();
}
}  // namespace di::res
