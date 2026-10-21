#pragma once

#include "win32.h"

#include <richedit.h>

#include <algorithm>
#include <cstring>

namespace xtray {

inline bool TextViewAtEnd(HWND window)
{
    SCROLLINFO scroll{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    return !GetScrollInfo(window, SB_VERT, &scroll) ||
        scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax - 1;
}

inline void SetTextWordWrap(HWND window, bool enabled)
{
    CHARRANGE selection{};
    POINT scroll{}, before{};
    SendMessageW(window, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(window, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    const auto line = SendMessageW(window, EM_GETFIRSTVISIBLELINE, 0, 0);
    const LONG character = (std::max)(LONG{0},
        static_cast<LONG>(SendMessageW(window, EM_LINEINDEX, line, 0)));
    SendMessageW(window, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&before), character);
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    // A null target device uses the window width; width 1 disables soft wrapping.
    const bool changed = SendMessageW(window, EM_SETTARGETDEVICE, 0, enabled ? 0 : 1) != 0;
    SendMessageW(window, EM_SHOWSCROLLBAR, SB_HORZ, !enabled);
    SendMessageW(window, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    POINT after{}, current{};
    SendMessageW(window, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&after), character);
    SendMessageW(window, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current));
    POINT target{enabled ? 0 : scroll.x, (std::max)(LONG{0}, current.y + after.y - before.y)};
    SendMessageW(window, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&target));
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(window, nullptr, FALSE);
    if (!changed) {
        throw std::runtime_error(Text("Cannot change word wrapping.", "无法切换自动换行。"));
    }
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
