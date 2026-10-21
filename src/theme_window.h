#pragma once

#include "theme.h"

#include <commctrl.h>
#include <oleacc.h>

namespace xtray {

void SetButtonIcons(HWND control, HICON normal, HICON disabled, int size);
bool SmokeTestNativeControl(HWND control);
bool SmokeTestTextWordWrap(HWND reference);

class ThemeWindow {
public:
    ThemeWindow() = default;
    ~ThemeWindow();
    ThemeWindow(const ThemeWindow&) = delete;
    ThemeWindow& operator=(const ThemeWindow&) = delete;
    void Attach(HWND window);
    void Detach();
    void Apply(bool dark, bool force = false);
    void ApplyOwned(bool dark);
    void SetFont(HFONT font) { font_ = font; }
    const ThemePalette& Colors() const { return colors_; }
    bool Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    static ThemeWindow* From(HWND window);
private:
    static LRESULT CALLBACK ColorProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    bool AttachControl(HWND control);
    void DrawItem(const DRAWITEMSTRUCT& item);
    void MeasureItem(MEASUREITEMSTRUCT& item);
    HWND window_ = nullptr;
    ThemePalette colors_ = PaletteFor(false);
    HBRUSH background_ = nullptr, surface_ = nullptr, logBrush_ = nullptr;
    HFONT font_ = nullptr;
};

} // namespace xtray
