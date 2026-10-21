#include "menu_theme.h"
#include "win32.h"

#include <vssym32.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwctype>
#include <iterator>

namespace xtray {
namespace {

std::wstring PlainCaption(std::wstring_view caption)
{
    std::wstring text;
    for (size_t index = 0; index < caption.size(); ++index) {
        if (caption[index] != L'&') {
            text += caption[index];
        } else if (index + 1 < caption.size() && caption[index + 1] == L'&') {
            text += L'&';
            ++index;
        }
    }
    return text;
}

void Fill(HDC device, const RECT& bounds, COLORREF color)
{
    SetDCBrushColor(device, color);
    FillRect(device, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

struct SavedDc {
    HDC device;
    int saved;
    explicit SavedDc(HDC value) : device(value), saved(SaveDC(value)) {}
    ~SavedDc() { if (saved != 0) { RestoreDC(device, saved); } }
};

} // namespace

MenuTheme::~MenuTheme()
{
    Detach();
    if (font_ != nullptr) { DeleteObject(font_); }
    if (background_ != nullptr) { DeleteObject(background_); }
}

void MenuTheme::Attach(HWND window, HMENU menu)
{
    Detach();
    window_ = window;
    menu_ = menu;
    Collect(menu, true);
}

void MenuTheme::Detach() noexcept
{
    if (IsWindow(window_)) {
        for (const auto& item : items_) {
            if (IsMenu(item->menu)) {
                MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
                info.fMask = MIIM_FTYPE | MIIM_DATA;
                info.fType = item->separator ? MFT_SEPARATOR : MFT_STRING;
                SetMenuItemInfoW(item->menu, item->position, TRUE, &info);
            }
        }
        for (const auto& menu : menus_) {
            if (IsMenu(menu.handle)) {
                MENUINFO info{sizeof(MENUINFO)};
                info.fMask = MIM_BACKGROUND;
                info.hbrBack = menu.originalBackground;
                SetMenuInfo(menu.handle, &info);
            }
        }
    }
    window_ = nullptr;
    menu_ = nullptr;
    items_.clear();
    menus_.clear();
}

void MenuTheme::Collect(HMENU menu, bool bar)
{
    MENUINFO original{sizeof(MENUINFO)};
    original.fMask = MIM_BACKGROUND;
    if (!GetMenuInfo(menu, &original)) {
        throw std::runtime_error("Cannot read the menu background.");
    }
    menus_.push_back({menu, original.hbrBack});
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
        info.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, index, TRUE, &info)) {
            throw std::runtime_error("Cannot read a menu item.");
        }
        auto item = std::make_unique<Item>();
        item->menu = menu;
        item->position = static_cast<UINT>(index);
        item->bar = bar;
        item->separator = (info.fType & MFT_SEPARATOR) != 0;
        items_.push_back(std::move(item));
        if (info.hSubMenu != nullptr) { Collect(info.hSubMenu, false); }
    }
}

void MenuTheme::ReadCaptions()
{
    for (auto& item : items_) {
        MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
        info.fMask = MIIM_STRING;
        if (!GetMenuItemInfoW(item->menu, item->position, TRUE, &info)) {
            throw std::runtime_error("Cannot read the menu caption length.");
        }
        std::wstring caption(static_cast<size_t>(info.cch) + 1, L'\0');
        info.dwTypeData = caption.data();
        info.cch = static_cast<UINT>(caption.size());
        if (!GetMenuItemInfoW(item->menu, item->position, TRUE, &info)) {
            throw std::runtime_error("Cannot read the menu caption.");
        }
        caption.resize(info.cch);
        item->caption = std::move(caption);
        item->spoken = PlainCaption(item->caption);
        item->accessible = {MSAA_MENU_SIG, static_cast<DWORD>(item->spoken.size()), item->spoken.data()};
    }
}

