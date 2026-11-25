#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>

namespace {
constexpr wchar_t App[] = L"BetterDimmer";
constexpr wchar_t SettingsKey[] = L"Software\\BetterDimmer";
constexpr UINT Tray = WM_APP + 1;
constexpr UINT_PTR Refresh = 1;
constexpr int First = 100, Exit = 200;
constexpr UINT Keys[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_HOME};
constexpr const wchar_t* Labels[] = {
    L"Increase brightness\tCtrl+Alt+Win+Up", L"Decrease brightness\tCtrl+Alt+Win+Down",
    L"Decrease gamma\tCtrl+Alt+Win+Left", L"Increase gamma\tCtrl+Alt+Win+Right",
    L"Reset\tCtrl+Alt+Win+Home"
};

struct Settings {
    int brightness = 100, gamma = 100;
    bool neutral() const { return brightness == 100 && gamma == 100; }
};
Settings load() {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, L"Levels", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS) {
        Settings s{int(value & 0xffff), int(value >> 16)};
        if (s.brightness >= 50 && s.brightness <= 100 && s.gamma >= 70 && s.gamma <= 150)
            return s;
    }
    return {};
}
bool save(Settings s) {
    DWORD value = DWORD(s.brightness) | (DWORD(s.gamma) << 16);
    return RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, L"Levels", REG_DWORD,
                          &value, sizeof value) == ERROR_SUCCESS;
}

using Ramp = std::array<WORD, 3 * 256>;
Ramp adjusted(const Ramp& base, Settings s) {
    if (s.neutral()) return base;
    Ramp out{};
    for (int i = 0; i < 256; ++i) {
        double input = 255 * std::pow(i / 255.0, 100.0 / s.gamma);
        int lo = int(input), hi = std::min(lo + 1, 255);
        for (int channel = 0; channel < 3; ++channel) {
            int offset = channel * 256;
            double sample = base[offset + lo] + (input - lo) *
                            (int(base[offset + hi]) - base[offset + lo]);
            out[offset + i] = WORD(std::lround(sample * s.brightness / 100.0));
        }
    }
    return out;
}
bool matchesRamp(const Ramp& a, const Ramp& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - b[i]) > 512) return false;
    return true;
}
std::wstring primary() {
    DISPLAY_DEVICEW dev{};
    for (DWORD i = 0;; ++i) {
        dev.cb = sizeof dev;
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0)) return {};
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return dev.DeviceName;
    }
}

