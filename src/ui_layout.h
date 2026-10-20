#pragma once

#include "win32.h"

#include <algorithm>

namespace xtray {

inline int UiScale(HWND window, int value)
{
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

inline SIZE ControlTextSize(HWND control, int wrapWidth = 0)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    HDC device = GetDC(control);
    if (device == nullptr) {
        throw std::runtime_error(Text("Cannot measure control text.", "无法测量控件文字。"));
    }
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const auto previous = SelectObject(device, font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    SIZE size{};
    bool measured = true;
    if (wrapWidth > 0) {
        RECT bounds{0, 0, wrapWidth, 0};
        measured = copied == 0 || DrawTextW(device, text.data(), copied, &bounds,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX) != 0;
        size = {bounds.right - bounds.left, bounds.bottom - bounds.top};
    } else {
        measured = GetTextExtentPoint32W(device, text.data(), copied, &size) != FALSE;
    }
    SelectObject(device, previous);
    ReleaseDC(control, device);
    if (!measured) {
        throw std::runtime_error(Text("Cannot measure control text.", "无法测量控件文字。"));
    }
    return size;
}

inline int TextButtonWidth(HWND control)
{
    return ControlTextSize(control).cx + UiScale(control, 24);
}

} // namespace xtray
