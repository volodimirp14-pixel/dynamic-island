#pragma once
#include <string>

// Replaces Windhawk's settings (Wh_GetIntSetting / Wh_GetStringSetting) and its
// per-mod value storage (Wh_Get/SetIntValue, Wh_Get/SetStringValue).
//
// settings.json -- what the user chooses (edited by the Launcher). Every key
//                  is described once in settings_schema.json (embedded in the
//                  exe): type, default, allowed options.
// state.json    -- small runtime state the island remembers between runs
//                  (Do Not Disturb, hidden/pinned, banner backup, ...).
// Both live in %APPDATA%\DynamicIsland and are written atomically.
namespace di::settings {
// Loads the schema and settings.json. Missing file -> written with defaults;
// broken file -> kept as settings.broken-<time>.json and replaced by defaults;
// invalid values -> default (logged) and the file is rewritten.
bool Init();
// Re-reads settings.json. Returns true if the effective values changed.
bool Reload();
// Like Wh_GetIntSetting: bool -> 0/1, int -> value, other types -> 0.
int GetInt(const wchar_t* key);
// Like Wh_GetStringSetting: string/enum value, "" for anything else.
std::wstring GetString(const wchar_t* key);
std::wstring FilePath();
// Set by Init/Reload when the file had to be repaired, for the Launcher to show.
std::wstring LastProblem();

// ── For the Launcher ──
// The embedded schema (UTF-8 JSON text).
std::string SchemaText();
// Current effective values as a JSON object {"Group.Key": value, ...}.
std::string ValuesJson();
// Validates `valueJson` against the schema and saves settings.json.
// On rejection returns false and a short reason in `error`.
bool SetValue(const std::string& key, const std::string& valueJson, std::string* error);
// Writes every setting's default (the old file is kept as settings.json.bak).
bool ResetToDefaults();
}  // namespace di::settings

namespace di::state {
void Init();
int GetInt(const wchar_t* key, int defaultValue);
void SetInt(const wchar_t* key, int value);
std::wstring GetString(const wchar_t* key);
void SetString(const wchar_t* key, const std::wstring& value);
}  // namespace di::state
