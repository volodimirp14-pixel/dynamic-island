#include "island/screentime/ScreenTime.h"

#include <winevt.h>

#include <algorithm>
#include <string>

#include "common/Log.h"

namespace di::screentime {
namespace {

constexpr ULONGLONG kTicksPerSecond = 10000000ULL;

ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Local midnight of today, as UTC FILETIME ticks.
ULONGLONG LocalMidnightUtc() {
    SYSTEMTIME local = {};
    GetLocalTime(&local);
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    SYSTEMTIME utc = {};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return 0;
    FILETIME ft = {};
    SystemTimeToFileTime(&utc, &ft);
    return FileTimeToU64(ft);
}

ULONGLONG NowUtc() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToU64(ft);
}

enum class Kind { None, On, Off };

// Which power events mean "switched on / woke up" and "shut down / went to sleep".
Kind Classify(const std::wstring& provider, unsigned id) {
    if (provider == L"Microsoft-Windows-Kernel-General") {
        if (id == 12) return Kind::On;   // operating system started
        if (id == 13) return Kind::Off;  // operating system is shutting down
    } else if (provider == L"Microsoft-Windows-Kernel-Power") {
        if (id == 42) return Kind::Off;   // entering sleep / hibernate
        if (id == 506) return Kind::Off;  // entering Modern Standby
        if (id == 107) return Kind::On;   // resumed from sleep
        if (id == 507) return Kind::On;   // left Modern Standby
    } else if (provider == L"Microsoft-Windows-Power-Troubleshooter") {
        if (id == 1) return Kind::On;  // returned from a low-power state
    }
    return Kind::None;
}

struct Event {
    ULONGLONG time;
    Kind kind;
};

bool ReadEvents(std::vector<Event>* out) {
    // The last 8 days are enough to know whether the computer was on at midnight.
    const wchar_t* query =
        L"*[System[("
        L"(Provider[@Name='Microsoft-Windows-Kernel-General'] and (EventID=12 or EventID=13)) or "
        L"(Provider[@Name='Microsoft-Windows-Kernel-Power'] and (EventID=42 or EventID=107 or EventID=506 or EventID=507)) or "
        L"(Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1)"
        L") and TimeCreated[timediff(@SystemTime) <= 691200000]]]";
    EVT_HANDLE results = EvtQuery(nullptr, L"System", query, EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!results) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            DI_LOG(L"[ScreenTime] event log query failed (%lu); using the estimate", GetLastError());
        }
        return false;
    }
    EVT_HANDLE context = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
    if (!context) {
        EvtClose(results);
        return false;
    }

    std::vector<BYTE> buffer(4096);
    EVT_HANDLE batch[64];
    DWORD returned = 0;
    while (EvtNext(results, ARRAYSIZE(batch), batch, 1000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0, count = 0;
            if (!EvtRender(context, batch[i], EvtRenderEventValues, static_cast<DWORD>(buffer.size()), buffer.data(),
                           &used, &count)) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used <= 1024 * 1024) {
                    buffer.resize(used);
                    EvtRender(context, batch[i], EvtRenderEventValues, static_cast<DWORD>(buffer.size()),
                              buffer.data(), &used, &count);
                } else {
                    count = 0;
                }
            }
            if (count > EvtSystemTimeCreated) {
                const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
                const EVT_VARIANT& provider = values[EvtSystemProviderName];
                const EVT_VARIANT& id = values[EvtSystemEventID];
                const EVT_VARIANT& time = values[EvtSystemTimeCreated];
                if (provider.Type == EvtVarTypeString && provider.StringVal && time.Type == EvtVarTypeFileTime) {
                    const Kind kind = Classify(provider.StringVal, id.UInt16Val);
                    if (kind != Kind::None) out->push_back({time.FileTimeVal, kind});
                }
            }
            EvtClose(batch[i]);
        }
    }
    EvtClose(context);
    EvtClose(results);
    std::sort(out->begin(), out->end(), [](const Event& a, const Event& b) { return a.time < b.time; });
    return true;
}

}  // namespace

Today Compute() {
    Today t;
    const ULONGLONG midnight = LocalMidnightUtc();
    const ULONGLONG now = NowUtc();
    if (midnight == 0 || now <= midnight) return t;
    const double day = 24.0 * 3600.0 * kTicksPerSecond;
    t.nowFraction = std::min(1.0, (now - midnight) / day);

    std::vector<std::pair<ULONGLONG, ULONGLONG>> on;  // [from, to) in UTC ticks, clipped to today
    auto add = [&](ULONGLONG from, ULONGLONG to) {
        from = std::max(from, midnight);
        to = std::min(to, now);
        if (to > from) on.emplace_back(from, to);
    };

    std::vector<Event> events;
    if (ReadEvents(&events)) {
        t.fromEventLog = true;
        // If the window starts with an "off" event, the computer was on before it.
        bool awake = !events.empty() && events.front().kind == Kind::Off;
        ULONGLONG since = 0;
        ULONGLONG lastOff = 0;
        for (const Event& e : events) {
            if (e.kind == Kind::On) {
                if (!awake) {
                    awake = true;
                    since = e.time;
                }
                // "On" while already on (e.g. after a crash without a shut-down event,
                // or two wake events): keep the period that is already running.
            } else {
                if (awake) add(since, e.time);
                awake = false;
                lastOff = e.time;
            }
        }
        if (awake) {
            add(since, now);
        } else {
            // The island is running, so the computer is on now even though the wake
            // event is missing: count the awake time since the last start / wake,
            // but not before the last "off" event.
            ULONGLONG unbiased = 0;
            QueryUnbiasedInterruptTime(&unbiased);
            add(std::max(lastOff, now > unbiased ? now - unbiased : 0), now);
        }
    }

    if (on.empty() || !t.fromEventLog) {
        // Estimate: awake time since the last start / wake (sleep is not counted by
        // the unbiased clock), but not before midnight.
        on.clear();
        ULONGLONG unbiased = 0;  // 100 ns units of awake time since start
        QueryUnbiasedInterruptTime(&unbiased);
        add(now > unbiased ? now - unbiased : 0, now);
    }

    for (const auto& [from, to] : on) {
        t.seconds += static_cast<long long>((to - from) / kTicksPerSecond);
        t.segments.push_back({(from - midnight) / day, (to - midnight) / day});
    }
    if (!on.empty()) t.firstStartMinute = static_cast<int>((on.front().first - midnight) / (60 * kTicksPerSecond));
    t.valid = true;
    return t;
}

}  // namespace di::screentime
