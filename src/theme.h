#pragma once

#include <windows.h>

#include <memory>

namespace xtray {

enum class ThemeMode { System, Light, Dark };
inline constexpr UINT kSystemThemeMessage = WM_APP + 44;

struct ThemePalette {
    bool dark;
    COLORREF window, surface, text, disabled, border, button, hot, pressed;
    COLORREF selection, selectionText, logBackground, trayLog, xrayLog;
    bool operator==(const ThemePalette&) const = default;
};

bool ResolveDark(ThemeMode preference, bool systemDark);
bool IsLightColor(BYTE red, BYTE green, BYTE blue);
ThemePalette PaletteFor(bool dark);

class ThemeMonitor {
public:
    explicit ThemeMonitor(HWND window);
    ~ThemeMonitor();
    ThemeMonitor(const ThemeMonitor&) = delete;
    ThemeMonitor& operator=(const ThemeMonitor&) = delete;
    bool SystemDark() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace xtray