bool attached(const std::wstring& name) {
    DISPLAY_DEVICEW dev{};
    for (DWORD i = 0;; ++i) {
        dev.cb = sizeof dev;
        if (!EnumDisplayDevicesW(nullptr, i, &dev, 0)) return false;
        if (name == dev.DeviceName) return (dev.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    }
}

class Display {
    HDC dc_ = nullptr;
    std::wstring device_;
    Ramp baseline_{}, last_{};
    bool owned_ = false;

public:
    ~Display() { restore(); if (dc_) DeleteDC(dc_); }
    Display() = default;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    bool restore() {
        if (!owned_) return true;
        if (!attached(device_)) { owned_ = false; return true; }
        Ramp current{}, result{};
        if (!GetDeviceGammaRamp(dc_, current.data())) return false;
        if (matchesRamp(current, last_) && !matchesRamp(current, baseline_) &&
            (!SetDeviceGammaRamp(dc_, baseline_.data()) ||
             !GetDeviceGammaRamp(dc_, result.data()) || !matchesRamp(result, baseline_)))
            return false;
        owned_ = false;
        return true;
    }

    bool prepare(bool reopen = false) {
        std::wstring name = primary();
        if (name.empty()) return false;
        if (name != device_ && !restore()) return false;
        if (!dc_ || reopen || name != device_) {
            HDC next = CreateDCW(L"DISPLAY", name.c_str(), nullptr, nullptr);
            if (!next) return false;
            if (dc_) DeleteDC(dc_);
            dc_ = next;
            device_ = name;
        }
        return true;
    }

    bool apply(Settings s, bool reopen = false, bool requireChange = false) {
        if (!prepare(reopen)) return false;
        Ramp before{}, after{};
        if (!GetDeviceGammaRamp(dc_, before.data())) return false;
        // If Windows or a calibration tool replaced our ramp, use that as the new baseline.
        if (!owned_ || !matchesRamp(before, last_)) {
            baseline_ = before;
            owned_ = false;
        }
        if (s.neutral()) return restore();
        Ramp target = adjusted(baseline_, s);
        if (matchesRamp(before, target)) return !requireChange;
        if (!SetDeviceGammaRamp(dc_, target.data())) return false;
        bool read = GetDeviceGammaRamp(dc_, after.data()) != FALSE;
        if (!read || !matchesRamp(after, target)) {
            // The driver may return success without accepting the ramp.
            if (!read || !matchesRamp(after, before)) {
                SetDeviceGammaRamp(dc_, before.data());
                Ramp rollback{};
                if (!GetDeviceGammaRamp(dc_, rollback.data()) || !matchesRamp(rollback, before)) {
                    last_ = read ? after : target;
                    owned_ = true; // Leave cleanup a chance to restore the baseline.
                }
            }
            return false;
        }
        last_ = after;
        owned_ = true;
        return true;
    }
};

Display display;
Settings levels;
HICON icon;
UINT taskbarCreated;
bool added;
bool displayError, settingsError;
std::wstring conflicts;
constexpr wchar_t RampError[] =
    L"Windows could not verify the requested gamma ramp. Try SDR or a milder setting.";

NOTIFYICONDATAW trayData(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof data;
    data.hWnd = window;
    data.uID = 1;
    return data;
}
bool trayIcon(HWND window, bool add) {
    auto data = trayData(window);
    data.uFlags = NIF_TIP;
    std::swprintf(data.szTip, ARRAYSIZE(data.szTip), L"BetterDimmer: light %d%%, gamma %.2f%ls",
                  levels.brightness, levels.gamma / 100.0,
                  displayError || settingsError ? L" (!)" : L"");
    if (add) {
        data.uFlags |= NIF_ICON | NIF_MESSAGE;
        data.hIcon = icon;
        data.uCallbackMessage = Tray;
    }
    return Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data) != FALSE;
}
void balloon(HWND window, const wchar_t* message) {
    auto data = trayData(window);
    data.uFlags = NIF_INFO;
    lstrcpynW(data.szInfoTitle, App, ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, message, ARRAYSIZE(data.szInfo));
    data.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
void action(HWND window, int id) {
    Settings next = levels;
    switch (id - First) {
    case 0: next.brightness = std::min(100, next.brightness + 5); break;
    case 1: next.brightness = std::max(50, next.brightness - 5); break;
    case 2: next.gamma = std::max(70, next.gamma - 5); break;
    case 3: next.gamma = std::min(150, next.gamma + 5); break;
    case 4: next = {}; break;
    default: return;
    }
    displayError = !display.apply(next);
    if (displayError) balloon(window, RampError);
    else {
        levels = next;
        settingsError = !save(levels);
        if (settingsError) balloon(window, L"Could not save settings for the next launch.");
    }
    trayIcon(window, false);
}
constexpr COLORREF MenuBackground = RGB(31, 31, 31);

struct MenuRow {
    const wchar_t* label;
    HFONT font;
    int padding;
};

void measureRow(HWND window, MEASUREITEMSTRUCT& item, const MenuRow& row) {
    const wchar_t* tab = std::wcschr(row.label, L'\t');
    SIZE left{}, right{};
    if (HDC dc = GetDC(window)) {
        auto old = SelectObject(dc, row.font);
        GetTextExtentPoint32W(dc, row.label, int(tab ? tab - row.label : std::wcslen(row.label)), &left);
        if (tab) GetTextExtentPoint32W(dc, tab + 1, lstrlenW(tab + 1), &right);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
    }
    item.itemWidth = left.cx + right.cx + row.padding * (tab ? 4 : 2);
    item.itemHeight = *row.label ? std::max(left.cy, right.cy) + row.padding : row.padding / 2;
}

void drawRow(const DRAWITEMSTRUCT& item, const MenuRow& row) {
    const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = !disabled && (item.itemState & ODS_SELECTED);
    const int saved = SaveDC(item.hDC);
    if (!saved) return;
    auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(item.hDC, selected ? RGB(59, 59, 59) : MenuBackground);
    FillRect(item.hDC, &item.rcItem, brush);
    RECT text = item.rcItem;
    text.left += row.padding;
    text.right -= row.padding;
    if (!*row.label) {
        text.top += (text.bottom - text.top) / 2;
        text.bottom = text.top + 1;
        SetDCBrushColor(item.hDC, RGB(77, 77, 77));
        FillRect(item.hDC, &text, brush);
    } else {
        const wchar_t* tab = std::wcschr(row.label, L'\t');
        constexpr UINT style = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
        SelectObject(item.hDC, row.font);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, disabled ? RGB(163, 163, 163) : RGB(245, 245, 245));
        DrawTextW(item.hDC, row.label, tab ? int(tab - row.label) : -1, &text, style);
        if (tab) {
            SetTextColor(item.hDC, RGB(181, 181, 181));
            DrawTextW(item.hDC, tab + 1, -1, &text, style | DT_RIGHT);
        }
    }
    RestoreDC(item.hDC, saved);
}

