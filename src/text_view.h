#pragma once

#include "win32.h"
#include "theme.h"

#include <richedit.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>
#include <vector>

namespace xtray {

inline bool TextViewAtEnd(HWND window)
{
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    return !GetScrollInfo(window, SB_VERT, &scroll) ||
        scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax - 1;
}

struct TextViewStream {
    std::vector<BYTE> bytes;
    size_t offset = 0;
    std::exception_ptr error;
    static DWORD CALLBACK Write(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* copied)
    {
        auto& stream = *reinterpret_cast<TextViewStream*>(cookie);
        try {
            stream.bytes.insert(stream.bytes.end(), buffer, buffer + count);
            *copied = count;
            return 0;
        } catch (...) {
            stream.error = std::current_exception();
            *copied = 0;
            return ERROR_NOT_ENOUGH_MEMORY;
        }
    }
    static DWORD CALLBACK Read(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* copied)
    {
        auto& stream = *reinterpret_cast<TextViewStream*>(cookie);
        const auto size = (std::min)(static_cast<size_t>(count), stream.bytes.size() - stream.offset);
        if (size != 0) { std::memcpy(buffer, stream.bytes.data() + stream.offset, size); }
        stream.offset += size;
        *copied = static_cast<LONG>(size);
        return 0;
    }
};

inline HWND RecreateTextView(HWND window, bool wrapped, const ThemePalette& palette)
{
    CHARRANGE selection{};
    POINT scroll{}, before{};
    SendMessageW(window, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(window, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    const auto line = SendMessageW(window, EM_GETFIRSTVISIBLELINE, 0, 0);
    const LONG character = (std::max)(LONG{0},
        static_cast<LONG>(SendMessageW(window, EM_LINEINDEX, line, 0)));
    SendMessageW(window, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&before), character);
    const auto mode = SendMessageW(window, EM_GETTEXTMODE, 0, 0) & TM_PLAINTEXT
        ? TM_PLAINTEXT : TM_RICHTEXT;
    const WPARAM format = mode == TM_PLAINTEXT ? SF_TEXT | SF_UNICODE : SF_RTF;
    TextViewStream text;
    EDITSTREAM saved{reinterpret_cast<DWORD_PTR>(&text), 0, TextViewStream::Write};
    SendMessageW(window, EM_STREAMOUT, format, reinterpret_cast<LPARAM>(&saved));
    if (text.error) { std::rethrow_exception(text.error); }
    if (saved.dwError != 0) {
        throw std::runtime_error(Text("Cannot preserve the text before changing word wrap.",
            "无法在切换自动换行前保留文本。"));
    }

    const HWND parent = GetParent(window);
    RECT bounds{};
    GetWindowRect(window, &bounds);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    DWORD nextStyle = style & ~(WS_VISIBLE | WS_HSCROLL | ES_AUTOHSCROLL);
    if (!wrapped) { nextStyle |= WS_HSCROLL | ES_AUTOHSCROLL; }
    struct Window {
        HWND value = nullptr;
        ~Window() { if (value != nullptr) { DestroyWindow(value); } }
    } next;
    // Establish wrap and scrollbar styles at creation, as for the initial working view.
    next.value = CreateWindowExW(extended, MSFTEDIT_CLASS, L"", nextStyle,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(GetDlgCtrlID(window))),
        GetModuleHandleW(nullptr), nullptr);
    if (next.value == nullptr) {
        throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot change word wrap", L"切换自动换行失败"))));
    }
    SendMessageW(next.value, EM_SETTEXTMODE, mode, 0);
    SendMessageW(next.value, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(next.value, EM_EXLIMITTEXT, 0, SendMessageW(window, EM_GETLIMITTEXT, 0, 0));
    SendMessageW(next.value, WM_SETFONT, static_cast<WPARAM>(SendMessageW(window, WM_GETFONT, 0, 0)), FALSE);
    SetWindowTheme(next.value, palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SendMessageW(next.value, EM_SETBKGNDCOLOR, 0, palette.logBackground);
    EDITSTREAM restored{reinterpret_cast<DWORD_PTR>(&text), 0, TextViewStream::Read};
    SendMessageW(next.value, EM_STREAMIN, format, reinterpret_cast<LPARAM>(&restored));
    if (restored.dwError != 0 || text.offset != text.bytes.size()) {
        throw std::runtime_error(Text("Cannot restore the complete text after changing word wrap.",
            "切换自动换行后无法还原完整文本。"));
    }
    if (mode == TM_PLAINTEXT) {
        CHARFORMAT2W colors{};
        colors.cbSize = sizeof(colors);
        colors.dwMask = CFM_COLOR | CFM_BACKCOLOR;
        colors.crTextColor = palette.text;
        colors.crBackColor = palette.logBackground;
        SendMessageW(next.value, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&colors));
    }
    SendMessageW(next.value, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    POINT after{}, current{};
    SendMessageW(next.value, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&after), character);
    SendMessageW(next.value, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current));
    POINT target{wrapped ? 0 : scroll.x, (std::max)(LONG{0}, current.y + after.y - before.y)};
    SendMessageW(next.value, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&target));
    SendMessageW(next.value, EM_SETMODIFY, SendMessageW(window, EM_GETMODIFY, 0, 0), 0);
    SendMessageW(next.value, EM_SETEVENTMASK, 0, SendMessageW(window, EM_GETEVENTMASK, 0, 0));
    if (!SetWindowPos(next.value, window, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        throw std::runtime_error("Cannot preserve the text control order.");
    }
    const bool focused = GetFocus() == window;
    const HWND result = std::exchange(next.value, nullptr);
    if ((style & WS_VISIBLE) != 0) { ShowWindow(result, SW_SHOWNOACTIVATE); }
    if (focused) { SetFocus(result); }
    DestroyWindow(window);
    RedrawWindow(result, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    return result;
}

inline void CopyTextToClipboard(HWND owner, std::wstring_view text)
{
    if (text.empty()) { return; }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data == nullptr) { throw std::runtime_error(Text("Cannot allocate clipboard text.", "无法分配剪贴板文本。")); }
    const auto pointer = static_cast<wchar_t*>(GlobalLock(data));
    if (pointer == nullptr) {
        GlobalFree(data);
        throw std::runtime_error(Text("Cannot access clipboard text.", "无法访问剪贴板文本。"));
    }
    std::memcpy(pointer, text.data(), text.size() * sizeof(wchar_t));
    pointer[text.size()] = L'\0';
    GlobalUnlock(data);
    if (!OpenClipboard(owner)) {
        GlobalFree(data);
        throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot open the clipboard", L"打开剪贴板失败"))));
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data) != nullptr;
    CloseClipboard();
    if (!copied) {
        GlobalFree(data);
        throw std::runtime_error(Text("Cannot copy all text.", "无法复制全部文本。"));
    }
}

inline const wchar_t* WordWrapCaption(bool enabled)
{
    return enabled ? Text(L"Word wrap: on", L"自动换行：已开启")
        : Text(L"Word wrap: off", L"自动换行：已关闭");
}

} // namespace xtray
