#pragma once

#include "theme_window.h"
#include "xray_tool.h"

#include <exception>

namespace xtray {

class XrayToolWindow {
public:
    explicit XrayToolWindow(std::filesystem::path root) : task_(std::move(root)) {}
    ~XrayToolWindow();
    void Open(HWND owner, XrayToolKind kind, const Settings& settings,
        bool settingsValid, bool dark, bool quiet = false);
    void Close();
    void RefreshLanguage();
    void SetAppearance(bool dark);
    bool Translate(MSG& message) const;
    HWND Window() const { return window_; }
    bool Running() const { return task_.Running(); }
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Initialize();
    void SetFonts();
    void Layout();
    void Poll();
    void SetDocument();
    void CopyAll() const;
    void ApplyTextColors();
    const wchar_t* Title() const;
    std::wstring Status() const;
    XrayTool task_;
    XrayToolKind kind_ = XrayToolKind::Check;
    std::optional<XrayToolResult> result_;
    std::wstring document_;
    ThemeWindow theme_;
    HWND window_ = nullptr, owner_ = nullptr;
    HWND status_ = nullptr, note_ = nullptr, output_ = nullptr, copy_ = nullptr, close_ = nullptr;
    HFONT font_ = nullptr, outputFont_ = nullptr;
    bool dark_ = false, quiet_ = false;
    std::exception_ptr error_;
};

bool SmokeTestXrayTools(HWND owner, const std::filesystem::path& root);

} // namespace xtray
