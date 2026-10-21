#pragma once

#include "theme.h"

#include <commctrl.h>
#include <oleacc.h>
#include <uxtheme.h>

#include <memory>
#include <string>
#include <vector>

namespace xtray {

// Windows retains menu placement, navigation and command dispatch.
class MenuTheme {
public:
    ~MenuTheme();
    void Attach(HWND window, HMENU menu);
    void Detach() noexcept;
    void Apply(const ThemePalette& colors);
    bool Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    bool SmokeTest();
private:
    struct Item {
        MSAAMENUINFO accessible{};
        HMENU menu = nullptr;
        UINT position = 0;
        bool bar = false, separator = false;
        std::wstring caption, spoken;
    };
    struct Menu {
        HMENU handle;
        HBRUSH originalBackground;
    };
    void Collect(HMENU menu, bool bar);
    void ReadCaptions();
    void ReadMetrics();
    void SetItems(bool dark);
    Item* Find(ULONG_PTR data) const;
    SIZE Measure(const Item& item) const;
    void Draw(const Item& item, const DRAWITEMSTRUCT& drawing) const;
    void PaintBarBackground(HDC device) const;
    LRESULT MenuCharacter(wchar_t character, HMENU menu) const;
    HWND window_ = nullptr;
    HMENU menu_ = nullptr;
    ThemePalette colors_ = PaletteFor(false);
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    UINT dpi_ = 0;
    int checkColumn_ = 0, gutter_ = 0, arrow_ = 0, separatorHeight_ = 0;
    int checkHeight_ = 0;
    MARGINS margins_{};
    std::vector<std::unique_ptr<Item>> items_;
    std::vector<Menu> menus_;
};

} // namespace xtray
