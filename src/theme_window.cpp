#include "theme_window.h"
#include "win32.h"

#include <dwmapi.h>
#include <oleacc.h>
#include <richedit.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwchar>
#include <exception>
#include <iterator>

namespace xtray {
namespace {

constexpr wchar_t kThemeProperty[] = L"XTray.ThemeWindow";
constexpr wchar_t kDisabledIconProperty[] = L"XTray.DisabledButtonIcon";
constexpr UINT_PTR kSubclassId = 1;

struct PaintSession {
    HWND window;
    PAINTSTRUCT paint{};
    HDC device;
    explicit PaintSession(HWND value) : window(value), device(BeginPaint(window, &paint)) {}
    ~PaintSession() { EndPaint(window, &paint); }
};

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

void Frame(HDC device, const RECT& bounds, COLORREF color)
{
    SetDCBrushColor(device, color);
    FrameRect(device, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

HFONT WindowFont(HWND window, HFONT fallback)
{
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    return font != nullptr ? font : fallback != nullptr ? fallback
        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

std::wstring ComboText(HWND control, UINT index)
{
    if (index == static_cast<UINT>(-1)) {
        return {};
    }
    const auto length = SendMessageW(control, CB_GETLBTEXTLEN, index, 0);
    if (length == CB_ERR) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const auto copied = SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(copied == CB_ERR ? 0 : static_cast<size_t>(copied));
    return text;
}

int Scale(HWND window, int value)
{
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

const wchar_t* MenuText(ULONG_PTR data)
{
    const auto* item = reinterpret_cast<const MSAAMENUINFO*>(data);
    return item != nullptr && item->dwMSAASignature == MSAA_MENU_SIG && item->pszWText != nullptr
        ? item->pszWText : L"";
}

void Arrow(HDC device, const RECT& area, COLORREF color, bool down, int size)
{
    const LONG x = area.left + (area.right - area.left) / 2;
    const LONG y = area.top + (area.bottom - area.top) / 2;
    const LONG offset = (std::max)(1, size);
    POINT points[] = {{x - offset, y + (down ? -offset / 2 : offset / 2)},
        {x, y + (down ? offset / 2 : -offset / 2)},
        {x + offset, y + (down ? -offset / 2 : offset / 2)}};
    SelectObject(device, GetStockObject(DC_PEN));
    SetDCPenColor(device, color);
    Polyline(device, points, static_cast<int>(std::size(points)));
}

} // namespace

void SetButtonIcons(HWND control, HICON normal, HICON disabled)
{
    if (disabled != nullptr) {
        if (!SetPropW(control, kDisabledIconProperty, reinterpret_cast<HANDLE>(disabled))) {
            throw std::runtime_error(ToUtf8(WindowsError(
                Text(L"Cannot set a button icon", L"无法设置按钮图标"))));
        }
    } else {
        RemovePropW(control, kDisabledIconProperty);
    }
    SendMessageW(control, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(normal));
    InvalidateRect(control, nullptr, FALSE);
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
            RemovePropW(control, kDisabledIconProperty);
            RemoveWindowSubclass(control, ControlProcedure, kSubclassId);
            if (IsClass(control, L"ComboBox")) {
                COMBOBOXINFO info{sizeof(info)};
                if (GetComboBoxInfo(control, &info)) {
                    RemoveWindowSubclass(info.hwndList, ControlProcedure, kSubclassId);
                }
            }
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
    if (!SetWindowSubclass(control, ControlProcedure, kSubclassId, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    SetWindowTheme(control, L"", L"");
    const LONG_PTR extended = GetWindowLongPtrW(control, GWL_EXSTYLE);
    if ((extended & WS_EX_CLIENTEDGE) != 0) {
        SetWindowLongPtrW(control, GWL_EXSTYLE, extended & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_BORDER);
        SetWindowPos(control, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (IsRichEdit(control)) {
        SendMessageW(control, EM_SETBKGNDCOLOR, 0, colors_.logBackground);
    }
    if (combo) {
        SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(control, 18));
        SendMessageW(control, CB_SETITEMHEIGHT, 0, Scale(control, 22));
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info) && info.hwndList != nullptr) {
            if (!SetWindowSubclass(info.hwndList, ControlProcedure, kSubclassId,
                reinterpret_cast<DWORD_PTR>(this))) {
                return false;
            }
            SetWindowTheme(info.hwndList, L"", L"");
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

void ThemeWindow::PaintControl(HWND control, HDC device)
{
    SavedDc saved(device);
    RECT bounds{};
    GetClientRect(control, &bounds);
    const bool disabled = !IsWindowEnabled(control);
    const bool focused = GetFocus() == control;
    SelectObject(device, WindowFont(control, font_));
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, disabled ? colors_.disabled : colors_.text);
    std::wstring text;
    if (IsClass(control, L"Button")) {
        const auto state = SendMessageW(control, BM_GETSTATE, 0, 0);
        POINT cursor{};
        GetCursorPos(&cursor);
        RECT screen{};
        GetWindowRect(control, &screen);
        const bool hot = PtInRect(&screen, cursor) != FALSE && !disabled;
        const auto type = GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK;
        const bool checkbox = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX;
        if (checkbox) {
            Fill(device, bounds, colors_.window);
            const LONG side = Scale(control, 13);
            RECT box{bounds.left + Scale(control, 1), bounds.top +
                (bounds.bottom - bounds.top - side) / 2, 0, 0};
            box.right = box.left + side;
            box.bottom = box.top + side;
            const bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
            Fill(device, box, checked && !disabled ? colors_.selection
                : (state & BST_PUSHED) != 0 ? colors_.pressed : hot ? colors_.hot : colors_.surface);
            Frame(device, box, disabled ? colors_.disabled : colors_.border);
            if (checked) {
                SelectObject(device, GetStockObject(DC_PEN));
                SetDCPenColor(device, disabled ? colors_.disabled : colors_.selectionText);
                for (int offset = 0; offset < (std::max)(1, Scale(control, 2)); ++offset) {
                    POINT tick[] = {
                        {box.left + Scale(control, 3), box.top + Scale(control, 6) + offset},
                        {box.left + Scale(control, 5), box.top + Scale(control, 9) + offset},
                        {box.left + Scale(control, 10), box.top + Scale(control, 3) + offset}};
                    Polyline(device, tick, static_cast<int>(std::size(tick)));
                }
            }
            bounds.left = box.right + Scale(control, 5);
        } else {
            Fill(device, bounds, (state & BST_PUSHED) != 0 ? colors_.pressed
                : hot ? colors_.hot : colors_.button);
            Frame(device, bounds, focused || type == BS_DEFPUSHBUTTON
                ? colors_.selection : colors_.border);
        }
        if (!checkbox && (GetWindowLongPtrW(control, GWL_STYLE) & BS_ICON) != 0) {
            auto icon = disabled
                ? reinterpret_cast<HICON>(GetPropW(control, kDisabledIconProperty)) : nullptr;
            if (icon == nullptr) {
                icon = reinterpret_cast<HICON>(SendMessageW(control, BM_GETIMAGE, IMAGE_ICON, 0));
            }
            const int side = Scale(control, 20);
            DrawIconEx(device, (static_cast<int>(bounds.right) - side) / 2,
                (static_cast<int>(bounds.bottom) - side) / 2, icon, side, side, 0, nullptr, DI_NORMAL);
        } else {
            const int length = GetWindowTextLengthW(control);
            text.resize(static_cast<size_t>(length) + 1);
            text.resize(static_cast<size_t>(GetWindowTextW(control, text.data(), length + 1)));
            DrawTextW(device, text.c_str(), static_cast<int>(text.size()), &bounds,
                (checkbox ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    ((SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0 ? DT_HIDEPREFIX : 0));
        }
    } else {
        Fill(device, bounds, colors_.surface);
        Frame(device, bounds, focused ? colors_.selection : colors_.border);
        RECT arrow = bounds;
        arrow.left = (std::max)(LONG{0}, bounds.right - Scale(control, 22));
        Arrow(device, arrow, disabled ? colors_.disabled : colors_.text, true, Scale(control, 4));
        text = ComboText(control, static_cast<UINT>(SendMessageW(control, CB_GETCURSEL, 0, 0)));
        bounds.left += Scale(control, 5);
        bounds.right = arrow.left;
        DrawTextW(device, text.c_str(), static_cast<int>(text.size()), &bounds,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (focused && !disabled &&
        (SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
        GetClientRect(control, &bounds);
        InflateRect(&bounds, -Scale(control, 3), -Scale(control, 3));
        DrawFocusRect(device, &bounds);
    }
}

void ThemeWindow::PaintFrame(HWND control)
{
    if (IsClass(control, L"Button") || IsClass(control, L"ComboBox")) {
        return;
    }
    const HDC device = GetWindowDC(control);
    if (device == nullptr) {
        return;
    }
    {
        SavedDc saved(device);
        RECT window{};
        GetWindowRect(control, &window);
        const RECT bounds{0, 0, window.right - window.left, window.bottom - window.top};
        Frame(device, bounds, GetFocus() == control ? colors_.selection : colors_.border);
        SCROLLBARINFO scroll{sizeof(scroll)};
        if (GetScrollBarInfo(control, OBJID_VSCROLL, &scroll) &&
            (scroll.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) == 0) {
            RECT bar = scroll.rcScrollBar;
            OffsetRect(&bar, -window.left, -window.top);
            Fill(device, bar, colors_.surface);
            RECT up = bar, down = bar;
            up.bottom = up.top + scroll.dxyLineButton;
            down.top = down.bottom - scroll.dxyLineButton;
            const auto text = (scroll.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) != 0
                ? colors_.disabled : colors_.text;
            Arrow(device, up, text, false, Scale(control, 3));
            Arrow(device, down, text, true, Scale(control, 3));
            if (scroll.xyThumbBottom > scroll.xyThumbTop &&
                (scroll.rgstate[3] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE)) == 0) {
                RECT thumb{bar.left + Scale(control, 3), bar.top + scroll.xyThumbTop,
                    bar.right - Scale(control, 3), bar.top + scroll.xyThumbBottom};
                Fill(device, thumb, colors_.dark ? RGB(128, 128, 128) : RGB(168, 168, 168));
            }
        }
    }
    ReleaseDC(control, device);
}

void ThemeWindow::MeasureItem(MEASUREITEMSTRUCT& item)
{
    if (item.CtlType == ODT_COMBOBOX) {
        item.itemHeight = static_cast<UINT>(Scale(window_, 22));
        return;
    }
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
    const bool edit = (item.itemState & ODS_COMBOBOXEDIT) != 0;
    const bool highlighted = selected && !edit;
    Fill(item.hDC, item.rcItem, highlighted ? colors_.selection
        : item.CtlType == ODT_MENU ? colors_.window : colors_.surface);
    SetTextColor(item.hDC, disabled ? colors_.disabled
        : highlighted ? colors_.selectionText : colors_.text);
    SetBkMode(item.hDC, TRANSPARENT);
    SelectObject(item.hDC, WindowFont(item.CtlType == ODT_MENU ? window_ : item.hwndItem, font_));
    const auto text = item.CtlType == ODT_MENU
        ? std::wstring(MenuText(item.itemData))
        : ComboText(item.hwndItem, item.itemID);
    RECT bounds = item.rcItem;
    bounds.left += Scale(window_, item.CtlType == ODT_MENU ? 14 : 5);
    bounds.right -= Scale(window_, 5);
    DrawTextW(item.hDC, text.c_str(), static_cast<int>(text.size()), &bounds,
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
        if ((item.CtlType == ODT_MENU && item.itemData != 0) || item.CtlType == ODT_COMBOBOX) {
            MeasureItem(item);
            result = TRUE;
            return true;
        }
    }
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if ((item.CtlType == ODT_MENU && item.itemData != 0) || item.CtlType == ODT_COMBOBOX) {
            DrawItem(item);
            result = TRUE;
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK ThemeWindow::ControlProcedure(HWND control, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR parameter)
{
    auto* self = reinterpret_cast<ThemeWindow*>(parameter);
    if (message == WM_NCDESTROY) {
        RemovePropW(control, kDisabledIconProperty);
        RemoveWindowSubclass(control, ControlProcedure, kSubclassId);
        return DefSubclassProc(control, message, wParam, lParam);
    }
    try {
        const bool painted = IsClass(control, L"Button") || IsClass(control, L"ComboBox");
        if (painted && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
            if (message == WM_PRINTCLIENT) {
                self->PaintControl(control, reinterpret_cast<HDC>(wParam));
            } else {
                PaintSession paint(control);
                if (paint.device != nullptr) {
                    self->PaintControl(control, paint.device);
                }
            }
            return 0;
        }
        LRESULT color = 0;
        if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC &&
            self->Handle(message, wParam, lParam, color)) {
            return color;
        }
        if (message == WM_MOUSEMOVE && painted) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
            TrackMouseEvent(&track);
        }
        const auto result = DefSubclassProc(control, message, wParam, lParam);
        if (message == WM_NCPAINT || message == WM_THEMECHANGED || message == WM_SIZE || message == WM_VSCROLL ||
            message == WM_MOUSEWHEEL || message == WM_NCLBUTTONDOWN ||
            message == WM_NCMOUSEMOVE || message == EM_SETSCROLLPOS ||
            message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            self->PaintFrame(control);
        }
        if (painted && (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE ||
            message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
            message == WM_SETTEXT || message == WM_SETFONT || message == WM_UPDATEUISTATE ||
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN ||
            message == WM_KEYUP || message == BM_SETIMAGE || message == BM_SETSTATE || message == BM_SETCHECK || message == BM_SETSTYLE ||
            message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
            message == CB_SETCURSEL || message == CB_SHOWDROPDOWN || message == CB_RESETCONTENT)) {
            InvalidateRect(control, nullptr, FALSE);
        }
        return result;
    } catch (...) {
        return DefSubclassProc(control, message, wParam, lParam);
    }
}

} // namespace xtray
