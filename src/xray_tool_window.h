#pragma once

#include "theme_window.h"
#include "xray_tool.h"
#include "xray_process.h"

#include <array>
#include <exception>

namespace xtray {

enum class XrayToolSource { SavedSettings, SettingsDraft };

class XrayToolWindow {
public:
    explicit XrayToolWindow(std::filesystem::path root) : task_(std::move(root)) {}
    ~XrayToolWindow();
    void Open(HWND owner, XrayToolKind kind, const Settings& settings,
        bool settingsValid, bool dark, bool quiet = false,
        XrayToolSource source = XrayToolSource::SavedSettings);
    void OpenRunInfo(HWND owner, XrayProcess& process, bool dark, bool quiet = false);
    void Close();
    void CloseOwned(HWND owner);
    void RefreshLanguage();
    void SetAppearance(bool dark);
    bool Translate(MSG& message) const;
    static bool TranslateAny(MSG& message);
    HWND Window() const { return window_; }
    bool Running() const { return task_.Running(); }
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Prepare(HWND owner, bool dark, bool quiet);
    void Show();
    void Initialize();
    void SetFonts();
    void Layout();
    void Poll();
    void SetDocument(bool preserveView = false);
    void CopyAll() const;
    void UpdateToolbar();
    void ApplyTextColors();
    const wchar_t* Title() const;
    std::wstring Status() const;
    XrayTool task_;
    XrayProcess* process_ = nullptr;
    XrayToolKind kind_ = XrayToolKind::Check;
    XrayToolSource source_ = XrayToolSource::SavedSettings;
    std::optional<XrayToolResult> result_;
    std::wstring document_;
    ThemeWindow theme_;
    HWND window_ = nullptr, owner_ = nullptr;
    HWND status_ = nullptr, note_ = nullptr, output_ = nullptr, copy_ = nullptr, close_ = nullptr;
    HWND wordWrapButton_ = nullptr, tooltip_ = nullptr;
    HFONT font_ = nullptr, outputFont_ = nullptr;
    std::array<HICON, 2> icons_{};
    std::array<HICON, 4> toolIcons_{};
    UINT toolIconDpi_ = 0;
    COLORREF toolIconColor_ = CLR_INVALID;
    bool wordWrap_ = false;
    bool dark_ = false, quiet_ = false;
    std::exception_ptr error_;
};

bool SmokeTestXrayTools(HWND owner, const std::filesystem::path& root);

} // namespace xtray