void MenuTheme::ReadMetrics()
{
    const UINT dpi = GetDpiForWindow(window_);
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        throw std::runtime_error("Cannot read the system menu font.");
    }
    const HFONT next = CreateFontIndirectW(&metrics.lfMenuFont);
    if (next == nullptr) { throw std::runtime_error("Cannot create the system menu font."); }
    if (font_ != nullptr) { DeleteObject(font_); }
    font_ = next;
    dpi_ = dpi;
    checkColumn_ = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi);
    const int borderX = GetSystemMetricsForDpi(SM_CXBORDER, dpi);
    const int borderY = GetSystemMetricsForDpi(SM_CYBORDER, dpi);
    margins_ = {borderX, borderX, borderY, borderY};
    SIZE check{checkColumn_, GetSystemMetricsForDpi(SM_CYMENUCHECK, dpi)};
    SIZE arrow{(std::max)(1, checkColumn_ / 2), check.cy};
    SIZE separator{1, (std::max)(1, borderY)};
    MARGINS checkMargins{}, checkBackground{};
    if (const HTHEME theme = OpenThemeDataForDpi(window_, L"MENU", dpi)) {
        GetThemeMargins(theme, nullptr, MENU_POPUPITEM, MPI_NORMAL, TMT_CONTENTMARGINS, nullptr, &margins_);
        GetThemeMargins(theme, nullptr, MENU_POPUPCHECK, MC_CHECKMARKNORMAL, TMT_CONTENTMARGINS, nullptr, &checkMargins);
        GetThemeMargins(theme, nullptr, MENU_POPUPCHECKBACKGROUND, MCB_NORMAL, TMT_CONTENTMARGINS, nullptr, &checkBackground);
        GetThemePartSize(theme, nullptr, MENU_POPUPCHECK, MC_CHECKMARKNORMAL, nullptr, TS_TRUE, &check);
        GetThemePartSize(theme, nullptr, MENU_POPUPSUBMENU, MSM_NORMAL, nullptr, TS_TRUE, &arrow);
        GetThemePartSize(theme, nullptr, MENU_POPUPSEPARATOR, 0, nullptr, TS_TRUE, &separator);
        CloseThemeData(theme);
    }
    gutter_ = check.cx + checkMargins.cxLeftWidth + checkMargins.cxRightWidth +
        checkBackground.cxLeftWidth + checkBackground.cxRightWidth;
    checkHeight_ = check.cy + checkMargins.cyTopHeight + checkMargins.cyBottomHeight;
    arrow_ = arrow.cx;
    separatorHeight_ = (std::max)(1L, separator.cy + margins_.cyTopHeight + margins_.cyBottomHeight);
}

void MenuTheme::SetItems(bool dark)
{
    for (const auto& item : items_) {
        if (!IsMenu(item->menu)) { continue; }
        MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
        info.fMask = MIIM_FTYPE | MIIM_DATA;
        info.fType = (item->separator ? MFT_SEPARATOR : MFT_STRING) | (dark ? MFT_OWNERDRAW : 0);
        info.dwItemData = dark ? reinterpret_cast<ULONG_PTR>(&item->accessible) : 0;
        if (!SetMenuItemInfoW(item->menu, item->position, TRUE, &info)) {
            throw std::runtime_error("Cannot apply menu colors.");
        }
    }
}

void MenuTheme::Apply(const ThemePalette& colors)
{
    if (window_ == nullptr) { return; }
    ReadCaptions();
    ReadMetrics();
    const HBRUSH next = CreateSolidBrush(colors.window);
    if (next == nullptr) { throw std::runtime_error("Cannot create the menu background."); }
    const HBRUSH previous = background_;
    background_ = next;
    colors_ = colors;
    for (const auto& menu : menus_) {
        MENUINFO info{sizeof(MENUINFO)};
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = colors.dark ? background_ : menu.originalBackground;
        if (!SetMenuInfo(menu.handle, &info)) {
            if (previous != nullptr) { DeleteObject(previous); }
            throw std::runtime_error("Cannot apply the menu background.");
        }
    }
    SetItems(colors.dark);
    DrawMenuBar(window_);
    if (previous != nullptr) { DeleteObject(previous); }
}

