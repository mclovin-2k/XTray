#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace xtray {

inline constexpr UINT kTrayCallbackMessage = WM_APP + 42;
inline constexpr UINT kTrayIconId = 1;

class TrayIcon {
public:
    using NotifyFunction = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
    explicit TrayIcon(NotifyFunction notify = Shell_NotifyIconW) : notify_(notify) {}
    ~TrayIcon() { Remove(); }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    bool Update(HWND window, HICON icon, const std::wstring& tooltip);
    void Lost() { added_ = false; }
    void Remove();
    void Focus();
    bool IsAdded() const { return added_; }
private:
    NotifyFunction notify_;
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};

} // namespace xtray
