// Dynamic Island -- island core (standalone; ported from the Windhawk mod
// "dynamic-island-for-windows-custom" 1.4.0-custom.20). Settings come from
// settings.json (see settings/Settings.h), logs from log/Log.h; the entry
// point and lifecycle live in main.cpp / the di::island API at the bottom.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <unknwn.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj.h>
#include <sddl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <devpropdef.h>
#include <dbt.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shcore.h>
#include <windowsx.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <objbase.h>
#include <wrl/client.h>
#include <uiautomation.h>
#include <winhttp.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <set>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#if __has_include(<winrt/Windows.UI.Notifications.Management.h>) && \
    __has_include(<winrt/Windows.UI.Notifications.h>)
#define DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER 1
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#else
#define DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER 0
#endif

#if __has_include(<winrt/Windows.Devices.Enumeration.h>) && \
    __has_include(<winrt/Windows.Devices.Bluetooth.h>) && \
    __has_include(<winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>)
#define DYNAMIC_ISLAND_HAS_BLUETOOTH_WATCHER 1
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#else
#define DYNAMIC_ISLAND_HAS_BLUETOOTH_WATCHER 0
#endif

#include "Island.h"
#include "common/Version.h"
#include "common/Paths.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "common/Security.h"

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {

// Progress ring percent (0..100, -1 = none), set by `DynamicIsland.exe --progress N`
// or the Launcher. Replaces the Windhawk value "ProgressPercent" other tools wrote.
std::atomic<int> g_progressPercent{-1};
// Island accent as 0x01RRGGBB (0 = not drawn yet); read by the Launcher via the "accent" command.
std::atomic<uint32_t> g_liveAccentRgb{0};
// Thread that called di::island::Start (runs the message loop in main.cpp).
DWORD g_mainThreadId = 0;

constexpr wchar_t kWindowClass[] = L"DynamicIsland.Island";
constexpr UINT WM_APP_LAYOUT_CHANGED = WM_APP + 0x442;
constexpr UINT WM_APP_NEW_EVENT = WM_APP + 0x443;
constexpr UINT WM_APP_MOUSE_WAKE = WM_APP + 0x446;
// RegisterHotKey cannot associate a hot key with a window created by another
// thread, and the same applies to tearing one down. LoadSettings runs on
// Windhawk's settings thread while the overlay window belongs to the render
// thread, so hotkey and backdrop changes are posted across and applied there.
constexpr UINT WM_APP_APPLY_HOTKEY = WM_APP + 0x447;
constexpr UINT WM_APP_APPLY_BACKDROP = WM_APP + 0x448;
constexpr int ID_HIDE_SHOW_HOTKEY = 1;
constexpr int ID_QUIET_HOTKEY = 3;  // custom fork: island Do Not Disturb
constexpr float kRenderPadX = 28.0f;
constexpr float kRenderPadY = 22.0f;

// Layout for the expanded media dashboard. DrawMedia paints every element at an
// offset from the content rect it is handed, and OverlayWndProc hit-tests in
// that same content space (see MediaContentFromClient), so both sides read the
// constants below and cannot drift apart.
namespace MediaLayout {
    constexpr float kExpandedWidth = 380.0f;
    constexpr float kExpandedHeight = 184.0f;


    constexpr float kScrubberY = 114.0f;
    constexpr float kScrubMargin = 24.0f;
    constexpr float kScrubBarLeftInset = 48.0f;
    constexpr float kScrubBarRightInset = 48.0f;

    constexpr float kScrubHitHalfHeight = 14.0f;
    constexpr float kScrubHitPadX = 6.0f;

    // Distance from the content rect's left/right edge to each end of the
    // scrubber bar. Used by the content-space hit test so the bar's clickable
    // span follows the actual pill width instead of assuming 380px.
    constexpr float kScrubInsetLeft = kScrubMargin + kScrubBarLeftInset;
    constexpr float kScrubInsetRight = kScrubMargin + kScrubBarRightInset;

    // Transport controls exactly as DrawMedia paints them: y is measured down
    // from the content rect's top edge, x as an offset from the content rect's
    // horizontal center. The hit test derives its boxes from these same values.
    constexpr float kControlsY = 148.0f;
    constexpr float kControlSpacing = 64.0f;
    constexpr float kNavButtonRadius = 16.0f;
    constexpr float kPlayButtonRadius = 22.0f;
    // Extra slack around each button so the round targets are comfortable to
    // hit. Kept small enough that prev/next never overlap play/pause:
    // prev spans -86..-42, play spans -28..+28.
    constexpr float kControlHitPad = 6.0f;

    // Expanded-layout album art, as drawn by DrawMedia.
    constexpr float kArtInsetX = 24.0f;
    constexpr float kArtInsetY = 20.0f;
    constexpr float kArtSize = 64.0f;

    // The expanded dashboard is faded in by DrawMedia via
    // expandedAlpha = clamp((contentHeight - 60) / 60), so anything at or below
    // this height is still the collapsed pill and must not be hit-tested.
    constexpr float kExpandedMinHeight = 60.0f;

    // ── Source dock ──────────────────────────────────────────────────────────
    // Widest layout is 3 slots = 86px, so it ends at x=106, clear of the prev
    // button (~110). Draw code and hit test both read these.
    constexpr float kDockLeft = 20.0f;
    constexpr float kDockHeight = 30.0f;
    constexpr float kDockSlot = 26.0f;
    constexpr float kDockGap = 2.0f;
    constexpr float kDockPad = 2.0f;
    constexpr float kDockIcon = 16.0f;
    constexpr float kDockHitMin = 28.0f;
    constexpr int kDockMaxSlots = 3;            // 3 sources, or 2 sources + "+N"
    constexpr double kDockSlideSec = 0.30;

    constexpr float DockWidth(int slots) {
        return slots <= 0 ? 0.0f
                          : kDockPad * 2.0f + slots * kDockSlot + (slots - 1) * kDockGap;
    }
    constexpr float DockSlotLeft(int slot) {
        return kDockLeft + kDockPad + slot * (kDockSlot + kDockGap);
    }
}


// Layout for the collapsed idle strip (the clock, and optionally a weather
// reading beside it).
//
// Fixes windhawk-mods#5086. Note the tracker: plain "#33"-style references in
// this file are issues on devcode90/Dynamic-Island-for-Windows, whereas
// "windhawk-mods#NNNN" is the ramensoftware/windhawk-mods tracker that the
// published mod is submitted through. Both use bare #N in their own context, so
// the upstream ones are always qualified here.
//
// This width used to be a bare constant in ActivityForKind -- 96px, or 170px
// with weather -- which was wrong in both directions. Measured at the idle
// format's own face and size, "9:41" is 22.7px of ink in a 96px pill, so 24%
// occupancy and the rest dead air. The weather variant then split the pill
// 50/50 at its centre no matter how wide the two strings actually were.
//
// The fixed width also truncated long clocks, though only at larger type: the
// text box was a flat 84px while the idle font is 13.0f * textScale, so
// "10:41:32 PM" fits at textScale 1.0 (67.7px) but is clipped at 1.4 (94.8px)
// and 1.6 (108.4px).
//
// The strip is now measured and sized to its real content. As with
// GameOverlayLayout, the sizer (the render loop) and the painter
// (DrawIdleDashboard) both read these constants so they cannot drift apart.
namespace IdleStripLayout {
    constexpr float kPadX = 14.0f;         // inner padding at each end
    constexpr float kSlotGap = 9.0f;       // clock <-> divider <-> weather
    constexpr float kDividerWidth = 1.0f;
    constexpr float kDividerInsetY = 9.0f;

    // Room for the mic/camera dot. DrawPrivacyDots anchors it at rect.right - 20
    // with a 4px radius, so 18px clears it. Standalone 0.6: reserved on BOTH
    // sides (the collapsed idle pill grows by 2 x 18px), so the clock stays
    // centred on the screen and the dot sits in its own lane on the right.
    constexpr float kPrivacyReserve = 18.0f;

    constexpr float kHeight = 36.0f;

    // The stadium cap is kHeight/2 at each end, so anything below ~2x the height
    // stops reading as a pill. The ceiling keeps a long localized string from
    // turning the island into a bar.
    constexpr float kMinWidth = 72.0f;
    constexpr float kMaxWidth = 280.0f;

    // Measured widths are rounded up to this so sub-pixel text metrics can't
    // resize the layered window every frame.
    constexpr float kWidthQuantum = 2.0f;
}

// Layout for the collapsed media pill when the optional clock is shown. A clock
// section is added on the LEFT of the normal pill: [clock | divider] then the
// usual cover ... spectrum. MeasureMediaPill (sizer) and DrawMedia (painter)
// both read these so they cannot drift apart. With the option off the section
// is 0 wide and the pill is exactly the old 150px.
namespace MediaPillLayout {
    constexpr float kBaseWidth = 150.0f;    // same as ActivityForKind's Media width
    constexpr float kClockPadLeft = 14.0f;  // left edge -> clock text
    constexpr float kSlotGap = 9.0f;        // clock text -> divider
    constexpr float kDividerWidth = 1.0f;
    constexpr float kDividerInsetY = 10.0f;
    constexpr float kWidthQuantum = 2.0f;
}

// Layout for the game overlay strip. The size the island animates to is decided
// in the render loop, while the contents are painted by DrawGameOverlay. Those
// two kept their own copies of the card width, padding and height, so widening a
// card in one place left the other sizing the island for the old value -- the
// strip would reserve room for four cards and then drop the last one on the
// "ran out of room" check. Both read this now.
//
// Deliberately free of g_settings so it can sit up here with the other layout
// namespaces; callers pass what they know.
namespace GameOverlayLayout {
    struct Metrics {
        float padX;
        float padY;
        float cardW;
        float gap;
        float fpsW;
        float fpsGap;
        float radius;
        float height;
    };

    // Cards are wider than the original 52/62 so a label and a three-digit value
    // sit in separate bands without touching, and the corner radius matches the
    // 9-10px used by every other card instead of the old 16, which on a 44px-tall
    // card was very nearly a stadium.
    constexpr Metrics For(bool compact) {
        return compact ? Metrics{10.0f,  9.0f, 62.0f, 6.0f, 74.0f, 7.0f,  9.0f, 56.0f}
                       : Metrics{12.0f, 11.0f, 74.0f, 7.0f, 88.0f, 8.0f, 10.0f, 68.0f};
    }

    constexpr float kMinWidth = 140.0f;

    constexpr float Width(bool compact, bool showFps, int metricCount) {
        const Metrics m = For(compact);
        float w = m.padX * 2.0f;
        if (showFps) {
            w += m.fpsW + m.fpsGap;
        }
        if (metricCount > 0) {
            w += static_cast<float>(metricCount) * m.cardW +
                 static_cast<float>(metricCount - 1) * m.gap;
        }
        return w < kMinWidth ? kMinWidth : w;
    }
}

// ── Media dashboard hit-test geometry ────────────────────────────────────────
// DrawMedia publishes the exact content-space rect it painted into, together
// with the scale that maps content px to client px. OverlayWndProc converts
// mouse positions through this instead of assuming the pill is centered in the
// client area, which it is not when the notch / border-merged offset, the hover
// scale, or a split (two-pill) layout is in play. Keeping one published source
// of truth is what stops the drawn buttons and their hit boxes from drifting
// apart -- the drift that made prev/next clicks fall through to "open the app".
// The fields are guarded by a seqlock rather than read independently: the render
// thread rewrites them every frame, and during the expand animation the content
// height sweeps ~36 -> 184px, so a reader that caught half of one frame and half
// of the next could misplace a hit box for a click. g_mediaHitSeq is odd while a
// write is in progress; readers retry until they see the same even value twice.
// The payload stays in relaxed atomics so there is no formal data race; the
// seqlock counter is what provides consistency *across* the fields.
std::atomic<unsigned> g_mediaHitSeq{0};
std::atomic<float> g_mediaHitLeft{0.0f};
std::atomic<float> g_mediaHitTop{0.0f};
std::atomic<float> g_mediaHitRight{0.0f};
std::atomic<float> g_mediaHitBottom{0.0f};
std::atomic<float> g_mediaHitScale{1.0f};
std::atomic<unsigned long long> g_mediaHitStamp{0};

struct MediaContentPoint {
    bool valid = false;
    float x = 0.0f;        // relative to the content rect's left edge
    float y = 0.0f;        // relative to the content rect's top edge
    float width = 0.0f;    // content rect width
    float height = 0.0f;   // content rect height
};

// The conversion and hit-test helpers live further down, just after Clamp().

enum class IslandKind {
    Idle,
    Media,
    Progress,
    Notification,
    Volume,
    Brightness,
    BatteryLow,
    Device,
    Bluetooth,
    Timer,
    Split,
};

enum class BluetoothDeviceCategory {
    Headphones,
    Speaker,
    Mouse,
    Keyboard,
    Phone,
    Gamepad,  // standalone 0.7
    Generic,
};

enum class Position {
    TopCenter,
    TopLeft,
    TopRight,
    BottomCenter,
    BottomLeft,
    BottomRight,
};

// Anything anchored to the bottom edge shares the same vertical placement and
// cannot use the top-edge macOS notch shape.
constexpr bool IsBottomPosition(Position position) {
    return position == Position::BottomCenter ||
           position == Position::BottomLeft ||
           position == Position::BottomRight;
}

enum class AccentMode {
    Auto,
    System,
    Custom,
};

enum class AnimationStyle {
    Smooth,
    Default,
    Bouncy,
    Snappy,
};

enum class ProgressStyle {
    Slim,
    Wavy,
    Squiggle,
    Bar,
};

enum class SpectrumStyle {
    Bars,
    Orb,
    Plasma,
    Led,
};

constexpr int kSpectrumBands = 24;  // log-spaced bands from SpectrumAnalyzer, stored in SharedState::bands

enum class CalendarAccentMode {
    Red,
    Accent,  // persisted as "red", from when it was a hardcoded red
    System,
};

// Curated palettes. These replace the original four (OLED Black, Fluent,
// Midnight Blue, Deep Purple), which were built for a material that painted a
// glass highlight along every edge. With the edge lighting gone, a palette has
// to carry the whole look on flat fills, so each of these is tuned for
// separation by value alone -- and one is light, which the old set had none of.
//
// Order is load-bearing: it is persisted as an integer index, so append new
// entries at the end rather than inserting. Custom must stay last.
enum class ThemePreset {
    Obsidian,
    Graphite,
    Slate,
    Nord,
    Evergreen,
    Espresso,
    Plum,
    Porcelain,
    Custom,
};

// One table drives the settings dropdown, the palette names and the
// resolved colors. It used to be three separate lists, which is how the menu
// ended up carrying special cases for palette indexes that no longer matched.
//
// Secondary text is held at roughly 4.5:1 against its own background in every
// row, because no edge highlight is left to help muted labels separate from the
// surface behind them.
struct ThemePalette {
    const wchar_t* id;      // value stored in Themes.ThemePreset
    const wchar_t* label;   // human-readable palette name
    const wchar_t* bg;
    const wchar_t* fg;
    const wchar_t* sec;
    const wchar_t* border;
};

static constexpr ThemePalette kThemePalettes[] = {
    {L"obsidian",  L"Obsidian (Default)", L"#08080A", L"#FFFFFF", L"#9B9BA5", L"#1E1E22"},
    {L"graphite",  L"Graphite",           L"#1C1C1E", L"#FFFFFF", L"#A8A8AE", L"#323236"},
    {L"slate",     L"Slate",              L"#111721", L"#E9EEF6", L"#92A2B8", L"#232C3A"},
    {L"nord",      L"Nord",               L"#2E3440", L"#ECEFF4", L"#A7B0C0", L"#3B4252"},
    {L"evergreen", L"Evergreen",          L"#0C1512", L"#E4F1EA", L"#8CAE9D", L"#1A2A23"},
    {L"espresso",  L"Espresso",           L"#1A1512", L"#F6EEE7", L"#B7A395", L"#2E2420"},
    {L"plum",      L"Plum",               L"#16111C", L"#F1EAF6", L"#AC9BB9", L"#281F33"},
    {L"porcelain", L"Porcelain (Light)",  L"#F5F6F8", L"#14161A", L"#5B6270", L"#D8DBE1"},
};

// Custom sits one past the last real palette. Derived, so adding a palette
// cannot leave a stale literal behind.
static constexpr int kCustomThemeIndex = static_cast<int>(ARRAYSIZE(kThemePalettes));

// Maps a preset id to its palette index, accepting the retired ids too: a
// user's stored setting keeps its old string after the option disappears from
// the dropdown, so each legacy id is pointed at the new palette closest to it.
inline int ThemeIndexFromId(std::wstring_view id) {
    auto iequals = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (towlower(a[i]) != towlower(b[i])) return false;
        }
        return true;
    };

    for (int i = 0; i < kCustomThemeIndex; ++i) {
        if (iequals(id, kThemePalettes[i].id)) return i;
    }
    if (iequals(id, L"custom")) return kCustomThemeIndex;

    if (iequals(id, L"oled-black")) return 0;     // -> Obsidian
    if (iequals(id, L"fluent") ||
        iequals(id, L"mica") ||
        iequals(id, L"dark-gray")) return 1;      // -> Graphite
    if (iequals(id, L"midnight-blue")) return 2;  // -> Slate
    if (iequals(id, L"deep-purple")) return 6;    // -> Plum
    return -1;
}

// Real Windows blur / acrylic painted behind the island (#59).
enum class BackdropMaterial {
    None,
    Blur,
    Acrylic,
    LiquidGlass,  // custom fork (custom.17): iOS-style Liquid Glass
};

enum class ContourBorderMode {
    Default,
    Auto,
    Borderless,
};

// ── Stocks (custom fork, custom.18) ─────────────────────────────────────────
// The stocks the settings offer (50 popular stocks + SpaceX and the S&P 500
// index, added on request in custom.19). `symbol` is the Yahoo Finance ticker (sent
// to Yahoo, and the option value in the settings), `label` is what the card
// shows (short), `name` is shown in the settings list.
struct StockInfo {
    const wchar_t* symbol;
    const wchar_t* label;
    const wchar_t* name;
};
constexpr StockInfo kStockCatalog[] = {
    {L"^GSPC", L"S&P 500", L"S&P 500 (индекс)"},
    {L"AAPL", L"AAPL", L"Apple"},
    {L"MSFT", L"MSFT", L"Microsoft"},
    {L"NVDA", L"NVDA", L"NVIDIA"},
    {L"GOOGL", L"GOOGL", L"Alphabet (Google)"},
    {L"AMZN", L"AMZN", L"Amazon"},
    {L"META", L"META", L"Meta Platforms"},
    {L"TSLA", L"TSLA", L"Tesla"},
    {L"SPCX", L"SPCX", L"SpaceX"},
    {L"BRK-B", L"BRK-B", L"Berkshire Hathaway"},
    {L"AVGO", L"AVGO", L"Broadcom"},
    {L"TSM", L"TSM", L"TSMC"},
    {L"LLY", L"LLY", L"Eli Lilly"},
    {L"JPM", L"JPM", L"JPMorgan Chase"},
    {L"V", L"V", L"Visa"},
    {L"MA", L"MA", L"Mastercard"},
    {L"WMT", L"WMT", L"Walmart"},
    {L"ORCL", L"ORCL", L"Oracle"},
    {L"XOM", L"XOM", L"Exxon Mobil"},
    {L"UNH", L"UNH", L"UnitedHealth"},
    {L"NFLX", L"NFLX", L"Netflix"},
    {L"COST", L"COST", L"Costco"},
    {L"JNJ", L"JNJ", L"Johnson & Johnson"},
    {L"PG", L"PG", L"Procter & Gamble"},
    {L"HD", L"HD", L"Home Depot"},
    {L"ABBV", L"ABBV", L"AbbVie"},
    {L"BAC", L"BAC", L"Bank of America"},
    {L"KO", L"KO", L"Coca-Cola"},
    {L"PEP", L"PEP", L"PepsiCo"},
    {L"MCD", L"MCD", L"McDonald's"},
    {L"CVX", L"CVX", L"Chevron"},
    {L"AMD", L"AMD", L"AMD"},
    {L"ASML", L"ASML", L"ASML"},
    {L"SAP", L"SAP", L"SAP"},
    {L"CRM", L"CRM", L"Salesforce"},
    {L"ADBE", L"ADBE", L"Adobe"},
    {L"PLTR", L"PLTR", L"Palantir"},
    {L"INTC", L"INTC", L"Intel"},
    {L"IBM", L"IBM", L"IBM"},
    {L"DIS", L"DIS", L"Disney"},
    {L"NKE", L"NKE", L"Nike"},
    {L"UBER", L"UBER", L"Uber"},
    {L"NVO", L"NVO", L"Novo Nordisk"},
    {L"AZN", L"AZN", L"AstraZeneca"},
    {L"SHEL", L"SHEL", L"Shell"},
    {L"TM", L"TM", L"Toyota"},
    {L"SONY", L"SONY", L"Sony"},
    {L"BABA", L"BABA", L"Alibaba"},
    {L"TCEHY", L"Tencent", L"Tencent"},
    {L"005930.KS", L"Samsung", L"Samsung Electronics (Seoul)"},
    {L"2222.SR", L"Aramco", L"Saudi Aramco (Riyadh)"},
    {L"MC.PA", L"LVMH", L"LVMH (Paris)"},
};
constexpr int kStockSlots = 6;
constexpr const wchar_t* kDefaultStocks[kStockSlots] = {L"AAPL", L"MSFT", L"NVDA", L"GOOGL", L"AMZN", L"TSLA"};

const StockInfo* FindStock(std::wstring_view symbol) {
    for (const StockInfo& st : kStockCatalog) {
        if (symbol.size() == wcslen(st.symbol) && _wcsnicmp(symbol.data(), st.symbol, symbol.size()) == 0) {
            return &st;
        }
    }
    return nullptr;
}

// Chart period choices: Yahoo `range` + bar `interval`.
enum class StocksRange { Day, FiveDays, Month, SixMonths, Year };

struct Settings {
    Position position = Position::TopCenter;
    int targetMonitor = 0;
    int offsetX = 0;
    int offsetY = 0;
    bool separateExpandedOffsetY = false;  // #83
    int offsetYExpanded = 0;               // #83
    float sizeScale = 1.0f;
    float baseSizeScale = 1.0f;
    std::wstring fontFamily;
    AccentMode accentMode = AccentMode::Auto;
    D2D1_COLOR_F customAccent = D2D1::ColorF(0x4cc9f0);
    int targetFps = 0; // 0 = Auto
    AnimationStyle animationStyle = AnimationStyle::Default;
    ProgressStyle progressStyle = ProgressStyle::Slim;
    SpectrumStyle spectrumStyle = SpectrumStyle::Bars;
    float animationSpeed = 1.0f;
    bool media = true;
    bool mediaAutoExpand = false;
    bool battery = true;
    bool progress = true;
    bool volume = true;
    bool brightness = true;
    CalendarAccentMode calendarAccent = CalendarAccentMode::Accent;
    bool privacyDots = true;
    bool privacyDotsMic = true;
    bool privacyDotsCam = true;
    bool privacyDotsPulse = true;
    bool timerEnabled = true;
    bool hideShowHotkeyEnabled = true;
    UINT hideShowModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT hideShowVk = 'D';

    bool bluetoothIndicator = true;
    bool bluetoothShowBattery = true;
    bool bluetoothDashboard = true;  // standalone 0.7: battery badge on the expanded island
    bool sonyPadBattery = true;      // standalone 0.7: switch DualSense/DS4 to the full BT report
    D2D1_COLOR_F privacyDotsMicHex = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f); // #FF9500
    D2D1_COLOR_F privacyDotsCamHex = D2D1::ColorF(0.133f, 0.776f, 0.239f, 1.0f); // #10B981
    D2D1_COLOR_F privacyDotsBothHex = D2D1::ColorF(0xBF5AF2);  // standalone 0.6: mic + camera
    float tintOpacity = 0.72f;
    float pillOpacity = 0.96f;
    bool gameOverlay = false;
    bool showMetricText = true;
    bool weather = true;
    std::wstring weatherCity;
    bool weatherFahrenheit = false;
    int autoHideIdleSeconds = 0;
    bool autoHideFullscreen = true;
    bool borderMergedMode = false;
    bool unhideOnHover = true;
    bool alwaysOnTop = true;
    bool expandOnHover = true;
    bool autoDpiScale = true;
    bool w11Style = false;
    bool notchStyle = false;
    // Color customization
    ThemePreset themePreset = ThemePreset::Obsidian;
    D2D1_COLOR_F pillBgColor = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f); // #08080A
    D2D1_COLOR_F textPrimaryColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f); // #FFFFFF
    D2D1_COLOR_F textSecondaryColor = D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f); // #9B9BA5
    ContourBorderMode contourBorderMode = ContourBorderMode::Default;
    bool contourBorderEnabled = true;
    D2D1_COLOR_F contourBorderColor = D2D1::ColorF(0.200f, 0.200f, 0.220f, 1.0f); // #333338
    bool clockAccentGlow = true;
    bool privacyDotsEnabled = true;
    bool privacyDotPulsing = true;
    D2D1_COLOR_F micDotColor = D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f); // #FF9500
    D2D1_COLOR_F camDotColor = D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f); // #34C759
    bool hardwareMonitorModule = true;
    // Custom fork (custom.18): stocks tab.
    bool stocksModule = true;
    std::array<std::wstring, kStockSlots> stockSymbols{
        kDefaultStocks[0], kDefaultStocks[1], kDefaultStocks[2],
        kDefaultStocks[3], kDefaultStocks[4], kDefaultStocks[5]};
    StocksRange stocksRange = StocksRange::Day;
    int stocksRefreshMinutes = 2;

    // ── Premium material / redesign ──────────────────────────────────────────
    bool materialDepth = true;      // downward depth shading + accent bloom
    bool dropShadow = true;         // soft shadow under the island
    float accentBloom = 1.0f;       // 0..2 multiplier on the accent wash
    float textScale = 1.0f;         // independent typography scale (#41)
    BackdropMaterial backdropMaterial = BackdropMaterial::None;  // #59
    float backdropTint = 0.55f;     // acrylic tint strength, 0..1
    float backdropFillAlpha = 0.45f;  // how opaque the pill's own fill stays
    // Custom fork (custom.17): Liquid Glass. liquidGlass stays true even when the
    // real backdrop falls back to None (no DWM blur), so the glass edges and
    // highlights are still drawn on the solid fill.
    bool liquidGlass = false;
    bool liquidGlassClear = false;   // "Clear" style: no blur, just the glass
    float liquidGlassClarity = 0.6f; // 0 = milky, 1 = as clear as readable
    float liquidGlassShine = 1.0f;   // 0..2 multiplier on edges and highlights

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    bool showSeconds = false;
    bool use24HourClock = false;    // false = follow system locale
    bool clockFollowSystem = true;
    std::wstring dateFormat;        // empty = locale default
    bool mediaPillClock = false;    // show the clock inside the collapsed media pill
    bool dateFirst = false;
    int firstDayOfWeek = -1;        // 0 = Sunday .. 6 = Saturday, -1 = follow Windows

    // ── Localization (#35) ───────────────────────────────────────────────────
    std::wstring language = L"auto";

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    std::vector<std::wstring> mediaExpandBlocklist;

    // ── Game overlay options (#25) ───────────────────────────────────────────
    bool gameOverlayShowFps = true;
    bool gameOverlayShowCpu = true;
    bool gameOverlayShowGpu = true;
    bool gameOverlayShowRam = true;
    bool gameOverlayShowDisk = true;
    bool gameOverlayCompact = false;

    // ── Custom fork: Telegram (Feature 1) ────────────────────────────────────
    bool telegramEnabled = true;
    bool telegramSuppressBanner = true;
    int telegramDisplaySeconds = 6;
    int telegramMaxMessages = 3;
    bool telegramShowText = true;
    bool telegramRespectWindowsDnd = true;
    bool telegramClearFromCenter = false;
    bool telegramDebugLog = false;

    // ── Custom fork: Discord (same card as Telegram) ────────────────────────
    bool discordEnabled = true;
    bool discordShowOverGames = true;
    bool discordSuppressBanner = true;

    // ── Custom fork: smart game mode (Feature 2) ─────────────────────────────
    bool smartGameMode = true;
    bool gameShowOtherAlerts = false;
    bool gameDetectD3D = true;
    std::vector<std::wstring> gameProcesses;  // lower-case exe names, e.g. "cs2.exe"
    bool gameLightweight = true;

    // ── Custom fork: island Do Not Disturb (Feature 3) ───────────────────────
    bool quietButton = true;
    bool quietHotkeyEnabled = true;
    UINT quietModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT quietVk = 'N';
};

struct BitmapPixels {
    std::vector<uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    uint64_t generation = 0;
    D2D1_COLOR_F sampledAccent = D2D1::ColorF(0x4cc9f0);
};

// Immutable, shared pixel data. Copying one is a refcount bump, so copying
// SharedState every frame no longer copies album art / icons.
// Null-safe: operator-> on an empty ref points at a static empty BitmapPixels.
class PixelsRef {
   public:
    PixelsRef() = default;
    explicit PixelsRef(BitmapPixels&& p)
        : p_(std::make_shared<const BitmapPixels>(std::move(p))) {}

    explicit operator bool() const { return p_ && !p_->bgra.empty(); }
    const BitmapPixels* operator->() const { return p_ ? p_.get() : &Empty(); }
    const BitmapPixels& operator*() const { return *operator->(); }

   private:
    static const BitmapPixels& Empty() {
        static const BitmapPixels e{};
        return e;
    }
    std::shared_ptr<const BitmapPixels> p_;
};

struct MediaSnapshot {
    bool available = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring albumTitle;
    std::wstring sourceAppUserModelId;
    std::wstring sourceName;
    std::wstring sourceBadge;
    PixelsRef art;
    PixelsRef sourceIcon;
    uint64_t artGeneration = 0;
    uint64_t sourceIconGeneration = 0;
    double artChangedAt = 0.0;
    double titleChangedAt = 0.0;
    int64_t positionTicks = 0;
    int64_t endTicks = 0;
    int64_t lastUpdatedTicks = 0;
};

// One SMTC media session the dock can switch to.
struct MediaSourceInfo {
    std::wstring aumid;
    std::wstring name;
    std::wstring badge;
    bool playing = false;
    PixelsRef icon;
};

struct BatterySnapshot {
    bool active = false;
    bool low = false;
    bool charging = false;
    int percent = 100;
    DWORD secondsRemaining = BATTERY_LIFE_UNKNOWN;
    double expiresAt = 0.0;
};

struct ProgressSnapshot {
    bool active = false;
    int percent = 0;
};

// One Telegram message as shown in the island (custom fork, Feature 1).
struct TelegramLine {
    std::wstring chat;    // chat / group title (empty for private chats)
    std::wstring sender;  // who wrote it
    std::wstring text;    // message text (or a placeholder)
    bool group = false;
};

struct NotificationSnapshot {
    bool active = false;
    std::wstring app;
    std::wstring title;
    std::wstring body;
    double expiresAt = 0.0;
    // Custom fork: lifetime of this card and the chat payload. Since custom.16 the
    // island shows only chat cards (Telegram, Discord), so isTelegram is always set
    // for a real notification.
    float ttl = 4.0f;
    bool isTelegram = false;  // true for every chat card (Telegram, Discord)
    std::wstring chatApp;     // custom fork: L"Telegram" / L"Discord"
    std::wstring sourceAumid;
    std::vector<TelegramLine> telegramLines;  // newest first
    int telegramTotal = 0;                    // messages received while this card stayed up
};

struct VolumeSnapshot {
    bool active = false;
    int percent = 0;
    bool muted = false;
    std::wstring deviceName;
    double expiresAt = 0.0;
    float displayPercent = 0.0f;  // animated bar level, set only on the render loop's snapshot copy
};

struct BrightnessSnapshot {
    bool active = false;
    int percent = 0;
    double expiresAt = 0.0;
};

struct TimerSnapshot {
    bool active = false;          // a session exists (running or paused)
    bool running = false;         // currently counting down
    bool isBreak = false;         // work vs break session
    int totalSeconds = 0;
    double endsAt = 0.0;          // NowSeconds() at completion, while running
    double remainingAtPause = 0.0;
    bool justFinished = false;
    double finishedExpiresAt = 0.0;
};

enum class DeviceEventType {
    Connected,
    Disconnected,
};

struct DeviceSnapshot {
    bool active = false;
    DeviceEventType eventType = DeviceEventType::Connected;
    std::wstring deviceName;  // e.g. "USB Drive" or "Bluetooth Device"
    bool isBluetoothLike = false;
    double expiresAt = 0.0;
};

struct BluetoothDeviceSnapshot {
    bool active = false;
    bool connected = true;   // true = just connected, false = just disconnected
    std::wstring deviceName;
    int batteryPercent = -1; // -1 = unknown/unavailable
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    double expiresAt = 0.0;
};

// Standalone 0.7: a connected Bluetooth accessory for the battery badge.
struct BluetoothBatteryItem {
    std::wstring name;
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    int percent = -1;  // -1 = Windows/the device does not report it
    bool charging = false;
};

struct SystemSnapshot {
    int volumePercent = 0;
    bool volumeMuted = false;
    int cpuPercent = 0;
    int memoryPercent = 0;
    float memoryUsedGB = 0.0f;
    float memoryTotalGB = 0.0f;
    int diskFreePercent = 0;
    int renderFps = 0;
    int gpuPercent = -1;
    float netUpMbps = 0.0f;
    float netDownMbps = 0.0f;
    bool charging = false;
    bool micActive = false;      // orange dot: microphone in use
    bool cameraActive = false;   // green dot: camera in use
    std::wstring foregroundTitle;
    std::wstring micApp;
    std::wstring cameraApp;
};

struct Activity {
    IslandKind kind = IslandKind::Idle;
    float width = 120.0f;
    float height = 36.0f;
};

// Custom fork (custom.18): one stock card. `points` are the chart's closing
// prices, oldest first; `basePrice` is the close before the chart period
// (yesterday's close on the 1-day chart), the reference for the change.
struct StockQuote {
    std::wstring symbol;
    std::wstring label;
    std::wstring currency;
    double price = 0.0;
    double basePrice = 0.0;
    std::vector<float> points;
    bool ok = false;
};

struct StocksSnapshot {
    std::array<StockQuote, kStockSlots> quotes;
    double lastUpdated = 0.0;   // NowSeconds() of the last finished fetch (0 = never)
    SYSTEMTIME updatedAt = {};  // local wall-clock time of that fetch, for the header
    bool loading = false;
    bool anyOk = false;
};

struct WeatherSnapshot {
    bool hasData = false;
    float temperature = 0.0f;
    int weatherCode = 0;
    std::wstring city;
    std::wstring weatherDesc;
    std::wstring windSpeed;
    std::wstring windDir;
    std::wstring humidity;
    std::wstring feelsLike;
    double lastUpdated = 0.0;
};


struct SharedState {
    MediaSnapshot media;
    std::vector<MediaSourceInfo> mediaSources;  // stable dock order
    NotificationSnapshot notification;
    VolumeSnapshot volume;
    BrightnessSnapshot brightness;
    TimerSnapshot timer;
    DeviceSnapshot device;
    BluetoothDeviceSnapshot bluetoothDevice;
    std::vector<BluetoothBatteryItem> bluetoothBatteries;  // standalone 0.7: connected now, best first
    BatterySnapshot battery;
    ProgressSnapshot progress;
    SystemSnapshot system;
    WeatherSnapshot weather;
    StocksSnapshot stocks;  // custom fork (custom.18)
    std::array<float, 48> waveform{};
    size_t waveformWrite = 0;
    std::array<float, kSpectrumBands> bands{};  // smoothed 0..1 magnitude per band (low -> high)
    bool muted = false;
};

struct SpringValue {
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;

    void Reset(float v) {
        value = target = v;
        velocity = 0.0f;
    }

    void Step(float totalDt, float stiffness, float damping) {
        const float kFixedDt = 0.0005f;
        while (totalDt > 0.0f) {
            float dt = std::min(totalDt, kFixedDt);
            const float displacement = value - target;
            const float acceleration = -stiffness * displacement - damping * velocity;
            velocity += acceleration * dt;
            value += velocity * dt;
            totalDt -= dt;
        }

        if (std::fabs(value - target) < 0.01f && std::fabs(velocity) < 0.01f) {
            value = target;
            velocity = 0.0f;
        }
    }
};

Settings g_settings;

// Guards every access to g_settings. LoadSettings() replaces the whole struct
// from Windhawk's settings thread *and* from the tray context menu (which runs
// on the render thread), while the render, weather and media threads read it
// concurrently. Settings holds std::wstring / std::vector members, so an
// unsynchronised assignment can free a buffer a reader is still walking -- not
// a stale-value glitch but a genuine use-after-free.
//
// This is a strict LEAF lock: never acquire another mutex while holding it.
// That keeps it deadlock-free even where a caller already owns g_stateMutex
// (see WeatherThreadProc), because no path can ever take the two in the
// opposite order.
std::mutex g_settingsMutex;

// Returns a private copy of the current settings. Callers then read from the
// copy for the rest of the frame, which also means a settings change landing
// mid-frame can't tear a single render across two configurations.
Settings GetSettingsCopy() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

std::mutex g_stateMutex;
SharedState g_state;
std::atomic<uint64_t> g_artGenerationCounter = 0;

HWND g_hwnd = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_settingsChangedEvent = nullptr;
HANDLE g_renderThread = nullptr;
HANDLE g_mediaThread = nullptr;
HANDLE g_audioThread = nullptr;
HANDLE g_weatherThread = nullptr;
// Custom fork (custom.18): stocks tab.
HANDLE g_stocksThread = nullptr;
HANDLE g_stocksWakeEvent = nullptr;          // auto-reset: settings changed / tab shown with old data
std::atomic<double> g_stocksLastViewed{-1e9};  // NowSeconds() when the stocks tab was last on screen
std::atomic<bool> g_stocksForceFetch{false};
HANDLE g_notificationThread = nullptr;
HANDLE g_bluetoothThread = nullptr;
std::atomic<bool> g_running = false;
std::atomic<int> g_idleTab = 0;
// Set when the default render endpoint must be re-read. Declared up here (not next to
// VolumeNotifyClient) so the Bluetooth watcher, which comes earlier in the file, can set it.
std::atomic<bool> g_volumeEndpointStale{true};
std::atomic<bool> g_layoutDirty = true;
std::atomic<bool> g_clickExpanded = false;
std::atomic<int> g_pressedMediaButton = -1;
std::atomic<int> g_hoveredMediaButton = -1;

// Media source selection (source dock).
std::mutex g_mediaSourceMutex;                     // leaf lock for the two strings below
std::wstring g_preferredMediaSource;               // AUMID the user picked; empty = Auto
std::wstring g_dockOverrideAumid;                  // temporary pick from the dock; guarded by g_mediaSourceMutex
std::atomic<unsigned> g_dockOverrideRev{0};        // bumped on every dock pick / Auto reset
std::atomic<double> g_userSourceSwitchAt{-100.0};  // NowSeconds() of the last user switch
HANDLE g_mediaRefreshEvent = nullptr;              // wakes MediaThreadProc
std::atomic<int> g_hoveredSourceSlot{-1};
std::atomic<int> g_pressedSourceSlot{-1};
std::atomic<int> g_tabEdgeBump{0};            // +1 / -1: wheel or swipe pushed past the last / first tab
std::atomic<double> g_tabLastPaintAt{0.0};    // last time the tab view painted a moving frame
std::atomic<bool> g_tabAnimating{false};      // a tab transition is in motion (render loop paces frames for it)
std::atomic<bool> g_scrubbing = false;           // true while press-dragging the media timeline scrubber
std::atomic<float> g_scrubDragFraction = 0.0f;   // live 0..1 drag position while g_scrubbing is true
std::atomic<double> g_lastLiveSeekTime = 0.0;    // throttle gate for live seeks while dragging
std::atomic<bool> g_audioCaptureNeeded = false;  // gates the WASAPI loopback thread
std::atomic<bool> g_spectrumBandsNeeded = false; // only Pulse Orb / Plasma Thread read the frequency bands
std::atomic<bool> g_manuallyHidden = false;      // user-toggled hide, via menu or hotkey
UINT g_registeredHotkeyModifiers = 0;            // modifiers currently registered with the OS
UINT g_registeredHotkeyVk = 0;                   // vk currently registered with the OS
bool g_hotkeyRegistered = false;

// --- Zero-CPU parking for idle/fullscreen auto-hide (mirrors g_manuallyHidden) ---
std::atomic<bool> g_autoHiddenParked = false;          // true while OS-hidden due to idle/fullscreen auto-hide
std::atomic<bool> g_fullscreenOverrideVisible = false; // user forced the island visible via hotkey while fullscreen
std::atomic<bool> g_isFullscreen = false;              // cached fullscreen state, shared with the hotkey handler
std::atomic<double> g_hotkeyUnhideUntil = 0.0;         // grace deadline to keep island visible after hotkey / settings unhide
FILETIME g_prevIdleTime = {};
FILETIME g_prevKernelTime = {};
FILETIME g_prevUserTime = {};
bool g_brightnessInitialized = false;
int g_lastBrightness = -1;
BYTE g_brightnessPowerSource = 255;  // ACLineStatus of the previous sample
std::atomic<double> g_lastNudgeTime = 0.0;

std::mutex g_bluetoothBatteryCacheMutex;
std::unordered_map<std::wstring, int> g_bluetoothBatteryCache;  // Bluetooth device Id -> last known battery percent (-1 = never learned)
std::atomic<uint64_t> g_bluetoothConnectGeneration = 0;
std::atomic<bool> g_isDnDActive = false;
void* g_wnfDndSubscription = nullptr;

// Custom fork, Feature 3: the island's own Do Not Disturb. Persisted as the
// "QuietMode" value. While on, the island is fully hidden and notifications are
// dropped. g_quietHideAt lets the "On" confirmation pill finish before hiding.
std::atomic<bool> g_quietMode{false};
// Standalone 0.5: the power button. The island shrinks away exactly like Do Not
// Disturb, then the program quits (the main thread runs the normal shutdown).
std::atomic<bool> g_exitRequested{false};
std::atomic<double> g_quietHideAt{0.0};    // when the shrink animation starts
std::atomic<bool> g_quietCollapsed{false};  // shrink finished -> window hidden
bool g_quietHotkeyRegistered = false;
std::atomic<bool> g_pinnedExpanded{false};

constexpr GUID kSubTypeIeeeFloat = {
    0x00000003,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};



double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

// Smoothstep easing (ease in / ease out).
float SmoothStep01(float t) {
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ── Media source selection ───────────────────────────────────────────────────
int ActiveMediaSourceIndex(const SharedState& state) {
    for (size_t i = 0; i < state.mediaSources.size(); ++i) {
        if (state.mediaSources[i].aumid == state.media.sourceAppUserModelId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::wstring GetPreferredMediaSource() {
    std::lock_guard lock(g_mediaSourceMutex);
    return g_preferredMediaSource;
}

// Instant switch. Empty AUMID = Auto. The timestamp tells MediaThreadProc this title
// change is not a "new track": no auto-expand, no 5s recentTrackChange window.
void RequestMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        // Dock pick: a TEMPORARY override, not a lock. MediaThreadProc drops it as soon as
        // something new starts playing or that app goes away, so Auto keeps working.
        g_dockOverrideAumid = aumid;
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// Right-click menu pick. Empty AUMID = Auto. A real AUMID LOCKS the island to that app
// until Auto or another app is picked; what starts playing elsewhere is ignored.
// Any temporary dock pick is cleared, so "Auto" really means "newest playing app".
void LockMediaSource(const std::wstring& aumid) {
    {
        std::lock_guard lock(g_mediaSourceMutex);
        g_preferredMediaSource.assign(aumid);
        g_dockOverrideAumid.clear();
        g_dockOverrideRev.fetch_add(1);
    }
    g_userSourceSwitchAt.store(NowSeconds());
    if (g_mediaRefreshEvent) {
        SetEvent(g_mediaRefreshEvent);
    }
    g_layoutDirty = true;
}

// "Playing from %s" + "Brave" -> "Playing from Brave"
std::wstring FormatWithName(const wchar_t* format, const std::wstring& name) {
    std::wstring out = format;
    const size_t pos = out.find(L"%s");
    if (pos != std::wstring::npos) {
        out.replace(pos, 2, name);
    }
    return out;
}

// ── Dashboard tab loop ───────────────────────────────────────────────────────
// Single source of truth for how many tabs are in the mouse-wheel scroll loop.
// The first tab's meaning depends on context -- the media view when something is
// playing, the clock when idle -- but the *count* is identical, which is why the
// renderer, the scroll handler and the hit tests can all share this. That
// arithmetic used to be copy-pasted in seven places, which is exactly how they
// drifted out of sync.
int ActiveTabCount(const Settings& settings) {
    int count = 2;  // primary (media or clock) + calendar
    if (settings.weather) ++count;
    if (settings.hardwareMonitorModule) ++count;
    if (settings.stocksModule) ++count;  // custom fork (custom.18): always the last tab
    return count;
}

int NormalizedTabIndex(const Settings& settings) {
    const int total = std::max(1, ActiveTabCount(settings));
    const int raw = g_idleTab.load(std::memory_order_relaxed);
    return ((raw % total) + total) % total;
}

// Tab order is always: primary (media or clock), calendar, weather?, hardware?
// The expensive GPU / network counters are only sampled while the hardware card
// is actually on screen, so that check needs the card's real index.
int HardwareMonitorTabIndex(const Settings& settings) {
    if (!settings.hardwareMonitorModule) {
        return -1;
    }
    return 2 + (settings.weather ? 1 : 0);
}

// Moves the dashboard one tab (+1 = next, -1 = previous). Clamped at both ends:
// pushing past the first or last tab is ignored completely (no wrap, no bounce),
// so a free-spinning wheel or touchpad inertia can never make the edge tabs twitch.
void StepIdleTab(int direction) {
    const Settings settings = GetSettingsCopy();
    const int count = std::max(1, ActiveTabCount(settings));
    const int next = NormalizedTabIndex(settings) + direction;
    if (next < 0 || next >= count) {
        return;
    }
    g_idleTab = next;
    g_layoutDirty = true;
}

// ── Localization (#35) ───────────────────────────────────────────────────────
// Every user-visible string in the island goes through Loc(). The English text
// doubles as the lookup key, so an untranslated string degrades to readable
// English rather than to a raw identifier, and adding a language means adding
// one column -- no key bookkeeping.
enum class UiLanguage {
    English,
    French,
    Spanish,
    German,
    Portuguese,
    Italian,
    Russian,
    Turkish,
    Hindi,
    ChineseSimplified,
    Japanese,
    Korean,
    Count,
};

std::atomic<int> g_uiLanguage{static_cast<int>(UiLanguage::English)};

UiLanguage LanguageFromTag(std::wstring_view tag) {
    struct Entry { const wchar_t* tag; UiLanguage lang; };
    static constexpr Entry kTags[] = {
        {L"en", UiLanguage::English},   {L"fr", UiLanguage::French},
        {L"es", UiLanguage::Spanish},   {L"de", UiLanguage::German},
        {L"pt", UiLanguage::Portuguese},{L"it", UiLanguage::Italian},
        {L"ru", UiLanguage::Russian},   {L"tr", UiLanguage::Turkish},
        {L"hi", UiLanguage::Hindi},     {L"zh", UiLanguage::ChineseSimplified},
        {L"ja", UiLanguage::Japanese},  {L"ko", UiLanguage::Korean},
    };
    if (tag.size() < 2) {
        return UiLanguage::English;
    }
    for (const Entry& e : kTags) {
        // Prefix match so "pt-BR" / "zh-Hans-CN" resolve to their base language.
        if (_wcsnicmp(tag.data(), e.tag, 2) == 0) {
            return e.lang;
        }
    }
    return UiLanguage::English;
}

// Resolves the "auto" language setting from the user's Windows UI language.
UiLanguage DetectSystemLanguage() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, ARRAYSIZE(name)) > 0) {
        return LanguageFromTag(name);
    }
    return UiLanguage::English;
}

// ── Clock / date presentation (#61) ──────────────────────────────────────────

// Formats the time honouring the 12h/24h override and the seconds toggle.
// Windows' own locale formatting is used unless the user forced a mode, so the
// default keeps regional conventions (separators, AM/PM placement) intact.
std::wstring FormatIslandTime(const SYSTEMTIME& local, bool followSystem, bool use24Hour,
                              bool showSeconds) {
    wchar_t buffer[64] = {};

    if (followSystem) {
        const DWORD flags = showSeconds ? 0 : TIME_NOSECONDS;
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &local, nullptr, buffer,
                            ARRAYSIZE(buffer)) > 0) {
            return buffer;
        }
        return L"--:--";
    }

    // Explicit override: build the pattern rather than fighting locale flags.
    const wchar_t* pattern = use24Hour ? (showSeconds ? L"HH:mm:ss" : L"HH:mm")
                                       : (showSeconds ? L"h:mm:ss tt" : L"h:mm tt");
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, pattern, buffer,
                        ARRAYSIZE(buffer)) > 0) {
        return buffer;
    }
    return L"--:--";
}

// Formats the date. A custom pattern is passed straight to Windows, which
// already supports yyyy / MM / dd / MMM / MMMM / ddd / dddd and prints anything
// else literally -- so CJK patterns such as yyyy年MM月dd日 work as typed.
std::wstring FormatIslandDate(const SYSTEMTIME& local, const std::wstring& customFormat,
                              const wchar_t* fallbackPattern) {
    wchar_t buffer[128] = {};

    if (!customFormat.empty()) {
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, customFormat.c_str(), buffer,
                            ARRAYSIZE(buffer), nullptr) > 0) {
            return buffer;
        }
        // An invalid pattern falls through to the default rather than showing
        // nothing at all.
    }

    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, fallbackPattern, buffer,
                        ARRAYSIZE(buffer), nullptr) > 0) {
        return buffer;
    }
    return std::wstring();
}

const wchar_t* Loc(const wchar_t* english) {
    struct Row {
        const wchar_t* key;
        // Indexed by UiLanguage; nullptr falls back to the English key.
        const wchar_t* text[static_cast<size_t>(UiLanguage::Count)];
    };

    // Order: en, fr, es, de, pt, it, ru, tr, hi, zh, ja, ko
    static const Row kRows[] = {
        {L"Media", {nullptr, L"Média", L"Multimedia", L"Medien", L"Mídia", L"Media", L"Медиа", L"Medya", L"मीडिया", L"媒体", L"メディア", L"미디어"}},
        {L"Calendar", {nullptr, L"Calendrier", L"Calendario", L"Kalender", L"Calendário", L"Calendario", L"Календарь", L"Takvim", L"कैलेंडर", L"日历", L"カレンダー", L"캘린더"}},
        {L"Weather", {nullptr, L"Météo", L"Tiempo", L"Wetter", L"Tempo", L"Meteo", L"Погода", L"Hava", L"मौसम", L"天气", L"天気", L"날씨"}},
        {L"Hardware Monitor", {nullptr, L"Moniteur matériel", L"Monitor de hardware", L"Hardware-Monitor", L"Monitor de hardware", L"Monitor hardware", L"Монитор системы", L"Donanım İzleme", L"हार्डवेयर मॉनिटर", L"硬件监视器", L"ハードウェア モニター", L"하드웨어 모니터"}},
        {L"Bluetooth", {nullptr, L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"Bluetooth", L"ब्लूटूथ", L"蓝牙", L"Bluetooth", L"블루투스"}},
        {L"Volume", {nullptr, L"Volume", L"Volumen", L"Lautstärke", L"Volume", L"Volume", L"Громкость", L"Ses", L"वॉल्यूम", L"音量", L"音量", L"볼륨"}},
        {L"Brightness", {nullptr, L"Luminosité", L"Brillo", L"Helligkeit", L"Brilho", L"Luminosità", L"Яркость", L"Parlaklık", L"चमक", L"亮度", L"明るさ", L"밝기"}},
        {L"Muted", {nullptr, L"Muet", L"Silenciado", L"Stumm", L"Sem som", L"Muto", L"Без звука", L"Sessiz", L"म्यूट", L"已静音", L"ミュート", L"음소거"}},
        {L"Low Battery", {nullptr, L"Batterie faible", L"Batería baja", L"Akku schwach", L"Bateria fraca", L"Batteria scarica", L"Батарея разряжена", L"Pil Az", L"बैटरी कम", L"电量低", L"バッテリー残量低下", L"배터리 부족"}},
        {L"Charging", {nullptr, L"En charge", L"Cargando", L"Wird geladen", L"Carregando", L"In carica", L"Зарядка", L"Şarj oluyor", L"चार्ज हो रहा है", L"正在充电", L"充電中", L"충전 중"}},
        {L"Connected", {nullptr, L"Connecté", L"Conectado", L"Verbunden", L"Conectado", L"Connesso", L"Подключено", L"Bağlandı", L"कनेक्ट किया गया", L"已连接", L"接続済み", L"연결됨"}},
        {L"Disconnected", {nullptr, L"Déconnecté", L"Desconectado", L"Getrennt", L"Desconectado", L"Disconnesso", L"Отключено", L"Bağlantı kesildi", L"डिस्कनेक्ट किया गया", L"已断开", L"切断されました", L"연결 끊김"}},
        {L"Focus", {nullptr, L"Concentration", L"Concentración", L"Fokus", L"Foco", L"Concentrazione", L"Фокус", L"Odak", L"फ़ोकस", L"专注", L"集中", L"집중"}},
        {L"Break", {nullptr, L"Pause", L"Descanso", L"Pause", L"Pausa", L"Pausa", L"Перерыв", L"Mola", L"विराम", L"休息", L"休憩", L"휴식"}},
        {L"Unknown", {nullptr, L"Inconnu", L"Desconocido", L"Unbekannt", L"Desconhecido", L"Sconosciuto", L"Неизвестно", L"Bilinmiyor", L"अज्ञात", L"未知", L"不明", L"알 수 없음"}},
        {L"Loading...", {nullptr, L"Chargement...", L"Cargando...", L"Wird geladen...", L"Carregando...", L"Caricamento...", L"Загрузка...", L"Yükleniyor...", L"लोड हो रहा है...", L"加载中...", L"読み込み中...", L"불러오는 중..."}},
        {L"Locating...", {nullptr, L"Localisation...", L"Ubicando...", L"Standort...", L"Localizando...", L"Localizzazione...", L"Определение...", L"Konum...", L"स्थान...", L"定位中...", L"位置情報...", L"위치 확인 중..."}},
        {L"Wind", {nullptr, L"Vent", L"Viento", L"Wind", L"Vento", L"Vento", L"Ветер", L"Rüzgar", L"हवा", L"风速", L"風", L"바람"}},
        {L"Feels Like", {nullptr, L"Ressenti", L"Sensación", L"Gefühlt", L"Sensação", L"Percepita", L"Ощущается", L"Hissedilen", L"महसूस", L"体感", L"体感", L"체감"}},
        {L"Humidity", {nullptr, L"Humidité", L"Humedad", L"Luftfeuchte", L"Umidade", L"Umidità", L"Влажность", L"Nem", L"नमी", L"湿度", L"湿度", L"습도"}},
        {L"No devices", {nullptr, L"Aucun appareil", L"Sin dispositivos", L"Keine Geräte", L"Nenhum dispositivo", L"Nessun dispositivo", L"Нет устройств", L"Cihaz yok", L"कोई डिवाइस नहीं", L"无设备", L"デバイスなし", L"장치 없음"}},
        {L"Media source", {nullptr, L"Source multimédia", L"Fuente multimedia", L"Medienquelle", L"Fonte de mídia", L"Sorgente multimediale", L"Источник медиа", L"Medya kaynağı", L"मीडिया स्रोत", L"媒体来源", L"メディア ソース", L"미디어 소스"}},
        {L"Auto", {nullptr, L"Auto", L"Automático", L"Automatisch", L"Automático", L"Automatico", L"Авто", L"Otomatik", L"स्वचालित", L"自动", L"自動", L"자동"}},
        {L"Playing from %s", {nullptr, L"Lecture depuis %s", L"Reproduciendo desde %s", L"Wiedergabe über %s", L"Reproduzindo em %s", L"In riproduzione da %s", L"Воспроизводится в %s", L"%s üzerinden oynatılıyor", L"%s से चल रहा है", L"正在 %s 中播放", L"%s で再生中", L"%s에서 재생 중"}},
        {L"Paused in %s", {nullptr, L"En pause dans %s", L"En pausa en %s", L"Pausiert in %s", L"Pausado em %s", L"In pausa in %s", L"Приостановлено в %s", L"%s içinde duraklatıldı", L"%s में रोका गया", L"已在 %s 中暂停", L"%s で一時停止中", L"%s에서 일시 중지됨"}},
    };

    const int langIndex = g_uiLanguage.load(std::memory_order_relaxed);
    if (langIndex <= static_cast<int>(UiLanguage::English) ||
        langIndex >= static_cast<int>(UiLanguage::Count)) {
        return english;
    }

    for (const Row& row : kRows) {
        if (wcscmp(row.key, english) == 0) {
            const wchar_t* translated = row.text[static_cast<size_t>(langIndex)];
            return translated ? translated : english;
        }
    }
    return english;
}

// ── Media dashboard hit-testing (see the notes next to MediaLayout) ──────────

// Converts a client-area mouse position into the media dashboard's content
// space. Returns valid == false when DrawMedia has not painted recently, so a
// stale frame can never produce a phantom hit.
MediaContentPoint MediaContentFromClient(int clientX, int clientY) {
    MediaContentPoint out;

    // Seqlock read: retry until a write is not in progress and the counter has
    // not moved, so all six fields come from the same frame.
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f, scale = 1.0f;
    unsigned long long stamp = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const unsigned before = g_mediaHitSeq.load(std::memory_order_relaxed);
        if (before & 1u) {
            continue;  // write in progress
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        left = g_mediaHitLeft.load(std::memory_order_relaxed);
        top = g_mediaHitTop.load(std::memory_order_relaxed);
        right = g_mediaHitRight.load(std::memory_order_relaxed);
        bottom = g_mediaHitBottom.load(std::memory_order_relaxed);
        scale = g_mediaHitScale.load(std::memory_order_relaxed);
        stamp = g_mediaHitStamp.load(std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_mediaHitSeq.load(std::memory_order_relaxed) == before) {
            break;
        }
        stamp = 0;  // torn; force another attempt (or bail out below)
    }

    if (stamp == 0 || GetTickCount64() - stamp > 500) {
        return out;
    }

    if (scale < 0.05f) {
        scale = 1.0f;
    }

    const float width = right - left;
    const float height = bottom - top;
    if (width <= 1.0f || height <= 1.0f) {
        return out;
    }

    // DrawPill scales content about the pill centre, so undo that about the
    // same point to get back into content space.
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float contentX = (static_cast<float>(clientX) - centerX) / scale + centerX;
    const float contentY = (static_cast<float>(clientY) - centerY) / scale + centerY;

    out.valid = true;
    out.x = contentX - left;
    out.y = contentY - top;
    out.width = width;
    out.height = height;
    return out;
}

// Returns 0 = previous, 1 = play/pause, 2 = next, or -1 for no button.
int MediaTransportHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }

    const float centerX = pt.width * 0.5f;
    const float dy = pt.y - MediaLayout::kControlsY;

    struct Target {
        float offsetX;
        float radius;
        int command;
    };
    const Target targets[] = {
        {-MediaLayout::kControlSpacing, MediaLayout::kNavButtonRadius, 0},
        {0.0f, MediaLayout::kPlayButtonRadius, 1},
        {MediaLayout::kControlSpacing, MediaLayout::kNavButtonRadius, 2},
    };

    for (const Target& t : targets) {
        const float reach = t.radius + MediaLayout::kControlHitPad;
        if (std::fabs(dy) <= reach && std::fabs(pt.x - (centerX + t.offsetX)) <= reach) {
            return t.command;
        }
    }
    return -1;
}

// Fraction along the scrubber bar (0..1) for a content-space point, or -1 when
// the point is not on the bar.
float MediaScrubFractionFromContent(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1.0f;
    }

    const float barLeft = MediaLayout::kScrubInsetLeft;
    const float barRight = pt.width - MediaLayout::kScrubInsetRight;
    if (barRight - barLeft <= 1.0f) {
        return -1.0f;
    }

    if (std::fabs(pt.y - MediaLayout::kScrubberY) > MediaLayout::kScrubHitHalfHeight ||
        pt.x < barLeft - MediaLayout::kScrubHitPadX ||
        pt.x > barRight + MediaLayout::kScrubHitPadX) {
        return -1.0f;
    }

    return Clamp((Clamp(pt.x, barLeft, barRight) - barLeft) / (barRight - barLeft), 0.0f, 1.0f);
}

// Clamped fraction for an ongoing drag, ignoring the on-bar test so the scrub
// keeps tracking once the press has been captured.
float MediaScrubFractionUnbounded(const MediaContentPoint& pt) {
    if (!pt.valid) {
        return -1.0f;
    }
    const float barLeft = MediaLayout::kScrubInsetLeft;
    const float barRight = pt.width - MediaLayout::kScrubInsetRight;
    if (barRight - barLeft <= 1.0f) {
        return -1.0f;
    }
    return Clamp((Clamp(pt.x, barLeft, barRight) - barLeft) / (barRight - barLeft), 0.0f, 1.0f);
}

// Custom fork, Feature 3: the Do Not Disturb toggle switch (moon + switch),
// top-right of the first tab of the expanded island -- the Time tab when idle,
// the player tab while media plays. That corner is free in
// both: album art is on the left, the title starts at y=34, the spectrum at
// y=42. Positions are relative to the right / top edge of the content rect
// (for idle: the expanded full rect, centred on the content rect).
namespace QuietToggleLayout {
    constexpr float kRightInset = 22.0f;  // content right edge -> track right edge
    constexpr float kCenterY = 20.0f;     // content top edge -> track centre
    constexpr float kTrackW = 36.0f;
    constexpr float kTrackH = 20.0f;
    constexpr float kMoonGap = 6.0f;      // track left edge -> moon glyph right edge
    constexpr float kMoonW = 16.0f;
    // Standalone 0.4/0.5: round buttons left of the moon -- settings (gear), then power.
    constexpr float kBtnD = 24.0f;        // circle diameter
    constexpr float kBtnGap = 9.0f;       // moon glyph left edge -> gear circle right edge
    constexpr float kBtnSpacing = 8.0f;   // gear circle left edge -> power circle right edge

    // Centre x of button `i` (0 = gear, 1 = power), from the moon's left edge.
    inline float ButtonCenterX(float moonLeft, int i) {
        return moonLeft - kBtnGap - kBtnD * 0.5f - static_cast<float>(i) * (kBtnD + kBtnSpacing);
    }
}

enum class TopButton { None, Settings, Power };

bool QuietButtonHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height < MediaLayout::kExpandedHeight - 12.0f) {
        return false;
    }
    const float fullW = std::max(MediaLayout::kExpandedWidth, pt.width);
    const float fullLeft = (pt.width - fullW) * 0.5f;
    const float trackRight = fullLeft + fullW - QuietToggleLayout::kRightInset;
    const float left = trackRight - QuietToggleLayout::kTrackW - QuietToggleLayout::kMoonGap -
                       QuietToggleLayout::kMoonW - 4.0f;
    const float right = trackRight + 6.0f;
    const float top = QuietToggleLayout::kCenterY - QuietToggleLayout::kTrackH * 0.5f - 6.0f;
    const float bottom = QuietToggleLayout::kCenterY + QuietToggleLayout::kTrackH * 0.5f + 6.0f;
    return pt.x >= left && pt.x <= right && pt.y >= top && pt.y <= bottom;
}

// Standalone 0.4/0.5: which round button (gear / power) the point is over.
TopButton TopButtonHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height < MediaLayout::kExpandedHeight - 12.0f) {
        return TopButton::None;
    }
    const float fullW = std::max(MediaLayout::kExpandedWidth, pt.width);
    const float fullLeft = (pt.width - fullW) * 0.5f;
    const float trackRight = fullLeft + fullW - QuietToggleLayout::kRightInset;
    const float moonLeft = trackRight - QuietToggleLayout::kTrackW - QuietToggleLayout::kMoonGap -
                           QuietToggleLayout::kMoonW;
    const float r = QuietToggleLayout::kBtnD * 0.5f + 2.0f;  // a little forgiving
    for (int i = 0; i < 2; ++i) {
        const float dx = pt.x - QuietToggleLayout::ButtonCenterX(moonLeft, i);
        const float dy = pt.y - QuietToggleLayout::kCenterY;
        if (dx * dx + dy * dy <= r * r) {
            return i == 0 ? TopButton::Settings : TopButton::Power;
        }
    }
    return TopButton::None;
}

// Opens the Launcher next to DynamicIsland.exe; without it, settings.json in
// Notepad (changes apply as soon as the file is saved).
void OpenLauncher() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring launcher(exe, n);
    launcher = launcher.substr(0, launcher.find_last_of(L'\\') + 1) + L"DynamicIslandLauncher.exe";
    HINSTANCE result = nullptr;
    if (GetFileAttributesW(launcher.c_str()) != INVALID_FILE_ATTRIBUTES) {
        result = ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::wstring path = di::settings::FilePath();
        result = ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr,
                               SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        DI_LOG(L"Failed to open the settings (%s).", launcher.c_str());
    }
}

// True when the point is over the expanded layout's album art.
bool MediaArtHitTest(const MediaContentPoint& pt) {
    if (!pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return false;
    }
    return pt.x >= MediaLayout::kArtInsetX &&
           pt.x <= MediaLayout::kArtInsetX + MediaLayout::kArtSize &&
           pt.y >= MediaLayout::kArtInsetY &&
           pt.y <= MediaLayout::kArtInsetY + MediaLayout::kArtSize;
}

// ── Source dock: slot layout + hit testing ──────────────────────────────────
struct DockSlots {
    int count = 0;                      // slots drawn
    int sourceIndex[3] = {-1, -1, -1};  // index into mediaSources; -1 = the "+N" slot
    int overflow = 0;                   // N in "+N" (0 = none)
};

// Drawing and hit testing call this with the same inputs, so they always agree.
DockSlots ComputeDockSlots(int sourceCount, int activeIndex) {
    DockSlots d;
    if (sourceCount < 2) return d;
    if (sourceCount <= MediaLayout::kDockMaxSlots) {
        d.count = sourceCount;
        for (int i = 0; i < sourceCount; ++i) d.sourceIndex[i] = i;
    } else {
        d.count = 3;  // first, the active one, and "+N" for the rest
        d.sourceIndex[0] = 0;
        d.sourceIndex[1] = activeIndex >= 2 ? activeIndex : 1;
        d.sourceIndex[2] = -1;
        d.overflow = sourceCount - 2;
    }
    return d;
}

// Slot under a content-space point, or -1. Boundaries sit in the middle of the gaps,
// so the targets tile the pill (28px+ each) with no dead space.
int MediaSourceDockHitTest(const MediaContentPoint& pt, int slotCount) {
    if (slotCount < 2 || !pt.valid || pt.height <= MediaLayout::kExpandedMinHeight) {
        return -1;
    }
    const float halfH = std::max(MediaLayout::kDockHitMin, MediaLayout::kDockHeight) * 0.5f;
    if (std::fabs(pt.y - MediaLayout::kControlsY) > halfH) return -1;

    const float left = MediaLayout::kDockLeft;
    const float right = left + MediaLayout::DockWidth(slotCount);
    if (pt.x < left || pt.x > right) return -1;

    const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
    const float origin = left + MediaLayout::kDockPad - MediaLayout::kDockGap * 0.5f;
    const int index = static_cast<int>(std::floor((pt.x - origin) / stride));
    return ClampInt(index, 0, slotCount - 1);
}

struct DockHit {
    int slot = -1;
    bool overflow = false;  // the "+N" slot
    bool isActive = false;
    std::wstring aumid;
};

DockHit ResolveSourceDockHit(const MediaContentPoint& pt) {
    DockHit hit;
    std::lock_guard lock(g_stateMutex);
    const int count = static_cast<int>(g_state.mediaSources.size());
    const int active = ActiveMediaSourceIndex(g_state);
    const DockSlots slots = ComputeDockSlots(count, active);
    const int slot = MediaSourceDockHitTest(pt, slots.count);
    if (slot < 0) return hit;

    hit.slot = slot;
    const int idx = slots.sourceIndex[slot];
    if (idx < 0) {
        hit.overflow = true;
        return hit;
    }
    hit.aumid = g_state.mediaSources[idx].aumid;
    hit.isActive = (idx == active);
    return hit;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) {
            return false;
        }
    }

    return true;
}

std::wstring GetStringSettingCopy(PCWSTR name) {
    return di::settings::GetString(name);
}

std::wstring GetStringSettingWithFallback(PCWSTR primary, PCWSTR fallback, PCWSTR fallback2 = nullptr) {
    std::wstring result = GetStringSettingCopy(primary);
    if (!result.empty()) {
        return result;
    }
    result = GetStringSettingCopy(fallback);
    if (!result.empty()) {
        return result;
    }
    if (fallback2) {
        return GetStringSettingCopy(fallback2);
    }
    return L"";
}



D2D1_COLOR_F ColorFromHex(std::wstring text, D2D1_COLOR_F fallback) {
    // Trim surrounding whitespace: a stray space used to make the whole value
    // fail to parse and silently fall back to the default color.
    const size_t firstChar = text.find_first_not_of(L" \t\r\n");
    if (firstChar == std::wstring::npos) {
        return fallback;
    }
    text = text.substr(firstChar, text.find_last_not_of(L" \t\r\n") - firstChar + 1);

    if (!text.empty() && text[0] == L'#') {
        text.erase(text.begin());
    }

    // Accepted forms: RGB, RGBA, RRGGBB, RRGGBBAA. The alpha-bearing forms are
    // how a translucent island background is specified independently of the
    // global pill transparency slider.
    const size_t digits = text.size();
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
        return fallback;
    }
    if (text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) {
        return fallback;
    }

    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return c - L'A' + 10;
    };

    int r = 0, g = 0, b = 0, a = 255;
    if (digits == 3 || digits == 4) {
        // Shorthand, each digit doubled so 'f' means 0xff.
        r = nibble(text[0]) * 17;
        g = nibble(text[1]) * 17;
        b = nibble(text[2]) * 17;
        if (digits == 4) {
            a = nibble(text[3]) * 17;
        }
    } else {
        r = nibble(text[0]) * 16 + nibble(text[1]);
        g = nibble(text[2]) * 16 + nibble(text[3]);
        b = nibble(text[4]) * 16 + nibble(text[5]);
        if (digits == 8) {
            a = nibble(text[6]) * 16 + nibble(text[7]);
        }
    }

    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static float HueToRgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

D2D1_COLOR_F HslToRgb(float h, float s, float l, float a = 1.0f) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = Clamp(s, 0.0f, 1.0f);
    l = Clamp(l, 0.0f, 1.0f);

    if (s <= 1e-5f) {
        return D2D1::ColorF(l, l, l, a);
    }

    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    const float hNorm = h / 360.0f;

    const float r = Clamp(HueToRgb(p, q, hNorm + 1.0f / 3.0f), 0.0f, 1.0f);
    const float g = Clamp(HueToRgb(p, q, hNorm), 0.0f, 1.0f);
    const float b = Clamp(HueToRgb(p, q, hNorm - 1.0f / 3.0f), 0.0f, 1.0f);

    return D2D1::ColorF(r, g, b, a);
}

void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    r = Clamp(r, 0.0f, 1.0f);
    g = Clamp(g, 0.0f, 1.0f);
    b = Clamp(b, 0.0f, 1.0f);

    const float maxVal = std::max({r, g, b});
    const float minVal = std::min({r, g, b});
    const float delta = maxVal - minVal;

    l = (maxVal + minVal) * 0.5f;

    if (delta <= 1e-5f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }

    s = (l > 0.5f) ? (delta / (2.0f - maxVal - minVal)) : (delta / (maxVal + minVal));

    if (maxVal == r) {
        h = ((g - b) / delta) + (g < b ? 6.0f : 0.0f);
    } else if (maxVal == g) {
        h = ((b - r) / delta) + 2.0f;
    } else {
        h = ((r - g) / delta) + 4.0f;
    }
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;
    if (h >= 360.0f) h -= 360.0f;
}

double RelativeLuminance(D2D1_COLOR_F c) {
    auto toLinear = [](float channel) -> double {
        const double v = Clamp(channel, 0.0f, 1.0f);
        return (v <= 0.04045) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * toLinear(c.r) + 0.7152 * toLinear(c.g) + 0.0722 * toLinear(c.b);
}

// Nudges an accent until it clears 3:1 against the surface it will sit on.
//
// This used to only ever *lighten*, which silently assumed a dark island. On a
// light background lightening reduces contrast, so the loop would run all the
// way to l = 0.95 and hand back a near-white accent that vanished into the
// surface. The step direction is now chosen from the background's luminance,
// which is what lets a light theme (Porcelain) keep a readable accent.
D2D1_COLOR_F EnsureContrastAgainstBackground(D2D1_COLOR_F candidate, D2D1_COLOR_F bgColor) {
    float h = 0.0f, s = 0.0f, l = 0.0f;
    RgbToHsl(candidate.r, candidate.g, candidate.b, h, s, l);

    if (s > 0.01f) {
        s = std::max(s, 0.35f);
    }

    const double bgLum = RelativeLuminance(bgColor);
    const bool onDark = bgLum < 0.45;

    // Clamp toward the half that can actually move away from the background.
    l = onDark ? std::min(l, 0.85f) : std::max(l, 0.15f);

    const float step = onDark ? 0.02f : -0.02f;
    const float limit = onDark ? 0.95f : 0.06f;

    candidate = HslToRgb(h, s, l, candidate.a);
    auto contrastOf = [&](const D2D1_COLOR_F& c) {
        const double lum = RelativeLuminance(c);
        return (std::max(bgLum, lum) + 0.05) / (std::min(bgLum, lum) + 0.05);
    };

    double contrast = contrastOf(candidate);
    // Bounded by `limit` in both directions, so this terminates either way.
    while (contrast < 3.0 && (onDark ? (l < limit) : (l > limit))) {
        l += step;
        candidate = HslToRgb(h, s, l, candidate.a);
        contrast = contrastOf(candidate);
    }

    return candidate;
}

// Returns true if the currently active foreground window is in full screen mode
bool IsForegroundFullscreen(HWND targetHwnd) {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == targetHwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) {
        return false;
    }

    if (!IsWindowVisible(fg)) return false;

    wchar_t className[256] = {};
    GetClassNameW(fg, className, 256);
    if (wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0) {
        return false;
    }

    RECT clientRect = {};
    if (!GetClientRect(fg, &clientRect)) return false;
    POINT pt = {0, 0};
    ClientToScreen(fg, &pt);
    clientRect.left += pt.x;
    clientRect.right += pt.x;
    clientRect.top += pt.y;
    clientRect.bottom += pt.y;

    HMONITOR hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (clientRect.left <= mi.rcMonitor.left &&
            clientRect.top <= mi.rcMonitor.top &&
            clientRect.right >= mi.rcMonitor.right &&
            clientRect.bottom >= mi.rcMonitor.bottom) {
            return true;
        }
    }

    return false;
}

struct MonitorEnumData {
    std::vector<HMONITOR> monitors;
};

inline BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) {
    auto* data = reinterpret_cast<MonitorEnumData*>(dwData);
    data->monitors.push_back(hMonitor);
    return TRUE;
}

inline HMONITOR GetAnchorMonitorFor(int targetMonitor) {
    HMONITOR selectedMonitor = nullptr;

    if (targetMonitor == -1) {
        POINT pt = {0, 0};
        GetCursorPos(&pt);
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else if (targetMonitor > 0) {
        MonitorEnumData data;
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));

        int index = targetMonitor - 1;
        if (index >= 0 && index < static_cast<int>(data.monitors.size())) {
            selectedMonitor = data.monitors[index];
        }
    }

    if (!selectedMonitor) {
        POINT pt = {0, 0};
        selectedMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return selectedMonitor;
}

inline HMONITOR GetAnchorMonitor() {
    return GetAnchorMonitorFor(g_settings.targetMonitor);
}

// Returns the DPI scale factor for a given monitor (1.0 = 96 DPI = 100%)
inline float GetMonitorDpiScale(HMONITOR monitor) {
    if (!monitor) {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpiForMonitor && monitor) {
        pGetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    }
    return static_cast<float>(dpiX) / 96.0f;
}


int GetMonitorRefreshRate(HWND hwnd) {
    HMONITOR monitor = nullptr;
    if (hwnd) {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT pt = {0, 0};
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            if (dm.dmDisplayFrequency > 1) {
                return static_cast<int>(dm.dmDisplayFrequency);
            }
        }
    }
    HDC hdc = GetDC(nullptr);
    int rate = GetDeviceCaps(hdc, VREFRESH);
    ReleaseDC(nullptr, hdc);
    return (rate > 1) ? rate : 60;
}

D2D1_COLOR_F GetSystemAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    using DwmGetColorizationColor_t = HRESULT(WINAPI*)(DWORD*, BOOL*);
    auto proc = reinterpret_cast<DwmGetColorizationColor_t>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetColorizationColor"));

    if (proc && SUCCEEDED(proc(&color, &opaque))) {
        return D2D1::ColorF(
            ((color >> 16) & 0xff) / 255.0f,
            ((color >> 8) & 0xff) / 255.0f,
            (color & 0xff) / 255.0f,
            1.0f);
    }

    return D2D1::ColorF(0x4cc9f0);
}

void ApplyHideShowHotkey();                 // forward declaration; defined after LoadSettings()

// Custom fork: defined in the TelegramBanner section, next to the notification
// listener. Turns the Windows banner for Telegram off (true) or restores it (false).
namespace TelegramBanner {
void Sync(bool suppressTelegram, bool suppressDiscord);
}

// Custom fork: shared parser for the "ctrl_alt"-style modifier options.
UINT ParseHotkeyModifiersOption(const std::wstring& value) {
    UINT mods = MOD_CONTROL | MOD_ALT;
    if (EqualsNoCase(value, L"ctrl_shift")) {
        mods = MOD_CONTROL | MOD_SHIFT;
    } else if (EqualsNoCase(value, L"alt_shift")) {
        mods = MOD_ALT | MOD_SHIFT;
    } else if (EqualsNoCase(value, L"win_alt")) {
        mods = MOD_WIN | MOD_ALT;
    } else if (EqualsNoCase(value, L"ctrl_alt_shift")) {
        mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    }
    return mods | MOD_NOREPEAT;
}

// Custom fork: single A-Z / 0-9 key, otherwise the fallback.
UINT ParseHotkeyLetterOption(const std::wstring& value, UINT fallback) {
    if (value.size() == 1) {
        const wchar_t ch = towupper(value[0]);
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            return static_cast<UINT>(ch);
        }
    }
    return fallback;
}

// Custom fork: "a.exe, B.EXE ,c" -> {"a.exe", "b.exe", "c.exe"}.
std::vector<std::wstring> ParseProcessListOption(const std::wstring& value) {
    std::vector<std::wstring> out;
    std::wstring item;
    auto flush = [&]() {
        size_t b = 0;
        size_t e = item.size();
        while (b < e && iswspace(item[b])) ++b;
        while (e > b && iswspace(item[e - 1])) --e;
        std::wstring name = item.substr(b, e - b);
        for (wchar_t& c : name) c = towlower(c);
        if (!name.empty()) {
            if (name.size() < 4 || name.compare(name.size() - 4, 4, L".exe") != 0) {
                name += L".exe";
            }
            out.push_back(std::move(name));
        }
        item.clear();
    };
    for (wchar_t c : value) {
        if (c == L',' || c == L';') {
            flush();
        } else {
            item.push_back(c);
        }
    }
    flush();
    return out;
}
void ApplyBackdropMaterial(HWND);           // forward declaration; defined after LoadSettings()


void LoadSettings() {
    Settings next;

    const std::wstring position = GetStringSettingCopy(L"Appearance.Position");
    if (EqualsNoCase(position, L"top-left")) {
        next.position = Position::TopLeft;
    } else if (EqualsNoCase(position, L"top-right")) {
        next.position = Position::TopRight;
    } else if (EqualsNoCase(position, L"bottom-center")) {
        next.position = Position::BottomCenter;
    } else if (EqualsNoCase(position, L"bottom-left")) {
        next.position = Position::BottomLeft;
    } else if (EqualsNoCase(position, L"bottom-right")) {
        next.position = Position::BottomRight;
    }

    const std::wstring scale = GetStringSettingCopy(L"Appearance.SizeScale");
    if (!scale.empty()) {
        wchar_t* end;
        float parsedScale = wcstof(scale.c_str(), &end);
        if (end != scale.c_str() && parsedScale > 0.1f && parsedScale < 10.0f) {
            next.sizeScale = parsedScale;
        }
    }
    next.baseSizeScale = next.sizeScale;

    std::wstring mon = GetStringSettingCopy(L"Appearance.TargetMonitor");
    if (mon == L"primary") next.targetMonitor = 0;
    else if (mon == L"follow") next.targetMonitor = -1;
    else next.targetMonitor = _wtoi(mon.c_str());

    // Auto DPI scaling: multiply sizeScale by monitor DPI factor for the target display.
    // On mixed-DPI setups or a 4K 200% display this scales the island to the right physical size.
    next.autoDpiScale = (di::settings::GetInt(L"Appearance.AutoDpiScale") != 0);
    HMONITOR anchorMon = GetAnchorMonitorFor(next.targetMonitor);
    const float dpi = next.autoDpiScale ? GetMonitorDpiScale(anchorMon) : 1.0f;
    next.sizeScale = next.baseSizeScale * dpi;

    std::wstring fontSetting = GetStringSettingWithFallback(L"Themes.FontFamily", L"Appearance.FontFamily");
    size_t firstChar = fontSetting.find_first_not_of(L" \t\r\n\"'");
    if (firstChar != std::wstring::npos) {
        size_t lastChar = fontSetting.find_last_not_of(L" \t\r\n\"'");
        next.fontFamily = fontSetting.substr(firstChar, lastChar - firstChar + 1);
    } else {
        next.fontFamily.clear();
    }

    const std::wstring accentMode = GetStringSettingCopy(L"Themes.AccentColorMode");
    if (EqualsNoCase(accentMode, L"system")) {
        next.accentMode = AccentMode::System;
    } else if (EqualsNoCase(accentMode, L"custom")) {
        next.accentMode = AccentMode::Custom;
    }

    next.customAccent = ColorFromHex(GetStringSettingCopy(L"Themes.CustomAccentHex"), next.customAccent);
    // Standalone 0.4: one of 10 accent presets. In Auto mode it is the colour
    // while nothing plays (with media the accent still comes from the artwork);
    // "custom" keeps Themes.CustomAccentHex.
    {
        static const struct { const wchar_t* id; UINT32 rgb; } kPresets[] = {
            {L"teal", 0x4CC9F0},   {L"blue", 0x0A84FF},  {L"violet", 0xBF5AF2}, {L"pink", 0xFF375F},
            {L"red", 0xFF453A},    {L"orange", 0xFF9F0A}, {L"yellow", 0xFFD60A}, {L"green", 0x30D158},
            {L"mint", 0x63E6BE},   {L"white", 0xF2F2F7},
        };
        const std::wstring preset = GetStringSettingCopy(L"Themes.AccentPreset");
        for (const auto& p : kPresets) {
            if (EqualsNoCase(preset, p.id)) {
                next.customAccent = D2D1::ColorF(p.rgb);
                break;
            }
        }
    }

    const std::wstring calAccent = GetStringSettingWithFallback(L"Themes.CalendarAccent", L"CalendarWeather.CalendarAccent", L"Modules.CalendarAccent");
    if (EqualsNoCase(calAccent, L"system")) {
        next.calendarAccent = CalendarAccentMode::System;
    } else {
        next.calendarAccent = CalendarAccentMode::Accent;
    }

    const std::wstring fpsStr = GetStringSettingWithFallback(L"Animations.TargetFPS", L"Appearance.TargetFPS");
    if (EqualsNoCase(fpsStr, L"auto") || fpsStr.empty()) {
        next.targetFps = 0;
    } else {
        next.targetFps = _wtoi(fpsStr.c_str());
        if (next.targetFps < 0) next.targetFps = 0;
    }

    const std::wstring styleStr = GetStringSettingWithFallback(L"Animations.AnimationStyle", L"Appearance.AnimationStyle");
    if (EqualsNoCase(styleStr, L"smooth")) {
        next.animationStyle = AnimationStyle::Smooth;
    } else if (EqualsNoCase(styleStr, L"bouncy")) {
        next.animationStyle = AnimationStyle::Bouncy;
    } else if (EqualsNoCase(styleStr, L"snappy")) {
        next.animationStyle = AnimationStyle::Snappy;
    } else {
        next.animationStyle = AnimationStyle::Default;
    }

    const std::wstring progressStr = GetStringSettingCopy(L"Appearance.ProgressStyle");
    if (EqualsNoCase(progressStr, L"wavy")) {
        next.progressStyle = ProgressStyle::Wavy;
    } else if (EqualsNoCase(progressStr, L"squiggle")) {
        next.progressStyle = ProgressStyle::Squiggle;
    } else if (EqualsNoCase(progressStr, L"bar")) {
        next.progressStyle = ProgressStyle::Bar;
    } else {
        next.progressStyle = ProgressStyle::Slim;  // default
    }

    const std::wstring spectrumStr = GetStringSettingCopy(L"Appearance.SpectrumStyle");
    if (EqualsNoCase(spectrumStr, L"orb")) {
        next.spectrumStyle = SpectrumStyle::Orb;
    } else if (EqualsNoCase(spectrumStr, L"plasma")) {
        next.spectrumStyle = SpectrumStyle::Plasma;
    } else if (EqualsNoCase(spectrumStr, L"led")) {
        next.spectrumStyle = SpectrumStyle::Led;
    } else {
        next.spectrumStyle = SpectrumStyle::Bars;  // default
    }
    g_spectrumBandsNeeded.store(next.spectrumStyle == SpectrumStyle::Orb ||
                                    next.spectrumStyle == SpectrumStyle::Plasma,
                                std::memory_order_relaxed);

    const std::wstring speed = GetStringSettingWithFallback(L"Animations.AnimationSpeed", L"Appearance.AnimationSpeed", L"Behavior.AnimationSpeed");
    if (EqualsNoCase(speed, L"very-slow")) {
        next.animationSpeed = 0.5f;
    } else if (EqualsNoCase(speed, L"slow")) {
        next.animationSpeed = 0.75f;
    } else if (EqualsNoCase(speed, L"fast")) {
        next.animationSpeed = 1.35f;
    } else if (EqualsNoCase(speed, L"very-fast")) {
        next.animationSpeed = 1.65f;
    } else if (EqualsNoCase(speed, L"ultra-fast")) {
        next.animationSpeed = 2.0f;
    } else {
        next.animationSpeed = 1.0f;
    }

    next.media = di::settings::GetInt(L"Modules.Media") != 0;
    next.mediaAutoExpand = di::settings::GetInt(L"Modules.MediaAutoExpand") != 0;
    next.volume = di::settings::GetInt(L"Modules.Volume") != 0;
    if (!next.volume) {
        std::lock_guard lock(g_stateMutex);
        g_state.volume.active = false;
    }
    next.brightness = di::settings::GetInt(L"Modules.Brightness") != 0;
    if (!next.brightness) {
        std::lock_guard lock(g_stateMutex);
        g_state.brightness.active = false;
        g_brightnessInitialized = false;
    }

    next.battery = di::settings::GetInt(L"Modules.Battery") != 0;
    next.progress = di::settings::GetInt(L"Modules.Progress") != 0;
    next.timerEnabled = di::settings::GetInt(L"Modules.TimerModule") != 0;

    next.hideShowHotkeyEnabled = di::settings::GetInt(L"Shortcuts.HideShowHotkeyEnabled") != 0;

    const std::wstring hotkeyModStr = GetStringSettingCopy(L"Shortcuts.HideShowModifiers");
    if (EqualsNoCase(hotkeyModStr, L"ctrl_shift")) {
        next.hideShowModifiers = MOD_CONTROL | MOD_SHIFT;
    } else if (EqualsNoCase(hotkeyModStr, L"alt_shift")) {
        next.hideShowModifiers = MOD_ALT | MOD_SHIFT;
    } else if (EqualsNoCase(hotkeyModStr, L"win_alt")) {
        next.hideShowModifiers = MOD_WIN | MOD_ALT;
    } else if (EqualsNoCase(hotkeyModStr, L"ctrl_alt_shift")) {
        next.hideShowModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    } else {
        next.hideShowModifiers = MOD_CONTROL | MOD_ALT;
    }
    next.hideShowModifiers |= MOD_NOREPEAT;

    // Single A-Z/0-9 key only; anything else (blank, multi-char, symbol) falls back to 'D'.
    const std::wstring hotkeyKeyStr = GetStringSettingCopy(L"Shortcuts.HideShowKey");
    next.hideShowVk = 'D';
    if (hotkeyKeyStr.size() == 1) {
        const wchar_t ch = towupper(hotkeyKeyStr[0]);
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            next.hideShowVk = static_cast<UINT>(ch);
        }
    }

    // ── Custom fork: Telegram / game mode / island Do Not Disturb ───────────
    next.telegramEnabled = di::settings::GetInt(L"Telegram.Enabled") != 0;
    next.telegramSuppressBanner = di::settings::GetInt(L"Telegram.SuppressWindowsBanner") != 0;
    next.telegramDisplaySeconds = ClampInt(di::settings::GetInt(L"Telegram.DisplaySeconds"), 2, 60);
    next.telegramMaxMessages = ClampInt(di::settings::GetInt(L"Telegram.MaxMessages"), 1, 5);
    next.telegramShowText = di::settings::GetInt(L"Telegram.ShowMessageText") != 0;
    next.telegramRespectWindowsDnd = di::settings::GetInt(L"Telegram.RespectWindowsDnD") != 0;
    next.telegramClearFromCenter = di::settings::GetInt(L"Telegram.ClearFromNotificationCenter") != 0;
    next.telegramDebugLog = di::settings::GetInt(L"Telegram.DebugLog") != 0;
    next.discordEnabled = di::settings::GetInt(L"Discord.Enabled") != 0;
    next.discordShowOverGames = di::settings::GetInt(L"Discord.ShowOverGames") != 0;
    next.discordSuppressBanner = di::settings::GetInt(L"Discord.SuppressWindowsBanner") != 0;

    next.smartGameMode = di::settings::GetInt(L"GameMode.SmartGameMode") != 0;
    next.gameShowOtherAlerts = di::settings::GetInt(L"GameMode.ShowOtherAlertsInGame") != 0;
    next.gameDetectD3D = di::settings::GetInt(L"GameMode.DetectD3DFullscreen") != 0;
    next.gameProcesses = ParseProcessListOption(GetStringSettingCopy(L"GameMode.GameProcesses"));
    next.gameLightweight = di::settings::GetInt(L"GameMode.LightweightOverlay") != 0;

    next.quietButton = di::settings::GetInt(L"QuietMode.ShowButton") != 0;
    next.quietHotkeyEnabled = di::settings::GetInt(L"QuietMode.HotkeyEnabled") != 0;
    next.quietModifiers = ParseHotkeyModifiersOption(GetStringSettingCopy(L"QuietMode.HotkeyModifiers"));
    next.quietVk = ParseHotkeyLetterOption(GetStringSettingCopy(L"QuietMode.HotkeyKey"), 'N');

    next.bluetoothIndicator = di::settings::GetInt(L"Modules.BluetoothIndicator") != 0;
    next.bluetoothShowBattery = di::settings::GetInt(L"Modules.BluetoothShowBattery") != 0;
    next.bluetoothDashboard = di::settings::GetInt(L"Modules.BluetoothDashboard") != 0;
    next.sonyPadBattery = di::settings::GetInt(L"Modules.SonyPadBattery") != 0;

    // ── Premium material / redesign ──────────────────────────────────────────
    {
        const std::wstring backdrop = GetStringSettingCopy(L"Themes.BackdropMaterial");
        if (EqualsNoCase(backdrop, L"liquidglass")) {
            next.backdropMaterial = BackdropMaterial::LiquidGlass;
        } else if (EqualsNoCase(backdrop, L"acrylic")) {
            next.backdropMaterial = BackdropMaterial::Acrylic;
        } else if (EqualsNoCase(backdrop, L"blur")) {
            next.backdropMaterial = BackdropMaterial::Blur;
        } else {
            next.backdropMaterial = BackdropMaterial::None;
        }
    }
    next.backdropTint = Clamp(di::settings::GetInt(L"Themes.BackdropTint") / 100.0f, 0.0f, 1.0f);
    next.backdropFillAlpha = Clamp(di::settings::GetInt(L"Themes.BackdropFillOpacity") / 100.0f, 0.0f, 1.0f);
    // Custom fork (custom.17): Liquid Glass.
    next.liquidGlass = next.backdropMaterial == BackdropMaterial::LiquidGlass;
    next.liquidGlassClear = EqualsNoCase(GetStringSettingCopy(L"Themes.LiquidGlassStyle"), L"clear");
    next.liquidGlassClarity = Clamp(di::settings::GetInt(L"Themes.LiquidGlassClarity") / 100.0f, 0.0f, 1.0f);
    next.liquidGlassShine = Clamp(di::settings::GetInt(L"Themes.LiquidGlassShine") / 100.0f, 0.0f, 2.0f);

    next.materialDepth = di::settings::GetInt(L"Themes.MaterialDepth") != 0;
    next.dropShadow = di::settings::GetInt(L"Themes.DropShadow") != 0;
    next.accentBloom = Clamp(di::settings::GetInt(L"Themes.AccentBloom") / 100.0f, 0.0f, 2.0f);
    next.textScale = Clamp(di::settings::GetInt(L"Themes.TextScale") / 100.0f, 0.7f, 1.6f);

    // ── Clock / date presentation (#61) ──────────────────────────────────────
    next.showSeconds = di::settings::GetInt(L"Modules.ShowSeconds") != 0;
    const std::wstring clockMode = GetStringSettingCopy(L"Modules.ClockFormat");
    next.clockFollowSystem = clockMode.empty() || EqualsNoCase(clockMode, L"system");
    next.use24HourClock = EqualsNoCase(clockMode, L"24h");
    next.dateFormat = GetStringSettingCopy(L"Modules.DateFormat");
    next.dateFirst = di::settings::GetInt(L"Modules.DateFirst") != 0;
    const std::wstring firstDay = GetStringSettingCopy(L"Modules.FirstDayOfWeek");
    if (EqualsNoCase(firstDay, L"monday")) {
        next.firstDayOfWeek = 1;
    } else if (EqualsNoCase(firstDay, L"sunday")) {
        next.firstDayOfWeek = 0;
    } else {
        next.firstDayOfWeek = -1;
    }
    next.mediaPillClock = di::settings::GetInt(L"Modules.MediaPillClock") != 0;

    // ── Localization (#35) ───────────────────────────────────────────────────
    {
        std::wstring langTag = GetStringSettingCopy(L"Modules.Language");
        if (langTag.empty()) {
            langTag = L"auto";
        }
        next.language = langTag;
        const UiLanguage resolved = EqualsNoCase(langTag, L"auto") ? DetectSystemLanguage()
                                                                  : LanguageFromTag(langTag);
        g_uiLanguage.store(static_cast<int>(resolved), std::memory_order_relaxed);
    }

    // ── Media auto-expand exclusions (#62) ───────────────────────────────────
    next.mediaExpandBlocklist.clear();
    {
        const std::wstring raw = GetStringSettingCopy(L"Modules.MediaExpandBlocklist");
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t comma = raw.find(L',', start);
            const size_t end = (comma == std::wstring::npos) ? raw.size() : comma;
            std::wstring token = raw.substr(start, end - start);
            const size_t a = token.find_first_not_of(L" \t\r\n");
            if (a != std::wstring::npos) {
                const size_t b = token.find_last_not_of(L" \t\r\n");
                std::wstring entry = token.substr(a, b - a + 1);
                // Stored lowercase so matching against media source/title is
                // case-insensitive without re-lowering on every frame.
                std::transform(entry.begin(), entry.end(), entry.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                next.mediaExpandBlocklist.push_back(std::move(entry));
            }
            if (comma == std::wstring::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    // ── Game overlay options (#25) ───────────────────────────────────────────
    next.gameOverlayShowFps = di::settings::GetInt(L"Modules.GameOverlayShowFps") != 0;
    next.gameOverlayShowCpu = di::settings::GetInt(L"Modules.GameOverlayShowCpu") != 0;
    next.gameOverlayShowGpu = di::settings::GetInt(L"Modules.GameOverlayShowGpu") != 0;
    next.gameOverlayShowRam = di::settings::GetInt(L"Modules.GameOverlayShowRam") != 0;
    next.gameOverlayShowDisk = di::settings::GetInt(L"Modules.GameOverlayShowDisk") != 0;
    next.gameOverlayCompact = di::settings::GetInt(L"Modules.GameOverlayCompact") != 0;
    next.tintOpacity = Clamp(di::settings::GetInt(L"Themes.TintIntensity") / 100.0f, 0.0f, 1.0f);
    const int settingOpacity = di::settings::GetInt(L"Themes.PillOpacity");
    g_pinnedExpanded.store(di::state::GetInt(L"PinnedExpanded", 0) != 0);
    next.pillOpacity = Clamp(settingOpacity / 100.0f, 0.35f, 1.0f);
    next.gameOverlay = di::settings::GetInt(L"Modules.GameOverlay") != 0;
    next.showMetricText = di::settings::GetInt(L"Modules.ShowMetricText") != 0;
    next.weather = di::settings::GetInt(L"Modules.Weather") != 0;
    next.weatherCity = GetStringSettingWithFallback(L"Modules.WeatherCity", L"Weather.WeatherCity", L"CalendarWeather.WeatherCity");
    next.weatherFahrenheit = di::settings::GetInt(L"Modules.WeatherFahrenheit") != 0;
    const std::wstring hideSec = GetStringSettingWithFallback(L"Behavior.AutoHideIdleSeconds", L"Appearance.AutoHideIdleSeconds");
    next.autoHideIdleSeconds = hideSec.empty() ? 0 : _wtoi(hideSec.c_str());
    next.unhideOnHover = di::settings::GetInt(L"Behavior.UnhideOnHover") != 0;
    next.alwaysOnTop = di::settings::GetInt(L"Behavior.AlwaysOnTop") != 0;
    next.expandOnHover = di::settings::GetInt(L"Behavior.ExpandOnHover") != 0;
    next.autoDpiScale = di::settings::GetInt(L"Appearance.AutoDpiScale") != 0;
    next.offsetX = di::settings::GetInt(L"Appearance.OffsetX");
    next.offsetY = di::settings::GetInt(L"Appearance.OffsetY");
    next.separateExpandedOffsetY = di::settings::GetInt(L"Appearance.SeparateExpandedOffsetY") != 0;
    next.offsetYExpanded = di::settings::GetInt(L"Appearance.OffsetYExpanded");


    const std::wstring shapeStr = GetStringSettingCopy(L"Appearance.ShapeStyle");
    next.w11Style = EqualsNoCase(shapeStr, L"w11");
    next.notchStyle = EqualsNoCase(shapeStr, L"notch");

    // A bottom-anchored island cannot be a top-edge macOS notch.
    if (IsBottomPosition(next.position)) {
        next.notchStyle = false;
    }

    // Color settings — check local theme override first, then settings YAML.
    // Palettes live in kThemePalettes at file scope so other code
    // resolves the same colors and labels this does.
    //
    // A custom hex field counts as an intentional override once it differs from
    // the value shipped as its default. Presets keep working untouched for
    // anyone who never edits these fields, but an edited hex now takes effect
    // immediately instead of being silently discarded unless the Theme preset
    // also happened to be switched to Custom. Clearing the field back to its
    // default hands control back to the preset.
    //
    // `defaultHexes` deliberately lists the *historical* defaults as well as the
    // current one. Retiring the old palettes moved these defaults, and Windhawk
    // keeps whatever value is already stored in a user's config -- so someone who
    // never touched the hex fields would still be carrying "#0D0D0F" from the old
    // OLED Black default. Matching only the current default would read that as a
    // deliberate override and pin every new palette back to the old background.
    auto resolveColor = [](const wchar_t* settingKey,
                           std::initializer_list<const wchar_t*> defaultHexes,
                           D2D1_COLOR_F presetColor) -> D2D1_COLOR_F {
        const std::wstring value = GetStringSettingCopy(settingKey);
        if (value.empty()) {
            return presetColor;
        }

        // Compare the parsed colors rather than the strings, so equivalent
        // spellings of the default ("0D0D0F" without the '#', "#0D0D0FFF",
        // different case) are all still recognised as "untouched" and leave the
        // preset in charge.
        const D2D1_COLOR_F sentinel = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        const D2D1_COLOR_F parsed = ColorFromHex(value, sentinel);
        if (parsed.r == sentinel.r && parsed.g == sentinel.g &&
            parsed.b == sentinel.b && parsed.a == sentinel.a) {
            // Unparseable, so treat it as not set.
            return presetColor;
        }

        for (const wchar_t* defaultHex : defaultHexes) {
            const D2D1_COLOR_F defaults = ColorFromHex(defaultHex, sentinel);
            if (parsed.r == defaults.r && parsed.g == defaults.g &&
                parsed.b == defaults.b && parsed.a == defaults.a) {
                return presetColor;
            }
        }
        return parsed;
    };

    auto applyPreset = [&](Settings& target, int idx) {
        const ThemePalette& p = kThemePalettes[idx];
        target.pillBgColor = resolveColor(
            L"Themes.PillBgColor", {kThemePalettes[0].bg, L"#0D0D0F"},
            ColorFromHex(p.bg, D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f)));
        target.textPrimaryColor = resolveColor(
            L"Themes.TextPrimaryColor", {L"#FFFFFF", L"#F7F7F7"},
            ColorFromHex(p.fg, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)));
        target.textSecondaryColor = resolveColor(
            L"Themes.TextSecondaryColor", {kThemePalettes[0].sec, L"#B0B0B8", L"#888888"},
            ColorFromHex(p.sec, D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f)));
        target.contourBorderColor = resolveColor(
            L"Themes.ContourBorderHex", {kThemePalettes[0].border, L"#333338"},
            ColorFromHex(p.border, D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f)));
    };

    std::wstring themePresetStr = GetStringSettingCopy(L"Themes.ThemePreset");
    // Standalone 0.5: the palette comes from settings only (the right-click
    // menu that could override it is gone).
    const int theme = ThemeIndexFromId(themePresetStr);
    if (theme >= 0 && theme < kCustomThemeIndex) {
        next.themePreset = static_cast<ThemePreset>(theme);
        applyPreset(next, theme);
    } else if (theme >= kCustomThemeIndex || EqualsNoCase(themePresetStr, L"custom")) {
        // Explicit Custom: the hex fields are authoritative, defaults included.
        next.themePreset = ThemePreset::Custom;
        next.pillBgColor = ColorFromHex(GetStringSettingCopy(L"Themes.PillBgColor"),
                                        D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f));
        next.textPrimaryColor = ColorFromHex(GetStringSettingCopy(L"Themes.TextPrimaryColor"),
                                             D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        next.textSecondaryColor = ColorFromHex(GetStringSettingCopy(L"Themes.TextSecondaryColor"),
                                               D2D1::ColorF(0.608f, 0.608f, 0.647f, 1.0f));
        next.contourBorderColor = ColorFromHex(GetStringSettingCopy(L"Themes.ContourBorderHex"),
                                               D2D1::ColorF(0.118f, 0.118f, 0.133f, 1.0f));
    } else {
        const int fromSettings = ThemeIndexFromId(themePresetStr);
        const int presetIdx = (fromSettings >= 0 && fromSettings < kCustomThemeIndex) ? fromSettings : 0;
        next.themePreset = static_cast<ThemePreset>(presetIdx);
        applyPreset(next, presetIdx);
    }

    // Graphite inherits the old Fluent theme's auto-translucency: it is the
    // neutral Windows 11 grey, and it reads best when the desktop shows through
    // a little. The rest of the palettes are opaque unless the user says so.
    if (next.themePreset == ThemePreset::Graphite) {
        next.pillOpacity = 0.88f;
    }

    next.borderMergedMode = di::settings::GetInt(L"Appearance.BorderMergedMode") != 0;
    next.autoHideFullscreen = di::settings::GetInt(L"Behavior.AutoHideFullscreen") != 0;
    next.hardwareMonitorModule = di::settings::GetInt(L"Modules.HardwareMonitorModule") != 0;
    // Custom fork (custom.18): stocks tab.
    next.stocksModule = di::settings::GetInt(L"Stocks.Enabled") != 0;
    for (int i = 0; i < kStockSlots; ++i) {
        wchar_t key[32];
        swprintf_s(key, L"Stocks.Stock%d", i + 1);
        const StockInfo* st = FindStock(GetStringSettingCopy(key));
        next.stockSymbols[i] = st ? st->symbol : kDefaultStocks[i];
    }
    {
        const std::wstring range = GetStringSettingCopy(L"Stocks.Range");
        next.stocksRange = EqualsNoCase(range, L"5d")    ? StocksRange::FiveDays
                           : EqualsNoCase(range, L"1mo") ? StocksRange::Month
                           : EqualsNoCase(range, L"6mo") ? StocksRange::SixMonths
                           : EqualsNoCase(range, L"1y")  ? StocksRange::Year
                                                         : StocksRange::Day;
    }
    next.stocksRefreshMinutes = ClampInt(di::settings::GetInt(L"Stocks.RefreshMinutes"), 1, 60);
    const std::wstring borderModeStr = GetStringSettingCopy(L"Themes.ContourBorderMode");
    if (EqualsNoCase(borderModeStr, L"borderless")) {
        next.contourBorderMode = ContourBorderMode::Borderless;
        next.contourBorderEnabled = false;
    } else if (EqualsNoCase(borderModeStr, L"auto")) {
        next.contourBorderMode = ContourBorderMode::Auto;
        next.contourBorderEnabled = true;
    } else if (EqualsNoCase(borderModeStr, L"default")) {
        next.contourBorderMode = ContourBorderMode::Default;
        next.contourBorderEnabled = true;
    } else {
        next.contourBorderEnabled = true;  // no such setting; the mode dropdown drives this
        next.contourBorderMode = next.contourBorderEnabled ? ContourBorderMode::Default : ContourBorderMode::Borderless;
    }
    next.clockAccentGlow = di::settings::GetInt(L"Themes.ClockAccentGlow") != 0;
    bool settingsChangedWhileHidden = (g_autoHiddenParked.load() || g_manuallyHidden.load());
    bool unhideRequested = (next.autoHideIdleSeconds == 0) ||
                           (next.unhideOnHover && !g_settings.unhideOnHover);
    if (unhideRequested || (settingsChangedWhileHidden && next.autoHideIdleSeconds == 0)) {
        g_autoHiddenParked = false;
        g_manuallyHidden = false;
        di::state::SetInt(L"ManuallyHidden", 0);
        g_hotkeyUnhideUntil.store(NowSeconds() + (next.autoHideIdleSeconds > 0 ? next.autoHideIdleSeconds : 6.0));
        if (g_hwnd) {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, WM_APP_NEW_EVENT, 0, 0);
        }
    } else if (settingsChangedWhileHidden && next.unhideOnHover) {
        g_autoHiddenParked = false;
        g_manuallyHidden = false;
        di::state::SetInt(L"ManuallyHidden", 0);
        g_hotkeyUnhideUntil.store(NowSeconds() + (next.autoHideIdleSeconds > 0 ? next.autoHideIdleSeconds : 6.0));
        if (g_hwnd) {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            PostMessageW(g_hwnd, WM_APP_NEW_EVENT, 0, 0);
        }
    }

    // Privacy Indicators: unified reading with complete fallback to legacy keys
    next.privacyDots = di::settings::GetInt(L"Indicators.PrivacyDots") != 0;
    next.privacyDotsMic = di::settings::GetInt(L"Indicators.PrivacyDotsMic") != 0;
    next.privacyDotsCam = di::settings::GetInt(L"Indicators.PrivacyDotsCam") != 0;
    next.privacyDotsPulse = true;  // no such setting; pulsing is always on

    std::wstring micHexStr = GetStringSettingWithFallback(L"Indicators.PrivacyDotsMicHex", L"Indicators.MicDotHex", L"Modules.PrivacyDotsMicHex");
    next.privacyDotsMicHex = ColorFromHex(micHexStr, D2D1::ColorF(1.0f, 0.584f, 0.0f, 1.0f));

    std::wstring camHexStr = GetStringSettingWithFallback(L"Indicators.PrivacyDotsCamHex", L"Indicators.CamDotHex", L"Modules.PrivacyDotsCamHex");
    next.privacyDotsCamHex = ColorFromHex(camHexStr, D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f));
    next.privacyDotsBothHex = ColorFromHex(GetStringSettingCopy(L"Indicators.PrivacyDotsBothHex"), D2D1::ColorF(0xBF5AF2));

    // Compatibility aliases: always keep in sync
    next.privacyDotsEnabled = next.privacyDots;
    next.privacyDotPulsing = next.privacyDotsPulse;
    next.micDotColor = next.privacyDotsMicHex;
    next.camDotColor = next.privacyDotsCamHex;

    g_pinnedExpanded.store(false);
    di::state::SetInt(L"PinnedExpanded", 0);

    // Compare-then-publish under one lock so the change flags describe exactly
    // the transition we are about to commit. Two LoadSettings() calls can run
    // concurrently (settings thread vs. tray menu on the render thread); without
    // this, both could read the same "old" values and each decide a hotkey
    // re-registration was needed, or neither would.
    bool cityChanged = false;
    bool stocksChanged = false;  // custom fork (custom.18)
    bool hotkeySettingChanged = false;
    bool backdropChanged = false;
    {
        std::lock_guard lock(g_settingsMutex);
        cityChanged = next.weatherCity != g_settings.weatherCity;
        stocksChanged = next.stocksModule != g_settings.stocksModule ||
                        next.stockSymbols != g_settings.stockSymbols ||
                        next.stocksRange != g_settings.stocksRange;
        hotkeySettingChanged =
            next.hideShowHotkeyEnabled != g_settings.hideShowHotkeyEnabled ||
            next.hideShowModifiers != g_settings.hideShowModifiers ||
            next.hideShowVk != g_settings.hideShowVk ||
            next.quietHotkeyEnabled != g_settings.quietHotkeyEnabled ||
            next.quietModifiers != g_settings.quietModifiers ||
            next.quietVk != g_settings.quietVk;
        backdropChanged =
            next.backdropMaterial != g_settings.backdropMaterial ||
            std::fabs(next.backdropTint - g_settings.backdropTint) > 0.001f ||
            std::fabs(next.backdropFillAlpha - g_settings.backdropFillAlpha) > 0.001f ||
            next.liquidGlassClear != g_settings.liquidGlassClear ||
            next.alwaysOnTop != g_settings.alwaysOnTop;
        g_settings = std::move(next);
    }
    g_layoutDirty = true;
    if (cityChanged && g_settingsChangedEvent) {
        SetEvent(g_settingsChangedEvent);
    }
    if (stocksChanged && g_stocksWakeEvent) {  // custom fork (custom.18)
        SetEvent(g_stocksWakeEvent);
    }
    // g_hwnd only exists once RenderThreadProc has created the overlay window;
    // the very first LoadSettings() call (at mod init, before StartThreads())
    // runs with g_hwnd still null, so ApplyHideShowHotkey() no-ops there and
    // RenderThreadProc does the initial registration itself right after
    // CreateWindowExW. Every later call (e.g. from the settings watcher)
    // re-registers live so hotkey edits apply without a mod restart.
    // Posted, not called: UnregisterHotKey / RegisterHotKey fail with
    // ERROR_WINDOW_OF_OTHER_THREAD from this thread, which left the old
    // combination registered, the new one never registered, and the bookkeeping
    // flag claiming otherwise -- so switching the hotkey off never released it.
    if (hotkeySettingChanged) {
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_APP_APPLY_HOTKEY, 0, 0);
        }
    }
    // Same story for the backdrop: no-ops before the window exists, and applies
    // live afterwards so switching blur/acrylic needs no mod restart.
    if (backdropChanged && g_hwnd) {
        PostMessageW(g_hwnd, WM_APP_APPLY_BACKDROP, 0, 0);
    }

    // Custom fork: keep the Telegram banner setting in Windows in step with the
    // mod settings. Only acts on a change (and once at start-up).
    {
        static int s_lastBannerSuppress = -1;
        const Settings applied = GetSettingsCopy();
        const bool wantTelegram = applied.telegramEnabled && applied.telegramSuppressBanner;
        const bool wantDiscord = applied.discordEnabled && applied.discordSuppressBanner;
        const int want = (wantTelegram ? 1 : 0) | (wantDiscord ? 2 : 0);
        if (want != s_lastBannerSuppress) {
            s_lastBannerSuppress = want;
            TelegramBanner::Sync(wantTelegram, wantDiscord);
        }
    }
}

void EnableBlurBehind(HWND hwnd) {
    DWM_BLURBEHIND blur = {};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = FALSE;
    DwmEnableBlurBehindWindow(hwnd, &blur);
}

// ── Backdrop material (#59) ──────────────────────────────────────────────────
// The blur / acrylic is composed by DWM (see DwmBackdrop); this namespace only holds
// the screen-clip helpers the island yields to.
namespace Backdrop {

// True while the foreground window belongs to the Snipping Tool / screen clip UI.
bool ScreenClipUiActive() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool hit = false;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        hit = _wcsicmp(name, L"SnippingTool.exe") == 0 ||
              _wcsicmp(name, L"ScreenClippingHost.exe") == 0;
    }
    CloseHandle(h);
    return hit;
}

// While the Snipping Tool overlay is foreground the island must not raise or
// show itself. Cached for 100 ms since this is checked every frame.
bool ClipUiActiveCached() {
    static std::atomic<ULONGLONG> lastCheck{0};
    static std::atomic<bool> last{false};
    const ULONGLONG now = GetTickCount64();
    if (now - lastCheck.load() >= 100) {
        last.store(ScreenClipUiActive());
        lastCheck.store(now);
    }
    return last.load();
}

}  // namespace Backdrop

// ── DWM backdrop (#59) ───────────────────────────────────────────────────────
// Real Windows blur / acrylic, composed by DWM on the GPU: the island never blurs
// the desktop itself. The accent only applies to a non-layered window, so each pill
// gets a mouse-transparent window just below the layered island, clipped to the
// pill's rounded region with accent flag 0x10 so it does not spill past the corners.
namespace DwmBackdrop {

struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor;  // ABGR: alpha = tint opacity
    DWORD animationId;
};

struct CompositionAttribData {
    int attrib;
    PVOID data;
    SIZE_T size;
};

using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, CompositionAttribData*);

constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableBlurBehind = 3;
constexpr int kAccentEnableAcrylicBlurBehind = 4;
constexpr int kWcaAccentPolicy = 19;
constexpr int kWcaTransitionsForcedisabled = 3;
constexpr int kMaxPills = 2;  // split / combined layouts still use one silhouette each
constexpr wchar_t kClassName[] = L"DynamicIslandBackdrop";

struct Slot {
    HWND hwnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0;  // last placed rect (screen px)
    int radius = -1;                 // last region corner radius
    int lift = -1;                   // last region top lift (notch: flat top)
    bool visible = false;
    bool topMost = false;
    int accentState = -1;
    DWORD accentColor = 0;
};

Slot g_slots[kMaxPills];
SetWindowCompositionAttribute_t g_setWca = nullptr;
bool g_resolved = false;
bool g_supported = false;  // API present and Windows 10 1803 or newer
bool g_classRegistered = false;

// Last Sync signature so idle presents that redraw content skip SetWindowPos work.
struct SyncFingerprint {
    HWND island = nullptr;
    POINT origin = {0, 0};
    int count = 0;
    int material = -1;
    DWORD color = 0;
    int state = -1;
    bool notch = false;
    int x[kMaxPills] = {};
    int y[kMaxPills] = {};
    int w[kMaxPills] = {};
    int h[kMaxPills] = {};
    int radius[kMaxPills] = {};
    int lift[kMaxPills] = {};
};
SyncFingerprint g_lastSync = {};

bool WindowsBuildAtLeast(DWORD build) {
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersion_t>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    if (!fn) return false;
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 && vi.dwBuildNumber >= build;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // belt-and-braces with WS_EX_TRANSPARENT
        case WM_ERASEBKGND:
            return 1;  // accent owns the pixels; avoid GDI flash on show/resize
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Resolve() {
    if (g_resolved) return;
    g_resolved = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_setWca = user32 ? reinterpret_cast<SetWindowCompositionAttribute_t>(
                            GetProcAddress(user32, "SetWindowCompositionAttribute"))
                      : nullptr;
    g_supported = g_setWca && WindowsBuildAtLeast(17134);  // acrylic: 1803+
}

// False only on Windows too old for the accent; the island then keeps its solid fill.
bool Available() {
    Resolve();
    return g_supported;
}

DWORD ColorToAccentAbgr(const D2D1_COLOR_F& tint, float tintAlpha) {
    auto to8 = [](float v) {
        return static_cast<DWORD>(std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255));
    };
    DWORD alpha = to8(tintAlpha);
    if (alpha == 0) alpha = 1;  // acrylic rejects a fully transparent tint
    return (alpha << 24) | (to8(tint.b) << 16) | (to8(tint.g) << 8) | to8(tint.r);
}

void DisableTransitions(HWND hwnd) {
    BOOL disable = TRUE;
    CompositionAttribData data = {kWcaTransitionsForcedisabled, &disable, sizeof(disable)};
    g_setWca(hwnd, &data);
}

void ApplyAccent(Slot& s, int state, DWORD color) {
    if (!s.hwnd || !g_setWca) return;
    if (s.accentState == state && s.accentColor == color) return;
    AccentPolicy policy = {};
    policy.accentState = state;
    policy.accentFlags = 0x10;  // clip the accent to the window region
    policy.gradientColor = color;
    CompositionAttribData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
    g_setWca(s.hwnd, &data);
    s.accentState = state;
    s.accentColor = color;
}

void ClearAccent(Slot& s) {
    if (!s.hwnd || !g_setWca) {
        s.accentState = -1;
        s.accentColor = 0;
        return;
    }
    if (s.accentState == kAccentDisabled) return;
    AccentPolicy policy = {};
    policy.accentState = kAccentDisabled;
    CompositionAttribData data = {kWcaAccentPolicy, &policy, sizeof(policy)};
    g_setWca(s.hwnd, &data);
    s.accentState = kAccentDisabled;
    s.accentColor = 0;
}

HWND CreateBackdropWindow(HWND owner) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!g_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // black = clear for accent
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
        g_classRegistered = true;
    }
    // No WS_EX_LAYERED (it would kill the accent); WS_EX_TRANSPARENT passes the mouse.
    // Unowned (hWndParent = nullptr): In Win32, an owned popup window is strictly forced
    // above its owner in the z-order upon owner activation. If owned by the island, calling
    // SetForegroundWindow(island) for the right-click context menu would
    // bring the backdrop window in front of the island, completely covering the island's
    // content (text, icons, album art) with blur/acrylic. Unowned popups stay behind the island.
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    if (g_settings.alwaysOnTop) exStyle |= WS_EX_TOPMOST;
    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                                inst, nullptr);
    if (!hwnd) return nullptr;
    DisableTransitions(hwnd);
    return hwnd;
}

void HideSlot(Slot& s) {
    if (s.hwnd && s.visible) {
        ClearAccent(s);  // drop GPU blur work while hidden
        ShowWindow(s.hwnd, SW_HIDE);
    }
    s.visible = false;
    // Force a fresh accent apply on the next show — DWM can drop it while hidden.
    s.accentState = -1;
}

void HideAll() {
    for (Slot& s : g_slots) HideSlot(s);
    g_lastSync = {};
}

void DestroyAll() {
    for (Slot& s : g_slots) {
        if (s.hwnd && IsWindow(s.hwnd)) {
            DestroyWindow(s.hwnd);
        }
        s = Slot{};
    }
    g_lastSync = {};
    if (g_classRegistered) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        g_classRegistered = false;
    }
}

// Clips the window to the pill; `lift` raises the top so a notch pill gets a flat top.
void UpdateRegion(Slot& s, int w, int h, int radius, int lift) {
    if (s.radius == radius && s.lift == lift && s.w == w && s.h == h) return;
    const int diameter = radius * 2;
    HRGN region = CreateRoundRectRgn(0, -lift, w + 1, h + 1, diameter, diameter);
    if (region && SetWindowRgn(s.hwnd, region, FALSE) == 0) {
        DeleteObject(region);  // the system owns it only when SetWindowRgn succeeds
    }
    s.radius = radius;
    s.lift = lift;
}

void EnsureTopMost(Slot& s, bool wantTopMost) {
    if (!s.hwnd || s.topMost == wantTopMost) return;
    // Style-bit edits alone do not move a window between the topmost bands;
    // SetWindowPos with HWND_TOPMOST / HWND_NOTOPMOST is required.
    SetWindowPos(s.hwnd, wantTopMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    s.topMost = wantTopMost;
}

// Keeps all active backdrop windows placed immediately behind the island window in the z-order.
// Called around context menu popups and foreground activation so the blur never covers the island.
void EnsureBehind(HWND island) {
    if (!island) return;
    HWND prev = island;
    for (Slot& s : g_slots) {
        if (s.hwnd && s.visible) {
            SetWindowPos(s.hwnd, prev, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            prev = s.hwnd;
        }
    }
}

// Places one backdrop window below `prev`. Only moves it when something changed:
// per-frame SetWindowPos lags on some Windows 10 builds.
void PlaceSlot(Slot& s, HWND prev, int x, int y, int w, int h, int radius, int lift,
               bool wantTopMost) {
    EnsureTopMost(s, wantTopMost);
    UpdateRegion(s, w, h, radius, lift);
    const bool moved = x != s.x || y != s.y || w != s.w || h != s.h;
    const bool stacked = GetWindow(prev, GW_HWNDNEXT) == s.hwnd;
    if (moved || !stacked || !s.visible) {
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (stacked) flags |= SWP_NOZORDER;
        if (!s.visible) flags |= SWP_SHOWWINDOW;
        SetWindowPos(s.hwnd, prev, x, y, w, h, flags);
    }
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.visible = true;
}

// Called after every presented frame with the island's screen origin and its pills
// (client px, including hover scale) with corner radii.
void Sync(HWND island, POINT origin, const D2D1_RECT_F* rects, const float* radii, int count,
          bool notch, BackdropMaterial material, const D2D1_COLOR_F& tint, float tintAlpha) {
    // Custom fork (custom.17): "Clear" Liquid Glass has no blur window at all.
    const bool clearGlass = material == BackdropMaterial::LiquidGlass && g_settings.liquidGlassClear;
    if (material == BackdropMaterial::None || clearGlass || count <= 0 || !island ||
        !IsWindowVisible(island) || !Available() || Backdrop::ClipUiActiveCached()) {
        HideAll();
        return;
    }
    count = std::min(count, kMaxPills);

    int state = kAccentEnableBlurBehind;
    DWORD color = ColorToAccentAbgr(tint, tintAlpha * 0.22f);  // soft wash so plain blur isn't harsh
    if (material == BackdropMaterial::Acrylic) {
        state = kAccentEnableAcrylicBlurBehind;
        color = ColorToAccentAbgr(tint, tintAlpha);
    } else if (material == BackdropMaterial::LiquidGlass) {
        // Glass is clear, not frosted: plain blur with a barely-there tint.
        color = ColorToAccentAbgr(tint, tintAlpha * 0.08f);
    }

    // Precompute geometry once so we can early-out when nothing moved.
    struct Geom {
        int x, y, w, h, radius, lift;
        bool valid;
    } geom[kMaxPills] = {};
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F& r = rects[i];
        // Inset 1px so the region's hard edge stays hidden behind the island contour.
        constexpr int inset = 1;
        const int x = static_cast<int>(std::lround(origin.x + r.left)) + inset;
        const int y = static_cast<int>(std::lround(origin.y + r.top)) + (notch ? 0 : inset);
        const int right = static_cast<int>(std::lround(origin.x + r.right)) - inset;
        const int bottom = static_cast<int>(std::lround(origin.y + r.bottom)) - inset;
        const int w = right - x;
        const int h = bottom - y;
        if (w < 4 || h < 4) {
            geom[i].valid = false;
            continue;
        }
        int radius = static_cast<int>(std::lround(radii[i])) - inset;
        radius = std::clamp(radius, 0, std::min(w, h) / 2);
        geom[i] = {x, y, w, h, radius, notch ? radius : 0, true};
    }

    const bool wantTopMost = g_settings.alwaysOnTop;
    bool unchanged = g_lastSync.island == island && g_lastSync.origin.x == origin.x &&
                     g_lastSync.origin.y == origin.y && g_lastSync.count == count &&
                     g_lastSync.material == static_cast<int>(material) &&
                     g_lastSync.color == color && g_lastSync.state == state &&
                     g_lastSync.notch == notch;
    if (unchanged && count > 0 && g_slots[0].hwnd && g_slots[0].visible) {
        // If another always-on-top window slipped between the island and its backdrop, restack.
        if (GetWindow(island, GW_HWNDNEXT) != g_slots[0].hwnd) {
            unchanged = false;
        }
    }
    if (unchanged) {
        for (int i = 0; i < count; ++i) {
            if (!geom[i].valid) {
                if (g_slots[i].visible) {
                    unchanged = false;
                    break;
                }
                continue;
            }
            if (g_lastSync.x[i] != geom[i].x || g_lastSync.y[i] != geom[i].y ||
                g_lastSync.w[i] != geom[i].w || g_lastSync.h[i] != geom[i].h ||
                g_lastSync.radius[i] != geom[i].radius || g_lastSync.lift[i] != geom[i].lift ||
                !g_slots[i].visible || g_slots[i].topMost != wantTopMost) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) {
        // Accent can still need a refresh after a settings tick that didn't move geometry.
        for (int i = 0; i < count; ++i) {
            if (geom[i].valid && g_slots[i].hwnd) {
                ApplyAccent(g_slots[i], state, color);
            }
        }
        return;
    }

    HWND prev = island;
    for (int i = 0; i < count; ++i) {
        Slot& s = g_slots[i];
        if (!geom[i].valid) {
            HideSlot(s);
            continue;
        }
        if (!s.hwnd) {
            s.hwnd = CreateBackdropWindow(island);
            s.topMost = wantTopMost;
            if (!s.hwnd) continue;
        }
        ApplyAccent(s, state, color);
        PlaceSlot(s, prev, geom[i].x, geom[i].y, geom[i].w, geom[i].h, geom[i].radius,
                  geom[i].lift, wantTopMost);
        prev = s.hwnd;
    }
    for (int i = count; i < kMaxPills; ++i) HideSlot(g_slots[i]);

    g_lastSync.island = island;
    g_lastSync.origin = origin;
    g_lastSync.count = count;
    g_lastSync.material = static_cast<int>(material);
    g_lastSync.color = color;
    g_lastSync.state = state;
    g_lastSync.notch = notch;
    for (int i = 0; i < kMaxPills; ++i) {
        if (i < count && geom[i].valid) {
            g_lastSync.x[i] = geom[i].x;
            g_lastSync.y[i] = geom[i].y;
            g_lastSync.w[i] = geom[i].w;
            g_lastSync.h[i] = geom[i].h;
            g_lastSync.radius[i] = geom[i].radius;
            g_lastSync.lift[i] = geom[i].lift;
        } else {
            g_lastSync.x[i] = g_lastSync.y[i] = g_lastSync.w[i] = g_lastSync.h[i] = 0;
            g_lastSync.radius[i] = g_lastSync.lift[i] = 0;
        }
    }
}

}  // namespace DwmBackdrop

// The backdrop picks up a material or tint change on the next frame; turning it
// off hides the DWM windows immediately so the blur does not linger a frame.
void ApplyBackdropMaterial(HWND) {
    if (g_settings.backdropMaterial == BackdropMaterial::None) {
        DwmBackdrop::HideAll();
    }
    g_layoutDirty = true;
}

// Always unregisters before registering so a settings change never leaks the
// previous hotkey. Safe to call before the overlay window exists (no-ops).
void ApplyHideShowHotkey() {
    // The Do Not Disturb hotkey shares this entry point so both hotkeys are
    // registered, re-registered and released in the same places (render thread only).
    if (!g_hwnd) {
        return;
    }

    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_hwnd, ID_HIDE_SHOW_HOTKEY);
        g_hotkeyRegistered = false;
    }

    if (g_settings.hideShowHotkeyEnabled) {
        if (RegisterHotKey(g_hwnd, ID_HIDE_SHOW_HOTKEY, g_settings.hideShowModifiers,
                           g_settings.hideShowVk)) {
            g_registeredHotkeyModifiers = g_settings.hideShowModifiers;
            g_registeredHotkeyVk = g_settings.hideShowVk;
            g_hotkeyRegistered = true;
        } else {
            DI_LOG(L"Failed to register hide/show hotkey (error %lu).", GetLastError());
        }
    }

    // Custom fork: island Do Not Disturb hotkey (default Ctrl+Alt+N).
    if (g_quietHotkeyRegistered) {
        UnregisterHotKey(g_hwnd, ID_QUIET_HOTKEY);
        g_quietHotkeyRegistered = false;
    }
    if (g_settings.quietHotkeyEnabled) {
        if (RegisterHotKey(g_hwnd, ID_QUIET_HOTKEY, g_settings.quietModifiers, g_settings.quietVk)) {
            g_quietHotkeyRegistered = true;
        } else {
            DI_LOG(L"Failed to register Do Not Disturb hotkey (error %lu) - "
                   L"another program may already use this combination.", GetLastError());
        }
    }
}



RECT GetAnchorWorkRect(HMONITOR* outMonitor = nullptr) {
    HMONITOR selectedMonitor = GetAnchorMonitor();
    if (outMonitor) {
        *outMonitor = selectedMonitor;
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(selectedMonitor, &mi);
    return mi.rcWork;
}

// Vertical offset for the island's current size (#83).
//
// One Offset Y forced a compromise: a value that tucks the idle pill up near the
// screen edge leaves the expanded dashboard awkward to reach, and vice versa.
// With the separate offset enabled, Offset Y becomes the collapsed value and
// OffsetYExpanded the expanded one, and the island *eases* between them in step
// with the expansion itself rather than snapping at some threshold.
//
// windowHeight is the layered window's height, which carries kRenderPadY on both
// edges, so the pill's own height is recovered before measuring how expanded it
// is.
int ResolveOffsetY(float windowHeight) {
    if (!g_settings.separateExpandedOffsetY) {
        return g_settings.offsetY;
    }

    const float scale = std::max(0.05f, g_settings.sizeScale);
    const float pillHeight = std::max(0.0f, windowHeight - kRenderPadY * 2.0f) / scale;

    // Collapsed alert pills are ~44px tall in content space; the expanded
    // dashboard is MediaLayout::kExpandedHeight.
    constexpr float kCollapsedHeight = 44.0f;
    const float span = MediaLayout::kExpandedHeight - kCollapsedHeight;
    const float t = (span <= 1.0f) ? 0.0f
                                   : Clamp((pillHeight - kCollapsedHeight) / span, 0.0f, 1.0f);

    const float collapsed = static_cast<float>(g_settings.offsetY);
    const float expanded = static_cast<float>(g_settings.offsetYExpanded);
    return static_cast<int>(std::lround(collapsed + (expanded - collapsed) * t));
}

// Where the layered window's top-left goes for a pill `pillWidth` px wide.
// *subX receives the pill's fractional left edge (0..1) so the renderer can draw
// at the exact float position. Snapping to whole pixels made the pill's centre
// wobble ~1px while its width animated.
POINT ComputeOverlayOrigin(int height, float pillWidth, float* subX) {
    RECT work = GetAnchorWorkRect();
    const bool flushTop = g_settings.notchStyle || g_settings.borderMergedMode;
    const float workW = static_cast<float>(work.right - work.left);

    float pillLeft = work.left + (workW - pillWidth) * 0.5f;
    int y = flushTop ? work.top : (work.top + 8);

    switch (g_settings.position) {
        case Position::TopLeft:
            pillLeft = work.left + 16.0f;
            break;
        case Position::TopRight:
            pillLeft = work.right - pillWidth - 16.0f;
            break;
        case Position::BottomCenter:
            y = work.bottom - height - 40;
            break;
        case Position::BottomLeft:
            pillLeft = work.left + 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::BottomRight:
            pillLeft = work.right - pillWidth - 16.0f;
            y = work.bottom - height - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    pillLeft += static_cast<float>(g_settings.offsetX);
    const float windowLeft = pillLeft - kRenderPadX;
    const float windowLeftFloor = std::floor(windowLeft);
    if (subX) {
        *subX = windowLeft - windowLeftFloor;
    }

    y += ResolveOffsetY(static_cast<float>(height));
    return POINT{static_cast<LONG>(windowLeftFloor), static_cast<LONG>(y)};
}

// ── Fixed stage ──────────────────────────────────────────────────────────────
// The layered window is one fixed size (the largest pill the island can draw) and
// the pill animates inside it. The window never resizes or moves while animating.
// Split layouts (two alert pills) are the widest case.
constexpr float kStageContentWidth = 880.0f;   // content px, multiplied by sizeScale
constexpr float kStageContentHeight = 320.0f;

// Set to false to force a full repaint every frame (useful if you ever see ghosting).
constexpr bool kUseDirtyRect = true;

struct StageLayout {
    POINT origin = {0, 0};   // screen position of the window
    int width = 0;           // window size in px
    int height = 0;
    float pillLeft = 0.0f;   // pill's left edge inside the window (fractional)
    float pillTop = 0.0f;    // pill's top edge inside the window, before the nudge
};

// Where the pill was last drawn, in client px. Written by Renderer::Render and read
// by WM_NCHITTEST. Both run on the render thread, so no lock is needed.
struct PillClientRect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    bool valid = false;
};
PillClientRect g_pillClientRect;

// The island always uses the fixed stage; the backdrop is painted by separate DWM
// windows (see DwmBackdrop) and does not depend on the window size.
inline bool UsesFixedStage(const Settings&) {
    return true;
}

StageLayout ComputeStageLayout(const Settings& settings, float pillW, float pillH) {
    StageLayout s;
    const bool flushTop = settings.notchStyle || settings.borderMergedMode;
    pillW = std::max(pillW, 0.0f);

    if (!UsesFixedStage(settings)) {
        // Legacy path, identical to what Render() did before.
        s.height = std::max(1, static_cast<int>(std::ceil(pillH + kRenderPadY * 2.0f)));
        float subX = 0.0f;
        s.origin = ComputeOverlayOrigin(s.height, pillW, &subX);
        s.width = std::max(1, static_cast<int>(std::ceil(pillW + subX + kRenderPadX * 2.0f)));
        s.pillLeft = kRenderPadX + subX;
        s.pillTop = flushTop ? 0.0f : kRenderPadY;
        return s;
    }

    const RECT work = GetAnchorWorkRect();
    const int workW = work.right - work.left;
    s.width = static_cast<int>(std::ceil(kStageContentWidth * settings.sizeScale + kRenderPadX * 2.0f));
    s.height = static_cast<int>(std::ceil(kStageContentHeight * settings.sizeScale + kRenderPadY * 2.0f));

    // Horizontal: the stage is anchored, and the pill sits inside it the same way
    // the old window-per-pill code anchored it on screen.
    switch (settings.position) {
        case Position::TopLeft:
        case Position::BottomLeft:
            s.origin.x = work.left + 16 - static_cast<int>(kRenderPadX);
            s.pillLeft = kRenderPadX;
            break;
        case Position::TopRight:
        case Position::BottomRight:
            s.origin.x = work.right - 16 + static_cast<int>(kRenderPadX) - s.width;
            s.pillLeft = static_cast<float>(s.width) - kRenderPadX - pillW;
            break;
        default:  // centered
            s.origin.x = work.left + (workW - s.width) / 2;
            s.pillLeft = (static_cast<float>(s.width) - pillW) * 0.5f;
            break;
    }
    s.origin.x += settings.offsetX;

    // Vertical. Bottom positions keep the pill's bottom edge fixed, so it grows upward.
    const int offsetY = ResolveOffsetY(pillH + kRenderPadY * 2.0f);
    if (IsBottomPosition(settings.position)) {
        s.origin.y = work.bottom - s.height - 40 + offsetY;
        s.pillTop = static_cast<float>(s.height) - kRenderPadY - pillH;
    } else {
        s.origin.y = (flushTop ? work.top : work.top + 8) + offsetY;
        s.pillTop = flushTop ? 0.0f : kRenderPadY;
    }
    return s;
}

// z-order / owner / backdrop-region upkeep only. Position and size are NOT touched
// here: UpdateLayeredWindow sets them together with the new pixels, so the window
// never moves or resizes in a separate step from its content.
void ApplyOverlayZOrder(HWND hwnd, int width, int height) {
    HWND zOrder = g_settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;

    // Yield to the screen-clip overlay.
    const bool yieldToClipUi = Backdrop::ClipUiActiveCached();
    SetWindowPos(hwnd, yieldToClipUi ? nullptr : zOrder, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     (yieldToClipUi ? SWP_NOZORDER : SWP_SHOWWINDOW));
    if (!yieldToClipUi) {
        DwmBackdrop::EnsureBehind(hwnd);
    }
}

// Windows' own shell surfaces are all WS_EX_TOPMOST, and they are *supposed* to
// sit above the island. Raising ourselves over the Start menu, the notification
// centre, the volume OSD, Task View, a popup menu or a tooltip would be a bug,
// not a fix, so these classes are never treated as something to reclaim from.
bool IsShellOwnedWindow(HWND hwnd) {
    wchar_t className[128] = {};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) <= 0) {
        // Unknown, so treat it as shell-owned and leave the z-order alone.
        return true;
    }

    static const wchar_t* const kShellClasses[] = {
        L"Windows.UI.Core.CoreWindow",              // Start, Action Center, Search
        L"Xaml_WindowedPopupClass",                 // WinUI flyouts and popups
        L"Shell_TrayWnd",                           // taskbar
        L"Shell_SecondaryTrayWnd",                  // taskbar on other monitors
        L"TopLevelWindowForOverflowXamlIsland",     // taskbar overflow
        L"MultitaskingViewFrame",                   // Task View
        L"ForegroundStaging",                       // Task View staging
        L"#32768",                                  // popup menus
        L"tooltips_class32",                        // tooltips
        L"NarratorHelperWindow",
    };

    for (const wchar_t* candidate : kShellClasses) {
        if (_wcsicmp(className, candidate) == 0) {
            return true;
        }
    }
    return false;
}

// Re-asserts the island's place in the topmost band when another always-on-top
// window has been raised over it (PowerToys' bar, taskbar mods, other
// overlays). PositionOverlayWindow only runs on a resize or an explicit layout
// change, so without this the island stayed buried until the next one.
//
// Deliberately conservative: shell surfaces are ignored, a slight clip does not
// count, and we only act when we are still in the topmost band ourselves. That
// keeps this from turning into a z-order tug-of-war or from stealing focus
// surfaces away from Windows.
void EnsureTopmost(HWND hwnd, const RECT* pillScreenRect = nullptr) {
    if (!g_settings.alwaysOnTop || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return;
    }
    if (Backdrop::ClipUiActiveCached()) {
        return;
    }

    // If we are not topmost at all, PositionOverlayWindow owns fixing that;
    // walking the whole z-order from a demoted position would be expensive.
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        return;
    }

    RECT islandRect = {};
    if (pillScreenRect && !IsRectEmpty(pillScreenRect)) {
        islandRect = *pillScreenRect;  // the stage is mostly empty, so test the pill only
    } else if (!GetWindowRect(hwnd, &islandRect)) {
        return;
    }

    const long islandArea = std::max(1L, (islandRect.right - islandRect.left) *
                                             (islandRect.bottom - islandRect.top));
    // Require a real overlap, not a one-pixel graze.
    const long minOverlapArea = std::max(256L, islandArea / 8);

    bool covered = false;
    for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above != nullptr;
         above = GetWindow(above, GW_HWNDPREV)) {
        if (above == hwnd) {
            continue;
        }
        // Everything above a topmost window is itself topmost, so reaching a
        // non-topmost window means we are already at the top of that band.
        if ((GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            break;
        }
        if (!IsWindowVisible(above) || IsIconic(above) || IsShellOwnedWindow(above)) {
            continue;
        }

        RECT otherRect = {};
        RECT intersection = {};
        if (GetWindowRect(above, &otherRect) &&
            IntersectRect(&intersection, &islandRect, &otherRect)) {
            const long overlap = (intersection.right - intersection.left) *
                                 (intersection.bottom - intersection.top);
            if (overlap >= minOverlapArea) {
                covered = true;
                break;
            }
        }
    }

    if (covered) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        DwmBackdrop::EnsureBehind(hwnd);
    }
}

RECT GetIslandDockRect() {
    RECT work = GetAnchorWorkRect();
    const float scale = g_settings.sizeScale;
    const int dockWidth = static_cast<int>(std::max(280.0f, 340.0f * scale));
    const int dockHeight = static_cast<int>(std::max(48.0f, 56.0f * scale));
    const bool flushTop = g_settings.notchStyle || g_settings.borderMergedMode;

    int x = work.left + (work.right - work.left - dockWidth) / 2;
    int y = flushTop ? work.top : (work.top + 8);

    switch (g_settings.position) {
        case Position::TopLeft:
            x = work.left + 16;
            y = flushTop ? work.top : (work.top + 8);
            break;
        case Position::TopRight:
            x = work.right - dockWidth - 16;
            y = flushTop ? work.top : (work.top + 8);
            break;
        case Position::BottomCenter:
            x = work.left + (work.right - work.left - dockWidth) / 2;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::BottomLeft:
            x = work.left + 16;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::BottomRight:
            x = work.right - dockWidth - 16;
            y = work.bottom - dockHeight - 40;
            break;
        case Position::TopCenter:
        default:
            break;
    }

    x += g_settings.offsetX;
    // The dock rect is the hover target for a hidden island, which is always in
    // its collapsed state, so it follows the collapsed offset.
    y += g_settings.offsetY;

    RECT r;
    r.left = x;
    r.right = x + dockWidth;
    if (IsBottomPosition(g_settings.position)) {
        r.top = y;
        r.bottom = std::max(static_cast<int>(work.bottom), y + dockHeight + 20);
    } else {
        r.top = std::min(y, static_cast<int>(work.top));
        r.bottom = y + dockHeight;
    }
    return r;
}

bool DecodeImageBytesToPixels(const std::vector<uint8_t>& bytes, BitmapPixels* outPixels) {
    if (!outPixels || bytes.empty()) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) {
        return false;
    }

    void* locked = GlobalLock(mem);
    if (!locked) {
        GlobalFree(mem);
        return false;
    }
    memcpy(locked, bytes.data(), bytes.size());
    GlobalUnlock(mem);

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(mem);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                          &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    converter->GetSize(&width, &height);
    if (!width || !height || width > 2048 || height > 2048) {
        return false;
    }

    BitmapPixels pixels;
    pixels.width = width;
    pixels.height = height;
    pixels.bgra.resize(static_cast<size_t>(width) * height * 4);

    hr = converter->CopyPixels(nullptr, width * 4,
                               static_cast<UINT>(pixels.bgra.size()),
                               pixels.bgra.data());
    if (FAILED(hr)) {
        return false;
    }

    struct Bucket {
        uint32_t count = 0;
        uint32_t r = 0;
        uint32_t g = 0;
        uint32_t b = 0;
        uint32_t satSum = 0;  // accumulated saturation for vibrancy-weighted winner
    };

    std::array<Bucket, 16 * 16 * 16> buckets{};
    for (size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
        const uint8_t alpha = pixels.bgra[i + 3];
        const uint8_t blue  = pixels.bgra[i + 0];
        const uint8_t green = pixels.bgra[i + 1];
        const uint8_t red   = pixels.bgra[i + 2];
        if (alpha < 32) {
            continue;
        }

        const int maxc = std::max({red, green, blue});
        const int minc = std::min({red, green, blue});
        const int luminance = (54 * red + 183 * green + 19 * blue) / 256;
        const int saturation = maxc - minc;
        // Reject near-black, near-white, and near-gray pixels.
        // Raise luminance floor slightly (36 instead of 28) so very dark-but-colorful
        // pixels don't swamp vibrancy scoring on dark-mood album art.
        if (luminance < 36 || luminance > 220 || saturation < 28) {
            continue;
        }

        const size_t bucketIndex = ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4);
        Bucket& bucket = buckets[bucketIndex];
        const uint32_t weight = 1 + static_cast<uint32_t>(saturation / 32);  // stronger vibrancy weight
        bucket.count  += weight;
        bucket.r      += red   * weight;
        bucket.g      += green * weight;
        bucket.b      += blue  * weight;
        bucket.satSum += static_cast<uint32_t>(saturation) * weight;
    }

    // --- Vibrancy-weighted winner selection ---
    // Raw frequency alone lets large muted regions (e.g. a beige background)
    // beat a smaller vivid color that's visually "the" accent.
    // Strategy: collect the top 3 by count, then pick whichever has the
    // highest average saturation — cheap, no extra image pass required.
    struct Candidate { const Bucket* b = nullptr; size_t idx = 0; };
    Candidate top[3];
    for (size_t i = 0; i < buckets.size(); ++i) {
        const Bucket& bk = buckets[i];
        if (bk.count == 0) continue;
        for (int s = 0; s < 3; ++s) {
            if (!top[s].b || bk.count > top[s].b->count) {
                for (int t = 2; t > s; --t) top[t] = top[t - 1];
                top[s] = {&bk, i};
                break;
            }
        }
    }

    // Among those top-3, pick the one with the highest avg saturation.
    const Bucket* best = nullptr;
    size_t bestIdx = 0;
    float bestVibrancy = -1.0f;
    for (int s = 0; s < 3; ++s) {
        if (!top[s].b) break;
        const float vibrancy = static_cast<float>(top[s].b->satSum) /
                               static_cast<float>(top[s].b->count);
        if (vibrancy > bestVibrancy) {
            bestVibrancy = vibrancy;
            best = top[s].b;
            bestIdx = top[s].idx;
        }
    }

    if (best && best->count > 0) {
        // --- Neighbor-bucket merging ---
        // A color straddling a bucket boundary splits its votes across up to 8
        // adjacent cells. Pool the winning bucket with all 26 face/edge/corner
        // neighbors before averaging so the final RGB is stable and representative.
        uint64_t poolCount = 0;
        double poolR = 0, poolG = 0, poolB = 0;

        const int bi = static_cast<int>((bestIdx >> 8) & 0xF);  // red index
        const int gi = static_cast<int>((bestIdx >> 4) & 0xF);  // green index
        const int bli = static_cast<int>( bestIdx       & 0xF); // blue index

        for (int dr = -1; dr <= 1; ++dr) {
            for (int dg = -1; dg <= 1; ++dg) {
                for (int db = -1; db <= 1; ++db) {
                    const int ni = bi + dr, nj = gi + dg, nk = bli + db;
                    if (ni < 0 || ni > 15 || nj < 0 || nj > 15 || nk < 0 || nk > 15) continue;
                    const size_t nIdx = (static_cast<size_t>(ni) << 8) |
                                        (static_cast<size_t>(nj) << 4) |
                                         static_cast<size_t>(nk);
                    const Bucket& nb = buckets[nIdx];
                    if (nb.count == 0) continue;
                    poolCount += nb.count;
                    poolR += nb.r;
                    poolG += nb.g;
                    poolB += nb.b;
                }
            }
        }

        if (poolCount > 0) {
            const float invC = 1.0f / static_cast<float>(poolCount);
            const float rawR = static_cast<float>(poolR * invC) / 255.0f;
            const float rawG = static_cast<float>(poolG * invC) / 255.0f;
            const float rawB = static_cast<float>(poolB * invC) / 255.0f;

            D2D1_COLOR_F bgColor = g_settings.pillBgColor;
            if (bgColor.a <= 0.0f) {
                bgColor = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f); // Obsidian default #08080A
            }

            pixels.sampledAccent = EnsureContrastAgainstBackground(
                D2D1::ColorF(rawR, rawG, rawB, 1.0f),
                bgColor);
        }
        // If poolCount is somehow 0 (all neighbors empty and center also cleared),
        // the existing sampledAccent default (#4cc9f0) is kept unchanged — explicit
        // fallback for fully-grayscale/near-black-and-white album art.
    }
    // else: no qualifying colorful pixel found (B&W / fully-filtered image)
    // — leave sampledAccent at the BitmapPixels default (#4cc9f0).

    pixels.generation = ++g_artGenerationCounter;
    *outPixels = std::move(pixels);
    return true;
}

bool IconToPixels(HICON icon, UINT size, BitmapPixels* outPixels) {
    if (!icon || !outPixels || !size) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) {
        return false;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(size);
    bi.bmiHeader.biHeight = -static_cast<LONG>(size);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return false;
    }

    // Fill with transparent black (alpha=0) so icon edges don't bleed dark fringe.
    HGDIOBJ old = SelectObject(dc, bitmap);
    RECT fill = {0, 0, static_cast<LONG>(size), static_cast<LONG>(size)};
    // Use NULL_BRUSH (transparent) then manually zero-fill the BGRA buffer after copy.
    FillRect(dc, &fill, reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH)));
    // Zero out the bits so background is fully transparent before drawing icon.
    ZeroMemory(bits, static_cast<size_t>(size) * size * 4);
    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);

    BitmapPixels pixels;
    pixels.width = size;
    pixels.height = size;
    pixels.bgra.resize(static_cast<size_t>(size) * size * 4);
    memcpy(pixels.bgra.data(), bits, pixels.bgra.size());

    // Older icons can have no alpha in the color bitmap. Treat black pixels as
    // transparent only when the icon did not write any alpha at all.
    bool hasAlpha = false;
    for (size_t i = 3; i < pixels.bgra.size(); i += 4) {
        if (pixels.bgra[i] != 0) {
            hasAlpha = true;
            break;
        }
    }
    if (!hasAlpha) {
        // No alpha channel: treat near-black as transparent, rest as opaque.
        for (size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
            const bool black = pixels.bgra[i] < 4 && pixels.bgra[i + 1] < 4 && pixels.bgra[i + 2] < 4;
            pixels.bgra[i + 3] = black ? 0 : 255;
        }
    } else {
        // Convert to premultiplied alpha so D2D renders edges cleanly without dark fringing.
        for (size_t i = 0; i + 3 < pixels.bgra.size(); i += 4) {
            const uint8_t a = pixels.bgra[i + 3];
            if (a < 255 && a > 0) {
                pixels.bgra[i + 0] = static_cast<uint8_t>(pixels.bgra[i + 0] * a / 255);
                pixels.bgra[i + 1] = static_cast<uint8_t>(pixels.bgra[i + 1] * a / 255);
                pixels.bgra[i + 2] = static_cast<uint8_t>(pixels.bgra[i + 2] * a / 255);
            }
        }
    }

    pixels.generation = ++g_artGenerationCounter;
    *outPixels = std::move(pixels);

    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return true;
}

PixelsRef IconToPixelsRef(HICON icon, UINT size) {
    BitmapPixels px;
    return IconToPixels(icon, size, &px) ? PixelsRef(std::move(px)) : PixelsRef();
}

bool ProcessImageNameForPid(DWORD pid, std::wstring* imageName) {
    if (!pid || !imageName) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (ok) {
        *imageName = path;
    }
    return ok;
}

HICON CopyWindowIcon(HWND hwnd, WPARAM iconType) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, WM_GETICON, iconType, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &result);
    return result ? CopyIcon(reinterpret_cast<HICON>(result)) : nullptr;
}

HICON getProcessIcon(DWORD pid) {
    std::wstring path;
    if (ProcessImageNameForPid(pid, &path) && !path.empty()) {
        HICON hIcon = nullptr;
        UINT iconId = 0;
        // Try to fetch a high-res 64x64 icon first to avoid pixelated icons
        using PrivateExtractIconsW_t = UINT(WINAPI*)(LPCWSTR, int, int, int, HICON*, UINT*, UINT, UINT);
        static auto pPrivateExtractIconsW = reinterpret_cast<PrivateExtractIconsW_t>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "PrivateExtractIconsW"));
        if (pPrivateExtractIconsW && pPrivateExtractIconsW(path.c_str(), 0, 64, 64, &hIcon, &iconId, 1, 0) == 1 && hIcon) {
            return hIcon;
        }

        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON)) {
            return sfi.hIcon;
        }

        HICON large = nullptr;
        HICON smallIcon = nullptr;  // not "small": <rpcndr.h> #defines that to char
        if (ExtractIconExW(path.c_str(), 0, &large, &smallIcon, 1) > 0) {
            if (smallIcon) {
                DestroyIcon(smallIcon);
            }
            if (large) {
                return large;
            }
        }
    }

    return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
}

HICON getWindowIcon(HWND hwnd) {
    if (!hwnd) {
        return CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }

    if (HICON icon = CopyWindowIcon(hwnd, ICON_BIG)) {
        return icon;
    }
    if (HICON icon = CopyWindowIcon(hwnd, ICON_SMALL)) {
        return icon;
    }

    if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) {
        return CopyIcon(icon);
    }
    if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM))) {
        return CopyIcon(icon);
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return getProcessIcon(pid);
}

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring BaseNameFromPath(std::wstring path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.erase(0, slash + 1);
    }
    return path;
}

std::wstring StripExtension(std::wstring value) {
    const size_t dot = value.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        value.resize(dot);
    }
    return value;
}

bool ProcessImageNameForWindow(HWND hwnd, std::wstring* imageName) {
    if (!imageName) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        return false;
    }

    return ProcessImageNameForPid(pid, imageName);
}

std::wstring FriendlyMediaSourceName(std::wstring_view source) {
    const std::wstring lower = ToLowerCopy(std::wstring(source));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YouTube";
    if (lower.find(L"spotify") != std::wstring::npos) return L"Spotify";
    if (lower.find(L"chrome") != std::wstring::npos) return L"Chrome";
    if (lower.find(L"msedge") != std::wstring::npos || lower.find(L"edge") != std::wstring::npos) return L"Edge";
    if (lower.find(L"firefox") != std::wstring::npos) return L"Firefox";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (lower.find(L"wmplayer") != std::wstring::npos) return L"Windows Media";
    if (lower.find(L"zune") != std::wstring::npos || lower.find(L"media") != std::wstring::npos) return L"Media Player";

    std::wstring text(source);
    const size_t bang = text.find(L'!');
    if (bang != std::wstring::npos && bang + 1 < text.size()) {
        text.erase(0, bang + 1);
    }
    const size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        text.resize(dot);
    }
    return text.empty() ? L"Media" : text;
}

std::wstring MediaSourceBadge(std::wstring_view sourceName) {
    const std::wstring lower = ToLowerCopy(std::wstring(sourceName));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"chrome") != std::wstring::npos) return L"CH";
    if (lower.find(L"edge") != std::wstring::npos) return L"ED";
    if (lower.find(L"firefox") != std::wstring::npos) return L"FF";
    if (lower.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (sourceName.empty()) return L"\u25b6";
    std::wstring badge;
    badge.push_back(static_cast<wchar_t>(towupper(sourceName[0])));
    return badge;
}

bool WindowLooksLikeMediaSource(HWND hwnd, const std::wstring& sourceLower) {
    if (!IsWindowVisible(hwnd) || hwnd == g_hwnd) {
        return false;
    }

    std::wstring image;
    if (!ProcessImageNameForWindow(hwnd, &image)) {
        return false;
    }

    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
    if (base.empty()) {
        return false;
    }

    const std::wstring stem = StripExtension(base);
    return sourceLower.find(base) != std::wstring::npos ||
           (stem.size() >= 4 && sourceLower.find(stem) != std::wstring::npos) ||
           (base.find(L"chrome") != std::wstring::npos && sourceLower.find(L"chrome") != std::wstring::npos) ||
           (base.find(L"msedge") != std::wstring::npos && sourceLower.find(L"edge") != std::wstring::npos) ||
           (base.find(L"vlc") != std::wstring::npos && sourceLower.find(L"vlc") != std::wstring::npos);
}

// #62: short-form video feeds change "track" every few seconds, which turned
// auto-expand into a constant popup. A blocklist entry is matched loosely
// against the friendly source name, the raw AUMID and the title, so a single
// entry like "tiktok" catches both the desktop app and a browser tab. The
// collapsed pill still updates -- only the expansion is suppressed.
//
// Reads the blocklist under g_settingsMutex rather than taking a Settings
// reference: the caller is the render loop, and walking a std::vector<std::wstring>
// that LoadSettings() may be reassigning is a use-after-free, not just a stale
// read. The haystack is built outside the lock because that part allocates.
bool MediaExpandBlocked(const MediaSnapshot& media) {
    {
        // Fast path for the overwhelmingly common empty-blocklist case: one
        // uncontended lock, no allocation, no string work.
        std::lock_guard lock(g_settingsMutex);
        if (g_settings.mediaExpandBlocklist.empty()) {
            return false;
        }
    }

    const std::wstring haystack = ToLowerCopy(
        media.sourceName + L"\n" + media.sourceAppUserModelId + L"\n" + media.title);

    std::lock_guard lock(g_settingsMutex);
    for (const std::wstring& needle : g_settings.mediaExpandBlocklist) {
        if (!needle.empty() && haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool IsBrowserMediaSource(std::wstring_view source) {
    const std::wstring lower = ToLowerCopy(std::wstring(source));
    return lower.find(L"chrome") != std::wstring::npos ||
           lower.find(L"edge") != std::wstring::npos ||
           lower.find(L"msedge") != std::wstring::npos ||
           lower.find(L"firefox") != std::wstring::npos ||
           lower.find(L"youtube") != std::wstring::npos;
}

std::wstring SiteBadgeFromTitle(std::wstring_view title) {
    const std::wstring lower = ToLowerCopy(std::wstring(title));
    if (lower.find(L"youtube") != std::wstring::npos) return L"YT";
    if (lower.find(L"netflix") != std::wstring::npos) return L"NF";
    if (lower.find(L"prime video") != std::wstring::npos || lower.find(L"amazon") != std::wstring::npos) return L"PV";
    if (lower.find(L"disney") != std::wstring::npos) return L"D+";
    if (lower.find(L"hotstar") != std::wstring::npos) return L"HS";
    if (lower.find(L"spotify") != std::wstring::npos) return L"SP";
    if (lower.find(L"soundcloud") != std::wstring::npos) return L"SC";
    return L"WEB";
}

struct IconCacheEntry {
    DWORD pid = 0;
    std::wstring exePath;
    PixelsRef pixels;
    uint64_t lastUsed = 0;
};

std::mutex g_iconCacheMutex;
std::unordered_map<DWORD, IconCacheEntry> g_iconCacheByPid;
uint64_t g_iconCacheClock = 0;

PixelsRef GetCachedProcessIconPixels(DWORD pid, UINT size) {
    std::wstring exePath;
    ProcessImageNameForPid(pid, &exePath);

    {
        std::lock_guard lock(g_iconCacheMutex);
        auto it = g_iconCacheByPid.find(pid);
        if (it != g_iconCacheByPid.end() && it->second.exePath == exePath &&
            it->second.pixels->width == size) {
            it->second.lastUsed = ++g_iconCacheClock;
            return it->second.pixels;
        }
    }

    PixelsRef pixels;
    HICON icon = getProcessIcon(pid);
    if (icon) {
        pixels = IconToPixelsRef(icon, size);
        DestroyIcon(icon);
    }

    if (pixels) {
        std::lock_guard lock(g_iconCacheMutex);
        g_iconCacheByPid[pid] = IconCacheEntry{pid, exePath, pixels, ++g_iconCacheClock};
        if (g_iconCacheByPid.size() > 32) {
            auto oldest = g_iconCacheByPid.begin();
            for (auto it = g_iconCacheByPid.begin(); it != g_iconCacheByPid.end(); ++it) {
                if (it->second.lastUsed < oldest->second.lastUsed) {
                    oldest = it;
                }
            }
            g_iconCacheByPid.erase(oldest);
        }
    }

    return pixels;
}

PixelsRef GetWindowIconPixels(HWND hwnd, UINT size) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    PixelsRef pixels = GetCachedProcessIconPixels(pid, size);
    if (pixels) {
        return pixels;
    }

    HICON icon = getWindowIcon(hwnd);
    if (icon) {
        pixels = IconToPixelsRef(icon, size);
        DestroyIcon(icon);
    }
    return pixels;
}

bool IsIgnorableForegroundWindow(HWND hwnd, const std::wstring& title) {
    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    const std::wstring cls = ToLowerCopy(className);
    const std::wstring lowerTitle = ToLowerCopy(title);

    std::wstring image;
    ProcessImageNameForWindow(hwnd, &image);
    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));

    return title.empty() ||
           hwnd == g_hwnd ||
           cls == L"shell_traywnd" ||
           cls == L"workerw" ||
           cls == L"progman" ||
           base == L"explorer.exe" ||
           base == L"dynamicislandlauncher.exe";
}

BOOL CALLBACK FindMediaSourceWindowProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<std::pair<const std::wstring*, HWND*>*>(lParam);
    if (WindowLooksLikeMediaSource(hwnd, *data->first)) {
        *data->second = hwnd;
        return FALSE;
    }
    return TRUE;
}

std::wstring FindBrowserMediaSiteBadge(const std::wstring& sourceAppUserModelId) {
    const std::wstring sourceLower = ToLowerCopy(sourceAppUserModelId);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&sourceLower, &found};
    EnumWindows(FindMediaSourceWindowProc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        wchar_t title[192] = {};
        GetWindowTextW(found, title, ARRAYSIZE(title));
        return SiteBadgeFromTitle(title);
    }
    return L"WEB";
}

PixelsRef FindMediaSourceIcon(const std::wstring& sourceAppUserModelId) {
    PixelsRef pixels;
    if (IsBrowserMediaSource(sourceAppUserModelId)) {
        // Continue searching for the browser window anyway to retrieve
        // the browser's app icon or the PWA's app icon.
    }

    const std::wstring sourceLower = ToLowerCopy(sourceAppUserModelId);
    HWND found = nullptr;
    std::pair<const std::wstring*, HWND*> data{&sourceLower, &found};
    EnumWindows(FindMediaSourceWindowProc, reinterpret_cast<LPARAM>(&data));
    if (found) {
        pixels = GetWindowIconPixels(found, 32);
    }
    return pixels;
}

// Standalone 0.8/0.9: every Bluetooth / stream WinRT call has a deadline. A plain .get() can
// block forever if the radio or a device misbehaves, freezing the Bluetooth
// thread (and its watchers' bookkeeping) for good. Timeout or error -> fallback.
template <typename T>
T AwaitOr(winrt::Windows::Foundation::IAsyncOperation<T> const& op, uint32_t timeoutMs, T fallback) {
    try {
        if (op.wait_for(std::chrono::milliseconds(timeoutMs)) == winrt::Windows::Foundation::AsyncStatus::Completed) {
            return op.GetResults();
        }
        op.Cancel();
    } catch (...) {
    }
    return fallback;
}

std::vector<uint8_t> ReadWinRtStreamBytes(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference) {
    std::vector<uint8_t> bytes;
    if (!reference) {
        return bytes;
    }

    try {
        // Standalone 0.9: a stuck or hostile media app can't hang us -- every
        // step has a deadline, and only the bytes actually loaded are read.
        auto stream = AwaitOr(reference.OpenReadAsync(), 3000,
                              winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType{nullptr});
        if (!stream) {
            return bytes;
        }

        const uint64_t size64 = stream.Size();
        if (size64 == 0 || size64 > 8 * 1024 * 1024) {
            return bytes;
        }

        const uint32_t size = static_cast<uint32_t>(size64);
        winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
        const uint32_t loaded = AwaitOr<uint32_t>(reader.LoadAsync(size), 3000, 0u);
        if (loaded == 0 || loaded > size) {
            return bytes;
        }
        bytes.resize(loaded);
        reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    } catch (...) {
        bytes.clear();
    }
    return bytes;
}

void TriggerNudge() {
    // If we're parked (auto-hidden), any real event must wake us regardless
    // of the normal throttle, or it'd never be seen until the next hook/
    // hotkey/fullscreen-recheck wake.
    const bool wasParked = g_autoHiddenParked.exchange(false, std::memory_order_relaxed);
    const double now = NowSeconds();
    const double previous = g_lastNudgeTime.load();
    if (!wasParked && now - previous < 0.45) {
        return;
    }
    g_lastNudgeTime = now;
    HWND hwnd = g_hwnd;
    if (hwnd) {
        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
    }
}

// Chooses which SMTC session the island should follow.
//
// GetCurrentSession() returns whatever Windows last treated as the foreground
// media app, which is often a stale or paused session belonging to a different
// player. Following it blindly meant a player that was genuinely playing could
// be ignored entirely -- the reason VLC frequently produced no reaction at all
// while Spotify worked. Prefer the current session when it is actually playing,
// otherwise the first session that is, and only then fall back to the current
// one so paused/idle state still shows.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession SelectActiveMediaSession(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    const std::wstring& preferredAumid) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    auto sessionIsPlaying = [](auto const& candidate) -> bool {
        if (!candidate) {
            return false;
        }
        try {
            return candidate.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
        } catch (...) {
            return false;
        }
    };

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession current = nullptr;
    try {
        current = manager.GetCurrentSession();
    } catch (...) {
    }

    // The user picked a source (dock / menu): follow it, playing or not.
    if (!preferredAumid.empty()) {
        try {
            for (auto const& candidate : manager.GetSessions()) {
                if (preferredAumid == candidate.SourceAppUserModelId().c_str()) {
                    return candidate;
                }
            }
        } catch (...) {
        }
    }

    if (sessionIsPlaying(current)) {
        return current;
    }

    try {
        for (auto const& candidate : manager.GetSessions()) {
            if (sessionIsPlaying(candidate)) {
                return candidate;
            }
        }
    } catch (...) {
    }

    return current;
}

// Exe name for path-style AUMIDs (classic desktop apps), friendly name otherwise.
std::wstring MediaSourceDisplayName(const std::wstring& aumid) {
    std::wstring name = FriendlyMediaSourceName(aumid);
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name.find(L':') != std::wstring::npos) {
        name = StripExtension(BaseNameFromPath(aumid));
        if (!name.empty()) {
            name[0] = static_cast<wchar_t>(towupper(name[0]));
        }
    }
    return name.empty() ? std::wstring(L"Media") : name;
}

struct MediaSourceTracker {
    // Auto mode / dock pick bookkeeping (see ChooseMediaSourceAumid).
    std::unordered_map<std::wstring, bool> wasPlaying;      // playing state at the previous poll
    std::unordered_map<std::wstring, uint64_t> startedSeq;  // when each source last STARTED playing (higher = newer)
    uint64_t seqCounter = 0;
    std::wstring dockOverride;                              // temporary dock pick; empty = none
    uint64_t dockOverrideBaseSeq = 0;                       // any start newer than this ends the dock pick
    unsigned dockOverrideRevSeen = 0;
    std::vector<std::wstring> order;                     // first seen = first in the dock
    std::unordered_map<std::wstring, PixelsRef> icons;
    std::unordered_map<std::wstring, int> iconRetry;     // polls before searching for an icon again
};

bool CollectMediaSources(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager const& manager,
    MediaSourceTracker& tracker, std::vector<MediaSourceInfo>* out) {
    using PlaybackStatus =
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    std::vector<std::pair<std::wstring, bool>> live;  // AUMID, playing
    try {
        for (auto const& session : manager.GetSessions()) {
            std::wstring aumid = session.SourceAppUserModelId().c_str();
            if (aumid.empty()) continue;
            bool playing = false;
            try {
                playing = session.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
            } catch (...) {
            }
            auto it = std::find_if(live.begin(), live.end(),
                                   [&](const auto& e) { return e.first == aumid; });
            if (it != live.end()) {
                it->second = it->second || playing;
            } else {
                live.emplace_back(std::move(aumid), playing);
            }
        }
    } catch (...) {
        return false;  // keep whatever the dock showed before
    }

    // Remember when each source last STARTED playing (not playing -> playing).
    // Auto mode follows the newest start.
    {
        std::wstring currentAumid;
        try {
            auto current = manager.GetCurrentSession();
            if (current) {
                currentAumid = current.SourceAppUserModelId().c_str();
            }
        } catch (...) {
        }

        // A source that disappeared forgets its history, so reopening it counts as a fresh start.
        for (auto it = tracker.wasPlaying.begin(); it != tracker.wasPlaying.end();) {
            const std::wstring& id = it->first;
            const bool stillLive = std::any_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
            if (stillLive) {
                ++it;
            } else {
                tracker.startedSeq.erase(id);
                it = tracker.wasPlaying.erase(it);
            }
        }

        std::vector<std::wstring> started;
        for (const auto& e : live) {
            bool& was = tracker.wasPlaying[e.first];
            if (e.second && !was) {
                started.push_back(e.first);
            }
            was = e.second;
        }
        // Several sources starting inside one poll: the one Windows calls "current" counts as newest.
        std::stable_sort(started.begin(), started.end(),
                         [&](const std::wstring& a, const std::wstring& b) {
                             return a != currentAumid && b == currentAumid;
                         });
        for (const std::wstring& id : started) {
            tracker.startedSeq[id] = ++tracker.seqCounter;
        }
    }

    // Stable order: new sources go on the end, vanished ones drop out.
    tracker.order.erase(
        std::remove_if(tracker.order.begin(), tracker.order.end(),
                       [&](const std::wstring& id) {
                           return std::none_of(live.begin(), live.end(),
                                               [&](const auto& e) { return e.first == id; });
                       }),
        tracker.order.end());
    for (const auto& e : live) {
        if (std::find(tracker.order.begin(), tracker.order.end(), e.first) == tracker.order.end()) {
            tracker.order.push_back(e.first);
        }
    }
    for (auto it = tracker.icons.begin(); it != tracker.icons.end();) {
        if (std::find(tracker.order.begin(), tracker.order.end(), it->first) == tracker.order.end()) {
            tracker.iconRetry.erase(it->first);
            it = tracker.icons.erase(it);
        } else {
            ++it;
        }
    }

    out->clear();
    for (const std::wstring& id : tracker.order) {
        MediaSourceInfo info;
        info.aumid = id;
        info.name = MediaSourceDisplayName(id);
        info.badge = MediaSourceBadge(info.name);
        for (const auto& e : live) {
            if (e.first == id) info.playing = e.second;
        }

        PixelsRef& icon = tracker.icons[id];
        if (!icon) {  // window enumeration is not free, so retry rarely
            int& wait = tracker.iconRetry[id];
            if (wait <= 0) {
                icon = FindMediaSourceIcon(id);
                wait = 8;
            } else {
                --wait;
            }
        }
        info.icon = icon;
        out->push_back(std::move(info));
    }
    return true;
}

// Which source should the island show?  Returns "" to fall back to the system's current session.
//   1. Dock pick: temporary. Ends when something NEW starts playing anywhere, or when
//      that app goes away. After that, the lock or Auto applies again.
//   2. Locked app (right-click menu): wins over Auto.
//   3. Auto: the playing app that started most recently. If it closes, the next most
//      recent playing app. With nothing playing: the app that played last.
std::wstring ChooseMediaSourceAumid(MediaSourceTracker& tracker, const std::wstring& lockedAumid) {
    auto exists = [&](const std::wstring& id) {
        return std::find(tracker.order.begin(), tracker.order.end(), id) != tracker.order.end();
    };
    auto seqOf = [&](const std::wstring& id) -> uint64_t {
        auto it = tracker.startedSeq.find(id);
        return it == tracker.startedSeq.end() ? 0 : it->second;
    };

    // Pick up a new dock request (or an Auto reset from the menu).
    const unsigned rev = g_dockOverrideRev.load();
    if (rev != tracker.dockOverrideRevSeen) {
        tracker.dockOverrideRevSeen = rev;
        {
            std::lock_guard lock(g_mediaSourceMutex);
            tracker.dockOverride = g_dockOverrideAumid;
        }
        tracker.dockOverrideBaseSeq = tracker.seqCounter;
    }

    if (!tracker.dockOverride.empty()) {
        bool somethingNewStarted = false;
        for (const std::wstring& id : tracker.order) {
            if (seqOf(id) > tracker.dockOverrideBaseSeq) {
                somethingNewStarted = true;
                break;
            }
        }
        if (!exists(tracker.dockOverride) || somethingNewStarted) {
            tracker.dockOverride.clear();  // back to the lock / Auto
        } else {
            return tracker.dockOverride;
        }
    }

    if (!lockedAumid.empty() && exists(lockedAumid)) {
        return lockedAumid;
    }

    // Auto. Playing sources beat paused ones; within each group the newest start wins.
    std::wstring best;
    bool bestPlaying = false;
    uint64_t bestSeq = 0;
    for (const std::wstring& id : tracker.order) {
        const auto pit = tracker.wasPlaying.find(id);
        const bool playing = pit != tracker.wasPlaying.end() && pit->second;
        const uint64_t seq = seqOf(id);
        if (best.empty() || (playing && !bestPlaying) || (playing == bestPlaying && seq > bestSeq)) {
            best = id;
            bestPlaying = playing;
            bestSeq = seq;
        }
    }
    if (!bestPlaying && bestSeq == 0) {
        return std::wstring();  // nothing has ever played: let Windows decide
    }
    return best;
}

DWORD WINAPI MediaThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using PlaybackStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    Manager manager{nullptr};
    bool loggedUnavailable = false;
    int64_t pendingSeekBaseline = -1;  // guards against bogus "went backwards" readings
    MediaSourceTracker sourceTracker;
    ULONGLONG pendingSeekBaselineTickCount = 0;
    // Standalone 0.9.1: our own clock for players whose LastUpdatedTime is stale
    // or missing (Apple Music): when Position() last changed, as seen by us.
    int64_t ownRawPos = -1;
    int64_t ownEnd = -1;
    bool ownWasPlaying = false;
    std::wstring ownSource;
    ULONGLONG ownRawSeenAt = 0;
    std::wstring loggedTimelineFor;

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MediaSnapshot next;
        std::vector<MediaSourceInfo> newSources;
        bool gotSources = false;

        try {
            if (!manager) {
                manager = Manager::RequestAsync().get();
            }

            if (manager) {
                gotSources = CollectMediaSources(manager, sourceTracker, &newSources);
                const std::wstring chosenAumid = gotSources
                    ? ChooseMediaSourceAumid(sourceTracker, GetPreferredMediaSource())
                    : GetPreferredMediaSource();
                auto session = SelectActiveMediaSession(manager, chosenAumid);
                if (session) {
                    auto properties = session.TryGetMediaPropertiesAsync().get();
                    auto playback = session.GetPlaybackInfo();
                    auto timeline = session.GetTimelineProperties();

                    next.available = true;
                    next.playing = playback.PlaybackStatus() == PlaybackStatus::Playing;
                    next.title = properties.Title().c_str();
                    next.artist = properties.Artist().c_str();
                    next.albumTitle = properties.AlbumTitle().c_str();

                    if (timeline) {
                        // Standalone 0.9.1: positions are measured from StartTime (Apple Music
                        // and some others don't start at 0), and when EndTime is missing the
                        // seekable range gives the length.
                        const int64_t startT = timeline.StartTime().count();
                        const int64_t rawPos = std::max<int64_t>(0, timeline.Position().count() - startT);
                        int64_t np = rawPos;
                        int64_t ne = timeline.EndTime().count() - startT;
                        if (ne <= 0) {
                            ne = timeline.MaxSeekTime().count() - timeline.MinSeekTime().count();
                        }
                        if (ne < 0) ne = 0;
                        bool npP = (playback.PlaybackStatus() == PlaybackStatus::Playing);
                        const std::wstring srcId = session.SourceAppUserModelId().c_str();

                        // Our own record of when Position() last moved.
                        const ULONGLONG tickNow = GetTickCount64();
                        if (rawPos != ownRawPos || ne != ownEnd || npP != ownWasPlaying || srcId != ownSource) {
                            ownRawPos = rawPos;
                            ownEnd = ne;
                            ownWasPlaying = npP;
                            ownSource = srcId;
                            ownRawSeenAt = tickNow;
                        }
                        if (npP) {
                            // Position() is the position at LastUpdatedTime, not "now".
                            bool fromApp = false;
                            try {
                                const auto lastUpdated = timeline.LastUpdatedTime();
                                const int64_t ageTicks = (winrt::clock::now() - lastUpdated).count();
                                if (ageTicks > 0 && ageTicks < 10 * 10000000LL) {
                                    np = rawPos + ageTicks;
                                    fromApp = true;
                                }
                            } catch (...) {
                            }
                            if (!fromApp) {
                                // The app's timestamp is stale or missing (Apple Music reports the
                                // position only on play / pause / seek): count from when we saw it.
                                np = rawPos + static_cast<int64_t>(tickNow - ownRawSeenAt) * 10000;
                            }
                        }
                        if (ne > 0 && np > ne) np = ne;
                        if (loggedTimelineFor != srcId) {
                            loggedTimelineFor = srcId;
                            DI_LOG(L"[Media] timeline of %s: start %.1fs, position %.1fs, end %.1fs, seek %.1f-%.1fs",
                                   srcId.c_str(), startT / 1e7, timeline.Position().count() / 1e7,
                                   timeline.EndTime().count() / 1e7, timeline.MinSeekTime().count() / 1e7,
                                   timeline.MaxSeekTime().count() / 1e7);
                        }

                        std::lock_guard lock(g_stateMutex);
                        // Some sources (notably the Spotify desktop app) refresh LastUpdatedTime
                        // without moving Position(), which makes the estimate jump backwards.
                        // Hold off until a second poll confirms it is a real seek.
                        constexpr int64_t kJitterToleranceTicks = 15000000LL;   // 1.5s
                        constexpr int64_t kConfirmToleranceTicks = 5000000LL;   // 0.5s
                        const bool sameTrackPlaying =
                            npP && g_state.media.playing && ne == g_state.media.endTicks;

                        if (sameTrackPlaying) {
                            const int64_t predictedNow = g_state.media.positionTicks +
                                (static_cast<int64_t>(GetTickCount64()) - g_state.media.lastUpdatedTicks) * 10000;

                            if (np < predictedNow - kJitterToleranceTicks) {
                                const ULONGLONG nowTicks64 = GetTickCount64();
                                bool confirmed = false;
                                if (pendingSeekBaseline >= 0) {
                                    const int64_t expectedIfRealSeek = pendingSeekBaseline +
                                        (static_cast<int64_t>(nowTicks64) -
                                         static_cast<int64_t>(pendingSeekBaselineTickCount)) * 10000;
                                    const int64_t diff = np - expectedIfRealSeek;
                                    if ((diff < 0 ? -diff : diff) < kConfirmToleranceTicks) {
                                        confirmed = true;
                                    }
                                }

                                if (confirmed) {
                                    pendingSeekBaseline = -1;
                                } else {
                                    pendingSeekBaseline = np;
                                    pendingSeekBaselineTickCount = nowTicks64;
                                    np = predictedNow;
                                }
                            } else {
                                pendingSeekBaseline = -1;
                            }
                        } else {
                            pendingSeekBaseline = -1;
                        }
                        if (np != g_state.media.positionTicks ||
                            ne != g_state.media.endTicks ||
                            npP != g_state.media.playing) {
                            next.positionTicks = np;
                            next.endTicks = ne;
                            next.lastUpdatedTicks = GetTickCount64();
                        } else {
                            next.positionTicks = g_state.media.positionTicks;
                            next.endTicks = g_state.media.endTicks;
                            next.lastUpdatedTicks = g_state.media.lastUpdatedTicks;
                        }
                    }

                    next.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
                    next.sourceName = MediaSourceDisplayName(next.sourceAppUserModelId);

                    // Fallback for VLC, which often exposes a session with no
                    // Title metadata at all.
                    //
                    // The marker must still be at the END of the caption, just
                    // without insisting on the exact " - " separator that skins
                    // and fullscreen do not reliably produce. Matching the
                    // marker anywhere would happily pick up a browser tab named
                    // "How to use VLC media player - YouTube". The owning
                    // process is checked too, so only a real VLC window counts.
                    if (next.title.empty() && next.sourceName == L"VLC") {
                        const std::wstring marker = L"VLC media player";
                        HWND hwnd = nullptr;
                        while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
                            if (!IsWindowVisible(hwnd)) {
                                continue;
                            }

                            wchar_t windowTitle[512];
                            if (GetWindowTextW(hwnd, windowTitle, ARRAYSIZE(windowTitle)) <= 0) {
                                continue;
                            }

                            std::wstring caption(windowTitle);
                            while (!caption.empty() && caption.back() == L' ') {
                                caption.pop_back();
                            }
                            if (caption.size() <= marker.size() ||
                                caption.compare(caption.size() - marker.size(),
                                                marker.size(), marker) != 0) {
                                continue;
                            }

                            std::wstring image;
                            if (!ProcessImageNameForWindow(hwnd, &image) ||
                                ToLowerCopy(BaseNameFromPath(image)).find(L"vlc") == std::wstring::npos) {
                                continue;
                            }

                            // Everything before the app name is the media title,
                            // minus any trailing " - " separator.
                            std::wstring candidate = caption.substr(0, caption.size() - marker.size());
                            while (!candidate.empty() &&
                                   (candidate.back() == L' ' || candidate.back() == L'-')) {
                                candidate.pop_back();
                            }
                            if (!candidate.empty()) {
                                next.title = candidate;
                                break;
                            }
                        }
                    }

                    std::wstring prevSourceAppUserModelId;
                    bool hasPrevIcon = false;
                    PixelsRef prevIcon;
                    uint64_t prevIconGeneration = 0;
                    std::wstring prevBadge;

                    std::wstring prevTitle;
                    std::wstring prevArtist;
                    PixelsRef prevArt;
                    uint64_t prevArtGeneration = 0;
                    double prevArtChangedAt = 0.0;
                    double prevTitleChangedAt = 0.0;

                    bool prevPlaying = false;
                    {
                        std::lock_guard lock(g_stateMutex);
                        prevSourceAppUserModelId = g_state.media.sourceAppUserModelId;
                        hasPrevIcon = static_cast<bool>(g_state.media.sourceIcon);
                        prevIcon = g_state.media.sourceIcon;
                        prevIconGeneration = g_state.media.sourceIconGeneration;
                        prevBadge = g_state.media.sourceBadge;

                        prevTitle = g_state.media.title;
                        prevArtist = g_state.media.artist;
                        prevArt = g_state.media.art;
                        prevArtGeneration = g_state.media.artGeneration;
                        prevArtChangedAt = g_state.media.artChangedAt;
                        prevTitleChangedAt = g_state.media.titleChangedAt;
                        prevPlaying = g_state.media.playing;
                    }

                    if (next.sourceAppUserModelId == prevSourceAppUserModelId) {
                        next.sourceBadge = prevBadge;
                        next.sourceIcon = prevIcon;
                        next.sourceIconGeneration = prevIconGeneration;

                        if (!hasPrevIcon) {
                            next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                            next.sourceIconGeneration = next.sourceIcon->generation;
                        }
                    } else {
                        next.sourceBadge = MediaSourceBadge(next.sourceName);
                        if (IsBrowserMediaSource(next.sourceAppUserModelId)) {
                            next.sourceBadge = FindBrowserMediaSiteBadge(next.sourceAppUserModelId);
                        }
                        next.sourceIcon = FindMediaSourceIcon(next.sourceAppUserModelId);
                        next.sourceIconGeneration = next.sourceIcon->generation;
                    }

                    // Track when title/artist last changed. Browsers often update
                    // the text metadata a beat before they swap the artwork bytes,
                    // so we keep retrying the art fetch for a short "settle window"
                    // after any track change instead of locking onto the first
                    // (possibly stale/empty) result forever.
                    const bool sourceChanged = (next.sourceAppUserModelId != prevSourceAppUserModelId);
                    const bool metadataChanged = (sourceChanged || next.title != prevTitle || next.artist != prevArtist || (!prevPlaying && next.playing));
                    next.titleChangedAt = (metadataChanged && !next.title.empty() && next.playing) ? NowSeconds() : prevTitleChangedAt;
                    constexpr double kArtSettleSeconds = 3.0;
                    const bool artSettled = !metadataChanged &&
                        (NowSeconds() - prevTitleChangedAt) > kArtSettleSeconds;

                    if (artSettled && prevArt) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    } else if (auto thumbnail = properties.Thumbnail()) {
                        std::vector<uint8_t> bytes = ReadWinRtStreamBytes(thumbnail);
                        if (!bytes.empty()) {
                            BitmapPixels decoded;
                            if (DecodeImageBytesToPixels(bytes, &decoded)) {
                                next.art = PixelsRef(std::move(decoded));
                                next.artGeneration = next.art->generation;
                                next.artChangedAt = NowSeconds();
                            }
                        }
                    }

                    // If this particular poll's fetch came back empty (thumbnail
                    // not ready yet), don't flash a blank cover for a track that
                    // hasn't actually changed — keep the old art until a fresh
                    // fetch succeeds. But never do this across an actual track
                    // change, or we're back to showing the wrong song's art.
                    if (!next.art && prevArt && !metadataChanged) {
                        next.art = prevArt;
                        next.artGeneration = prevArtGeneration;
                        next.artChangedAt = prevArtChangedAt;
                    }
                }
            }
        } catch (...) {
            if (!loggedUnavailable) {
                DI_LOG(L"WinRT media session unavailable; media module will fall back to idle.");
                loggedUnavailable = true;
            }
        }

        bool trackJustChanged = false;
        {
            std::lock_guard lock(g_stateMutex);

            // A switch the user asked for is not a "new track": it must not auto-expand
            // the island or open the 5s recentTrackChange window.
            const bool userSourceSwitch =
                !g_state.media.sourceAppUserModelId.empty() &&
                next.sourceAppUserModelId != g_state.media.sourceAppUserModelId &&
                NowSeconds() - g_userSourceSwitchAt.load() < 4.0;
            if (userSourceSwitch) {
                next.titleChangedAt = g_state.media.titleChangedAt;
            }

            const bool isDifferentTrack = !userSourceSwitch && (!next.title.empty() && next.playing) &&
                (next.title != g_state.media.title || next.artist != g_state.media.artist || (!g_state.media.playing && next.playing));

            if (isDifferentTrack) {
                next.titleChangedAt = NowSeconds();
                trackJustChanged = true;
                g_idleTab = 0;
            }

            if (g_state.media.art &&
                next.title == g_state.media.title && next.artist == g_state.media.artist &&
                next.positionTicks >= g_state.media.positionTicks) {
                next.art = g_state.media.art;
                next.artGeneration = g_state.media.artGeneration;
                next.artChangedAt = g_state.media.artChangedAt;
            }
            if (!next.sourceIcon &&
                next.sourceAppUserModelId == g_state.media.sourceAppUserModelId) {
                next.sourceIcon = g_state.media.sourceIcon;
                next.sourceIconGeneration = g_state.media.sourceIconGeneration;
            }

            g_state.media = std::move(next);
            if (gotSources) {
                g_state.mediaSources = std::move(newSources);
            }
        }

        if (trackJustChanged) {
            g_layoutDirty = true;
            if (g_settings.mediaAutoExpand) {
                TriggerNudge();
            }
        }

        HANDLE mediaWaits[2] = {g_stopEvent, g_mediaRefreshEvent};
        WaitForMultipleObjects(g_mediaRefreshEvent ? 2 : 1, mediaWaits, FALSE, 1500);
    }

    winrt::uninit_apartment();
    return 0;
}

typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PWNF_USER_CALLBACK)(
    ULONG64 StateName,
    ULONG ChangeStamp,
    void* TypeId,
    void* CallbackContext,
    const void* Buffer,
    ULONG BufferSize
);

typedef NTSTATUS(NTAPI* PFN_RtlSubscribeWnfStateChangeNotification)(
    void** Subscription,
    ULONG64 StateName,
    ULONG ChangeStamp,
    PWNF_USER_CALLBACK Callback,
    void* CallbackContext,
    const void* TypeId,
    ULONG SerializationGroup,
    ULONG Unknown
);

typedef NTSTATUS(NTAPI* PFN_RtlUnsubscribeWnfStateChangeNotification)(
    void* Subscription
);

typedef NTSTATUS(NTAPI* PFN_NtQueryWnfStateData)(
    const ULONG64* StateName,
    const void* TypeId,
    const void* ExplicitScope,
    ULONG* ChangeStamp,
    void* Buffer,
    ULONG* BufferSize
);

constexpr ULONG64 kWnfQuietHoursActiveProfileChanged = 0xD83063EA3BF1C75ULL;

NTSTATUS NTAPI WnfDndCallback(
    ULONG64 stateName,
    ULONG changeStamp,
    void* typeId,
    void* callbackContext,
    const void* buffer,
    ULONG bufferSize
) {
    if (stateName != kWnfQuietHoursActiveProfileChanged) return 0;
    int val = 0;
    if (buffer && bufferSize >= sizeof(int)) {
        val = *reinterpret_cast<const int*>(buffer);
    }
    const bool active = (val != 0);
    static std::atomic<bool> s_firstWnf = true;
    if (s_firstWnf.exchange(false)) {
        g_isDnDActive.store(active);
        return 0;
    }

    // Only the state is kept: the Telegram / Discord cards honour Windows'
    // Do Not Disturb outside games (Telegram.RespectWindowsDnD).
    g_isDnDActive.store(active);
    return 0;
}

void SubscribeDndNotification() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return;

    auto pfnQuery = reinterpret_cast<PFN_NtQueryWnfStateData>(
        GetProcAddress(hNtdll, "NtQueryWnfStateData"));
    if (pfnQuery) {
        ULONG stamp = 0;
        ULONG size = sizeof(int);
        int val = 0;
        ULONG64 stateName = kWnfQuietHoursActiveProfileChanged;
        if (pfnQuery(&stateName, nullptr, nullptr, &stamp, &val, &size) == 0 && size >= sizeof(int)) {
            g_isDnDActive.store(val != 0);
        }
    }

    auto pfnSubscribe = reinterpret_cast<PFN_RtlSubscribeWnfStateChangeNotification>(
        GetProcAddress(hNtdll, "RtlSubscribeWnfStateChangeNotification"));
    if (!pfnSubscribe) return;

    pfnSubscribe(&g_wnfDndSubscription, kWnfQuietHoursActiveProfileChanged, 0,
                 WnfDndCallback, nullptr, nullptr, 0, 0);
}

void UnsubscribeDndNotification() {
    if (!g_wnfDndSubscription) return;
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pfnUnsubscribe = reinterpret_cast<PFN_RtlUnsubscribeWnfStateChangeNotification>(
            GetProcAddress(hNtdll, "RtlUnsubscribeWnfStateChangeNotification"));
        if (pfnUnsubscribe) {
            pfnUnsubscribe(g_wnfDndSubscription);
        }
    }
    g_wnfDndSubscription = nullptr;
}

// ═════════════════════════════════════════════════════════════════════════════
// CUSTOM FORK MODULES (Telegram / smart game mode / island Do Not Disturb)
//
// Everything new lives in the namespaces below so the original code only gains
// small, clearly marked hooks:
//   QuietMode        - the island's own Do Not Disturb (state + persistence)
//   GameDetector     - "is a game / fullscreen app in front?" (no rendering)
//   TelegramBanner   - turns the Windows banner for Telegram off / back on
//   TelegramSource   - recognises and parses Telegram toasts, publishes them
//   TelegramActions  - what a click on a Telegram card does
// Visibility decisions stay in RenderThreadProc; drawing stays in Renderer.
// ═════════════════════════════════════════════════════════════════════════════

namespace QuietMode {

// True once the island has shrunk away and the window must stay hidden.
bool HidesIsland() {
    return (g_quietMode.load() || g_exitRequested.load()) && g_quietCollapsed.load();
}

// True while the island is shrinking into itself before hiding.
bool IsCollapsing(double now) {
    return (g_quietMode.load() || g_exitRequested.load()) && !g_quietCollapsed.load() &&
           now >= g_quietHideAt.load();
}

// fromToggle: switched with the on-island toggle. The switch animates to "on"
// in place and the island shrinks a moment later. From the hotkey or the menu
// the island shrinks right away.
void Set(bool on, bool fromToggle = false) {
    const bool previous = g_quietMode.exchange(on);
    if (previous == on) {
        return;
    }
    di::state::SetInt(L"QuietMode", on ? 1 : 0);

    const double now = NowSeconds();
    // Toggle: just long enough for the knob to glide over, then shrink.
    const double onConfirmSeconds = fromToggle ? 0.45 : 0.0;
    g_quietCollapsed.store(false);
    g_quietHideAt.store(on ? now + onConfirmSeconds : 0.0);
    {
        std::lock_guard lock(g_stateMutex);
        if (on) {
            g_state.notification.active = false;
            g_state.notification.expiresAt = 0.0;
        }
    }
    g_layoutDirty = true;
    DI_LOG(L"Island Do Not Disturb: %s", on ? L"ON" : L"OFF");
    TriggerNudge();
    if (HWND hwnd = g_hwnd) {
        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
    }
}

void Toggle() {
    Set(!g_quietMode.load());
}

// Standalone 0.5: power button -- the Do Not Disturb shrink, then quit.
void RequestAnimatedExit() {
    if (g_exitRequested.exchange(true)) {
        return;
    }
    DI_LOG(L"Exit requested from the island (power button).");
    g_quietCollapsed.store(false);
    g_quietHideAt.store(NowSeconds());
    {
        std::lock_guard lock(g_stateMutex);
        g_state.notification.active = false;
        g_state.notification.expiresAt = 0.0;
    }
    g_layoutDirty = true;
    TriggerNudge();
    if (HWND hwnd = g_hwnd) {
        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
    }
    // Safety net: quit even if the render loop never reports the shrink done.
    std::thread([] {
        Sleep(2500);
        if (g_mainThreadId) PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0);
    }).detach();
}

}  // namespace QuietMode

namespace GameDetector {

// Windows' own opinion: a Direct3D fullscreen game, a fullscreen app
// ("busy") or presentation mode.
bool ShellReportsFullscreen() {
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&st))) {
        return false;
    }
    return st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_BUSY || st == QUNS_PRESENTATION_MODE;
}

bool ForegroundIsListedGame(HWND island, const std::vector<std::wstring>& list) {
    if (list.empty()) {
        return false;
    }
    HWND fg = GetForegroundWindow();
    if (!fg || fg == island) {
        return false;
    }
    std::wstring image;
    if (!ProcessImageNameForWindow(fg, &image)) {
        return false;
    }
    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
    return std::find(list.begin(), list.end(), base) != list.end();
}

// Drop-in replacement for "autoHideFullscreen && IsForegroundFullscreen()".
// With the extra signals switched off it returns exactly what the original did.
// Cheap: a few window queries, called at most twice a second.
bool IsGameOrFullscreen(HWND island) {
    bool autoHide = false;
    bool detectD3D = false;
    std::vector<std::wstring> list;
    {
        std::lock_guard lock(g_settingsMutex);
        autoHide = g_settings.autoHideFullscreen;
        detectD3D = g_settings.gameDetectD3D;
        list = g_settings.gameProcesses;
    }
    if (!autoHide) {
        return false;
    }
    if (IsForegroundFullscreen(island)) {
        return true;
    }
    if (detectD3D && ShellReportsFullscreen()) {
        return true;
    }
    return ForegroundIsListedGame(island, list);
}

}  // namespace GameDetector

namespace TelegramBanner {

// Per-app notification settings that Windows Settings itself writes.
// ShowBanner = 0 is "Show notification banners: off": the toast still lands in
// the notification centre (so the listener sees it) but never pops up.
constexpr wchar_t kSettingsKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
constexpr wchar_t kBannerValue[] = L"ShowBanner";
constexpr wchar_t kBackupValue[] = L"TelegramBannerBackup";

std::mutex g_bannerMutex;               // leaf lock
std::set<std::wstring> g_appliedLower;  // AUMIDs already handled this session
bool g_suppressActive = false;
// Turning the banner off is only safe once the notification listener has
// proven it works: the fallback path reads the banner window itself, so with
// a broken listener a hidden banner would mean no Telegram messages at all.
std::atomic<bool> g_wanted{false};          // any app wanted
std::atomic<bool> g_wantTelegram{false};
std::atomic<bool> g_wantDiscord{false};
std::atomic<bool> g_listenerReady{false};

struct BackupEntry {
    std::wstring aumid;
    int original = -1;  // -1 = the ShowBanner value did not exist
};

// Stored as "aumid|value;aumid|value" so it survives restarts and crashes.
std::vector<BackupEntry> LoadBackup() {
    std::vector<BackupEntry> out;
    const std::wstring all = di::state::GetString(kBackupValue);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find(L';', start);
        if (end == std::wstring::npos) {
            end = all.size();
        }
        const std::wstring item = all.substr(start, end - start);
        const size_t bar = item.rfind(L'|');
        if (bar != std::wstring::npos && bar > 0) {
            BackupEntry entry;
            entry.aumid = item.substr(0, bar);
            entry.original = static_cast<int>(wcstol(item.c_str() + bar + 1, nullptr, 10));
            out.push_back(std::move(entry));
        }
        start = end + 1;
    }
    return out;
}

void SaveBackup(const std::vector<BackupEntry>& entries) {
    std::wstring all;
    for (const BackupEntry& e : entries) {
        if (!all.empty()) {
            all += L';';
        }
        all += e.aumid + L"|" + std::to_wstring(e.original);
    }
    di::state::SetString(kBackupValue, all);
}

std::wstring KeyPathFor(const std::wstring& aumid) {
    return std::wstring(kSettingsKey) + L"\\" + aumid;
}

void ReadShowBanner(const std::wstring& aumid, DWORD* value, bool* exists) {
    *exists = false;
    *value = 1;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KeyPathFor(aumid).c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    DWORD type = 0;
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (RegQueryValueExW(key, kBannerValue, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
        *exists = true;
        *value = data;
    }
    RegCloseKey(key);
}

bool WriteShowBanner(const std::wstring& aumid, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KeyPathFor(aumid).c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = RegSetValueExW(key, kBannerValue, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

void DeleteShowBanner(const std::wstring& aumid) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KeyPathFor(aumid).c_str(), 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, kBannerValue);
        RegCloseKey(key);
    }
}

std::vector<std::wstring> FindKeysContaining(const wchar_t* needle);

std::vector<std::wstring> FindTelegramKeys() {
    return FindKeysContaining(L"telegram");
}

std::vector<std::wstring> FindKeysContaining(const wchar_t* needle) {
    std::vector<std::wstring> out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS) {
        return out;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[512] = {};
        DWORD nameLen = ARRAYSIZE(name);
        const LSTATUS st = RegEnumKeyExW(key, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (st != ERROR_SUCCESS) {
            break;
        }
        if (ToLowerCopy(name).find(needle) != std::wstring::npos) {
            out.emplace_back(name);
        }
    }
    RegCloseKey(key);
    return out;
}

void ApplyLocked(const std::wstring& aumid, std::vector<BackupEntry>& backup) {
    const std::wstring lower = ToLowerCopy(aumid);
    if (aumid.empty() || g_appliedLower.count(lower)) {
        return;
    }
    g_appliedLower.insert(lower);

    DWORD current = 1;
    bool exists = false;
    ReadShowBanner(aumid, &current, &exists);

    const bool known = std::any_of(backup.begin(), backup.end(), [&](const BackupEntry& e) {
        return EqualsNoCase(e.aumid, aumid);
    });
    if (!known) {
        backup.push_back({aumid, exists ? static_cast<int>(current) : -1});
        SaveBackup(backup);
    }
    if (!exists || current != 0) {
        const bool ok = WriteShowBanner(aumid, 0);
        DI_LOG(L"[Banner] Windows banner for '%s' turned off: %s", aumid.c_str(),
               ok ? L"ok" : L"FAILED");
    }
}

void RestoreLocked(std::vector<BackupEntry>& backup) {
    for (const BackupEntry& e : backup) {
        if (e.original < 0) {
            DeleteShowBanner(e.aumid);
        } else {
            WriteShowBanner(e.aumid, static_cast<DWORD>(e.original));
        }
        DI_LOG(L"[Banner] Windows banner for '%s' restored.", e.aumid.c_str());
    }
    backup.clear();
    SaveBackup(backup);
    g_appliedLower.clear();
}

bool AumidWanted(const std::wstring& aumid) {
    const std::wstring lower = ToLowerCopy(aumid);
    return (g_wantTelegram.load() && lower.find(L"telegram") != std::wstring::npos) ||
           (g_wantDiscord.load() && lower.find(L"discord") != std::wstring::npos);
}

// Brings the Windows banner settings in line with what is wanted: apps that
// should be silent get ShowBanner = 0, everything else in the backup is
// restored. Nothing is suppressed until the listener has proven it works.
void ReconcileLocked() {
    const bool ready = g_listenerReady.load();
    std::vector<BackupEntry> backup = LoadBackup();

    std::vector<BackupEntry> keep;
    std::vector<BackupEntry> restore;
    for (const BackupEntry& e : backup) {
        (ready && AumidWanted(e.aumid) ? keep : restore).push_back(e);
    }
    if (!restore.empty()) {
        RestoreLocked(restore);  // also clears g_appliedLower
        SaveBackup(keep);
    }
    backup = keep;
    g_appliedLower.clear();

    g_suppressActive = ready && g_wanted.load();
    if (!g_suppressActive) {
        return;
    }
    std::vector<std::wstring> keys;
    if (g_wantTelegram.load()) {
        for (std::wstring& k : FindKeysContaining(L"telegram")) keys.push_back(std::move(k));
    }
    if (g_wantDiscord.load()) {
        for (std::wstring& k : FindKeysContaining(L"discord")) keys.push_back(std::move(k));
    }
    for (const std::wstring& name : keys) {
        ApplyLocked(name, backup);
    }
}

void Sync(bool suppressTelegram, bool suppressDiscord) {
    std::lock_guard lock(g_bannerMutex);
    g_wantTelegram = suppressTelegram;
    g_wantDiscord = suppressDiscord;
    g_wanted = suppressTelegram || suppressDiscord;
    ReconcileLocked();
}

// Called by the listener thread after its first successful read.
void OnListenerReady() {
    if (g_listenerReady.exchange(true)) {
        return;
    }
    DI_LOG(L"[Banner] Notification listener works; Windows banner suppression: Telegram %s, Discord %s.",
           g_wantTelegram.load() ? L"on" : L"off", g_wantDiscord.load() ? L"on" : L"off");
    std::lock_guard lock(g_bannerMutex);
    ReconcileLocked();
}

// Called for every Telegram toast: learns the AUMID if Telegram had never
// shown a notification before the mod started.
void EnsureFor(const std::wstring& aumid) {
    std::lock_guard lock(g_bannerMutex);
    if (!g_suppressActive || aumid.empty() || !AumidWanted(aumid) || !di::security::IsSafeAumid(aumid)) {
        return;
    }
    if (g_appliedLower.count(ToLowerCopy(aumid))) {
        return;
    }
    std::vector<BackupEntry> backup = LoadBackup();
    ApplyLocked(aumid, backup);
}

// Mod unload / disable: give Telegram its normal Windows banners back.
void RestoreAll() {
    std::lock_guard lock(g_bannerMutex);
    std::vector<BackupEntry> backup = LoadBackup();
    if (!backup.empty()) {
        RestoreLocked(backup);
    }
    g_suppressActive = false;
}

}  // namespace TelegramBanner

namespace TelegramSource {

bool IsTelegramApp(const std::wstring& aumid, const std::wstring& displayName) {
    return ToLowerCopy(aumid).find(L"telegram") != std::wstring::npos ||
           ToLowerCopy(displayName).find(L"telegram") != std::wstring::npos;
}

// One line, single spaces, capped length.
std::wstring Collapse(const std::wstring& in, size_t maxChars) {
    std::wstring out;
    out.reserve(in.size());
    bool pendingSpace = false;
    for (wchar_t c : in) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ' || c == 0x00A0) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    if (out.size() > maxChars) {
        out.resize(maxChars);
        out += L"…";
    }
    return out;
}

// Telegram Desktop's native toast has up to three text lines:
//   private chat: [sender, text]            (empty subtitle is dropped)
//   group / channel: [chat title, sender, text]
// The exact shape is confirmed with Telegram.DebugLog (see PROJECT_CONTEXT.md).
TelegramLine ParseLine(const std::vector<std::wstring>& raw, const Settings& settings) {
    std::vector<std::wstring> t;
    for (const std::wstring& item : raw) {
        std::wstring c = Collapse(item, 300);
        if (!c.empty()) {
            t.push_back(std::move(c));
        }
    }

    TelegramLine line;
    if (t.size() >= 3) {
        line.chat = t[0];
        line.sender = t[1];
        line.text = t[2];
        for (size_t i = 3; i < t.size(); ++i) {
            line.text += L" " + t[i];
        }
    } else if (t.size() == 2) {
        line.sender = t[0];
        line.text = t[1];
    } else if (t.size() == 1) {
        line.sender = t[0];
    }

    line.group = !line.chat.empty() && !line.sender.empty() && !EqualsNoCase(line.chat, line.sender);
    if (!line.group) {
        if (line.sender.empty()) {
            line.sender = line.chat;
        }
        line.chat.clear();
    }
    if (line.sender.empty()) {
        line.sender = L"Telegram";
    }
    if (!settings.telegramShowText || line.text.empty()) {
        line.text = L"Новое сообщение";
    }
    return line;
}

bool IsDiscordApp(const std::wstring& aumid, const std::wstring& displayName) {
    return ToLowerCopy(aumid).find(L"discord") != std::wstring::npos ||
           ToLowerCopy(displayName).find(L"discord") != std::wstring::npos;
}

// Discord's toast: [title, message]. In a server channel the title is
// "Sender (#channel, Server)" (or "Sender (#channel)"); in DMs just "Sender".
TelegramLine ParseDiscordLine(const std::vector<std::wstring>& raw, const Settings& settings) {
    std::vector<std::wstring> t;
    for (const std::wstring& item : raw) {
        std::wstring c = Collapse(item, 300);
        if (!c.empty()) {
            t.push_back(std::move(c));
        }
    }
    TelegramLine line;
    if (!t.empty()) {
        const std::wstring& title = t[0];
        const size_t open = title.rfind(L" (");
        if (open != std::wstring::npos && open > 0 && title.back() == L')') {
            line.sender = title.substr(0, open);
            line.chat = title.substr(open + 2, title.size() - open - 3);
            line.group = !line.chat.empty();
        } else {
            line.sender = title;
        }
        for (size_t i = 1; i < t.size(); ++i) {
            if (!line.text.empty()) line.text += L" ";
            line.text += t[i];
        }
    }
    if (line.sender.empty()) {
        line.sender = L"Discord";
    }
    if (!settings.telegramShowText || line.text.empty()) {
        line.text = L"Новое сообщение";
    }
    return line;
}

// Adds the message to the card: a new card, or stacked onto the one still up.
void Publish(const TelegramLine& line, const std::wstring& aumid, const Settings& settings,
             const std::wstring& chatApp = L"Telegram") {
    const double now = NowSeconds();
    {
        std::lock_guard lock(g_stateMutex);
        // The same message can arrive twice (listener + toast window): drop
        // repeats seen within the last 10 seconds. Guarded by g_stateMutex.
        static std::vector<std::pair<std::wstring, double>> s_recent;
        const std::wstring key = chatApp + L"\x1F" + line.chat + L"\x1F" + line.sender + L"\x1F" + line.text;
        s_recent.erase(std::remove_if(s_recent.begin(), s_recent.end(),
                                      [&](const std::pair<std::wstring, double>& r) {
                                          return now - r.second > 10.0;
                                      }),
                       s_recent.end());
        for (const auto& r : s_recent) {
            if (r.first == key) {
                return;
            }
        }
        s_recent.emplace_back(key, now);

        NotificationSnapshot& n = g_state.notification;
        // Messages stack only onto a card of the same app.
        const bool stacking = n.active && n.isTelegram && n.chatApp == chatApp && now < n.expiresAt;
        if (!stacking) {
            n = NotificationSnapshot();
            n.active = true;
            n.isTelegram = true;
            n.chatApp = chatApp;
            n.app = chatApp;
        }
        n.sourceAumid = aumid;
        // Standalone 0.9: text from the network is cut to a sane size before it
        // is stored or drawn (a giant message can't stall layout).
        TelegramLine safeLine = line;
        safeLine.text = di::security::Clip(std::move(safeLine.text), 1000);
        safeLine.sender = di::security::Clip(std::move(safeLine.sender), 128);
        safeLine.chat = di::security::Clip(std::move(safeLine.chat), 128);
        n.telegramLines.insert(n.telegramLines.begin(), std::move(safeLine));
        const size_t maxLines = static_cast<size_t>(std::max(1, settings.telegramMaxMessages));
        if (n.telegramLines.size() > maxLines) {
            n.telegramLines.resize(maxLines);
        }
        n.telegramTotal += 1;
        n.ttl = static_cast<float>(settings.telegramDisplaySeconds);
        n.expiresAt = now + n.ttl;
        n.title = (line.group ? line.chat + L" — " : std::wstring()) + line.sender + L": " + line.text;
        n.body.clear();
    }
    g_layoutDirty = true;
    TriggerNudge();
}

bool IsDismissButton(const std::wstring& name, const std::wstring& automationId) {
    const std::wstring n = ToLowerCopy(name);
    const std::wstring a = ToLowerCopy(automationId);
    return a.find(L"dismiss") != std::wstring::npos || a.find(L"close") != std::wstring::npos ||
           n.find(L"закрыть") != std::wstring::npos || n.find(L"скрыть") != std::wstring::npos ||
           n.find(L"dismiss") != std::wstring::npos || n.find(L"close") != std::wstring::npos;
}

// Path 2 (works without the notification listener): Windows just showed a
// toast window and the banner watcher (ToastWatcher) handed it to us. If it is Telegram's, read
// its text through UI Automation, show the Telegram card and close the
// Windows banner with its own "close" button (it stays in the notification
// centre). Runs on the UIA helper thread with COM initialised.
bool TryHandleToastWindow(IUIAutomation* uia, IUIAutomationElement* windowEl) {
    const Settings settings = GetSettingsCopy();
    if (!uia || !windowEl || !settings.telegramEnabled || g_quietMode.load()) {
        return false;
    }

    IUIAutomationCondition* cond = nullptr;
    uia->CreateTrueCondition(&cond);
    if (!cond) {
        return false;
    }
    IUIAutomationElementArray* elements = nullptr;
    const HRESULT hr = windowEl->FindAll(TreeScope_Descendants, cond, &elements);
    cond->Release();
    if (FAILED(hr) || !elements) {
        return false;
    }

    std::vector<std::wstring> allNames;
    std::vector<std::wstring> texts;
    IUIAutomationElement* dismissButton = nullptr;
    int count = 0;
    elements->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(elements->GetElement(i, &el)) || !el) {
            continue;
        }
        BSTR nameBstr = nullptr;
        BSTR idBstr = nullptr;
        CONTROLTYPEID type = 0;
        el->get_CurrentName(&nameBstr);
        el->get_CurrentAutomationId(&idBstr);
        el->get_CurrentControlType(&type);
        const std::wstring name = nameBstr ? nameBstr : L"";
        const std::wstring automationId = idBstr ? idBstr : L"";
        if (nameBstr) SysFreeString(nameBstr);
        if (idBstr) SysFreeString(idBstr);

        if (settings.telegramDebugLog) {
            DI_LOG(L"[Telegram/UIA]   #%d type=%d id='%s' name='%s'", i, static_cast<int>(type),
                   automationId.c_str(), name.c_str());
        }
        if (!name.empty()) {
            allNames.push_back(name);
            if (type == UIA_TextControlTypeId) {
                texts.push_back(name);
            }
        }
        if (type == UIA_ButtonControlTypeId && !dismissButton && IsDismissButton(name, automationId)) {
            dismissButton = el;
            dismissButton->AddRef();
        }
        el->Release();
    }
    elements->Release();

    // Is it Telegram's toast? The app name is among the first labels.
    bool isTelegram = false;
    for (size_t i = 0; i < allNames.size() && i < 6; ++i) {
        if (ToLowerCopy(allNames[i]).find(L"telegram") != std::wstring::npos) {
            isTelegram = true;
            break;
        }
    }
    if (!isTelegram) {
        if (dismissButton) dismissButton->Release();
        return false;
    }

    // Drop the app-name label(s) and duplicates; what is left is title / text.
    std::vector<std::wstring> payload;
    for (const std::wstring& t : texts) {
        const std::wstring lower = ToLowerCopy(t);
        if (lower == L"telegram" || lower == L"telegram desktop" ||
            (payload.empty() && !allNames.empty() && t == allNames.front() &&
             lower.find(L"telegram") != std::wstring::npos)) {
            continue;
        }
        if (std::find(payload.begin(), payload.end(), t) == payload.end()) {
            payload.push_back(t);
        }
    }
    if (settings.telegramDebugLog) {
        DI_LOG(L"[Telegram/UIA] toast window: %u labels, %u text lines used",
               static_cast<unsigned>(allNames.size()), static_cast<unsigned>(payload.size()));
    }
    if (payload.empty()) {
        if (dismissButton) dismissButton->Release();
        return false;
    }

    Publish(ParseLine(payload, settings), L"", settings);

    if (settings.telegramSuppressBanner && dismissButton) {
        IUIAutomationInvokePattern* invoke = nullptr;
        if (SUCCEEDED(dismissButton->GetCurrentPatternAs(UIA_InvokePatternId,
                                                         __uuidof(IUIAutomationInvokePattern),
                                                         reinterpret_cast<void**>(&invoke))) &&
            invoke) {
            invoke->Invoke();
            invoke->Release();
        }
    } else if (settings.telegramSuppressBanner && settings.telegramDebugLog) {
        DI_LOG(L"[Telegram/UIA] no close button found on the Windows banner.");
    }
    if (dismissButton) dismissButton->Release();
    return true;
}

}  // namespace TelegramSource

namespace TelegramActions {

bool IsTelegramExe(const std::wstring& baseLower) {
    return baseLower == L"telegram.exe" || baseLower == L"ayugram.exe" ||
           baseLower == L"kotatogram.exe" || baseLower == L"64gram.exe";
}

struct TelegramWindowSearch {
    HWND visible = nullptr;
    std::wstring exePath;
};

BOOL CALLBACK EnumTelegramWindows(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<TelegramWindowSearch*>(lParam);
    if (GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    std::wstring image;
    if (!ProcessImageNameForWindow(hwnd, &image) ||
        !IsTelegramExe(ToLowerCopy(BaseNameFromPath(image)))) {
        return TRUE;
    }
    search->exePath = image;
    if (IsWindowVisible(hwnd)) {
        search->visible = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Brings Telegram to the front. Opening the exact chat is not possible from a
// notification we only read (see PROJECT_CONTEXT.md, limitations).
void OpenTelegram(const std::wstring& aumid) {
    TelegramWindowSearch search;
    EnumWindows(EnumTelegramWindows, reinterpret_cast<LPARAM>(&search));

    if (search.visible) {
        if (IsIconic(search.visible)) {
            ShowWindow(search.visible, SW_RESTORE);
        }
        if (SetForegroundWindow(search.visible)) {
            return;
        }
    }
    // Hidden in the tray (or focus was refused): starting the exe again makes
    // the running Telegram instance show its window.
    if (!search.exePath.empty()) {
        ShellExecuteW(nullptr, L"open", search.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!aumid.empty() && di::security::IsSafeAumid(aumid)) {
        const std::wstring shellPath = L"shell:AppsFolder\\" + aumid;
        ShellExecuteW(nullptr, L"open", shellPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

struct DiscordWindowSearch {
    HWND visible = nullptr;
    std::wstring exePath;
};

BOOL CALLBACK EnumDiscordWindows(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<DiscordWindowSearch*>(lParam);
    if (GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    std::wstring image;
    if (!ProcessImageNameForWindow(hwnd, &image)) {
        return TRUE;
    }
    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
    if (base != L"discord.exe" && base != L"discordptb.exe" && base != L"discordcanary.exe") {
        return TRUE;
    }
    search->exePath = image;
    if (IsWindowVisible(hwnd)) {
        search->visible = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Brings Discord to the front; if it sits in the tray, starting its exe again
// makes the running instance show its window.
void OpenDiscord(const std::wstring& aumid) {
    DiscordWindowSearch search;
    EnumWindows(EnumDiscordWindows, reinterpret_cast<LPARAM>(&search));
    if (search.visible) {
        if (IsIconic(search.visible)) {
            ShowWindow(search.visible, SW_RESTORE);
        }
        if (SetForegroundWindow(search.visible)) {
            return;
        }
    }
    if (!search.exePath.empty()) {
        ShellExecuteW(nullptr, L"open", search.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!aumid.empty() && di::security::IsSafeAumid(aumid)) {
        const std::wstring shellPath = L"shell:AppsFolder\\" + aumid;
        ShellExecuteW(nullptr, L"open", shellPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace TelegramActions

// Path 2b: Windows 11 reuses its banner window, so a "window created" event
// would fire once at most; EVENT_OBJECT_SHOW fires on every banner. A
// dedicated thread owns the out-of-context WinEvent hook (callbacks arrive
// through its message loop). Only Telegram is handled here; since custom.16
// the original shell-hook capture of every toast window is gone.
namespace ToastWatcher {

HANDLE g_thread = nullptr;
DWORD g_threadId = 0;

bool IsShellHostProcess(HWND hwnd) {
    std::wstring image;
    if (!ProcessImageNameForWindow(hwnd, &image)) {
        return false;
    }
    const std::wstring base = ToLowerCopy(BaseNameFromPath(image));
    return base == L"shellexperiencehost.exe" || base == L"explorer.exe" || base == L"sihost.exe" ||
           base == L"shellhost.exe";
}

// Reads the banner through UI Automation on a short-lived helper thread.
void ReadWindowAsync(HWND hwnd) {
    static std::mutex s_mutex;
    static HWND s_lastHwnd = nullptr;
    static double s_lastAt = -10.0;
    {
        std::lock_guard lock(s_mutex);
        const double now = NowSeconds();
        if (hwnd == s_lastHwnd && now - s_lastAt < 1.0) {
            return;  // the same show reported twice
        }
        s_lastHwnd = hwnd;
        s_lastAt = now;
    }
    std::thread([hwnd]() {
        // Wait for XAML to fill in the text, then try once more if it was empty.
        for (int attempt = 0; attempt < 2; ++attempt) {
            Sleep(attempt == 0 ? 250 : 400);
            if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
                return;
            }
            bool handled = false;
            const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUIAutomation* uia = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                           __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia))) &&
                uia) {
                IUIAutomationElement* windowEl = nullptr;
                if (SUCCEEDED(uia->ElementFromHandle(hwnd, &windowEl)) && windowEl) {
                    handled = TelegramSource::TryHandleToastWindow(uia, windowEl);
                    windowEl->Release();
                }
                uia->Release();
            }
            if (SUCCEEDED(hrCo)) {
                CoUninitialize();
            }
            if (handled) {
                return;
            }
        }
    }).detach();
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                         DWORD) {
    if (event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || hwnd == g_hwnd) {
        return;
    }
    // Cheap class filter first: this event fires for every window shown.
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    const std::wstring lowerCls = ToLowerCopy(cls);
    const bool candidateClass = lowerCls == L"windows.ui.core.corewindow" ||
                                lowerCls.find(L"xaml_windowedpopupclass") != std::wstring::npos ||
                                lowerCls.find(L"toast") != std::wstring::npos ||
                                lowerCls.find(L"notification") != std::wstring::npos;
    if (!candidateClass) {
        return;
    }
    if (g_quietMode.load() || !GetSettingsCopy().telegramEnabled) {
        return;
    }
    // A banner is small; the Start menu, search and the notification centre are not.
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || w > 900 || h > 700) {
        return;
    }
    if (!IsShellHostProcess(hwnd)) {
        return;
    }
    if (GetSettingsCopy().telegramDebugLog) {
        wchar_t title[128] = {};
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        DI_LOG(L"[Telegram/Watch] shown: class='%s' title='%s' size=%dx%d", cls, title, w, h);
    }
    ReadWindowAsync(hwnd);
}

DWORD WINAPI ThreadProc(void*) {
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWinEvent, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    DI_LOG(L"[Telegram/Watch] banner watcher %s.", hook ? L"started" : L"FAILED to start");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hook) {
        UnhookWinEvent(hook);
    }
    return 0;
}

void Start() {
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, &g_threadId);
}

void Stop() {
    if (g_threadId) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
    }
    g_thread = nullptr;
    g_threadId = 0;
}

}  // namespace ToastWatcher

namespace TelegramLook {

// Up to two initials ("Иван Петров" -> "ИП"). IsCharAlphaNumericW / CharUpperBuffW
// are locale-independent, so Cyrillic works too.
std::wstring Initials(const std::wstring& name) {
    std::wstring out;
    bool wordStart = true;
    for (wchar_t c : name) {
        if (c == L' ' || c == L'\t' || c == L'-' || c == L'_') {
            wordStart = true;
            continue;
        }
        if (wordStart && c >= 0x20 && (c < 0xD800 || c > 0xDFFF) && IsCharAlphaNumericW(c)) {
            out.push_back(c);
            if (out.size() == 2) {
                break;
            }
        }
        wordStart = false;
    }
    if (out.empty()) {
        out = L"T";
    }
    CharUpperBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

// Stable colour per name, from Telegram's own userpic palette.
D2D1_COLOR_F AvatarColor(const std::wstring& name) {
    static const UINT32 kPalette[] = {0xE17076, 0xFAA774, 0xA695E7, 0x7BC862,
                                      0x6EC9CB, 0x65AADD, 0xEE7AAE};
    uint32_t hash = 2166136261u;
    for (wchar_t c : name) {
        hash = (hash ^ static_cast<uint32_t>(c)) * 16777619u;
    }
    return D2D1::ColorF(kPalette[hash % ARRAYSIZE(kPalette)]);
}

}  // namespace TelegramLook

#if DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER
namespace TelegramSource {

// Handles one Telegram toast seen by the listener thread.
void Handle(const winrt::Windows::UI::Notifications::Management::UserNotificationListener& listener,
            const winrt::Windows::UI::Notifications::UserNotification& userNotification,
            const std::wstring& aumid, const std::wstring& displayName, const Settings& settings,
            const std::wstring& chatApp = L"Telegram") {
    using winrt::Windows::UI::Notifications::KnownNotificationBindings;
    const bool discord = chatApp == L"Discord";

    std::vector<std::wstring> texts;
    try {
        auto binding = userNotification.Notification().Visual().GetBinding(
            KnownNotificationBindings::ToastGeneric());
        if (binding) {
            auto elements = binding.GetTextElements();
            for (uint32_t i = 0; i < elements.Size(); ++i) {
                texts.emplace_back(elements.GetAt(i).Text().c_str());
            }
        }
    } catch (...) {
    }

    const uint32_t id = userNotification.Id();
    if (settings.telegramDebugLog) {
        DI_LOG(L"[%s] toast id=%u aumid='%s' app='%s' texts=%u", chatApp.c_str(), id, aumid.c_str(),
               displayName.c_str(), static_cast<unsigned>(texts.size()));
        for (size_t i = 0; i < texts.size(); ++i) {
            DI_LOG(L"[%s]   text[%u] = '%s'", chatApp.c_str(), static_cast<unsigned>(i), texts[i].c_str());
        }
    }

    if (discord ? settings.discordSuppressBanner : settings.telegramSuppressBanner) {
        TelegramBanner::EnsureFor(aumid);
    }

    // Windows turns its own Do Not Disturb on by itself while a game runs, so
    // it is only honoured outside games (Feature 2 wants messages over games).
    const bool inGame = g_isFullscreen.load(std::memory_order_relaxed);
    if (settings.telegramRespectWindowsDnd && g_isDnDActive.load() && !inGame) {
        if (settings.telegramDebugLog) {
            DI_LOG(L"[Telegram]   skipped: Windows Do Not Disturb is on.");
        }
        return;
    }
    if (texts.empty()) {
        return;  // nothing readable (calls / service toasts)
    }

    TelegramSource::Publish(discord ? ParseDiscordLine(texts, settings) : ParseLine(texts, settings), aumid,
                            settings, chatApp);

    if (settings.telegramClearFromCenter) {
        try {
            listener.RemoveNotification(id);
        } catch (...) {
        }
    }
}

// For notifications whose app Windows will not name (AppInfo fails for
// non-Store apps such as Telegram Desktop and Discord -- seen as app='?').
// Windows stamps HKCU\\...\\Notifications\\Settings\\<AUMID>\\LastNotificationAddedTime
// whenever an app adds a notification; the app whose stamp is closest to this
// notification's creation time (within 5 s) is the sender. Only if Windows
// keeps no stamps at all: "Telegram is running" as a last resort.
bool IdentifyUnknownApp(uint64_t creationFileTime, std::wstring* aumidOut, const wchar_t** ruleOut) {
    constexpr uint64_t kWindow = 5ULL * 10000000ULL;  // 5 s in FILETIME units
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TelegramBanner::kSettingsKey, 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &root) != ERROR_SUCCESS) {
        return false;
    }
    bool anyStamp = false;
    uint64_t bestDiff = kWindow + 1;
    std::wstring best;
    for (DWORD index = 0;; ++index) {
        wchar_t name[512] = {};
        DWORD nameLen = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        uint64_t stamp = 0;
        DWORD type = 0;
        DWORD size = sizeof(stamp);
        if (RegGetValueW(root, name, L"LastNotificationAddedTime", RRF_RT_REG_QWORD | RRF_RT_REG_BINARY,
                         &type, &stamp, &size) != ERROR_SUCCESS ||
            size != sizeof(stamp) || stamp == 0) {
            continue;
        }
        anyStamp = true;
        const uint64_t diff = stamp > creationFileTime ? stamp - creationFileTime : creationFileTime - stamp;
        if (diff < bestDiff) {
            bestDiff = diff;
            best = name;
        }
    }
    RegCloseKey(root);
    if (!best.empty() && bestDiff <= kWindow) {
        *aumidOut = best;
        *ruleOut = L"registry time stamp";
        return true;
    }
    if (!anyStamp) {
        TelegramActions::TelegramWindowSearch search;
        EnumWindows(TelegramActions::EnumTelegramWindows, reinterpret_cast<LPARAM>(&search));
        if (!search.exePath.empty()) {
            *aumidOut = L"Telegram.TelegramDesktop";
            *ruleOut = L"Telegram is running (Windows keeps no time stamps)";
            return true;
        }
    }
    return false;
}

// Readable name for an AUMID, e.g. "com.squirrel.Discord.Discord" -> "Discord".
std::wstring FriendlyNameFromAumid(const std::wstring& aumid) {
    struct Known {
        const wchar_t* needle;
        const wchar_t* name;
    };
    static const Known kKnown[] = {
        {L"discord", L"Discord"},       {L"telegram", L"Telegram"},     {L"whatsapp", L"WhatsApp"},
        {L"slack", L"Slack"},           {L"spotify", L"Spotify"},       {L"viber", L"Viber"},
        {L"steam", L"Steam"},           {L"chrome", L"Google Chrome"},  {L"msedge", L"Microsoft Edge"},
        {L"firefox", L"Firefox"},       {L"outlook", L"Outlook"},       {L"teams", L"Microsoft Teams"},
    };
    const std::wstring lower = ToLowerCopy(aumid);
    for (const Known& k : kKnown) {
        if (lower.find(k.needle) != std::wstring::npos) {
            return k.name;
        }
    }
    // Otherwise the last meaningful part: "Vendor.App" -> "App", "Pkg!App" -> "App".
    size_t cut = aumid.find_last_of(L".!\\");
    std::wstring tail = cut == std::wstring::npos ? aumid : aumid.substr(cut + 1);
    if (tail.size() < 3 || EqualsNoCase(tail, L"exe") || EqualsNoCase(tail, L"app")) {
        tail = aumid;
    }
    return tail;
}

}  // namespace TelegramSource
#endif

#if DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER
DWORD WINAPI NotificationThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::UI::Notifications::KnownNotificationBindings;
    using winrt::Windows::UI::Notifications::NotificationKinds;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListener;
    using winrt::Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus;

    std::set<uint32_t> seenIds;
    bool firstPoll = true;
    bool accessLogged = false;

    // The Windows Notification Service (WNS) and UWP subsystem take time to initialize on boot.
    // If this runs too early, instantiating UserNotificationListener::Current()
    // can permanently bind to an uninitialized COM proxy, permanently breaking notifications for the process.
    // To prevent this, we enforce a strict 30-second delay from process creation before touching the API.
    FILETIME creationTime, exitTime, kernelTime, userTime;
    if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER ct;
        ct.LowPart = creationTime.dwLowDateTime;
        ct.HighPart = creationTime.dwHighDateTime;

        FILETIME systemTime;
        GetSystemTimeAsFileTime(&systemTime);
        ULARGE_INTEGER st;
        st.LowPart = systemTime.dwLowDateTime;
        st.HighPart = systemTime.dwHighDateTime;

        uint64_t msSinceProcessStart = (st.QuadPart - ct.QuadPart) / 10000;
        // Custom fork: the wait protects against a cold boot only. Without the
        // uptime check every mod restart / recompile went deaf for 30 seconds,
        // and messages from that window were then filed as "old".
        if (msSinceProcessStart < 30000 && GetTickCount64() < 3ULL * 60ULL * 1000ULL) {
            DWORD waitTime = 30000 - (DWORD)msSinceProcessStart;
            DI_LOG(L"Process started recently. Delaying UserNotificationListener init by %d ms to let UWP subsystem load...", waitTime);
            WaitForSingleObject(g_stopEvent, waitTime);
        }
    }

    DI_LOG(L"[Notif] Connecting to the Windows notification listener...");  // custom fork
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        try {
            auto listener = UserNotificationListener::Current();
            // Custom fork: GetAccessStatus() is synchronous; RequestAccessAsync()
            // is documented for UI threads and can hang forever on a background
            // thread of a non-packaged process (it did on the user's PC: the
            // module never logged anything). Bounded wait + explicit log.
            auto access = listener.GetAccessStatus();
            if (access != UserNotificationListenerAccessStatus::Allowed) {
                auto request = listener.RequestAccessAsync();
                if (request.wait_for(std::chrono::seconds(10)) ==
                    winrt::Windows::Foundation::AsyncStatus::Completed) {
                    access = request.GetResults();
                } else {
                    try {
                        request.Cancel();
                    } catch (...) {
                    }
                    if (!accessLogged) {
                        DI_LOG(L"[Notif] RequestAccessAsync did not answer within 10 s "
                               L"(status %d). The Windows notification listener is unavailable to "
                               L"this process; Telegram falls back to reading the banner window.",
                               static_cast<int>(access));
                        accessLogged = true;
                    }
                    WaitForSingleObject(g_stopEvent, 60000);
                    continue;
                }
            }
            {
                static int s_loggedAccess = -1;
                if (s_loggedAccess != static_cast<int>(access)) {
                    s_loggedAccess = static_cast<int>(access);
                    DI_LOG(L"[Notif] Notification listener access status: %d "
                           L"(0 = not decided, 1 = allowed, 2 = denied)", s_loggedAccess);
                }
            }
            if (access != UserNotificationListenerAccessStatus::Allowed) {
                if (!accessLogged) {
                    if (access == UserNotificationListenerAccessStatus::Denied) {
                        // Actionable: this is a Windows privacy switch, not a
                        // mod setting, and it is the usual reason the module
                        // stays silent when everything else is configured.
                        DI_LOG(L"Notification listener permission DENIED by Windows. "
                               L"Enable Settings > Privacy & security > Notifications > "
                               L"\"Let apps access your notifications\", then restart the mod. "
                               L"Retrying meanwhile...");
                    } else {
                        DI_LOG(L"Notification listener permission not granted or UWP subsystem not ready on boot (status: %d); retrying connection loop...", (int)access);
                    }
                    accessLogged = true;
                }
                WaitForSingleObject(g_stopEvent, 3000);
                continue;
            }

            if (accessLogged) {
                DI_LOG(L"WinRT UserNotificationListener successfully connected.");
                accessLogged = false;
            }

            while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                try {
                    // Custom fork: bounded wait instead of .get(), see above.
                    auto notificationsOp = listener.GetNotificationsAsync(NotificationKinds::Toast);
                    if (notificationsOp.wait_for(std::chrono::seconds(5)) !=
                        winrt::Windows::Foundation::AsyncStatus::Completed) {
                        try {
                            notificationsOp.Cancel();
                        } catch (...) {
                        }
                        static bool s_timeoutLogged = false;
                        if (!s_timeoutLogged) {
                            s_timeoutLogged = true;
                            DI_LOG(L"[Notif] GetNotificationsAsync timed out; retrying in 30 s.");
                        }
                        WaitForSingleObject(g_stopEvent, 30000);
                        break;
                    }
                    auto notifications = notificationsOp.GetResults();
                    std::set<uint32_t> currentIds;

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        currentIds.insert(notifications.GetAt(i).Id());
                    }

                    // Custom fork: diagnostics (Telegram.DebugLog). Shows what the
                    // notification centre holds, so a missing Telegram toast is visible.
                    const bool notifDebug = GetSettingsCopy().telegramDebugLog;
                    auto describe = [](const winrt::Windows::UI::Notifications::UserNotification& n) {
                        std::wstring app = L"?";
                        std::wstring aumid = L"?";
                        try {
                            app = n.AppInfo().DisplayInfo().DisplayName().c_str();
                        } catch (...) {
                        }
                        try {
                            aumid = n.AppInfo().AppUserModelId().c_str();
                        } catch (...) {
                        }
                        return std::make_pair(app, aumid);
                    };
                    {
                        static size_t s_lastCount = static_cast<size_t>(-1);
                        if (notifDebug && currentIds.size() != s_lastCount) {
                            s_lastCount = currentIds.size();
                            DI_LOG(L"[Notif] notification centre holds %u notification(s)",
                                   static_cast<unsigned>(s_lastCount));
                        }
                    }

                    if (firstPoll) {
                        if (notifDebug) {
                            for (uint32_t i = 0; i < notifications.Size(); ++i) {
                                try {
                                    const auto d = describe(notifications.GetAt(i));
                                    DI_LOG(L"[Notif]   already there: app='%s' aumid='%s'",
                                           d.first.c_str(), d.second.c_str());
                                } catch (...) {
                                }
                            }
                        }
                        seenIds = std::move(currentIds);
                        firstPoll = false;
                        TelegramBanner::OnListenerReady();  // custom fork
                        // Custom fork: notifications that arrived in the last 60 s
                        // (e.g. while the mod was restarting) are still shown.
                        unsigned recent = 0;
                        for (uint32_t i = 0; i < notifications.Size(); ++i) {
                            try {
                                auto n = notifications.GetAt(i);
                                if (winrt::clock::now() - n.CreationTime() < std::chrono::seconds(60)) {
                                    seenIds.erase(n.Id());
                                    ++recent;
                                }
                            } catch (...) {
                            }
                        }
                        DI_LOG(L"[Notif] Listening. %u notification(s) already in the centre, "
                               L"%u of them recent enough to show.",
                               static_cast<unsigned>(notifications.Size()), recent);
                        if (recent == 0) {
                            WaitForSingleObject(g_stopEvent, 1000);
                            continue;
                        }
                    }

                    for (uint32_t i = 0; i < notifications.Size(); ++i) {
                        try {
                            auto userNotification = notifications.GetAt(i);
                            const uint32_t id = userNotification.Id();

                            if (seenIds.count(id)) {
                                continue;
                            }

                            // Immediately mark as seen so we don't process it again
                            seenIds.insert(id);

                            {  // custom fork: always logged (app + AUMID only, no text)
                                const auto d = describe(userNotification);
                                DI_LOG(L"[Notif] NEW id=%u app='%s' aumid='%s'%s", id, d.first.c_str(),
                                       d.second.c_str(),
                                       g_quietMode.load() ? L" (dropped: island Do Not Disturb)" : L"");
                            }

                            // Custom fork: island Do Not Disturb drops every notification.
                            if (g_quietMode.load()) {
                                continue;
                            }

                            // Custom fork: AppInfo throws for non-Store apps (Telegram
                            // Desktop on the user's PC). The original code then dropped the
                            // whole notification silently; now the app is simply "unknown".
                            std::wstring appDisplayName;
                            std::wstring appAumid;
                            try {
                                auto appInfo = userNotification.AppInfo();
                                if (appInfo) {
                                    try {
                                        appAumid = appInfo.AppUserModelId().c_str();
                                    } catch (...) {
                                    }
                                    auto displayInfo = appInfo.DisplayInfo();
                                    if (displayInfo) {
                                        appDisplayName = displayInfo.DisplayName().c_str();
                                    }
                                }
                            } catch (const winrt::hresult_error& appEx) {
                                if (notifDebug) {
                                    DI_LOG(L"[Notif]   app info unavailable (0x%08X)",
                                           static_cast<unsigned>(appEx.code().value));
                                }
                            } catch (...) {
                            }

                            // Custom fork: Telegram gets its own card (Feature 1).
                            {
                                const Settings tgSettings = GetSettingsCopy();
                                // Windows named no app (non-Store apps: Telegram Desktop,
                                // Discord, ...): find the sender by its registry time stamp.
                                if (appAumid.empty() && appDisplayName.empty()) {
                                    uint64_t created = 0;
                                    try {
                                        created = static_cast<uint64_t>(
                                            winrt::clock::to_file_time(userNotification.CreationTime()).value);
                                    } catch (...) {
                                    }
                                    const wchar_t* rule = L"";
                                    std::wstring guessedAumid;
                                    if (created != 0 &&
                                        TelegramSource::IdentifyUnknownApp(created, &guessedAumid, &rule)) {
                                        appAumid = guessedAumid;
                                        appDisplayName = TelegramSource::FriendlyNameFromAumid(guessedAumid);
                                        DI_LOG(L"[Notif]   unknown app -> '%s' (%s), by %s", appDisplayName.c_str(),
                                               appAumid.c_str(), rule);
                                    } else {
                                        DI_LOG(L"[Notif]   unknown app -> could not identify the sender");
                                    }
                                }
                                // Custom fork: Discord gets the same chat card.
                                if (tgSettings.discordEnabled &&
                                    TelegramSource::IsDiscordApp(appAumid, appDisplayName)) {
                                    TelegramSource::Handle(listener, userNotification, appAumid,
                                                           appDisplayName, tgSettings, L"Discord");
                                    continue;
                                }
                                const bool isTelegram = tgSettings.telegramEnabled &&
                                                        TelegramSource::IsTelegramApp(appAumid, appDisplayName);
                                if (isTelegram) {
                                    TelegramSource::Handle(listener, userNotification, appAumid,
                                                           appDisplayName, tgSettings);
                                    continue;
                                }
                            }

                            // Custom fork (custom.16): the island shows only Telegram and
                            // Discord. Every other Windows notification is left to Windows.
                            if (notifDebug) {
                                DI_LOG(L"[Notif]   ignored: not Telegram or Discord");
                            }
                        } catch (const winrt::hresult_error& nex) {
                            if (nex.to_abi() == static_cast<int32_t>(0x80004001) || nex.to_abi() == static_cast<int32_t>(0x80040154)) { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                                // UWP subsystem not ready, skip without spamming logs
                            } else {
                                DI_LOG(L"Failed to parse a notification (0x%08X); skipping.", nex.to_abi());
                            }
                        } catch (...) {
                            DI_LOG(L"Failed to parse a notification; skipping.");
                        }
                    }

                    seenIds = std::move(currentIds);
                } catch (const winrt::hresult_error& ex) {
                    const HRESULT hr = ex.to_abi();
                    if (hr == static_cast<HRESULT>(0x80004001) || hr == static_cast<HRESULT>(0x80040154)) { // E_NOTIMPL or REGDB_E_CLASSNOTREG
                        if (!accessLogged) {
                            DI_LOG(L"Notification listener UWP subsystem not fully ready (0x%08X). Retrying in background...", hr);
                            accessLogged = true;
                        }
                    } else {
                        DI_LOG(L"NotificationThreadProc inner loop WinRT error: %s (0x%08X); reconnecting...", ex.message().c_str(), hr);
                    }
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                } catch (...) {
                    DI_LOG(L"NotificationThreadProc inner loop unknown exception; reconnecting...");
                    WaitForSingleObject(g_stopEvent, 3000);
                    break;
                }

                WaitForSingleObject(g_stopEvent, 1000);
            }
        } catch (const winrt::hresult_error& ex) {
            if (!accessLogged) {
                DI_LOG(L"NotificationThreadProc connection error: %s (0x%08X). Retrying in 3s...", ex.message().c_str(), ex.to_abi());
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        } catch (...) {
            if (!accessLogged) {
                DI_LOG(L"NotificationThreadProc unknown connection exception. Retrying in 3s...");
                accessLogged = true;
            }
            WaitForSingleObject(g_stopEvent, 3000);
        }
    }

    winrt::uninit_apartment();
    return 0;
}
#endif

#if DYNAMIC_ISLAND_HAS_BLUETOOTH_WATCHER

constexpr uint32_t kBtLookupTimeoutMs = 2000;  // FromIdAsync / FromBluetoothAddressAsync
constexpr uint32_t kBtGattTimeoutMs = 3000;    // each GATT step

BluetoothDeviceCategory ClassifyBluetoothDevice(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& info) {
    // 1) Name-based heuristic first — cheap, synchronous, and correct for
    //    the overwhelming majority of consumer devices, which advertise a
    //    descriptive friendly name.
    std::wstring name = ToLowerCopy(std::wstring(info.Name().c_str()));

    if (name.find(L"headphone") != std::wstring::npos ||
        name.find(L"headset") != std::wstring::npos ||
        name.find(L"earbud") != std::wstring::npos ||
        name.find(L"buds") != std::wstring::npos ||
        name.find(L"airpods") != std::wstring::npos) {
        return BluetoothDeviceCategory::Headphones;
    }
    if (name.find(L"speaker") != std::wstring::npos ||
        name.find(L"soundbar") != std::wstring::npos ||
        name.find(L"boombox") != std::wstring::npos) {
        return BluetoothDeviceCategory::Speaker;
    }
    // Standalone 0.7: gamepads (DualSense / DualShock 4 call themselves
    // "Wireless Controller" over Bluetooth).
    if (name.find(L"controller") != std::wstring::npos || name.find(L"gamepad") != std::wstring::npos ||
        name.find(L"dualsense") != std::wstring::npos || name.find(L"dualshock") != std::wstring::npos ||
        name.find(L"xbox") != std::wstring::npos || name.find(L"joy-con") != std::wstring::npos ||
        name.find(L"8bitdo") != std::wstring::npos) {
        return BluetoothDeviceCategory::Gamepad;
    }
    if (name.find(L"mouse") != std::wstring::npos) {
        return BluetoothDeviceCategory::Mouse;
    }
    if (name.find(L"keyboard") != std::wstring::npos) {
        return BluetoothDeviceCategory::Keyboard;
    }
    if (name.find(L"iphone") != std::wstring::npos ||
        name.find(L"phone") != std::wstring::npos ||
        name.find(L"galaxy") != std::wstring::npos ||
        name.find(L"pixel") != std::wstring::npos) {
        return BluetoothDeviceCategory::Phone;
    }

    // 2) Fall back to the classic Bluetooth Class-of-Device major-class bits.
    try {
        using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
        using winrt::Windows::Devices::Bluetooth::BluetoothMajorClass;
        auto device = AwaitOr(BluetoothDevice::FromIdAsync(info.Id()), kBtLookupTimeoutMs, BluetoothDevice{nullptr});
        if (device) {
            switch (device.ClassOfDevice().MajorClass()) {
                case BluetoothMajorClass::Phone:
                    return BluetoothDeviceCategory::Phone;
                case BluetoothMajorClass::AudioVideo:
                    return BluetoothDeviceCategory::Headphones;
                case BluetoothMajorClass::Peripheral: {
                    using winrt::Windows::Devices::Bluetooth::BluetoothMinorClass;
                    const BluetoothMinorClass minor = device.ClassOfDevice().MinorClass();
                    if (minor == BluetoothMinorClass::PeripheralGamepad ||
                        minor == BluetoothMinorClass::PeripheralJoystick) {
                        return BluetoothDeviceCategory::Gamepad;  // standalone 0.7
                    }
                    return BluetoothDeviceCategory::Mouse;
                }
                default:
                    break;
            }
        }
    } catch (...) {
        // Not every device id resolves to a classic BluetoothDevice — BLE-only
        // peripherals in particular will throw here. Fall through.
    }

    return BluetoothDeviceCategory::Generic;
}

// Reads the standard GATT Battery Service (0x180F) / Battery Level
// characteristic (0x2A19). Only works for devices that expose battery this
// way — mostly BLE and BLE-dual-mode devices. Classic-only devices will
// fail the GetGattServicesForUuidAsync call and we just report -1 (unknown).
int TryReadBluetoothBatteryPercentBLE(winrt::hstring const& deviceId) {
    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

    try {
        auto bleDevice = AwaitOr(BluetoothLEDevice::FromIdAsync(deviceId), kBtLookupTimeoutMs, BluetoothLEDevice{nullptr});
        if (!bleDevice) {
            return -1;
        }

        // Cached first: Windows usually knows the services already, and an
        // uncached discovery is a radio round trip (and can wake the device).
        auto servicesResult = AwaitOr(bleDevice.GetGattServicesForUuidAsync(GattServiceUuids::Battery(),
                                                                            BluetoothCacheMode::Cached),
                                      kBtGattTimeoutMs, GattDeviceServicesResult{nullptr});
        if (!servicesResult || servicesResult.Status() != GattCommunicationStatus::Success ||
            servicesResult.Services().Size() == 0) {
            return -1;
        }

        auto service = servicesResult.Services().GetAt(0);
        auto charsResult = AwaitOr(service.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel(),
                                                                          BluetoothCacheMode::Cached),
                                   kBtGattTimeoutMs, GattCharacteristicsResult{nullptr});
        if (!charsResult || charsResult.Status() != GattCommunicationStatus::Success ||
            charsResult.Characteristics().Size() == 0) {
            return -1;
        }

        auto characteristic = charsResult.Characteristics().GetAt(0);
        // The level itself must be fresh.
        auto readResult = AwaitOr(characteristic.ReadValueAsync(BluetoothCacheMode::Uncached), kBtGattTimeoutMs,
                                  GattReadResult{nullptr});
        if (!readResult || readResult.Status() != GattCommunicationStatus::Success) {
            return -1;
        }

        auto buffer = readResult.Value();
        if (buffer.Length() < 1) {
            return -1;
        }

        auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
        uint8_t raw = reader.ReadByte();
        return ClampInt(static_cast<int>(raw), 0, 100);
    } catch (...) {
        return -1;
    }
}

// Windows' own Settings > Bluetooth & devices page shows a battery percentage
// for most classic (non-BLE) headphones/earbuds/speakers using an
// undocumented per-devnode property exposed by the Microsoft Bluetooth
// classic driver stack — not GATT. This is the same property those battery
// tray-icon utilities read. Query it via SetupAPI on the Bluetooth-class
// devnode whose instance ID embeds the device's Bluetooth address.
static const GUID kGuidDevClassBluetooth = {
    0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};

static const DEVPROPKEY PKEY_Bluetooth_Battery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};

int TryReadClassicBluetoothBatteryPercent(uint64_t address) {
    if (!address) {
        DI_LOG(L"BT battery: no address to search for.");
        return -1;
    }

    wchar_t addrHex[16] = {};
    swprintf_s(addrHex, L"%012llX", static_cast<unsigned long long>(address));
    const std::wstring addrLower = ToLowerCopy(addrHex);

    // Enumerate everything the Bluetooth bus driver (BTHENUM) exposes,
    // regardless of which device setup class it landed in. This is broader
    // than filtering by GUID_DEVCLASS_BLUETOOTH and matches what battery
    // tray utilities do.
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsExW(
        nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT,
        nullptr, nullptr, nullptr);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        DI_LOG(L"BT battery: SetupDiGetClassDevsExW(BTHENUM) failed (0x%lx), falling back to class GUID.", GetLastError());
        deviceInfoSet = SetupDiGetClassDevsW(&kGuidDevClassBluetooth, nullptr, nullptr, DIGCF_PRESENT);
        if (deviceInfoSet == INVALID_HANDLE_VALUE) {
            DI_LOG(L"BT battery: fallback enumeration also failed.");
            return -1;
        }
    }

    int result = -1;
    SP_DEVINFO_DATA devInfoData = {};
    devInfoData.cbSize = sizeof(devInfoData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &devInfoData); ++i) {
        wchar_t instanceId[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, &devInfoData, instanceId,
                                         ARRAYSIZE(instanceId), nullptr)) {
            continue;
        }

        if (ToLowerCopy(instanceId).find(addrLower) == std::wstring::npos) {
            continue;
        }


        DEVPROPTYPE propType = 0;
        BYTE battery = 0;
        DWORD required = 0;
        if (SetupDiGetDevicePropertyW(deviceInfoSet, &devInfoData, &PKEY_Bluetooth_Battery,
                                      &propType, &battery, sizeof(battery), &required, 0)) {
            if (propType == DEVPROP_TYPE_BYTE && battery != 0xFF) {
                result = ClampInt(static_cast<int>(battery), 0, 100);
                break;
            }
        }
    }


    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return result;
}

int TryReadBluetoothBatteryPercent(winrt::hstring const& deviceId) {
    int result = -1;

    // Try the classic per-devnode battery property first — this is what
    // Settings > Bluetooth & devices reads, and it's what covers most
    // headsets/earbuds/speakers that pair over classic Bluetooth (BR/EDR)
    // rather than BLE.
    try {
        using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
        auto classicDevice = AwaitOr(BluetoothDevice::FromIdAsync(deviceId), kBtLookupTimeoutMs, BluetoothDevice{nullptr});
        if (classicDevice) {
            result = TryReadClassicBluetoothBatteryPercent(classicDevice.BluetoothAddress());
        }
    } catch (...) {
        // Not resolvable as a classic BluetoothDevice (BLE-only peripheral) — fall through.
    }

    if (result < 0) {
        // Fall back to BLE GATT Battery Service for BLE / dual-mode devices.
        result = TryReadBluetoothBatteryPercentBLE(deviceId);
    }

    if (result >= 0) {
        // Remember this reading so a later disconnect (when the device can
        // no longer be queried) can still show the last known level.
        std::lock_guard lock(g_bluetoothBatteryCacheMutex);
        g_bluetoothBatteryCache[std::wstring(deviceId.c_str())] = result;
    }

    return result;
}

// Returns the last battery percent we successfully read for this device
// while it was connected, or -1 if we never learned one.
int GetLastKnownBluetoothBatteryPercent(const std::wstring& deviceId) {
    std::lock_guard lock(g_bluetoothBatteryCacheMutex);
    auto it = g_bluetoothBatteryCache.find(deviceId);
    return it != g_bluetoothBatteryCache.end() ? it->second : -1;
}

// Small per-device cache so a Removed event (which only carries an Id, not
// a full DeviceInformation) can still show a name/icon on disconnect.
struct BluetoothTrackedDevice {
    std::wstring name;
    BluetoothDeviceCategory category;
};

// Standalone 0.8: everything the Bluetooth watchers and helper threads share.
// Allocated once and never freed on purpose: WinRT watcher callbacks and the
// short "wait until it is a sound device" threads can still be running while
// the island shuts down, so this must outlive BluetoothThreadProc.
struct BluetoothRegistry {
    std::mutex mutex;
    std::unordered_map<std::wstring, BluetoothTrackedDevice> known;  // id -> name/type, kept after disconnect
    std::set<std::wstring> connected;                                // ids connected right now
    std::atomic<double> nextBadgeUpdate{0.0};                        // when the badge should be rebuilt

    // Rebuild the badge no later than `delaySeconds` from now.
    void RequestBadgeUpdate(double delaySeconds) {
        const double at = NowSeconds() + delaySeconds;
        double cur = nextBadgeUpdate.load();
        while (at < cur && !nextBadgeUpdate.compare_exchange_weak(cur, at)) {
        }
    }
};

BluetoothRegistry& Bt() {
    static BluetoothRegistry* registry = new BluetoothRegistry();
    return *registry;
}

// ── Sony gamepads (standalone 0.7) ─────────────────────────────────────────────
// Windows does not report the battery of a DualSense / DualShock 4: it is only
// in the pad's own HID input report. Read one report and decode it, the same
// way Steam and SDL do. Over Bluetooth the pad first sends a short report
// without the battery; reading its calibration feature report switches it to
// the full report (Steam does the same) -- only when the user allows it
// (Modules.SonyPadBattery).
namespace SonyPad {

struct Reading {
    int percent = -1;
    bool charging = false;
    bool bluetooth = false;
};

enum class Model { DualSense, DualShock4 };

bool ModelFor(USHORT vid, USHORT pid, Model* model) {
    if (vid != 0x054C) return false;
    switch (pid) {
        case 0x0CE6:  // DualSense
        case 0x0DF2:  // DualSense Edge
            *model = Model::DualSense;
            return true;
        case 0x05C4:  // DualShock 4 (v1)
        case 0x09CC:  // DualShock 4 (v2)
        case 0x0BA0:  // DualShock 4 USB wireless adaptor
            *model = Model::DualShock4;
            return true;
    }
    return false;
}

// One input report, or 0 bytes after `timeoutMs`.
DWORD ReadReport(HANDLE h, BYTE* buf, DWORD size, DWORD timeoutMs) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, size, &got, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
                GetOverlappedResult(h, &ov, &got, FALSE);
            } else {
                CancelIo(h);
                GetOverlappedResult(h, &ov, &got, TRUE);
                got = 0;
            }
        } else {
            got = 0;
        }
    }
    CloseHandle(ov.hEvent);
    return got;
}

// Decodes the battery byte. DualSense: low nibble 0..10 (x10 %), high nibble
// 1 = charging, 2 = full. DualShock 4: low nibble 0..10 (0..11 on cable), bit 4 = cable.
Reading Decode(Model model, BYTE status) {
    Reading r;
    if (model == Model::DualSense) {
        const int level = status & 0x0F;
        const int state = (status & 0xF0) >> 4;
        r.percent = state == 2 ? 100 : std::min(level * 10 + 5, 100);
        r.charging = state == 1;
    } else {
        const int level = status & 0x0F;
        const bool cable = (status & 0x10) != 0;
        r.percent = std::min(cable ? level * 100 / 11 : level * 10, 100);
        r.charging = cable && level < 11;
    }
    return r;
}

std::optional<Reading> ReadPad(const wchar_t* path, Model model, bool bluetooth, bool allowFullReport) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DI_LOG(L"[Pad] cannot open the pad (error %lu).", GetLastError());
        return std::nullopt;
    }
    const wchar_t* modelName = model == Model::DualSense ? L"DualSense" : L"DualShock 4";

    // Windows queues up to 32 old input reports per handle, so after switching
    // the pad to the full report the first reads can still be short ones: keep
    // reading (up to ~1 s) until a report with the battery byte arrives.
    BYTE buf[128] = {};
    std::optional<Reading> result;
    bool requested = false;
    std::wstring seen;
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (!result && GetTickCount64() < deadline) {
        const DWORD got = ReadReport(h, buf, sizeof(buf), 200);
        if (got == 0) break;
        if (seen.size() < 24) {
            wchar_t id[8];
            swprintf_s(id, L"%02X ", buf[0]);
            seen += id;
        }
        if (model == Model::DualSense) {
            if (buf[0] == 0x01 && !bluetooth && got > 53) result = Decode(model, buf[53]);      // USB
            else if (buf[0] == 0x31 && got > 54) result = Decode(model, buf[54]);               // Bluetooth, full report
        } else {
            if (buf[0] == 0x01 && !bluetooth && got > 30) result = Decode(model, buf[30]);      // USB
            else if (buf[0] == 0x11 && got > 32) result = Decode(model, buf[32]);               // Bluetooth, full report
        }
        if (!result && bluetooth && !requested && buf[0] == 0x01) {
            if (!allowFullReport) break;
            // Short Bluetooth report: reading the calibration feature report
            // switches the pad to the full report (as Steam/SDL do).
            requested = true;
            BYTE feature[80] = {};
            feature[0] = model == Model::DualSense ? 0x05 : 0x02;
            const ULONG len = model == Model::DualSense ? 41 : 37;
            const BOOL ok = HidD_GetFeature(h, feature, len);
            DI_LOG(L"[Pad] %s over Bluetooth sends the short report; full report requested: %s.", modelName,
                   ok ? L"ok" : L"failed");
            if (!ok) break;
        }
    }
    CloseHandle(h);
    if (result) {
        result->bluetooth = bluetooth;
        static int s_lastLogged = -2;
        if (result->percent != s_lastLogged) {
            s_lastLogged = result->percent;
            DI_LOG(L"[Pad] %s battery %d%%%s (%s).", modelName, result->percent,
                   result->charging ? L", charging" : L"", bluetooth ? L"Bluetooth" : L"USB");
        }
    } else {
        DI_LOG(L"[Pad] %s: no battery in the reports (report ids seen: %s).", modelName,
               seen.empty() ? L"none" : seen.c_str());
    }
    return result;
}

// Battery of every Sony pad that is connected (USB and Bluetooth).
std::vector<Reading> ReadAll(bool allowFullReport) {
    std::vector<Reading> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifData = {sizeof(ifData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifData); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifData, nullptr, 0, &needed, nullptr);
        if (needed == 0 || needed > 4096) continue;
        std::vector<BYTE> detailBuf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, needed, nullptr, nullptr)) continue;

        // Attributes only (no read access needed, works even while a game holds the pad).
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr = {sizeof(attr)};
        const bool ok = HidD_GetAttributes(q, &attr);
        CloseHandle(q);
        Model model;
        if (!ok || !ModelFor(attr.VendorID, attr.ProductID, &model)) continue;

        const std::wstring lowerPath = ToLowerCopy(detail->DevicePath);
        const bool bluetooth = lowerPath.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
        if (auto r = ReadPad(detail->DevicePath, model, bluetooth, allowFullReport)) {
            out.push_back(*r);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool LooksLikeSonyPad(const std::wstring& name) {
    const std::wstring n = ToLowerCopy(name);
    return n.find(L"dualsense") != std::wstring::npos || n.find(L"dualshock") != std::wstring::npos ||
           n == L"wireless controller";  // what DualShock 4 / DualSense call themselves over Bluetooth
}

}  // namespace SonyPad

// ── AirPods / Beats battery (standalone 0.7.7) ────────────────────────────────
// Apple headphones do not give Windows their battery. They do broadcast it to
// everyone nearby in a Bluetooth LE advertisement ("proximity pairing", Apple
// company id 0x004C, message type 0x07) -- the same thing an iPhone shows when
// the case opens. While AirPods are connected, listen for those messages and
// take the strongest (= closest) one. Left/right are in 10 % steps, 15 = unknown.
namespace AirPodsBattery {

struct Reading {
    int percent = -1;       // the lower of the two buds that report a level
    bool charging = false;  // a bud is charging
    short rssi = -128;
    double at = 0.0;
};

std::mutex g_lock;
Reading g_last;
winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher g_watcher{nullptr};

bool LooksLikeAppleHeadphones(const std::wstring& name) {
    const std::wstring n = ToLowerCopy(name);
    return n.find(L"airpods") != std::wstring::npos || n.find(L"beats") != std::wstring::npos ||
           n.find(L"powerbeats") != std::wstring::npos;
}

void OnAdvertisement(winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs const& args) {
    try {
        for (const auto& md : args.Advertisement().ManufacturerData()) {
            if (md.CompanyId() != 0x004C) continue;
            const auto buffer = md.Data();
            if (buffer.Length() < 9) continue;
            const uint8_t* d = buffer.data();
            if (d[0] != 0x07 || d[2] != 0x01) continue;  // proximity pairing message

            const bool flip = (d[5] & 0x20) == 0;
            const int left = flip ? (d[6] >> 4) : (d[6] & 0x0F);
            const int right = flip ? (d[6] & 0x0F) : (d[6] >> 4);
            const int chargeBits = d[7] >> 4;
            const bool chargeL = (chargeBits & (flip ? 0x02 : 0x01)) != 0;
            const bool chargeR = (chargeBits & (flip ? 0x01 : 0x02)) != 0;

            int percent = -1;
            if (left <= 10) percent = left * 10;
            if (right <= 10) percent = percent < 0 ? right * 10 : std::min(percent, right * 10);
            if (percent < 0) continue;

            const short rssi = args.RawSignalStrengthInDBm();
            if (rssi < -80) continue;  // another room / someone else's AirPods
            const double now = NowSeconds();
            std::lock_guard lock(g_lock);
            // Keep the closest pair: a stronger signal wins; a weaker one only
            // replaces a reading that is getting old.
            if (rssi >= g_last.rssi - 3 || now - g_last.at > 8.0) {
                g_last.percent = std::max(0, std::min(percent, 100));
                g_last.charging = chargeL || chargeR;
                g_last.rssi = rssi;
                g_last.at = now;
            }
        }
    } catch (...) {
    }
}

// Listens only while Apple headphones are connected (passive scan, no pairing).
void SetListening(bool on) {
    using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
    using winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
    try {
        if (on && !g_watcher) {
            g_watcher = BluetoothLEAdvertisementWatcher();
            g_watcher.ScanningMode(BluetoothLEScanningMode::Passive);
            // Let Windows drop everything that is not from Apple, so the handler
            // is not called for every advertisement around.
            winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEManufacturerData apple;
            apple.CompanyId(0x004C);
            g_watcher.AdvertisementFilter().Advertisement().ManufacturerData().Append(apple);
            g_watcher.Received([](auto const&, auto const& args) { OnAdvertisement(args); });
            g_watcher.Start();
            DI_LOG(L"[AirPods] listening for battery broadcasts.");
        } else if (!on && g_watcher) {
            g_watcher.Stop();
            g_watcher = nullptr;
            std::lock_guard lock(g_lock);
            g_last = Reading{};
            DI_LOG(L"[AirPods] stopped listening.");
        }
    } catch (const winrt::hresult_error& e) {
        DI_LOG(L"[AirPods] scanner error 0x%08X", static_cast<unsigned>(e.code().value));
        g_watcher = nullptr;
    }
}

// Fresh reading (last 30 s) or percent -1.
Reading Get() {
    std::lock_guard lock(g_lock);
    Reading r = g_last;
    if (NowSeconds() - r.at > 30.0) r.percent = -1;
    return r;
}

}  // namespace AirPodsBattery

// ── Connected Bluetooth devices with battery (standalone 0.7, reworked in 0.8) ──
// Shown top-left on the first tab of the expanded island (clock and player).
// Cost model: the badge is rebuilt every 60 s (every 5 s while headphones are
// connected, to follow them in and out of the case). Rebuilding is cheap; the
// expensive part -- talking to devices -- is cached per device for 60 s.

// Standalone 0.7.4: headphones and speakers can have a separate low-energy
// entry (charging case, fast pairing) that Windows reports as connected while
// no audio is connected; the real headphone connection is the classic one.
// (0.7.6: AirPods are *named* "... - Find My" even on the classic side, so the
// name is not a reason to drop an entry.)
bool IsShadowBluetoothEntry(const std::wstring& id, BluetoothDeviceCategory category) {
    const bool lowEnergy = ToLowerCopy(id).rfind(L"bluetoothle#", 0) == 0;
    return lowEnergy && (category == BluetoothDeviceCategory::Headphones || category == BluetoothDeviceCategory::Speaker);
}

bool IsAudioBluetoothCategory(BluetoothDeviceCategory category) {
    return category == BluetoothDeviceCategory::Headphones || category == BluetoothDeviceCategory::Speaker;
}

// Lower-cased names of the sound devices Windows can use right now.
std::vector<std::wstring> ActiveAudioEndpointNames() {
    std::vector<std::wstring> names;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return names;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices)) || !devices) {
        return names;
    }
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count && i < 64; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        if (FAILED(devices->Item(i, &device)) || !device ||
            FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
            continue;
        }
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            names.push_back(ToLowerCopy(value.pwszVal));
        }
        PropVariantClear(&value);
    }
    return names;
}

// Standalone 0.7.6: AirPods (and other headphones) keep their Bluetooth link up
// while they sit in the case, so "connected" is not "in use". They count only
// while Windows has an active sound device for them -- matched by name: the
// endpoint is called e.g. "Headphones (AirPods Pro #2 - Find My)".
bool HasAudioEndpointFor(const std::wstring& deviceName, const std::vector<std::wstring>& endpoints) {
    std::wstring key = ToLowerCopy(deviceName);
    const size_t findMy = key.find(L" - find my");
    if (findMy != std::wstring::npos) key.resize(findMy);
    if (key.empty()) return false;
    for (const std::wstring& e : endpoints) {
        if (e.find(key) != std::wstring::npos) return true;
    }
    return false;
}

bool HasActiveAudioEndpointFor(const std::wstring& deviceName) {
    return HasAudioEndpointFor(deviceName, ActiveAudioEndpointNames());
}

int BluetoothCategoryRank(BluetoothDeviceCategory c) {
    switch (c) {
        case BluetoothDeviceCategory::Gamepad: return 0;
        case BluetoothDeviceCategory::Headphones: return 1;
        case BluetoothDeviceCategory::Speaker: return 2;
        case BluetoothDeviceCategory::Mouse: return 3;
        case BluetoothDeviceCategory::Keyboard: return 4;
        default: return 5;
    }
}

// Last battery reading per device (Bluetooth thread only).
struct BadgeBattery {
    int percent = -1;
    bool charging = false;
    double at = -1e9;
};
std::unordered_map<std::wstring, BadgeBattery> g_badgeBattery;
constexpr double kBadgeBatteryMaxAge = 60.0;

// Rebuilds the badge. Returns true while headphones/speakers are connected, so
// the caller looks again in 5 s instead of a minute.
bool RefreshBluetoothBadge(const std::vector<std::pair<std::wstring, BluetoothTrackedDevice>>& connected) {
    const Settings settings = GetSettingsCopy();
    std::vector<BluetoothBatteryItem> items;
    bool audioConnected = false;

    if (!settings.bluetoothDashboard) {
        AirPodsBattery::SetListening(false);
        g_badgeBattery.clear();
    } else {
        // 1. Which devices to show.
        std::vector<std::pair<std::wstring, BluetoothTrackedDevice>> shown;
        std::vector<std::wstring> endpoints;
        bool endpointsRead = false;
        for (const auto& [id, dev] : connected) {
            if (dev.category == BluetoothDeviceCategory::Phone) continue;  // a phone linked for calls, not an accessory
            if (IsShadowBluetoothEntry(id, dev.category)) continue;
            bool duplicate = false;
            for (const auto& s2 : shown) duplicate = duplicate || s2.second.name == dev.name;
            if (duplicate) continue;  // dual-mode devices show up in both watchers
            if (IsAudioBluetoothCategory(dev.category)) {
                audioConnected = true;
                if (!endpointsRead) {
                    endpoints = ActiveAudioEndpointNames();  // once per rebuild
                    endpointsRead = true;
                }
                if (!HasAudioEndpointFor(dev.name, endpoints)) continue;  // linked, but not a sound device (case)
            }
            shown.emplace_back(id, dev);
        }

        // 2. AirPods/Beats: their battery comes from broadcasts, listened to only while shown.
        bool apple = false;
        for (const auto& [id, dev] : shown) {
            apple = apple || (IsAudioBluetoothCategory(dev.category) && AirPodsBattery::LooksLikeAppleHeadphones(dev.name));
        }
        AirPodsBattery::SetListening(apple);

        // 3. Sony pads: one HID pass for all of them, at most once a minute.
        const double now = NowSeconds();
        auto isSony = [](const BluetoothTrackedDevice& d) {
            return d.category == BluetoothDeviceCategory::Gamepad && SonyPad::LooksLikeSonyPad(d.name);
        };
        bool sonyStale = false;
        for (const auto& [id, dev] : shown) {
            if (isSony(dev)) {
                auto it = g_badgeBattery.find(id);
                sonyStale = sonyStale || it == g_badgeBattery.end() || now - it->second.at > kBadgeBatteryMaxAge;
            }
        }
        if (sonyStale) {
            std::vector<SonyPad::Reading> bt;
            for (const SonyPad::Reading& r : SonyPad::ReadAll(settings.sonyPadBattery)) {
                if (r.bluetooth) bt.push_back(r);
            }
            size_t next = 0;
            for (const auto& [id, dev] : shown) {
                if (!isSony(dev)) continue;
                BadgeBattery b;
                b.at = now;
                if (next < bt.size()) {
                    b.percent = bt[next].percent;
                    b.charging = bt[next].charging;
                    ++next;
                }
                g_badgeBattery[id] = b;
            }
        }

        // 4. Everything else: Windows' own battery value, at most once a minute.
        for (const auto& [id, dev] : shown) {
            BluetoothBatteryItem item;
            item.name = dev.name;
            item.category = dev.category;
            if (isSony(dev)) {
                const BadgeBattery& b = g_badgeBattery[id];
                item.percent = b.percent;
                item.charging = b.charging;
            } else if (AirPodsBattery::LooksLikeAppleHeadphones(dev.name) && AirPodsBattery::Get().percent >= 0) {
                const AirPodsBattery::Reading r = AirPodsBattery::Get();
                item.percent = r.percent;
                item.charging = r.charging;
            } else {
                BadgeBattery& b = g_badgeBattery[id];
                if (now - b.at > kBadgeBatteryMaxAge) {
                    b.percent = TryReadBluetoothBatteryPercent(winrt::hstring(id));
                    b.charging = false;
                    b.at = now;
                }
                item.percent = b.percent;
            }
            items.push_back(std::move(item));
        }

        // Forget devices that are gone, so the cache cannot grow.
        for (auto it = g_badgeBattery.begin(); it != g_badgeBattery.end();) {
            bool stillShown = false;
            for (const auto& [id, dev] : shown) stillShown = stillShown || id == it->first;
            it = stillShown ? std::next(it) : g_badgeBattery.erase(it);
        }

        std::stable_sort(items.begin(), items.end(), [](const BluetoothBatteryItem& a, const BluetoothBatteryItem& b) {
            const bool ab = a.percent >= 0;
            const bool bb = b.percent >= 0;
            if (ab != bb) return ab;  // devices with a known battery first
            return BluetoothCategoryRank(a.category) < BluetoothCategoryRank(b.category);
        });
    }

    bool changed = false;
    {
        std::lock_guard lock(g_stateMutex);
        const auto& old = g_state.bluetoothBatteries;
        changed = old.size() != items.size();
        for (size_t i = 0; !changed && i < items.size(); ++i) {
            changed = old[i].name != items[i].name || old[i].percent != items[i].percent ||
                      old[i].charging != items[i].charging || old[i].category != items[i].category;
        }
        if (changed) {
            for (const BluetoothBatteryItem& it : items) {
                DI_LOG(L"[BT] badge: %s (type %d, battery %d%%)", it.name.c_str(), static_cast<int>(it.category),
                       it.percent);
            }
            if (items.empty()) DI_LOG(L"[BT] badge: no connected devices");
            g_state.bluetoothBatteries = std::move(items);
        }
    }
    if (changed) {
        g_layoutDirty = true;
        TriggerNudge();
    }
    return audioConnected;
}

void HandleBluetoothConnected(
    const winrt::Windows::Devices::Enumeration::DeviceInformation& info,
    std::unordered_map<std::wstring, BluetoothTrackedDevice>& cache,
    std::mutex& cacheMutex, bool audioReady = false) {
    std::wstring name = info.Name().c_str();
    if (name.empty()) {
        return;
    }

    // Re-read the default output once (event-driven, no polling).
    g_volumeEndpointStale = true;
    if (HWND recheckHwnd = g_hwnd) {
        PostMessageW(recheckHwnd, WM_NULL, 0, 0);
    }

    BluetoothDeviceCategory category = ClassifyBluetoothDevice(info);
    {
        std::lock_guard lock(cacheMutex);
        cache[std::wstring(info.Id().c_str())] = BluetoothTrackedDevice{name, category};
    }

    if (!g_settings.bluetoothIndicator) {
        return;
    }
    // Standalone 0.7.4: no "connected" card for the low-energy side link of headphones.
    if (IsShadowBluetoothEntry(std::wstring(info.Id().c_str()), category)) {
        return;
    }
    // Standalone 0.7.6: headphones/speakers announce themselves only once Windows
    // can play sound through them (AirPods link up while still in the case).
    // Waits off the watcher's callback thread, then comes back here.
    if (IsAudioBluetoothCategory(category) && !audioReady) {
        std::thread([info, &cache, &cacheMutex, name]() {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            bool ready = false;
            for (int i = 0; i < 16 && !ready && WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0; ++i) {
                ready = HasActiveAudioEndpointFor(name);
                if (!ready) Sleep(500);  // up to ~8 s
            }
            if (ready) {
                HandleBluetoothConnected(info, cache, cacheMutex, /*audioReady=*/true);
            } else {
                DI_LOG(L"[BT] %s linked but no sound device for it; no card.", name.c_str());
            }
            winrt::uninit_apartment();
        }).detach();
        return;
    }

    DI_LOG(L"Bluetooth: connected - %s", name.c_str());

    const uint64_t myGeneration = ++g_bluetoothConnectGeneration;
    const std::wstring deviceId = info.Id().c_str();

    BluetoothDeviceSnapshot snapshot;
    snapshot.active = true;
    snapshot.connected = true;
    snapshot.deviceName = name;
    snapshot.category = category;
    snapshot.expiresAt = NowSeconds() + 4.0;
    snapshot.batteryPercent = g_settings.bluetoothShowBattery &&
                                      !(category == BluetoothDeviceCategory::Gamepad && SonyPad::LooksLikeSonyPad(name))
        ? TryReadBluetoothBatteryPercent(info.Id())
        : -1;  // Sony pads: the badge reads them via HID a few seconds later

    {
        std::lock_guard lock(g_stateMutex);
        g_state.bluetoothDevice = std::move(snapshot);
    }
    TriggerNudge();

    // Windows often hasn't populated the battery property at the exact
    // instant the connection event fires — it needs a moment to actually
    // query the device. Keep retrying in the background for a while; if a
    // value shows up and this connection is still the one being displayed,
    // patch it into the live state and re-render.
    if (g_settings.bluetoothShowBattery && snapshot.batteryPercent < 0 &&
        !(category == BluetoothDeviceCategory::Gamepad && SonyPad::LooksLikeSonyPad(name))) {
        std::thread([deviceId, myGeneration]() {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for (int attempt = 0; attempt < 6; ++attempt) {
                Sleep(1500);
                if (g_bluetoothConnectGeneration.load() != myGeneration) {
                    break;  // a newer connect/disconnect event superseded this one
                }

                int battery = TryReadBluetoothBatteryPercent(winrt::hstring(deviceId));
                if (battery >= 0) {
                    std::lock_guard lock(g_stateMutex);
                    if (g_bluetoothConnectGeneration.load() == myGeneration &&
                        g_state.bluetoothDevice.connected) {
                        g_state.bluetoothDevice.batteryPercent = battery;
                        DI_LOG(L"Bluetooth: battery arrived late (%d%%) on retry %d.", battery, attempt + 1);
                    }
                    TriggerNudge();
                    break;
                }
            }
            winrt::uninit_apartment();
        }).detach();
    }
}

void HandleBluetoothDisconnected(
    winrt::hstring const& id,
    std::unordered_map<std::wstring, BluetoothTrackedDevice>& cache,
    std::mutex& cacheMutex) {
    ++g_bluetoothConnectGeneration;  // cancel any pending battery retry for the old connection

    // Re-read the default output once (event-driven, no polling).
    g_volumeEndpointStale = true;
    if (HWND recheckHwnd = g_hwnd) {
        PostMessageW(recheckHwnd, WM_NULL, 0, 0);
    }

    if (!g_settings.bluetoothIndicator) {
        return;
    }

    std::wstring name;
    BluetoothDeviceCategory category = BluetoothDeviceCategory::Generic;
    {
        std::lock_guard lock(cacheMutex);
        auto it = cache.find(std::wstring(id.c_str()));
        if (it != cache.end()) {
            name = it->second.name;
            category = it->second.category;
        }
    }
    if (name.empty()) {
        name = L"Bluetooth Device";
    }
    if (IsShadowBluetoothEntry(std::wstring(id.c_str()), category)) {
        return;  // standalone 0.7.4: the headphones' low-energy side link, not the real disconnect
    }

    DI_LOG(L"Bluetooth: disconnected - %s", name.c_str());

    BluetoothDeviceSnapshot snapshot;
    snapshot.active = true;
    snapshot.connected = false;
    snapshot.deviceName = name;
    snapshot.category = category;
    snapshot.batteryPercent = GetLastKnownBluetoothBatteryPercent(std::wstring(id.c_str()));
    snapshot.expiresAt = NowSeconds() + 4.0;

    {
        std::lock_guard lock(g_stateMutex);
        g_state.bluetoothDevice = std::move(snapshot);
    }
    TriggerNudge();
}

DWORD WINAPI BluetoothThreadProc(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    using winrt::Windows::Devices::Enumeration::DeviceInformation;
    using winrt::Windows::Devices::Enumeration::DeviceInformationUpdate;
    using winrt::Windows::Devices::Enumeration::DeviceWatcher;
    using winrt::Windows::Devices::Bluetooth::BluetoothDevice;
    using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
    using winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus;

    std::vector<DeviceWatcher> watchers;
    Bt().RequestBadgeUpdate(2.0);

    // One watcher per Bluetooth flavour, on the "connected" selector: Added =
    // connected, Removed = disconnected. Callbacks run on WinRT threads and
    // only touch Bt() (process lifetime), never this thread's locals.
    auto startWatcher = [&watchers](winrt::hstring const& selector, const wchar_t* label) {
        try {
            // Standalone 0.7.1: ask for IsConnected so the watcher re-evaluates it;
            // without it a device switched off was never reported as removed.
            auto props = winrt::single_threaded_vector<winrt::hstring>({L"System.Devices.Aep.IsConnected"});
            DeviceWatcher watcher = DeviceInformation::CreateWatcher(selector, props);
            auto enumDone = std::make_shared<std::atomic<bool>>(false);

            watcher.EnumerationCompleted(
                [enumDone](DeviceWatcher const&, winrt::Windows::Foundation::IInspectable const&) {
                    *enumDone = true;
                });

            watcher.Added([enumDone, label](DeviceWatcher const&, DeviceInformation const& info) {
                BluetoothRegistry& bt = Bt();
                const std::wstring id = info.Id().c_str();
                {
                    std::lock_guard lock(bt.mutex);
                    bt.connected.insert(id);
                }
                DI_LOG(L"[BT] %s watcher: added %s%s", label, info.Name().c_str(),
                       *enumDone ? L"" : L" (already connected at start)");
                // Windows needs a moment before it knows the battery of a new connection.
                bt.RequestBadgeUpdate(4.0);
                // The initial snapshot (connected before the island started) is only
                // remembered -- no "connected" card for something the user did not just do.
                if (!*enumDone) {
                    const std::wstring name = info.Name().c_str();
                    if (!name.empty()) {
                        const BluetoothDeviceCategory category = ClassifyBluetoothDevice(info);
                        std::lock_guard lock(bt.mutex);
                        bt.known[id] = BluetoothTrackedDevice{name, category};
                    }
                    return;
                }
                HandleBluetoothConnected(info, bt.known, bt.mutex);
            });

            watcher.Updated([label](DeviceWatcher const&, DeviceInformationUpdate const& update) {
                auto value = update.Properties().TryLookup(L"System.Devices.Aep.IsConnected");
                if (!value) return;
                const auto connected = value.try_as<winrt::Windows::Foundation::IReference<bool>>();
                if (!connected) return;
                BluetoothRegistry& bt = Bt();
                const std::wstring id = update.Id().c_str();
                std::wstring name = L"(unknown device)";
                {
                    std::lock_guard lock(bt.mutex);
                    if (connected.Value()) bt.connected.insert(id);
                    else bt.connected.erase(id);
                    auto known = bt.known.find(id);
                    if (known != bt.known.end()) name = known->second.name;
                }
                DI_LOG(L"[BT] %s watcher: %s is now %s", label, name.c_str(),
                       connected.Value() ? L"connected" : L"disconnected");
                bt.RequestBadgeUpdate(connected.Value() ? 4.0 : 0.0);
            });

            watcher.Removed([](DeviceWatcher const&, DeviceInformationUpdate const& update) {
                BluetoothRegistry& bt = Bt();
                {
                    std::lock_guard lock(bt.mutex);
                    bt.connected.erase(std::wstring(update.Id().c_str()));
                }
                bt.RequestBadgeUpdate(0.0);
                HandleBluetoothDisconnected(update.Id(), bt.known, bt.mutex);
            });

            watcher.Start();
            watchers.push_back(watcher);
            DI_LOG(L"Bluetooth: %s watcher started.", label);
        } catch (...) {
            DI_LOG(L"Bluetooth: failed to start %s watcher.", label);
        }
    };

    startWatcher(
        BluetoothDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
        L"classic");
    startWatcher(
        BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(BluetoothConnectionStatus::Connected),
        L"BLE");

    // Standalone 0.7.1/0.7.2: ask Windows directly whether each listed device is
    // still connected. Watchers can miss a switch-off, and at start-up the
    // initial list can include a device Windows still remembers as connected.
    auto verifyConnected = []() {
        BluetoothRegistry& bt = Bt();
        std::vector<std::pair<std::wstring, BluetoothDeviceCategory>> ids;
        {
            std::lock_guard lock(bt.mutex);
            for (const std::wstring& id : bt.connected) {
                auto it = bt.known.find(id);
                ids.emplace_back(id, it != bt.known.end() ? it->second.category : BluetoothDeviceCategory::Generic);
            }
        }
        for (const auto& [id, category] : ids) {
            bool stillConnected = true;
            const wchar_t* why = L"";
            if (auto classic = AwaitOr(BluetoothDevice::FromIdAsync(winrt::hstring(id)), kBtLookupTimeoutMs,
                                       BluetoothDevice{nullptr})) {
                stillConnected = classic.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                why = L"classic link is down";
            } else if (auto le = AwaitOr(BluetoothLEDevice::FromIdAsync(winrt::hstring(id)), kBtLookupTimeoutMs,
                                         BluetoothLEDevice{nullptr})) {
                stillConnected = le.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                why = L"LE link is down";
                // Standalone 0.7.3: for audio devices the classic (audio) link decides.
                if (stillConnected && IsAudioBluetoothCategory(category)) {
                    auto twin = AwaitOr(BluetoothDevice::FromBluetoothAddressAsync(le.BluetoothAddress()),
                                        kBtLookupTimeoutMs, BluetoothDevice{nullptr});
                    if (twin && twin.ConnectionStatus() != BluetoothConnectionStatus::Connected) {
                        stillConnected = false;
                        why = L"only the LE side is up, audio is off";
                    }
                }
            }
            if (!stillConnected) {
                std::wstring name;
                {
                    std::lock_guard lock(bt.mutex);
                    bt.connected.erase(id);
                    auto it = bt.known.find(id);
                    if (it != bt.known.end()) name = it->second.name;
                }
                DI_LOG(L"[BT] not connected (%s): %s", why, name.c_str());
                bt.RequestBadgeUpdate(0.0);
            }
        }
    };

    // Housekeeping once a second: a connection check every 10 s, the badge when
    // it is due (events pull it forward). Nothing here spins.
    double nextConnectionCheck = NowSeconds() + 10.0;
    while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT) {
        const double now = NowSeconds();
        const bool badgeDue = now >= Bt().nextBadgeUpdate.load();
        if (badgeDue || now >= nextConnectionCheck) {
            verifyConnected();  // never show a device that is not really connected
            nextConnectionCheck = now + 10.0;
        }
        if (!badgeDue) {
            continue;
        }
        Bt().nextBadgeUpdate.store(now + 60.0);
        std::vector<std::pair<std::wstring, BluetoothTrackedDevice>> connected;
        {
            std::lock_guard lock(Bt().mutex);
            for (const std::wstring& id : Bt().connected) {
                auto it = Bt().known.find(id);
                if (it != Bt().known.end()) connected.emplace_back(id, it->second);
            }
        }
        if (RefreshBluetoothBadge(connected)) {
            Bt().RequestBadgeUpdate(5.0);  // headphones: follow them in and out of the case
        }
    }

    for (auto& watcher : watchers) {
        try {
            watcher.Stop();
        } catch (...) {
        }
    }
    AirPodsBattery::SetListening(false);

    winrt::uninit_apartment();
    return 0;
}

#else  // !DYNAMIC_ISLAND_HAS_BLUETOOTH_WATCHER

DWORD WINAPI BluetoothThreadProc(void*) {
    // SDK used to build this mod doesn't expose the WinRT Bluetooth headers;
    // the indicator silently stays inactive instead of failing the mod.
    return 0;
}

#endif  // DYNAMIC_ISLAND_HAS_BLUETOOTH_WATCHER

float SampleAudioAmplitude(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (!data || !frames || !format || !format->nChannels) {
        return 0.0f;
    }

    double sum = 0.0;
    size_t samples = static_cast<size_t>(frames) * format->nChannels;

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                     kSubTypeIeeeFloat))) {
        auto* f = reinterpret_cast<float*>(data);
        for (size_t i = 0; i < samples; ++i) {
            sum += f[i] * f[i];
        }
    } else if (format->wBitsPerSample == 16) {
        auto* s = reinterpret_cast<int16_t*>(data);
        for (size_t i = 0; i < samples; ++i) {
            const double v = s[i] / 32768.0;
            sum += v * v;
        }
    }

    const double rms = samples ? std::sqrt(sum / samples) : 0.0;
    return Clamp(static_cast<float>(rms * 4.0), 0.0f, 1.0f);
}

// Goertzel filter bank over a sliding window of the mixed-down system audio.
// Produces kSpectrumBands log-spaced magnitudes (50 Hz .. 14 kHz) with an
// attack/release envelope and a slow auto-gain so quiet and loud tracks both
// fill the visualizer. Only ever touched from the audio thread.
struct SpectrumAnalyzer {
    static constexpr size_t kWindow = 1024;
    static constexpr size_t kHop = 512;

    static constexpr int kLog2Window = 10;  // kWindow == 1 << 10

    float ring[kWindow]{};
    float window[kWindow]{};
    float twCos[kWindow / 2]{};
    float twSin[kWindow / 2]{};
    uint16_t bitRev[kWindow]{};
    int   binLo[kSpectrumBands]{};  // FFT bins covered by each band, [lo, hi)
    int   binHi[kSpectrumBands]{};
    float smooth[kSpectrumBands]{};
    float agc = 0.04f;
    float sampleRate = 0.0f;
    size_t write = 0;
    size_t sinceHop = 0;
    bool windowReady = false;

    void Configure(float rate) {
        if (rate < 8000.0f) rate = 48000.0f;
        if (!windowReady) {
            for (size_t i = 0; i < kWindow; ++i) {
                window[i] = 0.5f - 0.5f * std::cos(6.2831853f * i / (kWindow - 1));
            }
            for (size_t i = 0; i < kWindow / 2; ++i) {
                const float a = -6.2831853f * static_cast<float>(i) / static_cast<float>(kWindow);
                twCos[i] = std::cos(a);
                twSin[i] = std::sin(a);
            }
            for (size_t i = 0; i < kWindow; ++i) {
                size_t r = 0;
                for (int bit = 0; bit < kLog2Window; ++bit) {
                    if (i & (static_cast<size_t>(1) << bit)) {
                        r |= static_cast<size_t>(1) << (kLog2Window - 1 - bit);
                    }
                }
                bitRev[i] = static_cast<uint16_t>(r);
            }
            windowReady = true;
        }
        if (rate == sampleRate) return;
        sampleRate = rate;
        const float fMin = 50.0f;
        const float fMax = std::min(14000.0f, rate * 0.45f);
        const float ratio = std::pow(fMax / fMin, 1.0f / (kSpectrumBands - 1));
        const float halfStep = std::sqrt(ratio);
        const float binHz = rate / kWindow;
        const int nyquistBin = static_cast<int>(kWindow / 2) - 1;
        for (int b = 0; b < kSpectrumBands; ++b) {
            const float fc = fMin * std::pow(ratio, static_cast<float>(b));
            const float lo = fc / halfStep, hi = fc * halfStep;
            const int centre = static_cast<int>(std::lround(fc / binHz));
            int b0 = std::min(static_cast<int>(std::floor(lo / binHz)), centre);
            int b1 = std::max(static_cast<int>(std::ceil(hi / binHz)), centre + 1);
            b0 = std::clamp(b0, 1, nyquistBin);
            b1 = std::clamp(b1, b0 + 1, nyquistBin + 1);
            binLo[b] = b0;
            binHi[b] = b1;
        }
    }

    // In-place radix-2 FFT. Replaces ~100 Goertzel filters (each a 1024-step
    // dependent chain) with ~5k butterflies.
    void Fft(float* re, float* im) const {
        for (size_t i = 0; i < kWindow; ++i) {
            const size_t j = bitRev[i];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t len = 2; len <= kWindow; len <<= 1) {
            const size_t half = len >> 1, step = kWindow / len;
            for (size_t i = 0; i < kWindow; i += len) {
                for (size_t k = 0; k < half; ++k) {
                    const float wr = twCos[k * step], wi = twSin[k * step];
                    const size_t a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
    }

    void Analyze() {
        float re[kWindow];
        float im[kWindow];
        for (size_t i = 0; i < kWindow; ++i) {
            re[i] = ring[(write + i) % kWindow] * window[i];
            im[i] = 0.0f;
        }
        Fft(re, im);
        float peak = 0.0f;
        float raw[kSpectrumBands];
        for (int b = 0; b < kSpectrumBands; ++b) {
            float sum = 0.0f, mx = 0.0f;
            for (int k = binLo[b]; k < binHi[b]; ++k) {
                const float p = re[k] * re[k] + im[k] * im[k];
                sum += p;
                mx = std::max(mx, p);
            }
            // Half mean, half strongest bin: broadband energy still counts, but a
            // single strong partial is not averaged away.
            const float power = 0.5f * (sum / static_cast<float>(binHi[b] - binLo[b])) + 0.5f * mx;
            // 4/kWindow normalises a Hann-windowed sinusoid back to ~its amplitude.
            float amp = std::sqrt(power) * (4.0f / kWindow);
            // Music has a steep spectral tilt; lift the highs so they stay visible.
            const float tilt = 1.0f + 2.2f * static_cast<float>(b) / (kSpectrumBands - 1);
            amp *= tilt;
            raw[b] = amp;
            peak = std::max(peak, amp);
        }
        agc = std::max(peak, agc * 0.9985f);
        const float norm = 1.0f / std::max(agc, 0.02f);
        for (int b = 0; b < kSpectrumBands; ++b) {
            float v = std::sqrt(Clamp(raw[b] * norm, 0.0f, 1.0f));
            // Silence must stay silent even though the auto-gain is boosted.
            v *= Clamp(peak * 40.0f, 0.0f, 1.0f);
            smooth[b] += (v > smooth[b] ? 0.65f : 0.14f) * (v - smooth[b]);
        }
    }

    void Decay() {
        for (int b = 0; b < kSpectrumBands; ++b) smooth[b] *= 0.88f;
    }

    // Returns true when at least one new analysis frame was produced.
    bool Feed(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
        if (!data || !frames || !format || !format->nChannels) { Decay(); return true; }
        const bool isFloat =
            format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
             IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                         kSubTypeIeeeFloat));
        const bool is16 = format->wBitsPerSample == 16;
        if (!isFloat && !is16) { Decay(); return true; }

        Configure(static_cast<float>(format->nSamplesPerSec));
        const UINT32 ch = format->nChannels;
        bool produced = false;
        for (UINT32 f = 0; f < frames; ++f) {
            float mono = 0.0f;
            if (isFloat) {
                const float* p = reinterpret_cast<const float*>(data) + static_cast<size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c) mono += p[c];
            } else {
                const int16_t* p = reinterpret_cast<const int16_t*>(data) + static_cast<size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c) mono += p[c] / 32768.0f;
            }
            ring[write] = mono / ch;
            write = (write + 1) % kWindow;
            if (++sinceHop >= kHop) {
                sinceHop = 0;
                Analyze();
                produced = true;
            }
        }
        return produced;
    }
};

SpectrumAnalyzer g_spectrumAnalyzer;

void PublishSpectrumBands() {
    std::lock_guard lock(g_stateMutex);
    for (int b = 0; b < kSpectrumBands; ++b) {
        g_state.bands[b] = g_spectrumAnalyzer.smooth[b];
    }
}

void DecayAndPublishSpectrum() {
    if (!g_spectrumBandsNeeded.load(std::memory_order_relaxed)) {
        return;
    }
    g_spectrumAnalyzer.Decay();
    std::lock_guard lock(g_stateMutex);
    for (int b = 0; b < kSpectrumBands; ++b) {
        g_state.bands[b] = g_spectrumAnalyzer.smooth[b];
    }
}

void PushWaveformSample(float amplitude) {
    std::lock_guard lock(g_stateMutex);

    float lastVal = 0.0f;
    if (g_state.waveformWrite > 0) {
        lastVal = g_state.waveform[(g_state.waveformWrite - 1) % g_state.waveform.size()];
    }

    // Apply an attack/release envelope (Exponential Moving Average)
    // Quick snappy attack (0.7) for beats, buttery smooth release (0.85) for decay.
    float smoothed;
    if (amplitude > lastVal) {
        smoothed = lastVal * 0.3f + amplitude * 0.7f;
    } else {
        smoothed = lastVal * 0.85f + amplitude * 0.15f;
    }

    g_state.waveform[g_state.waveformWrite % g_state.waveform.size()] = smoothed;
    ++g_state.waveformWrite;
}

void PushAudioChunks(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (g_spectrumBandsNeeded.load(std::memory_order_relaxed) &&
        g_spectrumAnalyzer.Feed(data, frames, format)) {
        PublishSpectrumBands();
    }
    if (!data || !frames || !format || !format->nChannels) {
        PushWaveformSample(0.0f);
        return;
    }

    constexpr UINT32 chunkFrames = 64;
    const UINT32 channels = format->nChannels;
    const bool isFloat =
        format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                     kSubTypeIeeeFloat));
    const bool is16 = format->wBitsPerSample == 16;

    if (!isFloat && !is16) {
        PushWaveformSample(SampleAudioAmplitude(data, frames, format));
        return;
    }

    for (UINT32 frame = 0; frame < frames; frame += chunkFrames) {
        const UINT32 chunk = std::min(chunkFrames, frames - frame);
        double sum = 0.0;
        const size_t samples = static_cast<size_t>(chunk) * channels;
        const size_t start = static_cast<size_t>(frame) * channels;

        if (isFloat) {
            auto* f = reinterpret_cast<float*>(data);
            for (size_t i = 0; i < samples; ++i) {
                const double v = f[start + i];
                sum += v * v;
            }
        } else {
            auto* s = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < samples; ++i) {
                const double v = s[start + i] / 32768.0;
                sum += v * v;
            }
        }

        PushWaveformSample(Clamp(static_cast<float>(std::sqrt(sum / samples) * 4.0), 0.0f, 1.0f));
    }
}

// --- Weather Fetching Helpers ---
std::string HttpGet(const wchar_t* host, const wchar_t* path, bool https = true) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"DynamicIsland/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        DI_LOG(L"Weather HttpGet: WinHttpOpen failed with error %lu", GetLastError());
        return response;
    }
    // Standalone 0.9: a slow or hostile server can't hang the thread or flood memory.
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);
    constexpr size_t kMaxResponse = 2 * 1024 * 1024;

    HINTERNET hConnect = WinHttpConnect(hSession, host, https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD size = 0;
                DWORD downloaded = 0;
                do {
                    if (WinHttpQueryDataAvailable(hRequest, &size) && size > 0) {
                        std::vector<char> buffer(size);
                        downloaded = 0;
                        if (WinHttpReadData(hRequest, buffer.data(), size, &downloaded) && downloaded > 0) {
                            response.append(buffer.data(), downloaded);
                        } else {
                            DI_LOG(L"Weather HttpGet: WinHttpReadData failed with error %lu", GetLastError());
                            break;
                        }
                    }
                    if (response.size() > kMaxResponse) {
                        DI_LOG(L"Weather HttpGet: response too large, dropped");
                        response.clear();
                        break;
                    }
                } while (size > 0);
            } else {
                DI_LOG(L"Weather HttpGet: WinHttpSendRequest/ReceiveResponse failed with error %lu", GetLastError());
            }
            WinHttpCloseHandle(hRequest);
        } else {
            DI_LOG(L"Weather HttpGet: WinHttpOpenRequest failed with error %lu", GetLastError());
        }
        WinHttpCloseHandle(hConnect);
    } else {
        DI_LOG(L"Weather HttpGet: WinHttpConnect failed with error %lu", GetLastError());
    }
    WinHttpCloseHandle(hSession);
    return response;
}

// Percent-encodes a city name for use as a wttr.in path segment. Only spaces
// used to be escaped, so a city containing a comma, an accent or any other
// non-ASCII character produced a malformed request; wttr.in then answered with
// an error page that the parser happily turned into "0 degrees".
std::wstring PercentEncodeUtf8(const std::wstring& text) {
    // Some users worked around the old space-only escaping by typing the escape
    // themselves ("New%20York"). Re-encoding that would send "New%2520York", so
    // an already-encoded value is passed through untouched. Only the exact
    // "%XX" shape counts, so a literal percent sign in a name still encodes.
    auto looksPreEncoded = [](const std::wstring& value) {
        const size_t pos = value.find(L'%');
        if (pos == std::wstring::npos || pos + 2 >= value.size()) {
            return false;
        }
        return iswxdigit(value[pos + 1]) && iswxdigit(value[pos + 2]);
    };
    if (looksPreEncoded(text)) {
        return text;
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return std::wstring();
    }

    std::string utf8(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);

    static constexpr wchar_t kHexDigits[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (const unsigned char c : utf8) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(kHexDigits[(c >> 4) & 0x0f]);
            out.push_back(kHexDigits[c & 0x0f]);
        }
    }
    return out;
}

DWORD WINAPI WeatherThreadProc(void*) {
    // Initial delay to avoid slowing down startup
    WaitForSingleObject(g_stopEvent, 3000);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        std::wstring cityOverride;
        bool isFahrenheit = false;
        bool weatherEnabled = true;
        {
            // g_settingsMutex, not g_stateMutex: all three values below live in
            // g_settings, so the old g_stateMutex here guarded nothing and left
            // the weatherCity string copy racing against LoadSettings().
            std::lock_guard lock(g_settingsMutex);
            weatherEnabled = g_settings.weather;
            cityOverride = g_settings.weatherCity;
            isFahrenheit = g_settings.weatherFahrenheit;
        }

        if (!weatherEnabled) {
            WaitForSingleObject(g_stopEvent, 2000);
            continue;
        }

        std::wstring url = L"/?format=j1";
        if (!cityOverride.empty()) {
            const std::wstring encodedCity = PercentEncodeUtf8(cityOverride);
            if (!encodedCity.empty()) {
                url = L"/" + encodedCity + L"?format=j1";
            }
        }

        DI_LOG(L"Weather: Requesting weather from wttr.in/host: wttr.in, path: %s", url.c_str());
        std::string wRes = HttpGet(L"wttr.in", url.c_str(), true);

        bool weatherCommitted = false;

        if (!wRes.empty()) {
            DI_LOG(L"Weather: Received response from wttr.in (size: %zu bytes)", wRes.size());
            float temp = 0.0f;
            int code = 0;
            std::wstring desc = L"";
            std::wstring windSpeed = L"";
            std::wstring windDir = L"";
            std::wstring humidity = L"";
            std::wstring feelsLike = L"";
            std::wstring cityLabel = L"Local Weather";

            const char* areaStr = strstr(wRes.c_str(), "\"areaName\":");
            if (areaStr) {
                const char* valStr = strstr(areaStr, "\"value\":");
                if (valStr) {
                    valStr += 8;
                    while (*valStr == ' ' || *valStr == '\"') valStr++;
                    const char* end = strchr(valStr, '\"');
                    if (end) {
                        std::string cityA(valStr, end - valStr);
                        int wchars_num = MultiByteToWideChar(CP_UTF8, 0, cityA.c_str(), -1, NULL, 0);
                        if (wchars_num > 0) {
                            std::vector<wchar_t> wstr(wchars_num);
                            MultiByteToWideChar(CP_UTF8, 0, cityA.c_str(), -1, &wstr[0], wchars_num);
                            cityLabel = wstr.data();
                        }
                    }
                }
            }

            const char* currentStr = strstr(wRes.c_str(), "\"current_condition\":");
            if (currentStr) {
                auto ParseStringField = [&](const char* key, std::wstring& out) {
                    const char* kStr = strstr(currentStr, key);
                    if (kStr) {
                        kStr += strlen(key);
                        while (*kStr == ' ' || *kStr == '\"' || *kStr == ':') kStr++;
                        const char* end = strchr(kStr, '\"');
                        if (end) {
                            std::string valA(kStr, end - kStr);
                            int wchars_num = MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, NULL, 0);
                            if (wchars_num > 0) {
                                std::vector<wchar_t> wstr(wchars_num);
                                MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, &wstr[0], wchars_num);
                                out = wstr.data();
                            }
                        }
                    }
                };

                const char* tempStr = strstr(currentStr, isFahrenheit ? "\"temp_F\":" : "\"temp_C\":");
                if (tempStr) {
                    tempStr += 9;
                    while (*tempStr == ' ' || *tempStr == '\"') tempStr++;
                    sscanf(tempStr, "%f", &temp);
                }
                const char* codeStr = strstr(currentStr, "\"weatherCode\":");
                if (codeStr) {
                    codeStr += 14;
                    while (*codeStr == ' ' || *codeStr == '\"') codeStr++;
                    sscanf(codeStr, "%d", &code);
                }

                const char* descStr = strstr(currentStr, "\"weatherDesc\":");
                if (descStr) {
                    const char* valStr = strstr(descStr, "\"value\":");
                    if (valStr) {
                        valStr += 8;
                        while (*valStr == ' ' || *valStr == '\"') valStr++;
                        const char* end = strchr(valStr, '\"');
                        if (end) {
                            std::string valA(valStr, end - valStr);
                            int wchars_num = MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, NULL, 0);
                            if (wchars_num > 0) {
                                std::vector<wchar_t> wstr(wchars_num);
                                MultiByteToWideChar(CP_UTF8, 0, valA.c_str(), -1, &wstr[0], wchars_num);
                                desc = wstr.data();
                                while(!desc.empty() && desc.back() == L' ') desc.pop_back();
                            }
                        }
                    }
                }

                ParseStringField(isFahrenheit ? "\"windspeedMiles\"" : "\"windspeedKmph\"", windSpeed);
                ParseStringField("\"winddir16Point\"", windDir);
                ParseStringField("\"humidity\"", humidity);
                ParseStringField(isFahrenheit ? "\"FeelsLikeF\"" : "\"FeelsLikeC\"", feelsLike);

                // Treat the response as usable only if a real reading came out
                // of it. wttr.in answers rate limiting and unknown locations
                // with a page that contains no current_condition (or an empty
                // one); committing that regardless is what made the island
                // display a confident, wrong "0 degrees".
                // 0 is not a valid WWO weather code, so it doubles as a
                // "nothing was parsed" sentinel here.
                weatherCommitted = (code != 0) || !desc.empty();

                std::wstring finalCity = cityOverride.empty() ? cityLabel : cityOverride;
                if (weatherCommitted) {
                    DI_LOG(L"Weather parsed success: city=%s, temp=%.1f, feelsLike=%s, humidity=%s%%, desc=%s",
                           finalCity.c_str(), temp, feelsLike.c_str(), humidity.c_str(), desc.c_str());
                } else {
                    DI_LOG(L"Weather: response had no usable reading, keeping previous data.");
                }
            } else {
                DI_LOG(L"Weather: Failed to find \"current_condition\" in response.");
            }

            if (weatherCommitted) {
                std::lock_guard lock(g_stateMutex);
                g_state.weather.hasData = true;
                g_state.weather.temperature = temp;
                g_state.weather.weatherCode = code;
                if (!cityOverride.empty()) g_state.weather.city = cityOverride;
                else g_state.weather.city = cityLabel;
                g_state.weather.weatherDesc = desc;
                g_state.weather.windSpeed = windSpeed;
                g_state.weather.windDir = windDir;
                g_state.weather.humidity = humidity;
                g_state.weather.feelsLike = feelsLike;
                g_state.weather.lastUpdated = NowSeconds();
            }
        } else {
            DI_LOG(L"Weather: HttpGet returned empty response.");
        }

        // Retry soon after a failure instead of leaving the dashboard empty for
        // a full refresh interval, but back off on repeated failures. wttr.in is
        // a free community service and rate limiting is one of the failures being
        // recovered from here, so a flat fast retry would sustain the very
        // condition it is reacting to.
        static const DWORD kBackoffMs[] = {60 * 1000, 2 * 60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000};
        static size_t backoffIndex = 0;

        DWORD waitMs;
        if (weatherCommitted) {
            backoffIndex = 0;
            waitMs = 15 * 60 * 1000;
        } else {
            waitMs = kBackoffMs[backoffIndex];
            if (backoffIndex + 1 < ARRAYSIZE(kBackoffMs)) {
                ++backoffIndex;
            }
        }

        HANDLE events[] = {g_stopEvent, g_settingsChangedEvent};
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, waitMs);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // The city changed, so retry immediately at full speed.
            backoffIndex = 0;
        }
    }
    return 0;
}

// ── Stocks (custom fork, custom.18) ─────────────────────────────────────────
// Prices and charts come from Yahoo Finance's public chart endpoint
// (query1/query2.finance.yahoo.com/v8/finance/chart/<ticker>). No account, no
// key and no cookies: the only thing sent is the ticker and the chart period.
// Yahoo rejects library user agents, so a browser one is used. The endpoint is
// unofficial and may change; failures are logged with a [Stocks] prefix.
namespace Stocks {

std::string HttpGetJson(const wchar_t* host, const std::wstring& path, DWORD* statusOut) {
    std::string response;
    *statusOut = 0;
    HINTERNET session = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        L"Chrome/131.0.0.0 Safari/537.36",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return response;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request) {
            const wchar_t* headers = L"Accept: application/json\r\n";
            if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                *statusOut = status;
                DWORD available = 0;
                do {
                    available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                        break;
                    }
                    std::vector<char> buffer(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) {
                        break;
                    }
                    response.append(buffer.data(), read);
                } while (response.size() < 4 * 1024 * 1024);
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return response;
}

// Locale-independent JSON number reader (strtod follows the C locale).
bool ParseJsonNumber(const std::string& j, size_t pos, double* out, size_t* endOut = nullptr) {
    size_t p = pos;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) ++p;
    bool neg = false;
    if (p < j.size() && j[p] == '-') {
        neg = true;
        ++p;
    }
    if (p >= j.size() || !(j[p] >= '0' && j[p] <= '9')) {
        return false;
    }
    double value = 0.0;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        value = value * 10.0 + (j[p] - '0');
        ++p;
    }
    if (p < j.size() && j[p] == '.') {
        ++p;
        double scale = 0.1;
        while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
            value += (j[p] - '0') * scale;
            scale *= 0.1;
            ++p;
        }
    }
    if (p < j.size() && (j[p] == 'e' || j[p] == 'E')) {
        ++p;
        bool expNeg = false;
        if (p < j.size() && (j[p] == '+' || j[p] == '-')) {
            expNeg = j[p] == '-';
            ++p;
        }
        int e = 0;
        while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
            e = std::min(e * 10 + (j[p] - '0'), 400);
            ++p;
        }
        value *= std::pow(10.0, expNeg ? -e : e);
    }
    *out = neg ? -value : value;
    if (endOut) *endOut = p;
    return true;
}

bool FindNumber(const std::string& j, const char* key, double* out) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    if (p == std::string::npos) {
        return false;
    }
    return ParseJsonNumber(j, p + k.size(), out);
}

std::wstring FindString(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = j.find(k);
    if (p == std::string::npos) {
        return L"";
    }
    const size_t start = p + k.size();
    const size_t end = j.find('"', start);
    if (end == std::string::npos || end - start > 16) {
        return L"";
    }
    std::wstring w;
    for (size_t i = start; i < end; ++i) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(j[i])));
    return w;
}

// The "close" series of the first quote block. Nulls (bars with no trade) are skipped.
std::vector<float> FindCloses(const std::string& j) {
    std::vector<float> out;
    const char* key = "\"close\":[";
    const size_t p = j.find(key);
    if (p == std::string::npos) {
        return out;
    }
    size_t i = p + strlen(key);
    while (i < j.size() && j[i] != ']') {
        if (j.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            double v = 0.0;
            size_t end = i;
            if (ParseJsonNumber(j, i, &v, &end)) {
                out.push_back(static_cast<float>(v));
                i = end;
            } else {
                ++i;
            }
        }
        while (i < j.size() && (j[i] == ',' || j[i] == ' ')) ++i;
    }
    return out;
}

void RangeQuery(StocksRange range, const wchar_t** rangeOut, const wchar_t** intervalOut) {
    switch (range) {
        case StocksRange::FiveDays:  *rangeOut = L"5d";  *intervalOut = L"30m"; break;
        case StocksRange::Month:     *rangeOut = L"1mo"; *intervalOut = L"1d";  break;
        case StocksRange::SixMonths: *rangeOut = L"6mo"; *intervalOut = L"1d";  break;
        case StocksRange::Year:      *rangeOut = L"1y";  *intervalOut = L"1wk"; break;
        case StocksRange::Day:
        default:                     *rangeOut = L"1d";  *intervalOut = L"5m";  break;
    }
}

bool FetchQuote(const std::wstring& symbol, StocksRange range, StockQuote* q) {
    const wchar_t* r = L"1d";
    const wchar_t* iv = L"5m";
    RangeQuery(range, &r, &iv);
    std::wstring path = L"/v8/finance/chart/";
    path += PercentEncodeUtf8(symbol);
    path += L"?range=";
    path += r;
    path += L"&interval=";
    path += iv;
    path += L"&includePrePost=false";

    const wchar_t* hosts[] = {L"query1.finance.yahoo.com", L"query2.finance.yahoo.com"};
    for (const wchar_t* host : hosts) {
        DWORD status = 0;
        const std::string json = HttpGetJson(host, path, &status);
        if (status != 200 || json.empty()) {
            DI_LOG(L"[Stocks] %s: %s answered HTTP %lu (%zu bytes)", symbol.c_str(), host, status, json.size());
            continue;
        }
        if (json.find("\"result\":[") == std::string::npos) {
            DI_LOG(L"[Stocks] %s: no chart in the answer", symbol.c_str());
            return false;
        }
        std::vector<float> closes = FindCloses(json);
        double price = 0.0;
        if (!FindNumber(json, "regularMarketPrice", &price) || price <= 0.0) {
            price = closes.empty() ? 0.0 : closes.back();
        }
        double base = 0.0;
        if (!FindNumber(json, "chartPreviousClose", &base) || base <= 0.0) {
            if (!FindNumber(json, "previousClose", &base) || base <= 0.0) {
                base = closes.empty() ? price : closes.front();
            }
        }
        if (price <= 0.0) {
            DI_LOG(L"[Stocks] %s: no price in the answer", symbol.c_str());
            return false;
        }
        // The last bar can lag the live price by a few minutes; end the line on it.
        if (!closes.empty() && range == StocksRange::Day) {
            closes.back() = static_cast<float>(price);
        }
        // A sparkline needs no more than ~120 points.
        if (closes.size() > 120) {
            std::vector<float> thin;
            const double step = static_cast<double>(closes.size() - 1) / 119.0;
            for (int k = 0; k < 120; ++k) thin.push_back(closes[static_cast<size_t>(std::lround(k * step))]);
            closes = std::move(thin);
        }
        q->price = price;
        q->basePrice = base;
        q->currency = FindString(json, "currency");
        q->points = std::move(closes);
        q->ok = true;
        return true;
    }
    return false;
}

void FetchAll(const Settings& settings) {
    {
        std::lock_guard lock(g_stateMutex);
        g_state.stocks.loading = true;
    }
    int okCount = 0;
    for (int i = 0; i < kStockSlots; ++i) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            return;
        }
        const std::wstring symbol = settings.stockSymbols[i];
        const StockInfo* info = FindStock(symbol);
        StockQuote q;
        q.symbol = symbol;
        q.label = info ? info->label : symbol;
        const bool ok = FetchQuote(symbol, settings.stocksRange, &q);
        if (ok) ++okCount;
        {
            std::lock_guard lock(g_stateMutex);
            StockQuote& slot = g_state.stocks.quotes[i];
            if (ok || slot.symbol != symbol) {
                slot = std::move(q);  // a failed refresh keeps the last good chart
            }
        }
        g_layoutDirty = true;
        TriggerNudge();
        Sleep(120);  // be gentle with Yahoo
    }
    {
        std::lock_guard lock(g_stateMutex);
        g_state.stocks.loading = false;
        g_state.stocks.lastUpdated = NowSeconds();
        GetLocalTime(&g_state.stocks.updatedAt);
        bool any = false;
        for (const StockQuote& q : g_state.stocks.quotes) any = any || q.ok;
        g_state.stocks.anyOk = any;
    }
    DI_LOG(L"[Stocks] updated: %d of %d", okCount, kStockSlots);
    g_layoutDirty = true;
    TriggerNudge();
}

}  // namespace Stocks

// Fetches once at start, then only while the stocks tab was looked at in the
// last 10 minutes, every Stocks.RefreshMinutes. A settings change or opening
// the tab with old data fetches right away (at most every 20 s).
DWORD WINAPI StocksThreadProc(void*) {
    double lastFetch = -1e9;
    std::wstring lastKey;
    while (true) {
        const Settings settings = GetSettingsCopy();
        if (settings.stocksModule) {
            std::wstring key = std::to_wstring(static_cast<int>(settings.stocksRange));
            for (const std::wstring& sym : settings.stockSymbols) key += L"|" + sym;
            const double now = NowSeconds();
            const bool keyChanged = key != lastKey;
            const bool viewedRecently = now - g_stocksLastViewed.load() < 600.0;
            const bool due = now - lastFetch >= settings.stocksRefreshMinutes * 60.0 - 1.0;
            const bool forced = g_stocksForceFetch.exchange(false) && now - lastFetch > 20.0;
            if (keyChanged || forced || (due && viewedRecently)) {
                lastKey = key;
                lastFetch = now;
                Stocks::FetchAll(settings);
            }
        }
        HANDLE events[] = {g_stopEvent, g_stocksWakeEvent};
        const DWORD count = g_stocksWakeEvent ? 2 : 1;
        if (WaitForMultipleObjects(count, events, FALSE, 30 * 1000) == WAIT_OBJECT_0) {
            break;
        }
    }
    return 0;
}

DWORD WINAPI AudioThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        // Don't touch the audio engine at all unless a playing waveform is
        // actually going to be shown right now. Cheap poll, no capture open.
        if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
            WaitForSingleObject(g_stopEvent, 250);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mixFormat = nullptr;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetMixFormat(&mixFormat);
        }
        if (SUCCEEDED(hr)) {
            REFERENCE_TIME bufferDuration = 10000000 / 5;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, mixFormat, nullptr);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&capture));
        }
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }

        if (FAILED(hr)) {
            if (mixFormat) {
                CoTaskMemFree(mixFormat);
            }
            WaitForSingleObject(g_stopEvent, 2500);
            continue;
        }

        // Watchdog state — reset every time a capture session (re)starts so we
        // don't flag a stall before the first packet has even arrived.
        double lastPacketTime = NowSeconds();
        double nextWatchdogCheck = lastPacketTime + 1.0;

        while (WaitForSingleObject(g_stopEvent, 16) == WAIT_TIMEOUT) {
            if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
                break;
            }
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                break;
            }

            float amplitude = 0.0f;
            int packets = 0;
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    break;
                }

                lastPacketTime = NowSeconds();

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    amplitude = std::max(amplitude, SampleAudioAmplitude(data, frames, mixFormat));
                    PushAudioChunks(data, frames, mixFormat);
                } else {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
                capture->ReleaseBuffer(frames);
                ++packets;

                if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                    packetFrames = 0;
                }
            }

            if (packets > 0) {
                if (amplitude <= 0.001f) {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
            } else {
                PushWaveformSample(0.0f);
                DecayAndPublishSpectrum();
            }

            // --- Stall watchdog -------------------------------------------
            // Checked at most once a second (cheap: one time comparison plus
            // a mutex-protected bool read, same cost as work already done
            // elsewhere in this loop). Only forces a reconnect if SMTC says
            // media is playing but we've had zero packets for 5+ seconds —
            // that combination almost never happens unless the loopback
            // engine has stalled, so it won't fire during normal silence.
            const double now = NowSeconds();
            if (now >= nextWatchdogCheck) {
                nextWatchdogCheck = now + 1.0;
                bool mediaPlayingNow = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaPlayingNow = g_state.media.playing;
                }
                if (mediaPlayingNow && (now - lastPacketTime) > 5.0) {
                    DI_LOG(L"Audio loopback capture appears stalled while media is playing; reconnecting...");
                    break;
                }
            }
        }

        client->Stop();
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}
void UpdateBatterySnapshot() {
    SYSTEM_POWER_STATUS status = {};
    if (!GetSystemPowerStatus(&status)) {
        return;
    }

    bool newCharging = (status.ACLineStatus == 1);
    int newPercent = status.BatteryLifePercent == 255 ? 100 : status.BatteryLifePercent;

    bool triggerAlert = false;

    {
        std::lock_guard lock(g_stateMutex);
        static bool s_batteryInit = false;
        if (!s_batteryInit) {
            g_state.battery.charging = newCharging;
            g_state.battery.percent = newPercent;
            s_batteryInit = true;
        }

        if (g_state.battery.charging != newCharging) {
            triggerAlert = true;
        }

        if (!newCharging && newPercent < g_state.battery.percent && (newPercent == 20 || newPercent == 10)) {
            triggerAlert = true;
        }

        g_state.battery.charging = newCharging;
        g_state.battery.percent = newPercent;
        g_state.battery.secondsRemaining = status.BatteryLifeTime;
        g_state.battery.low = (!newCharging && newPercent <= 20);

        if (triggerAlert) {
            g_state.battery.active = true;
            g_state.battery.expiresAt = NowSeconds() + 4.0;
        }
    }

    if (triggerAlert) {
        TriggerNudge();
    }
}

ULONGLONG FileTimeToUInt64(FILETIME ft) {
    ULARGE_INTEGER value = {};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static PDH_HQUERY g_gpuQuery = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;

static void InitGpuQuery() {
    if (g_gpuQuery == NULL) {
        if (PdhOpenQueryW(NULL, 0, &g_gpuQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_gpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter);
            PdhCollectQueryData(g_gpuQuery);
        }
    }
}

static int GetGpuUsage() {
    InitGpuQuery();
    if (!g_gpuQuery || !g_gpuCounter) return 0;

    PdhCollectQueryData(g_gpuQuery);

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);

    if (bufferSize > 0) {
        std::vector<BYTE> buffer(bufferSize);
        PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());

        if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
            double total = 0;
            for (DWORD i = 0; i < itemCount; i++) {
                if (items[i].szName && wcsstr(items[i].szName, L"engtype_3D")) {
                    total += items[i].FmtValue.doubleValue;
                }
            }
            return ClampInt(static_cast<int>(total), 0, 100);
        }
    }
    return 0;
}

static PDH_HQUERY g_netQuery = NULL;
static PDH_HCOUNTER g_netUpCounter = NULL;
static PDH_HCOUNTER g_netDownCounter = NULL;

static void InitNetQuery() {
    if (g_netQuery == NULL) {
        if (PdhOpenQueryW(NULL, 0, &g_netQuery) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(g_netQuery, L"\\Network Interface(*)\\Bytes Sent/sec", 0, &g_netUpCounter);
            PdhAddEnglishCounterW(g_netQuery, L"\\Network Interface(*)\\Bytes Received/sec", 0, &g_netDownCounter);
            PdhCollectQueryData(g_netQuery);
        }
    }
}

static void GetNetworkUsage(float& outUpMbps, float& outDownMbps) {
    outUpMbps = 0.0f;
    outDownMbps = 0.0f;
    InitNetQuery();
    if (!g_netQuery || !g_netUpCounter || !g_netDownCounter) return;

    PdhCollectQueryData(g_netQuery);

    auto getSum = [](PDH_HCOUNTER counter) -> double {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, NULL);
        if (bufferSize > 0) {
            std::vector<BYTE> buffer(bufferSize);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                double total = 0;
                for (DWORD i = 0; i < itemCount; i++) {
                    if (items[i].szName) {
                        if (wcsstr(items[i].szName, L"Loopback") == nullptr) {
                            total += items[i].FmtValue.doubleValue;
                        }
                    }
                }
                return total;
            }
        }
        return 0.0;
    };

    // Bytes to Mbps
    outUpMbps = static_cast<float>(getSum(g_netUpCounter) * 8.0 / 1000000.0);
    outDownMbps = static_cast<float>(getSum(g_netDownCounter) * 8.0 / 1000000.0);
}

// ---- Endpoint volume notifications ----
// Event-driven rather than polled: a 1s poll turned a held volume key into
// one visible step per second.
void ApplyVolumeReading(float level, bool muted, bool canSurface) {
    const int percent = ClampInt(static_cast<int>(level * 100.0f + 0.5f), 0, 100);

    bool flyoutEnabled = false;
    {
        std::lock_guard settingsLock(g_settingsMutex);
        flyoutEnabled = g_settings.volume;
    }

    bool surfaced = false;
    {
        std::lock_guard lock(g_stateMutex);
        const bool changed = percent != g_state.system.volumePercent ||
                             muted != g_state.system.volumeMuted;
        g_state.system.volumePercent = percent;
        g_state.system.volumeMuted = muted;
        g_state.muted = muted;

        if (canSurface && changed && flyoutEnabled) {
            surfaced = !g_state.volume.active;
            g_state.volume.active = true;
            g_state.volume.percent = percent;
            g_state.volume.muted = muted;
            g_state.volume.deviceName = L"System audio";
            g_state.volume.expiresAt = NowSeconds() + 1.8;
        }
    }

    // Only the first step nudges; nudging on every step made the pill bounce.
    if (surfaced) {
        TriggerNudge();
    }
}

// g_volumeEndpointStale is declared with the other globals near the top of the file.

// Called on audio service threads, not the render thread.
class VolumeNotifyClient final : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
        } else if (riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (data) {
            ApplyVolumeReading(data->fMasterVolume, data->bMuted != FALSE, true);
        }
        return S_OK;
    }

    // MMDevice API calls are not allowed from inside these callbacks, so the
    // render thread rebinds; the posted message wakes it even while parked.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_volumeEndpointStale = true;
            if (HWND hwnd = g_hwnd) {
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
};

// Owned by the render thread.
ComPtr<IMMDeviceEnumerator> g_volumeEnumerator;
ComPtr<IAudioEndpointVolume> g_endpointVolume;
VolumeNotifyClient* g_volumeNotifyClient = nullptr;

void BindVolumeEndpoint() {
    if (!g_volumeEndpointStale.exchange(false)) {
        return;
    }

    if (!g_volumeNotifyClient) {
        g_volumeNotifyClient = new VolumeNotifyClient();
    }
    if (!g_volumeEnumerator) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&g_volumeEnumerator)))) {
            g_volumeEnumerator.Reset();
            return;
        }
        g_volumeEnumerator->RegisterEndpointNotificationCallback(g_volumeNotifyClient);
    }

    if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = g_volumeEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(g_endpointVolume.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        hr = g_endpointVolume->RegisterControlChangeNotify(g_volumeNotifyClient);
    }

    float level = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(hr) && SUCCEEDED(g_endpointVolume->GetMasterVolumeLevelScalar(&level)) &&
        SUCCEEDED(g_endpointVolume->GetMute(&muted))) {
        // Baseline only: switching device must not pop the flyout.
        ApplyVolumeReading(level, muted != FALSE, false);
    } else if (g_endpointVolume) {
        g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        g_endpointVolume.Reset();
    }
}

void UnbindVolumeEndpoint() {
    if (g_volumeNotifyClient) {
        if (g_endpointVolume) {
            g_endpointVolume->UnregisterControlChangeNotify(g_volumeNotifyClient);
        }
        if (g_volumeEnumerator) {
            g_volumeEnumerator->UnregisterEndpointNotificationCallback(g_volumeNotifyClient);
        }
    }
    g_endpointVolume.Reset();
    g_volumeEnumerator.Reset();
    if (g_volumeNotifyClient) {
        g_volumeNotifyClient->Release();
        g_volumeNotifyClient = nullptr;
    }
    g_volumeEndpointStale = true;
}

void UpdateSystemSnapshot(bool includeGpuStats, bool includeNetStats) {
    SystemSnapshot next;
    {
        std::lock_guard lock(g_stateMutex);
        next = g_state.system;
        next.charging = g_state.system.charging;
    }

    // GPU/network sampling is comparatively expensive (the GPU counter in
    // particular enumerates every GPU engine instance across every process
    // using the GPU), so only sample them when something on screen is
    // actually displaying them. Otherwise just carry the last sampled
    // value forward (already done via `next = g_state.system` above).
    if (includeGpuStats) {
        next.gpuPercent = GetGpuUsage();
    }
    if (includeNetStats) {
        GetNetworkUsage(next.netUpMbps, next.netDownMbps);
    }

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        next.memoryPercent = static_cast<int>(memory.dwMemoryLoad);
        next.memoryTotalGB = static_cast<float>(memory.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);
        next.memoryUsedGB = next.memoryTotalGB - static_cast<float>(memory.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    }

    ULARGE_INTEGER freeBytesAvailable = {};
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER totalFreeBytes = {};
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvailable, &totalBytes, &totalFreeBytes) &&
        totalBytes.QuadPart > 0) {
        next.diskFreePercent = ClampInt(
            static_cast<int>(totalFreeBytes.QuadPart * 100 / totalBytes.QuadPart), 0, 100);
    }

    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_hwnd) {
        wchar_t title[96] = {};
        GetWindowTextW(foreground, title, ARRAYSIZE(title));
        if (!IsIgnorableForegroundWindow(foreground, title)) {
            next.foregroundTitle = title;
            if (next.foregroundTitle.size() > 42) {
                next.foregroundTitle.resize(42);
                next.foregroundTitle += L"...";
            }

        } else {
            next.foregroundTitle.clear();
        }
    }

    FILETIME idle = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const ULONGLONG idleNow = FileTimeToUInt64(idle);
        const ULONGLONG kernelNow = FileTimeToUInt64(kernel);
        const ULONGLONG userNow = FileTimeToUInt64(user);
        const ULONGLONG idlePrev = FileTimeToUInt64(g_prevIdleTime);
        const ULONGLONG kernelPrev = FileTimeToUInt64(g_prevKernelTime);
        const ULONGLONG userPrev = FileTimeToUInt64(g_prevUserTime);

        const ULONGLONG total = (kernelNow - kernelPrev) + (userNow - userPrev);
        const ULONGLONG idleDelta = idleNow - idlePrev;
        if (total > 0 && kernelPrev != 0) {
            next.cpuPercent = ClampInt(static_cast<int>((total - idleDelta) * 100 / total), 0, 100);
        }

        g_prevIdleTime = idle;
        g_prevKernelTime = kernel;
        g_prevUserTime = user;
    }

    std::lock_guard lock(g_stateMutex);
    // Volume is owned by ApplyVolumeReading and may have moved since `next` was copied.
    next.volumePercent = g_state.system.volumePercent;
    next.volumeMuted = g_state.system.volumeMuted;
    g_state.system = next;
    g_state.muted = next.volumeMuted;
}

// Built-in panel brightness (0-100), or -1 when there is no such panel (desktops)
// or the query fails. Same IOCTL the Windows brightness slider uses, so no WMI.
int ReadPanelBrightness(bool onAc) {
    constexpr DWORD kIoctlQueryDisplayBrightness = 0x230498;  // CTL_CODE(FILE_DEVICE_VIDEO, 0x126, METHOD_BUFFERED, FILE_ANY_ACCESS)
    struct DisplayBrightness {  // DISPLAY_BRIGHTNESS from ntddvdeo.h
        UCHAR policy;           // bit 0 = AC level valid, bit 1 = DC level valid
        UCHAR acBrightness;
        UCHAR dcBrightness;
    };

    static double s_retryAt = 0.0;  // desktops have no LCD device, so do not retry every poll
    const double now = NowSeconds();
    if (now < s_retryAt) {
        return -1;
    }

    HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (lcd == INVALID_HANDLE_VALUE) {
        lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    }
    if (lcd == INVALID_HANDLE_VALUE) {
        // Standalone 0.9: back off 30 s -> 10 min and log once. A desktop simply has
        // no built-in panel; retrying every 30 s forever only filled the log.
        static double s_backoff = 30.0;
        static bool s_logged = false;
        if (!s_logged) {
            DI_LOG(L"Brightness: no built-in display to read (error %lu); checking again now and then.", GetLastError());
            s_logged = true;
        }
        s_retryAt = now + s_backoff;
        s_backoff = std::min(s_backoff * 2.0, 600.0);
        return -1;
    }

    DisplayBrightness db = {};
    DWORD returned = 0;
    const bool ok = DeviceIoControl(lcd, kIoctlQueryDisplayBrightness, nullptr, 0, &db, sizeof(db),
                                    &returned, nullptr) && returned >= sizeof(db);
    const DWORD error = ok ? 0 : GetLastError();
    CloseHandle(lcd);
    if (!ok) {
        DI_LOG(L"Brightness: query failed (error %lu, %lu bytes), retrying in 30s", error, returned);
        s_retryAt = now + 30.0;
        return -1;
    }

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        s_loggedFirst = true;
        DI_LOG(L"Brightness: policy=%d ac=%d dc=%d", db.policy, db.acBrightness, db.dcBrightness);
    }
    // Pick the level for the current power source, falling back to the other one
    // when the driver only reports one of them.
    const bool hasAc = (db.policy & 1) != 0;
    const bool hasDc = (db.policy & 2) != 0;
    int level = onAc ? db.acBrightness : db.dcBrightness;
    if (onAc && !hasAc && hasDc) {
        level = db.dcBrightness;
    } else if (!onAc && !hasDc && hasAc) {
        level = db.acBrightness;
    }
    return ClampInt(level, 0, 100);
}

void UpdateBrightnessSnapshot() {
    SYSTEM_POWER_STATUS power = {};
    GetSystemPowerStatus(&power);
    const int level = ReadPanelBrightness(power.ACLineStatus != 0);
    if (level < 0) {
        return;
    }

    std::lock_guard lock(g_stateMutex);
    // Plugging in or unplugging swaps between the AC and battery levels, which is
    // not the user moving the slider, so that sample only resets the baseline.
    const bool changed = g_brightnessInitialized && level != g_lastBrightness &&
                         power.ACLineStatus == g_brightnessPowerSource;
    if (changed && g_settings.brightness) {
        g_state.brightness.active = true;
        g_state.brightness.percent = level;
        g_state.brightness.expiresAt = NowSeconds() + 1.8;
        TriggerNudge();
    }
    g_lastBrightness = level;
    g_brightnessPowerSource = power.ACLineStatus;
    g_brightnessInitialized = true;
}

// ---- Privacy indicator helpers ----
std::wstring CleanActiveAppName(const std::wstring& raw) {
    std::wstring name;
    size_t hashPos = raw.rfind(L'#');
    if (hashPos != std::wstring::npos) {
        name = raw.substr(hashPos + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
            name.resize(name.size() - 4);
        }
    } else {
        size_t underPos = raw.find(L'_');
        name = (underPos != std::wstring::npos) ? raw.substr(0, underPos) : raw;
        if (name.rfind(L"Microsoft.", 0) == 0) name = name.substr(10);
        if (name.rfind(L"Windows.", 0) == 0) name = name.substr(8);
        else if (name.rfind(L"Windows", 0) == 0 && name.size() > 7) name = name.substr(7);
    }
    std::wstring lower = ToLowerCopy(name);
    if (lower == L"msedge") return L"Microsoft Edge";
    if (lower == L"chrome") return L"Google Chrome";
    if (lower == L"brave") return L"Brave";
    if (lower == L"firefox") return L"Firefox";
    if (lower == L"discord") return L"Discord";
    if (lower == L"zoom") return L"Zoom";
    if (lower == L"obs64" || lower == L"obs32" || lower == L"obs") return L"OBS Studio";
    if (lower == L"teams") return L"Microsoft Teams";
    if (lower == L"skype") return L"Skype";
    if (lower == L"audacity") return L"Audacity";
    if (lower == L"fl64" || lower == L"fl") return L"FL Studio";
    if (lower == L"ciscocollabhost") return L"Webex";
    return name;
}

// Reads a REG_QWORD, returning false when the value is absent or is not a
// well-formed 64-bit quantity. The previous code read straight into a uint64_t
// without checking the type or size, so a shorter value could leave the high
// bytes untouched and fake a zero timestamp.
bool ReadRegQword(HKEY key, const wchar_t* name, uint64_t* out) {
    DWORD type = 0;
    uint64_t value = 0;
    DWORD dataSize = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &dataSize) != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_QWORD || dataSize != sizeof(value)) {
        return false;
    }
    *out = value;
    return true;
}

// A ConsentStore entry counts as in-use only when Windows has recorded a start
// and has not yet recorded a stop. Testing LastUsedTimeStop == 0 on its own
// also matched entries that had never been used at all (both timestamps zero),
// which is what left the microphone dot lit permanently for some users even
// though Windows' own privacy indicator showed nothing in use.
bool ConsentStoreEntryInUse(HKEY key) {
    uint64_t stopTime = 0;
    if (!ReadRegQword(key, L"LastUsedTimeStop", &stopTime) || stopTime != 0) {
        return false;
    }

    uint64_t startTime = 0;
    if (ReadRegQword(key, L"LastUsedTimeStart", &startTime)) {
        return startTime != 0;
    }

    // Stop says "not stopped" but there is no readable start timestamp. This is
    // the shape of a never-used entry, so it is reported as not in use, but log
    // it once: if some Windows build stores these differently it would otherwise
    // be an indicator that silently stops working with nothing to explain it.
    static std::atomic<bool> loggedMissingStart{false};
    if (!loggedMissingStart.exchange(true)) {
        DI_LOG(L"Privacy indicator: a ConsentStore entry has LastUsedTimeStop=0 but no "
               L"readable REG_QWORD LastUsedTimeStart; treating it as not in use.");
    }
    return false;
}

bool IsDeviceActiveViaRegistry(const wchar_t* capability, std::wstring* outAppName = nullptr) {
    bool isActive = false;
    std::wstring basePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    basePath += capability;

    auto CheckSubkeys = [&](HKEY hKeyParent) -> bool {
        constexpr REGSAM kSam = KEY_READ | KEY_WOW64_64KEY;
        DWORD index = 0;
        wchar_t subKeyName[256];
        DWORD nameLen = ARRAYSIZE(subKeyName);
        while (RegEnumKeyExW(hKeyParent, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSub;
            if (RegOpenKeyExW(hKeyParent, subKeyName, 0, kSam, &hSub) == ERROR_SUCCESS) {
                if (_wcsicmp(subKeyName, L"NonPackaged") == 0) {
                    DWORD npIndex = 0;
                    wchar_t npSubKeyName[256];
                    DWORD npNameLen = ARRAYSIZE(npSubKeyName);
                    while (RegEnumKeyExW(hSub, npIndex, npSubKeyName, &npNameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                        HKEY hNpSub;
                        if (RegOpenKeyExW(hSub, npSubKeyName, 0, kSam, &hNpSub) == ERROR_SUCCESS) {
                            if (ConsentStoreEntryInUse(hNpSub)) {
                                if (outAppName && outAppName->empty()) {
                                    *outAppName = CleanActiveAppName(npSubKeyName);
                                }
                                RegCloseKey(hNpSub);
                                RegCloseKey(hSub);
                                return true;
                            }
                            RegCloseKey(hNpSub);
                        }
                        npIndex++;
                        npNameLen = ARRAYSIZE(npSubKeyName);
                    }
                } else {
                    if (ConsentStoreEntryInUse(hSub)) {
                        if (outAppName && outAppName->empty()) {
                            *outAppName = CleanActiveAppName(subKeyName);
                        }
                        RegCloseKey(hSub);
                        return true;
                    }
                }
                RegCloseKey(hSub);
            }
            index++;
            nameLen = ARRAYSIZE(subKeyName);
        }
        return false;
    };

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, basePath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        isActive = CheckSubkeys(hKey);
        RegCloseKey(hKey);
    }

    if (!isActive) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, basePath.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            isActive = CheckSubkeys(hKey);
            RegCloseKey(hKey);
        }
    }

    return isActive;
}

bool IsMicrophoneActive(std::wstring* outAppName = nullptr) {
    return IsDeviceActiveViaRegistry(L"microphone", outAppName);
}

bool IsCameraActive(std::wstring* outAppName = nullptr) {
    return IsDeviceActiveViaRegistry(L"webcam", outAppName);
}

void UpdateProgressSnapshot() {
    const int progress = g_progressPercent.load();
    std::lock_guard lock(g_stateMutex);
    g_state.progress.active = progress >= 0 && progress <= 100;
    g_state.progress.percent = ClampInt(progress, 0, 100);
}

void UpdatePrivacyIndicators() {
    std::wstring micApp;
    std::wstring camApp;
    const bool mic = (g_settings.privacyDots && g_settings.privacyDotsMic) ? IsMicrophoneActive(&micApp) : false;
    const bool cam = (g_settings.privacyDots && g_settings.privacyDotsCam) ? IsCameraActive(&camApp) : false;
    std::lock_guard lock(g_stateMutex);
    g_state.system.micActive = mic;
    g_state.system.cameraActive = cam;
    g_state.system.micApp = micApp;
    g_state.system.cameraApp = camApp;
}

void SetClickThrough(HWND hwnd, bool clickThrough) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool has = (exStyle & WS_EX_TRANSPARENT) != 0;
    if (clickThrough == has) {
        return;
    }

    if (clickThrough) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }

    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void OpenRelevantApp();


void ToggleEndpointMute();
void SeekMediaToTicks(int64_t targetTicks);

void HandleStatusClickAtPoint(HWND hwnd, LPARAM lParam) {
    return;
}

void StartFocusTimer(int minutes, bool isBreak) {
    {
        std::lock_guard lock(g_stateMutex);
        g_state.timer.active = true;
        g_state.timer.running = true;
        g_state.timer.isBreak = isBreak;
        g_state.timer.totalSeconds = minutes * 60;
        g_state.timer.endsAt = NowSeconds() + minutes * 60;
        g_state.timer.remainingAtPause = 0.0;
        g_state.timer.justFinished = false;
    }
    TriggerNudge();
}

void ToggleTimerPause() {
    {
        std::lock_guard lock(g_stateMutex);
        if (!g_state.timer.active) {
            return;
        }
        const double now = NowSeconds();
        if (g_state.timer.running) {
            g_state.timer.remainingAtPause = std::max(0.0, g_state.timer.endsAt - now);
            g_state.timer.running = false;
        } else {
            g_state.timer.endsAt = now + g_state.timer.remainingAtPause;
            g_state.timer.running = true;
        }
    }
    TriggerNudge();
}

void StopFocusTimer() {
    {
        std::lock_guard lock(g_stateMutex);
        g_state.timer.active = false;
        g_state.timer.running = false;
        g_state.timer.justFinished = false;
    }
    TriggerNudge();
}

void ToggleEndpointMute() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(volume.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) {
        BOOL muted = FALSE;
        volume->GetMute(&muted);
        volume->SetMute(!muted, nullptr);
        std::lock_guard lock(g_stateMutex);
        g_state.muted = !muted;
    }
}

// Shared by the scrubber's live drag updates and its on-release commit.
// Standalone 0.9.1: one seek at a time. While the player is still busy with the
// previous one, newer drag positions just replace the pending target, so a slow
// player (Apple Music) isn't flooded with requests and ends at the last position.
std::atomic<int64_t> g_seekPendingTicks{-1};
std::atomic<bool> g_seekWorkerRunning{false};

void SeekWorkerOnce(int64_t targetTicks);

void SeekMediaToTicks(int64_t targetTicks) {
    g_seekPendingTicks.store(targetTicks);
    bool expected = false;
    if (!g_seekWorkerRunning.compare_exchange_strong(expected, true)) {
        return;  // the running worker will pick the new target up
    }
    std::thread([]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            const int64_t target = g_seekPendingTicks.exchange(-1);
            if (target < 0) {
                g_seekWorkerRunning.store(false);
                // A target may have arrived between the exchange and the store.
                bool again = false;
                if (g_seekPendingTicks.load() >= 0 && g_seekWorkerRunning.compare_exchange_strong(again, true)) {
                    continue;
                }
                break;
            }
            SeekWorkerOnce(target);
        }
    }).detach();
}

void SeekWorkerOnce(int64_t targetTicks) {
        try {
            using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
            auto manager = Manager::RequestAsync().get();
            if (manager) {
                auto sessions = manager.GetSessions();
                std::wstring currentAumid;
                {
                    std::lock_guard lock(g_stateMutex);
                    currentAumid = g_state.media.sourceAppUserModelId;
                }
                winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session = nullptr;
                for (auto const& s : sessions) {
                    if (s.SourceAppUserModelId().c_str() == currentAumid) {
                        session = s;
                        break;
                    }
                }
                if (!session) session = manager.GetCurrentSession();

                if (session) {
                    // Positions on the island count from StartTime; the player wants its own scale.
                    const auto timeline = session.GetTimelineProperties();
                    const int64_t absolute = targetTicks + (timeline ? timeline.StartTime().count() : 0);
                    const bool ok = AwaitOr(session.TryChangePlaybackPositionAsync(absolute), 3000, false);
                    if (!ok) {
                        static std::atomic<int> s_logged{0};
                        if (s_logged.fetch_add(1) < 3) {
                            const bool enabled = session.GetPlaybackInfo().Controls().IsPlaybackPositionEnabled();
                            DI_LOG(L"[Media] seek refused by %s (player allows seeking: %s)",
                                   session.SourceAppUserModelId().c_str(), enabled ? L"yes" : L"no");
                        }
                    }
                }
            }
        } catch (...) {}
}

// Resolves the SMTC session matching the island's current media source, falling
// back to whatever Windows reports as the current session. Must be called on a
// thread with an initialised apartment.
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession ResolveMediaSession() {
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    auto manager = Manager::RequestAsync().get();
    if (!manager) {
        return nullptr;
    }

    std::wstring currentAumid;
    {
        std::lock_guard lock(g_stateMutex);
        currentAumid = g_state.media.sourceAppUserModelId;
    }

    if (!currentAumid.empty()) {
        for (auto const& s : manager.GetSessions()) {
            if (s.SourceAppUserModelId().c_str() == currentAumid) {
                return s;
            }
        }
    }
    return manager.GetCurrentSession();
}

// Sends a transport command: 0 = previous, 1 = play/pause, 2 = next.
//
// SMTC is the only mechanism used when a session exists. The global media keys
// are deliberately NOT used as a fallback for a session that merely *reports*
// failure: the shell routes those keys to whatever it considers the current
// session, which is not necessarily the session the island is showing (see
// SelectActiveMediaSession), and a player that acts on the request while still
// answering false would then be driven twice -- skipping two tracks, or toggling
// play/pause straight back. The keys are only synthesized when SMTC could not
// act at all, where there is nothing to double-drive and no better option.
void SendMediaTransportCommand(int cmd) {
    std::thread([cmd]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        bool sessionAttempted = false;
        bool reportedSuccess = false;
        try {
            auto session = ResolveMediaSession();
            if (session) {
                sessionAttempted = true;
                if (cmd == 0) {
                    reportedSuccess = session.TrySkipPreviousAsync().get();
                } else if (cmd == 1) {
                    reportedSuccess = session.TryTogglePlayPauseAsync().get();
                } else if (cmd == 2) {
                    reportedSuccess = session.TrySkipNextAsync().get();
                }
            }
        } catch (...) {
            sessionAttempted = false;
        }

        if (sessionAttempted) {
            if (!reportedSuccess) {
                // Not escalated to a media key on purpose (see above). Logged so
                // a player that genuinely refuses transport control is
                // diagnosable from the mod log.
                DI_LOG(L"Media transport command %d was refused by the session.", cmd);
            }
            return;
        }

        BYTE vk = 0;
        if (cmd == 0) {
            vk = VK_MEDIA_PREV_TRACK;
        } else if (cmd == 1) {
            vk = VK_MEDIA_PLAY_PAUSE;
        } else if (cmd == 2) {
            vk = VK_MEDIA_NEXT_TRACK;
        }
        if (vk != 0) {
            keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
            keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        }
    }).detach();
}

struct WindowSearch {
    std::wstring targetTitle;
    std::wstring targetApp;
    HWND foundHwnd = nullptr;
    HWND fallbackHwnd = nullptr;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    auto* search = reinterpret_cast<WindowSearch*>(lParam);

    wchar_t title[512];
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) > 0) {
        std::wstring wTitle(title);
        // Case-insensitive check if window title contains currently playing media title
        auto it = std::search(
            wTitle.begin(), wTitle.end(),
            search->targetTitle.begin(), search->targetTitle.end(),
            [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); }
        );

        if (it != wTitle.end()) {
            search->foundHwnd = hwnd;
            return FALSE; // found exact title, stop enumerating
        }
    }

    // Fallback: check if the window belongs to the target app (by process executable name)
    if (!search->fallbackHwnd && !search->targetApp.empty()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0) {
            std::wstring exePath;
            if (ProcessImageNameForPid(pid, &exePath)) {
                std::wstring targetLower = ToLowerCopy(search->targetApp);
                std::wstring exeLower = ToLowerCopy(exePath);

                // Remove quotes from target AppUserModelId if any
                if (targetLower.size() >= 2 && targetLower.front() == L'"' && targetLower.back() == L'"') {
                    targetLower = targetLower.substr(1, targetLower.size() - 2);
                }

                if (exeLower == targetLower) {
                    search->fallbackHwnd = hwnd;
                } else {
                    std::wstring exeName = exeLower;
                    size_t slashPos = exeName.find_last_of(L"\\/");
                    if (slashPos != std::wstring::npos) {
                        exeName = exeName.substr(slashPos + 1);
                    }
                    if (exeName == targetLower || exeName == targetLower + L".exe") {
                        search->fallbackHwnd = hwnd;
                    }
                }
            }
        }
    }

    return TRUE;
}

void OpenRelevantApp() {
    std::wstring title;
    std::wstring app;
    {
        std::lock_guard lock(g_stateMutex);
        title = g_state.media.title;
        app = g_state.media.sourceAppUserModelId;
    }

    // Try to find and focus window containing track title (ideal for browser playing YouTube/Spotify)
    if (!title.empty() || !app.empty()) {
        WindowSearch search;
        search.targetTitle = title;
        search.targetApp = app;
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&search));

        HWND hwndToFocus = search.foundHwnd ? search.foundHwnd : search.fallbackHwnd;

        if (hwndToFocus) {
            if (IsIconic(hwndToFocus)) {
                ShowWindow(hwndToFocus, SW_RESTORE);
            }
            SetForegroundWindow(hwndToFocus);
            return;
        }
    }

    // Fallback: Launch or focus via AppUserModelId or Path
    if (!app.empty()) {
        std::wstring executePath = app;

        // Remove surrounding quotes if any
        if (executePath.size() >= 2 && executePath.front() == L'"' && executePath.back() == L'"') {
            executePath = executePath.substr(1, executePath.size() - 2);
        }

        bool isFilePath = (executePath.find(L":\\") != std::wstring::npos ||
                           (executePath.size() >= 4 && executePath.substr(executePath.size() - 4) == L".exe"));

        if (isFilePath) {
            if (GetFileAttributesW(executePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                // Path doesn't exist. Try 64-bit Program Files if it was in x86
                size_t x86Pos = executePath.find(L" (x86)");
                if (x86Pos != std::wstring::npos) {
                    std::wstring altPath = executePath;
                    altPath.erase(x86Pos, 6);
                    if (GetFileAttributesW(altPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        executePath = altPath;
                    }
                }

                // If it STILL doesn't exist after trying alternatives, just gracefully abort!
                // Trying to guess 'brave.exe' triggers broken Windows Registry App Paths.
                if (GetFileAttributesW(executePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    return;
                }
            }

            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpFile = executePath.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        } else {
            // It's a UWP/Desktop AppUserModelId, launch via AppsFolder
            std::wstring shellPath = L"shell:AppsFolder\\" + executePath;
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpFile = shellPath.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        return;
    }

    ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
}

void DismissTransientState() {
    std::lock_guard lock(g_stateMutex);
    g_state.notification.active = false;
    g_state.volume.active = false;
    g_state.brightness.active = false;
    g_state.progress.active = false;
    g_state.device.active = false;
    g_state.bluetoothDevice.active = false;
    g_state.battery.active = false;
    g_progressPercent.store(-1);
}

// ── Media source menu (the dock's "+N" list) ─────────────────────────────────
static constexpr UINT kMediaSourceMenuIdBase = 100;  // 100 = Auto, 101.. = sources

// Shape style submenu: +0 = iPhone pill, +1 = Windows 11 flyout, +2 = macOS notch.
static constexpr UINT kShapeMenuIdBase = 50;

struct MediaSourceMenuItem {
    std::wstring aumid;
    std::wstring name;
};

// Ticks: "Auto" = automatic mode, an app = the one currently being controlled.
std::vector<MediaSourceMenuItem> AppendMediaSourceItems(HMENU menu) {
    std::vector<MediaSourceMenuItem> items;
    std::wstring active;
    {
        std::lock_guard lock(g_stateMutex);
        active = g_state.media.sourceAppUserModelId;
        for (const MediaSourceInfo& source : g_state.mediaSources) {
            items.push_back(MediaSourceMenuItem{source.aumid, source.name});
        }
    }

    const bool autoMode = GetPreferredMediaSource().empty();
    AppendMenuW(menu, MF_STRING | (autoMode ? MF_CHECKED : 0), kMediaSourceMenuIdBase, Loc(L"Auto"));
    if (!items.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        AppendMenuW(menu, MF_STRING | (items[i].aumid == active ? MF_CHECKED : 0),
                    kMediaSourceMenuIdBase + 1 + static_cast<UINT>(i), items[i].name.c_str());
    }
    return items;
}

void ApplyMediaSourceMenuCommand(UINT cmd, const std::vector<MediaSourceMenuItem>& items) {
    if (cmd < kMediaSourceMenuIdBase) return;
    if (cmd == kMediaSourceMenuIdBase) {
        LockMediaSource(L"");
        return;
    }
    const size_t index = cmd - kMediaSourceMenuIdBase - 1;
    if (index < items.size()) {
        LockMediaSource(items[index].aumid);
    }
}

void ShowMediaSourcePopup(HWND hwnd, POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    const std::vector<MediaSourceMenuItem> items = AppendMediaSourceItems(menu);
    SetForegroundWindow(hwnd);
    DwmBackdrop::EnsureBehind(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DwmBackdrop::EnsureBehind(hwnd);
    g_layoutDirty = true;
    ApplyMediaSourceMenuCommand(cmd, items);
}


// ── Weather iconography ──────────────────────────────────────────────────────
// The dashboard used colour emoji (☀️ ⛅ 🌡️) rendered with ENABLE_COLOR_FONT,
// which clashed badly with the island's monochrome, accent-tinted UI and varied
// in size and baseline between glyphs. These are drawn as vectors instead: they
// inherit the accent colour, scale cleanly, sit on a predictable baseline, and
// can never fall back to a missing-glyph box the way a font-dependent icon can.
enum class WeatherVisual {
    Clear,
    PartlyCloudy,
    Cloudy,
    Fog,
    Storm,
    Rain,
    Snow,
    Unknown,
};

// Groupings mirror GetWeatherIconAndText so both stay keyed off the same WWO
// weather codes.
WeatherVisual WeatherVisualFromCode(int code) {
    switch (code) {
        case 113:
            return WeatherVisual::Clear;
        case 116:
            return WeatherVisual::PartlyCloudy;
        case 119: case 122:
            return WeatherVisual::Cloudy;
        case 143: case 248: case 260:
            return WeatherVisual::Fog;
        case 200: case 386: case 389: case 392: case 395:
            return WeatherVisual::Storm;
        case 176: case 263: case 266: case 281: case 284: case 293: case 296:
        case 299: case 302: case 305: case 308: case 311: case 314: case 353:
        case 356: case 359:
            return WeatherVisual::Rain;
        case 179: case 182: case 185: case 227: case 230: case 317: case 320:
        case 323: case 326: case 329: case 332: case 335: case 338: case 350:
        case 362: case 365: case 368: case 371:
            return WeatherVisual::Snow;
        default:
            return WeatherVisual::Unknown;
    }
}

struct MarqueeLayoutCache {
    std::wstring text;
    IDWriteTextFormat* format = nullptr;
    float wrapWidth = 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
};

// Measured geometry of the collapsed idle strip, produced by
// Renderer::MeasureIdleStrip. The render loop uses totalWidth to size the
// island; DrawIdleDashboard uses the per-slot widths to place the clock, divider
// and weather reading inside it.
// Standalone 0.6: one place that decides whether the mic/camera dot is shown on
// the collapsed island. Hidden while the music player is up (the user's choice).
inline bool PrivacyDotShown(const SharedState& state, const Settings& settings) {
    if (settings.media && state.media.available) {
        return false;
    }
    return settings.privacyDots && ((state.system.micActive && settings.privacyDotsMic) ||
                                    (state.system.cameraActive && settings.privacyDotsCam));
}

struct IdleStripMetrics {
    float clockWidth = 0.0f;
    float weatherWidth = 0.0f;
    bool hasWeather = false;
    bool hasPrivacy = false;
    float totalWidth = IdleStripLayout::kMinWidth;
};

struct MediaPillMetrics {
    float clockWidth = 0.0f;
    float sectionWidth = 0.0f;                       // clock + divider block on the left
    float totalWidth = MediaPillLayout::kBaseWidth;  // content space, before sizeScale
};

// Substitutes '0' for every decimal digit before measuring.
//
// Proportional faces give '1' a visibly narrower advance than '0', so measuring
// the live clock would change the pill's width as the time changed -- with
// seconds enabled that is a layered-window resize and reposition every second,
// and a visible twitch. Normalising to the widest digit makes the width stable
// for a given digit count, and since '0' is never narrower than the digit it
// replaces, the real string is guaranteed to fit the box we reserve.
std::wstring WidestDigitForm(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'0' && c <= L'9') {
            c = L'0';
        }
    }
    return text;
}

class Renderer {
   public:
    bool Initialize(HWND hwnd) {
        hwnd_ = hwnd;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory),
                                       reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

        hr = d2dFactory_->CreateDCRenderTarget(&props, &target_);
        if (FAILED(hr)) {
            return false;
        }

        const Settings initialSettings = GetSettingsCopy();
        EnsureTextFormats(initialSettings.sizeScale, initialSettings.fontFamily,
                          initialSettings.textScale);

        return CreateBackingBitmap(520, 140);
    }

    bool Render(const SharedState& state, const Settings& settings, const Activity& primary,
                const std::optional<Activity>& secondary, float width, float height,
                float nudge, bool hover, bool pinned, double now) {
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        width = std::max(width, 0.0f);
        if (UsesFixedStage(settings)) {
            // A pill can never be bigger than the stage it lives in.
            width = std::min(width, kStageContentWidth * settings.sizeScale);
            height = std::min(height, kStageContentHeight * settings.sizeScale);
        }

        const StageLayout stage = ComputeStageLayout(settings, width, height);
        const int pixelWidth = stage.width;
        const int pixelHeight = stage.height;

        const bool flushTop = settings.notchStyle || settings.borderMergedMode;
        const float top = stage.pillTop + (flushTop ? std::max(0.0f, nudge) : nudge);
        const float left = stage.pillLeft;

        const BYTE alpha = static_cast<BYTE>(Clamp(settings.pillOpacity, 0.35f, 1.0f) * 255.0f);
        const bool sizeChanged = pixelWidth != bitmapWidth_ || pixelHeight != bitmapHeight_;
        const bool moved = stage.origin.x != lastOrigin_.x || stage.origin.y != lastOrigin_.y;

        // Full repaint when anything about the window itself changed, plus once a
        // second as a safety net (sleep/resume, display changes).
        const bool fullPaint = !kUseDirtyRect || !stageValid_ || sizeChanged || moved ||
                               alpha != lastAlpha_ || (now - lastFullPaint_) > 1.0;

        if (sizeChanged) {
            if (!CreateBackingBitmap(pixelWidth, pixelHeight)) {
                return false;
            }
        } else if (g_layoutDirty.exchange(false)) {
            upkeepPending_ = true;
        }
        if (upkeepPending_) {
            if (now - lastUpkeepAt_ > 0.25) {
                upkeepPending_ = false;
                lastUpkeepAt_ = now;
                ApplyOverlayZOrder(hwnd_, pixelWidth, pixelHeight);
            } else {
                g_layoutDirty = true;  // retry once the throttle expires
            }
        }

        // Region that can contain pixels this frame: the pill plus its shadow and
        // nudge slack, unioned with where it was last frame so the old pixels get erased.
        RECT bounds;
        bounds.left = std::max(0, static_cast<int>(std::floor(left - kRenderPadX - 4.0f)));
        bounds.top = std::max(0, static_cast<int>(std::floor(top - kRenderPadY - 12.0f)));
        bounds.right = std::min(pixelWidth, static_cast<int>(std::ceil(left + width + kRenderPadX + 4.0f)));
        bounds.bottom = std::min(pixelHeight, static_cast<int>(std::ceil(top + height + kRenderPadY + 12.0f)));

        RECT dirty = {0, 0, pixelWidth, pixelHeight};
        if (!fullPaint) {
            UnionRect(&dirty, &lastBounds_, &bounds);
        }

        if (IsRectEmpty(&dirty)) {
            dirty = RECT{0, 0, pixelWidth, pixelHeight};
        }

        // Bind only the region being repainted. Binding the whole stage made D2D copy
        // all of it in at BeginDraw and out at EndDraw every frame, even for a small pill.
        // The bound rect's top-left becomes (0,0); the translation restores stage
        // coordinates, and every draw routine composes with the current transform.
        RECT rc = dirty;
        HRESULT hr = target_->BindDC(memDc_, &rc);
        if (FAILED(hr)) {
            return false;
        }

        pillCount_ = 0;  // DrawPill records each pill it draws for the backdrop windows

        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(dirty.left),
                                                            -static_cast<float>(dirty.top)));
        // Clear() honours the active clip, so this only wipes the dirty region.
        target_->PushAxisAlignedClip(
            D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                        static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        target_->Clear(D2D1::ColorF(0, 0.0f));
        BeginFrameResources();

        EnsureBrushes(settings, state, now);
        settingsOpacity_ = settings.pillOpacity;

        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            settings.gameOverlay;
        // True while the island is collapsing to nothing (auto-hide / fullscreen).
        islandHiding_ = primary.width < 1.0f && primary.height < 1.0f && !secondary;

        const float hoverScale = (settings.expandOnHover && ((hover && !gameMetricsPresent) || pinned)) ? 1.025f : 1.0f;
        const float scale = hoverScale;
        hoverScale_ = scale;


        if (width >= 2.0f && height >= 2.0f) {
            if (secondary) {
                const float gap = 12.0f * settings.sizeScale;
                const float maxH = std::max(primary.height, secondary->height);
                // Notch / border-merged islands hang from the top edge, so the
                // shorter pill stays flush with it instead of being centred
                // inside the taller one's height (which pushed it down).
                const bool hangFromTop = settings.notchStyle || settings.borderMergedMode;
                const float pTop = hangFromTop ? top : top + (maxH - primary.height) * 0.5f;
                const float sTop = hangFromTop ? top : top + (maxH - secondary->height) * 0.5f;

                DrawPill(state, settings, primary,
                         D2D1::RectF(left, pTop, left + primary.width, pTop + primary.height),
                         scale, now);
                DrawPill(state, settings, *secondary,
                         D2D1::RectF(left + primary.width + gap, sTop,
                                      left + primary.width + gap + secondary->width,
                                      sTop + secondary->height),
                         scale, now);
            } else if (quietSqueeze_ && quietSqueezeW_ > 1.0f && quietSqueezeH_ > 1.0f) {
                // Custom fork, Feature 3: the frame is drawn at the size it had
                // when the shrink began and squeezed onto the shrinking pill, so
                // the content shrinks together with it.
                const float sx = Clamp(width / quietSqueezeW_, 0.0f, 1.0f);
                const float sy = Clamp(height / quietSqueezeH_, 0.0f, 1.0f);
                D2D1_MATRIX_3X2_F oldTransform;
                target_->GetTransform(&oldTransform);
                target_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy, D2D1::Point2F(left, top)) * oldTransform);
                DrawPill(state, settings, primary,
                         D2D1::RectF(left, top, left + quietSqueezeW_, top + quietSqueezeH_), scale, now);
                target_->SetTransform(oldTransform);
            } else {
                DrawPill(state, settings, primary,
                         D2D1::RectF(left, top, left + width, top + height), scale, now);
            }
        }

        target_->PopAxisAlignedClip();
        hr = target_->EndDraw();
        if (FAILED(hr)) {
            stageValid_ = false;
            return false;
        }

        POINT src = {0, 0};
        SIZE size = {bitmapWidth_, bitmapHeight_};
        POINT dst = stage.origin;  // constant while the pill animates

        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UPDATELAYEREDWINDOWINFO info = {};
        info.cbSize = sizeof(info);
        info.pptDst = &dst;
        info.psize = &size;
        info.hdcSrc = memDc_;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = fullPaint ? nullptr : &dirty;  // only this rect is re-composited

        const bool presented = UpdateLayeredWindowIndirect(hwnd_, &info) != FALSE;
        if (presented) {
            stageValid_ = true;
            lastOrigin_ = stage.origin;
            lastBounds_ = bounds;
            lastAlpha_ = alpha;
            if (fullPaint) lastFullPaint_ = now;
            g_pillClientRect = PillClientRect{left, top, left + width, top + height, true};
        } else {
            stageValid_ = false;
        }

        if (presented) {
            DwmBackdrop::Sync(hwnd_, stage.origin, pillRects_, pillRadii_, pillCount_,
                              settings.notchStyle, settings.backdropMaterial, pillBgColor_,
                              settings.backdropTint);
        }
        return presented;
    }

    void Shutdown() {
        ReleaseCachedResources();
        marqueeTitleCache_.layout.Reset();
        marqueeArtistCache_.layout.Reset();
        marqueeAlbumCache_.layout.Reset();
        marqueeNotificationCache_.layout.Reset();
        scratchColorBrush_.Reset();

        artBitmap_.Reset();
        mediaSourceIconBitmap_.Reset();
        accentBrush_.Reset();
        redBrush_.Reset();
        textBrush_.Reset();
        mutedBrush_.Reset();
        tintBrush_.Reset();
        shadowBrush_.Reset();
        micDotBrush_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        weatherDescFormat_.Reset();
        micGlowBrush_.Reset();
        camDotBrush_.Reset();
        camGlowBrush_.Reset();
        target_.Reset();
        textFormat_.Reset();
        smallTextFormat_.Reset();
        boldTextFormat_.Reset();
        hugeTextFormat_.Reset();
        clockFormat_.Reset();
        iconFormat_.Reset();
        mediaPlayIconFormat_.Reset();
        mediaNavIconFormat_.Reset();
        idleTextFormat_.Reset();
        calDayLargeFormat_.Reset();
        calGridFormat_.Reset();
        dwriteFactory_.Reset();
        skipTriFull_.Reset();
        skipTriNotch_.Reset();
        roundJoinStyle_.Reset();
        d2dFactory_.Reset();

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (memDc_) {
            DeleteDC(memDc_);
            memDc_ = nullptr;
        }
    }

    // Measures the collapsed idle strip so the render loop can size the island to
    // the text it is actually about to paint, instead of the old fixed 96/170px.
    // DrawIdleDashboard calls this too and lays the slots out from the same
    // numbers, so the pill can never be sized for a clock width the painter is
    // not using.
    IdleStripMetrics MeasureIdleStrip(const SharedState& state, const Settings& settings,
                                      double now) {
        // Idempotent and cheap when nothing changed, but necessary here: textScale
        // drives the idle font size, so measuring before the formats are rebuilt
        // would size the pill for the previous Text size setting.
        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);

        IdleStripMetrics metrics;
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring clock = FormatIslandTime(local, settings.clockFollowSystem,
                                                    settings.use24HourClock, settings.showSeconds);
        metrics.clockWidth =
            MeasureTextWidthCached(WidestDigitForm(clock), fmt, idleClockWidthCache_);

        metrics.hasWeather = settings.weather;
        if (metrics.hasWeather) {
            // Mirrors the label DrawIdleDashboard builds, including the no-data
            // placeholder, so the reserved slot matches what gets drawn.
            const bool hasData = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
            wchar_t label[32] = {};
            if (hasData) {
                std::wstring icon = L"\U0001F321\uFE0F";
                std::wstring desc = state.weather.weatherDesc;
                GetWeatherIconAndText(state.weather.weatherCode, icon, desc);
                swprintf_s(label, L"%s %.0f\x00B0", icon.c_str(), state.weather.temperature);
            } else {
                wcscpy_s(label, ARRAYSIZE(label), L"\U0001F321\uFE0F --\x00B0");
            }
            metrics.weatherWidth =
                MeasureTextWidthCached(WidestDigitForm(label), fmt, idleWeatherWidthCache_);
        }

        metrics.hasPrivacy = PrivacyDotShown(state, settings);

        float total = IdleStripLayout::kPadX * 2.0f + metrics.clockWidth;
        if (metrics.hasWeather) {
            total += IdleStripLayout::kSlotGap * 2.0f + IdleStripLayout::kDividerWidth +
                     metrics.weatherWidth;
        }
        if (metrics.hasPrivacy) {
            total += IdleStripLayout::kPrivacyReserve * 2.0f;  // both sides: stays centred
        }

        total = std::ceil(total / IdleStripLayout::kWidthQuantum) * IdleStripLayout::kWidthQuantum;
        metrics.totalWidth = Clamp(total, IdleStripLayout::kMinWidth, IdleStripLayout::kMaxWidth);
        return metrics;
    }

    // Collapsed media pill, with the optional clock. Returns content-space
    // sizes; the render loop multiplies by sizeScale like every other collapsed
    // width. Measured in the widest-digit form so the width does not wobble as
    // the digits change.
    MediaPillMetrics MeasureMediaPill(const SharedState& state, const Settings& settings) {
        (void)state;
        MediaPillMetrics metrics;
        if (!settings.mediaPillClock) {
            return metrics;
        }

        EnsureTextFormats(settings.sizeScale, settings.fontFamily, settings.textScale);
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring clock = FormatIslandTime(local, settings.clockFollowSystem,
                                                    settings.use24HourClock, settings.showSeconds);
        metrics.clockWidth =
            MeasureTextWidthCached(WidestDigitForm(clock), fmt, idleClockWidthCache_);

        float section = MediaPillLayout::kClockPadLeft + metrics.clockWidth +
                        MediaPillLayout::kSlotGap + MediaPillLayout::kDividerWidth;
        section = std::ceil(section / MediaPillLayout::kWidthQuantum) * MediaPillLayout::kWidthQuantum;
        metrics.sectionWidth = section;
        metrics.totalWidth = MediaPillLayout::kBaseWidth + section;
        return metrics;
    }

   private:
    // Keyed on the string alone. EnsureTextFormats invalidates both caches
    // whenever it rebuilds the formats, so a cached width can never outlive the
    // font size it was measured at -- comparing the IDWriteTextFormat pointer
    // would not be enough, since a rebuilt format can land on the freed address.
    struct TextWidthCache {
        std::wstring key;
        float width = 0.0f;
        bool valid = false;
    };
    TextWidthCache idleClockWidthCache_;
    TextWidthCache idleWeatherWidthCache_;
    TextWidthCache btPctWidthCache_;  // standalone 0.7: Bluetooth badge

    float MeasureTextWidthCached(const std::wstring& text, IDWriteTextFormat* fmt,
                                 TextWidthCache& cache) {
        if (cache.valid && cache.key == text) {
            return cache.width;
        }

        float width = 0.0f;
        if (fmt && dwriteFactory_ && !text.empty()) {
            ComPtr<IDWriteTextLayout> layout;
            // Effectively unbounded wrap width: the idle formats are NO_WRAP, and
            // we want the natural advance, not a wrapped block.
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()), fmt,
                    4096.0f, IdleStripLayout::kHeight, &layout)) &&
                layout) {
                DWRITE_TEXT_METRICS tm = {};
                if (SUCCEEDED(layout->GetMetrics(&tm))) {
                    width = std::max(tm.width, tm.widthIncludingTrailingWhitespace);
                }
            }
        }

        cache.key = text;
        cache.width = width;
        cache.valid = true;
        return width;
    }

    bool CreateBackingBitmap(int width, int height) {
        const int logicalW = width;
        const int logicalH = height;
        // Expand/collapse changes the needed size almost every frame. Reuse the
        // existing DIB whenever it is big enough; UpdateLayeredWindow only reads
        // the (0,0,width,height) region, so a larger DIB is fine.
        if (dib_ && width <= bitmapCapW_ && height <= bitmapCapH_) {
            bitmapWidth_ = width;
            bitmapHeight_ = height;
            return true;
        }

        // Round up with headroom so growing animations rarely reallocate.
        width = ((width + 63) / 64) * 64 + 64;
        height = ((height + 31) / 32) * 32 + 32;

        if (oldBitmap_) {
            SelectObject(memDc_, oldBitmap_);
            oldBitmap_ = nullptr;
        }
        if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
        if (!memDc_) {
            HDC screen = GetDC(nullptr);
            memDc_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!memDc_) {
                return false;
            }
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        dib_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib_) {
            return false;
        }

        oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dib_));
        bitmapCapW_ = width;
        bitmapCapH_ = height;
        bitmapWidth_ = logicalW;
        bitmapHeight_ = logicalH;
        return true;
    }

    float lastFontScale_ = 0.0f;
    float lastTextScale_ = 0.0f;
    std::wstring lastFontFamily_;
    // textScale is an independent typography multiplier (the Text size setting):
    // sizeScale magnifies the whole island via a transform, whereas this changes
    // only the type, so the island can stay compact while the clock and labels
    // get bigger.
    void EnsureTextFormats(float scale, const std::wstring& fontFamily, float textScale) {
        if (std::abs(scale - lastFontScale_) < 0.001f && fontFamily == lastFontFamily_ &&
            std::abs(textScale - lastTextScale_) < 0.001f) {
            return;
        }
        lastTextScale_ = textScale;
        const float ts = Clamp(textScale, 0.7f, 1.6f);

        // Every format below is about to be recreated at a new size, so any
        // width measured against the old ones is stale.
        idleClockWidthCache_.valid = false;
        idleWeatherWidthCache_.valid = false;

        textFormat_ = nullptr;
        smallTextFormat_ = nullptr;
        clockFormat_ = nullptr;
        boldTextFormat_ = nullptr;
        hugeTextFormat_ = nullptr;
        iconFormat_ = nullptr;
        mediaPlayIconFormat_ = nullptr;
        mediaNavIconFormat_ = nullptr;
        idleTextFormat_ = nullptr;
        calDayLargeFormat_ = nullptr;
        calGridFormat_ = nullptr;
        timeDashboardFormat_ = nullptr;
        dateDashboardFormat_ = nullptr;

        const wchar_t* defaultDisplay = L"Segoe UI Variable Display";
        const wchar_t* defaultSmall = L"Segoe UI Variable Small";
        const wchar_t* mainFamily = fontFamily.empty() ? defaultDisplay : fontFamily.c_str();
        const wchar_t* smallFamily = fontFamily.empty() ? defaultSmall : fontFamily.c_str();

        auto createFormat = [&](const wchar_t* family, const wchar_t* fallback,
                                DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                                DWRITE_FONT_STRETCH stretch, float size,
                                ComPtr<IDWriteTextFormat>& out) {
            HRESULT hr = dwriteFactory_->CreateTextFormat(
                family, nullptr, weight, style, stretch, size, L"", &out);
            if (FAILED(hr) || !out) {
                if (fallback && wcscmp(family, fallback) != 0) {
                    hr = dwriteFactory_->CreateTextFormat(
                        fallback, nullptr, weight, style, stretch, size, L"", &out);
                }
                if (FAILED(hr) || !out) {
                    dwriteFactory_->CreateTextFormat(
                        L"Segoe UI", nullptr, weight, style, stretch, size, L"", &out);
                }
            }
        };

        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.5f * ts, textFormat_);
        createFormat(smallFamily, defaultSmall,
                     DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 11.0f * ts, smallTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 18.0f * ts, clockFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 12.0f * ts, boldTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 42.0f * ts, hugeTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 13.0f * ts, idleTextFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 63.0f * ts, calDayLargeFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 14.4f * ts, calGridFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 56.0f * ts, timeDashboardFormat_);
        createFormat(mainFamily, defaultDisplay,
                     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                     DWRITE_FONT_STRETCH_NORMAL, 16.0f * ts, dateDashboardFormat_);

        usingFluentIcons_ = false;
        ComPtr<IDWriteFontCollection> sysFonts;
        if (dwriteFactory_ && SUCCEEDED(dwriteFactory_->GetSystemFontCollection(&sysFonts, FALSE)) && sysFonts) {
            UINT32 fontIdx = 0;
            BOOL fontFound = FALSE;
            if (SUCCEEDED(sysFonts->FindFamilyName(L"Segoe Fluent Icons", &fontIdx, &fontFound)) && fontFound) {
                usingFluentIcons_ = true;
            }
        }

        const wchar_t* iconFontFamily = usingFluentIcons_ ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

        HRESULT hrIcon = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        if (FAILED(hrIcon) || !iconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"", &iconFormat_);
        }

        HRESULT hrPlay = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        if (FAILED(hrPlay) || !mediaPlayIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &mediaPlayIconFormat_);
        }

        HRESULT hrNav = dwriteFactory_->CreateTextFormat(
            iconFontFamily, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        if (FAILED(hrNav) || !mediaNavIconFormat_) {
            dwriteFactory_->CreateTextFormat(
                L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &mediaNavIconFormat_);
        }

        if (textFormat_) {
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (smallTextFormat_) {
            smallTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (boldTextFormat_) {
            boldTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            // Match every other centered format: without this the text hugs the
            // top of its layout rect, which left the weather dashboard's city /
            // icon / temperature visually "stuck" near the top of the island.
            boldTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (hugeTextFormat_) {
            hugeTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            hugeTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (clockFormat_) {
            clockFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            clockFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            clockFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (idleTextFormat_) {
            idleTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            idleTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            idleTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calDayLargeFormat_) {
            calDayLargeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calDayLargeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calDayLargeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (calGridFormat_) {
            calGridFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            calGridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            calGridFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (timeDashboardFormat_) {
            timeDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            timeDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            timeDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (dateDashboardFormat_) {
            dateDashboardFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            dateDashboardFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dateDashboardFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (iconFormat_) {
            iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (mediaPlayIconFormat_) {
            mediaPlayIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaPlayIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaPlayIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (mediaNavIconFormat_) {
            mediaNavIconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            mediaNavIconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            mediaNavIconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (fontFamily != lastFontFamily_) {
            marqueeTitleCache_.layout.Reset();
            marqueeArtistCache_.layout.Reset();
            marqueeAlbumCache_.layout.Reset();
                marqueeNotificationCache_.layout.Reset();
            weatherDescFormat_.Reset();
            weatherDescFormatSize_ = -1.0f;
        }

        lastFontScale_ = scale;
        lastFontFamily_ = fontFamily;
    }

    void EnsureBrushes(const Settings& settings, const SharedState& state, double now) {
        D2D1_COLOR_F targetAccent = settings.customAccent;
        if (settings.accentMode == AccentMode::System) {
            targetAccent = GetSystemAccentColor();
        } else if (settings.accentMode == AccentMode::Auto && state.media.art) {
            // Already contrast-corrected when the art was decoded.
            targetAccent = state.media.art->sampledAccent;
        } else {
            // A custom or system accent was never checked against the island's
            // own background. That did not matter while every theme was dark,
            // but it does now that one is light: the default cyan sits at about
            // 1.6:1 on Porcelain. Corrected before the smoothing lerp so the
            // value is stable rather than re-derived every frame.
            targetAccent = EnsureContrastAgainstBackground(targetAccent, settings.pillBgColor);
        }

        // Smooth accent transition: exponential lerp toward target, ~300ms half-life.
        // On the very first frame (lastAccentTime_ < 0) snap immediately so there's
        // no fade-in from the default color on startup.
        if (lastAccentTime_ < 0.0) {
            currentAccent_ = targetAccent;
            lastAccentTime_ = now;
        } else {
            const double dt = std::max(0.0, std::min(now - lastAccentTime_, 0.1));  // cap at 100ms
            lastAccentTime_ = now;
            // k = 1 - exp(-dt / tau), tau ≈ 0.18s → reaches 95% in ~350ms
            const float k = 1.0f - static_cast<float>(std::exp(-dt / 0.18));
            currentAccent_.r += (targetAccent.r - currentAccent_.r) * k;
            currentAccent_.g += (targetAccent.g - currentAccent_.g) * k;
            currentAccent_.b += (targetAccent.b - currentAccent_.b) * k;
            currentAccent_.a = 1.0f;
        }

        const D2D1_COLOR_F accent = currentAccent_;
        // Standalone 0.4: the Launcher tints its own UI with the island's live accent.
        g_liveAccentRgb.store(0x01000000u |
                              (static_cast<uint32_t>(Clamp(accent.r, 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
                              (static_cast<uint32_t>(Clamp(accent.g, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
                              static_cast<uint32_t>(Clamp(accent.b, 0.0f, 1.0f) * 255.0f + 0.5f));

        if (!accentBrush_) target_->CreateSolidColorBrush(accent, &accentBrush_);
        else accentBrush_->SetColor(accent);

        if (!redBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.27f, 0.27f, 1.0f), &redBrush_);

        // Use user-configured text colors.
        D2D1_COLOR_F primary = settings.textPrimaryColor;
        primary.a = 0.98f;
        if (!textBrush_) target_->CreateSolidColorBrush(primary, &textBrush_);
        else textBrush_->SetColor(primary);

        D2D1_COLOR_F secondary = settings.textSecondaryColor;
        secondary.a = 0.90f;
        if (!mutedBrush_) target_->CreateSolidColorBrush(secondary, &mutedBrush_);
        else mutedBrush_->SetColor(secondary);

        // Keep the authored alpha. An 8-digit #RRGGBBAA background is how a
        // translucent island is requested independently of the global pill
        // transparency slider; DrawPillSurface combines the two.
        pillBgColor_ = settings.pillBgColor;

        D2D1_COLOR_F tintColor = D2D1::ColorF(0.010f, 0.010f, 0.012f, settings.tintOpacity);
        if (!tintBrush_) target_->CreateSolidColorBrush(tintColor, &tintBrush_);
        else tintBrush_->SetColor(tintColor);

        if (!shadowBrush_) target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.70f), &shadowBrush_);

        BuildMaterialTokens(settings);
    }

    // ── Premium material system ─────────────────────────────────────────────
    // Every surface in the island is built from one small token set so the whole
    // UI shares a single visual language. Tokens are recomputed once per frame
    // from the resolved theme, and adapt to background luminance so a light
    // custom background gets dark separators instead of washed-out white ones.
    struct MaterialTokens {
        D2D1_COLOR_F base{};          // the pill's own fill
        D2D1_COLOR_F raised{};        // inner cards, chips, wells
        D2D1_COLOR_F raisedStrong{};  // pressed / active chips
        D2D1_COLOR_F hairline{};      // 1px separators between content
        D2D1_COLOR_F stroke{};        // outer contour
        D2D1_COLOR_F shadow{};        // drop shadow tint
        D2D1_COLOR_F textPrimary{};
        D2D1_COLOR_F textSecondary{};
        D2D1_COLOR_F textTertiary{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accentSoft{};
        bool onDark = true;
        float opacity = 1.0f;
    };

    static D2D1_COLOR_F MixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
        return D2D1::ColorF(a.r + (b.r - a.r) * t,
                            a.g + (b.g - a.g) * t,
                            a.b + (b.b - a.b) * t,
                            a.a + (b.a - a.a) * t);
    }

    static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float alpha) {
        c.a = Clamp(alpha, 0.0f, 1.0f);
        return c;
    }

    // ── Cached Direct2D resources ───────────────────────────────────────────
    // Nothing below is created per frame once it has warmed up. Render thread only.
    struct LinearGradientSlot { ComPtr<ID2D1LinearGradientBrush> brush; uint64_t key = 0; };
    struct RadialGradientSlot { ComPtr<ID2D1RadialGradientBrush> brush; uint64_t key = 0; };

    struct ShapeKey {
        float l = 0, t = 0, r = 0, b = 0, radius = 0;
        bool notch = false;
        bool operator==(const ShapeKey& o) const {
            return l == o.l && t == o.t && r == o.r && b == o.b &&
                   radius == o.radius && notch == o.notch;
        }
    };
    struct ShapeEntry {
        ShapeKey key;
        ComPtr<ID2D1Geometry> geom;
        uint64_t lastUsed = 0;
    };
    static constexpr size_t kShapeCacheSize = 32;

    static uint32_t PackColor8(const D2D1_COLOR_F& c) {
        auto q = [](float v) { return static_cast<uint32_t>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
    }

    static uint64_t HashStops(const D2D1_GRADIENT_STOP* s, UINT32 n) {
        uint64_t h = 1469598103934665603ull;
        for (UINT32 i = 0; i < n; ++i) {
            h = (h ^ PackColor8(s[i].color)) * 1099511628211ull;
            h = (h ^ static_cast<uint64_t>(s[i].position * 1000.0f + 0.5f)) * 1099511628211ull;
        }
        return h;
    }

    // Call once per frame, right after BeginDraw().
    void BeginFrameResources() {
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
    }

    // Returns a brush that is unique for the rest of this frame. Colour and opacity are
    // reset, so a previous user's SetOpacity() can never leak into the next one.
    ID2D1SolidColorBrush* AcquireBrush(const D2D1_COLOR_F& color) {
        if (brushPoolUsed_ >= brushPool_.size()) {
            ComPtr<ID2D1SolidColorBrush> b;
            if (FAILED(target_->CreateSolidColorBrush(color, &b)) || !b) {
                return nullptr;
            }
            brushPool_.push_back(std::move(b));
        }
        ID2D1SolidColorBrush* brush = brushPool_[brushPoolUsed_++].Get();
        brush->SetColor(color);
        brush->SetOpacity(1.0f);
        return brush;
    }

    // Same idea for layers. Each Acquire is distinct within a frame, so nested
    // PushLayer calls are safe, and the layer's backing store is kept between frames.
    ID2D1Layer* AcquireLayer() {
        if (layerPoolUsed_ >= layerPool_.size()) {
            ComPtr<ID2D1Layer> l;
            if (FAILED(target_->CreateLayer(&l)) || !l) {
                return nullptr;
            }
            layerPool_.push_back(std::move(l));
        }
        return layerPool_[layerPoolUsed_++].Get();
    }

    void SetPersistentBrush(ComPtr<ID2D1SolidColorBrush>& brush, const D2D1_COLOR_F& c) {
        if (!brush) {
            target_->CreateSolidColorBrush(c, &brush);
        } else {
            brush->SetColor(c);
        }
    }

    // The collection + brush are rebuilt only when the stops change; start/end move per call.
    ID2D1LinearGradientBrush* UseLinearGradient(LinearGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F start, D2D1_POINT_2F end) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(start, end), coll.Get(), &slot.brush)) ||
                !slot.brush) {
                slot.brush.Reset();
                return nullptr;
            }
            slot.key = key;
        }
        slot.brush->SetStartPoint(start);
        slot.brush->SetEndPoint(end);
        return slot.brush.Get();
    }

    ID2D1RadialGradientBrush* UseRadialGradient(RadialGradientSlot& slot,
                                                const D2D1_GRADIENT_STOP* stops, UINT32 count,
                                                D2D1_POINT_2F center, float rx, float ry) {
        const uint64_t key = HashStops(stops, count);
        if (!slot.brush || slot.key != key) {
            slot.brush.Reset();
            ComPtr<ID2D1GradientStopCollection> coll;
            if (FAILED(target_->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                             D2D1_EXTEND_MODE_CLAMP, &coll)) || !coll) {
                return nullptr;
            }
            if (FAILED(target_->CreateRadialGradientBrush(
                    D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry),
                    coll.Get(), &slot.brush)) ||
                !slot.brush) {
                slot.brush.Reset();
                return nullptr;
            }
            slot.key = key;
        }
        slot.brush->SetCenter(center);
        slot.brush->SetGradientOriginOffset(D2D1::Point2F(0, 0));
        slot.brush->SetRadiusX(rx);
        slot.brush->SetRadiusY(ry);
        return slot.brush.Get();
    }

    // Rounded-rect or notch geometry, cached by rect + radius. Returns a ref-counted
    // pointer, so a caller holding it is safe even if the entry is evicted meanwhile.
    ComPtr<ID2D1Geometry> GetShapeGeometry(D2D1_RECT_F rect, float radius, bool notch) {
        const ShapeKey key{rect.left, rect.top, rect.right, rect.bottom, radius, notch};
        ShapeEntry* victim = &shapeCache_[0];
        for (ShapeEntry& e : shapeCache_) {
            if (e.geom && e.key == key) {
                e.lastUsed = ++shapeClock_;
                return e.geom;
            }
            if (e.lastUsed < victim->lastUsed) {
                victim = &e;  // empty entries have lastUsed == 0, so they go first
            }
        }

        ComPtr<ID2D1Geometry> geom;
        if (notch) {
            ComPtr<ID2D1PathGeometry> path = CreateNotchGeometry(rect, radius);
            if (path) path.As(&geom);
        }
        if (!geom) {  // not a notch, or the notch failed: same fallback as before
            ComPtr<ID2D1RoundedRectangleGeometry> rr;
            if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(rect, radius, radius), &rr)) && rr) {
                rr.As(&geom);
            }
        }
        if (!geom) return nullptr;

        victim->key = key;
        victim->geom = geom;
        victim->lastUsed = ++shapeClock_;
        return geom;
    }

    void ReleaseCachedResources() {
        brushPool_.clear();
        layerPool_.clear();
        brushPoolUsed_ = 0;
        layerPoolUsed_ = 0;
        raisedBrush_.Reset();
        raisedStrongBrush_.Reset();
        depthGrad_ = LinearGradientSlot{};
        trackGrad_ = LinearGradientSlot{};
        bloomGrad_ = RadialGradientSlot{};
        glassSheenGrad_ = LinearGradientSlot{};
        clockGlowGrad_ = RadialGradientSlot{};
        for (LinearGradientSlot& g : sparkGrad_) g = LinearGradientSlot{};
        glassRimGrad_ = LinearGradientSlot{};
        for (ShapeEntry& e : shapeCache_) e = ShapeEntry{};
    }

    void BuildMaterialTokens(const Settings& settings) {
        MaterialTokens t;
        t.opacity = settingsOpacity_;
        t.base = pillBgColor_;

        // Light surfaces need dark scrims/edges, dark surfaces need light ones.
        const bool onDark = RelativeLuminance(t.base) < 0.45;
        t.onDark = onDark;

        const D2D1_COLOR_F white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        const D2D1_COLOR_F black = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
        const D2D1_COLOR_F lift = onDark ? white : black;

        // Cards carry a slightly stronger fill than before. They used to be
        // outlined with a hairline ring, and losing that ring is what the fill
        // now has to compensate for so a card still reads as a distinct surface.
        t.raised = WithAlpha(lift, onDark ? 0.085f : 0.062f);
        t.raisedStrong = WithAlpha(lift, onDark ? 0.150f : 0.105f);
        t.hairline = WithAlpha(lift, onDark ? 0.100f : 0.085f);
        t.shadow = WithAlpha(black, 0.55f);

        t.stroke = settings.contourBorderColor;
        t.accent = currentAccent_;
        t.accentSoft = WithAlpha(currentAccent_, 0.18f);

        t.textPrimary = WithAlpha(settings.textPrimaryColor, 0.98f);
        t.textSecondary = WithAlpha(settings.textSecondaryColor, 0.88f);
        // Derived rather than configured, so a third tier always reads as
        // quieter than "secondary" whatever the user picked.
        t.textTertiary = WithAlpha(MixColor(settings.textSecondaryColor, t.base, 0.35f), 0.72f);

        material_ = t;
        SetPersistentBrush(raisedBrush_, t.raised);
        SetPersistentBrush(raisedStrongBrush_, t.raisedStrong);
    }

    void DrawPill(const SharedState& state, const Settings& settings, const Activity& activity,
                  D2D1_RECT_F rect, float scale, double now) {
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float w = (rect.right - rect.left) * scale;
        const float h = (rect.bottom - rect.top) * scale;
        rect = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);

        float radius = settings.w11Style ? 8.0f * settings.sizeScale : (rect.bottom - rect.top) * 0.5f;
        if (settings.notchStyle) {
            radius = 16.0f * settings.sizeScale;
        } else if (!settings.w11Style) {
            radius = std::min(radius, 44.0f * settings.sizeScale);
        }
        if (pillCount_ < DwmBackdrop::kMaxPills) {
            pillRects_[pillCount_] = rect;
            pillRadii_[pillCount_] = radius;
            ++pillCount_;
        }
        DrawSoftShadow(rect, radius);

        DrawPillSurface(rect, radius, activity.kind, settings);

        if (activity.kind == IslandKind::Progress) {
            DrawProgressRing(rect, state.progress.percent);
        }

        if (activity.kind == IslandKind::BatteryLow) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 * 2.1));
            redBrush_->SetOpacity(0.45f + 0.45f * pulse);
            DrawIslandShape(rect, radius, settings.w11Style, settings.notchStyle, redBrush_.Get(), 2.0f);
            redBrush_->SetOpacity(1.0f);
        }

        // No inner highlight ring here. A second bright hairline just inside the
        // contour read as a light rim tracing the whole island, which is exactly
        // the edge glow this design drops. The contour stroke alone defines the
        // silhouette now.

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        D2D1_POINT_2F pillCenter = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        target_->SetTransform(D2D1::Matrix3x2F::Scale(settings.sizeScale, settings.sizeScale, pillCenter) * oldTransform);

        float invScale = 1.0f / settings.sizeScale;
        float unW = (rect.right - rect.left) * invScale;
        float unH = (rect.bottom - rect.top) * invScale;
        D2D1_RECT_F unscaledRect = D2D1::RectF(pillCenter.x - unW * 0.5f, pillCenter.y - unH * 0.5f, pillCenter.x + unW * 0.5f, pillCenter.y + unH * 0.5f);

        switch (activity.kind) {
            case IslandKind::Media:
                DrawMedia(state, unscaledRect, settings, now);
                break;
            case IslandKind::Notification:
                DrawTelegramNotification(state, unscaledRect);
                break;
            case IslandKind::Volume:
                DrawVolume(state, unscaledRect);
                break;
            case IslandKind::Brightness:
                DrawBrightness(state, unscaledRect);
                break;
            case IslandKind::Device:
                DrawDevice(state, unscaledRect);
                break;
            case IslandKind::Bluetooth:
                DrawBluetoothDevice(state, unscaledRect);
                break;
            case IslandKind::Timer:
                DrawTimer(state, unscaledRect);
                break;
            case IslandKind::BatteryLow:
                DrawBattery(state, unscaledRect);
                break;
            case IslandKind::Progress:
                DrawProgress(state, unscaledRect);
                break;
            case IslandKind::Idle:
            default:
                DrawIdleDashboard(state, unscaledRect, settings, now);
                break;
        }

        // ── Apple-style privacy indicator dot ────────────────────────────────
        // Standalone 0.6: on the collapsed clock pill only (it reserves a lane
        // for it on both sides, so the island stays centred); hidden while the
        // music player is up. Orange = mic, green = camera, purple = both.
        if (activity.kind == IslandKind::Idle && PrivacyDotShown(state, settings)) {
            DrawPrivacyDots(state, settings, unscaledRect, now);
        }

        target_->SetTransform(oldTransform);
    }

    // A real soft shadow, approximated by stacking concentric rounded shapes
    // with a quadratic alpha falloff. There is no GPU device here (this renders
    // to a DC-bound target for UpdateLayeredWindow), so a Gaussian blur effect
    // is not available -- but the render padding around the pill leaves room to
    // fake it convincingly and cheaply.
    void DrawSoftShadow(D2D1_RECT_F rect, float radius) {
        // The island sits on top of the backdrop, so a shadow here would darken the blur.
        if (!g_settings.dropShadow || g_settings.backdropMaterial != BackdropMaterial::None) {
            return;
        }

        const float spread = Clamp(14.0f * g_settings.sizeScale, 6.0f, kRenderPadY - 4.0f);
        const float yOffset = spread * 0.35f;
        constexpr int kSteps = 7;

        ID2D1SolidColorBrush* brush = AcquireBrush(material_.shadow);
        if (!brush) {
            return;
        }

        for (int i = kSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float grow = spread * t;
            // Quadratic falloff keeps the core dense and the outer edge feathered.
            const float alpha = material_.shadow.a * (1.0f - t) * (1.0f - t) * 0.55f * settingsOpacity_;
            if (alpha <= 0.002f) {
                continue;
            }
            brush->SetOpacity(alpha);

            const D2D1_RECT_F shadowRect = D2D1::RectF(
                rect.left - grow, rect.top - grow * 0.55f + yOffset,
                rect.right + grow, rect.bottom + grow + yOffset);
            FillIslandShape(shadowRect, radius + grow, g_settings.w11Style,
                            g_settings.notchStyle, brush);
        }
        brush->SetOpacity(1.0f);
    }

    // Vertical depth shading. This used to open with a white sheen across the
    // top, which on a dark island read as a lit strip along the upper edge --
    // the same edge-glow look the rest of this design removes. What is left is
    // a single downward shade that grounds the surface without lighting any
    // edge: neutral at the top, gradually deeper toward the bottom.
    void FillSurfaceDepth(D2D1_RECT_F rect, float radius, bool strong) {
        const float h = rect.bottom - rect.top;
        if (h <= 2.0f) {
            return;
        }

        // Light backgrounds need less of it: the same alpha over near-white
        // turns into a visible grey wash rather than subtle depth.
        const float botA = (strong ? 0.085f : 0.060f) *
                           (material_.onDark ? 1.0f : 0.55f) * settingsOpacity_;

        // Stops are constant; `botA` is applied as brush opacity (identical result,
        // since every stop alpha was just a multiple of botA). One brush forever.
        static const D2D1_GRADIENT_STOP kStops[3] = {
            {0.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            {0.45f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f)},
            {1.0f,  D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)},
        };
        ID2D1LinearGradientBrush* brush = UseLinearGradient(
            depthGrad_, kStops, 3,
            D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(botA, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // A wide, very soft accent wash bled in from the top of the surface. Driven
    // by the album-art accent, this is what makes the media surface feel alive
    // without tinting the whole pill.
    void FillAccentBloom(D2D1_RECT_F rect, float radius, float strength) {
        if (strength <= 0.01f) {
            return;
        }

        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        if (w <= 2.0f || h <= 2.0f) {
            return;
        }

        // Only the accent colour is baked into the stops. Strength is brush opacity, so
        // the gradient is rebuilt only while the accent is actually changing colour.
        const D2D1_COLOR_F a = material_.accent;
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, D2D1::ColorF(a.r, a.g, a.b, 1.0f)},
            {1.0f, D2D1::ColorF(a.r, a.g, a.b, 0.0f)},
        };
        const D2D1_POINT_2F center = D2D1::Point2F((rect.left + rect.right) * 0.5f, rect.top);
        ID2D1RadialGradientBrush* brush =
            UseRadialGradient(bloomGrad_, stops, 2, center, w * 0.75f, h * 1.15f);
        if (!brush) {
            return;
        }
        brush->SetOpacity(Clamp(strength * settingsOpacity_, 0.0f, 1.0f));
        FillIslandShape(rect, radius, g_settings.w11Style, g_settings.notchStyle, brush);
    }

    // Rounded inner card used by every dashboard, so panels across the media,
    // calendar, weather and hardware surfaces match exactly.
    //
    // Fill only, no outline. Each card used to also get a hairline ring, and six
    // of those side by side in the hardware grid turned into a mesh of bright
    // edges competing with the content. The fill alpha in BuildMaterialTokens
    // was raised to carry the separation on its own.
    void DrawCard(D2D1_RECT_F rect, float radius, bool active = false) {
        ID2D1SolidColorBrush* fill = active ? raisedStrongBrush_.Get() : raisedBrush_.Get();
        if (fill) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill);
        }
    }

    // Accent-filled progress track shared by the scrubber, volume, battery and
    // timer, so "progress" looks the same everywhere.
    void DrawAccentTrack(D2D1_RECT_F track, float progress, float radius) {
        if (raisedStrongBrush_) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), raisedStrongBrush_.Get());
        }

        const float span = (track.right - track.left) * Clamp(progress, 0.0f, 1.0f);
        if (span <= 0.5f) {
            return;
        }

        const D2D1_RECT_F fillRect = D2D1::RectF(track.left, track.top, track.left + span, track.bottom);

        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, WithAlpha(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.30f), 0.95f)},
            {1.0f, WithAlpha(material_.accent, 1.0f)},
        };
        if (ID2D1LinearGradientBrush* grad = UseLinearGradient(
                trackGrad_, stops, 2,
                D2D1::Point2F(fillRect.left, fillRect.top),
                D2D1::Point2F(fillRect.right, fillRect.top))) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(fillRect, radius, radius), grad);
        }
    }

    void DrawPrivacyDots(const SharedState& state, const Settings& settings, D2D1_RECT_F rect, double now) {
        UNREFERENCED_PARAMETER(now);
        const float height = rect.bottom - rect.top;
        if (height > 55.0f) return;

        const bool mic = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool cam = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;
        if (!mic && !cam) return;

        const float dotR   = 4.0f;
        const float margin = 16.0f;
        const float dotY   = rect.top + (rect.bottom - rect.top) * 0.5f;

        const float x = rect.right - margin - dotR;

        // Standalone 0.6: one dot -- orange = microphone, green = camera,
        // purple = both at once.
        D2D1_COLOR_F color = (mic && cam) ? settings.privacyDotsBothHex
                             : cam        ? settings.privacyDotsCamHex
                                          : settings.privacyDotsMicHex;
        color.a = settingsOpacity_;
        if (!micDotBrush_) target_->CreateSolidColorBrush(color, &micDotBrush_);
        else micDotBrush_->SetColor(color);
        if (micDotBrush_) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, dotY), dotR, dotR), micDotBrush_.Get());
        }
    }

    ComPtr<ID2D1PathGeometry> CreateNotchGeometry(D2D1_RECT_F rect, float radius) {
        ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(d2dFactory_->CreatePathGeometry(&geom))) return nullptr;

        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(&sink))) return nullptr;

        float r = std::min({radius, (rect.right - rect.left) * 0.5f, (rect.bottom - rect.top) * 0.5f});
        if (r < 0.0f) r = 0.0f;

        sink->BeginFigure(D2D1::Point2F(rect.left, rect.top), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(rect.right, rect.top));
        sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - r));
        if (r > 0.0f) {
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.right - r, rect.bottom),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(rect.left + r, rect.bottom));
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rect.left, rect.bottom - r),
                D2D1::SizeF(r, r), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        } else {
            sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();

        return geom;
    }

    ComPtr<ID2D1Geometry> CreateIslandMaskGeometry(D2D1_RECT_F rect, float radius, bool notchStyle) {
        return GetShapeGeometry(rect, radius, notchStyle);
    }

    void FillIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush) {
        if (!brush) return;
        if (notchStyle) {
            ComPtr<ID2D1Geometry> geom = GetShapeGeometry(rect, radius, true);
            if (geom) {
                target_->FillGeometry(geom.Get(), brush);
                return;
            }
        }
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    }

    void DrawIslandShape(D2D1_RECT_F rect, float radius, bool w11Style, bool notchStyle, ID2D1Brush* brush, float strokeWidth) {
        if (!brush) return;
        if (notchStyle) {
            auto geom = CreateNotchGeometry(rect, radius);
            if (geom) {
                target_->DrawGeometry(geom.Get(), brush, strokeWidth);
                return;
            }
        }
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, strokeWidth);
    }

    // ── Liquid Glass (custom fork, custom.17) ───────────────────────────────
    // An approximation of Apple's Liquid Glass. Windows gives the island no access
    // to the pixels behind it, so real lens refraction is impossible; the look is
    // built from what glass does to light at its edges:
    //   1. a thick, bright lens band just inside the contour (the glass "edge"),
    //   2. a soft sheen over the top and a faint caustic glow along the bottom,
    //   3. a specular rim: bright at the top-left and bottom-right, nearly gone
    //      in between, plus a fainter inner rim that gives the glass thickness,
    //   4. a hairline dark outline so the glass still separates from a light
    //      desktop.
    // The blur (or no blur, for "Clear") comes from DwmBackdrop underneath.
    void DrawLiquidGlass(D2D1_RECT_F rect, float radius, const Settings& settings) {
        const float w = rect.right - rect.left;
        const float h = rect.bottom - rect.top;
        if (w <= 8.0f || h <= 8.0f) {
            return;
        }
        const float shine = settings.liquidGlassShine * settingsOpacity_;
        if (shine <= 0.01f) {
            return;
        }
        const bool notch = settings.notchStyle;
        const bool w11 = settings.w11Style;
        const float lightBoost = material_.onDark ? 1.0f : 0.7f;  // white reads less on light themes

        // 1. Lens band: concentric white strokes fading inward.
        {
            const float thickness = Clamp(std::min(w, h) * 0.16f, 3.0f, 12.0f);
            constexpr int kSteps = 5;
            const float step = thickness / kSteps;
            for (int i = 0; i < kSteps; ++i) {
                const float t = static_cast<float>(i) / kSteps;
                const float a = 0.11f * (1.0f - t) * (1.0f - t) * shine * lightBoost;
                if (a < 0.004f) continue;
                const float d = step * (i + 0.5f);
                const D2D1_RECT_F r = D2D1::RectF(rect.left + d, rect.top + (notch ? 0.0f : d),
                                                  rect.right - d, rect.bottom - d);
                if (r.right - r.left < 2.0f || r.bottom - r.top < 2.0f) break;
                if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, Clamp(a, 0.0f, 1.0f)))) {
                    DrawIslandShape(r, std::max(radius - d, 0.0f), w11, notch, b, step + 0.6f);
                }
            }
        }

        // 2. Top sheen and bottom caustic in one vertical gradient.
        {
            static const D2D1_GRADIENT_STOP kStops[5] = {
                {0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f)},
                {0.18f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f)},
                {0.50f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f)},
                {0.80f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f)},
                {1.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f)},
            };
            if (ID2D1LinearGradientBrush* brush = UseLinearGradient(
                    glassSheenGrad_, kStops, 5, D2D1::Point2F(rect.left, rect.top),
                    D2D1::Point2F(rect.left, rect.bottom))) {
                brush->SetOpacity(Clamp(0.13f * shine * lightBoost, 0.0f, 1.0f));
                FillIslandShape(rect, radius, w11, notch, brush);
            }
        }

        // 3. Specular rim: a faint even rim, plus corner highlights that fade out
        //    toward the middle -- strong at the top-left, softer at the
        //    bottom-right (light from above-left, bounced back by the glass).
        {
            const D2D1_RECT_F outer = D2D1::RectF(rect.left + 0.7f, rect.top + (notch ? 0.0f : 0.7f),
                                                  rect.right - 0.7f, rect.bottom - 0.7f);
            const float outerR = std::max(radius - 0.7f, 0.0f);
            if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f,
                                                                    Clamp(0.09f * shine * lightBoost, 0.0f, 1.0f)))) {
                DrawIslandShape(outer, outerR, w11, notch, b, 1.0f);
            }

            static const D2D1_GRADIENT_STOP kStops[3] = {
                {0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f)},
                {0.35f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f)},
                {1.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f)},
            };
            // The highlight covers a square-ish corner area, so on a wide pill it
            // stays at the corner instead of smearing along the whole length.
            const float span = std::min(w * 0.55f, h * 2.4f);
            const float lean = std::min(h, span) * 0.6f;
            ID2D1LinearGradientBrush* brush = UseLinearGradient(
                glassRimGrad_, kStops, 3, D2D1::Point2F(rect.left, rect.top),
                D2D1::Point2F(rect.left + span, rect.top + lean));
            if (brush) {
                // Top-left corner.
                brush->SetOpacity(Clamp(0.70f * shine, 0.0f, 1.0f));
                DrawIslandShape(outer, outerR, w11, notch, brush, 1.4f);

                // Bottom-right corner.
                brush->SetStartPoint(D2D1::Point2F(rect.right, rect.bottom));
                brush->SetEndPoint(D2D1::Point2F(rect.right - span, rect.bottom - lean));
                brush->SetOpacity(Clamp(0.40f * shine, 0.0f, 1.0f));
                DrawIslandShape(outer, outerR, w11, notch, brush, 1.2f);

                // Inner rim at the top-left: fainter and thinner, a couple of px
                // in, so the edge reads as a slab of glass with thickness.
                const float d = 2.4f;
                const D2D1_RECT_F inner = D2D1::RectF(rect.left + d, rect.top + (notch ? 0.0f : d),
                                                      rect.right - d, rect.bottom - d);
                brush->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
                brush->SetEndPoint(D2D1::Point2F(rect.left + span, rect.top + lean));
                brush->SetOpacity(Clamp(0.22f * shine, 0.0f, 1.0f));
                DrawIslandShape(inner, std::max(radius - d, 0.0f), w11, notch, brush, 0.7f);
            }
        }

        // 4. Dark hairline on the very edge, for light wallpapers.
        if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f,
                                                                Clamp(0.14f * settingsOpacity_, 0.0f, 1.0f)))) {
            const D2D1_RECT_F edge = D2D1::RectF(rect.left + 0.25f, rect.top + (notch ? 0.0f : 0.25f),
                                                 rect.right - 0.25f, rect.bottom - 0.25f);
            DrawIslandShape(edge, std::max(radius - 0.25f, 0.0f), w11, notch, b, 0.5f);
        }
    }

    // The island's material, composed bottom-up:
    //   tint scrim -> base fill -> depth shading -> accent bloom -> contour
    // Each layer is individually subtle; together they give the pill depth
    // instead of the flat single-fill look it had before. Deliberately absent:
    // any bright rim, hairline or sheen tracing the island's edge.
    void DrawPillSurface(D2D1_RECT_F rect, float radius, IslandKind kind, const Settings& settings) {
        // The dark tint scrim exists to deepen an opaque background. With a real
        // backdrop enabled it would just mud up the blur, so it is skipped.
        if (tintBrush_ && settings.backdropMaterial == BackdropMaterial::None) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, tintBrush_.Get());
        }

        // Blur / acrylic and tint come from the DWM window below (see DwmBackdrop);
        // only the fill is drawn here.

        // User-defined pill background color. The authored alpha is combined
        // with the global pill transparency, so a plain 6-digit hex behaves
        // exactly as before (alpha 1.0) while #RRGGBBAA stays translucent.
        D2D1_COLOR_F bg = pillBgColor_;
        bg.a = Clamp(bg.a * settingsOpacity_, 0.0f, 1.0f);
        if (settings.backdropMaterial == BackdropMaterial::LiquidGlass) {
            // Custom fork (custom.17): glass keeps only a thin tint so the desktop
            // reads through. "Clear" keeps a little more, since nothing is blurred
            // behind it and white text still has to stay readable.
            const float c = settings.liquidGlassClarity;
            const float glassFill = settings.liquidGlassClear ? (0.62f - 0.40f * c)   // 0.62 .. 0.22
                                                              : (0.55f - 0.43f * c);  // 0.55 .. 0.12
            bg.a = std::min(bg.a, glassFill);
        } else if (settings.backdropMaterial != BackdropMaterial::None) {
            // An opaque fill would hide the backdrop, so cap it.
            bg.a = std::min(bg.a, settings.backdropFillAlpha);
        }
        if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(bg)) {
            FillIslandShape(rect, radius, settings.w11Style, settings.notchStyle, fillBrush);
        }

        if (settings.liquidGlass) {
            // Custom fork (custom.17): glass has no dark depth shading, only a
            // lighter accent tint from the content, then its own edges and shine.
            if (settings.materialDepth) {
                FillAccentBloom(rect, radius, 0.06f * settings.accentBloom);
            }
            DrawLiquidGlass(rect, radius, settings);
            return;
        }

        if (settings.materialDepth) {
            FillSurfaceDepth(rect, radius, kind == IslandKind::Media || kind == IslandKind::Idle);

            // Media leans on the album-art accent; other surfaces get a whisper
            // of it so the whole UI still feels connected to what is playing.
            const float bloom = (kind == IslandKind::Media) ? 0.115f
                                : (kind == IslandKind::Idle) ? 0.055f
                                                             : 0.075f;
            FillAccentBloom(rect, radius, bloom * settings.accentBloom);
        }

        if (settings.contourBorderMode != ContourBorderMode::Borderless && settings.contourBorderEnabled) {
            D2D1_COLOR_F borderColor = settings.contourBorderColor;
            float strokeWidth = settings.w11Style ? 1.0f : 0.8f;

            if (settings.contourBorderMode == ContourBorderMode::Auto) {
                if (currentAccent_.a > 0.0f) {
                    borderColor = currentAccent_;
                    borderColor.a = std::min(1.0f, (kind == IslandKind::Idle ? 0.35f : 0.60f) * settingsOpacity_);
                    strokeWidth = 1.0f;
                } else {
                    borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
                }
            } else {
                borderColor.a = std::min(1.0f, borderColor.a * settingsOpacity_);
            }

            if (ID2D1SolidColorBrush* border = AcquireBrush(borderColor)) {
                D2D1_RECT_F borderRect = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f,
                                                     rect.right - 0.5f, rect.bottom - 0.5f);
                DrawIslandShape(borderRect, radius, settings.w11Style, settings.notchStyle, border, strokeWidth);
            }
        }
    }


    // ── Stocks tab (custom fork, custom.18) ─────────────────────────────────
    // Six cards in the same 2 x 3 grid as the hardware monitor. Each card:
    //   ticker ............ change %
    //   price            ~~~sparkline~~~
    // Green / red follow the change against the close before the chart period.
    static std::wstring FormatStockPrice(double price, const std::wstring& currency, bool isIndex) {
        std::wstring prefix;
        std::wstring suffix;
        double value = price;
        if (isIndex) {
            // An index is points, not money (custom.19: S&P 500).
        } else if (currency == L"USD") prefix = L"$";
        else if (currency == L"EUR") prefix = L"€";
        else if (currency == L"GBP") prefix = L"£";
        else if (currency == L"GBp") { prefix = L"£"; value = price / 100.0; }
        else if (currency == L"JPY") prefix = L"¥";
        else if (currency == L"KRW") prefix = L"₩";
        else if (!currency.empty()) suffix = L" " + currency;

        wchar_t num[48];
        if (value >= 10000.0) swprintf_s(num, L"%.0f", value);
        else if (value >= 1000.0) swprintf_s(num, L"%.1f", value);
        else swprintf_s(num, L"%.2f", value);

        // Thin-space thousands separators on the integer part.
        std::wstring s = num;
        size_t dot = s.find(L'.');
        if (dot == std::wstring::npos) dot = s.size();
        for (int i = static_cast<int>(dot) - 3; i > 0; i -= 3) {
            s.insert(static_cast<size_t>(i), L" ");
        }
        return prefix + s + suffix;
    }

    static const wchar_t* StocksRangeLabel(StocksRange range) {
        switch (range) {
            case StocksRange::FiveDays:  return L"5 дней";
            case StocksRange::Month:     return L"1 месяц";
            case StocksRange::SixMonths: return L"6 месяцев";
            case StocksRange::Year:      return L"1 год";
            case StocksRange::Day:
            default:                     return L"1 день";
        }
    }

    void DrawStockSparkline(const StockQuote& q, D2D1_RECT_F area, const D2D1_COLOR_F& color,
                            bool showBase, int slot) {
        const size_t n = q.points.size();
        if (n < 2 || area.right - area.left < 8.0f || area.bottom - area.top < 4.0f) {
            return;
        }
        float lo = q.points[0];
        float hi = q.points[0];
        for (float v : q.points) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const float base = static_cast<float>(q.basePrice);
        if (showBase && base > 0.0f) {
            lo = std::min(lo, base);
            hi = std::max(hi, base);
        }
        if (hi - lo < 1e-6f) {
            hi += 0.5f;
            lo -= 0.5f;
        }
        const float w = area.right - area.left;
        const float h = area.bottom - area.top;
        auto px = [&](size_t i) { return area.left + w * static_cast<float>(i) / static_cast<float>(n - 1); };
        auto py = [&](float v) { return area.bottom - (v - lo) / (hi - lo) * h; };

        // Dotted reference line at the previous close.
        if (showBase && base > 0.0f) {
            if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(material_.textPrimary, 0.22f * settingsOpacity_))) {
                const float y = py(base);
                for (float x = area.left; x < area.right; x += 4.0f) {
                    target_->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(std::min(x + 1.6f, area.right), y), b, 0.8f);
                }
            }
        }

        ComPtr<ID2D1PathGeometry> line;
        ComPtr<ID2D1PathGeometry> fill;
        if (FAILED(d2dFactory_->CreatePathGeometry(&line)) || !line ||
            FAILED(d2dFactory_->CreatePathGeometry(&fill)) || !fill) {
            return;
        }
        ComPtr<ID2D1GeometrySink> ls;
        ComPtr<ID2D1GeometrySink> fs;
        if (FAILED(line->Open(&ls)) || FAILED(fill->Open(&fs))) {
            return;
        }
        ls->BeginFigure(D2D1::Point2F(px(0), py(q.points[0])), D2D1_FIGURE_BEGIN_HOLLOW);
        fs->BeginFigure(D2D1::Point2F(px(0), area.bottom), D2D1_FIGURE_BEGIN_FILLED);
        fs->AddLine(D2D1::Point2F(px(0), py(q.points[0])));
        for (size_t i = 1; i < n; ++i) {
            const D2D1_POINT_2F p = D2D1::Point2F(px(i), py(q.points[i]));
            ls->AddLine(p);
            fs->AddLine(p);
        }
        fs->AddLine(D2D1::Point2F(px(n - 1), area.bottom));
        ls->EndFigure(D2D1_FIGURE_END_OPEN);
        fs->EndFigure(D2D1_FIGURE_END_CLOSED);
        ls->Close();
        fs->Close();

        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, WithAlpha(color, 0.30f * settingsOpacity_)},
            {1.0f, WithAlpha(color, 0.0f)},
        };
        if (ID2D1LinearGradientBrush* grad =
                UseLinearGradient(sparkGrad_[std::clamp(slot, 0, kStockSlots - 1)], stops, 2,
                                  D2D1::Point2F(area.left, area.top), D2D1::Point2F(area.left, area.bottom))) {
            target_->FillGeometry(fill.Get(), grad);  // standalone 0.9: cached per card
        }
        if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(color, 0.95f * settingsOpacity_))) {
            target_->DrawGeometry(line.Get(), b, 1.4f);
        }
    }

    void DrawStocksDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now,
                             float scale) {
        // Tell the stocks thread someone is looking; ask for fresh data if it is old.
        g_stocksLastViewed.store(now);
        {
            static double s_lastAsk = -1e9;
            const double age = now - state.stocks.lastUpdated;
            if (!state.stocks.loading && (state.stocks.lastUpdated <= 0.0 ||
                                          age > settings.stocksRefreshMinutes * 60.0) &&
                now - s_lastAsk > 5.0 && g_stocksWakeEvent) {
                s_lastAsk = now;
                g_stocksForceFetch.store(true);
                SetEvent(g_stocksWakeEvent);
            }
        }

        const float padX = 24.0f * scale;
        const D2D1_COLOR_F up = D2D1::ColorF(0.204f, 0.780f, 0.349f, 1.0f);    // #34C759
        const D2D1_COLOR_F down = D2D1::ColorF(1.0f, 0.231f, 0.188f, 1.0f);    // #FF3B30
        const D2D1_COLOR_F flat = WithAlpha(material_.textPrimary, 0.6f);

        // Header: title left, period and update time right.
        const D2D1_RECT_F headRect = D2D1::RectF(rect.left + padX, rect.top + 16.0f * scale,
                                                 rect.right - padX, rect.top + 32.0f * scale);
        if (boldTextFormat_) {
            textBrush_->SetOpacity(0.96f);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const wchar_t* title = L"Акции";
            target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), boldTextFormat_.Get(), headRect,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (smallTextFormat_) {
            std::wstring info = StocksRangeLabel(settings.stocksRange);
            if (state.stocks.lastUpdated > 0.0) {
                wchar_t t[32];
                swprintf_s(t, L" · %02d:%02d", state.stocks.updatedAt.wHour, state.stocks.updatedAt.wMinute);
                info += t;
                if (!state.stocks.anyOk) info = L"нет данных · проверьте интернет";
            } else {
                info += L" · загрузка…";
            }
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            mutedBrush_->SetOpacity(0.60f);
            target_->DrawTextW(info.c_str(), static_cast<UINT32>(info.size()), smallTextFormat_.Get(), headRect,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float colGap = 9.0f * scale;
        const float colW = (rect.right - rect.left - padX * 2.0f - colGap) * 0.5f;
        const float rowH = 38.0f * scale;
        const float rowGap = 6.0f * scale;
        const float gridTop = rect.top + 38.0f * scale;

        for (int i = 0; i < kStockSlots; ++i) {
            const StockQuote& q = state.stocks.quotes[i];
            const int col = i % 2;
            const int row = i / 2;
            const float cardLeft = rect.left + padX + static_cast<float>(col) * (colW + colGap);
            const float cardTop = gridTop + static_cast<float>(row) * (rowH + rowGap);
            const D2D1_RECT_F card = D2D1::RectF(cardLeft, cardTop, cardLeft + colW, cardTop + rowH);
            DrawCard(card, 9.0f * scale);

            const std::wstring& sym = settings.stockSymbols[i];
            const StockInfo* info = FindStock(sym);
            const std::wstring label = info ? info->label : sym;
            const bool hasData = q.ok && q.symbol == sym;

            double change = 0.0;
            if (hasData && q.basePrice > 0.0) {
                change = (q.price - q.basePrice) / q.basePrice * 100.0;
            }
            const D2D1_COLOR_F tint = !hasData ? flat : (change > 0.005 ? up : (change < -0.005 ? down : flat));

            const float inX = 9.0f * scale;
            const float leftColRight = card.left + colW * 0.52f;

            // Ticker (top-left) and change (top-right).
            if (smallTextFormat_) {
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                mutedBrush_->SetOpacity(0.75f);
                const D2D1_RECT_F topRow = D2D1::RectF(card.left + inX, card.top + 2.0f * scale,
                                                       card.right - inX, card.top + 15.0f * scale);
                target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), smallTextFormat_.Get(), topRow,
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (hasData) {
                    wchar_t pct[24];
                    swprintf_s(pct, L"%s%.2f%%", change > 0.0 ? L"+" : (change < 0.0 ? L"−" : L""),
                               std::fabs(change));
                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(tint, 0.95f * settingsOpacity_))) {
                        target_->DrawTextW(pct, static_cast<UINT32>(wcslen(pct)), smallTextFormat_.Get(), topRow, b,
                                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            // Price (bottom-left).
            if (textFormat_) {
                const std::wstring price = hasData ? FormatStockPrice(q.price, q.currency, !sym.empty() && sym[0] == L'^')
                                                   : std::wstring(state.stocks.loading || state.stocks.lastUpdated <= 0.0
                                                                      ? L"…" : L"—");
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                target_->DrawTextW(price.c_str(), static_cast<UINT32>(price.size()), textFormat_.Get(),
                                   D2D1::RectF(card.left + inX, card.top + 15.0f * scale, leftColRight + 6.0f * scale,
                                               card.bottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            // Sparkline (bottom-right).
            if (hasData) {
                const D2D1_RECT_F area = D2D1::RectF(leftColRight + 10.0f * scale, card.top + 17.0f * scale,
                                                     card.right - inX, card.bottom - 5.0f * scale);
                DrawStockSparkline(q, area, tint, settings.stocksRange == StocksRange::Day, i);
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    static void GetWeatherIconAndText(int code, std::wstring& icon, std::wstring& text) {
        switch (code) {
            case 113: icon = L"☀️"; break;
            case 116: icon = L"⛅"; break;
            case 119: case 122: icon = L"☁️"; break;
            case 143: case 248: case 260: icon = L"🌫️"; break;
            case 200: case 386: case 389: case 392: case 395: icon = L"⛈️"; break;
            case 176: case 263: case 266: case 281: case 284: case 293: case 296: case 299: case 302: case 305: case 308: case 311: case 314: case 353: case 356: case 359: icon = L"🌧️"; break;
            case 179: case 182: case 185: case 227: case 230: case 317: case 320: case 323: case 326: case 329: case 332: case 335: case 338: case 350: case 362: case 365: case 368: case 371: icon = L"❄️"; break;
            default: icon = L"🌡️"; break;
        }
    }

    static int GetDaysInMonth(int year, int month) {
        if (month == 2) {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            return leap ? 29 : 28;
        }
        if (month == 4 || month == 6 || month == 9 || month == 11) return 30;
        return 31;
    }

    static int GetDayOfWeek(int year, int month, int day) {
        if (month < 3) { month += 12; year -= 1; }
        int k = year % 100;
        int j = year / 100;
        int h = (day + 13 * (month + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        return (h + 6) % 7;
    }



    void DrawCalendarDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, SYSTEMTIME& local) {
        (void)state;
        (void)now;

        // ── Hero column ──────────────────────────────────────────────────────
        // Today's date as an editorial block: month over a large day figure over
        // the weekday. Uses the shared DrawCard so it matches every other panel
        // instead of being a one-off translucent slab.
        const D2D1_RECT_F hero = D2D1::RectF(rect.left + 22.0f * scale, rect.top + 16.0f * scale,
                                             rect.left + 115.0f * scale, rect.bottom - 20.0f * scale);
        DrawCard(hero, 12.0f * scale);

        // The accent is the album-art / system accent like everywhere else. This
        // used to fall back to a hardcoded red (#D94A38) whenever the mode wasn't
        // "System", which was the one colour in the whole island that answered to
        // nothing -- it clashed with the accent on every other surface.
        D2D1_COLOR_F accentColor = (settings.calendarAccent == CalendarAccentMode::System)
            ? GetSystemAccentColor()
            : material_.accent;
        accentColor.a = 0.95f * settingsOpacity_;

        ID2D1SolidColorBrush* accent = AcquireBrush(accentColor);  // per-frame pool, no allocation

        if (calendarCachedDate_.wYear != local.wYear || calendarCachedDate_.wMonth != local.wMonth ||
            calendarCachedDate_.wDay != local.wDay ||
            calendarCachedFirstDaySetting_ != settings.firstDayOfWeek) {
            wchar_t monthNameBuf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"MMMM", monthNameBuf, ARRAYSIZE(monthNameBuf), nullptr);
            // No longer uppercased. towupper is per-character and locale-blind:
            // it turns Turkish "i" into "I" rather than "İ", and does nothing at
            // all for CJK month names, so the effect was inconsistent by locale.
            calendarCachedMonthName_ = monthNameBuf;

            wchar_t weekdayNameBuf[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, L"dddd", weekdayNameBuf, ARRAYSIZE(weekdayNameBuf), nullptr);
            calendarCachedWeekdayName_ = weekdayNameBuf;

            calendarCachedFirstDay_ = settings.firstDayOfWeek;
            if (calendarCachedFirstDay_ < 0) {
                DWORD localeFirstDay = 0;  // locale counts 0 = Monday
                calendarCachedFirstDay_ =
                    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                                    reinterpret_cast<LPWSTR>(&localeFirstDay),
                                    sizeof(localeFirstDay) / sizeof(wchar_t)) > 0
                        ? static_cast<int>((localeFirstDay + 1) % 7)
                        : 0;
            }

            // Same locale as the month and weekday names above, so the grid
            // headers can't end up in a different language from the hero.
            static const wchar_t* const kFallbackInitials[] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
            for (int weekday = 0; weekday < 7; ++weekday) {
                wchar_t initialBuf[16] = {};
                // CAL_SSHORTESTDAYNAME1 is Monday; weekday 0 here is Sunday.
                const CALTYPE calType = CAL_SSHORTESTDAYNAME1 + static_cast<CALTYPE>((weekday + 6) % 7);
                calendarCachedInitials_[weekday] =
                    GetCalendarInfoEx(LOCALE_NAME_USER_DEFAULT, CAL_GREGORIAN, nullptr, calType,
                                      initialBuf, ARRAYSIZE(initialBuf), nullptr) > 0
                        ? initialBuf
                        : kFallbackInitials[weekday];
            }
            calendarCachedFirstDaySetting_ = settings.firstDayOfWeek;

            calendarCachedDate_ = local;
        }

        // Month and year stay on separate lines. Putting them on one ("September
        // 2026") needs about 95px at 12px bold and the hero column only offers
        // 85, so any long month name clipped -- and month names are exactly the
        // strings that get long once localized.
        if (boldTextFormat_) {
            target_->DrawTextW(calendarCachedMonthName_.c_str(),
                               static_cast<UINT32>(calendarCachedMonthName_.size()),
                               boldTextFormat_.Get(),
                               D2D1::RectF(hero.left + 3.0f * scale, hero.top + 6.0f * scale,
                                           hero.right - 3.0f * scale, hero.top + 22.0f * scale),
                               accent, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        wchar_t yearStr[16] = {};
        swprintf_s(yearStr, L"%d", local.wYear);
        mutedBrush_->SetOpacity(0.62f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(yearStr, static_cast<UINT32>(wcslen(yearStr)), smallTextFormat_.Get(),
                               D2D1::RectF(hero.left, hero.top + 21.0f * scale,
                                           hero.right, hero.top + 36.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t dayStr[16] = {};
        swprintf_s(dayStr, L"%d", local.wDay);
        textBrush_->SetOpacity(0.97f);
        target_->DrawTextW(dayStr, static_cast<UINT32>(wcslen(dayStr)),
                           calDayLargeFormat_ ? calDayLargeFormat_.Get() : hugeTextFormat_.Get(),
                           D2D1::RectF(hero.left, hero.top + 34.0f * scale,
                                       hero.right, hero.bottom - 26.0f * scale),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

        mutedBrush_->SetOpacity(0.70f);
        if (boldTextFormat_) {
            target_->DrawTextW(calendarCachedWeekdayName_.c_str(),
                               static_cast<UINT32>(calendarCachedWeekdayName_.size()),
                               boldTextFormat_.Get(),
                               D2D1::RectF(hero.left + 4.0f * scale, hero.bottom - 24.0f * scale,
                                           hero.right - 4.0f * scale, hero.bottom - 6.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // ── Month grid ───────────────────────────────────────────────────────
        const float gridStart = rect.left + 134.0f * scale;
        const float gridTop = rect.top + 16.0f * scale;
        const float colW = 28.0f * scale;
        const float headerH = 18.0f * scale;
        const int firstDay = calendarCachedFirstDay_;

        const int startDayIdx = (GetDayOfWeek(local.wYear, local.wMonth, 1) - firstDay + 7) % 7;
        const int monthDays = GetDaysInMonth(local.wYear, local.wMonth);
        const int rowCount = (startDayIdx + monthDays + 6) / 7;  // 5 or 6

        // Row height is derived from the space actually available rather than
        // fixed at 26px. At 26 a six-row month ran to y=197 inside a 184px
        // island, so the last row was silently clipped by the island mask --
        // visible every month that starts late in the week.
        const float datesTop = gridTop + headerH + 7.0f * scale;
        const float gridBottom = rect.bottom - 14.0f * scale;
        const float rowH = (gridBottom - datesTop) / static_cast<float>(rowCount > 0 ? rowCount : 1);

        IDWriteTextFormat* gridFmt = calGridFormat_ ? calGridFormat_.Get() : boldTextFormat_.Get();

        // Weekday initials are uniformly quiet. They used to paint S and S in the
        // accent, which put three competing accent marks in the grid (both
        // weekend headers plus today) and made the headers look selected.
        mutedBrush_->SetOpacity(0.55f);
        for (int i = 0; i < 7; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(gridStart + i * colW, gridTop,
                                                 gridStart + (i + 1) * colW, gridTop + headerH);
            const std::wstring& initial = calendarCachedInitials_[(firstDay + i) % 7];
            target_->DrawTextW(initial.c_str(), static_cast<UINT32>(initial.size()), gridFmt, cell,
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        // Hairline under the weekday headers.
        const float hrY = gridTop + headerH + 3.0f * scale;
        ComPtr<ID2D1SolidColorBrush> hrBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_), &hrBrush)) && hrBrush) {
            target_->DrawLine(D2D1::Point2F(gridStart + 2.0f * scale, hrY),
                              D2D1::Point2F(gridStart + 7.0f * colW - 2.0f * scale, hrY),
                              hrBrush.Get(), 1.0f * scale);
        }

        // Today's marker is sized from the cell it has to live in, so it stays a
        // circle around the figure instead of a fixed 12px disc that swallowed
        // the digits once the rows got shorter.
        const float markerR = std::min(colW, rowH) * 0.5f - 1.5f * scale;

        int row = 0;
        int col = startDayIdx;
        for (int d = 1; d <= monthDays; ++d) {
            const D2D1_RECT_F cell = D2D1::RectF(gridStart + col * colW, datesTop + row * rowH,
                                                 gridStart + (col + 1) * colW, datesTop + (row + 1) * rowH);
            const std::wstring dayText = std::to_wstring(d);
            const int weekday = (firstDay + col) % 7;

            if (d == local.wDay) {
                target_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cell.left + colW * 0.5f, cell.top + rowH * 0.5f),
                                  markerR, markerR),
                    accent);
                // Today's figure is drawn against the accent fill, so it needs the
                // primary text colour at full strength rather than the 0.85 the
                // other days use.
                textBrush_->SetOpacity(1.0f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else if (weekday == 0 || weekday == 6) {
                // Weekends recede instead of taking the accent, leaving today as
                // the only accented thing in the grid.
                mutedBrush_->SetOpacity(0.62f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            } else {
                textBrush_->SetOpacity(0.88f);
                target_->DrawTextW(dayText.c_str(), static_cast<UINT32>(dayText.size()), gridFmt,
                                   cell, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }

            if (++col > 6) { col = 0; ++row; }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Fixes windhawk-mods#4352: wind direction was printed as compass
    // initialisms (WSW, NE), which is meteorologist shorthand rather than
    // something glanceable. These are flow arrows, not bearing arrows -- a wind
    // *from* the north-east is drawn as an arrow pointing south-west, matching
    // the convention weather apps use.
    std::wstring WindDirToArrow(const std::wstring& dir) {
        if (dir == L"N") return L"\x2193";
        if (dir == L"NNE" || dir == L"NE" || dir == L"ENE") return L"\x2199";
        if (dir == L"E") return L"\x2190";
        if (dir == L"ESE" || dir == L"SE" || dir == L"SSE") return L"\x2196";
        if (dir == L"S") return L"\x2191";
        if (dir == L"SSW" || dir == L"SW" || dir == L"WSW") return L"\x2197";
        if (dir == L"W") return L"\x2192";
        if (dir == L"WNW" || dir == L"NW" || dir == L"NNW") return L"\x2198";
        return dir;
    }

    // ── Vector weather icons ────────────────────────────────────────────────
    // All sized relative to `s` (the icon's box width) and centred on `c`, so a
    // single call site controls scale. Built from ellipses and rounded rects
    // rather than path geometry wherever possible, to keep per-frame allocation
    // down.

    void DrawCloudShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        const D2D1_RECT_F base = D2D1::RectF(c.x - 0.44f * s, c.y + 0.02f * s,
                                             c.x + 0.44f * s, c.y + 0.21f * s);
        target_->FillRoundedRectangle(D2D1::RoundedRect(base, 0.10f * s, 0.10f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.02f * s), 0.18f * s, 0.18f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.01f * s, c.y - 0.10f * s), 0.24f * s, 0.24f * s), brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x + 0.25f * s, c.y + 0.03f * s), 0.17f * s, 0.17f * s), brush);
    }

    void DrawSunShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush, float coreRadius = 0.26f) {
        target_->FillEllipse(D2D1::Ellipse(c, coreRadius * s, coreRadius * s), brush);

        const float inner = (coreRadius + 0.09f) * s;
        const float outer = (coreRadius + 0.21f) * s;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            target_->DrawLine(D2D1::Point2F(c.x + ca * inner, c.y + sa * inner),
                              D2D1::Point2F(c.x + ca * outer, c.y + sa * outer),
                              brush, std::max(1.2f, 0.055f * s));
        }
    }

    void DrawLightningShape(D2D1_POINT_2F c, float s, ID2D1Brush* brush) {
        ComPtr<ID2D1PathGeometry> bolt;
        if (FAILED(d2dFactory_->CreatePathGeometry(&bolt)) || !bolt) {
            return;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(bolt->Open(&sink)) || !sink) {
            return;
        }
        sink->BeginFigure(D2D1::Point2F(c.x + 0.07f * s, c.y + 0.16f * s), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(c.x - 0.11f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.00f * s, c.y + 0.44f * s));
        sink->AddLine(D2D1::Point2F(c.x - 0.06f * s, c.y + 0.66f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.15f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.03f * s, c.y + 0.36f * s));
        sink->AddLine(D2D1::Point2F(c.x + 0.12f * s, c.y + 0.16f * s));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target_->FillGeometry(bolt.Get(), brush);
    }

    void DrawWeatherIcon(D2D1_POINT_2F center, float size, WeatherVisual visual,
                         ID2D1Brush* strong, ID2D1Brush* soft) {
        const float s = size;
        const float stroke = std::max(1.3f, 0.06f * s);

        switch (visual) {
            case WeatherVisual::Clear:
                DrawSunShape(center, s, strong, 0.28f);
                break;

            case WeatherVisual::PartlyCloudy: {
                // Sun peeking out behind the cloud's upper-left.
                DrawSunShape(D2D1::Point2F(center.x - 0.20f * s, center.y - 0.20f * s), s * 0.62f, soft, 0.30f);
                DrawCloudShape(D2D1::Point2F(center.x + 0.05f * s, center.y + 0.06f * s), s * 0.92f, strong);
                break;
            }

            case WeatherVisual::Cloudy:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.06f * s), s, soft);
                DrawCloudShape(D2D1::Point2F(center.x + 0.04f * s, center.y + 0.06f * s), s * 0.86f, strong);
                break;

            case WeatherVisual::Fog: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.16f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float y = center.y + (0.24f + 0.15f * static_cast<float>(i)) * s;
                    const float half = (0.34f - 0.05f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(center.x - half, y),
                                      D2D1::Point2F(center.x + half, y), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Storm:
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.20f * s), s * 0.92f, soft);
                DrawLightningShape(center, s, strong);
                break;

            case WeatherVisual::Rain: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const float x = center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    const float y = center.y + 0.24f * s;
                    target_->DrawLine(D2D1::Point2F(x + 0.05f * s, y),
                                      D2D1::Point2F(x - 0.03f * s, y + 0.26f * s), soft, stroke);
                }
                break;
            }

            case WeatherVisual::Snow: {
                DrawCloudShape(D2D1::Point2F(center.x, center.y - 0.18f * s), s * 0.92f, strong);
                for (int i = 0; i < 3; ++i) {
                    const D2D1_POINT_2F f = D2D1::Point2F(
                        center.x + (-0.22f + 0.22f * static_cast<float>(i)) * s,
                        center.y + (0.34f + (i == 1 ? 0.06f : 0.0f)) * s);
                    const float r = 0.075f * s;
                    for (int k = 0; k < 3; ++k) {
                        const float a = static_cast<float>(k) * 3.14159265f / 3.0f;
                        target_->DrawLine(D2D1::Point2F(f.x - std::cos(a) * r, f.y - std::sin(a) * r),
                                          D2D1::Point2F(f.x + std::cos(a) * r, f.y + std::sin(a) * r),
                                          soft, std::max(1.0f, 0.035f * s));
                    }
                }
                break;
            }

            case WeatherVisual::Unknown:
            default: {
                // Thermometer: stem, bulb, and a couple of gradation ticks.
                const float stemW = 0.11f * s;
                const D2D1_RECT_F stem = D2D1::RectF(center.x - stemW, center.y - 0.40f * s,
                                                     center.x + stemW, center.y + 0.18f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, stemW, stemW), soft);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 0.26f * s), 0.19f * s, 0.19f * s), strong);
                const D2D1_RECT_F mercury = D2D1::RectF(center.x - stemW * 0.55f, center.y - 0.12f * s,
                                                        center.x + stemW * 0.55f, center.y + 0.20f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(mercury, stemW * 0.55f, stemW * 0.55f), strong);
                break;
            }
        }
    }

    // Small vector glyphs shared by the weather, hardware and game-overlay cards.
    //   0 wind   1 thermometer   2 droplet
    //   3 cpu    4 memory        5 gpu       6 upload   7 download   8 disk
    //   9 gauge (frame rate)
    //
    // The game overlay used to carry its own icon set (DrawGameIcon) drawn at
    // different stroke weights and proportions, so the same CPU appeared as two
    // different symbols depending on which surface you were looking at. Every
    // surface now draws from this one family.
    void DrawMetricGlyph(D2D1_POINT_2F c, float s, int kind, ID2D1Brush* brush) {
        const float stroke = std::max(1.1f, 0.11f * s);

        if (kind == 9) {  // gauge: dial arc, ticks and a needle
            const float r = 0.40f * s;
            target_->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke);
            for (int i = 0; i < 5; ++i) {
                const float a = -3.14159265f * 0.8f + static_cast<float>(i) * 3.14159265f * 0.4f;
                const float ca = std::cos(a);
                const float sa = std::sin(a);
                target_->DrawLine(D2D1::Point2F(c.x + ca * r, c.y + sa * r),
                                  D2D1::Point2F(c.x + ca * (r - 0.10f * s), c.y + sa * (r - 0.10f * s)),
                                  brush, stroke * 0.7f);
            }
            target_->FillEllipse(D2D1::Ellipse(c, 0.075f * s, 0.075f * s), brush);
            const float na = -3.14159265f * 0.25f;
            target_->DrawLine(c, D2D1::Point2F(c.x + std::cos(na) * r * 0.82f,
                                               c.y + std::sin(na) * r * 0.82f),
                              brush, stroke);
            return;
        }

        // Arrow used by the upload/download glyphs; `dir` is -1 up, +1 down.
        auto drawArrow = [&](float dir) {
            target_->DrawLine(D2D1::Point2F(c.x, c.y - 0.38f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.34f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x - 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
            target_->DrawLine(D2D1::Point2F(c.x + 0.24f * s, c.y + 0.10f * s * dir),
                              D2D1::Point2F(c.x, c.y + 0.36f * s * dir), brush, stroke);
        };

        switch (kind) {
            case 3: {  // cpu: chip body with pins on all four sides
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.28f * s, c.y - 0.28f * s,
                                                     c.x + 0.28f * s, c.y + 0.28f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.07f * s, 0.07f * s), brush, stroke);
                const D2D1_RECT_F core = D2D1::RectF(c.x - 0.11f * s, c.y - 0.11f * s,
                                                     c.x + 0.11f * s, c.y + 0.11f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(core, 0.03f * s, 0.03f * s), brush);
                for (int i = -1; i <= 1; ++i) {
                    const float o = static_cast<float>(i) * 0.15f * s;
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y - 0.28f * s), D2D1::Point2F(c.x + o, c.y - 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + o, c.y + 0.28f * s), D2D1::Point2F(c.x + o, c.y + 0.42f * s), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x - 0.28f * s, c.y + o), D2D1::Point2F(c.x - 0.42f * s, c.y + o), brush, stroke * 0.8f);
                    target_->DrawLine(D2D1::Point2F(c.x + 0.28f * s, c.y + o), D2D1::Point2F(c.x + 0.42f * s, c.y + o), brush, stroke * 0.8f);
                }
                break;
            }
            case 4: {  // memory: module with contact notches along the bottom
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.42f * s, c.y - 0.26f * s,
                                                     c.x + 0.42f * s, c.y + 0.22f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                for (int i = -2; i <= 2; ++i) {
                    const float x = c.x + static_cast<float>(i) * 0.16f * s;
                    target_->DrawLine(D2D1::Point2F(x, c.y - 0.10f * s), D2D1::Point2F(x, c.y + 0.06f * s), brush, stroke * 0.85f);
                }
                target_->DrawLine(D2D1::Point2F(c.x - 0.20f * s, c.y + 0.34f * s),
                                  D2D1::Point2F(c.x + 0.20f * s, c.y + 0.34f * s), brush, stroke);
                break;
            }
            case 5: {  // gpu: board with a fan
                const D2D1_RECT_F body = D2D1::RectF(c.x - 0.44f * s, c.y - 0.24f * s,
                                                     c.x + 0.44f * s, c.y + 0.26f * s);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.06f * s, 0.06f * s), brush, stroke);
                target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.15f * s, 0.15f * s), brush, stroke * 0.9f);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - 0.14f * s, c.y + 0.01f * s), 0.045f * s, 0.045f * s), brush);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y - 0.10f * s), D2D1::Point2F(c.x + 0.32f * s, c.y - 0.10f * s), brush, stroke * 0.8f);
                target_->DrawLine(D2D1::Point2F(c.x + 0.16f * s, c.y + 0.04f * s), D2D1::Point2F(c.x + 0.32f * s, c.y + 0.04f * s), brush, stroke * 0.8f);
                break;
            }
            case 6:  // upload
                drawArrow(-1.0f);
                break;
            case 7:  // download
                drawArrow(1.0f);
                break;
            case 8: {  // disk: stacked platters
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y - 0.22f * s + static_cast<float>(i) * 0.22f * s;
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, y), 0.36f * s, 0.12f * s), brush, stroke * 0.9f);
                }
                break;
            }
            default:
                break;
        }
        if (kind >= 3) {
            return;
        }

        switch (kind) {
            case 0: {  // wind: three streaming lines
                const float lens[3] = {0.46f, 0.30f, 0.38f};
                for (int i = 0; i < 3; ++i) {
                    const float y = c.y + (-0.22f + 0.22f * static_cast<float>(i)) * s;
                    target_->DrawLine(D2D1::Point2F(c.x - 0.44f * s, y),
                                      D2D1::Point2F(c.x - 0.44f * s + lens[i] * s * 1.9f, y),
                                      brush, stroke);
                }
                break;
            }
            case 1: {  // thermometer
                const float w = 0.13f * s;
                const D2D1_RECT_F stem = D2D1::RectF(c.x - w, c.y - 0.42f * s, c.x + w, c.y + 0.12f * s);
                target_->FillRoundedRectangle(D2D1::RoundedRect(stem, w, w), brush);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y + 0.24f * s), 0.22f * s, 0.22f * s), brush);
                break;
            }
            case 2:
            default: {  // droplet: circle body with a tapered tip
                ComPtr<ID2D1PathGeometry> drop;
                if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&drop)) && drop) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(drop->Open(&sink)) && sink) {
                        sink->BeginFigure(D2D1::Point2F(c.x, c.y - 0.44f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x + 0.34f * s, c.y - 0.02f * s),
                            D2D1::Point2F(c.x + 0.30f * s, c.y + 0.36f * s),
                            D2D1::Point2F(c.x, c.y + 0.38f * s)));
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(c.x - 0.30f * s, c.y + 0.36f * s),
                            D2D1::Point2F(c.x - 0.34f * s, c.y - 0.02f * s),
                            D2D1::Point2F(c.x, c.y - 0.44f * s)));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        target_->FillGeometry(drop.Get(), brush);
                    }
                }
                break;
            }
        }
    }

    void DrawWeatherDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, bool hasWeather, const std::wstring& wIcon, const std::wstring& wText) {
        wchar_t wTemp[32] = {};
        if (hasWeather) swprintf_s(wTemp, L"%.0f\x00B0", state.weather.temperature);
        else wcscpy_s(wTemp, L"--\x00B0");

        std::wstring city = hasWeather ? state.weather.city : std::wstring(Loc(L"Locating..."));
        std::wstring desc = wText;

        // ── Layout ───────────────────────────────────────────────────────────
        // Left is an editorial hero block (place / reading / condition), left
        // aligned so the city, temperature and description share one optical
        // margin. Right is a stack of metric cards using the same card primitive
        // as every other dashboard. The old version centred each element in its
        // own box, which is what left the icon and the temperature floating apart
        // with a dead gap between them.
        const float heroLeft = rect.left + 26.0f * scale;
        const float heroRight = rect.left + 188.0f * scale;
        const float dividerX = rect.left + 200.0f * scale;
        const float metricLeft = rect.left + 214.0f * scale;
        const float metricRight = rect.right - 22.0f * scale;

        // Accent-tinted icon brushes: `strong` carries the shape, `soft` the
        // secondary detail (rain, rays, fog bands).
        ComPtr<ID2D1SolidColorBrush> iconStrong;
        ComPtr<ID2D1SolidColorBrush> iconSoft;
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.95f * settingsOpacity_), &iconStrong);
        target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.42f * settingsOpacity_), &iconSoft);

        // Place label.
        textBrush_->SetOpacity(0.55f);
        if (smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(city.c_str(), static_cast<UINT32>(city.length()), smallTextFormat_.Get(),
                               D2D1::RectF(heroLeft, rect.top + 26.0f * scale, heroRight, rect.top + 44.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // Icon and temperature share one band and sit adjacent, so they read as
        // a single unit instead of two centred items.
        const float readingCenterY = rect.top + 82.0f * scale;
        if (hasWeather && iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisualFromCode(state.weather.weatherCode),
                            iconStrong.Get(), iconSoft.Get());
        } else if (iconStrong && iconSoft) {
            DrawWeatherIcon(D2D1::Point2F(heroLeft + 22.0f * scale, readingCenterY), 44.0f * scale,
                            WeatherVisual::Unknown, iconStrong.Get(), iconSoft.Get());
        }

        textBrush_->SetOpacity(0.98f);
        if (hugeTextFormat_) {
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(wTemp, static_cast<UINT32>(wcslen(wTemp)), hugeTextFormat_.Get(),
                               D2D1::RectF(heroLeft + 52.0f * scale, readingCenterY - 32.0f * scale,
                                           heroRight, readingCenterY + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            hugeTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float descTop = rect.top + 120.0f * scale;
        const float descBottom = rect.top + 162.0f * scale;

        // Description
        const size_t descLength = desc.length();
        float descFontSize = 13.5f;
        if (descLength > 58) descFontSize = 9.8f;
        else if (descLength > 44) descFontSize = 10.5f;
        else if (descLength > 32) descFontSize = 11.5f;
        else if (descLength > 22) descFontSize = 12.5f;
        descFontSize *= scale;

        if (std::fabs(weatherDescFormatSize_ - descFontSize) > 0.01f || !weatherDescFormat_) {
            weatherDescFormat_.Reset();
            if (dwriteFactory_) {
                bool created = false;
                if (!settings.fontFamily.empty()) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        settings.fontFamily.c_str(), nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (SUCCEEDED(hr)) created = true;
                }
                if (!created) {
                    HRESULT hr = dwriteFactory_->CreateTextFormat(
                        L"Segoe UI Variable Display", nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        descFontSize, L"", &weatherDescFormat_);
                    if (FAILED(hr)) {
                        dwriteFactory_->CreateTextFormat(
                            L"Segoe UI", nullptr,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            descFontSize, L"", &weatherDescFormat_);
                    }
                }
            }
            if (weatherDescFormat_) {
                weatherDescFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                // Left aligned to share the hero column's margin with the city
                // and temperature.
                weatherDescFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                weatherDescFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
            weatherDescFormatSize_ = descFontSize;
        }

        textBrush_->SetOpacity(0.82f);
        target_->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.length()),
                           weatherDescFormat_ ? weatherDescFormat_.Get() : textFormat_.Get(),
                           D2D1::RectF(heroLeft, descTop, heroRight, descBottom),
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        textBrush_->SetOpacity(0.96f);

        // Hairline divider, inset from both ends so it reads as a separator
        // rather than a full-height rule.
        ID2D1SolidColorBrush* divider = AcquireBrush(WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_));  // per-frame pool, no allocation
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dividerX, rect.top + 34.0f * scale,
                                          dividerX + 1.0f * scale, rect.bottom - 34.0f * scale),
                              0.5f * scale, 0.5f * scale), divider);

        // ── Metric cards ─────────────────────────────────────────────────────
        // Each card is icon + label on one line with the value beneath, which
        // keeps long localized labels from colliding with the value the way a
        // single "Label: value" line did.
        struct Metric {
            int glyph;
            const wchar_t* label;
            std::wstring value;
        };

        const std::wstring windUnit = settings.weatherFahrenheit ? L" mph" : L" km/h";
        const Metric metrics[3] = {
            {0, Loc(L"Wind"),
             hasWeather ? state.weather.windSpeed + windUnit + L" " + WindDirToArrow(state.weather.windDir)
                        : std::wstring(L"--")},
            {1, Loc(L"Feels Like"),
             hasWeather ? state.weather.feelsLike + L"\x00B0" : std::wstring(L"--")},
            {2, Loc(L"Humidity"),
             hasWeather ? state.weather.humidity + L"%" : std::wstring(L"--")},
        };

        const float cardH = 40.0f * scale;
        const float cardGap = 7.0f * scale;
        const float stackH = cardH * 3.0f + cardGap * 2.0f;
        float cardTop = (rect.top + rect.bottom) * 0.5f - stackH * 0.5f;

        for (const Metric& m : metrics) {
            const D2D1_RECT_F card = D2D1::RectF(metricLeft, cardTop, metricRight, cardTop + cardH);
            DrawCard(card, 10.0f * scale);

            if (iconSoft) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 17.0f * scale, card.top + 14.0f * scale),
                                14.0f * scale, m.glyph, iconSoft.Get());
            }

            const float textLeft = card.left + 30.0f * scale;
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.62f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 4.0f * scale,
                                               card.right - 8.0f * scale, card.top + 20.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                target_->DrawTextW(m.value.c_str(), static_cast<UINT32>(m.value.size()), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 19.0f * scale,
                                               card.right - 8.0f * scale, card.bottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            cardTop += cardH + cardGap;
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    // Load colour is *semantic*, not decorative: the accent while a component is
    // comfortable, amber under pressure, red when saturated. The previous design
    // assigned a fixed rainbow colour per metric, which carried no information
    // (CPU and NET UP were both green, GPU and NET DOWN both cyan) and fought the
    // album-art accent.
    D2D1_COLOR_F LoadStateColor(float fraction) const {
        if (fraction >= 0.90f) {
            return D2D1::ColorF(1.0f, 0.35f, 0.32f, 1.0f);
        }
        if (fraction >= 0.75f) {
            return D2D1::ColorF(1.0f, 0.69f, 0.13f, 1.0f);
        }
        return material_.accent;
    }

    void DrawHardwareMonitorDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, float scale) {
        (void)settings;

        const float padX = 24.0f * scale;

        // Header, left aligned to match the weather dashboard.
        textBrush_->SetOpacity(0.96f);
        const wchar_t* hwTitle = Loc(L"Hardware Monitor");
        if (boldTextFormat_) {
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            target_->DrawTextW(hwTitle, static_cast<UINT32>(wcslen(hwTitle)), boldTextFormat_.Get(),
                               D2D1::RectF(rect.left + padX, rect.top + 16.0f * scale,
                                           rect.right - padX, rect.top + 32.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            boldTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        wchar_t cpuBuf[32], ramBuf[40], gpuBuf[32], upBuf[32], downBuf[32], diskBuf[32];
        swprintf_s(cpuBuf, L"%d%%", state.system.cpuPercent);
        swprintf_s(ramBuf, L"%.1f / %.1f GB", state.system.memoryUsedGB, state.system.memoryTotalGB);
        if (state.system.gpuPercent >= 0) {
            swprintf_s(gpuBuf, L"%d%%", state.system.gpuPercent);
        } else {
            wcscpy_s(gpuBuf, L"--");
        }
        swprintf_s(upBuf, L"%.1f Mbps", state.system.netUpMbps);
        swprintf_s(downBuf, L"%.1f Mbps", state.system.netDownMbps);
        const int diskUsed = 100 - state.system.diskFreePercent;
        swprintf_s(diskBuf, L"%d%%", diskUsed);

        const float ramFraction = (state.system.memoryTotalGB > 0.01f)
            ? Clamp(state.system.memoryUsedGB / state.system.memoryTotalGB, 0.0f, 1.0f)
            : -1.0f;

        struct HwMetric {
            int glyph;
            const wchar_t* label;
            const wchar_t* value;
            float fraction;  // negative means "no meaningful 0-100 scale"
        };

        // Network throughput has no natural ceiling, so those two cards get no
        // load bar rather than a bar against an invented maximum.
        const HwMetric metrics[6] = {
            {3, L"CPU",      cpuBuf,  Clamp(state.system.cpuPercent / 100.0f, 0.0f, 1.0f)},
            {6, L"NET UP",   upBuf,   -1.0f},
            {4, L"RAM",      ramBuf,  ramFraction},
            {7, L"NET DOWN", downBuf, -1.0f},
            {5, L"GPU",      gpuBuf,  state.system.gpuPercent >= 0
                                          ? Clamp(state.system.gpuPercent / 100.0f, 0.0f, 1.0f)
                                          : -1.0f},
            {8, L"DISK",     diskBuf, Clamp(diskUsed / 100.0f, 0.0f, 1.0f)},
        };

        // 2 x 3 grid of cards. Cards give the grid its structure, so the old
        // full-height centre divider is gone.
        const float colGap = 9.0f * scale;
        const float colW = (rect.right - rect.left - padX * 2.0f - colGap) * 0.5f;
        const float rowH = 38.0f * scale;
        const float rowGap = 6.0f * scale;
        const float gridTop = rect.top + 38.0f * scale;

        for (int i = 0; i < 6; ++i) {
            const HwMetric& m = metrics[i];
            const int col = i % 2;
            const int row = i / 2;

            const float cardLeft = rect.left + padX + static_cast<float>(col) * (colW + colGap);
            const float cardTop = gridTop + static_cast<float>(row) * (rowH + rowGap);
            const D2D1_RECT_F card = D2D1::RectF(cardLeft, cardTop, cardLeft + colW, cardTop + rowH);

            DrawCard(card, 9.0f * scale);

            const bool hasBar = m.fraction >= 0.0f;
            const D2D1_COLOR_F tint = hasBar ? LoadStateColor(m.fraction) : material_.accent;

            if (ID2D1SolidColorBrush* glyphBrush = AcquireBrush(WithAlpha(tint, 0.85f * settingsOpacity_))) {
                DrawMetricGlyph(D2D1::Point2F(card.left + 16.0f * scale, card.top + 15.0f * scale),
                                15.0f * scale, m.glyph, glyphBrush);
            }

            const float textLeft = card.left + 30.0f * scale;
            const float textRight = card.right - 9.0f * scale;

            // Label sits above the value rather than sharing a line with it.
            // A single "Label: value" line collides once localized -- German
            // "Luftfeuchte" or Russian "Влажность" leave no room for the number.
            // The three bands (label / value / bar) are kept adjacent but
            // non-overlapping so nothing clips into its neighbour.
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.60f);
                target_->DrawTextW(m.label, static_cast<UINT32>(wcslen(m.label)), smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 2.0f * scale,
                                               textRight, card.top + 15.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (textFormat_) {
                textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                textBrush_->SetOpacity(0.95f);
                // Without a bar the value takes the band the bar would have
                // used, so the two card shapes stay optically balanced.
                const float valueBottom = hasBar ? card.top + 30.0f * scale : card.bottom - 4.0f * scale;
                target_->DrawTextW(m.value, static_cast<UINT32>(wcslen(m.value)), textFormat_.Get(),
                                   D2D1::RectF(textLeft, card.top + 15.0f * scale, textRight, valueBottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            if (hasBar) {
                const D2D1_RECT_F track = D2D1::RectF(textLeft, card.bottom - 6.0f * scale,
                                                      textRight, card.bottom - 3.5f * scale);
                ComPtr<ID2D1SolidColorBrush> trackBrush;
                if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                    target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
                }

                const float span = (track.right - track.left) * m.fraction;
                if (span > 0.5f) {
                    if (ID2D1SolidColorBrush* fillBrush = AcquireBrush(WithAlpha(tint, 0.95f))) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                              1.25f * scale, 1.25f * scale),
                            fillBrush);
                    }
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    void DrawTimeDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now, float scale, SYSTEMTIME& local) {
        (void)now;

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (settings.clockAccentGlow) {
            D2D1_COLOR_F accentColor = accentBrush_ ? accentBrush_->GetColor() : D2D1::ColorF(0x4cc9f0);
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.24f)},
                {1.0f, D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.0f)},
            };
            // Standalone 0.9: cached; rebuilt only while the accent colour changes.
            const D2D1_POINT_2F glowCenter = D2D1::Point2F(cx, cy - 8.0f * scale);
            if (ID2D1RadialGradientBrush* glowBrush =
                    UseRadialGradient(clockGlowGrad_, stops, 2, glowCenter, 150.0f * scale, 70.0f * scale)) {
                target_->FillEllipse(D2D1::Ellipse(glowCenter, 150.0f * scale, 70.0f * scale), glowBrush);
            }
        }

        // Time and date as a single typographic block. `dateFirst` promotes the
        // date to the headline for people who care about it more than the clock
        // (#61); either way the pair stays optically centred as a unit.
        const std::wstring timeText = FormatIslandTime(local, settings.clockFollowSystem,
                                                       settings.use24HourClock, settings.showSeconds);
        const std::wstring dateText = FormatIslandDate(local, settings.dateFormat, L"dddd, MMMM d");

        IDWriteTextFormat* bigFmt = timeDashboardFormat_ ? timeDashboardFormat_.Get() : hugeTextFormat_.Get();
        IDWriteTextFormat* smallFmt = dateDashboardFormat_ ? dateDashboardFormat_.Get() : boldTextFormat_.Get();

        const std::wstring& headline = settings.dateFirst ? dateText : timeText;
        const std::wstring& subline = settings.dateFirst ? timeText : dateText;
        IDWriteTextFormat* headlineFmt = settings.dateFirst ? smallFmt : bigFmt;
        IDWriteTextFormat* sublineFmt = settings.dateFirst ? bigFmt : smallFmt;

        if (settings.dateFirst) {
            // Date on top reads as a label, so give the big clock the lower slot.
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 46.0f * scale, rect.right, cy - 22.0f * scale);
            textBrush_->SetOpacity(0.92f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy - 20.0f * scale, rect.right, cy + 44.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const D2D1_RECT_F headRect = D2D1::RectF(rect.left, cy - 44.0f * scale, rect.right, cy + 20.0f * scale);
            textBrush_->SetOpacity(0.98f);
            target_->DrawTextW(headline.c_str(), static_cast<UINT32>(headline.size()), headlineFmt,
                               headRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

            const D2D1_RECT_F subRect = D2D1::RectF(rect.left, cy + 22.0f * scale, rect.right, cy + 46.0f * scale);
            mutedBrush_->SetOpacity(0.85f);
            target_->DrawTextW(subline.c_str(), static_cast<UINT32>(subline.size()), sublineFmt,
                               subRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        textBrush_->SetOpacity(0.98f);

        const bool micActive = state.system.micActive && settings.privacyDots && settings.privacyDotsMic;
        const bool camActive = state.system.cameraActive && settings.privacyDots && settings.privacyDotsCam;

        if (camActive || micActive) {
            std::wstring label;
            D2D1_COLOR_F dotColor;

            if (camActive && micActive) {
                dotColor = settings.privacyDotsBothHex;
                if (!state.system.cameraApp.empty() && !state.system.micApp.empty() && state.system.cameraApp == state.system.micApp) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.cameraApp.empty() && !state.system.micApp.empty()) {
                    label = state.system.cameraApp + L" & " + state.system.micApp + L" using camera & mic";
                } else if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using camera & microphone";
                } else if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using camera & microphone";
                } else {
                    label = L"Camera & microphone in use";
                }
            } else if (camActive) {
                dotColor = settings.privacyDotsCamHex;
                if (!state.system.cameraApp.empty()) {
                    label = state.system.cameraApp + L" is using your camera";
                } else {
                    label = L"Camera in use";
                }
            } else {
                dotColor = settings.privacyDotsMicHex;
                if (!state.system.micApp.empty()) {
                    label = state.system.micApp + L" is using your microphone";
                } else {
                    label = L"Microphone in use";
                }
            }

            IDWriteTextFormat* fmt = smallTextFormat_ ? smallTextFormat_.Get() : textFormat_.Get();
            if (fmt && dwriteFactory_) {
                ComPtr<IDWriteTextLayout> textLayout;
                HRESULT hr = dwriteFactory_->CreateTextLayout(
                    label.c_str(), static_cast<UINT32>(label.size()),
                    fmt, 500.0f, 30.0f, &textLayout);

                if (SUCCEEDED(hr) && textLayout) {
                    DWRITE_TEXT_METRICS tm = {};
                    textLayout->GetMetrics(&tm);

                    const float pillH = 22.0f * scale;
                    const float pillW = tm.width + 28.0f * scale;
                    const float pillX = cx - pillW * 0.5f;
                    const float pillY = rect.bottom - 18.0f * scale - pillH;

                    ID2D1SolidColorBrush* badgeBg = AcquireBrush(WithAlpha(material_.raised, material_.raised.a * settingsOpacity_));  // per-frame pool, no allocation
                    if (badgeBg) {
                        target_->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH), pillH * 0.5f, pillH * 0.5f),
                            badgeBg);
                    }

                    ComPtr<ID2D1SolidColorBrush> dotBrush;
                    dotColor.a = settingsOpacity_;
                    target_->CreateSolidColorBrush(dotColor, &dotBrush);
                    if (dotBrush) {
                        const float badgeDotR = 3.5f * scale;
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(pillX + 10.0f * scale, pillY + pillH * 0.5f), badgeDotR, badgeDotR),
                            dotBrush.Get());
                    }

                    mutedBrush_->SetOpacity(0.90f);
                    target_->DrawTextLayout(
                        D2D1::Point2F(pillX + 18.0f * scale, pillY + (pillH - tm.height) * 0.5f),
                        textLayout.Get(), mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    mutedBrush_->SetOpacity(0.75f);
                }
            }
        }

        textBrush_->SetOpacity(0.96f);
        mutedBrush_->SetOpacity(0.75f);
    }

    void DrawIdleDashboard(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                           double now) {
        if (settings.gameOverlay) {
            DrawGameOverlay(state, rect, 1.0f);
            return;
        }
        if (!clockFormat_) return;

        // Clip to the island's real silhouette (pill / notch / w11 rounded
        // rect), not its bounding box — a plain rect clip leaves the corners
        // outside the rounded shape unclipped, which is what was showing up
        // as a faint square "border" around the round island while collapsing.
        // Publish the same content-space geometry the media surface does, so the
        // Do Not Disturb toggle's hit test works while the island is idle too.
        PublishContentGeometry(rect);

        const float dashHeight = rect.bottom - rect.top;
        const float dashRadius = ContentIslandRadius(dashHeight);
        ComPtr<ID2D1Geometry> dashMask = CreateIslandMaskGeometry(rect, dashRadius, g_settings.notchStyle);
        ID2D1Layer* dashLayer = AcquireLayer();
        const bool haveDashMask = dashMask && dashLayer;
        if (haveDashMask) {
            target_->PushLayer(D2D1::LayerParameters(rect, dashMask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), dashLayer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        SYSTEMTIME local = {};
        GetLocalTime(&local);
        const std::wstring collapsedTime = FormatIslandTime(local, settings.clockFollowSystem,
                                                            settings.use24HourClock,
                                                            settings.showSeconds);
        const wchar_t* timeBuf = collapsedTime.c_str();

        const float scale = 1.0f;
        const float width = rect.right - rect.left;

        bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
        std::wstring wIcon = L"🌡️";
        std::wstring wText = Loc(L"Loading...");
        if (hasWeather) {
            wText = state.weather.weatherDesc;
            GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
        }

        // Cross-fade the collapsed status-bar view and the expanded tab view
        // across a small width band around the switchover point instead of
        // a hard cut. Previously this was a plain if/else on width, so the
        // clock's glow (and everything else in the expanded view) just got
        // clipped smaller as the pill shrank and then vanished outright the
        // instant width crossed the threshold — a pop, not a fade. Blending
        // both views by opacity (with an eased curve) makes it a dissolve.
        (void)width;
        auto SmoothFade = [](float t) {
            t = Clamp(t, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        // Height-driven, like the media surface: collapsed content is gone by
        // ~70px, expanded content starts at ~56px and is solid by ~120px.
        const float collapsedAlpha = SmoothFade((70.0f - dashHeight) / 30.0f);
        const float expandedAlpha = SmoothFade((dashHeight - 56.0f) / 64.0f);

        ID2D1Layer* dashFadeLayer = AcquireLayer();

        if (collapsedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), collapsedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Collapsed Mode (Apple Dynamic Island status bar).
            //
            // Slots are placed from the same measurements that sized the pill, so
            // the clock gets exactly the room it needs and the divider sits between
            // the two strings instead of at an arbitrary geometric centre. The old
            // version split the pill 50/50 at centerX and padded both ends by 6px,
            // which is what produced the dead air on short strings
            // (windhawk-mods#5086).
            const IdleStripMetrics idleMetrics = MeasureIdleStrip(state, settings, now);
            IDWriteTextFormat* idleFmt =
                idleTextFormat_ ? idleTextFormat_.Get() : smallTextFormat_.Get();

            // The dot is anchored to the right edge by DrawPrivacyDots, so reserve
            // its lane rather than shrinking the text box out from under the clock.
            const float privacyReserve =
                idleMetrics.hasPrivacy ? IdleStripLayout::kPrivacyReserve * scale : 0.0f;
            const float innerLeft = rect.left + IdleStripLayout::kPadX * scale + privacyReserve;
            const float innerRight =
                rect.right - IdleStripLayout::kPadX * scale - privacyReserve;

            float blockWidth = idleMetrics.clockWidth;
            if (idleMetrics.hasWeather) {
                blockWidth += (IdleStripLayout::kSlotGap * 2.0f +
                               IdleStripLayout::kDividerWidth) * scale +
                              idleMetrics.weatherWidth;
            }

            // Centre the block in whatever room the pill currently has. Mid-spring
            // the rect is wider or narrower than the natural width, and clamping
            // the slack at zero stops the slots from inverting when it is narrower.
            const float slack = std::max(0.0f, (innerRight - innerLeft) - blockWidth);
            float slotX = innerLeft + slack * 0.5f;

            // Each slot is exactly its measured width, and the measurement used the
            // widest-digit form, so the live string is centred inside a box it is
            // guaranteed to fit rather than drifting as the digits change.
            textBrush_->SetOpacity(0.96f);
            const D2D1_RECT_F timeRect =
                D2D1::RectF(slotX, rect.top, slotX + idleMetrics.clockWidth, rect.bottom);
            target_->DrawTextW(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), idleFmt,
                               timeRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            slotX += idleMetrics.clockWidth;

            if (idleMetrics.hasWeather) {
                slotX += IdleStripLayout::kSlotGap * scale;

                ComPtr<ID2D1SolidColorBrush> divider;
                target_->CreateSolidColorBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_),
                    &divider);
                if (divider) {
                    const float divW = IdleStripLayout::kDividerWidth * scale;
                    const float divTop = rect.top + IdleStripLayout::kDividerInsetY * scale;
                    const float divBottom = rect.bottom - IdleStripLayout::kDividerInsetY * scale;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(slotX, divTop, slotX + divW, divBottom),
                                          divW * 0.5f, divW * 0.5f), divider.Get());
                }
                slotX += (IdleStripLayout::kDividerWidth + IdleStripLayout::kSlotGap) * scale;

                wchar_t weatherLabel[32] = {};
                if (hasWeather) swprintf_s(weatherLabel, L"%s %.0f\x00B0", wIcon.c_str(), state.weather.temperature);
                else wcscpy_s(weatherLabel, ARRAYSIZE(weatherLabel), L"\U0001F321\uFE0F --\x00B0");

                const D2D1_RECT_F wRect =
                    D2D1::RectF(slotX, rect.top, slotX + idleMetrics.weatherWidth, rect.bottom);
                target_->DrawTextW(weatherLabel, static_cast<UINT32>(wcslen(weatherLabel)), idleFmt,
                                   wRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            }
            textBrush_->SetOpacity(1.0f);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha > 0.01f) {
            if (dashFadeLayer) {
                target_->PushLayer(D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                          D2D1::IdentityMatrix(), expandedAlpha, nullptr,
                                                          D2D1_LAYER_OPTIONS_NONE),
                                   dashFadeLayer);
            }

            // Expanded Mode. Order must match ActiveTabCount().
            std::vector<int> activeTabs;
            activeTabs.push_back(3); // Time (shown first when the island expands/on hover)
            activeTabs.push_back(0); // Calendar
            if (settings.weather) activeTabs.push_back(1);
            if (settings.hardwareMonitorModule) activeTabs.push_back(2);
            if (settings.stocksModule) activeTabs.push_back(4);  // custom fork (custom.18)

            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            // Lay the expanded view out at its final size, centred horizontally and
            // pinned to the top, so growing the pill only reveals it (same as media)
            // instead of re-centring and rescaling the content every frame.
            const float fullW = std::max(MediaLayout::kExpandedWidth, rect.right - rect.left);
            const float fullH = std::max(MediaLayout::kExpandedHeight, rect.bottom - rect.top);
            const float fullCx = (rect.left + rect.right) * 0.5f;
            const D2D1_RECT_F fullRect = D2D1::RectF(fullCx - fullW * 0.5f, rect.top,
                                                     fullCx + fullW * 0.5f, rect.top + fullH);

            DrawTabTransition(1, tabIdx, activeTabs, fullRect, now, [&](int activeTabId) {
                if (activeTabId == 3) DrawTimeDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 0) DrawCalendarDashboard(state, fullRect, settings, now, scale, local);
                else if (activeTabId == 1) DrawWeatherDashboard(state, fullRect, settings, now, scale, hasWeather, wIcon, wText);
                else if (activeTabId == 2) DrawHardwareMonitorDashboard(state, fullRect, settings, scale);
                else if (activeTabId == 4) DrawStocksDashboard(state, fullRect, settings, now, scale);
                // Custom fork, Feature 3: the Do Not Disturb toggle is part of the
                // first (Time) tab, so it slides in and out together with it
                // instead of popping in after the tab animation has finished.
                if (settings.quietButton && !activeTabs.empty() && activeTabId == activeTabs.front()) {
                    DrawQuietToggle(fullRect);
                }
                // Standalone 0.7: Bluetooth accessories + battery, top-left of the clock tab.
                if (settings.bluetoothDashboard && !activeTabs.empty() && activeTabId == activeTabs.front()) {
                    DrawBluetoothBadges(state, fullRect.left + 24.0f, fullRect.top + QuietToggleLayout::kCenterY,
                                        fullRect.right - 150.0f);
                }
            });

            DrawTabPagination(fullRect, maxTabs);

            if (dashFadeLayer) {
                target_->PopLayer();
            }
        }

        if (expandedAlpha <= 0.01f) {
            g_idleTab = 0;
            ResetTabTransition();
        }

        if (haveDashMask) target_->PopLayer(); else target_->PopAxisAlignedClip();
    }

    void DrawGameOverlay(const SharedState& state, D2D1_RECT_F rect, float unused_scale) {
        (void)unused_scale;
        const float scale = 1.0f;
        const bool compact = g_settings.gameOverlayCompact;
        const GameOverlayLayout::Metrics m = GameOverlayLayout::For(compact);

        const float cardTop = rect.top + m.padY;
        const float cardBottom = rect.bottom - m.padY;
        float cursorX = rect.left + m.padX;

        // ── FPS, given hero treatment ────────────────────────────────────────
        // Frame rate is the number a player actually watches, so it gets a wider
        // card and a larger figure than the percentages beside it. It has no
        // 0-100 ceiling, so it gets no load bar and keeps the plain accent.
        if (g_settings.gameOverlayShowFps) {
            const D2D1_RECT_F fpsCard = D2D1::RectF(cursorX, cardTop, cursorX + m.fpsW, cardBottom);
            DrawCard(fpsCard, m.radius);

            ComPtr<ID2D1SolidColorBrush> fpsIcon;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.accent, 0.90f * settingsOpacity_), &fpsIcon)) && fpsIcon) {
                DrawMetricGlyph(D2D1::Point2F(fpsCard.left + 15.0f * scale, fpsCard.top + 15.0f * scale),
                                15.0f * scale, 9, fpsIcon.Get());
            }

            const float textLeft = fpsCard.left + 28.0f * scale;
            const float textRight = fpsCard.right - 9.0f * scale;

            if (g_settings.showMetricText && smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                mutedBrush_->SetOpacity(0.58f);
                target_->DrawTextW(L"FPS", 3, smallTextFormat_.Get(),
                                   D2D1::RectF(textLeft, fpsCard.top + 2.0f * scale,
                                               textRight, fpsCard.top + 16.0f * scale),
                                   mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }

            wchar_t fpsValue[16] = {};
            swprintf_s(fpsValue, L"%d", state.system.renderFps);
            IDWriteTextFormat* fpsFmt = clockFormat_ ? clockFormat_.Get() : textFormat_.Get();
            if (fpsFmt) {
                // clockFormat_ is centre-aligned by default; the strip reads as a
                // left-aligned column, so override for this draw and restore.
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textBrush_->SetOpacity(0.97f);
                // Shares the label's left margin so the glyph sits in its own
                // column and the text column has one straight edge, rather than
                // the value hanging out to the left of the label above it.
                const float top = g_settings.showMetricText ? fpsCard.top + 16.0f * scale : cardTop;
                target_->DrawTextW(fpsValue, static_cast<UINT32>(wcslen(fpsValue)), fpsFmt,
                                   D2D1::RectF(textLeft, top,
                                               textRight, cardBottom - 3.0f * scale),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                fpsFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            cursorX = fpsCard.right + m.fpsGap;
        }

        // ── Load cards ───────────────────────────────────────────────────────
        // Laid out from whichever metrics are enabled (#25), so turning some off
        // closes the gap instead of leaving a hole. Icons come from the shared
        // DrawMetricGlyph family, so CPU here is the same symbol as CPU on the
        // hardware dashboard.
        struct GameCard {
            const wchar_t* label;
            int percent;
            int glyph;
            bool enabled;
        };
        const GameCard cards[] = {
            {L"CPU", state.system.cpuPercent,              3, g_settings.gameOverlayShowCpu},
            {L"RAM", state.system.memoryPercent,           4, g_settings.gameOverlayShowRam},
            {L"GPU", state.system.gpuPercent,              5, g_settings.gameOverlayShowGpu},
            {L"DISK", 100 - state.system.diskFreePercent,  8, g_settings.gameOverlayShowDisk},
        };

        for (const GameCard& card : cards) {
            if (!card.enabled) {
                continue;
            }
            if (cursorX + m.cardW > rect.right - m.padX + 0.5f) {
                break;  // ran out of room
            }
            DrawGameMetricCard(D2D1::RectF(cursorX, cardTop, cursorX + m.cardW, cardBottom),
                               card.label, card.percent, card.glyph, m.radius);
            cursorX += m.cardW + m.gap;
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawGameMetricCard(D2D1_RECT_F rect, const wchar_t* label, int percent, int glyph, float radius) {
        const float scale = 1.0f;

        // Same semantic load colour the hardware dashboard uses: accent while a
        // component is comfortable, amber under pressure, red when saturated.
        // This replaces a per-metric rainbow (cyan CPU, magenta RAM, green GPU,
        // orange disk) that encoded nothing and fought the album-art accent.
        const bool known = percent >= 0;
        const float pct = known ? Clamp(percent / 100.0f, 0.0f, 1.0f) : 0.0f;
        const D2D1_COLOR_F tint = known ? LoadStateColor(pct) : material_.accent;

        // Fill only. This card used to carry a hairline border *and* a
        // metric-coloured ring on top of it -- two bright outlines per card,
        // four cards across, which is the edge lighting this design drops.
        DrawCard(rect, radius);

        ComPtr<ID2D1SolidColorBrush> glyphBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(
                WithAlpha(tint, 0.85f * settingsOpacity_), &glyphBrush)) && glyphBrush) {
            DrawMetricGlyph(D2D1::Point2F(rect.left + 15.0f * scale, rect.top + 15.0f * scale),
                            14.0f * scale, glyph, glyphBrush.Get());
        }

        const float textLeft = rect.left + 28.0f * scale;
        const float textRight = rect.right - 8.0f * scale;

        // Label above value above bar, matching the hardware dashboard exactly.
        if (g_settings.showMetricText && smallTextFormat_) {
            smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            mutedBrush_->SetOpacity(0.58f);
            target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), smallTextFormat_.Get(),
                               D2D1::RectF(textLeft, rect.top + 2.0f * scale,
                                           textRight, rect.top + 16.0f * scale),
                               mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        wchar_t value[16] = {};
        if (known) {
            swprintf_s(value, L"%d%%", percent);
        } else {
            wcscpy_s(value, ARRAYSIZE(value), L"--");
        }

        if (textFormat_) {
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity(0.95f);
            // Aligned with the label above it, so the glyph owns the left column
            // and label/value share one straight edge.
            const float valueTop = g_settings.showMetricText ? rect.top + 17.0f * scale
                                                             : rect.top + 4.0f * scale;
            target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                               D2D1::RectF(textLeft, valueTop,
                                           textRight, rect.bottom - 8.0f * scale),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // The bar spans the card rather than just the text column: at this size it
        // reads as the card's own meter, and a 38px stub under the value did not.
        const D2D1_RECT_F track = D2D1::RectF(rect.left + 13.0f * scale, rect.bottom - 7.0f * scale,
                                              rect.right - 13.0f * scale, rect.bottom - 4.5f * scale);
        ComPtr<ID2D1SolidColorBrush> trackBrush;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(track, 1.25f * scale, 1.25f * scale), trackBrush.Get());
        }
        const float span = (track.right - track.left) * pct;
        if (span > 0.5f) {
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(tint, 0.95f), &fillBrush)) && fillBrush) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + span, track.bottom),
                                      1.25f * scale, 1.25f * scale),
                    fillBrush.Get());
            }
        }

        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    // ── Tab transition (spring-driven carousel) ─────────────────────────────
    // tabPos_ is a continuous "which tab is in view" value that a damped spring
    // chases toward the selected index. Every visible tab is drawn from its
    // distance to that position (slide + fade), so the motion is
    // interruptible, keeps its momentum on fast scrolling, and never restarts.
    void ResetTabTransition() {
        tabInit_ = false;
        tabAnimating_ = false;
        g_tabAnimating.store(false, std::memory_order_relaxed);
        g_tabEdgeBump.store(0, std::memory_order_relaxed);
    }

    template <typename DrawFn>
    void DrawTabTransition(int context, int tabIdx, const std::vector<int>& tabIds,
                           D2D1_RECT_F rect, double now, DrawFn&& drawTab) {
        const int count = static_cast<int>(tabIds.size());
        if (count <= 0) {
            ResetTabTransition();
            return;
        }
        tabIdx = ClampInt(tabIdx, 0, count - 1);

        // ── Feel: tweak these ──
        constexpr float kStiffness = 250.0f;        // higher = faster (settles in ~0.29s)
        constexpr float kDamping = 31.0f;           // ~0.92 damping ratio: no visible overshoot
        constexpr float kTravelPx = 34.0f;          // vertical gap between neighbouring tabs
        constexpr float kFadePower = 1.4f;          // >1 thins the mid-point slightly so text never ghosts

        float dt = static_cast<float>(now - tabLastDrawn_);
        tabLastDrawn_ = now;
        // First frame of a motion: the previous paint may be long ago, so don't
        // let a big gap swallow the start of the animation.
        dt = tabAnimating_ ? Clamp(dt, 0.001f, 0.05f) : (1.0f / 120.0f);

        // Snap (no animation) when the tab view was just opened, switched between
        // media/idle views, or the number of tabs changed.
        if (!tabInit_ || context != tabContext_ || count != tabCount_) {
            tabInit_ = true;
            tabContext_ = context;
            tabCount_ = count;
            tabPos_ = static_cast<float>(tabIdx);
            tabTarget_ = tabPos_;
            tabVel_ = 0.0f;
            tabDir_ = 1.0f;
            tabAnimating_ = false;
        }
        const float newTarget = static_cast<float>(tabIdx);
        if (newTarget != tabTarget_) {
            tabDir_ = newTarget > tabTarget_ ? 1.0f : -1.0f;  // remembered for the whole move
            tabTarget_ = newTarget;
        }

        // The first and last tab are hard stops: there is no edge bump any more.
        g_tabEdgeBump.store(0, std::memory_order_relaxed);

        if (tabPos_ != tabTarget_ || tabVel_ != 0.0f) {
            float remaining = dt;
            while (remaining > 0.0f) {
                const float h = std::min(remaining, 0.004f);
                tabVel_ += (-kStiffness * (tabPos_ - tabTarget_) - kDamping * tabVel_) * h;
                tabPos_ += tabVel_ * h;

                // Never travel past the first or last tab, and kill any velocity
                // pushing further out, so the edge tabs cannot overshoot or twitch.
                const float maxTabPos = static_cast<float>(count - 1);
                if (tabPos_ < 0.0f) {
                    tabPos_ = 0.0f;
                    if (tabVel_ < 0.0f) tabVel_ = 0.0f;
                } else if (tabPos_ > maxTabPos) {
                    tabPos_ = maxTabPos;
                    if (tabVel_ > 0.0f) tabVel_ = 0.0f;
                }
                remaining -= h;
            }
            if (std::fabs(tabPos_ - tabTarget_) < 0.0008f && std::fabs(tabVel_) < 0.02f) {
                tabPos_ = tabTarget_;
                tabVel_ = 0.0f;
            }
        }

        const bool moving = (tabPos_ != tabTarget_ || tabVel_ != 0.0f);
        tabAnimating_ = moving;
        g_tabAnimating.store(moving, std::memory_order_relaxed);

        if (!moving) {
            drawTab(tabIds[tabIdx]);
            return;
        }
        g_tabLastPaintAt.store(now, std::memory_order_relaxed);  // render loop keeps frames coming until it settles

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        if (!tabLayer_) {
            target_->CreateLayer(&tabLayer_);  // cached: re-creating it every frame is costly
        }

        // Every tab is a pure function of its distance to tabPos_, with no
        // "arriving" / "leaving" roles. Motion is therefore continuous even when you
        // reverse mid-scroll, and the tabs travel as one rigid strip (a fixed gap
        // apart), which is what reads as real scrolling rather than two fading cards.
        auto drawLayer = [&](int index) {
            if (index < 0 || index >= count) {
                return;
            }
            const float d = static_cast<float>(index) - tabPos_;  // <0 above, >0 below
            const float ad = Clamp(std::fabs(d), 0.0f, 1.0f);
            const float eased = SmoothStep01(ad);

            const float alpha = std::pow(1.0f - eased, kFadePower);
            if (alpha <= 0.004f) {
                return;
            }

            // Custom fork (custom.20): no scaling any more. The outgoing/incoming
            // tab used to shrink to 96.5 %, so numbers visibly "grew" back to size
            // as a tab arrived. Now the tab only slides and fades, at full size.
            // The offset is also rounded to whole screen pixels: at a fractional
            // offset DirectWrite drops its pixel-grid fitting, the glyphs come out
            // a touch smaller/softer, and they visibly snap back to size the
            // moment the tab stops. Whole pixels keep them identical throughout.
            float dy = d * kTravelPx;
            const float devScaleY = std::fabs(oldTransform._22) > 0.001f ? oldTransform._22 : 1.0f;
            dy = std::round(dy * devScaleY) / devScaleY;

            target_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, dy) * oldTransform);
            if (alpha < 0.995f && tabLayer_) {
                target_->PushLayer(
                    D2D1::LayerParameters(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), alpha, nullptr,
                                          D2D1_LAYER_OPTIONS_NONE),
                    tabLayer_.Get());
                drawTab(tabIds[index]);
                target_->PopLayer();
            } else {
                drawTab(tabIds[index]);  // fully opaque: skip the layer, much cheaper
            }
            target_->SetTransform(oldTransform);
        };

        // Farther tab first, so the one closer to centre ends up on top.
        const int lo = static_cast<int>(std::floor(tabPos_));
        if ((tabPos_ - static_cast<float>(lo)) < 0.5f) {
            drawLayer(lo + 1);
            drawLayer(lo);
        } else {
            drawLayer(lo);
            drawLayer(lo + 1);
        }
    }

    // Vertical pagination dots on the right edge. The active dot stretches into a
    // pill and slides with the transition instead of jumping.
    // Custom fork, Feature 3: Do Not Disturb toggle switch with a moon glyph.
    // Off: neutral track, knob left. On: accent track, knob right. The knob
    // glides between the two (quietKnob_), so turning it on reads clearly
    // before the island hides.
    void DrawQuietToggle(D2D1_RECT_F contentRect) {
        const bool on = g_quietMode.load();
        const double now = NowSeconds();
        const float target = on ? 1.0f : 0.0f;
        if (quietKnobAt_ <= 0.0 || now - quietKnobAt_ > 0.5) {
            quietKnob_ = target;  // first frame after a while: no stale glide
        } else {
            const float dt = static_cast<float>(now - quietKnobAt_);
            quietKnob_ += (target - quietKnob_) * std::min(1.0f, dt * 14.0f);
            if (std::fabs(target - quietKnob_) < 0.002f) quietKnob_ = target;
        }
        quietKnobAt_ = now;
        const float k = SmoothStep01(quietKnob_);

        const float trackRight = contentRect.right - QuietToggleLayout::kRightInset;
        const float trackLeft = trackRight - QuietToggleLayout::kTrackW;
        const float cy = contentRect.top + QuietToggleLayout::kCenterY;
        const float h = QuietToggleLayout::kTrackH;
        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(trackLeft, cy - h * 0.5f, trackRight, cy + h * 0.5f), h * 0.5f, h * 0.5f);

        D2D1_COLOR_F offColor = WithAlpha(material_.textPrimary, 0.16f);
        D2D1_COLOR_F onColor = accentBrush_ ? accentBrush_->GetColor() : D2D1::ColorF(0x7C5CFF);
        onColor.a = 0.95f;
        D2D1_COLOR_F trackColor = MixColor(offColor, onColor, k);
        trackColor.a *= settingsOpacity_;
        if (ID2D1SolidColorBrush* b = AcquireBrush(trackColor)) {
            target_->FillRoundedRectangle(track, b);
        }

        const float knobR = h * 0.5f - 2.5f;
        const float knobX = trackLeft + h * 0.5f + (QuietToggleLayout::kTrackW - h) * k;
        if (ID2D1SolidColorBrush* b = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * settingsOpacity_))) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), knobR, knobR), b);
        }

        if (iconFormat_) {
            const float moonRight = trackLeft - QuietToggleLayout::kMoonGap;
            const wchar_t* glyph = L"\uE708";  // QuietHours (moon), MDL2 and Fluent
            const DWRITE_TEXT_ALIGNMENT oldAlign = iconFormat_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT oldPara = iconFormat_->GetParagraphAlignment();
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            textBrush_->SetOpacity((0.55f + 0.40f * k) * settingsOpacity_);
            target_->DrawTextW(glyph, 1, iconFormat_.Get(),
                               D2D1::RectF(moonRight - QuietToggleLayout::kMoonW, cy - 11.0f, moonRight, cy + 11.0f),
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            // Standalone 0.4/0.5: round buttons -- settings (gear) and power. A thin
            // ring and a faint fill show they can be pressed; icons at 80 % size.
            const float moonLeft = moonRight - QuietToggleLayout::kMoonW;
            const wchar_t* glyphs[2] = {L"\uE713", L"\uE7E8"};  // Settings, PowerButton (MDL2 / Fluent)
            const float radius = QuietToggleLayout::kBtnD * 0.5f;
            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);
            for (int i = 0; i < 2; ++i) {
                const float bx = QuietToggleLayout::ButtonCenterX(moonLeft, i);
                const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(bx, cy), radius, radius);
                if (ID2D1SolidColorBrush* fill = AcquireBrush(WithAlpha(material_.textPrimary, 0.07f * settingsOpacity_))) {
                    target_->FillEllipse(circle, fill);
                }
                if (ID2D1SolidColorBrush* ring = AcquireBrush(WithAlpha(material_.textPrimary, 0.28f * settingsOpacity_))) {
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(bx, cy), radius - 0.5f, radius - 0.5f), ring, 1.0f);
                }
                target_->SetTransform(D2D1::Matrix3x2F::Scale(0.8f, 0.8f, D2D1::Point2F(bx, cy)) * oldTransform);
                textBrush_->SetOpacity(0.85f * settingsOpacity_);
                target_->DrawTextW(glyphs[i], 1, iconFormat_.Get(),
                                   D2D1::RectF(bx - 12.0f, cy - 12.0f, bx + 12.0f, cy + 12.0f),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                target_->SetTransform(oldTransform);
            }
            textBrush_->SetOpacity(1.0f);
            iconFormat_->SetTextAlignment(oldAlign);
            iconFormat_->SetParagraphAlignment(oldPara);
        }
    }
    float quietKnob_ = 0.0f;
    double quietKnobAt_ = 0.0;
  public:
    // Custom fork, Feature 3: Do Not Disturb shrink in progress; w/h = the size
    // the frame is laid out at (squeezed onto the current pill size).
    void SetQuietSqueeze(bool active, float w, float h) {
        quietSqueeze_ = active;
        quietSqueezeW_ = w;
        quietSqueezeH_ = h;
    }
  private:
    bool quietSqueeze_ = false;
    float quietSqueezeW_ = 0.0f;
    float quietSqueezeH_ = 0.0f;

    void DrawTabPagination(D2D1_RECT_F rect, int tabCount) {
        if (tabCount <= 1) {
            return;
        }
        ID2D1SolidColorBrush* dotBrush = AcquireBrush(WithAlpha(material_.textPrimary, 1.0f));
        if (!dotBrush) {
            return;
        }

        const float dotX = rect.right - 10.0f;
        const float spacing = 9.0f;
        const float r = 2.5f;
        const float startY = (rect.top + rect.bottom) * 0.5f - spacing * (tabCount - 1) * 0.5f;

        // Resting dots. The one under the indicator fades out, so the pill reads
        // as travelling over the track instead of sitting on top of a dot.
        for (int i = 0; i < tabCount; ++i) {
            const float under = SmoothStep01(Clamp(1.0f - std::fabs(tabPos_ - static_cast<float>(i)), 0.0f, 1.0f));
            dotBrush->SetOpacity(0.22f * (1.0f - under) * settingsOpacity_);
            target_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(dotX, startY + spacing * static_cast<float>(i)), r, r),
                dotBrush);
        }

        // Travelling indicator: follows the spring exactly (so it also gives a little
        // at the ends) and stretches slightly with speed.
        const float pos = Clamp(tabPos_, -0.3f, static_cast<float>(tabCount) - 0.7f);
        const float stretch = Clamp(std::fabs(tabVel_) * 0.55f, 0.0f, 3.5f);
        const float halfH = r + 2.2f + stretch;
        const float cy = startY + spacing * pos;
        dotBrush->SetOpacity(0.90f * settingsOpacity_);
        target_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(dotX - r, cy - halfH, dotX + r, cy + halfH), r, r),
            dotBrush);
    }

    // Publishes the content-space rect currently being painted so
    // OverlayWndProc hit-tests against the real geometry rather than assuming
    // the pill is centered in the client area. Seqlock write: bump to odd,
    // store, bump to even, so a reader can never mix two frames.
    void PublishContentGeometry(D2D1_RECT_F rect) {
        const unsigned seq = g_mediaHitSeq.load(std::memory_order_relaxed);
        g_mediaHitSeq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        g_mediaHitLeft.store(rect.left, std::memory_order_relaxed);
        g_mediaHitTop.store(rect.top, std::memory_order_relaxed);
        g_mediaHitRight.store(rect.right, std::memory_order_relaxed);
        g_mediaHitBottom.store(rect.bottom, std::memory_order_relaxed);
        g_mediaHitScale.store(g_settings.sizeScale, std::memory_order_relaxed);
        g_mediaHitStamp.store(GetTickCount64(), std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        g_mediaHitSeq.store(seq + 2, std::memory_order_relaxed);
    }

    // Corner radius for the island mask in *content* space -- the coordinate
    // system DrawMedia and DrawIdleDashboard are handed, which DrawPill scales
    // by sizeScale afterwards. Because that scale is applied later, these radii
    // must not be pre-multiplied by sizeScale. They previously were, so the
    // stadium corner arc grew with the size scale until it cut across the album
    // art's top-left corner (visible from roughly 2x upwards).
    float ContentIslandRadius(float contentHeight) const {
        if (g_settings.notchStyle) {
            return 16.0f;
        }
        if (g_settings.w11Style) {
            return 8.0f;
        }
        return std::min(contentHeight * 0.5f, 44.0f);
    }

    // Takes settings by reference from Render()'s private copy rather than
    // reaching for g_settings. The dashboards it delegates to read std::wstring
    // members (DrawCalendarDashboard -> settings.dateFormat), which would
    // otherwise race with LoadSettings() replacing the struct mid-frame.
    void DrawMedia(const SharedState& state, D2D1_RECT_F rect, const Settings& settings,
                   double now) {
        const float height = rect.bottom - rect.top;

        // Island resize tracking: "settled" once the size has stopped changing for 0.15s.
        // Small per-frame drifts accumulate against the last latched size, so the
        // spring's tail can't sneak past the check.
        {
            const float curW = rect.right - rect.left;
            if (std::fabs(curW - islandSizeLastW_) > 0.25f ||
                std::fabs(height - islandSizeLastH_) > 0.25f) {
                islandSizeLastW_ = curW;
                islandSizeLastH_ = height;
                islandSizeMovedAt_ = now;
            }
            islandSettled_ = (now - islandSizeMovedAt_) > 0.15;
        }


        PublishContentGeometry(rect);

        const float radius = ContentIslandRadius(height);
        ComPtr<ID2D1Geometry> mask = CreateIslandMaskGeometry(rect, radius, settings.notchStyle);
        ID2D1Layer* layer = AcquireLayer();

        float expandedAlpha = std::clamp((height - MediaLayout::kExpandedMinHeight) / 60.0f, 0.0f, 1.0f);
        float collapsedAlpha = std::clamp((80.0f - height) / 30.0f, 0.0f, 1.0f);

        // ── Shared album art ─────────────────────────────────────────────────
        // One art element that morphs between the collapsed geometry (small circle
        // on the left) and the expanded geometry (64px rounded square) as the island
        // grows or shrinks, instead of cross-fading two different shapes.
        // artMorph: 0 = collapsed pill, 1 = fully expanded (reached at 120px, where
        // the expanded layer is already fully opaque and takes over drawing the art).
        constexpr float kCollapsedPillHeight = 44.0f;
        constexpr float kArtMorphEndHeight = 120.0f;
        constexpr float kCollapsedArtPad = 6.0f;
        constexpr float kCollapsedArtSize = kCollapsedPillHeight - kCollapsedArtPad * 2.0f;
        // Raw progress straight from the island's height.
        const float artMorphRaw = Clamp((height - kCollapsedPillHeight) /
                                        (kArtMorphEndHeight - kCollapsedPillHeight), 0.0f, 1.0f);

        // Shrinking follows the height exactly (unchanged). Expanding is not
        // allowed to run faster than kArtExpandSec for the full circle -> square
        // change, because the expand spring is so fast that the morph would
        // otherwise be over in a few frames.
        constexpr float kArtExpandSec = 0.26f;
        // The art only travels to its expanded spot on the Media tab. On any other
        // first tab it just follows the height, with no throttle.
        const bool artOnMediaTab = NormalizedTabIndex(settings) == 0;
        const bool freshMorph = artMorphLast_ >= 0.0 && (now - artMorphLast_) < 0.25;
        const float dtMorph = freshMorph ? Clamp(static_cast<float>(now - artMorphLast_), 0.001f, 0.05f) : 0.0f;
        artMorphLast_ = now;

        if (!freshMorph || artMorphRaw <= artMorphLin_ || !artOnMediaTab) {
            artMorphLin_ = artMorphRaw;
        } else {
            artMorphLin_ = std::min(artMorphRaw, artMorphLin_ + dtMorph / kArtExpandSec);
            g_layoutDirty = true;  // keep frames coming until the morph catches up
        }
        const float artMorph = SmoothStep01(artMorphLin_);

        // Source badge: hidden during the morph, fades in once the art hands over
        // to the expanded layer (this used to pop in instantly).
        if (artMorph >= 1.0f) {
            if (!freshMorph) {
                artBadgeAlpha_ = 1.0f;
            } else if (artBadgeAlpha_ < 1.0f) {
                artBadgeAlpha_ = std::min(1.0f, artBadgeAlpha_ + dtMorph / 0.20f);
                g_layoutDirty = true;
            }
        } else {
            artBadgeAlpha_ = 0.0f;
        }
        float pillClockSection = 0.0f;
        if (settings.mediaPillClock && idleTextFormat_) {
            pillClockSection = MeasureMediaPill(state, settings).sectionWidth;
        }
        const D2D1_RECT_F collapsedArtRect = D2D1::RectF(
            rect.left + pillClockSection + kCollapsedArtPad, rect.top + kCollapsedArtPad,
            rect.left + pillClockSection + kCollapsedArtPad + kCollapsedArtSize,
            rect.top + kCollapsedArtPad + kCollapsedArtSize);
        auto lerpF = [](float a, float b, float t) { return a + (b - a) * t; };
        const D2D1_RECT_F expandedArtRect = D2D1::RectF(
            rect.left + MediaLayout::kArtInsetX, rect.top + MediaLayout::kArtInsetY,
            rect.left + MediaLayout::kArtInsetX + MediaLayout::kArtSize,
            rect.top + MediaLayout::kArtInsetY + MediaLayout::kArtSize);
        const float expandedArtRadius = 16.0f;
        const D2D1_RECT_F morphArtRect = D2D1::RectF(
            lerpF(collapsedArtRect.left, expandedArtRect.left, artMorph),
            lerpF(collapsedArtRect.top, expandedArtRect.top, artMorph),
            lerpF(collapsedArtRect.right, expandedArtRect.right, artMorph),
            lerpF(collapsedArtRect.bottom, expandedArtRect.bottom, artMorph));
        const float morphArtRadius = lerpF(kCollapsedArtSize * 0.5f, expandedArtRadius, artMorph);

        // Expanded UI
        if (expandedAlpha > 0.01f && mask && layer) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), expandedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);

            // Order must match ActiveTabCount().
            std::vector<int> activeTabs;
            activeTabs.push_back(0); // Media
            activeTabs.push_back(1); // Calendar
            if (settings.weather) activeTabs.push_back(2);
            if (settings.hardwareMonitorModule) activeTabs.push_back(3);
            if (settings.stocksModule) activeTabs.push_back(4);  // custom fork (custom.18)

            // Same settings object as the list above, so the index can't be
            // normalised against a tab set that no longer matches.
            int maxTabs = static_cast<int>(activeTabs.size());
            int tabIdx = NormalizedTabIndex(settings);
            if (tabIdx >= maxTabs) tabIdx = maxTabs - 1;

            auto drawTabContent = [&](int activeTabId) {
            if (activeTabId == 0) {
                {
                // Expanded Apple DI media: large square art on left, text center.
                const float artSize = MediaLayout::kArtSize;
                D2D1_RECT_F artRect = D2D1::RectF(rect.left + MediaLayout::kArtInsetX,
                                                  rect.top + MediaLayout::kArtInsetY,
                                                  rect.left + MediaLayout::kArtInsetX + artSize,
                                                  rect.top + MediaLayout::kArtInsetY + artSize);
                // Until the morph has finished, the shared art pass at the bottom of
                // DrawMedia owns the cover, so it is never drawn twice.
                if (artMorph >= 1.0f) {
                    DrawAlbumArt(state.media, expandedArtRect, now, expandedArtRadius, true);
                }

                {

                const float waveW = 32.0f;
                const float waveH = 20.0f;
                D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 24.0f - waveW,
                                                   rect.top + 20.0f + (artSize - waveH) * 0.5f,
                                                   rect.right - 24.0f,
                                                   rect.top + 20.0f + (artSize + waveH) * 0.5f);

                const float textLeft = artRect.right + 18.0f;
                const float textRight = waveRect.left - 16.0f;

                // Title — bold, prominent.
                D2D1_RECT_F titleRect = D2D1::RectF(textLeft, rect.top + 34.0f, textRight, rect.top + 54.0f);
                const std::wstring titleText =
                    state.media.title.empty() ? std::wstring(Loc(L"Unknown")) : state.media.title;
                DrawMarqueeText(titleText, titleRect, textFormat_.Get(), textBrush_.Get(), now, 42.0f,
                                marqueeTitleCache_);

                // Artist — muted below title.
                D2D1_RECT_F artistRect = D2D1::RectF(textLeft, rect.top + 54.0f, textRight, rect.top + 74.0f);
                mutedBrush_->SetOpacity(0.80f);
                DrawMarqueeText(state.media.artist, artistRect, smallTextFormat_.Get(),
                                mutedBrush_.Get(), now, 30.0f, marqueeArtistCache_);
                mutedBrush_->SetOpacity(0.75f);

                if (!state.media.albumTitle.empty()) {
                    D2D1_RECT_F albumRect = D2D1::RectF(textLeft, rect.top + 68.0f, textRight, rect.top + 84.0f);
                    mutedBrush_->SetOpacity(0.70f);
                    DrawMarqueeText(state.media.albumTitle, albumRect, smallTextFormat_.Get(),
                                    mutedBrush_.Get(), now, 28.0f, marqueeAlbumCache_);
                    mutedBrush_->SetOpacity(0.75f);
                }

                if (state.media.playing) {
                    DrawSpectrum(state, waveRect, settings, now);
                } else {
                    const float gap = 2.5f;
                    const float availableW = waveRect.right - waveRect.left;
                    const int count = 7;
                    const float barWidth = (availableW - gap * (count - 1)) / count;
                    const float centerY = (waveRect.top + waveRect.bottom) * 0.5f;
                    mutedBrush_->SetOpacity(0.5f);
                    for (int i = 0; i < count; ++i) {
                        const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, centerY), 1.2f, 1.2f), mutedBrush_.Get());
                    }
                }

                // Source line, under the artist (or under the album when there is one).
                const float srcLineTop = rect.top + (state.media.albumTitle.empty() ? 70.0f : 85.0f);
                DrawSourceLine(state.media, D2D1::RectF(textLeft, srcLineTop, textRight, srcLineTop + 14.0f), now);
                }
                }

                // Timeline (Scrubber)
                const float scrubberY = rect.top + MediaLayout::kScrubberY;
                double currentPosition = state.media.positionTicks / 10000000.0;
                double duration = state.media.endTicks / 10000000.0;
                if (state.media.playing && state.media.lastUpdatedTicks > 0) {
                    currentPosition += (GetTickCount64() - state.media.lastUpdatedTicks) / 1000.0;
                }
                currentPosition = std::max(0.0, std::min(currentPosition, duration));

                const bool isDraggingThisBar = g_scrubbing.load(std::memory_order_relaxed);
                float progress;
                if (isDraggingThisBar) {
                    progress = Clamp(g_scrubDragFraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    currentPosition = duration * progress;
                } else {
                    progress = duration > 0.0 ? static_cast<float>(currentPosition / duration) : 0.0f;
                }

                auto FormatTime = [](double seconds) -> std::wstring {
                    if (seconds <= 0.0 || _isnan(seconds)) return L"0:00";
                    int m = static_cast<int>(seconds) / 60;
                    int s = static_cast<int>(seconds) % 60;
                    wchar_t buf[16];
                    swprintf_s(buf, L"%d:%02d", m, s);
                    return buf;
                };

                std::wstring elapsedStr = FormatTime(currentPosition);
                std::wstring remainStr = L"-" + FormatTime(duration - currentPosition);

                const float scrubLeft = rect.left + MediaLayout::kScrubMargin;
                const float scrubRight = rect.right - MediaLayout::kScrubMargin;

                const float barLeft = scrubLeft + MediaLayout::kScrubBarLeftInset;
                const float barRight = scrubRight - MediaLayout::kScrubBarRightInset;
                const float timeGap = 8.0f;

                D2D1_RECT_F elRect = D2D1::RectF(scrubLeft, scrubberY - 10.0f, barLeft - timeGap, scrubberY + 10.0f);
                D2D1_RECT_F remRect = D2D1::RectF(barRight + timeGap, scrubberY - 10.0f, scrubRight, scrubberY + 10.0f);

                mutedBrush_->SetOpacity(0.8f);
                if (smallTextFormat_) {
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    target_->DrawTextW(elapsedStr.c_str(), static_cast<UINT32>(elapsedStr.size()), smallTextFormat_.Get(), elRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    target_->DrawTextW(remainStr.c_str(), static_cast<UINT32>(remainStr.size()), smallTextFormat_.Get(), remRect, mutedBrush_.Get());

                    smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }

                // The scrubber uses the shared accent track, so it matches the
                // volume, battery and timer bars exactly. The bar thickens while
                // dragging, which is the standard cue that it is grabbable.
                if (settings.progressStyle != ProgressStyle::Slim) {
                    DrawStyledProgress(settings.progressStyle, barLeft, barRight, scrubberY, progress,
                                       isDraggingThisBar, state.media.playing, now);
                } else {
                const float barHalf = isDraggingThisBar ? 3.5f : 2.5f;
                DrawAccentTrack(D2D1::RectF(barLeft, scrubberY - barHalf, barRight, scrubberY + barHalf),
                                progress, barHalf);

                const D2D1_COLOR_F scrubColor =
                    (currentAccent_.a > 0.0f) ? currentAccent_ : D2D1::ColorF(0x4cc9f0);
                const float scrubW = (barRight - barLeft) * progress;
                const float thumbX = barLeft + scrubW;
                const float thumbR = isDraggingThisBar ? 6.5f : 4.5f;

                // Halo first so the thumb sits on top of it.
                if (isDraggingThisBar) {
                    ComPtr<ID2D1SolidColorBrush> thumbHalo;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.22f), &thumbHalo)) &&
                        thumbHalo) {
                        target_->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR * 2.2f, thumbR * 2.2f),
                            thumbHalo.Get());
                    }
                }

                // A white thumb with an accent ring reads more precisely against
                // album art than a solid accent dot.
                ComPtr<ID2D1SolidColorBrush> thumbFill;
                if (SUCCEEDED(target_->CreateSolidColorBrush(
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), &thumbFill)) && thumbFill) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbFill.Get());
                }
                ComPtr<ID2D1SolidColorBrush> thumbRing;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(scrubColor, 0.85f), &thumbRing)) &&
                    thumbRing) {
                    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, scrubberY), thumbR, thumbR),
                                         thumbRing.Get(), 1.6f);
                }
                }  // end Slim style

                // Controls. Positions come from MediaLayout so the hit test in
                // OverlayWndProc stays in lockstep with what is drawn here.
                const float cy = rect.top + MediaLayout::kControlsY;
                const float cx = (rect.left + rect.right) * 0.5f;
                DrawMediaControls(state.media.playing,
                                  D2D1::Point2F(cx - MediaLayout::kControlSpacing, cy),
                                  D2D1::Point2F(cx, cy),
                                  D2D1::Point2F(cx + MediaLayout::kControlSpacing, cy),
                                  now);
                DrawSourceDock(state, rect, now);
            } else if (activeTabId == 1) {
                SYSTEMTIME local = {}; GetLocalTime(&local);
                DrawCalendarDashboard(state, rect, settings, now, 1.0f, local);
            } else if (activeTabId == 2) {
                bool hasWeather = state.weather.hasData && (now - state.weather.lastUpdated < 3600.0);
                std::wstring wIcon = L"🌡️"; std::wstring wText = Loc(L"Loading...");
                if (hasWeather) {
                    wText = state.weather.weatherDesc;
                    GetWeatherIconAndText(state.weather.weatherCode, wIcon, wText);
                }
                DrawWeatherDashboard(state, rect, settings, now, 1.0f, hasWeather, wIcon, wText);
            } else if (activeTabId == 3) {
                DrawHardwareMonitorDashboard(state, rect, settings, 1.0f);
            } else if (activeTabId == 4) {
                DrawStocksDashboard(state, rect, settings, now, 1.0f);
            }
            };  // end drawTabContent

            // Custom fork, Feature 3: the same toggle on the player's first tab
            // (so it does not vanish when music starts), drawn as part of that
            // tab so it moves with it.
            DrawTabTransition(0, tabIdx, activeTabs, rect, now, [&](int activeTabId) {
                drawTabContent(activeTabId);
                if (settings.quietButton && !activeTabs.empty() &&
                    activeTabId == activeTabs.front()) {
                    DrawQuietToggle(rect);
                }
                // Standalone 0.7: the same badge on the player tab, right of the cover.
                if (settings.bluetoothDashboard && !activeTabs.empty() && activeTabId == activeTabs.front()) {
                    DrawBluetoothBadges(state,
                                        rect.left + MediaLayout::kArtInsetX + MediaLayout::kArtSize + 16.0f,
                                        rect.top + QuietToggleLayout::kCenterY - 6.0f, rect.right - 150.0f);
                }
            });
            DrawTabPagination(rect, maxTabs);

            target_->PopLayer();
        }

        // Collapsed UI
        if (collapsedAlpha > 0.01f && mask && layer) {
            g_idleTab = 0;
            ResetTabTransition();
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), collapsedAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE), layer);;

            const float cy = (rect.top + rect.bottom) * 0.5f;

            // Optional clock section on the left. Zero wide when the option is
            // off, so the cover sits exactly where it always did.
            MediaPillMetrics pillMetrics;
            if (settings.mediaPillClock && idleTextFormat_) {
                pillMetrics = MeasureMediaPill(state, settings);
            }
            const float clockSection = pillMetrics.sectionWidth;

            // The cover itself is drawn by the shared art pass at the end of DrawMedia.

            // Standalone 0.6: the mic/camera dot is not shown while the player is,
            // so the spectrum keeps its place.
            D2D1_RECT_F waveRect = D2D1::RectF(rect.right - 42.0f, cy - 10.0f,
                                               rect.right - 14.0f, cy + 10.0f);
            if (state.media.playing) {
                DrawSpectrum(state, waveRect, settings, now);
            } else {
                const float gap = 2.5f;
                const float availableW = waveRect.right - waveRect.left;
                const int count = std::max(1, static_cast<int>((availableW + gap) / (2.0f + gap)));
                const float barWidth = (availableW - gap * (count - 1)) / count;
                mutedBrush_->SetOpacity(0.5f);
                for (int i = 0; i < count; ++i) {
                    const float dotX = waveRect.left + i * (barWidth + gap) + barWidth * 0.5f;
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, cy), 1.2f, 1.2f), mutedBrush_.Get());
                }
            }

            // Clock + divider on the left, same face, size and time format as the
            // idle strip. On hover the collapsed layer fades out and the normal
            // player shows.
            if (clockSection > 0.0f) {
                SYSTEMTIME local = {};
                GetLocalTime(&local);
                const std::wstring clockText = FormatIslandTime(local, settings.clockFollowSystem,
                                                                settings.use24HourClock,
                                                                settings.showSeconds);
                const float clockLeft = rect.left + MediaPillLayout::kClockPadLeft;
                textBrush_->SetOpacity(0.96f);
                target_->DrawTextW(clockText.c_str(), static_cast<UINT32>(clockText.size()),
                                   idleTextFormat_.Get(),
                                   D2D1::RectF(clockLeft, rect.top, clockLeft + pillMetrics.clockWidth, rect.bottom),
                                   textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textBrush_->SetOpacity(1.0f);

                ID2D1SolidColorBrush* divider = AcquireBrush(
                    WithAlpha(material_.hairline, material_.hairline.a * settingsOpacity_));  // per-frame pool, no allocation
                if (divider) {
                    const float divX = clockLeft + pillMetrics.clockWidth + MediaPillLayout::kSlotGap;
                    const float divW = MediaPillLayout::kDividerWidth;
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(divX, rect.top + MediaPillLayout::kDividerInsetY,
                                                      divX + divW, rect.bottom - MediaPillLayout::kDividerInsetY),
                                          divW * 0.5f, divW * 0.5f),
                        divider);
                }
            }

            target_->PopLayer();
        }

        // ── Shared album art pass ────────────────────────────────────────────
        // Drawn once, outside both cross-fading layers, at full opacity, so the cover
        // morphs smoothly in position, size and corner radius while the island
        // resizes. Once fully expanded, the expanded layer takes over (source badge). Alpha is eased so switching tabs can't make it pop in.
        {
            const bool onMediaTab = NormalizedTabIndex(settings) == 0;
            const float targetAlpha = onMediaTab ? 1.0f : collapsedAlpha;

            const bool recent = artOuterLast_ >= 0.0 && (now - artOuterLast_) < 0.25;
            const float dtArt = recent ? Clamp(static_cast<float>(now - artOuterLast_), 0.001f, 0.05f) : 0.016f;
            artOuterLast_ = now;
            // Off the Media tab the art has no business following a slow ease; it
            // fades exactly with the collapsed pill's contents.
            if (!recent || artMorph >= 1.0f || !onMediaTab) {
                artOuterAlpha_ = targetAlpha;
            } else {
                artOuterAlpha_ += (targetAlpha - artOuterAlpha_) * (1.0f - std::exp(-dtArt / 0.06f));
            }

            // While the island is hiding, the art follows the pill: it shrinks
            // toward the pill's centre and fades, instead of staying 32px and
            // sticking out until the window stops painting.
            float hideS = 1.0f;
            if (islandHiding_) {
                const float needW = pillClockSection + kCollapsedArtPad * 2.0f + kCollapsedArtSize;
                const float curW = rect.right - rect.left;
                hideS = SmoothStep01(std::min(height / kCollapsedPillHeight, curW / needW));
            }
            // On the Media tab the cover morphs circle -> square. On any other tab it
            // stays a small circle on the collapsed pill and never flies into the
            // expanded layout, so it can't show up on top of another tab.
            const D2D1_RECT_F artBaseRect = onMediaTab ? morphArtRect : collapsedArtRect;
            const float artBaseRadius = onMediaTab ? morphArtRadius : kCollapsedArtSize * 0.5f;

            D2D1_RECT_F artDrawRect = artBaseRect;
            float artDrawRadius = artBaseRadius;
            if (hideS < 1.0f) {
                const float pcx = (rect.left + rect.right) * 0.5f;
                const float pcy = (rect.top + rect.bottom) * 0.5f;
                const float acx = (artBaseRect.left + artBaseRect.right) * 0.5f;
                const float acy = (artBaseRect.top + artBaseRect.bottom) * 0.5f;
                const float ncx = pcx + (acx - pcx) * hideS;
                const float ncy = pcy + (acy - pcy) * hideS;
                const float hw = (artBaseRect.right - artBaseRect.left) * 0.5f * hideS;
                const float hh = (artBaseRect.bottom - artBaseRect.top) * 0.5f * hideS;
                artDrawRect = D2D1::RectF(ncx - hw, ncy - hh, ncx + hw, ncy + hh);
                artDrawRadius = artBaseRadius * hideS;
            }
            const float artAlphaEff = artOuterAlpha_ * hideS;

            if (artMorph < 1.0f && artAlphaEff > 0.01f) {
                ID2D1Layer* artLayer = (artAlphaEff < 0.99f) ? AcquireLayer() : nullptr;
                if (artLayer) {
                    target_->PushLayer(
                        D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                              Clamp(artAlphaEff, 0.0f, 1.0f), nullptr,
                                              D2D1_LAYER_OPTIONS_NONE),
                        artLayer);
                }
                DrawAlbumArt(state.media, artDrawRect, now, artDrawRadius, false);
                if (artLayer) {
                    target_->PopLayer();
                }
            }
        }
    }

    // "Playing from Brave" / "Paused in Spotify", 11px, tertiary text.
    void DrawSourceLine(const MediaSnapshot& media, D2D1_RECT_F area, double now) {
        if (media.sourceName.empty() || !smallTextFormat_) return;
        const std::wstring text =
            FormatWithName(Loc(media.playing ? L"Playing from %s" : L"Paused in %s"), media.sourceName);
        if (!scratchColorBrush_) {
            target_->CreateSolidColorBrush(material_.textTertiary, &scratchColorBrush_);
        } else {
            scratchColorBrush_->SetColor(material_.textTertiary);
        }
        if (!scratchColorBrush_) return;
        DrawMarqueeText(text, area, smallTextFormat_.Get(), scratchColorBrush_.Get(), now, 26.0f,
                        marqueeSourceCache_);
    }

    // One rounded pill, one circular slot per source. Shown only with 2+ sources.
    void DrawSourceDock(const SharedState& state, D2D1_RECT_F rect, double now) {
        const int sourceCount = static_cast<int>(state.mediaSources.size());
        if (sourceCount < 2) {
            dockInit_ = false;
            return;
        }

        const int liveIdx = ActiveMediaSourceIndex(state);
        const DockSlots slots = ComputeDockSlots(sourceCount, liveIdx);  // same inputs as the hit test
        if (slots.count < 2) return;

        const int shownIdx = liveIdx;
        auto slotOf = [&](int sourceIdx) {
            for (int s = 0; s < slots.count; ++s) {
                if (slots.sourceIndex[s] == sourceIdx) return s;
            }
            return -1;
        };
        int targetSlot = slotOf(shownIdx);
        if (targetSlot < 0) targetSlot = slotOf(liveIdx);
        if (targetSlot < 0) targetSlot = 0;

        // ── Animation state ──────────────────────────────────────────────────
        const bool fresh = dockInit_ && dockLastDrawn_ >= 0.0 && (now - dockLastDrawn_) < 0.25 &&
                           dockSlotCount_ == slots.count;
        const float dt = fresh ? Clamp(static_cast<float>(now - dockLastDrawn_), 0.001f, 0.05f) : 0.016f;
        dockLastDrawn_ = now;

        if (!fresh) {
            dockInit_ = true;
            dockSlotCount_ = slots.count;
            dockIndPos_ = dockIndFrom_ = dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = -1.0;
            for (int i = 0; i < MediaLayout::kDockMaxSlots; ++i) {
                dockHover_[i] = 0.0f;
                dockPress_[i] = 0.0f;
            }
        } else if (static_cast<float>(targetSlot) != dockIndTarget_) {
            dockIndFrom_ = dockIndPos_;
            dockIndTarget_ = static_cast<float>(targetSlot);
            dockIndStart_ = now;
        }
        if (dockIndStart_ >= 0.0) {
            const float p = Clamp(static_cast<float>((now - dockIndStart_) / MediaLayout::kDockSlideSec), 0.0f, 1.0f);
            dockIndPos_ = dockIndFrom_ + (dockIndTarget_ - dockIndFrom_) * SkipEaseInOut(p);
            if (p >= 1.0f) {
                dockIndPos_ = dockIndTarget_;
                dockIndStart_ = -1.0;
            }
            g_layoutDirty = true;
        }

        const int hovered = g_hoveredSourceSlot.load(std::memory_order_relaxed);
        const int pressed = g_pressedSourceSlot.load(std::memory_order_relaxed);
        bool moving = false;
        for (int i = 0; i < slots.count; ++i) {
            const float hoverTarget = (i == hovered) ? 1.0f : 0.0f;
            dockHover_[i] += (hoverTarget - dockHover_[i]) * (1.0f - std::exp(-dt / 0.09f));
            if (std::fabs(hoverTarget - dockHover_[i]) < 0.004f) dockHover_[i] = hoverTarget; else moving = true;

            const float pressTarget = (i == pressed) ? 1.0f : 0.0f;
            const float tau = pressTarget > dockPress_[i] ? 0.025f : 0.075f;  // same feel as mediaBtnPress_
            dockPress_[i] += (pressTarget - dockPress_[i]) * (1.0f - std::exp(-dt / tau));
            if (std::fabs(pressTarget - dockPress_[i]) < 0.004f) dockPress_[i] = pressTarget; else moving = true;
        }
        if (moving) g_layoutDirty = true;

        // ── Drawing ──────────────────────────────────────────────────────────
        const float cy = rect.top + MediaLayout::kControlsY;
        const float x0 = rect.left + MediaLayout::kDockLeft;
        const D2D1_RECT_F pill = D2D1::RectF(x0, cy - MediaLayout::kDockHeight * 0.5f,
                                             x0 + MediaLayout::DockWidth(slots.count),
                                             cy + MediaLayout::kDockHeight * 0.5f);
        DrawCard(pill, MediaLayout::kDockHeight * 0.5f);

        // The pill's real colour (raised over the island base), used for the dot's ring.
        const D2D1_COLOR_F pillBg = MixColor(WithAlpha(material_.base, 1.0f),
                                             WithAlpha(material_.raised, 1.0f), material_.raised.a);

        // Active indicator: slides between slots.
        const float stride = MediaLayout::kDockSlot + MediaLayout::kDockGap;
        const float indCx = rect.left + MediaLayout::DockSlotLeft(0) + dockIndPos_ * stride +
                            MediaLayout::kDockSlot * 0.5f;
        const float indR = MediaLayout::kDockSlot * 0.5f;
        ComPtr<ID2D1SolidColorBrush> indFill;
        if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &indFill)) && indFill) {
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR, indR), indFill.Get());
        }
        accentBrush_->SetOpacity(1.0f);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(indCx, cy), indR - 0.75f, indR - 0.75f),
                             accentBrush_.Get(), 1.5f);

        for (int i = 0; i < slots.count; ++i) {
            const D2D1_POINT_2F c = D2D1::Point2F(
                rect.left + MediaLayout::DockSlotLeft(i) + MediaLayout::kDockSlot * 0.5f, cy);
            const float active = Clamp(1.0f - std::fabs(dockIndPos_ - static_cast<float>(i)), 0.0f, 1.0f);
            const float lit = std::max(active, dockHover_[i]);
            const float opacity = 0.5f + 0.5f * lit;  // 50% idle, 100% active / hovered
            const float s = 1.0f - 0.10f * Clamp(dockPress_[i], 0.0f, 1.0f);
            const D2D1_RECT_F slotRect = D2D1::RectF(c.x - 13.0f, c.y - 13.0f, c.x + 13.0f, c.y + 13.0f);

            D2D1_MATRIX_3X2_F oldTransform;
            target_->GetTransform(&oldTransform);
            target_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, c) * oldTransform);

            const int srcIdx = slots.sourceIndex[i];
            if (srcIdx < 0) {
                wchar_t more[16] = {};
                swprintf_s(more, L"+%d", slots.overflow);
                if (boldTextFormat_) {
                    mutedBrush_->SetOpacity(0.55f + 0.45f * lit);
                    target_->DrawTextW(more, static_cast<UINT32>(wcslen(more)), boldTextFormat_.Get(),
                                       slotRect, mutedBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    mutedBrush_->SetOpacity(0.75f);
                }
            } else {
                const MediaSourceInfo& src = state.mediaSources[srcIdx];
                if (src.icon) {
                    DrawCircularBitmapPixels(*src.icon, c, MediaLayout::kDockIcon * 0.5f,
                                             dockIconBitmap_[i], dockIconGen_[i], opacity);
                } else if (boldTextFormat_ && !src.badge.empty()) {
                    textBrush_->SetOpacity(opacity);
                    target_->DrawTextW(src.badge.c_str(), static_cast<UINT32>(src.badge.size()),
                                       boldTextFormat_.Get(), slotRect, textBrush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    textBrush_->SetOpacity(0.96f);
                }

                if (src.playing) {  // 7px green dot, bottom-right, 1.5px ring in the pill colour
                    const D2D1_POINT_2F d = D2D1::Point2F(c.x + 9.5f, c.y + 9.5f);
                    ComPtr<ID2D1SolidColorBrush> ring, dot;
                    if (SUCCEEDED(target_->CreateSolidColorBrush(pillBg, &ring)) && ring) {
                        target_->FillEllipse(D2D1::Ellipse(d, 5.0f, 5.0f), ring.Get());
                    }
                    if (SUCCEEDED(target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &dot)) && dot) {
                        target_->FillEllipse(D2D1::Ellipse(d, 3.5f, 3.5f), dot.Get());
                    }
                }
            }
            target_->SetTransform(oldTransform);
        }
    }

    void UpdateMediaButtonAnimations(double now, bool playing) {
        float dt = 0.016f;
        if (lastMediaBtnTime_ > 0.0) {
            dt = static_cast<float>(std::max(0.001, std::min(now - lastMediaBtnTime_, 0.05)));
        }
        lastMediaBtnTime_ = now;

        const int pressedCmd = g_pressedMediaButton.load();
        bool isStillAnimating = false;

        for (int i = 0; i < 3; ++i) {
            const float target = (pressedCmd == i) ? 1.0f : 0.0f;
            const float tau = (target > mediaBtnPress_[i]) ? 0.025f : 0.075f;
            const float k = 1.0f - std::exp(-dt / tau);
            mediaBtnPress_[i] += (target - mediaBtnPress_[i]) * k;

            if (std::abs(mediaBtnPress_[i] - target) < 0.002f) {
                mediaBtnPress_[i] = target;
            } else {
                isStillAnimating = true;
            }
        }

        // Play/pause icon follows the state instantly.
        playMorph_ = playing ? 0.0f : 1.0f;

        if (isStillAnimating) {
            g_layoutDirty = true;
        }
    }

    // ── Skip icon: two rounded triangles + "depth swap" animation ────────────
    static constexpr float kSkipTriW = 0.626f;   // triangle width / height
    static constexpr float kSkipPitch = 0.451f;  // front-left minus back-left, / height

    HRESULT BuildRoundedTriangle(float x0, float w, float H, ComPtr<ID2D1PathGeometry>& out) {
        const D2D1_POINT_2F v[3] = {
            D2D1::Point2F(x0, -0.5f * H), D2D1::Point2F(x0, 0.5f * H), D2D1::Point2F(x0 + w, 0.0f)};
        const float d[3] = {0.11f * H, 0.11f * H, 0.14f * H};
        D2D1_POINT_2F a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            const D2D1_POINT_2F p = v[(i + 2) % 3], n = v[(i + 1) % 3];
            float px = p.x - v[i].x, py = p.y - v[i].y;
            float nx = n.x - v[i].x, ny = n.y - v[i].y;
            const float pl = std::sqrt(px * px + py * py), nl = std::sqrt(nx * nx + ny * ny);
            a[i] = D2D1::Point2F(v[i].x + px / pl * d[i], v[i].y + py / pl * d[i]);
            b[i] = D2D1::Point2F(v[i].x + nx / nl * d[i], v[i].y + ny / nl * d[i]);
        }
        ComPtr<ID2D1PathGeometry> geom;
        HRESULT hr = d2dFactory_->CreatePathGeometry(&geom);
        if (FAILED(hr) || !geom) return FAILED(hr) ? hr : E_FAIL;
        ComPtr<ID2D1GeometrySink> sink;
        hr = geom->Open(&sink);
        if (FAILED(hr) || !sink) return FAILED(hr) ? hr : E_FAIL;
        sink->BeginFigure(b[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(a[1]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[1], b[1]));
        sink->AddLine(a[2]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[2], b[2]));
        sink->AddLine(a[0]);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(v[0], b[0]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        hr = sink->Close();
        if (FAILED(hr)) return hr;
        out = geom;
        return S_OK;
    }

    bool EnsureSkipGeometry() {
        if (skipTriFull_ && skipTriNotch_) return true;
        if (!d2dFactory_) return false;
        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;

        ComPtr<ID2D1PathGeometry> full, back, front;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, full))) return false;
        if (FAILED(BuildRoundedTriangle(0.0f, w, H, back))) return false;
        if (FAILED(BuildRoundedTriangle(pitch, w, H, front))) return false;

        // The back triangle is cut by a slightly enlarged copy of the front one,
        // leaving a clean gap that works on translucent backgrounds too.
        ComPtr<ID2D1TransformedGeometry> grown;
        if (FAILED(d2dFactory_->CreateTransformedGeometry(
                front.Get(), D2D1::Matrix3x2F::Scale(1.16f, 1.16f, D2D1::Point2F(pitch + w / 3.0f, 0.0f)),
                &grown)) || !grown) return false;

        ComPtr<ID2D1PathGeometry> notch;
        if (FAILED(d2dFactory_->CreatePathGeometry(&notch)) || !notch) return false;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(notch->Open(&sink)) || !sink) return false;
        if (FAILED(back->CombineWithGeometry(grown.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, 0.05f, sink.Get()))) {
            return false;
        }
        if (FAILED(sink->Close())) return false;

        skipTriFull_ = full;
        skipTriNotch_ = notch;
        return true;
    }

    static float SkipSmooth(float x) {            // smoothstep 0..1
        x = Clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float SkipEaseInOut(float x) {         // easeInOutCubic
        x = Clamp(x, 0.0f, 1.0f);
        return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
    }

    // forward = true -> "next" (>>), false -> "previous" (<<, mirrored).
    void DrawSkipIcon(D2D1_POINT_2F center, bool forward, float opacity, float press) {
        if (!EnsureSkipGeometry()) return;

        const float H = MediaLayout::kNavButtonRadius * 0.78f;
        const float w = kSkipTriW * H, pitch = kSkipPitch * H;
        const float pairLeft = -0.5f * (pitch + w);

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);
        const float sf = 1.0f - 0.13f * press;
        const D2D1_MATRIX_3X2_F base =
            D2D1::Matrix3x2F::Scale(forward ? 1.0f : -1.0f, 1.0f) *
            D2D1::Matrix3x2F::Translation(center.x, center.y) *
            D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform;

        auto fill = [&](ID2D1Geometry* g, float x, float scale, float alpha) {
            if (alpha <= 0.003f) return;
            const D2D1_MATRIX_3X2_F m =
                D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(w / 3.0f, 0.0f)) *
                D2D1::Matrix3x2F::Translation(x, 0.0f) * base;
            target_->SetTransform(m);
            accentBrush_->SetOpacity(Clamp(opacity * alpha, 0.0f, 1.0f));
            target_->FillGeometry(g, accentBrush_.Get());
        };

        fill(skipTriNotch_.Get(), pairLeft, 1.0f, 1.0f);
        fill(skipTriFull_.Get(), pairLeft + pitch, 1.0f, 1.0f);

        target_->SetTransform(oldTransform);
        accentBrush_->SetOpacity(1.0f);
    }

    void DrawMediaControls(bool playing, D2D1_POINT_2F prev, D2D1_POINT_2F play, D2D1_POINT_2F next, double now) {
        UpdateMediaButtonAnimations(now, playing);
        DrawMediaButton(prev, MediaLayout::kNavButtonRadius, 0, false);
        DrawMediaButton(play, MediaLayout::kPlayButtonRadius, playing ? 1 : 2, true);
        DrawMediaButton(next, MediaLayout::kNavButtonRadius, 3, false);
    }

    void DrawMediaButton(D2D1_POINT_2F center, float radius, int kind, bool primary) {
        int buttonCmd = (kind == 0) ? 0 : ((kind == 1 || kind == 2) ? 1 : 2);
        bool isHovered = (g_hoveredMediaButton.load() == buttonCmd);
        const float press = (buttonCmd >= 0 && buttonCmd < 3) ? mediaBtnPress_[buttonCmd] : 0.0f;

        const float r = radius * (1.0f - 0.13f * press);

        const D2D1_COLOR_F restingBg = D2D1::ColorF(
            1.0f, 1.0f, 1.0f,
            primary ? (isHovered ? 0.16f : 0.080f) : (isHovered ? 0.09f : 0.040f)
        );
        const D2D1_COLOR_F pressedBg = D2D1::ColorF(
            currentAccent_.r, currentAccent_.g, currentAccent_.b,
            primary ? 0.28f : 0.18f
        );

        const D2D1_COLOR_F currentBg = D2D1::ColorF(
            restingBg.r + (pressedBg.r - restingBg.r) * press,
            restingBg.g + (pressedBg.g - restingBg.g) * press,
            restingBg.b + (pressedBg.b - restingBg.b) * press,
            restingBg.a + (pressedBg.a - restingBg.a) * press
        );

        if (ID2D1SolidColorBrush* bg = AcquireBrush(currentBg)) {
            target_->FillEllipse(D2D1::Ellipse(center, r, r), bg);
        }

        const float baseOpacity = primary ? (isHovered ? 1.0f : 0.88f) : (isHovered ? 0.92f : 0.62f);
        const float iconOpacity = Clamp(baseOpacity + (1.0f - baseOpacity) * press, 0.0f, 1.0f);
        accentBrush_->SetOpacity(iconOpacity);

        if (kind == 0 || kind == 3) {
            DrawSkipIcon(center, kind == 3, iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        if (kind == 1 || kind == 2) {
            DrawPlayPauseIcon(center, iconOpacity, press);
            accentBrush_->SetOpacity(1.0f);
            return;
        }

        const wchar_t* glyph = nullptr;
        IDWriteTextFormat* fmt = nullptr;
        if (usingFluentIcons_) {
            if (kind == 0) {
                glyph = L"\uF8AC";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uF8AE";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uF5B0";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uF8AD";
                fmt = mediaNavIconFormat_.Get();
            }
        } else {
            if (kind == 0) {
                glyph = L"\uE100";
                fmt = mediaNavIconFormat_.Get();
            } else if (kind == 1) {
                glyph = L"\uE103";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 2) {
                glyph = L"\uE102";
                fmt = mediaPlayIconFormat_.Get();
            } else if (kind == 3) {
                glyph = L"\uE101";
                fmt = mediaNavIconFormat_.Get();
            }
        }

        if (glyph && fmt) {
            const float offsetX = (kind == 2) ? 1.0f : 0.0f;
            D2D1_RECT_F glyphRect = D2D1::RectF(
                center.x - radius + offsetX,
                center.y - radius,
                center.x + radius + offsetX,
                center.y + radius
            );

            if (press > 0.001f) {
                D2D1_MATRIX_3X2_F oldTransform;
                target_->GetTransform(&oldTransform);
                const float scaleFactor = 1.0f - 0.13f * press;
                target_->SetTransform(D2D1::Matrix3x2F::Scale(scaleFactor, scaleFactor, center) * oldTransform);
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                target_->SetTransform(oldTransform);
            } else {
                target_->DrawTextW(glyph, 1, fmt, glyphRect, accentBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }

        accentBrush_->SetOpacity(1.0f);
    }

    // ── Play/pause morph ─────────────────────────────────────────────────────
    // Both icons are two quads with matching vertices: the pause bars become the
    // two halves of the play triangle (the right half collapses into the tip).
    static constexpr float kPlayPauseCorner = 1.5f;

    struct PlayPauseShapes {
        D2D1_POINT_2F pause[8];
        D2D1_POINT_2F play[8];
    };

    static const PlayPauseShapes& GetPlayPauseShapes() {
        static const PlayPauseShapes shapes = [] {
            PlayPauseShapes s = {};
            const float rr = kPlayPauseCorner;
            const float bx0 = 2.1f, bx1 = 6.5f, by = 7.5f;
            // Left bar, then right bar: TL, TR, BR, BL.
            s.pause[0] = D2D1::Point2F(-bx1 + rr, -by + rr);
            s.pause[1] = D2D1::Point2F(-bx0 - rr, -by + rr);
            s.pause[2] = D2D1::Point2F(-bx0 - rr,  by - rr);
            s.pause[3] = D2D1::Point2F(-bx1 + rr,  by - rr);
            s.pause[4] = D2D1::Point2F( bx0 + rr, -by + rr);
            s.pause[5] = D2D1::Point2F( bx1 - rr, -by + rr);
            s.pause[6] = D2D1::Point2F( bx1 - rr,  by - rr);
            s.pause[7] = D2D1::Point2F( bx0 + rr,  by - rr);

            // Triangle (optically centred), split by a shared edge T-U.
            const D2D1_POINT_2F A = D2D1::Point2F(-5.5f, -7.5f);
            const D2D1_POINT_2F B = D2D1::Point2F(-5.5f,  7.5f);
            const D2D1_POINT_2F C = D2D1::Point2F( 7.5f,  0.0f);
            const D2D1_POINT_2F T = D2D1::Point2F( 1.0f, -3.75f);
            const D2D1_POINT_2F U = D2D1::Point2F( 1.0f,  3.75f);
            const float la = std::hypot(B.x - C.x, B.y - C.y);
            const float lb = std::hypot(C.x - A.x, C.y - A.y);
            const float lc = std::hypot(A.x - B.x, A.y - B.y);
            const float per = la + lb + lc;
            const float ix = (la * A.x + lb * B.x + lc * C.x) / per;
            const float iy = (la * A.y + lb * B.y + lc * C.y) / per;
            const float area = 0.5f * std::fabs((B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x));
            const float inr = 2.0f * area / per;
            const float k = (inr - rr) / inr;
            auto shrink = [&](D2D1_POINT_2F p) {
                return D2D1::Point2F(ix + k * (p.x - ix), iy + k * (p.y - iy));
            };
            s.play[0] = shrink(A); s.play[1] = shrink(T); s.play[2] = shrink(U); s.play[3] = shrink(B);
            s.play[4] = shrink(T); s.play[5] = shrink(C); s.play[6] = shrink(C); s.play[7] = shrink(U);
            return s;
        }();
        return shapes;
    }

    bool EnsureRoundJoinStyle() {
        if (roundJoinStyle_) return true;
        if (!d2dFactory_) return false;
        return SUCCEEDED(d2dFactory_->CreateStrokeStyle(
                   D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                                               D2D1_DASH_STYLE_SOLID, 0.0f),
                   nullptr, 0, &roundJoinStyle_)) && roundJoinStyle_;
    }

    void DrawPlayPauseIcon(D2D1_POINT_2F center, float opacity, float press) {
        if (!d2dFactory_) return;
        EnsureRoundJoinStyle();
        const PlayPauseShapes& sh = GetPlayPauseShapes();
        const float m = Clamp(playMorph_, 0.0f, 1.0f);

        D2D1_POINT_2F p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = D2D1::Point2F(sh.pause[i].x + (sh.play[i].x - sh.pause[i].x) * m,
                                 sh.pause[i].y + (sh.play[i].y - sh.pause[i].y) * m);
        }

        ComPtr<ID2D1PathGeometry> quad[2];
        for (int q = 0; q < 2; ++q) {
            if (FAILED(d2dFactory_->CreatePathGeometry(&quad[q])) || !quad[q]) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(quad[q]->Open(&sink)) || !sink) return;
            sink->BeginFigure(p[q * 4], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(p[q * 4 + 1]);
            sink->AddLine(p[q * 4 + 2]);
            sink->AddLine(p[q * 4 + 3]);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return;
        }

        D2D1_MATRIX_3X2_F oldTransform;
        target_->GetTransform(&oldTransform);

        // Everything is drawn opaque inside a layer that carries the icon opacity,
        // so the overlap between fill, stroke and the two halves never double-blends.
        ComPtr<ID2D1Layer> layer;
        const bool layered = SUCCEEDED(target_->CreateLayer(nullptr, &layer)) && layer;
        if (layered) {
            target_->PushLayer(
                D2D1::LayerParameters(D2D1::RectF(center.x - 18.0f, center.y - 18.0f,
                                                  center.x + 18.0f, center.y + 18.0f),
                                      nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), opacity, nullptr, D2D1_LAYER_OPTIONS_NONE),
                layer.Get());
        }
        accentBrush_->SetOpacity(layered ? 1.0f : opacity);

        const float sf = 1.0f - 0.13f * press;
        target_->SetTransform(D2D1::Matrix3x2F::Translation(center.x, center.y) *
                              D2D1::Matrix3x2F::Scale(sf, sf, center) * oldTransform);
        for (int q = 0; q < 2; ++q) {
            target_->FillGeometry(quad[q].Get(), accentBrush_.Get());
            target_->DrawGeometry(quad[q].Get(), accentBrush_.Get(), kPlayPauseCorner * 2.0f,
                                  roundJoinStyle_.Get());
        }
        target_->SetTransform(oldTransform);
        if (layered) target_->PopLayer();
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Progress bar styles (Wavy / Squiggle / Bar) ──────────────────────────
    // Slim is the original bar and keeps its own code path in DrawMedia. The
    // hit test is untouched: every shape here stays inside the scrubber's hit area.
    void DrawStyledProgress(ProgressStyle style, float barLeft, float barRight, float cy,
                            float progress, bool dragging, bool playing, double now) {
        const float width = barRight - barLeft;
        if (width <= 8.0f) return;
        progress = Clamp(progress, 0.0f, 1.0f);
        const float thumbX = barLeft + width * progress;

        ComPtr<ID2D1SolidColorBrush> accent, track;
        if (FAILED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 1.0f), &accent)) || !accent) return;
        if (FAILED(target_->CreateSolidColorBrush(material_.raisedStrong, &track)) || !track) return;
        EnsureRoundJoinStyle();

        // Wave strength eases toward 0 when paused and back to 1 when playing.
        {
            const bool fresh = progressLastTime_ >= 0.0 && (now - progressLastTime_) < 0.25;
            const float target = playing ? 1.0f : 0.0f;
            if (!fresh) {
                progressAmp_ = target;
            } else {
                const float dt = static_cast<float>(now - progressLastTime_);
                progressAmp_ += (target - progressAmp_) * (1.0f - std::exp(-dt / 0.16f));
                if (std::fabs(target - progressAmp_) < 0.004f) {
                    progressAmp_ = target;
                } else {
                    g_layoutDirty = true;
                }
            }
            progressLastTime_ = now;
        }
        const float amp = progressAmp_;

        auto drawEndDot = [&](float x) {
            ComPtr<ID2D1SolidColorBrush> dot;
            if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.9f), &dot)) && dot) {
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 1.7f, 1.7f), dot.Get());
            }
        };

        if (style == ProgressStyle::Bar) {
            // Thick rounded bar split by a vertical line thumb, with a dot at the end.
            const float half = dragging ? 4.5f : 4.0f;
            const float gap = dragging ? 5.0f : 4.0f;
            const float fillRight = thumbX - gap;
            const float trackLeft = thumbX + gap;
            if (fillRight - barLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(barLeft, cy - half, fillRight, cy + half), half, half),
                    accent.Get());
            }
            if (barRight - trackLeft > 1.0f) {
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(trackLeft, cy - half, barRight, cy + half), half, half),
                    track.Get());
                drawEndDot(barRight - half);
            }
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 24.0f : 20.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
            return;
        }

        // Wavy and Squiggle: a travelling sine wave up to the thumb, a thin flat
        // line after it. Squiggle is shorter, shallower and about half the speed.
        const bool wavy = (style == ProgressStyle::Wavy);
        const float A = (wavy ? 3.2f : 1.8f) * amp;
        const float lambda = wavy ? 26.0f : 15.0f;
        const double freq = wavy ? 0.9 : 0.45;
        const float strokeW = wavy ? 3.4f : 3.0f;
        const float k = 6.2831853f / lambda;
        const float phase = static_cast<float>(std::fmod(now * freq, 1.0) * 6.2831853);
        const float endTaper = wavy ? 16.0f : 10.0f;  // flattens into the thumb

        if (thumbX - barLeft > 1.5f) {
            std::vector<D2D1_POINT_2F> pts;
            pts.reserve(static_cast<size_t>((thumbX - barLeft) / 1.25f) + 3);
            for (float x = barLeft;; x += 1.25f) {
                const float xx = std::min(x, thumbX);
                const float taper = std::min(SkipSmooth((xx - barLeft) / 10.0f),
                                             SkipSmooth((thumbX - xx) / endTaper));
                pts.push_back(D2D1::Point2F(xx, cy + A * taper * std::sin(k * (xx - barLeft) - phase)));
                if (x >= thumbX) break;
            }
            ComPtr<ID2D1PathGeometry> path;
            if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&path)) && path) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(path->Open(&sink)) && sink) {
                    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                    if (pts.size() > 1) sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        target_->DrawGeometry(path.Get(), accent.Get(), strokeW, roundJoinStyle_.Get());
                    }
                }
            }
        }

        const float thumbR = dragging ? 7.0f : 5.5f;
        const float trackStart = thumbX + (wavy ? thumbR + 3.0f : 4.0f);
        if (barRight - trackStart > 2.0f) {
            const float tw = wavy ? 2.2f : 2.8f;
            target_->DrawLine(D2D1::Point2F(trackStart, cy), D2D1::Point2F(barRight - 2.0f, cy),
                              track.Get(), tw, roundJoinStyle_.Get());
            if (wavy) drawEndDot(barRight - 1.7f);
        }

        if (wavy) {
            if (dragging) {
                ComPtr<ID2D1SolidColorBrush> halo;
                if (SUCCEEDED(target_->CreateSolidColorBrush(WithAlpha(material_.accent, 0.22f), &halo)) && halo) {
                    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR * 2.0f, thumbR * 2.0f),
                                         halo.Get());
                }
            }
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, cy), thumbR, thumbR), accent.Get());
        } else {
            const float tw = dragging ? 4.0f : 3.0f;
            const float th = dragging ? 22.0f : 18.0f;
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(thumbX - tw * 0.5f, cy - th * 0.5f, thumbX + tw * 0.5f, cy + th * 0.5f),
                                  tw * 0.5f, tw * 0.5f),
                accent.Get());
        }
    }

    // ── Album art ────────────────────────────────────────────────────────────
    void EnsureArtBitmap(const MediaSnapshot& media) {
        if (!media.art) return;
        const BitmapPixels& art = *media.art;
        if (artGeneration_ != art.generation || !artBitmap_) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(art.width, art.height),
                                  art.bgra.data(), art.width * 4,
                                  &props, &artBitmap_);
            artGeneration_ = art.generation;
        }
    }

    // ── Audio spectrum styles ────────────────────────────────────────────────
    // DrawSpectrum is the single entry point used by the collapsed pill, the
    // expanded player. It owns the track-change detection and
    // hands the animation clock to the selected style via specT_/specDir_.
    float SpecBandRange(const SharedState& state, float from, float to) const {
        // Average of the analyzer bands covering [from, to) in 0..kSpectrumBands.
        int a = std::clamp(static_cast<int>(std::floor(from)), 0, kSpectrumBands - 1);
        int b = std::clamp(static_cast<int>(std::ceil(to)), a + 1, kSpectrumBands);
        float sum = 0.0f;
        for (int i = a; i < b; ++i) sum += state.bands[static_cast<size_t>(i)];
        return sum / static_cast<float>(b - a);
    }

    void DrawSpectrum(const SharedState& state, D2D1_RECT_F rect, const Settings& settings, double now) {
        constexpr double kSpecDur = 1.15;
        const bool recentlyDrawn = specLastDrawn_ >= 0.0 && (now - specLastDrawn_) < 0.25;
        float dt = recentlyDrawn ? static_cast<float>(now - specLastDrawn_) : 0.016f;
        dt = Clamp(dt, 0.001f, 0.05f);
        specLastDrawn_ = now;

        const size_t key = std::hash<std::wstring>{}(state.media.title) * 31u +
                           std::hash<std::wstring>{}(state.media.artist);
        if (!specInit_ || !recentlyDrawn) {
            specInit_ = true;
            specKey_ = key;
            specStart_ = -1.0;
            if (!recentlyDrawn) {
                std::fill(std::begin(specPeak_), std::end(specPeak_), 0.0f);
                std::fill(std::begin(specPeakVel_), std::end(specPeakVel_), 0.0f);
                specBassSlow_ = 0.0f;
                specKick_ = 0.0f;
            }
        } else if (key != specKey_ && !state.media.title.empty()) {
            const bool running = specStart_ >= 0.0 && (now - specStart_) < 0.35;
            specKey_ = key;
            // Title and artist often land a few frames apart: absorb the second one.
            if (!running) {
                specStart_ = now;
                specDir_ = 1.0f;
                // Led: the old peaks rain down as the scanline reboots the matrix.
                for (float& v : specPeakVel_) v = 0.0f;
                for (int i = 0; i < kSpectrumBands; ++i) specPeak_[i] *= 0.6f;
            }
        }

        specT_ = -1.0f;
        if (specStart_ >= 0.0) {
            const float t = static_cast<float>((now - specStart_) / kSpecDur);
            if (t >= 1.0f) {
                specStart_ = -1.0;
            } else {
                specT_ = t;
                g_layoutDirty = true;
            }
        }

        // Low-end energy drives the orb's pulse and a beat "kick" envelope.
        const float bass = SpecBandRange(state, 0.0f, 4.0f);
        specBassSlow_ += (bass - specBassSlow_) * (1.0f - std::exp(-dt / 0.45f));
        const float beat = std::max(0.0f, bass - specBassSlow_ - 0.04f);
        specKick_ = std::max(specKick_ * std::exp(-dt / 0.18f), Clamp(beat * 3.2f, 0.0f, 1.0f));
        specBass_ += (bass - specBass_) * (1.0f - std::exp(-dt / 0.07f));
        specPhase_ += dt * (1.6f + 5.0f * specBass_ + 8.0f * specKick_);

        switch (settings.spectrumStyle) {
            case SpectrumStyle::Orb:    DrawSpectrumOrb(state, rect); break;
            case SpectrumStyle::Plasma: DrawSpectrumPlasma(state, rect); break;
            case SpectrumStyle::Led:    DrawSpectrumLed(state, rect, dt); break;
            default:                    DrawWaveform(state, rect); break;
        }
    }

    // ── Pulse Orb ────────────────────────────────────────────────────────────
    void DrawSpectrumOrb(const SharedState& state, D2D1_RECT_F rect) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const D2D1_POINT_2F c = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        const float R = std::min(w, h) * 0.5f + 1.0f;  // outer reach
        const float r0 = R * 0.40f;  // core radius at rest
        // Fewer, thinner spokes on a small orb so they never merge into a solid disc.
        const int kSpokes = R < 14.0f ? 16 : 24;
        const int half = kSpokes / 2;
        constexpr float kTwoPi = 6.28318531f;

        const float t = specT_;
        // Spokes are fixed (bass at the bottom, mirrored left/right) so the shape always
        // reads as a spectrum; only the track-change detonation spins the orb.
        float lenScale = 1.0f, coreScale = 1.0f, ringShrink = 0.0f, spin = 0.0f;
        float burstAlpha = 0.0f, burstR = 0.0f;
        if (t >= 0.0f) {
            if (t < 0.28f) {
                const float u = SkipSmooth(t / 0.28f);
                lenScale = 1.0f - u;
                coreScale = 1.0f - 0.35f * u;
                ringShrink = 0.30f * u;
            } else {
                const float u = Clamp((t - 0.28f) / 0.72f, 0.0f, 1.0f);
                const float spring = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
                lenScale = spring;
                coreScale = 1.0f + 0.55f * std::exp(-7.0f * u) * std::cos(6.0f * u);
                ringShrink = 0.30f * std::exp(-9.0f * u);
                spin += specDir_ * 4.2f * std::pow(1.0f - u, 3.0f);
                burstR = r0 + (R + 3.0f - r0) * SkipEaseInOut(u / 0.8f);
                burstAlpha = 0.75f * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f)) * (1.0f - Clamp(u / 0.85f, 0.0f, 1.0f));
            }
        }

        const float baseR = r0 * (1.0f - ringShrink);
        const float bassPulse = 0.18f * specBass_ + 0.22f * specKick_;

        EnsureRoundJoinStyle();
        const float strokeW = Clamp(kTwoPi * (baseR + 1.0f) / static_cast<float>(kSpokes) * 0.62f, 1.0f, R * 0.16f);

        // Thin shell ring just inside the spokes: breathes with the bass (no filled background).
        accentBrush_->SetOpacity(0.25f + 0.35f * specBass_ + 0.25f * specKick_);
        const float shellR = baseR * (0.90f + 0.18f * bassPulse);
        target_->DrawEllipse(D2D1::Ellipse(c, shellR, shellR), accentBrush_.Get(), 0.8f);

        for (int k = 0; k < kSpokes; ++k) {
            const int m = k < half ? k : kSpokes - 1 - k;  // mirrored 0..half-1
            const float lo = static_cast<float>(m) * kSpectrumBands / half;
            float v = SpecBandRange(state, lo, lo + static_cast<float>(kSpectrumBands) / half);
            v = Clamp(v * 1.1f, 0.04f, 1.0f);
            // Each spoke trails the detonation slightly so the burst reads as a wave.
            float ls = lenScale;
            if (t >= 0.28f) {
                const float u = Clamp((t - 0.28f - 0.012f * m) / 0.72f, 0.0f, 1.0f);
                ls = 1.0f - std::exp(-6.0f * u) * std::cos(10.0f * u);
            }
            const float len = (R - baseR - 1.5f) * (0.10f + 0.90f * v) * Clamp(ls, 0.0f, 1.45f);
            // k = 0 and k = kSpokes-1 sit either side of the bottom (bass), highs meet at the top.
            const float a = spin + 1.5707963f + kTwoPi * (static_cast<float>(k) + 0.5f) / kSpokes;
            const float ca = std::cos(a), sa = std::sin(a);
            const float l = std::max(len, 0.6f);
            const D2D1_POINT_2F p0 = D2D1::Point2F(c.x + ca * (baseR + 1.0f), c.y + sa * (baseR + 1.0f));
            const D2D1_POINT_2F p1 = D2D1::Point2F(c.x + ca * (baseR + 1.0f + l), c.y + sa * (baseR + 1.0f + l));
            // Soft glow underlay, then the spoke itself.
            accentBrush_->SetOpacity(0.10f + 0.14f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW * 2.0f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(0.45f + 0.55f * v);
            target_->DrawLine(p0, p1, accentBrush_.Get(), strokeW, roundJoinStyle_.Get());
        }

        // Core.
        accentBrush_->SetOpacity(0.95f);
        const float coreR = std::max(0.8f, baseR * 0.58f * coreScale * (1.0f + bassPulse));
        target_->FillEllipse(D2D1::Ellipse(c, coreR, coreR), accentBrush_.Get());
        ComPtr<ID2D1SolidColorBrush> hot;
        if (SUCCEEDED(target_->CreateSolidColorBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.65f), &hot)) && hot) {
            hot->SetOpacity(0.55f + 0.35f * specKick_);
            target_->FillEllipse(D2D1::Ellipse(c, coreR * 0.45f, coreR * 0.45f), hot.Get());
        }

        if (burstAlpha > 0.01f) {
            accentBrush_->SetOpacity(burstAlpha);
            target_->DrawEllipse(D2D1::Ellipse(c, burstR, burstR), accentBrush_.Get(), 0.8f + 1.4f * burstAlpha);
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Plasma Thread ────────────────────────────────────────────────────────
    void DrawSpectrumPlasma(const SharedState& state, D2D1_RECT_F rect) {
        constexpr int kPts = 34;
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float maxA = h * 0.46f;
        const float t = specT_;
        const float dir = specDir_;

        EnsureRoundJoinStyle();
        ID2D1SolidColorBrush* hot = AcquireBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.6f));  // per-frame pool, no allocation

        // Front position (normalised, already mirrored for "previous").
        const float front = t < 0.15f ? -0.4f : -0.4f + 1.9f * SkipEaseInOut((t - 0.15f) / 0.78f);

        for (int strand = 0; strand < 2; ++strand) {
            D2D1_POINT_2F pts[kPts];
            for (int i = 0; i < kPts; ++i) {
                const float xn = static_cast<float>(i) / (kPts - 1);
                const float xd = dir >= 0.0f ? xn : 1.0f - xn;  // distance along the sweep
                // max(): sin(pi) is slightly negative in float and pow(neg, 0.7) is NaN.
                float env = std::pow(std::max(0.0f, std::sin(3.14159265f * xn)), 0.7f);  // pinned at both ends
                float band = SpecBandRange(state, xn * (kSpectrumBands - 1), xn * (kSpectrumBands - 1) + 1.5f);
                band = Clamp(0.10f + 0.95f * band + 0.25f * specKick_, 0.0f, 1.25f);

                float ignite = 1.0f, boost = 0.0f;
                if (t >= 0.0f) {
                    if (t < 0.15f) {
                        ignite = 1.0f - SkipSmooth(t / 0.15f);
                    } else {
                        ignite = SkipSmooth((front - xd) / 0.22f + 0.15f);
                        const float g = (xd - front) / 0.16f;
                        boost = 0.95f * std::exp(-g * g);
                    }
                }
                const float ph = specPhase_ + (strand ? 3.14159265f : 0.0f);
                const float wave = std::sin(xn * (7.0f + 4.0f * band) - ph) * 0.65f +
                                   std::sin(xn * 13.0f + ph * 0.6f) * 0.35f;
                const float A = maxA * env * (band * ignite + boost * env);
                pts[i] = D2D1::Point2F(rect.left + xn * w, cy + wave * A * (strand ? -1.0f : 1.0f));
            }
            ComPtr<ID2D1PathGeometry> path;
            if (FAILED(d2dFactory_->CreatePathGeometry(&path)) || !path) continue;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(path->Open(&sink)) || !sink) continue;
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(pts + 1, kPts - 1);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) continue;

            // Glow underlay, body, hot core.
            accentBrush_->SetOpacity(strand ? 0.16f : 0.22f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), 4.2f, roundJoinStyle_.Get());
            accentBrush_->SetOpacity(strand ? 0.62f : 0.95f);
            target_->DrawGeometry(path.Get(), accentBrush_.Get(), strand ? 1.1f : 1.6f, roundJoinStyle_.Get());
            if (hot && !strand) {
                hot->SetOpacity(0.35f + 0.4f * specKick_);
                target_->DrawGeometry(path.Get(), hot, 0.6f, roundJoinStyle_.Get());
            }
        }

        // Spark riding the wave front during the re-ignite.
        if (t >= 0.15f && hot) {
            const float xd = Clamp(front, 0.0f, 1.0f);
            const float xn = dir >= 0.0f ? xd : 1.0f - xd;
            const float a = 1.0f - Clamp((t - 0.80f) / 0.2f, 0.0f, 1.0f);
            if (front > -0.1f && front < 1.15f) {
                hot->SetOpacity(0.85f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 2.2f, 2.2f), hot);
                accentBrush_->SetOpacity(0.28f * a);
                target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + xn * w, cy), 5.5f, 5.5f), accentBrush_.Get());
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Peak Matrix ──────────────────────────────────────────────────────────
    // Stacked LED cells per column with gravity-driven falling peak caps.
    void DrawSpectrumLed(const SharedState& state, D2D1_RECT_F rect, float dt) {
        const float w = rect.right - rect.left, h = rect.bottom - rect.top;
        const float colGap = 1.8f, colW = 2.6f;
        const int cols = std::clamp(static_cast<int>((w + colGap) / (colW + colGap)), 3, 14);
        const float cw = (w - colGap * (cols - 1)) / cols;
        const int rows = std::clamp(static_cast<int>(h / 3.6f), 4, 8);
        const float cellGap = 1.0f;
        const float ch = (h - cellGap * (rows - 1)) / rows;
        const float t = specT_;
        const float dir = specDir_;

        ID2D1SolidColorBrush* hot = AcquireBrush(MixColor(material_.accent, D2D1::ColorF(1, 1, 1, 1), 0.7f));  // per-frame pool, no allocation

        // Scan position in column units, mirrored for "previous".
        const float travel = static_cast<float>(cols) + 4.0f;
        const float scan = t < 0.0f ? -10.0f : -1.5f + travel * SkipEaseInOut(t / 0.82f);

        for (int c = 0; c < cols; ++c) {
            const int slot = dir >= 0.0f ? c : cols - 1 - c;  // order the scan reaches this column
            // Same source as Classic Bars: each column reads the level history, newest on the
            // right, so every column rises and falls with the music (no frequency tilt).
            const size_t wfSize = state.waveform.size();
            const size_t wfOffset = static_cast<size_t>(cols - c) * 3;
            const size_t wfSrc = (state.waveformWrite + wfSize * 2 - wfOffset) % wfSize;
            float v = Clamp(state.waveform[wfSrc], 0.03f, 1.0f);

            // Falling peak cap with gravity; a hit snaps it up.
            float& pk = specPeak_[c];
            float& pv = specPeakVel_[c];
            if (v >= pk) { pk = v; pv = 0.0f; }
            else { pv += 2.4f * dt; pk = std::max(v, pk - pv * dt); }

            float colAlpha = 1.0f, flash = 0.0f;
            if (t >= 0.0f) {
                const float d = scan - static_cast<float>(slot);  // >0: scanline already passed
                if (d < 0.0f) {
                    colAlpha = 0.35f;  // old song, about to be wiped
                } else {
                    flash = Clamp(1.0f - d / 3.2f, 0.0f, 1.0f);  // glowing tail behind the scanline
                    colAlpha = 0.35f + 0.65f * SkipSmooth(d / 1.2f);
                    if (d < 1.6f) { pk = std::min(pk, 0.0f + v); }      // peaks reset as the line passes
                }
            }

            const float x = rect.left + c * (cw + colGap);
            const int lit = static_cast<int>(std::round(v * rows));
            const int cap = std::clamp(static_cast<int>(std::round(pk * rows)) - 1, 0, rows - 1);
            for (int r = 0; r < rows; ++r) {
                const float yb = rect.bottom - r * (ch + cellGap);
                D2D1_RECT_F cell = D2D1::RectF(x, yb - ch, x + cw, yb);
                const float rr = std::min(cw, ch) * 0.35f;
                const float topness = rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.0f;
                if (flash > 0.0f) {
                    // Whole column ignites, fading from the bottom up as the tail dies.
                    ID2D1SolidColorBrush* b = (hot && flash > 0.7f && r >= rows / 2) ? hot : accentBrush_.Get();
                    b->SetOpacity(Clamp(0.25f + 0.75f * flash, 0.0f, 1.0f) * (0.6f + 0.4f * (1.0f - topness)));
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), b);
                } else if (r < lit) {
                    accentBrush_->SetOpacity((0.50f + 0.45f * topness) * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                } else if (r == cap && cap >= lit && pk > 0.05f && hot) {
                    hot->SetOpacity(0.9f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), hot);
                } else {
                    accentBrush_->SetOpacity(0.09f * colAlpha);
                    target_->FillRoundedRectangle(D2D1::RoundedRect(cell, rr, rr), accentBrush_.Get());
                }
            }
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // ── Brightness banner (shares the Volume banner's look) ──────────────────
    void DrawLevelBanner(D2D1_RECT_F rect, const wchar_t* glyph, const std::wstring& label,
                         const wchar_t* value, float pct, bool drained) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), raisedStrongBrush_.Get());

        textBrush_->SetOpacity(0.95f);

        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
        mutedBrush_->SetOpacity(0.50f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        if (drained) {
            ComPtr<ID2D1SolidColorBrush> trackBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), trackBrush.Get());
            }
            D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                           track.left + (track.right - track.left) * pct,
                                           track.bottom);
            ComPtr<ID2D1SolidColorBrush> dim;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.textSecondary, 0.35f), &dim)) && dim) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
            }
        } else {
            DrawAccentTrack(track, pct, 2.5f);
        }
        accentBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawBrightness(const SharedState& state, D2D1_RECT_F rect) {
        wchar_t value[32] = {};
        swprintf_s(value, L"%d%%", state.brightness.percent);
        DrawLevelBanner(rect, L"\uE706", Loc(L"Brightness"), value,
                        Clamp(state.brightness.percent / 100.0f, 0.0f, 1.0f), false);
    }

    void DrawAlbumArt(const MediaSnapshot& media, D2D1_RECT_F rect, double now, float radius = 9.0f, bool drawBadge = true) {
        ComPtr<ID2D1Geometry> mask = GetShapeGeometry(rect, radius, false);
        ID2D1Layer* layer = AcquireLayer();
        const bool roundedClip = mask && layer;
        if (roundedClip) {
            target_->PushLayer(D2D1::LayerParameters(rect, mask.Get()), layer);
        } else {
            target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }

        if (media.art) {
            EnsureArtBitmap(media);

            D2D1_RECT_F dst = D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom);
            target_->DrawBitmap(artBitmap_.Get(), dst, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            accentBrush_->SetOpacity(0.24f);
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush_.Get());
            accentBrush_->SetOpacity(1.0f);
            if (media.sourceIcon) {
                D2D1_RECT_F iconRect = D2D1::RectF(rect.left + 11, rect.top + 11,
                                                  rect.right - 11, rect.bottom - 11);
                DrawBitmapPixels(*media.sourceIcon, iconRect, mediaSourceIconBitmap_,
                                 mediaSourceIconGeneration_, 0.95f);
            } else {
                target_->DrawTextW(media.sourceBadge.empty() ? L"\u25b6" : media.sourceBadge.c_str(),
                                   static_cast<UINT32>(media.sourceBadge.empty() ? 1 : media.sourceBadge.size()),
                                   textFormat_.Get(), rect, textBrush_.Get());
            }
        }

        if (drawBadge && media.sourceIcon && artBadgeAlpha_ > 0.01f) {
            D2D1_RECT_F badge = D2D1::RectF(rect.right - 24, rect.bottom - 22,
                                           rect.right - 3, rect.bottom - 3);
            DrawCircularBitmapPixels(*media.sourceIcon,
                                     D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                   (badge.top + badge.bottom) * 0.5f),
                                     9.5f, mediaSourceIconBitmap_,
                                     mediaSourceIconGeneration_, 0.98f * artBadgeAlpha_);
        }

        if (roundedClip) {
            target_->PopLayer();
        } else {
            target_->PopAxisAlignedClip();
        }
    }

    void DrawBitmapPixels(const BitmapPixels& pixels, D2D1_RECT_F rect,
                          ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                          float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        if (cachedGeneration != pixels.generation || !cache) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            target_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height),
                                  pixels.bgra.data(), pixels.width * 4,
                                  &props, &cache);
            cachedGeneration = pixels.generation;
        }

        if (cache) {
            target_->DrawBitmap(cache.Get(), rect, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    void DrawCircularBitmapPixels(const BitmapPixels& pixels, D2D1_POINT_2F center, float radius,
                                  ComPtr<ID2D1Bitmap>& cache, uint64_t& cachedGeneration,
                                  float opacity = 1.0f) {
        if (pixels.bgra.empty()) {
            return;
        }

        D2D1_RECT_F rect = D2D1::RectF(center.x - radius, center.y - radius,
                                      center.x + radius, center.y + radius);
        ComPtr<ID2D1EllipseGeometry> ellipse;
        d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(center, radius, radius), &ellipse);
        ComPtr<ID2D1Layer> layer;
        target_->CreateLayer(nullptr, &layer);

        if (ellipse && layer) {
            target_->PushLayer(D2D1::LayerParameters(
                                  rect, ellipse.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                              layer.Get());
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
            target_->PopLayer();
        } else {
            DrawBitmapPixels(pixels, rect, cache, cachedGeneration, opacity);
        }

        ID2D1SolidColorBrush* border = AcquireBrush(material_.hairline);  // per-frame pool, no allocation
        if (border) {
            target_->DrawEllipse(D2D1::Ellipse(center, radius, radius), border, 1.0f);
        }
    }

    void DrawMarqueeText(const std::wstring& text, D2D1_RECT_F rect, IDWriteTextFormat* format,
                         ID2D1Brush* brush, double now, float speed, MarqueeLayoutCache& cache) {
        if (!format || !brush || text.empty()) {
            return;
        }

        const float wrapHeight = rect.bottom - rect.top;
        // Only rebuild the layout when text/format/height actually changed.
        // Scroll offset is applied via the translated draw origin below, so
        // it never invalidates the cache — this is what lets the marquee
        // scroll every frame without calling CreateTextLayout every frame.
        if (cache.text != text || cache.format != format ||
            std::fabs(cache.wrapWidth - wrapHeight) > 0.01f || !cache.layout) {
            cache.layout.Reset();
            dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                             format, 2000.0f, wrapHeight, &cache.layout);
            cache.text = text;
            cache.format = format;
            cache.wrapWidth = wrapHeight;
            cache.metrics = {};
            if (cache.layout) {
                cache.layout->GetMetrics(&cache.metrics);
            }
        }

        if (!cache.layout) {
            return;
        }

        const float available = rect.right - rect.left;

        D2D1_RECT_F clipRect = rect;
        clipRect.top -= 10.0f;
        clipRect.bottom += 10.0f;
        target_->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (cache.metrics.widthIncludingTrailingWhitespace <= available) {
            target_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), cache.layout.Get(), brush,
                                    D2D1_DRAW_TEXT_OPTIONS_NONE);
        } else {
            const float cycle = cache.metrics.widthIncludingTrailingWhitespace + 38.0f;
            const float offset = std::fmod(static_cast<float>(now) * speed, cycle);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), cache.layout.Get(),
                                    brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            target_->DrawTextLayout(D2D1::Point2F(rect.left - offset + cycle, rect.top),
                                    cache.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
        target_->PopAxisAlignedClip();
    }

    void DrawWaveform(const SharedState& state, D2D1_RECT_F rect) {
        const float gap = 2.5f;
        const float minBarWidth = 2.0f;
        const float availableW = rect.right - rect.left;
        size_t count = std::max<size_t>(1, static_cast<size_t>((availableW + gap) / (minBarWidth + gap)));
        count = std::min<size_t>(count, 32);

        const float barWidth = (availableW - gap * (count - 1)) / count;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        const float maxH = (rect.bottom - rect.top) * 0.86f;

        // Use a step size of 4 samples (approx 40ms) so bars aren't identical
        const size_t step = 4;

        const float t = specT_;
        for (size_t i = 0; i < count; ++i) {
            const size_t offset = (count - i) * step;
            const size_t source = (state.waveformWrite + state.waveform.size() - offset) %
                                  state.waveform.size();
            const float amp = Clamp(state.waveform[source], 0.03f, 1.0f);
            float h = std::max(3.0f, amp * maxH);
            float glow = 0.0f;

            // Track change: domino ripple. Each bar collapses to a dot, then springs
            // back up with overshoot; the ripple runs with the skip direction.
            if (t >= 0.0f && count > 1) {
                float f = static_cast<float>(i) / static_cast<float>(count - 1);
                if (specDir_ < 0.0f) f = 1.0f - f;
                const float u = (t - 0.34f * f) / 0.66f;
                float scale = 1.0f;
                if (u < 0.0f) {
                    scale = 1.0f;
                } else if (u < 0.22f) {
                    scale = 1.0f - SkipSmooth(u / 0.22f);
                } else {
                    const float v = Clamp((u - 0.22f) / 0.78f, 0.0f, 1.0f);
                    scale = 1.0f - std::exp(-5.5f * v) * std::cos(9.5f * v);
                    glow = std::exp(-6.0f * v);
                }
                h = std::max(2.0f, h * Clamp(scale, 0.0f, 1.5f));
            }

            const float x = rect.left + i * (barWidth + gap);
            D2D1_RECT_F bar = D2D1::RectF(x, centerY - h * 0.5f, x + barWidth, centerY + h * 0.5f);
            accentBrush_->SetOpacity(Clamp(0.45f + 0.5f * amp + 0.35f * glow, 0.0f, 1.0f));
            target_->FillRoundedRectangle(D2D1::RoundedRect(bar, barWidth * 0.5f, barWidth * 0.5f),
                                         accentBrush_.Get());
        }
        accentBrush_->SetOpacity(1.0f);
    }

    // Custom fork, Feature 1: Telegram card. Row 1 = avatar + name + message;
    // further rows = earlier messages that arrived while the card was up.
    void DrawTelegramNotification(const SharedState& state, D2D1_RECT_F rect) {
        const NotificationSnapshot& n = state.notification;
        if (n.telegramLines.empty() || !textFormat_ || !smallTextFormat_) return;
        if (rect.bottom - rect.top < 48.0f || rect.right - rect.left < 140.0f) return;

        const double now = NowSeconds();

        const TelegramLine& top = n.telegramLines.front();
        constexpr float kRowH = 58.0f;
        const float cy = rect.top + kRowH * 0.5f;
        const float avatarR = (kRowH - 16.0f) * 0.5f;
        // When several messages are stacked the card grows downward; the avatar
        // moves down with it (half the extra height, at most 18 px) so it does
        // not sit pinned to the top corner. Follows the height spring smoothly.
        const float extraH = std::max(0.0f, (rect.bottom - rect.top) - kRowH);
        const float avatarCy = cy + std::min(extraH * 0.5f, 18.0f);
        const D2D1_POINT_2F ac = D2D1::Point2F(rect.left + 14.0f + avatarR, avatarCy);

        // Avatar: coloured circle with initials (the listener API gives no photo).
        const std::wstring& who = top.group ? top.chat : top.sender;
        ID2D1SolidColorBrush* avatarBrush = AcquireBrush(TelegramLook::AvatarColor(who));  // per-frame pool, no allocation
        if (avatarBrush) {
            target_->FillEllipse(D2D1::Ellipse(ac, avatarR, avatarR), avatarBrush);
        }
        ID2D1SolidColorBrush* whiteBrush = AcquireBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));  // per-frame pool, no allocation
        if (whiteBrush) {
            const std::wstring initials = TelegramLook::Initials(who);
            const DWRITE_TEXT_ALIGNMENT oldAlign = textFormat_->GetTextAlignment();
            const DWRITE_PARAGRAPH_ALIGNMENT oldPara = textFormat_->GetParagraphAlignment();
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(initials.c_str(), static_cast<UINT32>(initials.size()), textFormat_.Get(),
                               D2D1::RectF(ac.x - avatarR, ac.y - avatarR, ac.x + avatarR, ac.y + avatarR),
                               whiteBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            textFormat_->SetTextAlignment(oldAlign);
            textFormat_->SetParagraphAlignment(oldPara);
        }

        // Small Telegram-blue dot on the avatar, like an app badge.
        ComPtr<ID2D1SolidColorBrush> tgBlue;
        // Telegram blue, or Discord blurple for Discord cards.
        target_->CreateSolidColorBrush(D2D1::ColorF(n.chatApp == L"Discord" ? 0x5865F2 : 0x2AABEE), &tgBlue);
        if (tgBlue && whiteBrush) {
            const float dotR = avatarR * 0.30f;
            const D2D1_POINT_2F dot = D2D1::Point2F(ac.x + avatarR * 0.72f, ac.y + avatarR * 0.72f);
            target_->FillEllipse(D2D1::Ellipse(dot, dotR, dotR), tgBlue.Get());
            target_->DrawEllipse(D2D1::Ellipse(dot, dotR, dotR), whiteBrush, 1.2f);
        }

        const float tx = ac.x + avatarR + 14.0f;
        const float right = rect.right - 16.0f;

        // Header: name (private) or group title, plus "+N" when messages stacked up.
        std::wstring header = who;
        if (n.telegramTotal > 1) {
            header += L"  ·  +" + std::to_wstring(n.telegramTotal - 1);
        }
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(header.c_str(), static_cast<UINT32>(header.size()), smallTextFormat_.Get(),
                           D2D1::RectF(tx, cy - 16.0f, right, cy - 1.0f), textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Message: in groups prefixed with the sender's name.
        const std::wstring body = top.group ? top.sender + L": " + top.text : top.text;
        textBrush_->SetOpacity(0.90f);
        DrawMarqueeText(body, D2D1::RectF(tx, cy - 1.0f, right, cy + 16.0f), textFormat_.Get(),
                        textBrush_.Get(), now, 28.0f, marqueeNotificationCache_);

        // Earlier messages, one compact row each.
        for (size_t i = 1; i < n.telegramLines.size(); ++i) {
            const TelegramLine& l = n.telegramLines[i];
            const float y = rect.top + kRowH - 4.0f + 22.0f * static_cast<float>(i - 1);
            const std::wstring row = (l.group ? l.sender + L" (" + l.chat + L")" : l.sender) + L": " + l.text;
            mutedBrush_->SetOpacity(0.80f);
            target_->DrawTextW(row.c_str(), static_cast<UINT32>(row.size()), smallTextFormat_.Get(),
                               D2D1::RectF(tx, y, right - 10.0f, y + 18.0f), mutedBrush_.Get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        mutedBrush_->SetOpacity(0.50f);
        textBrush_->SetOpacity(0.90f);

    }

    void DrawVolume(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const bool muted = state.volume.muted || state.volume.percent == 0;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f; // Softer squircle corners

        ID2D1SolidColorBrush* badgeBg = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg);

        const wchar_t* glyph = muted ? L"\uE74F" : (usingFluentIcons_ ? L"\uE767" : L"\uE993");
        textBrush_->SetOpacity(0.95f);

        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 13.0f, rect.right - 58, cy + 3.0f);
        mutedBrush_->SetOpacity(0.50f);
        const std::wstring deviceLabel =
            state.volume.deviceName.empty() ? std::wstring(Loc(L"Volume")) : state.volume.deviceName;
        target_->DrawTextW(deviceLabel.c_str(), static_cast<UINT32>(deviceLabel.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        if (muted) {
            wcscpy_s(value, ARRAYSIZE(value), Loc(L"Muted"));
        } else {
            swprintf_s(value, L"%d%%", state.volume.percent);
        }
        D2D1_RECT_F valueRect = D2D1::RectF(rect.right - 58, cy - 13.0f, rect.right - 14, cy + 3.0f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), smallTextFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        D2D1_RECT_F track = D2D1::RectF(tx, cy + 7.0f, rect.right - 14, cy + 12.0f);
        // Shared accent track, so the volume bar matches the media scrubber.
        const float pct = Clamp(state.volume.displayPercent / 100.0f, 0.0f, 1.0f);
        if (muted) {
            // Muted still shows the level, just drained of colour.
            ComPtr<ID2D1SolidColorBrush> trackBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(material_.raisedStrong, &trackBrush)) && trackBrush) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(track, 2.5f, 2.5f), trackBrush.Get());
            }
            D2D1_RECT_F fill = D2D1::RectF(track.left, track.top,
                                           track.left + (track.right - track.left) * pct,
                                           track.bottom);
            ComPtr<ID2D1SolidColorBrush> dim;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    WithAlpha(material_.textSecondary, 0.35f), &dim)) && dim) {
                target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 2.5f, 2.5f), dim.Get());
            }
        } else {
            DrawAccentTrack(track, pct, 2.5f);
        }
        accentBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawTimer(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const double now = NowSeconds();
        double remaining = state.timer.justFinished
            ? 0.0
            : (state.timer.running ? std::max(0.0, state.timer.endsAt - now)
                                    : state.timer.remainingAtPause);
        const int totalSec = state.timer.totalSeconds > 0 ? state.timer.totalSeconds : 1;
        const float progress = state.timer.justFinished
            ? 1.0f
            : Clamp(1.0f - static_cast<float>(remaining / totalSec), 0.0f, 1.0f);

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ID2D1SolidColorBrush* badgeBg = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg);

        const float ringR = badgeSz * 0.30f;
        D2D1_POINT_2F ringCenter = D2D1::Point2F((badge.left + badge.right) * 0.5f,
                                                  (badge.top + badge.bottom) * 0.5f);
        ID2D1SolidColorBrush* ringTrack = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->DrawEllipse(D2D1::Ellipse(ringCenter, ringR, ringR), ringTrack, 2.0f);

        ComPtr<ID2D1PathGeometry> geometry;
        d2dFactory_->CreatePathGeometry(&geometry);
        ComPtr<ID2D1GeometrySink> sink;
        geometry->Open(&sink);
        const float start = -3.14159265f * 0.5f;
        const float sweep = 2.0f * 3.14159265f * progress;
        const int segments = std::max(2, static_cast<int>(40 * progress));
        auto pointAt = [&](float a) {
            return D2D1::Point2F(ringCenter.x + std::cos(a) * ringR,
                                  ringCenter.y + std::sin(a) * ringR);
        };
        sink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= segments; ++i) {
            sink->AddLine(pointAt(start + sweep * i / segments));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        ComPtr<ID2D1SolidColorBrush> ringFg;
        D2D1_COLOR_F fgColor = state.timer.isBreak
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.58f, 0.0f, 1.0f);
        if (state.timer.justFinished) {
            fgColor = D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f);
        }
        target_->CreateSolidColorBrush(fgColor, &ringFg);
        target_->DrawGeometry(geometry.Get(), ringFg.Get(), 2.4f);

        if (state.timer.active && !state.timer.running) {
            ID2D1SolidColorBrush* pauseBrush = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.9f));  // per-frame pool, no allocation
            const float h = ringR * 0.7f;
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x - 2.6f, ringCenter.y - h * 0.5f,
                            ringCenter.x - 0.8f, ringCenter.y + h * 0.5f), pauseBrush);
            target_->FillRectangle(
                D2D1::RectF(ringCenter.x + 0.8f, ringCenter.y - h * 0.5f,
                            ringCenter.x + 2.6f, ringCenter.y + h * 0.5f), pauseBrush);
        }

        const float tx = badge.right + 14;
        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = state.timer.justFinished
            ? (state.timer.isBreak ? L"Break Complete" : L"Focus Complete")
            : (state.timer.isBreak ? L"Break Timer" : L"Focus Timer");
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[32] = {};
        int rem = static_cast<int>(std::ceil(remaining));
        swprintf_s(value, L"%d:%02d", rem / 60, rem % 60);
        D2D1_RECT_F valueRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);
        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 100.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const bool connected = (state.device.eventType == DeviceEventType::Connected);

        // Badge circle with colored dot
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ID2D1SolidColorBrush* badgeBg = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg);

        const wchar_t* glyph = usingFluentIcons_ ? L"\uECF0" : L"\uE88E";
        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f,  0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);

        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;

        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = connected ? L"Device Connected" : L"Device Removed";
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring& name = state.device.deviceName.empty()
            ? (state.device.isBluetoothLike ? std::wstring(L"Bluetooth") : std::wstring(L"USB Device"))
            : state.device.deviceName;
        D2D1_RECT_F nameRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        target_->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                           textFormat_.Get(), nameRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);

        mutedBrush_->SetOpacity(0.58f);
    }

    void DrawBluetoothCategoryIcon(D2D1_RECT_F badge, BluetoothDeviceCategory category) {
        const wchar_t* glyph = L"\uE702";
        switch (category) {
            case BluetoothDeviceCategory::Headphones:
                glyph = L"\uE7F6";
                break;
            case BluetoothDeviceCategory::Speaker:
                glyph = L"\uE7F5";
                break;
            case BluetoothDeviceCategory::Mouse:
                glyph = L"\uE962";
                break;
            case BluetoothDeviceCategory::Keyboard:
                glyph = L"\uE92E";
                break;
            case BluetoothDeviceCategory::Phone:
                glyph = L"\uE8EA";
                break;
            case BluetoothDeviceCategory::Gamepad:
                glyph = L"\uE7FC";
                break;
            case BluetoothDeviceCategory::Generic:
            default:
                glyph = L"\uE702";
                break;
        }

        textBrush_->SetOpacity(0.95f);
        if (iconFormat_) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            target_->DrawTextW(glyph, static_cast<UINT32>(wcslen(glyph)), iconFormat_.Get(), badge,
                               textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        textBrush_->SetOpacity(0.90f);
    }

    // Standalone 0.7: connected Bluetooth accessories with their battery, top-left
    // of the first tab (clock / player): icon + battery per device (no names).
    void DrawBluetoothBadges(const SharedState& state, float left, float cy, float maxRight) {
        const std::vector<BluetoothBatteryItem>& items = state.bluetoothBatteries;
        if (items.empty() || !smallTextFormat_ || !iconFormat_ || maxRight - left < 40.0f) {
            return;
        }
        const DWRITE_TEXT_ALIGNMENT oldAlign = smallTextFormat_->GetTextAlignment();
        const DWRITE_PARAGRAPH_ALIGNMENT oldPara = smallTextFormat_->GetParagraphAlignment();
        smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        smallTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        float x = left;
        for (const BluetoothBatteryItem& it : items) {
            const float chipW = 20.0f + (it.percent >= 0 ? 56.0f : 0.0f);
            if (x + chipW > maxRight) break;

            DrawBluetoothCategoryIcon(D2D1::RectF(x, cy - 10.0f, x + 18.0f, cy + 10.0f), it.category);
            x += 22.0f;

            if (it.percent >= 0) {
                // Battery outline + fill + tip. Red at 20 % and below, green while charging.
                const D2D1_COLOR_F base = material_.textPrimary;
                const D2D1_COLOR_F fillColor = it.charging        ? D2D1::ColorF(0x30D158)
                                               : it.percent <= 20 ? D2D1::ColorF(0xFF453A)
                                                                  : base;
                const D2D1_RECT_F body = D2D1::RectF(x, cy - 5.0f, x + 20.0f, cy + 5.0f);
                if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(base, 0.55f * settingsOpacity_))) {
                    target_->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.5f, 2.5f), b, 1.0f);
                    target_->FillRectangle(D2D1::RectF(body.right + 1.0f, cy - 2.0f, body.right + 2.5f, cy + 2.0f), b);
                }
                const float innerW = 16.0f * Clamp(it.percent / 100.0f, 0.06f, 1.0f);
                if (ID2D1SolidColorBrush* b = AcquireBrush(WithAlpha(fillColor, 0.95f * settingsOpacity_))) {
                    target_->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(body.left + 2.0f, body.top + 2.0f, body.left + 2.0f + innerW,
                                                      body.bottom - 2.0f),
                                          1.2f, 1.2f),
                        b);
                }
                x += 26.0f;
                wchar_t pct[8];
                swprintf_s(pct, L"%d%%", it.percent);
                textBrush_->SetOpacity(0.92f * settingsOpacity_);
                target_->DrawTextW(pct, static_cast<UINT32>(wcslen(pct)), smallTextFormat_.Get(),
                                   D2D1::RectF(x, cy - 9.0f, x + 34.0f, cy + 9.0f), textBrush_.Get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                x += MeasureTextWidthCached(pct, smallTextFormat_.Get(), btPctWidthCache_) + 12.0f;
            } else {
                x += 6.0f;
            }
        }
        textBrush_->SetOpacity(1.0f);
        mutedBrush_->SetOpacity(1.0f);
        smallTextFormat_->SetTextAlignment(oldAlign);
        smallTextFormat_->SetParagraphAlignment(oldPara);
    }

    void DrawBluetoothDevice(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;

        const bool connected = state.bluetoothDevice.connected;
        const int battery = state.bluetoothDevice.batteryPercent;
        const bool hasBattery = g_settings.bluetoothShowBattery && battery >= 0;

        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ID2D1SolidColorBrush* badgeBg = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg);

        DrawBluetoothCategoryIcon(badge, state.bluetoothDevice.category);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        D2D1_COLOR_F dotColor = connected
            ? D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f)
            : D2D1::ColorF(1.0f, 0.27f, 0.22f, 1.0f);
        target_->CreateSolidColorBrush(dotColor, &dotBrush);
        D2D1_POINT_2F dotCenter = D2D1::Point2F(badge.right - 4.5f, badge.bottom - 4.5f);
        target_->FillEllipse(D2D1::Ellipse(dotCenter, 4.5f, 4.5f), dotBrush.Get());

        const float tx = badge.right + 14.0f;
        const float rightEdge = hasBattery ? rect.right - 62.0f : rect.right - 14.0f;

        // Standalone 0.7.2: no device names anywhere (the user's choice) -- the
        // icon says what it is: "Bluetooth" on top, connected / disconnected below.
        mutedBrush_->SetOpacity(0.50f);
        const std::wstring label = Loc(L"Bluetooth");
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rightEdge, cy - 1.0f);
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        textBrush_->SetOpacity(0.95f);
        const std::wstring status = connected ? Loc(L"Connected") : Loc(L"Disconnected");
        D2D1_RECT_F statusRect = D2D1::RectF(tx, cy - 1.0f, rightEdge, cy + 16.0f);
        target_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()),
                           textFormat_.Get(), statusRect, textBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (hasBattery) {
            const float colCenter = rect.right - 31.0f;
            const float bw = 16.0f;
            const float bh = 8.5f;
            const float nubW = 1.6f;
            const float totalW = bw + nubW;
            const float batLeft = colCenter - totalW * 0.5f;
            const float batY = cy - 9.0f;

            D2D1_RECT_F batRect = D2D1::RectF(batLeft, batY - bh * 0.5f,
                                              batLeft + bw, batY + bh * 0.5f);
            ID2D1SolidColorBrush* batBorder = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.75f));  // per-frame pool, no allocation
            target_->DrawRoundedRectangle(D2D1::RoundedRect(batRect, 1.5f, 1.5f), batBorder, 1.2f);
            D2D1_RECT_F nub = D2D1::RectF(batRect.right, batY - 2.0f, batRect.right + nubW, batY + 2.0f);
            target_->FillRectangle(nub, batBorder);

            const float pct = Clamp(battery / 100.0f, 0.0f, 1.0f);
            D2D1_RECT_F fill = D2D1::RectF(batRect.left + 1.5f, batRect.top + 1.5f,
                                           batRect.left + 1.5f + (bw - 3.0f) * pct, batRect.bottom - 1.5f);
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            D2D1_COLOR_F fillColor = battery <= 20 ? D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f)
                                                    : D2D1::ColorF(1, 1, 1, 0.92f);
            target_->CreateSolidColorBrush(fillColor, &fillBrush);
            target_->FillRoundedRectangle(D2D1::RoundedRect(fill, 0.8f, 0.8f), fillBrush.Get());

            wchar_t pctBuf[16] = {};
            swprintf_s(pctBuf, L"%d%%", battery);
            D2D1_RECT_F pctRect = D2D1::RectF(colCenter - 25.0f, cy - 1.0f, colCenter + 25.0f, cy + 15.0f);
            textBrush_->SetOpacity(0.85f);
            if (smallTextFormat_) {
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                target_->DrawTextW(pctBuf, static_cast<UINT32>(wcslen(pctBuf)), smallTextFormat_.Get(),
                                   pctRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                smallTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }

        mutedBrush_->SetOpacity(0.58f);
        textBrush_->SetOpacity(0.90f);
    }

    void DrawBattery(const SharedState& state, D2D1_RECT_F rect) {
        if (rect.bottom - rect.top < 24.0f || rect.right - rect.left < 140.0f) return;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float badgeSz = (rect.bottom - rect.top) - 16.0f;
        D2D1_RECT_F badge = D2D1::RectF(rect.left + 14, cy - badgeSz * 0.5f,
                                        rect.left + 14 + badgeSz, cy + badgeSz * 0.5f);
        const float br = badgeSz * 0.35f;

        ID2D1SolidColorBrush* badgeBg = AcquireBrush(material_.raisedStrong);  // per-frame pool, no allocation
        target_->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), badgeBg);

        // Draw battery vector icon
        const float bx = badge.left + badgeSz * 0.25f;
        const float by = cy - badgeSz * 0.22f;
        const float bw = badgeSz * 0.45f;
        const float bh = badgeSz * 0.44f;
        D2D1_RECT_F batRect = D2D1::RectF(bx, by, bx + bw, by + bh);

        ID2D1SolidColorBrush* batBorder = AcquireBrush(D2D1::ColorF(1, 1, 1, 0.85f));  // per-frame pool, no allocation
        target_->DrawRoundedRectangle(D2D1::RoundedRect(batRect, 2, 2), batBorder, 1.5f);

        // Battery Terminal (Nub)
        D2D1_RECT_F nubRect = D2D1::RectF(batRect.right, cy - 3, batRect.right + 2.5f, cy + 3);
        target_->FillRectangle(nubRect, batBorder);

        // Battery Fill
        const float pct = Clamp(state.battery.percent / 100.0f, 0.0f, 1.0f);
        D2D1_RECT_F fillRect = D2D1::RectF(batRect.left + 2, batRect.top + 2,
                                           batRect.left + 2 + (bw - 4) * pct, batRect.bottom - 2);

        ComPtr<ID2D1SolidColorBrush> batFill;
        if (state.battery.low) {
            target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.23f, 0.18f, 1.0f), &batFill); // Red
        } else if (state.battery.charging) {
            target_->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.83f, 0.38f, 1.0f), &batFill); // Green
        } else {
            target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), &batFill); // White
        }
        target_->FillRoundedRectangle(D2D1::RoundedRect(fillRect, 1, 1), batFill.Get());

        // Text Labels
        const float tx = badge.right + 14;
        D2D1_RECT_F labelRect = D2D1::RectF(tx, cy - 16.0f, rect.right - 14.0f, cy - 1.0f);
        mutedBrush_->SetOpacity(0.50f);
        std::wstring label = state.battery.charging ? L"Power Connected" : L"Battery Alert";
        target_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           smallTextFormat_.Get(), labelRect, mutedBrush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);

        wchar_t value[128] = {};
        if (state.battery.secondsRemaining != BATTERY_LIFE_UNKNOWN && !state.battery.charging) {
            const DWORD minutes = state.battery.secondsRemaining / 60;
            swprintf_s(value, ARRAYSIZE(value), L"%d%% \u2022 %luh %02lum left",
                       state.battery.percent, minutes / 60, minutes % 60);
        } else {
            swprintf_s(value, ARRAYSIZE(value), L"%d%%", state.battery.percent);
        }

        D2D1_RECT_F valueRect = D2D1::RectF(tx, cy - 1.0f, rect.right - 14.0f, cy + 16.0f);
        textBrush_->SetOpacity(0.95f);
        target_->DrawTextW(value, static_cast<UINT32>(wcslen(value)), textFormat_.Get(),
                           valueRect, textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        textBrush_->SetOpacity(0.90f);
    }

    void DrawProgress(const SharedState& state, D2D1_RECT_F rect) {
        wchar_t buffer[64] = {};
        swprintf_s(buffer, L"Progress %d%%", state.progress.percent);
        IDWriteTextFormat* fmt = idleTextFormat_ ? idleTextFormat_.Get() : textFormat_.Get();
        target_->DrawTextW(buffer, static_cast<UINT32>(wcslen(buffer)), fmt,
                           rect,
                           textBrush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawProgressRing(D2D1_RECT_F rect, int percent) {
        ComPtr<ID2D1PathGeometry> geometry;
        d2dFactory_->CreatePathGeometry(&geometry);
        ComPtr<ID2D1GeometrySink> sink;
        geometry->Open(&sink);

        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const float rx = (rect.right - rect.left) * 0.5f + 6.0f;
        const float ry = (rect.bottom - rect.top) * 0.5f + 6.0f;
        const float start = -3.14159265f * 0.5f;
        const float sweep = 2.0f * 3.14159265f * Clamp(percent / 100.0f, 0.0f, 1.0f);
        const int segments = std::max(2, static_cast<int>(48 * percent / 100.0f));

        auto pointAt = [&](float a) {
            return D2D1::Point2F(cx + std::cos(a) * rx, cy + std::sin(a) * ry);
        };

        sink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= segments; ++i) {
            const float a = start + sweep * i / segments;
            sink->AddLine(pointAt(a));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        accentBrush_->SetOpacity(0.92f);
        target_->DrawGeometry(geometry.Get(), accentBrush_.Get(), 3.0f);
        accentBrush_->SetOpacity(1.0f);
    }

    HWND hwnd_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    int bitmapWidth_ = 0;     // logical size of the frame being drawn
    int bitmapHeight_ = 0;
    int bitmapCapW_ = 0;      // allocated size of the DIB (>= logical size)
    int bitmapCapH_ = 0;

    // Fixed-stage bookkeeping (see Render)
    POINT lastOrigin_ = {0, 0};
    RECT lastBounds_ = {0, 0, 0, 0};
    BYTE lastAlpha_ = 0;
    bool stageValid_ = false;
    double lastFullPaint_ = -1e9;

    // Pills drawn this frame, in client px, for the DWM backdrop windows (#59).
    D2D1_RECT_F pillRects_[DwmBackdrop::kMaxPills] = {};
    float pillRadii_[DwmBackdrop::kMaxPills] = {};
    int pillCount_ = 0;

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> smallTextFormat_;
    ComPtr<IDWriteTextFormat> boldTextFormat_;
    ComPtr<IDWriteTextFormat> hugeTextFormat_;
    ComPtr<IDWriteTextFormat> clockFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    ComPtr<IDWriteTextFormat> mediaPlayIconFormat_;
    ComPtr<IDWriteTextFormat> mediaNavIconFormat_;
    bool usingFluentIcons_ = true;
    ComPtr<IDWriteTextFormat> idleTextFormat_;
    ComPtr<IDWriteTextFormat> calDayLargeFormat_;
    ComPtr<IDWriteTextFormat> calGridFormat_;
    ComPtr<IDWriteTextFormat> timeDashboardFormat_;
    ComPtr<IDWriteTextFormat> dateDashboardFormat_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> redBrush_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    ComPtr<ID2D1SolidColorBrush> tintBrush_;
    ComPtr<ID2D1SolidColorBrush> shadowBrush_;
    ComPtr<ID2D1SolidColorBrush> micDotBrush_;
    ComPtr<ID2D1SolidColorBrush> micGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> camDotBrush_;
    ComPtr<ID2D1SolidColorBrush> camGlowBrush_;
    ComPtr<ID2D1SolidColorBrush> scratchColorBrush_;
    MarqueeLayoutCache marqueeTitleCache_;
    MarqueeLayoutCache marqueeArtistCache_;
    MarqueeLayoutCache marqueeAlbumCache_;
    MarqueeLayoutCache marqueeNotificationCache_;
    ComPtr<ID2D1Bitmap> artBitmap_;
    ComPtr<ID2D1Bitmap> mediaSourceIconBitmap_;
    uint64_t artGeneration_ = 0;
    SYSTEMTIME calendarCachedDate_{};
    std::wstring calendarCachedMonthName_;
    std::wstring calendarCachedWeekdayName_;
    std::array<std::wstring, 7> calendarCachedInitials_;  // indexed 0 = Sunday
    int calendarCachedFirstDay_ = 0;                       // resolved, 0 = Sunday
    int calendarCachedFirstDaySetting_ = -2;               // -2 forces the first fill
    ComPtr<IDWriteTextFormat> weatherDescFormat_;
    float weatherDescFormatSize_ = -1.0f;
    uint64_t mediaSourceIconGeneration_ = 0;

    float  hoverScale_ = 1.0f;       // pill scale set by Render(); gives the resting expanded size
    float  islandSizeLastW_ = 0.0f;  // island resize tracking (see DrawMedia)
    float  islandSizeLastH_ = 0.0f;
    double islandSizeMovedAt_ = -1e9;
    bool   islandSettled_ = true;    // false while the island is still springing to a new size
    float settingsOpacity_ = 0.96f;
    D2D1_COLOR_F pillBgColor_ = D2D1::ColorF(0.031f, 0.031f, 0.039f, 1.0f);
    // Design tokens for the current frame, rebuilt by EnsureBrushes.
    MaterialTokens material_{};
    // Accent color lerp: smoothly transition between successive sampled accents
    // so track changes don't produce a jarring instant color pop.
    D2D1_COLOR_F currentAccent_ = D2D1::ColorF(0x4cc9f0);
    double       lastAccentTime_ = -1.0;  // -1 = not yet set (will snap on first frame)
    float        mediaBtnPress_[3] = {0.0f, 0.0f, 0.0f};
    double       lastMediaBtnTime_ = -1.0;
    ComPtr<ID2D1PathGeometry> skipTriFull_;   // skip icons: rounded triangle, left edge at x=0
    ComPtr<ID2D1PathGeometry> skipTriNotch_;  // same, with the front triangle's gap cut out
    float        playMorph_ = 0.0f;  // play/pause icon: 0 = pause bars, 1 = play triangle
    ComPtr<ID2D1StrokeStyle> roundJoinStyle_;
    double       artOuterLast_ = -1.0;  // shared art pass: last frame it ran
    float        artOuterAlpha_ = 1.0f; // shared art pass: eased opacity
    bool         upkeepPending_ = false;  // z-order upkeep requested, throttled in Render()
    double       lastUpkeepAt_ = -1e9;    // last time ApplyOverlayZOrder actually ran
    bool         islandHiding_ = false; // target size is 0 (auto-hide / fullscreen): art shrinks with the pill
    float        artMorphLin_ = 0.0f;   // art circle->square progress (linear, before easing)
    double       artMorphLast_ = -1.0;
    float        artBadgeAlpha_ = 1.0f; // source badge opacity on the expanded art
    bool         specInit_ = false;     // audio spectrum state
    double       specLastDrawn_ = -1.0;
    double       specStart_ = -1.0;     // -1 = no track-change animation running
    size_t       specKey_ = 0;
    float        specT_ = -1.0f;        // 0..1 animation progress, <0 when idle
    float        specDir_ = 1.0f;
    float        specPhase_ = 0.0f;
    float        specBass_ = 0.0f;
    float        specBassSlow_ = 0.0f;
    float        specKick_ = 0.0f;
    float        specPeak_[kSpectrumBands] = {};
    float        specPeakVel_[kSpectrumBands] = {};
    float        progressAmp_ = 1.0f;   // wavy/squiggle bar: wave strength, eases to 0 while paused
    double       progressLastTime_ = -1.0;

    // ── Source dock ─────────────────────────────────────────────────────────
    bool         dockInit_ = false;
    int          dockSlotCount_ = 0;
    double       dockLastDrawn_ = -1.0;
    float        dockIndPos_ = 0.0f;
    float        dockIndFrom_ = 0.0f;
    float        dockIndTarget_ = 0.0f;
    double       dockIndStart_ = -1.0;
    float        dockHover_[MediaLayout::kDockMaxSlots] = {};
    float        dockPress_[MediaLayout::kDockMaxSlots] = {};
    ComPtr<ID2D1Bitmap> dockIconBitmap_[MediaLayout::kDockMaxSlots];
    uint64_t     dockIconGen_[MediaLayout::kDockMaxSlots] = {};
    MarqueeLayoutCache marqueeSourceCache_;

    // ── Tab transition state ────────────────────────────────────────────────
    int          tabContext_ = -1;      // 0 = media view, 1 = idle view (ids mean different things)
    int          tabCount_ = 0;
    bool         tabInit_ = false;      // false = next draw snaps to the selected tab
    bool         tabAnimating_ = false;
    float        tabPos_ = 0.0f;        // continuous position the spring moves
    float        tabTarget_ = 0.0f;
    float        tabVel_ = 0.0f;
    float        tabDir_ = 1.0f;        // +1 = last move went forward, -1 = backward
    double       tabLastDrawn_ = -1.0;
    ComPtr<ID2D1Layer> tabLayer_;       // reused layer for the tab fade

    // ── Cached resources (see "Cached Direct2D resources") ──────────────────
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushPool_;
    size_t brushPoolUsed_ = 0;
    std::vector<ComPtr<ID2D1Layer>> layerPool_;
    size_t layerPoolUsed_ = 0;
    ComPtr<ID2D1SolidColorBrush> raisedBrush_;        // DrawCard fill
    ComPtr<ID2D1SolidColorBrush> raisedStrongBrush_;  // active card, badges, tracks
    LinearGradientSlot depthGrad_;
    LinearGradientSlot trackGrad_;
    RadialGradientSlot bloomGrad_;
    LinearGradientSlot glassSheenGrad_;  // custom fork: Liquid Glass
    RadialGradientSlot clockGlowGrad_;   // standalone 0.9: cached instead of per frame
    LinearGradientSlot sparkGrad_[kStockSlots];
    LinearGradientSlot glassRimGrad_;
    ShapeEntry shapeCache_[kShapeCacheSize];
    uint64_t shapeClock_ = 0;
};

Activity ActivityForKind(IslandKind kind, const Settings& settings, const SharedState& state) {
    Activity activity;
    activity.kind = kind;

    switch (kind) {
        case IslandKind::Media:
            activity.width = 150.0f;
            activity.height = 44.0f;
            break;
        case IslandKind::Progress:
            activity.width = 230.0f;
            activity.height = 48.0f;
            break;
        case IslandKind::Notification:
            activity.width = 360.0f;
            activity.height = 58.0f;
            // Custom fork: a Telegram card grows by one row per stacked message.
            if (state.notification.isTelegram) {
                const size_t lines = state.notification.telegramLines.size();
                activity.width = 380.0f;
                if (lines > 1) {
                    activity.height = 58.0f + 22.0f * static_cast<float>(lines - 1) + 6.0f;
                }
            }
            break;
        case IslandKind::Volume:
        case IslandKind::Brightness:
            activity.width = 300.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::BatteryLow:
            activity.width = 290.0f;
            activity.height = 52.0f;
            break;
        case IslandKind::Device:
            activity.width = 240.0f;
            activity.height = 50.0f;
            break;
        case IslandKind::Bluetooth:
            activity.width = 280.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::Timer:
            activity.width = 260.0f;
            activity.height = 54.0f;
            break;
        case IslandKind::Idle:
        default:
            if (settings.autoHideIdleSeconds == -1 && !state.system.micActive && !state.system.cameraActive) {
                activity.width = 0.0f;
                activity.height = 0.0f;
            } else {
                // Seed only. The real collapsed width is measured from the clock
                // and weather strings by Renderer::MeasureIdleStrip and applied in
                // the render loop -- this function has no DWrite access. A fixed
                // width here was the whole bug behind windhawk-mods#5086: dead
                // air around a short "9:41", and clipping on "10:41:32 PM" once
                // Text size reached 140.
                activity.width = settings.weather ? 170.0f : 96.0f;
                activity.height = IdleStripLayout::kHeight;
            }
            break;
    }

    activity.width *= settings.sizeScale;
    activity.height *= settings.sizeScale;
    return activity;
}

std::vector<IslandKind> ChooseActivities(const SharedState& state, const Settings& settings, double now) {
    std::vector<IslandKind> activities;

    if (state.device.active && now < state.device.expiresAt) {
        activities.push_back(IslandKind::Device);
    }
    if (settings.bluetoothIndicator && state.bluetoothDevice.active &&
        now < state.bluetoothDevice.expiresAt) {
        activities.push_back(IslandKind::Bluetooth);
    }
    if (settings.volume && state.volume.active && now < state.volume.expiresAt) {
        activities.push_back(IslandKind::Volume);
    }
    if (settings.brightness && state.brightness.active && now < state.brightness.expiresAt) {
        activities.push_back(IslandKind::Brightness);
    }
    if (state.notification.active && now < state.notification.expiresAt) {
        activities.push_back(IslandKind::Notification);
    }
    if (settings.battery && state.battery.active && now < state.battery.expiresAt) {
        activities.push_back(IslandKind::BatteryLow);
    }
    if (settings.progress && state.progress.active) {
        activities.push_back(IslandKind::Progress);
    }
    if (settings.timerEnabled &&
        ((state.timer.justFinished && now < state.timer.finishedExpiresAt) ||
         state.timer.active)) {
        activities.push_back(IslandKind::Timer);
    }
    if (settings.media && state.media.available) {
        activities.push_back(IslandKind::Media);
    }

    if (activities.empty()) {
        activities.push_back(IslandKind::Idle);
    }

    return activities;
}

// --- Low-level mouse hook: wakes the parked render thread when the cursor
// approaches where the (currently OS-hidden) island sits, so hover-to-unhide
// keeps working without polling GetCursorPos every frame.
HHOOK g_mouseHook = nullptr;
HANDLE g_mouseThread = nullptr;
DWORD g_mouseThreadId = 0;

// Wakes the mouse thread so it re-evaluates whether the wake hook is needed.
// Posted whenever the island parks or unparks, so the hook is installed and
// removed in step with that rather than on the thread's backstop timeout.
inline void NotifyMouseThreadParkedChanged() {
    if (g_mouseThreadId != 0) {
        PostThreadMessageW(g_mouseThreadId, WM_NULL, 0, 0);
    }
}
std::atomic<int64_t> g_lastMouseWakeCheckMs = 0;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE &&
        g_settings.unhideOnHover &&
        g_autoHiddenParked.load(std::memory_order_relaxed)) {
        const int64_t nowMs = static_cast<int64_t>(GetTickCount64());
        const int64_t last = g_lastMouseWakeCheckMs.load(std::memory_order_relaxed);
        if (nowMs - last >= 60) {  // ~16Hz check rate for responsive unhide
            g_lastMouseWakeCheckMs.store(nowMs, std::memory_order_relaxed);
            auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            HWND hwnd = g_hwnd;
            RECT dockRect = GetIslandDockRect();
            if (hwnd && PtInRect(&dockRect, info->pt)) {
                g_autoHiddenParked = false;
                PostMessageW(hwnd, WM_APP_MOUSE_WAKE, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// A system-wide WH_MOUSE_LL routes every mouse event on the desktop through this
// thread, so it is only installed while it can actually do something: the hook's
// whole job is to notice a hover over a *parked* island.
//
// Gating on the settings instead was not enough. UnhideOnHover and
// AutoHideFullscreen both default to true, so every user on defaults still got
// the hook from startup -- the exact case the gate was added to avoid. Tying it
// to the parked state means it exists only while the island is actually hidden.
static bool MouseWakeHookWanted() {
    return g_settings.unhideOnHover &&
           g_autoHiddenParked.load(std::memory_order_relaxed);
}

DWORD WINAPI MouseThreadProc(void*) {
    while (!g_hwnd && WaitForSingleObject(g_stopEvent, 10) == WAIT_TIMEOUT) {
    }

    bool quit = false;
    while (!quit && WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        const bool wanted = MouseWakeHookWanted();
        if (wanted && !g_mouseHook) {
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        } else if (!wanted && g_mouseHook) {
            UnhookWindowsHookEx(g_mouseHook);
            g_mouseHook = nullptr;
        }

        // A low-level hook is delivered through the installing thread's message
        // queue, so this has to keep pumping while the hook is up.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) {
            break;
        }

        // Blocks until the stop event fires, a message arrives, or the timeout.
        // The render thread posts a WM_NULL when it parks or unparks, so the hook
        // is reconciled immediately rather than up to a tick later; the timeout
        // is only a backstop in case a transition is ever missed. This is a timed
        // wait, not a spin, so an idle mouse thread costs nothing.
        MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1000, QS_ALLINPUT);
    }

    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    return 0;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static POINT s_touchStart = {0, 0};
    static ULONGLONG s_touchStartTime = 0;
    switch (msg) {
        case WM_CREATE:
            return 0;

        // Both of these touch the window from the thread that owns it, the only
        // thread allowed to bind a hot key to it or reshape it.
        case WM_APP_APPLY_HOTKEY:
            ApplyHideShowHotkey();
            return 0;

        case WM_APP_APPLY_BACKDROP:
            ApplyBackdropMaterial(hwnd);
            return 0;

        case WM_APP_MOUSE_WAKE:
            // No-op payload — its only job is to wake MsgWaitForMultipleObjects
            // and get drained by the PeekMessage pump.
            return 0;

        case WM_DEVICECHANGE: {
            // DBT_DEVICEARRIVAL = 0x8000, DBT_DEVICEREMOVECOMPLETE = 0x8004
            if (wParam == 0x8000 || wParam == 0x8004) {
                bool arrived = (wParam == 0x8000);
                std::wstring devName;
                bool isBt = false;

                if (lParam) {
                    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
                    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                        devName = L"USB Drive";
                    } else if (hdr->dbch_devicetype == DBT_DEVTYP_PORT) {
                        devName = L"COM Device";
                    } else {
                        // Generic/Bluetooth OEM
                        isBt = true;
                        devName = L"Bluetooth Device";
                    }
                }

                {
                    std::lock_guard lock(g_stateMutex);
                    g_state.device.active = true;
                    g_state.device.eventType = arrived ? DeviceEventType::Connected
                                                       : DeviceEventType::Disconnected;
                    g_state.device.deviceName = devName;
                    g_state.device.isBluetoothLike = isBt;
                    g_state.device.expiresAt = NowSeconds() + 3.0;
                }
                TriggerNudge();
            }
            return 0;
        }

        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMECRITICAL) {
                if (g_settingsChangedEvent) {
                    SetEvent(g_settingsChangedEvent);
                }
                {
                    std::lock_guard lock(g_stateMutex);
                    g_state.weather.lastUpdated = 0.0;
                }
                TriggerNudge();
            }
            return TRUE;
        }




        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            // The window is now a big transparent stage, so only the pill itself
            // is clickable; everything else falls through to the windows below.
            if (g_pillClientRect.valid) {
                const PillClientRect& r = g_pillClientRect;
                if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom) {
                    return HTTRANSPARENT;
                }
                return HTCLIENT;
            }

            RECT clientRect = {};
            GetClientRect(hwnd, &clientRect);

            const int minX = static_cast<int>(kRenderPadX);
            const int maxX = clientRect.right - static_cast<int>(kRenderPadX);
            const int minY = (g_settings.notchStyle || g_settings.borderMergedMode) ? 0 : static_cast<int>(kRenderPadY);
            const int maxY = clientRect.bottom - static_cast<int>(kRenderPadY);

            if (maxX > minX && maxY > minY) {
                if (pt.x < minX || pt.x > maxX || pt.y < minY || pt.y > maxY) {
                    return HTTRANSPARENT;
                }
            }
            return HTCLIENT;
        }

        case WM_APP_LAYOUT_CHANGED:
            g_layoutDirty = true;
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                if (g_scrubbing.load()) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }

                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);

                bool mediaActive = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                }

                const int currentTab = NormalizedTabIndex(g_settings);
                if (mediaActive && currentTab == 0) {
                    const MediaContentPoint cp = MediaContentFromClient(pt.x, pt.y);
                    const bool hoverClickable = MediaArtHitTest(cp) ||
                                               MediaTransportHitTest(cp) != -1 ||
                                               MediaScrubFractionFromContent(cp) >= 0.0f ||
                                               g_hoveredSourceSlot.load(std::memory_order_relaxed) >= 0;
                    if (hoverClickable) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }

            }
            break;

        case WM_LBUTTONDOWN:
            {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                s_touchStart.x = xPos;
                s_touchStart.y = yPos;
                s_touchStartTime = GetTickCount64();

                bool mediaActive = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                }

                const int currentTab = NormalizedTabIndex(g_settings);
                if (mediaActive && currentTab == 0) {
                    const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);

                    // Source dock: remember the held slot for its press animation.
                    {
                        const DockHit dock = ResolveSourceDockHit(cp);
                        if (dock.slot >= 0) {
                            g_pressedSourceSlot = dock.slot;
                            g_layoutDirty = true;
                            return 0;
                        }
                    }

                    const int cmd = MediaTransportHitTest(cp);
                    if (cmd != -1) {
                        g_pressedMediaButton = cmd;
                        SetCapture(hwnd);
                        g_layoutDirty = true;
                        return 0;
                    }

                    const float fraction = MediaScrubFractionFromContent(cp);
                    if (fraction >= 0.0f) {
                        g_scrubDragFraction.store(fraction, std::memory_order_relaxed);
                        g_scrubbing = true;
                        g_lastLiveSeekTime = 0.0;
                        SetCapture(hwnd);
                        g_layoutDirty = true;
                        return 0;
                    }
                }
            }
            break;

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            const int xPos = GET_X_LPARAM(lParam);
            const int yPos = GET_Y_LPARAM(lParam);

            if (g_scrubbing.load()) {
                const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);
                const float fraction = MediaScrubFractionUnbounded(cp);
                if (fraction < 0.0f) {
                    return 0;
                }
                g_scrubDragFraction.store(fraction, std::memory_order_relaxed);
                g_layoutDirty = true;

                const double now = NowSeconds();
                if (now - g_lastLiveSeekTime.load(std::memory_order_relaxed) >= 0.15) {
                    g_lastLiveSeekTime.store(now, std::memory_order_relaxed);
                    int64_t endTicks = 0;
                    {
                        std::lock_guard lock(g_stateMutex);
                        endTicks = g_state.media.endTicks;
                    }
                    if (endTicks > 0) {
                        SeekMediaToTicks(static_cast<int64_t>(fraction * endTicks));
                    }
                }
                return 0;
            }

            int hovered = -1;
            bool mediaActive = false;
            {
                std::lock_guard lock(g_stateMutex);
                mediaActive = g_settings.media && g_state.media.available;
            }
            const int currentTab = NormalizedTabIndex(g_settings);

            int hoveredSlot = -1;
            if (mediaActive && currentTab == 0) {
                const MediaContentPoint mediaPt = MediaContentFromClient(xPos, yPos);
                hoveredSlot = ResolveSourceDockHit(mediaPt).slot;
                // The dock's right edge overlaps the prev button's slop; the dock wins.
                hovered = hoveredSlot >= 0 ? -1 : MediaTransportHitTest(mediaPt);
            }

            if (g_hoveredMediaButton.exchange(hovered) != hovered) {
                g_layoutDirty = true;
            }
            if (g_hoveredSourceSlot.exchange(hoveredSlot) != hoveredSlot) {
                g_layoutDirty = true;
            }

            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hoveredSourceSlot.exchange(-1) != -1 || g_pressedSourceSlot.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            if (g_hoveredMediaButton.exchange(-1) != -1) {
                g_layoutDirty = true;
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd) {
                if (g_scrubbing.exchange(false)) {
                    g_layoutDirty = true;
                }
                if (g_pressedMediaButton.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }
                if (g_hoveredMediaButton.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }
            }
            return 0;

        case WM_LBUTTONUP:
            {
                if (g_scrubbing.load()) {
                    g_scrubbing = false;
                    ReleaseCapture();

                    const float finalFraction = Clamp(g_scrubDragFraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    int64_t endTicks = 0;
                    {
                        std::lock_guard lock(g_stateMutex);
                        endTicks = g_state.media.endTicks;
                    }
                    if (endTicks > 0) {
                        const int64_t targetTicks = static_cast<int64_t>(finalFraction * endTicks);
                        SeekMediaToTicks(targetTicks);
                        std::lock_guard lock(g_stateMutex);
                        g_state.media.positionTicks = targetTicks;
                        g_state.media.lastUpdatedTicks = GetTickCount64();
                    }
                    g_layoutDirty = true;
                    return 0;
                }

                if (g_pressedMediaButton.load() != -1) {
                    g_pressedMediaButton = -1;
                    ReleaseCapture();
                    g_layoutDirty = true;
                }
                if (g_pressedSourceSlot.exchange(-1) != -1) {
                    g_layoutDirty = true;
                }

                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                ULONGLONG now = GetTickCount64();
                if (s_touchStartTime > 0 && (now - s_touchStartTime) < 500) {
                    const int dx = xPos - s_touchStart.x;
                    const int dy = yPos - s_touchStart.y;
                    const int travel = abs(dy) > abs(dx) ? dy : dx;  // dominant axis wins
                    if (abs(travel) > 40) {
                        // Swipe up / left -> next tab, down / right -> previous tab
                        if (ActiveTabCount(GetSettingsCopy()) > 1) {
                            StepIdleTab(travel > 0 ? -1 : 1);
                        }
                        s_touchStartTime = 0;
                        return 0; // Consume swipe gesture
                    }
                }
                s_touchStartTime = 0;

                bool mediaActive = false;
                std::vector<IslandKind> kinds;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaActive = g_settings.media && g_state.media.available;
                    kinds = ChooseActivities(g_state, g_settings, NowSeconds());
                }

                // Custom fork, Feature 3: the Do Not Disturb button on the expanded Time tab.
                if (g_settings.quietButton && !kinds.empty() &&
                    ((kinds[0] == IslandKind::Idle && !g_settings.gameOverlay) ||
                     kinds[0] == IslandKind::Media) &&
                    NormalizedTabIndex(g_settings) == 0 &&
                    QuietButtonHitTest(MediaContentFromClient(xPos, yPos))) {
                    QuietMode::Set(true, /*fromToggle=*/true);
                    return 0;
                }
                // Standalone 0.4/0.5: the round buttons next to it -- settings, power.
                if (g_settings.quietButton && !kinds.empty() &&
                    ((kinds[0] == IslandKind::Idle && !g_settings.gameOverlay) ||
                     kinds[0] == IslandKind::Media) &&
                    NormalizedTabIndex(g_settings) == 0) {
                    const TopButton button = TopButtonHitTest(MediaContentFromClient(xPos, yPos));
                    if (button == TopButton::Settings) {
                        OpenLauncher();
                        return 0;
                    }
                    if (button == TopButton::Power) {
                        QuietMode::RequestAnimatedExit();
                        return 0;
                    }
                }

                // Clicking a Telegram / Discord card dismisses it and opens the app.
                // In a split layout (kinds.size() > 1), don't dismiss if clicking on the secondary pill on the right.
                const bool onSecondarySplit = (kinds.size() > 1 && g_pillClientRect.valid &&
                                               xPos > (g_pillClientRect.left + (g_pillClientRect.right - g_pillClientRect.left) * 0.5f));
                if (!kinds.empty() && !onSecondarySplit &&
                    kinds[0] == IslandKind::Notification) {
                    bool openTelegram = false;  // custom fork: a chat card opens its app
                    std::wstring telegramAumid;
                    std::wstring chatApp;
                    {
                        std::lock_guard lock(g_stateMutex);
                        openTelegram = g_state.notification.isTelegram;
                        telegramAumid = g_state.notification.sourceAumid;
                        chatApp = g_state.notification.chatApp;
                        g_state.notification.active = false;
                        g_state.notification.expiresAt = 0.0;
                    }
                    g_clickExpanded = false;
                    g_layoutDirty = true;
                    TriggerNudge();
                    if (openTelegram) {
                        if (chatApp == L"Discord") {
                            TelegramActions::OpenDiscord(telegramAumid);
                        } else {
                            TelegramActions::OpenTelegram(telegramAumid);
                        }
                    }
                    return 0;
                }
                const bool gameMetricsPresent =
                    !kinds.empty() && kinds[0] == IslandKind::Idle &&
                    g_settings.gameOverlay;

                bool expanded = g_pinnedExpanded.load() || g_clickExpanded.load();
                if (!gameMetricsPresent && !g_settings.expandOnHover && !expanded) {
                    g_clickExpanded = true;
                    g_layoutDirty = true;
                    return 0; // consumed click to expand
                }

                RECT clientRect;
                GetClientRect(hwnd, &clientRect);
                const float height = static_cast<float>(clientRect.bottom - clientRect.top);

                const int currentTab = NormalizedTabIndex(g_settings);

                const MediaContentPoint cp = MediaContentFromClient(xPos, yPos);

                if (mediaActive && currentTab == 0) {
                    // Source dock: "+N" opens the list, another slot switches to it.
                    {
                        const DockHit dock = ResolveSourceDockHit(cp);
                        if (dock.slot >= 0) {
                            if (dock.overflow) {
                                POINT sp = {xPos, yPos};
                                ClientToScreen(hwnd, &sp);
                                ShowMediaSourcePopup(hwnd, sp);
                            } else if (!dock.isActive) {
                                RequestMediaSource(dock.aumid);
                            }
                            g_layoutDirty = true;
                            return 0;
                        }
                    }

                    const int cmd = MediaTransportHitTest(cp);
                    if (cmd != -1) {
                        g_layoutDirty = true;
                        SendMediaTransportCommand(cmd);
                        return 0;
                    }

                    // A release over the scrubber that never went through the
                    // drag path (press started elsewhere) is swallowed rather
                    // than falling through to "open the app". The actual seek
                    // is handled by the g_scrubbing branch above.
                    if (MediaScrubFractionFromContent(cp) >= 0.0f) {
                        return 0;
                    }
                }

                if (height > 50.0f) {
                    if (mediaActive) {
                        if (currentTab == 0) {
                            OpenRelevantApp();
                        }
                    } else if (kinds.empty() || kinds[0] != IslandKind::Idle) {
                        HandleStatusClickAtPoint(hwnd, lParam);
                    }
                } else {
                    if (mediaActive) {
                        OpenRelevantApp();
                    } else {
                        HandleStatusClickAtPoint(hwnd, lParam);
                    }
                }
            }
            return 0;

        case WM_MBUTTONUP:
            ToggleEndpointMute();
            return 0;

        case WM_LBUTTONDBLCLK: {
            const bool nextVal = !g_pinnedExpanded.load();
            g_pinnedExpanded.store(nextVal);
            di::state::SetInt(L"PinnedExpanded", nextVal ? 1 : 0);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // Delta is accumulated rather than dropped inside a debounce window, so
            // every notch counts and touchpads (many small deltas) work properly.
            // The spring is interruptible, so a step never has to wait for the
            // previous one to finish.
            static int s_wheelAccum = 0;
            static ULONGLONG s_lastWheelEvent = 0;
            static ULONGLONG s_lastWheelStep = 0;

            const ULONGLONG wheelNow = GetTickCount64();
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta == 0) return 0;

            // New gesture (pause, or direction change): forget the leftover.
            if (wheelNow - s_lastWheelEvent > 220 ||
                (s_wheelAccum != 0 && ((delta > 0) != (s_wheelAccum > 0)))) {
                s_wheelAccum = 0;
            }
            s_lastWheelEvent = wheelNow;
            s_wheelAccum += delta;

            if (abs(s_wheelAccum) < WHEEL_DELTA) return 0;

            // Rate limit so a free-spinning wheel or touchpad inertia can't race
            // through every tab. At most one notch is held pending, never a backlog.
            const bool precise = abs(delta) < WHEEL_DELTA;
            const ULONGLONG cooldown = precise ? 200 : 85;
            if (wheelNow - s_lastWheelStep < cooldown) {
                s_wheelAccum = s_wheelAccum > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
                return 0;
            }
            s_lastWheelStep = wheelNow;
            StepIdleTab(s_wheelAccum > 0 ? -1 : 1);  // wheel up = previous tab
            s_wheelAccum = 0;
            return 0;
        }

        case WM_RBUTTONUP:
            // Standalone 0.5: no right-click menu. Everything it offered is in the
            // Launcher (gear), on the island (Do Not Disturb, power) or a hotkey;
            // returning here also keeps DefWindowProc from sending WM_CONTEXTMENU.
            return 0;

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE: {
            g_layoutDirty = true;
            PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
            return 0;
        }

        case WM_HOTKEY: {
            if (wParam == ID_QUIET_HOTKEY) {  // custom fork: island Do Not Disturb
                QuietMode::Toggle();
                return 0;
            }
            if (wParam == ID_HIDE_SHOW_HOTKEY) {
                if (g_isFullscreen.load(std::memory_order_relaxed)) {
                    g_fullscreenOverrideVisible = !g_fullscreenOverrideVisible.load();
                    g_autoHiddenParked = false;
                    g_layoutDirty = true;
                    if (g_fullscreenOverrideVisible.load()) {
                        g_manuallyHidden = false;
                        di::state::SetInt(L"ManuallyHidden", 0);
                        g_hotkeyUnhideUntil.store(NowSeconds() + (g_settings.autoHideIdleSeconds > 0 ? g_settings.autoHideIdleSeconds : 6.0));
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    }
                } else {
                    bool isCurrentlyHidden = g_manuallyHidden.load() || g_autoHiddenParked.load();
                    if (hwnd && !IsWindowVisible(hwnd)) {
                        isCurrentlyHidden = true;
                    }
                    if (isCurrentlyHidden) {
                        // User wants to reveal / unhide
                        g_manuallyHidden = false;
                        di::state::SetInt(L"ManuallyHidden", 0);
                        g_autoHiddenParked = false;
                        g_fullscreenOverrideVisible = true;
                        g_hotkeyUnhideUntil.store(NowSeconds() + (g_settings.autoHideIdleSeconds > 0 ? g_settings.autoHideIdleSeconds : 6.0));
                        g_layoutDirty = true;
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    } else {
                        // User wants to manually hide
                        g_manuallyHidden = true;
                        di::state::SetInt(L"ManuallyHidden", 1);
                        g_hotkeyUnhideUntil.store(0.0);
                        g_fullscreenOverrideVisible = false;
                        g_layoutDirty = true;
                        PostMessageW(hwnd, WM_APP_NEW_EVENT, 0, 0);
                    }
                }
            }
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI RenderThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClass, L"Dynamic Island for Windows", WS_POPUP, 0, 0, 520, 140,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        DI_LOG(L"Failed to create Dynamic Island overlay window.");
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    g_hwnd = hwnd;
    EnableBlurBehind(hwnd);
    ApplyBackdropMaterial(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ApplyHideShowHotkey();

    if (g_settings.autoHideIdleSeconds == 0) {
        g_manuallyHidden = false;
        di::state::SetInt(L"ManuallyHidden", 0);
    } else {
        g_manuallyHidden = di::state::GetInt(L"ManuallyHidden", 0) != 0;
    }
    if (g_manuallyHidden.load() || QuietMode::HidesIsland()) {  // custom fork: + Do Not Disturb
        ShowWindow(hwnd, SW_HIDE);
    }

    Renderer renderer;
    if (!renderer.Initialize(hwnd)) {
        DestroyWindow(hwnd);
        g_hwnd = nullptr;
        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
        return 0;
    }

    // A 1ms timer resolution costs power, so it is requested only while the island
    // is actually animating. It used to be requested once here and released at
    // shutdown, so a parked or idle island held it for the mod's whole lifetime.
    //
    // Since Windows 10 2004 this affects only the calling process's timers rather
    // than the system clock globally, but the power cost is the reason to scope it
    // either way.
    //
    // The flag is only set when timeBeginPeriod actually succeeded, so the
    // begin/end pairs stay balanced -- these calls are reference counted per
    // process, and an unmatched timeEndPeriod would decrement someone else's
    // request.
    bool highResTimer = false;

    // How long the resolution is held after animation stops. Long enough to ride
    // out the gaps between 60Hz content frames and brief pauses between spring
    // animations without flapping, short enough that a settled island gives it back
    // promptly. A plain local rather than a static, so an unload/reload cycle cannot
    // carry a stale timestamp across.
    constexpr double kHighResTimerHoldSec = 0.5;
    double lastAnimatingAt = -1.0;

    auto setHighResTimer = [&](bool want) {
        if (want == highResTimer) {
            return;
        }
        if (want) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                highResTimer = true;
            }
        } else {
            timeEndPeriod(1);
            highResTimer = false;
        }
    };

    SpringValue widthSpring;
    SpringValue heightSpring;
    SpringValue nudgeSpring;
    SpringValue volumeBarSpring;
    widthSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 120.0f) * g_settings.sizeScale);
    heightSpring.Reset((g_settings.autoHideIdleSeconds == -1 ? 0.0f : 36.0f) * g_settings.sizeScale);
    nudgeSpring.Reset(0.0f);

    IslandKind previousPrimary = IslandKind::Idle;
    auto previousFrame = std::chrono::steady_clock::now();
    auto nextFrameTarget = previousFrame;
    double nextBatteryPoll = 0.0;
    double nextProgressPoll = 0.0;
    double nextSystemPoll = 0.0;
    double nextBrightnessPoll = 0.0;
    double nextPrivacyPoll = 0.0;
    bool wasManuallyHidden = false;
    bool wasAutoHiddenParked = false;
    bool parkedForFullscreen = false;

    static double lastInteractionTime = NowSeconds();
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_APP_NEW_EVENT) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
                lastInteractionTime = NowSeconds();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        BindVolumeEndpoint();

        bool justUnhidden = false;

        // Custom fork: island Do Not Disturb parks exactly like a manual hide
        // (zero CPU, no animations) but keeps its own persisted flag.
        if (g_manuallyHidden.load() || QuietMode::HidesIsland()) {
            // Manual hide always takes precedence over — and invalidates —
            // any auto-park bookkeeping, since the window's shown/hidden
            // state is now fully owned by the manual toggle. Without this,
            // toggling manual-hide off after having been auto-parked would
            // leave g_autoHiddenParked stale-true and the window stuck
            // hidden.
            g_autoHiddenParked = false;
            wasAutoHiddenParked = false;

            if (!wasManuallyHidden) {
                // Just hid: drop to zero-CPU parking immediately, no
                // lingering render/animation work this frame.
                ShowWindow(hwnd, SW_HIDE);
                DwmBackdrop::HideAll();
                g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
                wasManuallyHidden = true;
            } else if (IsWindowVisible(hwnd)) {
                // Custom fork: something (e.g. the hide/show hotkey) re-showed the
                // window while Do Not Disturb still holds it hidden -- hide again
                // instead of leaving a frozen frame on screen.
                ShowWindow(hwnd, SW_HIDE);
                DwmBackdrop::HideAll();
            }
            previousFrame = std::chrono::steady_clock::now();
            // Fully parked: no polling, no timer wakeups — only the stop
            // event or a posted/queued message (hotkey, settings change,
            // notification, etc.) wakes this thread while hidden. Nothing
            // is being paced, so give the system timer resolution back.
            setHighResTimer(false);
            MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
            nextFrameTarget = std::chrono::steady_clock::now();
            continue;
        }

        // Sits ahead of the parked branch below on purpose: that branch can block
        // for up to 1.5s, and the mouse thread needs to hear about a transition
        // before that, not after.
        {
            static bool prevParkedForHook = false;
            const bool parkedNow = g_autoHiddenParked.load(std::memory_order_relaxed);
            if (parkedNow != prevParkedForHook) {
                prevParkedForHook = parkedNow;
                NotifyMouseThreadParkedChanged();
            }
        }

        if (g_autoHiddenParked.load()) {
            if (g_settings.autoHideIdleSeconds == 0 && !parkedForFullscreen) {
                g_autoHiddenParked = false;
            } else {
                previousFrame = std::chrono::steady_clock::now();
                // Auto-parked, so nothing is animating; same reasoning as above.
                setHighResTimer(false);
                if (parkedForFullscreen) {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, 1500, QS_ALLINPUT);
                    const bool stillFullscreen =
                        GameDetector::IsGameOrFullscreen(hwnd)  /* custom fork: + D3D signal + game list */;
                    g_isFullscreen.store(stillFullscreen, std::memory_order_relaxed);
                    if (!stillFullscreen) {
                        g_fullscreenOverrideVisible = false;
                        g_autoHiddenParked = false;
                    } else if (g_fullscreenOverrideVisible.load()) {
                        g_autoHiddenParked = false;
                    }
                } else {
                    MsgWaitForMultipleObjects(1, &g_stopEvent, FALSE, INFINITE, QS_ALLINPUT);
                }
                nextFrameTarget = std::chrono::steady_clock::now();
                continue;
            }
        }

        if (wasManuallyHidden) {
            // Just un-hid. Show now; springs get snapped straight to their
            // freshly-computed targets further down this same iteration so
            // there's no stale pop-in animation from wherever they were
            // left off before hiding.
            wasManuallyHidden = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        if (wasAutoHiddenParked && !g_autoHiddenParked.load()) {
            // Something woke us (mouse near the island, a transient alert,
            // the hotkey, fullscreen ending, etc.) — reveal and let this
            // frame's normal logic decide whether to actually stay visible
            // or immediately re-collapse and re-park.
            wasAutoHiddenParked = false;
            justUnhidden = true;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            lastInteractionTime = NowSeconds();
            g_layoutDirty = true;
            previousFrame = std::chrono::steady_clock::now();
            nextFrameTarget = previousFrame;
        }

        const double now = NowSeconds();
        // True while a tab transition is genuinely being painted. The registry /
        // device polls below run on this thread and can take several ms, which
        // shows up as a hitch mid-animation, so they wait for the spring to settle.
        const bool tabBusy = g_tabAnimating.load(std::memory_order_relaxed) &&
                             (now - g_tabLastPaintAt.load(std::memory_order_relaxed)) < 0.25;
        // The island itself is resizing (spring state from the previous frame).
        const bool springBusy =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f;
        const bool pollBusy = tabBusy || springBusy;
        if (now >= nextBatteryPoll) {
            UpdateBatterySnapshot();
            nextBatteryPoll = now + 15.0;
        }
        if (now >= nextProgressPoll) {
            UpdateProgressSnapshot();
            nextProgressPoll = now + 0.25;
        }
        if (!pollBusy && now >= nextPrivacyPoll) {
            UpdatePrivacyIndicators();
            nextPrivacyPoll = now + 2.0;  // poll every 2 s
        }

        bool timerJustCompleted = false;
        SharedState snapshot;
        {
            std::lock_guard lock(g_stateMutex);
            snapshot = g_state;
            if (g_state.timer.active && g_state.timer.running && now >= g_state.timer.endsAt) {
                g_state.timer.active = false;
                g_state.timer.running = false;
                g_state.timer.justFinished = true;
                g_state.timer.finishedExpiresAt = now + 6.0;
                timerJustCompleted = true;
            }
            if (g_state.timer.justFinished && now >= g_state.timer.finishedExpiresAt) {
                g_state.timer.justFinished = false;
            }
            snapshot.timer = g_state.timer;
            if (g_state.notification.active && now >= g_state.notification.expiresAt) {
                g_state.notification.active = false;
                snapshot.notification.active = false;
            }
            if (g_state.volume.active && now >= g_state.volume.expiresAt) {
                g_state.volume.active = false;
                snapshot.volume.active = false;
            }
            if (g_state.brightness.active && now >= g_state.brightness.expiresAt) {
                g_state.brightness.active = false;
                snapshot.brightness.active = false;
            }
            if (g_state.battery.active && now >= g_state.battery.expiresAt) {
                g_state.battery.active = false;
                snapshot.battery.active = false;
            }
            if (g_state.device.active && now >= g_state.device.expiresAt) {
                g_state.device.active = false;
                snapshot.device.active = false;
            }
        }
            if (timerJustCompleted) {
                TriggerNudge();
        }

        std::vector<IslandKind> kinds = ChooseActivities(snapshot, g_settings, now);

        // Custom fork, Feature 2: smart game mode. While a game / fullscreen app
        // is in front (and the user has not forced the island visible with the
        // hide/show hotkey), only Telegram cards may surface; everything else
        // stays hidden. Uses last iteration's fullscreen result (<= 0.5s old).
        const bool smartGameActive =
            g_settings.smartGameMode && !g_settings.gameShowOtherAlerts &&
            g_isFullscreen.load(std::memory_order_relaxed) &&
            !g_fullscreenOverrideVisible.load(std::memory_order_relaxed);
        if (smartGameActive) {
            kinds.erase(std::remove_if(kinds.begin(), kinds.end(),
                                       [&](IslandKind k) {
                                           return !(k == IslandKind::Notification &&
                                                    snapshot.notification.isTelegram &&
                                                    (snapshot.notification.chatApp != L"Discord" ||
                                                     g_settings.discordShowOverGames));
                                       }),
                        kinds.end());
            if (kinds.empty()) {
                kinds.push_back(IslandKind::Idle);
            }
        }
        const bool telegramOverGame = smartGameActive && kinds[0] == IslandKind::Notification;
        Activity primary = ActivityForKind(kinds[0], g_settings, snapshot);
        std::optional<Activity> secondary;
        if (kinds.size() >= 2) {
            secondary = ActivityForKind(kinds[1], g_settings, snapshot);
        }

        const bool pinned = g_pinnedExpanded.load();

        if (primary.kind != previousPrimary) {
            if (primary.kind != IslandKind::Idle) {
                nudgeSpring.value = -6.0f;
                nudgeSpring.velocity = 0.0f;
                nudgeSpring.target = 0.0f;
            }
            lastInteractionTime = now;
        }
        previousPrimary = primary.kind;

        POINT cursor = {};
        GetCursorPos(&cursor);

        // Pill rectangle in screen space, from the same layout Render() uses. The window
        // is a big fixed stage now, so its own rect says nothing about where the pill is.
        const bool flushTopNow = g_settings.notchStyle || g_settings.borderMergedMode;
        const StageLayout hoverStage =
            ComputeStageLayout(g_settings, widthSpring.value, heightSpring.value);
        const float hoverTop = hoverStage.pillTop +
            (flushTopNow ? std::max(0.0f, nudgeSpring.value) : nudgeSpring.value);
        RECT pillRect = {
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop)),
            hoverStage.origin.x + static_cast<int>(std::lround(hoverStage.pillLeft + widthSpring.value)),
            hoverStage.origin.y + static_cast<int>(std::lround(hoverTop + heightSpring.value))
        };

        bool hover = false;
        if (widthSpring.value > 1.0f && heightSpring.value > 1.0f) {
            hover = PtInRect(&pillRect, cursor) != FALSE;
        } else if (g_settings.unhideOnHover) {
            RECT dockRect = GetIslandDockRect();
            hover = PtInRect(&dockRect, cursor) != FALSE;
        }

        bool needsRender = false;

        if (!hover && g_clickExpanded.load()) {
            g_clickExpanded = false;
            needsRender = true;
        }
        if (!hover && g_hoveredMediaButton.load() != -1) {
            g_hoveredMediaButton = -1;
            needsRender = true;
        }
        // Fixes windhawk-mods#4738: active playback used to count as a continuous
        // event, so the island stayed visible for as long as anything was playing
        // and kept resetting the auto-hide timer. Only the 5s window after a
        // *title* change is transient now, so the pill behaves like the
        // battery alert -- it surfaces, then hides again while playback
        // continues in the background.
        const bool recentTrackChange = g_settings.mediaAutoExpand &&
                                       !MediaExpandBlocked(snapshot.media) &&
                                       primary.kind == IslandKind::Media &&
                                       snapshot.media.playing &&
                                       !snapshot.media.title.empty() &&
                                       (now - snapshot.media.titleChangedAt < 5.0);

        bool isTransientAlert = (primary.kind == IslandKind::Notification ||
                                 primary.kind == IslandKind::Volume ||
                                 primary.kind == IslandKind::Brightness ||
                                 primary.kind == IslandKind::BatteryLow ||
                                 primary.kind == IslandKind::Device ||
                                 primary.kind == IslandKind::Bluetooth ||
                                 recentTrackChange);

        const bool unhideGraceActive = (now < g_hotkeyUnhideUntil.load());
        const bool hoverUnhides = g_settings.unhideOnHover && hover;

        bool currentlyHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !pinned) {
                currentlyHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                currentlyHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
            }
        }

        bool isHoverExpanded = g_settings.expandOnHover ? hover : (hover && g_clickExpanded.load());
        // Standalone 0.9.1: the game overlay (FPS / CPU / RAM) used to replace only
        // the clock, so while music was playing it never appeared. Now it also takes
        // the collapsed player's place; hovering / clicking / pinning still opens the
        // full player, and a new track still pops up for a few seconds.
        if (g_settings.gameOverlay && primary.kind == IslandKind::Media && !secondary &&
            !isHoverExpanded && !pinned && !recentTrackChange) {
            primary = ActivityForKind(IslandKind::Idle, g_settings, snapshot);
        }
        const bool gameMetricsPresent = primary.kind == IslandKind::Idle &&
            g_settings.gameOverlay;
        if (gameMetricsPresent) {
            isHoverExpanded = false;
        }
        // Custom fork: in a game the cursor brushing the top of the screen must
        // not pop the island open.
        if (smartGameActive && !telegramOverGame) {
            isHoverExpanded = false;
        }

        if (currentlyHidden && !g_settings.unhideOnHover) {
            isHoverExpanded = false;
        } else if (isHoverExpanded || hoverUnhides || pinned || isTransientAlert || unhideGraceActive) {
            lastInteractionTime = now;
        }

        bool isHidden = false;
        if (!unhideGraceActive) {
            if (g_settings.autoHideIdleSeconds == -1 && !isTransientAlert && !isHoverExpanded && !hoverUnhides && !pinned) {
                isHidden = true;
            } else if (g_settings.autoHideIdleSeconds > 0) {
                if (hoverUnhides) {
                    isHidden = false;
                } else {
                    isHidden = (now - lastInteractionTime > g_settings.autoHideIdleSeconds);
                }
            }
        }

        static bool isFullscreen = false;
        static double lastFullscreenCheck = -1.0;  // -1.0 guarantees the very first iteration checks
        if (now - lastFullscreenCheck > 0.5) {
            const bool newFullscreen = GameDetector::IsGameOrFullscreen(hwnd)  /* custom fork: + D3D signal + game list */;
            if (isFullscreen && !newFullscreen) {
                // Fullscreen ended — re-arm so the next fullscreen session
                // hides again even if the hotkey was used to reveal the
                // island this time.
                g_fullscreenOverrideVisible = false;
            }
            isFullscreen = newFullscreen;
            g_isFullscreen.store(isFullscreen, std::memory_order_relaxed);
            lastFullscreenCheck = now;
        }

        // Reclaim the top of the z-order if another always-on-top window has
        // been raised over the island. Cheap and rate-limited, and skipped
        // while the island is hidden or suppressed so it cannot un-hide itself.
        static double lastTopmostCheck = 0.0;
        if (!isFullscreen && !g_manuallyHidden.load() && !g_autoHiddenParked.load() &&
            now - lastTopmostCheck > 1.0) {
            EnsureTopmost(hwnd, &pillRect);
            lastTopmostCheck = now;
        }
        // Custom fork, Feature 2: when a Telegram card surfaces over a game, make
        // sure the island is above it once (the check above is skipped in fullscreen).
        {
            static bool s_telegramRaised = false;
            if (telegramOverGame && !s_telegramRaised && IsWindowVisible(hwnd)) {
                EnsureTopmost(hwnd, &pillRect);
                s_telegramRaised = true;
            } else if (!telegramOverGame) {
                s_telegramRaised = false;
            }
        }

        const bool micIndicatorActive = snapshot.system.micActive && g_settings.privacyDots && g_settings.privacyDotsMic;
        const bool camIndicatorActive = snapshot.system.cameraActive && g_settings.privacyDots && g_settings.privacyDotsCam;
        // Custom fork: in smart game mode the mic / camera dots do not keep the
        // island on screen (a voice chat would otherwise show it all game long).
        const bool privacyActive = (micIndicatorActive || camIndicatorActive) && !smartGameActive;
        // Custom fork: a pinned island is not kept open over a game either.
        const bool pinnedKeepsVisible = pinned && !smartGameActive;

        // Whether any surface that actually draws CPU / RAM / disk / GPU / network
        // figures is on screen: the in-game overlay, or the idle dashboard's
        // Hardware Monitor tab while expanded (hovered or pinned) and scrolled into
        // view.
        //
        // Hoisted out of the poll block below because two decisions need it: whether
        // to sample the expensive GPU and network counters at all, and whether a
        // change in those numbers is worth repainting for.
        const int metricTabIdx = NormalizedTabIndex(g_settings);
        const bool onHardwareMonitorTab = (metricTabIdx == HardwareMonitorTabIndex(g_settings));
        const bool hwMonitorVisible = (primary.kind == IslandKind::Idle || primary.kind == IslandKind::Media) &&
            !isFullscreen && !gameMetricsPresent && (pinned || isHoverExpanded) && onHardwareMonitorTab;
        const bool gameOverlayVisible = gameMetricsPresent && !isFullscreen;
        const bool systemMetricsVisible = hwMonitorVisible || gameOverlayVisible;

        if (g_settings.brightness && !pollBusy && now >= nextBrightnessPoll) {
            UpdateBrightnessSnapshot();
            nextBrightnessPoll = now + 0.25;
        }

        if (!pollBusy && now >= nextSystemPoll) {
            const bool needGpuStats = gameOverlayVisible || hwMonitorVisible;
            const bool needNetStats = hwMonitorVisible;  // net is only ever drawn in the HW dashboard

            UpdateSystemSnapshot(needGpuStats, needNetStats);
            if (!g_endpointVolume) {
                g_volumeEndpointStale = true;  // no audio device yet, or binding failed
            }
            nextSystemPoll = now + 1.0;
        }

        if (primary.kind == IslandKind::Idle) {
            if (!isFullscreen && (pinned || isHoverExpanded)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
            } else if (primary.width > 0.0f) {
                // Collapsed: size the strip to the text it will actually render
                // (windhawk-mods#5086). ActivityForKind's 96/170px was wrong both
                // ways -- dead air around a short "9:41", and clipping on
                // "10:41:32 PM" at larger Text size.
                // The > 0 guard preserves ActivityForKind's fully-hidden case.
                primary.width =
                    renderer.MeasureIdleStrip(snapshot, g_settings, now).totalWidth *
                    g_settings.sizeScale;
            }
        }
        if (!isFullscreen && primary.kind == IslandKind::Idle &&
            g_settings.gameOverlay) {
            // Width follows the metrics actually enabled (#25) so switching some
            // off shrinks the strip instead of leaving empty space, and compact
            // mode narrows it enough to sit neatly in the taskbar area. Both the
            // size and the painting come from GameOverlayLayout, so the strip
            // cannot end up sized for a card width DrawGameOverlay no longer uses.
            const int metricCount = (g_settings.gameOverlayShowCpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowRam ? 1 : 0) +
                                    (g_settings.gameOverlayShowGpu ? 1 : 0) +
                                    (g_settings.gameOverlayShowDisk ? 1 : 0);

            const bool compact = g_settings.gameOverlayCompact;
            primary.width = GameOverlayLayout::Width(compact, g_settings.gameOverlayShowFps,
                                                     metricCount) * g_settings.sizeScale;
            primary.height = GameOverlayLayout::For(compact).height * g_settings.sizeScale;
        }
        if (primary.kind == IslandKind::Media) {
            if (!isFullscreen && (isHoverExpanded || pinned || recentTrackChange)) {
                primary.width = MediaLayout::kExpandedWidth * g_settings.sizeScale;
                primary.height = MediaLayout::kExpandedHeight * g_settings.sizeScale;
            } else {
                // Optional clock section on the left of the collapsed pill.
                const float clockSection = g_settings.mediaPillClock
                    ? renderer.MeasureMediaPill(snapshot, g_settings).sectionWidth
                    : 0.0f;
                if (g_settings.mediaPillClock) {
                    primary.width = (MediaPillLayout::kBaseWidth + clockSection) * g_settings.sizeScale;
                }
            }
        }

        const bool fullscreenSuppressed =
            isFullscreen && !g_fullscreenOverrideVisible.load(std::memory_order_relaxed);
        if ((isHidden || fullscreenSuppressed) && !privacyActive && !pinnedKeepsVisible && !isHoverExpanded && !isTransientAlert) {
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        }
        // Custom fork, Feature 3: Do Not Disturb was just turned on -- the island
        // shrinks into itself on the same springs as the fullscreen hide. The
        // content is laid out at the size it had when the shrink began and
        // squeezed onto the shrinking pill, so text shrinks with it instead of
        // spilling over the edges.
        static bool s_quietShrinking = false;
        static float s_quietShrinkW = 0.0f;
        static float s_quietShrinkH = 0.0f;
        static Activity s_quietShrinkActivity;
        const bool quietCollapsing = QuietMode::IsCollapsing(now);
        if (quietCollapsing) {
            if (!s_quietShrinking) {
                s_quietShrinking = true;
                s_quietShrinkW = widthSpring.value;
                s_quietShrinkH = heightSpring.value;
                s_quietShrinkActivity = primary;
                s_quietShrinkActivity.width = widthSpring.value;
                s_quietShrinkActivity.height = heightSpring.value;
            }
            primary.width = 0.0f;
            primary.height = 0.0f;
            secondary.reset();
        } else {
            s_quietShrinking = false;
        }

        const bool mediaWaveformVisible =
            g_settings.media && snapshot.media.playing &&
            ((primary.kind == IslandKind::Media && primary.width > 1.0f && primary.height > 1.0f) ||
             (secondary && secondary->kind == IslandKind::Media && secondary->width > 1.0f && secondary->height > 1.0f));
        g_audioCaptureNeeded.store(mediaWaveformVisible, std::memory_order_relaxed);

        float targetWidth = primary.width;
        float targetHeight = primary.height;
        if (secondary) {
            targetWidth = primary.width + secondary->width + 12.0f * g_settings.sizeScale;
            targetHeight = std::max(primary.height, secondary->height);
        }

        widthSpring.target = targetWidth;
        heightSpring.target = targetHeight;

        if (justUnhidden || (unhideGraceActive && widthSpring.value < 0.5f && targetWidth > 1.0f)) {
            widthSpring.Reset(targetWidth);
            heightSpring.Reset(targetHeight);
            nudgeSpring.Reset(0.0f);
        }

        const auto currentFrame = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(currentFrame - previousFrame).count();
        previousFrame = currentFrame;
        dt = Clamp(dt, 0.001f, 0.050f);

        float styleStiffnessMult = 1.0f;
        float styleDampingMult = 1.0f;
        if (g_settings.animationStyle == AnimationStyle::Smooth) {
            styleStiffnessMult = 1.0f;
            styleDampingMult = 1.35f; // Critically damped, no bounciness
        } else if (g_settings.animationStyle == AnimationStyle::Bouncy) {
            styleStiffnessMult = 1.1f;
            styleDampingMult = 0.70f; // Underdamped, lively elasticity
        } else if (g_settings.animationStyle == AnimationStyle::Snappy) {
            styleStiffnessMult = 1.5f;
            styleDampingMult = 1.25f; // High stiffness and quick settle
        }

        const float speed = g_settings.animationSpeed;
        float widthStiffness = 280.0f * styleStiffnessMult;
        float widthDamping = 24.0f * styleDampingMult;
        if (targetWidth > widthSpring.value) {
            widthStiffness = 380.0f * styleStiffnessMult;
            widthDamping = 26.0f * styleDampingMult;
        } else if (targetWidth < widthSpring.value) {
            widthStiffness = 200.0f * styleStiffnessMult;
            widthDamping = 28.0f * styleDampingMult;
        }

        float heightStiffness = 280.0f * styleStiffnessMult;
        float heightDamping = 24.0f * styleDampingMult;
        if (targetHeight > heightSpring.value) {
            heightStiffness = 380.0f * styleStiffnessMult;
            heightDamping = 26.0f * styleDampingMult;
        } else if (targetHeight < heightSpring.value) {
            heightStiffness = 200.0f * styleStiffnessMult;
            heightDamping = 28.0f * styleDampingMult;
        }

        widthSpring.Step(dt * speed, widthStiffness, widthDamping);
        if (widthSpring.value < 0.0f) {
            widthSpring.value = 0.0f;
            widthSpring.velocity = 0.0f;
        }

        heightSpring.Step(dt * speed, heightStiffness, heightDamping);
        if (heightSpring.value < 0.0f) {
            heightSpring.value = 0.0f;
            heightSpring.velocity = 0.0f;
        }

        nudgeSpring.Step(dt * speed, 280.0f * styleStiffnessMult, 24.0f * styleDampingMult);

        // Tracks the live level while the flyout is closed, so opening it doesn't sweep from a stale value.
        if (snapshot.volume.active) {
            volumeBarSpring.target = static_cast<float>(snapshot.volume.percent);
        } else {
            volumeBarSpring.Reset(static_cast<float>(snapshot.system.volumePercent));
        }
        // Fixed, overdamped constants: the animation style must not make the bar overshoot.
        volumeBarSpring.Step(dt * speed, 520.0f, 46.0f);
        snapshot.volume.displayPercent = volumeBarSpring.value;

        {
            std::lock_guard lock(g_stateMutex);
            g_state.system.renderFps = ClampInt(static_cast<int>(1.0f / std::max(dt, 0.001f) + 0.5f), 0, 1000);
        }

        const bool draggingOrHover = hover || ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && PtInRect(&pillRect, cursor));

        // Ctrl+hover see-through: holding Ctrl while hovering makes the island
        // transparent and click-through in ANY state, so clicks pass through
        // to windows underneath. Releasing Ctrl or moving away restores it.
        const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool ctrlHoverCT = hover && ctrlHeld;

        SetClickThrough(hwnd, (primary.kind == IslandKind::Idle && !draggingOrHover && !pinned) || ctrlHoverCT);

        // Check if animating structurally
        if (std::abs(widthSpring.velocity) > 0.01f || std::abs(widthSpring.target - widthSpring.value) > 0.01f ||
            std::abs(heightSpring.velocity) > 0.01f || std::abs(heightSpring.target - heightSpring.value) > 0.01f ||
            std::abs(nudgeSpring.velocity) > 0.01f || std::abs(nudgeSpring.target - nudgeSpring.value) > 0.01f ||
            std::abs(volumeBarSpring.velocity) > 0.01f || std::abs(volumeBarSpring.target - volumeBarSpring.value) > 0.01f) {
            needsRender = true;
        }

        // Active Monitor Tracking & dynamic DPI scaling
        {
            HMONITOR currentAnchor = nullptr;
            GetAnchorWorkRect(&currentAnchor);
            const float currentDpi = GetMonitorDpiScale(currentAnchor);
            static HMONITOR s_lastAnchorMon = nullptr;
            static float s_lastDpi = 0.0f;
            if (currentAnchor != s_lastAnchorMon || currentDpi != s_lastDpi) {
                s_lastAnchorMon = currentAnchor;
                s_lastDpi = currentDpi;
                std::lock_guard lock(g_settingsMutex);
                if (g_settings.autoDpiScale) {
                    g_settings.sizeScale = g_settings.baseSizeScale * currentDpi;
                } else {
                    g_settings.sizeScale = g_settings.baseSizeScale;
                }
                g_layoutDirty = true;
            }
        }


        // A tab transition keeps painting until its spring settles. It used to
        // request that through g_layoutDirty, which also made Render() run
        // SetWindowPos / backdrop-region upkeep on every frame of the animation.
        if (g_tabAnimating.load(std::memory_order_relaxed)) {
            if (tabBusy) {
                needsRender = true;
            } else {
                // The tab view stopped drawing mid-motion (island collapsed or an
                // alert took over); don't keep the loop spinning for it.
                g_tabAnimating.store(false, std::memory_order_relaxed);
            }
        }

        // Check if layout was explicitly invalidated
        if (g_layoutDirty.load()) {
            needsRender = true;
        }

        // Hover or pinned state changes visual elements slightly
        static bool prevHover = false;
        static bool prevPinned = false;
        if (hover != prevHover || pinned != prevPinned) {
            needsRender = true;
            prevHover = hover;
            prevPinned = pinned;
        }

        // Activities that animate continuously: the waveform bars, the marquee
        // scroll and the battery pulse. These still only want ~60Hz -- they look no
        // different above it, and they should not piggyback on whatever high Target
        // FPS the user picked for structural resize animation.
        //
        // The rate limit lives in the pacer at the bottom of the loop, not here.
        // This used to gate needsRender behind a 1/60s (16.667ms) timer and then
        // fall through to the flat 16ms idle wait, which cannot satisfy it: 16ms is
        // shorter than the gate, so the next pass failed the check and waited a
        // second time. The result was a paint roughly every 32ms -- about 31fps
        // instead of 60, which is what made playing media look choppy.
        // Media only counts as continuous while something on it is actually moving.
        //
        // ChooseActivities selects the Media pill for any *available* SMTC session,
        // playing or not, so keying off the kind alone meant a paused Spotify or a
        // browser tab with a paused video -- which can sit there for hours, since
        // browsers keep the session alive -- held the island at a 60fps render loop
        // and 1ms timer resolution indefinitely. Nothing on it moves in that state:
        // the collapsed pill draws the album art plus a row of flat bars, taking the
        // !playing branch that skips DrawWaveform entirely.
        //
        // Paused media now falls back to the ordinary change detection above (title
        // and art generation, springs, hover), so the timer is released 0.5s after
        // the last paint.
        //
        // The marquees are expanded-only, hence the hover/pinned terms. No need to
        // include recentTrackChange: it already requires snapshot.media.playing, so
        // it cannot be true while paused.
        // Checks the secondary pill too. The island can show two pills side by side,
        // so a playing media pill sitting next to a running timer or a progress ring
        // is the secondary one -- and looking only at primary left its waveform
        // frozen.
        const bool mediaShown = primary.kind == IslandKind::Media ||
                                (secondary && secondary->kind == IslandKind::Media);
        const bool mediaAnimating =
            mediaShown && (snapshot.media.playing || isHoverExpanded || pinned);

        // Transient alert pills. All expire within a few seconds, so treating them as
        // continuous cannot run away.
        const auto isTransientPill = [](IslandKind kind) {
            return kind == IslandKind::BatteryLow || kind == IslandKind::Notification;
        };
        const bool continuousAnimation =
            mediaAnimating || isTransientPill(primary.kind) ||
            (secondary && isTransientPill(secondary->kind));
        if (continuousAnimation) {
            needsRender = true;
        }

        // Privacy dots
        if (snapshot.system.micActive || snapshot.system.cameraActive) {
            needsRender = true;
        }

        // Idle dashboard clock changes once a minute
        static SYSTEMTIME prevTime = {};
        const bool clockOnScreen =
            primary.kind == IslandKind::Idle ||
            (primary.kind == IslandKind::Media && g_settings.mediaPillClock);
        if (clockOnScreen && !isHidden) {
            SYSTEMTIME local = {};
            GetLocalTime(&local);
            if (local.wMinute != prevTime.wMinute) {
                needsRender = true;
                prevTime = local;
            }
        }

        // Text that ticks once a second: the focus timer countdown and, when Show
        // seconds is on, the clock.
        //
        // Both used to ride on a side effect. The system poll refreshed CPU load
        // every second and the change detection compared it unconditionally, so the
        // whole island repainted about once a second whether anything visible had
        // changed or not. Gating those metric comparisons on visibility removed that,
        // which left DrawTimer recomputing its m:ss from NowSeconds() on a surface
        // nothing marked dirty -- a 25 minute session sat at 25:00 until some
        // unrelated event forced a paint -- and left the seconds clock updating once
        // a minute despite its own setting promising every second.
        //
        // Keyed to the displayed value rather than to elapsed time, so each visible
        // change paints exactly once. Deliberately not folded into
        // continuousAnimation: these need one frame per second, not the 1ms timer
        // resolution that continuous animation asks for.
        int shownSecond = -1;
        const bool timerShown = primary.kind == IslandKind::Timer ||
                               (secondary && secondary->kind == IslandKind::Timer);
        if (timerShown && snapshot.timer.running) {
            shownSecond = static_cast<int>(std::ceil(snapshot.timer.endsAt - now));
        } else if (clockOnScreen && g_settings.showSeconds && !isHidden) {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            shownSecond = st.wSecond;
        }
        static int s_prevShownSecond = -1;
        if (shownSecond != s_prevShownSecond) {
            s_prevShownSecond = shownSecond;
            needsRender = true;
        }

        // Compare data snapshot to detect changes
        static uint64_t prevArtGen = 0;
        static uint64_t prevSrcIconGen = 0;
        static int prevCpu = -1;
        static int prevRam = -1;
        static int prevDisk = -1;
        static int prevVol = -1;
        static int prevBright = -1;
        static bool prevMuted = false;
        static int prevBat = -1;
        static bool prevCharging = false;
        static int prevProg = -1;
        static std::wstring prevMediaTitle;
        static bool prevPlaying = false;

        // CPU / RAM / disk are compared only while a surface that draws them is on
        // screen. UpdateSystemSnapshot refreshes them every second and CPU load
        // essentially always differs between samples, so comparing them
        // unconditionally repainted the whole island once a second for numbers that
        // appear nowhere on the collapsed pill.
        const bool systemMetricsChanged =
            systemMetricsVisible && (snapshot.system.cpuPercent != prevCpu ||
                                     snapshot.system.memoryPercent != prevRam ||
                                     snapshot.system.diskFreePercent != prevDisk);

        if (snapshot.media.artGeneration != prevArtGen ||
            snapshot.media.sourceIconGeneration != prevSrcIconGen ||
            snapshot.media.title != prevMediaTitle ||
            // Play/pause has to be in here now that the metric tick no longer
            // repaints every second as a side effect. Pausing while collapsed swaps
            // the live waveform for the static bars, and without this the frozen
            // last playing frame stayed up until some unrelated repaint came along.
            snapshot.media.playing != prevPlaying ||
            systemMetricsChanged ||
            snapshot.system.volumePercent != prevVol ||
            snapshot.system.volumeMuted != prevMuted ||
            snapshot.brightness.percent != prevBright ||
            snapshot.battery.percent != prevBat ||
            snapshot.battery.charging != prevCharging ||
            snapshot.progress.percent != prevProg) {
            needsRender = true;
            prevArtGen = snapshot.media.artGeneration;
            prevSrcIconGen = snapshot.media.sourceIconGeneration;
            prevMediaTitle = snapshot.media.title;
            prevPlaying = snapshot.media.playing;
            prevCpu = snapshot.system.cpuPercent;
            prevRam = snapshot.system.memoryPercent;
            prevDisk = snapshot.system.diskFreePercent;
            prevVol = snapshot.system.volumePercent;
            prevMuted = snapshot.system.volumeMuted;
            prevBright = snapshot.brightness.percent;
            prevBat = snapshot.battery.percent;
            prevCharging = snapshot.battery.charging;
            prevProg = snapshot.progress.percent;
        }

        // Track whether Ctrl+hover click-through state changed so we re-render
        static bool prevCtrlHoverCT = false;
        if (ctrlHoverCT != prevCtrlHoverCT) {
            needsRender = true;
            prevCtrlHoverCT = ctrlHoverCT;
        }

        if (needsRender) {
            // When Ctrl+hover click-through is active, reduce pill opacity so the
            // island becomes visually see-through to match the pass-through behavior.
            Settings renderSettings = GetSettingsCopy();
            if (ctrlHoverCT) {
                renderSettings.pillOpacity = Clamp(renderSettings.pillOpacity * 0.35f, 0.15f, 0.45f);
            } else if (renderSettings.themePreset == ThemePreset::Graphite) {
                const bool isExpanded = isHoverExpanded || pinned || isTransientAlert || (widthSpring.value > 260.0f);
                renderSettings.pillOpacity = isExpanded ? 0.98f : 0.88f;
            }
            // No usable backdrop (old Windows / effects off): keep the solid background.
            // ("Clear" Liquid Glass needs no DWM blur, so it never falls back.)
            if (renderSettings.backdropMaterial != BackdropMaterial::None &&
                !(renderSettings.liquidGlass && renderSettings.liquidGlassClear) &&
                !DwmBackdrop::Available()) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            // Custom fork, Feature 2: lightweight card over games (no real blur).
            if (telegramOverGame && renderSettings.gameLightweight) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            // Custom fork: during the Do Not Disturb shrink, draw the frozen-size
            // frame scaled down (no blur window: it would not shrink with it).
            if (quietCollapsing) {
                renderSettings.backdropMaterial = BackdropMaterial::None;
            }
            renderer.SetQuietSqueeze(quietCollapsing, s_quietShrinkW, s_quietShrinkH);
            renderer.Render(snapshot, renderSettings, quietCollapsing ? s_quietShrinkActivity : primary,
                            secondary, widthSpring.value, heightSpring.value, nudgeSpring.value,
                            hover, pinned, now);
        }

        // --- Zero-CPU parking for auto-hidden states (idle timeout / fullscreen) ---
        // Only parks once the collapse animation has actually settled at 0,
        // so the shrink still animates before we cut over to OS-hidden.
        // Custom fork: once shrunk (or after a safety timeout) hand over to the
        // zero-CPU hidden state of Do Not Disturb.
        if (quietCollapsing &&
            ((widthSpring.value < 0.5f && heightSpring.value < 0.5f) ||
             now - g_quietHideAt.load() > 1.5)) {
            g_quietCollapsed = true;
            s_quietShrinking = false;
            if (g_exitRequested.load() && g_mainThreadId) {  // standalone 0.5: power button
                PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0);
            }
            continue;
        }

        const bool wantsAutoHiddenPark =
            !g_manuallyHidden.load() && !pinnedKeepsVisible && !isHoverExpanded && !isTransientAlert &&
            !privacyActive && (isHidden || fullscreenSuppressed) &&
            widthSpring.value < 0.5f && heightSpring.value < 0.5f &&
            std::fabs(widthSpring.velocity) < 0.5f && std::fabs(heightSpring.velocity) < 0.5f;

        if (wantsAutoHiddenPark) {
            parkedForFullscreen = fullscreenSuppressed;
            wasAutoHiddenParked = true;
            g_autoHiddenParked = true;
            ShowWindow(hwnd, SW_HIDE);
            DwmBackdrop::HideAll();
            g_audioCaptureNeeded.store(false, std::memory_order_relaxed);
            setHighResTimer(false);
            continue;
        }

        int targetFps = g_settings.targetFps;
        if (targetFps <= 0) {
            static int s_cachedRate = 60;
            static double s_rateCheckedAt = -1e9;
            if (now - s_rateCheckedAt > 2.0) {
                s_cachedRate = GetMonitorRefreshRate(hwnd);
                s_rateCheckedAt = now;
            }
            targetFps = s_cachedRate;
        }
        targetFps = ClampInt(targetFps, 30, 1000);
        // Custom fork, Feature 2: no more than 60 fps while drawing over a game.
        if (telegramOverGame && g_settings.gameLightweight) {
            targetFps = std::min(targetFps, 60);
        }
        double targetFrameMs = 1000.0 / static_cast<double>(targetFps);

        // Structural animation -- the springs resizing or nudging the island -- is
        // what benefits from a high Target FPS. Continuous content does not, so once
        // the springs have settled the interval is relaxed to 60Hz and the precise
        // pacer below hits it accurately.
        //
        // std::max, not a plain assignment: a user who deliberately set Target FPS
        // to 40 should keep 40 rather than being pushed up to 60.
        const bool springsAnimating =
            std::fabs(widthSpring.value - widthSpring.target) > 0.5f ||
            std::fabs(heightSpring.value - heightSpring.target) > 0.5f ||
            std::fabs(widthSpring.velocity) > 0.5f ||
            std::fabs(heightSpring.velocity) > 0.5f ||
            std::fabs(nudgeSpring.value) > 0.5f ||
            std::fabs(nudgeSpring.velocity) > 0.5f ||
            std::fabs(volumeBarSpring.value - volumeBarSpring.target) > 0.5f ||
            std::fabs(volumeBarSpring.velocity) > 0.5f ||
            g_tabAnimating.load(std::memory_order_relaxed);

        if (continuousAnimation && !springsAnimating) {
            constexpr double kContinuousFrameMs = 1000.0 / 60.0;
            targetFrameMs = std::max(targetFrameMs, kContinuousFrameMs);
        }

        // Keyed to actual animation, not to repainting.
        //
        // A single repaint does not need 1ms pacing -- it needs one frame, which the
        // pacer delivers fine at the default resolution. Only things that draw a
        // *sequence* of frames care: continuous content and spring motion.
        //
        // Keying it to needsRender was wrong twice over. Per frame it thrashed,
        // because needsRender alternates when 60Hz content runs under a higher
        // Target FPS. And with the hold added, any one-off repaint still took the
        // resolution for the full hold -- including the metric tick, which fired
        // every second, so a plain idle island requested and released it once a
        // second and held it roughly half the time with nothing moving at all.
        //
        // continuousAnimation and springsAnimating are exactly the two cases that
        // want precise frame spacing, so they drive it directly.
        if (continuousAnimation || springsAnimating) {
            lastAnimatingAt = now;
        }
        setHighResTimer(lastAnimatingAt >= 0.0 && now - lastAnimatingAt < kHighResTimerHoldSec);

        if (!needsRender) {
            // Nothing changed on screen, so poll at ~60Hz rather than the target
            // frame rate. Accuracy does not matter here -- this is a poll interval,
            // not frame pacing -- so it runs at whatever resolution is in effect.
            WaitForSingleObject(g_stopEvent, 16);
            nextFrameTarget = std::chrono::steady_clock::now();
        } else if (springsAnimating && g_settings.targetFps <= 0 && SUCCEEDED(DwmFlush())) {
            // Auto FPS while the island is resizing: block until DWM's next
            // composition so every frame lands exactly on a vsync. A timer drifts
            // against the display's refresh and produces judder even at high fps.
            nextFrameTarget = std::chrono::steady_clock::now();
        } else {
            // When animating, achieve ultra-smooth target refresh rate (e.g. 144Hz, 240Hz, 360Hz+).
            nextFrameTarget += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(targetFrameMs));

            auto nowTime = std::chrono::steady_clock::now();
            if (nowTime < nextFrameTarget) {
                double remainingMs = std::chrono::duration<double, std::milli>(nextFrameTarget - nowTime).count();
                if (remainingMs >= 1.5) {
                    // Sleep for the bulk of the remaining time using OS event wait (zero CPU usage)
                    WaitForSingleObject(g_stopEvent, static_cast<DWORD>(remainingMs - 0.5));
                }
                // Spinning only pays off for fast spring motion on high refresh rates.
                // For 60Hz content a frame landing under 1ms late is invisible, so sleep only.
                if (springsAnimating && targetFrameMs < 10.0) {
                    while (std::chrono::steady_clock::now() < nextFrameTarget &&
                           WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
                        std::this_thread::yield();
                    }
                }
            } else {
                // If we fell behind, reset target to avoid speed-up catch-up loop
                nextFrameTarget = nowTime;
            }
        }
    }

    // Balances whichever state the loop exited in; a no-op if already released.
    setHighResTimer(false);

    UnbindVolumeEndpoint();
    DwmBackdrop::DestroyAll();
    renderer.Shutdown();
    DestroyWindow(hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kWindowClass, wc.hInstance);

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}



bool StartThreads() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_settingsChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_mediaRefreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stopEvent || !g_settingsChangedEvent) {
        return false;
    }

    g_running = true;
    g_renderThread = CreateThread(nullptr, 0, RenderThreadProc, nullptr, 0, nullptr);
    if (!g_renderThread) {
        return false;
    }

    g_mediaThread = CreateThread(nullptr, 0, MediaThreadProc, nullptr, 0, nullptr);
    g_audioThread = CreateThread(nullptr, 0, AudioThreadProc, nullptr, 0, nullptr);
    g_weatherThread = CreateThread(nullptr, 0, WeatherThreadProc, nullptr, 0, nullptr);
    g_stocksWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // custom fork (custom.18)
    g_stocksThread = CreateThread(nullptr, 0, StocksThreadProc, nullptr, 0, nullptr);
    g_mouseThread = CreateThread(nullptr, 0, MouseThreadProc, nullptr, 0, &g_mouseThreadId);
#if DYNAMIC_ISLAND_HAS_USER_NOTIFICATION_LISTENER
    g_notificationThread = CreateThread(nullptr, 0, NotificationThreadProc, nullptr, 0, nullptr);
#endif
    g_bluetoothThread = CreateThread(nullptr, 0, BluetoothThreadProc, nullptr, 0, nullptr);
    SubscribeDndNotification();
    ToastWatcher::Start();  // custom fork

    return true;
}

void StopThreads() {
    UnsubscribeDndNotification();
    ToastWatcher::Stop();  // custom fork
    if (g_mouseThreadId != 0) {
        PostThreadMessageW(g_mouseThreadId, WM_QUIT, 0, 0);
    }
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }

    HANDLE handles[] = {g_renderThread, g_mediaThread, g_audioThread, g_weatherThread, g_stocksThread, g_notificationThread, g_mouseThread, g_bluetoothThread};
    for (HANDLE handle : handles) {
        if (handle) {
            WaitForSingleObject(handle, 3000);
            CloseHandle(handle);
        }
    }

    g_renderThread = nullptr;
    g_mediaThread = nullptr;
    g_audioThread = nullptr;
    g_weatherThread = nullptr;
    g_stocksThread = nullptr;
    g_notificationThread = nullptr;
    g_mouseThread = nullptr;
    g_mouseThreadId = 0;
    g_bluetoothThread = nullptr;

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_settingsChangedEvent) {
        CloseHandle(g_settingsChangedEvent);
        g_settingsChangedEvent = nullptr;
    }
    if (g_mediaRefreshEvent) {
        CloseHandle(g_mediaRefreshEvent);
        g_mediaRefreshEvent = nullptr;
    }
    if (g_stocksWakeEvent) {
        CloseHandle(g_stocksWakeEvent);
        g_stocksWakeEvent = nullptr;
    }

    g_running = false;
}



}  // namespace

// ── Lifecycle (called from main.cpp) ─────────────────────────────────────────
namespace di::island {

bool Start() {
    g_mainThreadId = GetCurrentThreadId();
    // Custom fork: island Do Not Disturb survives restarts / reboots.
    g_quietMode = di::state::GetInt(L"QuietMode", 0) != 0;
    g_quietHideAt = 0.0;
    g_quietCollapsed = g_quietMode.load();  // already on at start: hidden, no animation
    LoadSettings();

    if (!StartThreads()) {
        StopThreads();
        return false;
    }

    g_layoutDirty = true;
    DI_LOG(L"Dynamic Island %s initialized.", di::kAppVersion);
    return true;
}

void ApplySettings() {
    LoadSettings();
}

// Brings a manually hidden island back (second launch of DynamicIsland.exe, Launcher).
void Show() {
    if (g_hwnd && !g_quietMode.load() && !g_isFullscreen.load(std::memory_order_relaxed) &&
        (g_manuallyHidden.load() || !IsWindowVisible(g_hwnd))) {
        PostMessageW(g_hwnd, WM_HOTKEY, ID_HIDE_SHOW_HOTKEY, 0);
    }
}

// Focus timer, from the Launcher (it used to be in the right-click menu).
void TimerCommand(const std::wstring& action) {
    if (action == L"25") StartFocusTimer(25, false);
    else if (action == L"50") StartFocusTimer(50, false);
    else if (action == L"break") StartFocusTimer(5, true);
    else if (action == L"pause") ToggleTimerPause();
    else if (action == L"stop") StopFocusTimer();
    g_layoutDirty = true;
}

unsigned CurrentAccent() {
    return g_liveAccentRgb.load();
}

void SetProgress(int percent) {
    g_progressPercent.store(percent < 0 ? -1 : std::min(percent, 100));
    g_layoutDirty = true;
    TriggerNudge();
}

void Stop() {
    static std::atomic<bool> s_stopped{false};
    if (s_stopped.exchange(true)) {
        return;
    }
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    StopThreads();
    // Custom fork: with the island off nothing would show Telegram messages, so
    // give Telegram its Windows banners back. Re-applied on the next start.
    TelegramBanner::RestoreAll();
    DI_LOG(L"Dynamic Island stopped.");
}

}  // namespace di::island