MenuTheme::Item* MenuTheme::Find(ULONG_PTR data) const
{
    for (const auto& item : items_) {
        if (reinterpret_cast<ULONG_PTR>(&item->accessible) == data) { return item.get(); }
    }
    return nullptr;
}

SIZE MenuTheme::Measure(const Item& item) const
{
    if (item.separator) { return {0, separatorHeight_}; }
    const HDC device = GetDC(window_);
    if (device == nullptr) { throw std::runtime_error("Cannot measure menu text."); }
    SIZE size{};
    const auto previous = SelectObject(device, font_);
    const bool measured = GetTextExtentPoint32W(device, item.spoken.c_str(),
        static_cast<int>(item.spoken.size()), &size) != FALSE;
    SelectObject(device, previous);
    ReleaseDC(window_, device);
    if (!measured) { throw std::runtime_error("Cannot measure menu text."); }
    if (item.bar) {
        // Windows adds its own horizontal padding to owner-drawn menu-bar items.
        size.cy = GetSystemMetricsForDpi(SM_CYMENU, dpi_);
    } else {
        // Retain the system check/bitmap gutter and the trailing submenu column.
        size.cx += checkColumn_ * 2 + gutter_ + arrow_ + margins_.cxLeftWidth + margins_.cxRightWidth;
        size.cy = (std::max<LONG>)(size.cy, checkHeight_) + margins_.cyTopHeight + margins_.cyBottomHeight;
    }
    return size;
}

