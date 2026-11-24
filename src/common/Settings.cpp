#include "common/Settings.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "common/Resources.h"
#include "common/Edition.h"
#include "common/Security.h"
#include "common/Utf.h"
#include "common/Paths.h"
#include "common/Log.h"

using json = nlohmann::json;

namespace di {
namespace {

// ── Files ─────────────────────────────────────────────────────────────────────
std::optional<std::string> ReadFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size = {};
    std::string data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {  // settings are a few KB
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) read = 0;
        data.resize(read);
    }
    CloseHandle(f);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);  // UTF-8 BOM (Notepad)
    return data;
}

// Write to <path>.tmp, then swap it in, so a crash never leaves a half-written file.
bool WriteFileAtomic(const std::wstring& path, const std::string& content, bool keepBackup) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        DI_LOG(L"[Settings] cannot write %s (error %lu)", tmp.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(f, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (keepBackup && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DI_LOG(L"[Settings] cannot replace %s (error %lu)", path.c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring TimeStamp() {
    SYSTEMTIME t = {};
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// ── Schema ────────────────────────────────────────────────────────────────────
enum class Type { Bool, Int, String, Enum };

struct Entry {
    Type type = Type::Int;
    json def;
    std::vector<std::string> options;  // enum values
    bool pro = false;                       // standalone 0.11: Pro edition only
    std::vector<std::string> proOptions;    // single enum choices that are Pro only
};

struct Value {
    int i = 0;
    std::wstring s;
};

std::map<std::string, Entry> g_schema;  // ordered: settings.json is written in schema order
int g_schemaVersion = 1;

std::shared_mutex g_lock;
std::unordered_map<std::wstring, Value> g_values;
std::string g_lastFileContent;
json g_current = json::object();  // clean values as written to settings.json
std::wstring g_lastProblem;

bool LoadSchema() {
    const std::string_view text = res::LoadText(res::kSettingsSchema);
    if (text.empty()) {
        DI_LOG(L"[Settings] settings schema resource is missing");
        return false;
    }
    try {
        const json j = json::parse(text);
        g_schemaVersion = j.value("schemaVersion", 1);
        for (const json& s : j.at("settings")) {
            Entry e;
            const std::string type = s.at("type").get<std::string>();
            e.type = type == "bool" ? Type::Bool : type == "int" ? Type::Int : type == "enum" ? Type::Enum : Type::String;
            e.def = s.at("default");
            if (e.type == Type::Enum) {
                for (const json& o : s.at("options")) e.options.push_back(o.at("value").get<std::string>());
            }
            e.pro = s.value("pro", false);
            if (s.contains("proOptions")) {
                for (const json& o : s.at("proOptions")) e.proOptions.push_back(o.get<std::string>());
            }
            g_schema[s.at("key").get<std::string>()] = std::move(e);
        }
    } catch (const std::exception& ex) {
        DI_LOG(L"[Settings] settings schema is broken: %s", Utf8ToWide(ex.what()).c_str());
        return false;
    }
    DI_LOG(L"[Settings] schema v%d, %zu settings", g_schemaVersion, g_schema.size());
    return true;
}

// Returns the value to use for `key`: the file's value if it fits the schema,
// otherwise the default. `fixed` is set when the file's value was rejected.
json Validate(const std::string& key, const Entry& e, const json* fromFile, bool* fixed) {
    if (!fromFile) {
        *fixed = true;  // missing key (e.g. a new setting after an update): add it with its default
        return e.def;
    }
    const json& v = *fromFile;
    switch (e.type) {
        case Type::Bool:
            if (v.is_boolean()) return v;
            if (v.is_number_integer()) return json(v.get<int64_t>() != 0);  // 0/1 written by hand
            break;
        case Type::Int:
            if (v.is_number_integer()) return v;
            if (v.is_number_float()) return json(static_cast<int64_t>(v.get<double>()));
            if (v.is_boolean()) return json(v.get<bool>() ? 1 : 0);
            break;
        case Type::String:
            if (v.is_string()) {
                // Standalone 0.9: no setting needs more than this; a huge value is a damaged file.
                if (v.get_ref<const std::string&>().size() <= 2048) return v;
            }
            break;
        case Type::Enum:
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                for (const std::string& o : e.options) {
                    if (_stricmp(o.c_str(), s.c_str()) == 0) return json(o);
                }
            }
            break;
    }
    *fixed = true;
    DI_LOG(L"[Settings] %s: invalid value %s, using the default", Utf8ToWide(key).c_str(),
           Utf8ToWide(v.dump()).c_str());
    return e.def;
}

// Standalone 0.11: is this (clean) value locked in the current edition?
bool LockedInEdition(const Entry& e, const json& v) {
    if (kIsPro) return false;
    if (e.pro) return true;
    if (e.type == Type::Enum && v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        for (const std::string& o : e.proOptions) {
            if (_stricmp(o.c_str(), s.c_str()) == 0) return true;
        }
    }
    return false;
}

// What the program actually uses. In the trial a Pro-only feature reads as off
// (bool -> false) and any other Pro-only value as its default. The user's own
// choice stays in settings.json, so it is back as soon as Pro is installed.
json Effective(const Entry& e, const json& v) {
    if (kIsPro) return v;
    if (e.pro) return e.type == Type::Bool ? json(false) : e.def;
    if (LockedInEdition(e, v)) return e.def;
    return v;
}

Value ToValue(const Entry& e, const json& v) {
    Value out;
    switch (e.type) {
        case Type::Bool: out.i = v.get<bool>() ? 1 : 0; break;
        case Type::Int: out.i = static_cast<int>(v.get<int64_t>()); break;
        case Type::String:
        case Type::Enum: out.s = Utf8ToWide(v.get<std::string>()); break;
    }
    return out;
}

// Reads settings.json into g_values. Repairs and rewrites the file when needed.
std::mutex g_loadMutex;  // Init (main thread) and Reload (file watcher / command) never overlap

bool LoadFile(bool* changedOut) {
    std::lock_guard loadLock(g_loadMutex);
    const std::wstring path = settings::FilePath();
    const std::optional<std::string> content = ReadFileUtf8(path);
    if (content && *content == g_lastFileContent && !g_values.empty()) {
        if (changedOut) *changedOut = false;  // our own write, or a save without changes
        return true;
    }

    json values = json::object();
    bool rewrite = false;
    std::wstring problem;
    if (!content) {
        DI_LOG(L"[Settings] %s not found; creating it with defaults", path.c_str());
        rewrite = true;
    } else {
        try {
            if (!security::JsonLooksSafe(*content, 1024 * 1024)) throw std::runtime_error("too large or too deeply nested");
            const json root = json::parse(*content);
            if (root.contains("values") && root["values"].is_object()) values = root["values"];
            else throw std::runtime_error("no \"values\" object");
        } catch (const std::exception& ex) {
            const std::wstring broken = paths::ConfigDir() + L"\\settings.broken-" + TimeStamp() + L".json";
            MoveFileExW(path.c_str(), broken.c_str(), MOVEFILE_REPLACE_EXISTING);
            problem = L"Файл настроек был повреждён и заменён настройками по умолчанию. Старый файл: " + broken;
            DI_LOG(L"[Settings] settings.json is broken (%s); saved as %s, using defaults",
                   Utf8ToWide(ex.what()).c_str(), broken.c_str());
            rewrite = true;
        }
    }

    json clean = json::object();
    std::unordered_map<std::wstring, Value> next;
    for (const auto& [key, entry] : g_schema) {
        const json* fromFile = values.contains(key) ? &values[key] : nullptr;
        bool fixed = false;
        const json v = Validate(key, entry, fromFile, &fixed);
        rewrite = rewrite || fixed;
        clean[key] = v;
        next[Utf8ToWide(key)] = ToValue(entry, Effective(entry, v));
    }
    for (const auto& [key, v] : values.items()) {
        if (!g_schema.count(key)) {
            DI_LOG(L"[Settings] unknown setting %s ignored", Utf8ToWide(key).c_str());
            rewrite = true;
        }
    }

    bool changed = false;
    {
        std::unique_lock lock(g_lock);
        for (const auto& [k, v] : next) {
            auto it = g_values.find(k);
            if (it == g_values.end() || it->second.i != v.i || it->second.s != v.s) {
                changed = true;
                break;
            }
        }
        g_values = std::move(next);
        if (!problem.empty()) g_lastProblem = problem;
    }

    g_current = clean;
    std::string text = content.value_or("");
    if (rewrite) {
        json root = {{"schemaVersion", g_schemaVersion}, {"values", clean}};
        text = root.dump(2);
        WriteFileAtomic(path, text, content.has_value());
    }
    g_lastFileContent = text;
    if (changedOut) *changedOut = changed;
    return true;
}

// Writes g_current to settings.json (caller holds g_loadMutex).
bool SaveCurrentLocked() {
    json root = {{"schemaVersion", g_schemaVersion}, {"values", g_current}};
    const std::string text = root.dump(2);
    if (!WriteFileAtomic(settings::FilePath(), text, true)) return false;
    g_lastFileContent = text;
    return true;
}

// ── state.json ────────────────────────────────────────────────────────────────
std::mutex g_stateLock;
json g_state = json::object();

std::wstring StatePath() { return paths::ConfigDir() + L"\\state.json"; }

void SaveStateLocked() {
    WriteFileAtomic(StatePath(), json({{"values", g_state}}).dump(2), false);
}
}  // namespace

// ── settings API ──────────────────────────────────────────────────────────────
namespace settings {
std::wstring FilePath() { return paths::ConfigDir() + L"\\settings.json"; }

bool Init() {
    paths::EnsureDir(paths::ConfigDir());
    if (!LoadSchema()) return false;
    return LoadFile(nullptr);
}

bool Reload() {
    bool changed = false;
    LoadFile(&changed);
    return changed;
}

int GetInt(const wchar_t* key) {
    std::shared_lock lock(g_lock);
    auto it = g_values.find(key);
    if (it == g_values.end()) {
        DI_LOG(L"[Settings] GetInt: unknown setting %s", key);
        return 0;
    }
    return it->second.i;
}

std::wstring GetString(const wchar_t* key) {
    std::shared_lock lock(g_lock);
    auto it = g_values.find(key);
    return it == g_values.end() ? std::wstring() : it->second.s;
}

std::wstring LastProblem() {
    std::shared_lock lock(g_lock);
    return g_lastProblem;
}

std::string SchemaText() {
    return std::string(res::LoadText(res::kSettingsSchema));
}

std::string ValuesJson() {
    std::lock_guard loadLock(g_loadMutex);
    if (kIsPro) return g_current.dump();
    json shown = json::object();  // the trial shows what is really in effect
    for (const auto& [key, entry] : g_schema) {
        if (g_current.contains(key)) shown[key] = Effective(entry, g_current[key]);
    }
    return shown.dump();
}

bool SetValue(const std::string& key, const std::string& valueJson, std::string* error) {
    std::lock_guard loadLock(g_loadMutex);
    auto it = g_schema.find(key);
    if (it == g_schema.end()) {
        if (error) *error = "unknown setting";
        return false;
    }
    json v;
    try {
        if (!security::JsonLooksSafe(valueJson, 4096, 4)) throw std::runtime_error("unsafe");
        v = json::parse(valueJson);
    } catch (...) {
        if (error) *error = "not JSON";
        return false;
    }
    bool fixed = false;
    const json clean = Validate(key, it->second, &v, &fixed);
    if (fixed) {
        if (error) *error = "invalid value";
        return false;
    }
    if (LockedInEdition(it->second, clean)) {
        if (error) *error = "pro";
        return false;
    }
    g_current[key] = clean;
    {
        std::unique_lock lock(g_lock);
        g_values[Utf8ToWide(key)] = ToValue(it->second, Effective(it->second, clean));
    }
    if (!SaveCurrentLocked()) {
        if (error) *error = "cannot write settings.json";
        return false;
    }
    DI_LOG(L"[Settings] %s = %s", Utf8ToWide(key).c_str(), Utf8ToWide(clean.dump()).c_str());
    return true;
}

bool ResetToDefaults() {
    std::lock_guard loadLock(g_loadMutex);
    json clean = json::object();
    std::unordered_map<std::wstring, Value> next;
    for (const auto& [key, entry] : g_schema) {
        clean[key] = entry.def;
        next[Utf8ToWide(key)] = ToValue(entry, Effective(entry, entry.def));
    }
    g_current = clean;
    {
        std::unique_lock lock(g_lock);
        g_values = std::move(next);
    }
    DI_LOG(L"[Settings] reset to defaults");
    return SaveCurrentLocked();
}
}  // namespace settings

// ── state API ─────────────────────────────────────────────────────────────────
namespace state {
void Init() {
    std::lock_guard lock(g_stateLock);
    const std::optional<std::string> content = ReadFileUtf8(StatePath());
    if (!content) return;
    try {
        if (!security::JsonLooksSafe(*content, 256 * 1024)) throw std::runtime_error("unsafe");
        const json root = json::parse(*content);
        if (root.contains("values") && root["values"].is_object()) g_state = root["values"];
    } catch (...) {
        DI_LOG(L"[State] state.json is broken; starting with an empty state");
        g_state = json::object();
    }
}

int GetInt(const wchar_t* key, int defaultValue) {
    std::lock_guard lock(g_stateLock);
    const std::string k = WideToUtf8(key);
    if (g_state.contains(k) && g_state[k].is_number_integer()) return g_state[k].get<int>();
    return defaultValue;
}

void SetInt(const wchar_t* key, int value) {
    std::lock_guard lock(g_stateLock);
    const std::string k = WideToUtf8(key);
    if (g_state.contains(k) && g_state[k].is_number_integer() && g_state[k].get<int>() == value) return;
    g_state[k] = value;
    SaveStateLocked();
}

std::wstring GetString(const wchar_t* key) {
    std::lock_guard lock(g_stateLock);
    const std::string k = WideToUtf8(key);
    if (g_state.contains(k) && g_state[k].is_string()) return Utf8ToWide(g_state[k].get<std::string>());
    return {};
}

void SetString(const wchar_t* key, const std::wstring& value) {
    std::lock_guard lock(g_stateLock);
    const std::string k = WideToUtf8(key);
    const std::string v = WideToUtf8(value);
    if (g_state.contains(k) && g_state[k].is_string() && g_state[k].get<std::string>() == v) return;
    g_state[k] = v;
    SaveStateLocked();
}
}  // namespace state
}  // namespace di