void menu(HWND window) {
    HMENU popup = CreatePopupMenu();
    POINT cursor{};
    if (!popup) return;
    if (!GetCursorPos(&cursor)) { DestroyMenu(popup); return; }

    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof contrast;
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
                              && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    HBRUSH background = highContrast ? nullptr : CreateSolidBrush(MenuBackground);
    MENUINFO info{};
    info.cbSize = sizeof info;
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = background;
    info.dwStyle = MNS_NOCHECK;
    const bool dark = background && SetMenuInfo(popup, &info);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    HFONT custom = nullptr;
    if (dark && SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0))
        custom = CreateFontIndirectW(&metrics.lfMenuFont);
    HFONT font = custom ? custom : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const int padding = std::max(8, int(GetSystemMetrics(SM_CYMENU)) / 2);

    std::array<MenuRow, ARRAYSIZE(Keys) + 5> rows{};
    size_t count = 0;
    auto append = [&](UINT id, const wchar_t* label) {
        if (dark) {
            auto& row = rows[count++];
            row = {label, font, padding};
            MENUITEMINFOW item{};
            item.cbSize = sizeof item;
            item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA | MIIM_STRING;
            item.fType = MFT_OWNERDRAW;
            item.fState = id ? MFS_ENABLED : MFS_DISABLED;
            item.wID = id;
            item.dwItemData = reinterpret_cast<ULONG_PTR>(&row);
            item.dwTypeData = const_cast<wchar_t*>(label);
            if (InsertMenuItemW(popup, UINT(GetMenuItemCount(popup)), TRUE, &item)) return;
        }
        const UINT flags = !*label ? MF_SEPARATOR : id ? MF_STRING : MF_STRING | MF_GRAYED;
        AppendMenuW(popup, flags, id, label);
    };

    const std::wstring conflictsLabel = conflicts;
    for (UINT i = 0; i < ARRAYSIZE(Keys); ++i) append(First + i, Labels[i]);
    if (displayError) append(0, L"Gamma ramp not verified");
    if (settingsError) append(0, L"Settings not saved");
    if (!conflictsLabel.empty()) append(0, conflictsLabel.c_str());
    append(0, L"");
    append(Exit, L"Exit and restore display");
    SetForegroundWindow(window);
    UINT id = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             cursor.x, cursor.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(popup);
    if (custom) DeleteObject(custom);
    if (background) DeleteObject(background);
    if (id == Exit) SendMessageW(window, WM_CLOSE, 0, 0);
    else if (id) action(window, int(id));
}
LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (taskbarCreated && msg == taskbarCreated) {
        added = trayIcon(window, true);
        return 0;
    }
    switch (msg) {
    case WM_MEASUREITEM: {
        if (!l) break;
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (item.CtlType != ODT_MENU || !item.itemData) break;
        measureRow(window, item, *reinterpret_cast<const MenuRow*>(item.itemData));
        return TRUE;
    }
    case WM_DRAWITEM: {
        if (!l) break;
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(l);
        if (item.CtlType != ODT_MENU || !item.itemData) break;
        drawRow(item, *reinterpret_cast<const MenuRow*>(item.itemData));
        return TRUE;
    }
    case Tray:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) menu(window);
        return 0;
    case WM_HOTKEY: action(window, int(w)); return 0;
    case WM_DISPLAYCHANGE: SetTimer(window, Refresh, 500, nullptr); return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)
            SetTimer(window, Refresh, 500, nullptr);
        return TRUE;
    case WM_TIMER:
        if (w == Refresh) {
            KillTimer(window, Refresh);
            displayError = !display.apply(levels, true);
            trayIcon(window, false);
        }
        return 0;
    case WM_ENDSESSION:
        if (w) display.restore();
        return 0;
    case WM_CLOSE:
        if (!display.restore()) {
            MessageBoxW(window, L"Could not restore the original gamma ramp. "
                                L"BetterDimmer is still running so you can retry.",
                        App, MB_OK | MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        for (UINT i = 0; i < ARRAYSIZE(Keys); ++i) UnregisterHotKey(window, First + i);
        auto data = trayData(window);
        if (added) Shell_NotifyIconW(NIM_DELETE, &data);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window, msg, w, l);
}