void MenuTheme::Draw(const Item& item, const DRAWITEMSTRUCT& drawing) const
{
    SavedDc saved(drawing.hDC);
    const bool disabled = (drawing.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool active = !disabled && (drawing.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const COLORREF background = active ? item.bar ? colors_.hot : colors_.selection : colors_.window;
    Fill(drawing.hDC, drawing.rcItem, background);
    RECT text = drawing.rcItem;
    if (item.separator) {
        text.left += checkColumn_ * 2 + margins_.cxLeftWidth;
        text.right -= margins_.cxRightWidth;
        text.top = (text.top + text.bottom) / 2;
        text.bottom = text.top + 1;
        Fill(drawing.hDC, text, colors_.border);
        return;
    }
    SelectObject(drawing.hDC, font_);
    SetBkMode(drawing.hDC, TRANSPARENT);
    SetTextColor(drawing.hDC, disabled ? colors_.disabled
        : active && !item.bar ? colors_.selectionText : colors_.text);
    if (!item.bar) {
        text.left += checkColumn_ * 2 + margins_.cxLeftWidth;
        text.right -= arrow_ + margins_.cxRightWidth;
    }
    const UINT cues = (drawing.itemState & ODS_NOACCEL) != 0 ? DT_HIDEPREFIX : 0;
    DrawTextW(drawing.hDC, item.caption.c_str(), static_cast<int>(item.caption.size()), &text,
        DT_SINGLELINE | DT_VCENTER | (item.bar ? DT_CENTER : DT_LEFT) | cues);
    if ((drawing.itemState & ODS_FOCUS) != 0 && (drawing.itemState & ODS_NOFOCUSRECT) == 0) {
        DrawFocusRect(drawing.hDC, &drawing.rcItem);
    }
}

void MenuTheme::PaintBarBackground(HDC device) const
{
    MENUBARINFO bar{sizeof(MENUBARINFO)};
    RECT window{};
    if (!GetMenuBarInfo(window_, OBJID_MENU, 0, &bar) || !GetWindowRect(window_, &window)) { return; }
    SavedDc saved(device);
    RECT bounds = bar.rcBar;
    OffsetRect(&bounds, -window.left, -window.top);
    for (const auto& item : items_) {
        if (item->bar) {
            RECT itemBounds{};
            if (GetMenuItemRect(window_, menu_, item->position, &itemBounds)) {
                OffsetRect(&itemBounds, -window.left, -window.top);
                ExcludeClipRect(device, itemBounds.left, itemBounds.top, itemBounds.right, itemBounds.bottom);
            }
        }
    }
    FillRect(device, &bounds, background_);
}

LRESULT MenuTheme::MenuCharacter(wchar_t character, HMENU menu) const
{
    std::vector<UINT> matches;
    for (const auto& item : items_) {
        if (item->menu != menu || item->separator ||
            (GetMenuState(menu, item->position, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)) != 0) {
            continue;
        }
        wchar_t accelerator = 0;
        for (size_t index = 0; index + 1 < item->caption.size(); ++index) {
            if (item->caption[index] == L'&') {
                if (item->caption[index + 1] == L'&') { ++index; }
                else { accelerator = item->caption[index + 1]; break; }
            }
        }
        if (accelerator == 0 && !item->spoken.empty()) { accelerator = item->spoken.front(); }
        if (std::towupper(accelerator) == std::towupper(character)) { matches.push_back(item->position); }
    }
    if (matches.empty()) { return MAKELRESULT(0, MNC_IGNORE); }
    if (matches.size() == 1) { return MAKELRESULT(matches.front(), MNC_EXECUTE); }
    for (size_t index = 0; index < matches.size(); ++index) {
        if ((GetMenuState(menu, matches[index], MF_BYPOSITION) & MF_HILITE) != 0) {
            return MAKELRESULT(matches[(index + 1) % matches.size()], MNC_SELECT);
        }
    }
    return MAKELRESULT(matches.front(), MNC_SELECT);
}

bool MenuTheme::Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (!colors_.dark || window_ == nullptr) { return false; }
    if (message == WM_MEASUREITEM && lParam != 0) {
        auto& measure = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure.CtlType == ODT_MENU) {
            if (const auto item = Find(measure.itemData)) {
                const auto size = Measure(*item);
                measure.itemWidth = static_cast<UINT>(size.cx);
                measure.itemHeight = static_cast<UINT>(size.cy);
                result = TRUE;
                return true;
            }
        }
    }
    if (message == WM_DRAWITEM && lParam != 0) {
        const auto& drawing = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawing.CtlType == ODT_MENU) {
            if (const auto item = Find(drawing.itemData)) {
                Draw(*item, drawing);
                result = TRUE;
                return true;
            }
        }
    }
    if (message == WM_MENUCHAR) {
        const HMENU menu = reinterpret_cast<HMENU>(lParam);
        if (std::any_of(menus_.begin(), menus_.end(), [menu](const auto& value) { return value.handle == menu; })) {
            result = MenuCharacter(LOWORD(wParam), menu);
            return true;
        }
    }
    if (message == WM_NCPAINT || message == WM_NCACTIVATE ||
        (message == WM_PRINT && (lParam & PRF_NONCLIENT) != 0)) {
        result = DefWindowProcW(window_, message, wParam, lParam);
        const HDC device = message == WM_PRINT ? reinterpret_cast<HDC>(wParam) : GetWindowDC(window_);
        if (device != nullptr) {
            PaintBarBackground(device);
            if (message != WM_PRINT) { ReleaseDC(window_, device); }
        }
        return true;
    }
    return false;
}

