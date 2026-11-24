#pragma once
#include <windows.h>

#include <vector>

// Standalone 0.13: "screen time" -- how long the computer has been switched on
// (awake) today, from local midnight until now. Sleep, hibernation and shut-down
// periods do not count. No per-program tracking: only on / off.
//
// Source: the Windows System event log (readable without admin rights), which
// records every start, shut-down, sleep and wake. If the log cannot be read, the
// fallback is "awake since the last start / wake, but not before midnight".
namespace di::screentime {

struct Segment {
    double from = 0.0;  // fractions of the local day, 0 = 00:00, 1 = 24:00
    double to = 0.0;
};

struct Today {
    bool valid = false;
    bool fromEventLog = false;          // false = fallback estimate
    long long seconds = 0;              // switched-on time today
    int firstStartMinute = -1;          // minutes after midnight of the first switch-on today
    double nowFraction = 0.0;           // where "now" is on the day bar
    std::vector<Segment> segments;      // on-periods for the day bar
};

// Reads the event log; takes a few milliseconds. Call from a worker thread.
Today Compute();

}  // namespace di::screentime
