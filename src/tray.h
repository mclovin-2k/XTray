#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <string>

namespace xtray {

inline constexpr UINT kTrayCallbackMessage = WM_APP + 42;
inline constexpr UINT kTrayIconId = 1;

enum class TrayFailure { None, MissingWindow, MissingIcon, Add, SetVersion };

class TrayIcon {
public:
    using NotifyFunction = BOOL(WINAPI*)(DWORD, PNOTIFYICONDATAW);
    explicit TrayIcon(NotifyFunction notify = Shell_NotifyIconW) : notify_(notify) {}
    ~TrayIcon() { Remove(); }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    bool Update(HWND window, HICON icon, const std::wstring& tooltip);
    void UpdateTooltip(const std::wstring& tooltip);
    void Lost() { added_ = false; }
    void Remove();
    void Focus();
    bool ShowWarning(const wchar_t* title, const std::wstring& message);
    bool IsAdded() const { return added_; }
    uint64_t Registration() const { return registration_; }
    TrayFailure Failure() const { return failure_; }
    const wchar_t* FailureText() const;
private:
    NotifyFunction notify_;
    NOTIFYICONDATAW data_{};
    bool added_ = false;
    uint64_t registration_ = 0;
    TrayFailure failure_ = TrayFailure::None;
};

} // namespace xtray