bool MenuTheme::SmokeTest()
{
    std::fwprintf(stderr, L"Smoke: main menu %ls colors, system metrics, accessibility, and keyboard navigation.\n",
        colors_.dark ? L"dark" : L"native light");
    for (const auto& item : items_) {
        MENUITEMINFOW info{sizeof(MENUITEMINFOW)};
        info.fMask = MIIM_FTYPE | MIIM_DATA;
        if (!GetMenuItemInfoW(item->menu, item->position, TRUE, &info) ||
            ((info.fType & MFT_OWNERDRAW) != 0) != colors_.dark ||
            info.dwItemData != (colors_.dark ? reinterpret_cast<ULONG_PTR>(&item->accessible) : 0)) {
            std::fwprintf(stderr, L"Smoke: main menu type or data failed (position=%u, bar=%d).\n",
                item->position, item->bar);
            return false;
        }
        if (!colors_.dark) { continue; }
        if (item->accessible.dwMSAASignature != MSAA_MENU_SIG ||
            std::wstring_view(item->accessible.pszWText, item->accessible.cchWText) != item->spoken ||
            (!item->separator && item->spoken.empty())) {
            std::fwprintf(stderr, L"Smoke: main menu accessible caption failed.\n");
            return false;
        }
        const auto measured = Measure(*item);
        struct Canvas {
            HDC device = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = nullptr;
            HGDIOBJ previous = nullptr;
            ~Canvas()
            {
                if (previous != nullptr) { SelectObject(device, previous); }
                if (bitmap != nullptr) { DeleteObject(bitmap); }
                if (device != nullptr) { DeleteDC(device); }
            }
        } canvas;
        const int width = (std::max)(1L, measured.cx + checkColumn_ * 2);
        const int height = (std::max)(1L, measured.cy);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width;
        bitmap.bmiHeader.biHeight = -height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        canvas.bitmap = CreateDIBSection(canvas.device, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (canvas.device == nullptr || canvas.bitmap == nullptr || pixels == nullptr) { return false; }
        canvas.previous = SelectObject(canvas.device, canvas.bitmap);
        if (canvas.previous == nullptr || canvas.previous == HGDI_ERROR) { return false; }
        for (const UINT state : {UINT{0}, UINT{ODS_SELECTED}, UINT{ODS_HOTLIGHT},
            UINT{ODS_DISABLED}, UINT{ODS_NOACCEL}}) {
            DRAWITEMSTRUCT draw{};
            draw.CtlType = ODT_MENU;
            draw.itemState = state;
            draw.itemData = reinterpret_cast<ULONG_PTR>(&item->accessible);
            draw.hDC = canvas.device;
            draw.rcItem = {0, 0, width, height};
            LRESULT result = 0;
            if (!Handle(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw), result) || result != TRUE) { return false; }
            GdiFlush();
            const bool active = (state & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
            const COLORREF background = active ? item->bar ? colors_.hot : colors_.selection : colors_.window;
            if (GetPixel(canvas.device, 0, 0) != background) {
                std::fwprintf(stderr, L"Smoke: main menu background failed (state=%u).\n", state);
                return false;
            }
            if (!item->separator) {
                const auto color = (state & ODS_DISABLED) != 0 ? colors_.disabled
                    : active && !item->bar ? colors_.selectionText : colors_.text;
                const DWORD expected = (static_cast<DWORD>(GetRValue(color)) << 16) |
                    (static_cast<DWORD>(GetGValue(color)) << 8) | GetBValue(color);
                const auto* begin = static_cast<const DWORD*>(pixels);
                const size_t count = static_cast<size_t>(width) * height;
                if (std::none_of(begin, begin + count, [expected](DWORD pixel) { return (pixel & 0xFFFFFF) == expected; })) {
                    std::fwprintf(stderr, L"Smoke: main menu text contrast failed (state=%u, caption=%ls).\n",
                        state, item->caption.c_str());
                    return false;
                }
            }
        }
    }
    for (const auto& menu : menus_) {
        MENUINFO info{sizeof(MENUINFO)};
        info.fMask = MIM_BACKGROUND;
        if (!GetMenuInfo(menu.handle, &info) ||
            info.hbrBack != (colors_.dark ? background_ : menu.originalBackground)) {
            std::fwprintf(stderr, L"Smoke: main menu background brush failed.\n");
            return false;
        }
    }
    if (colors_.dark) {
        for (const auto& item : items_) {
            if (item->separator || item->spoken.empty()) { continue; }
            wchar_t key = item->spoken.front();
            for (size_t index = 0; index + 1 < item->caption.size(); ++index) {
                if (item->caption[index] == L'&' && item->caption[index + 1] != L'&') {
                    key = item->caption[index + 1];
                    break;
                }
            }
            const auto result = MenuCharacter(key, item->menu);
            if ((HIWORD(result) != MNC_EXECUTE && HIWORD(result) != MNC_SELECT) ||
                LOWORD(result) >= GetMenuItemCount(item->menu)) {
                std::fwprintf(stderr, L"Smoke: main menu mnemonic failed.\n");
                return false;
            }
        }
    }
    return true;
}

} // namespace xtray
