#include "theme_window.h"
#include "win32.h"

#include <dwmapi.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <algorithm>
#include <cwchar>
#include <cstdio>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string>

namespace xtray {
namespace {

constexpr wchar_t kThemeProperty[] = L"XTray.ThemeWindow";
constexpr UINT_PTR kColorSubclassId = 1;
constexpr UINT_PTR kIconListSubclassId = 2;

struct SavedDc {
    HDC device;
    int saved;
    explicit SavedDc(HDC value) : device(value), saved(SaveDC(value)) {}
    ~SavedDc() { if (saved != 0) { RestoreDC(device, saved); } }
};

bool IsClass(HWND window, const wchar_t* expected)
{
    wchar_t name[64]{};
    return GetClassNameW(window, name, static_cast<int>(std::size(name))) != 0 &&
        _wcsicmp(name, expected) == 0;
}

bool IsRichEdit(HWND window) { return IsClass(window, L"RICHEDIT50W"); }

void Fill(HDC device, const RECT& bounds, COLORREF color)
{
    SetDCBrushColor(device, color);
    FillRect(device, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

HFONT WindowFont(HWND window, HFONT fallback)
{
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    return font != nullptr ? font : fallback != nullptr ? fallback
        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

int Scale(HWND window, int value)
{
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

bool DrawCheckboxCaption(HWND control, const NMCUSTOMDRAW& item, const ThemePalette& colors)
{
    const auto theme = GetWindowTheme(control);
    const int length = GetWindowTextLengthW(control);
    if (theme == nullptr || length <= 0) {
        return false;
    }
    SavedDc saved(item.hdc);
    SelectObject(item.hdc, WindowFont(control, nullptr));
    SIZE glyph{}, spacing{};
    if (FAILED(GetThemePartSize(theme, item.hdc, BP_CHECKBOX, CBS_UNCHECKEDNORMAL,
        nullptr, TS_DRAW, &glyph)) ||
        !GetTextExtentPoint32W(item.hdc, L"0", 1, &spacing)) {
        return false;
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    if (copied <= 0) {
        return false;
    }
    RECT bounds = item.rc;
    bounds.left += glyph.cx + spacing.cx / 2;
    const auto cues = SendMessageW(control, WM_QUERYUISTATE, 0, 0);
    const UINT flags = DT_SINGLELINE | ((cues & UISF_HIDEACCEL) != 0 ? DT_HIDEPREFIX : 0);
    SetBkMode(item.hdc, TRANSPARENT);
    SetTextColor(item.hdc, IsWindowEnabled(control) ? colors.text : colors.disabled);
    DrawTextW(item.hdc, text.data(), copied, &bounds, flags | DT_VCENTER | DT_END_ELLIPSIS);
    if ((item.uItemState & CDIS_FOCUS) != 0 && (cues & UISF_HIDEFOCUS) == 0) {
        RECT focus = bounds;
        DrawTextW(item.hdc, text.data(), copied, &focus, flags | DT_CALCRECT);
        const LONG height = focus.bottom - focus.top;
        focus.top = bounds.top + (bounds.bottom - bounds.top - height) / 2;
        focus.bottom = focus.top + height;
        focus.right = (std::min)(focus.right, bounds.right);
        InflateRect(&focus, 1, 1);
        IntersectRect(&focus, &focus, &item.rc);
        DrawFocusRect(item.hdc, &focus);
    }
    return true;
}

const wchar_t* MenuText(ULONG_PTR data)
{
    const auto* item = reinterpret_cast<const MSAAMENUINFO*>(data);
    return item != nullptr && item->dwMSAASignature == MSAA_MENU_SIG && item->pszWText != nullptr
        ? item->pszWText : L"";
}

LRESULT CALLBACK IconListProcedure(HWND control, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR parameter)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, IconListProcedure, kIconListSubclassId);
        const auto result = DefSubclassProc(control, message, wParam, lParam);
        ImageList_Destroy(reinterpret_cast<HIMAGELIST>(parameter));
        return result;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

} // namespace

void SetButtonIcons(HWND control, HICON normal, HICON disabled, int size)
{
    struct Images {
        HIMAGELIST value;
        ~Images() { if (value != nullptr) { ImageList_Destroy(value); } }
    } images{ImageList_Create(size, size, ILC_COLOR32, 6, 0)};
    if (images.value == nullptr) {
        throw std::runtime_error(Text("Cannot create native button images.", "无法创建系统按钮图像。"));
    }
    for (const auto icon : {normal, normal, normal, disabled, normal, normal}) {
        if (ImageList_AddIcon(images.value, icon) == -1) {
            throw std::runtime_error(Text("Cannot add a native button image.", "无法添加系统按钮图像。"));
        }
    }
    DWORD_PTR previous = 0;
    const bool attached = GetWindowSubclass(control, IconListProcedure, kIconListSubclassId, &previous) != FALSE;
    if (!SetWindowSubclass(control, IconListProcedure, kIconListSubclassId,
        reinterpret_cast<DWORD_PTR>(images.value))) {
        throw std::runtime_error(Text("Cannot attach native button images.", "无法关联系统按钮图像。"));
    }
    BUTTON_IMAGELIST list{images.value, {}, BUTTON_IMAGELIST_ALIGN_CENTER};
    if (!SendMessageW(control, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&list))) {
        if (attached) {
            SetWindowSubclass(control, IconListProcedure, kIconListSubclassId, previous);
        } else {
            RemoveWindowSubclass(control, IconListProcedure, kIconListSubclassId);
        }
        throw std::runtime_error(Text("Cannot set native button images.", "无法设置系统按钮图像。"));
    }
    images.value = nullptr;
    if (attached) {
        ImageList_Destroy(reinterpret_cast<HIMAGELIST>(previous));
    }
    InvalidateRect(control, nullptr, FALSE);
}

bool SmokeTestNativeControl(HWND control)
{
    const auto* theme = ThemeWindow::From(GetParent(control));
    const bool combo = IsClass(control, L"ComboBox");
    const bool button = IsClass(control, L"Button");
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
    const bool checkbox = button && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
    RECT bounds{};
    if (theme == nullptr || (!combo && !button) ||
        (button && (style & BS_TYPEMASK) == BS_OWNERDRAW) ||
        (combo && (style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0) ||
        !GetClientRect(control, &bounds) || bounds.right <= 0 || bounds.bottom <= 0) {
        std::fwprintf(stderr, L"Smoke: invalid native control (id=%d).\n", GetDlgCtrlID(control));
        return false;
    }
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
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.right;
    info.bmiHeader.biHeight = -bounds.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    canvas.bitmap = CreateDIBSection(canvas.device, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (canvas.device == nullptr || canvas.bitmap == nullptr || pixels == nullptr) {
        std::fwprintf(stderr, L"Smoke: cannot create native control canvas.\n");
        return false;
    }
    canvas.previous = SelectObject(canvas.device, canvas.bitmap);
    if (canvas.previous == nullptr || canvas.previous == HGDI_ERROR) {
        canvas.previous = nullptr;
        return false;
    }
    const UINT query = combo ? CB_GETCURSEL : BM_GETCHECK;
    const auto state = SendMessageW(control, query, 0, 0);
    const auto draw = [&] {
        SavedDc saved(canvas.device);
        Fill(canvas.device, bounds, RGB(255, 0, 255));
        SendMessageW(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(canvas.device), PRF_CLIENT);
        GdiFlush();
        const auto* begin = static_cast<const DWORD*>(pixels);
        const auto count = static_cast<size_t>(bounds.right) * static_cast<size_t>(bounds.bottom);
        const bool painted = std::any_of(begin, begin + count,
            [](DWORD pixel) { return (pixel & 0x00FFFFFF) != 0x00FF00FF; });
        if (checkbox && theme->Colors().dark) {
            const auto& colors = theme->Colors();
            const auto foreground = IsWindowEnabled(control) ? colors.text : colors.disabled;
            const auto threshold = (GetRValue(foreground) + GetRValue(colors.window)) / 2;
            size_t readable = 0;
            // Exclude the native check glyph, so its bright pixels cannot hide a black caption.
            for (LONG y = 0; y < bounds.bottom; ++y) {
                for (LONG x = Scale(control, 32); x < bounds.right; ++x) {
                    const auto pixel = begin[static_cast<size_t>(y) * bounds.right + x];
                    if ((pixel & 0x00FFFFFF) != 0x00FF00FF &&
                        GetRValue(pixel) > threshold && GetGValue(pixel) > threshold &&
                        GetBValue(pixel) > threshold) {
                        ++readable;
                    }
                }
            }
            if (readable < 5) {
                std::fwprintf(stderr, L"Smoke: native checkbox caption contrast failed (id=%d, enabled=%d, pixels=%zu).\n",
                    GetDlgCtrlID(control), IsWindowEnabled(control), readable);
                return false;
            }
        }
        return painted;
    };
    bool passed = draw();
    SendMessageW(control, WM_MOUSEMOVE, 0, MAKELPARAM(Scale(control, 6), Scale(control, 6)));
    passed = draw() && passed;
    SendMessageW(control, WM_MOUSELEAVE, 0, 0);
    passed = draw() && passed && SendMessageW(control, query, 0, 0) == state;
    if (IsAppThemed() && IsThemeActive()) {
        passed = GetWindowTheme(control) != nullptr && passed;
    }
    if (!passed) {
        std::fwprintf(stderr, L"Smoke: native control rendering failed (id=%d).\n", GetDlgCtrlID(control));
    }
    return passed;
}

ThemeWindow::~ThemeWindow()
{
    Detach();
    DeleteObject(background_);
    DeleteObject(surface_);
    DeleteObject(logBrush_);
}

ThemeWindow* ThemeWindow::From(HWND window)
{
    return static_cast<ThemeWindow*>(GetPropW(window, kThemeProperty));
}

void ThemeWindow::Attach(HWND window)
{
    window_ = window;
    if (!SetPropW(window_, kThemeProperty, this)) {
        throw std::runtime_error(Text("Cannot attach the window theme.", "无法初始化窗口主题。"));
    }
    Apply(false, true);
}

void ThemeWindow::Detach()
{
    if (window_ != nullptr && From(window_) == this) {
        EnumChildWindows(window_, [](HWND control, LPARAM) -> BOOL {
            RemoveWindowSubclass(control, ColorProcedure, kColorSubclassId);
            return TRUE;
        }, 0);
        RemovePropW(window_, kThemeProperty);
    }
    window_ = nullptr;
}

bool ThemeWindow::AttachControl(HWND control)
{
    const bool combo = IsClass(control, L"ComboBox");
    const bool edit = IsClass(control, L"Edit") || IsRichEdit(control);
    if (!combo && !edit && !IsClass(control, L"Button") && !IsClass(control, L"ComboLBox")) {
        return true;
    }
    const wchar_t* visualStyle = colors_.dark
        ? combo ? L"DarkMode_CFD" : L"DarkMode_Explorer"
        : L"Explorer";
    SetWindowTheme(control, visualStyle, nullptr);
    if (IsRichEdit(control)) {
        SendMessageW(control, EM_SETBKGNDCOLOR, 0, colors_.logBackground);
    }
    if (combo) {
        // Forward the native list's color query without intercepting any painting.
        if (!SetWindowSubclass(control, ColorProcedure, kColorSubclassId, reinterpret_cast<DWORD_PTR>(this))) {
            return false;
        }
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info) && info.hwndList != nullptr) {
            SetWindowTheme(info.hwndList, colors_.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
    }
    return true;
}

void ThemeWindow::Apply(bool dark, bool force)
{
    if (window_ == nullptr || (!force && background_ != nullptr && colors_.dark == dark)) {
        return;
    }
    const auto next = PaletteFor(dark);
    HBRUSH background = CreateSolidBrush(next.window);
    HBRUSH surface = CreateSolidBrush(next.surface);
    HBRUSH log = CreateSolidBrush(next.logBackground);
    if (background == nullptr || surface == nullptr || log == nullptr) {
        DeleteObject(background);
        DeleteObject(surface);
        DeleteObject(log);
        throw std::runtime_error(Text("Cannot create theme brushes.", "无法创建主题画刷。"));
    }
    DeleteObject(background_);
    DeleteObject(surface_);
    DeleteObject(logBrush_);
    background_ = background;
    surface_ = surface;
    logBrush_ = log;
    colors_ = next;
    const BOOL enabled = dark;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &colors_.window, sizeof(colors_.window));
    DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR, &colors_.text, sizeof(colors_.text));
    struct Controls { ThemeWindow* self; bool success = true; } controls{this};
    EnumChildWindows(window_, [](HWND control, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Controls*>(parameter);
        if (!context.self->AttachControl(control)) {
            context.success = false;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&controls));
    if (!controls.success) {
        throw std::runtime_error(Text("Cannot theme a window control.", "无法设置控件主题。"));
    }
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void ThemeWindow::ApplyOwned(bool dark)
{
    struct Change { HWND owner; bool dark; std::exception_ptr error; } change{window_, dark, {}};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
        auto& value = *reinterpret_cast<Change*>(parameter);
        if (window != value.owner && GetWindow(window, GW_OWNER) == value.owner) {
            if (auto* theme = From(window)) {
                // All registered windows belong to this UI thread.
                try {
                    theme->Apply(value.dark);
                } catch (...) {
                    value.error = std::current_exception();
                    return FALSE;
                }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&change));
    if (change.error) {
        std::rethrow_exception(change.error);
    }
}

void ThemeWindow::MeasureItem(MEASUREITEMSTRUCT& item)
{
    const auto* text = MenuText(item.itemData);
    HDC device = GetDC(window_);
    SIZE size{};
    if (device != nullptr) {
        const auto previous = SelectObject(device, WindowFont(window_, font_));
        GetTextExtentPoint32W(device, text, static_cast<int>(std::wcslen(text)), &size);
        SelectObject(device, previous);
        ReleaseDC(window_, device);
    }
    item.itemWidth = static_cast<UINT>(size.cx + Scale(window_, 32));
    item.itemHeight = static_cast<UINT>((std::max<LONG>)(size.cy + Scale(window_, 10), Scale(window_, 24)));
}

void ThemeWindow::DrawItem(const DRAWITEMSTRUCT& item)
{
    SavedDc saved(item.hDC);
    const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    Fill(item.hDC, item.rcItem, selected ? colors_.selection : colors_.window);
    SetTextColor(item.hDC, disabled ? colors_.disabled : selected ? colors_.selectionText : colors_.text);
    SetBkMode(item.hDC, TRANSPARENT);
    SelectObject(item.hDC, WindowFont(window_, font_));
    const auto* text = MenuText(item.itemData);
    RECT bounds = item.rcItem;
    bounds.left += Scale(window_, 14);
    bounds.right -= Scale(window_, 5);
    DrawTextW(item.hDC, text, static_cast<int>(std::wcslen(text)), &bounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(item.hDC, &item.rcItem);
    }
}

bool ThemeWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (background_ == nullptr) {
        return false;
    }
    if (message == WM_ERASEBKGND) {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, background_);
        result = TRUE;
        return true;
    }
    if (message == WM_NOTIFY && lParam != 0 && colors_.dark) {
        const auto& item = *reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
        if (item.hdr.code == NM_CUSTOMDRAW && item.dwDrawStage == CDDS_PREPAINT &&
            GetParent(item.hdr.hwndFrom) == window_ && IsClass(item.hdr.hwndFrom, L"Button") &&
            (GetWindowLongPtrW(item.hdr.hwndFrom, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX &&
            DrawCheckboxCaption(item.hdr.hwndFrom, item, colors_)) {
            // Native checkbox painting has drawn the glyph in CDDS_PREERASE; replace only its caption.
            result = CDRF_SKIPDEFAULT;
            return true;
        }
    }
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC) {
        const HWND control = reinterpret_cast<HWND>(lParam);
        const bool rich = IsRichEdit(control);
        const bool input = rich || IsClass(control, L"Edit") || message == WM_CTLCOLORLISTBOX;
        const auto color = rich ? colors_.logBackground : input ? colors_.surface : colors_.window;
        const HDC device = reinterpret_cast<HDC>(wParam);
        SetBkColor(device, color);
        SetTextColor(device, IsWindowEnabled(control) ? colors_.text : colors_.disabled);
        result = reinterpret_cast<LRESULT>(rich ? logBrush_ : input ? surface_ : background_);
        return true;
    }
    if (message == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_MENU && item.itemData != 0) {
            MeasureItem(item);
            result = TRUE;
            return true;
        }
    }
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_MENU && item.itemData != 0) {
            DrawItem(item);
            result = TRUE;
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK ThemeWindow::ColorProcedure(HWND control, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR parameter)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, ColorProcedure, kColorSubclassId);
        return DefSubclassProc(control, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<ThemeWindow*>(parameter);
    LRESULT result = 0;
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC &&
        self->Handle(message, wParam, lParam, result)) {
        return result;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

} // namespace xtray
