#include "common/Security.h"

#include <windows.h>

#include "common/Log.h"

namespace di::security {

void HardenProcess(bool disableExtensionPoints) {
    // DLL search: System32 + the exe's folder only.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetDllDirectoryW(L"");  // removes the current directory from the search path
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);

    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY image = {};
    image.NoRemoteImages = 1;
    image.NoLowMandatoryLabelImages = 1;
    image.PreferSystem32Images = 1;
    const BOOL imageOk = SetProcessMitigationPolicy(ProcessImageLoadPolicy, &image, sizeof(image));

    BOOL extOk = TRUE;
    if (disableExtensionPoints) {
        PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext = {};
        ext.DisableExtensionPoints = 1;
        extOk = SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));
    }
    DI_LOG(L"[Security] DLL search locked; image-load policy: %s; extension points blocked: %s",
           imageOk ? L"on" : L"unavailable", disableExtensionPoints ? (extOk ? L"yes" : L"unavailable") : L"n/a");
}

bool JsonLooksSafe(std::string_view text, size_t maxBytes, int maxDepth) {
    if (text.size() > maxBytes) return false;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') {
            if (++depth > maxDepth) return false;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

bool IsSafeAumid(std::wstring_view aumid) {
    if (aumid.empty() || aumid.size() > 256) return false;
    for (const wchar_t c : aumid) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                        c == L'.' || c == L'_' || c == L'-' || c == L'!';
        if (!ok) return false;
    }
    return aumid.find(L"..") == std::wstring_view::npos;
}

std::wstring Clip(std::wstring text, size_t maxChars) {
    if (text.size() > maxChars) {
        text.resize(maxChars);
        text += L"…";
    }
    return text;
}

}  // namespace di::security