bool selfTest() {
    Ramp base{};
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            base[c * 256 + i] = WORD(i * (257 - c));
    const auto dim = adjusted(base, {50, 100});
    const auto dark = adjusted(base, {100, 70});
    const auto light = adjusted(base, {100, 150});
    if (adjusted(base, {}) != base || dim[255] != 32768 || dim[511] != 32640 ||
        !(dark[128] < base[128] && light[128] > base[128]) ||
        dark[255] != base[255] || light[255] != base[255] || matchesRamp(dim, base))
        return false;
    for (const auto* ramp : {&dim, &dark, &light})
        for (int c = 0; c < 3; ++c)
            if (!std::is_sorted(ramp->begin() + c * 256, ramp->begin() + (c + 1) * 256))
                return false;
    return true;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR args, int) {
    if (std::wcscmp(args, L"--self-test") == 0) return selfTest() ? 0 : 1;
    bool probe = std::wcscmp(args, L"--probe") == 0;
    bool check = std::wcscmp(args, L"--check") == 0;
    if (*args && !probe && !check) return 64;
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\BetterDimmer.Singleton");
    if (!singleton) return 1;
    const bool running = GetLastError() == ERROR_ALREADY_EXISTS;
    if (running) { CloseHandle(singleton); return 2; }
    if (check || probe) {
        bool applied = display.apply(probe ? Settings{90, 100} : Settings{}, false, probe);
        bool restored = display.restore();
        if (restored) CloseHandle(singleton);
        return !restored ? 4 : !applied ? 3 : 0;
    }

    icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpszClassName = App;
    cls.lpfnWndProc = procedure;
    cls.hIcon = icon;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, App, App, WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    levels = load();
    added = trayIcon(window, true);
    if (!added) {
        MessageBoxW(nullptr, L"Could not create the tray icon.", App, MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 1;
    }
    constexpr const wchar_t* shortNames[] = {L"Up", L"Down", L"Left", L"Right", L"Home"};
    for (UINT i = 0; i < ARRAYSIZE(Keys); ++i) {
        if (!RegisterHotKey(window, First + i, MOD_CONTROL | MOD_ALT | MOD_WIN, Keys[i])) {
            if (conflicts.empty()) conflicts = L"Unavailable shortcuts: ";
            else conflicts += L", ";
            conflicts += shortNames[i];
        }
    }
    displayError = !display.apply(levels);
    trayIcon(window, false);
    if (displayError) balloon(window, RampError);
    else if (!conflicts.empty()) balloon(window, conflicts.c_str());
    MSG msg{};
    BOOL more;
    while ((more = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    display.restore();
    CloseHandle(singleton);
    return more == -1 ? 1 : int(msg.wParam);
}
