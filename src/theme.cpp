#include "theme.h"

#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <atomic>
#include <optional>

namespace xtray {

bool ResolveDark(ThemeMode preference, bool systemDark)
{
    return preference == ThemeMode::Dark || (preference == ThemeMode::System && systemDark);
}

bool IsLightColor(BYTE red, BYTE green, BYTE blue)
{
    return 5 * green + 2 * red + blue > 8 * 128;
}

ThemePalette PaletteFor(bool dark)
{
    if (dark) {
        return {true, RGB(32, 32, 32), RGB(43, 43, 43), RGB(230, 230, 230),
            RGB(176, 176, 176), RGB(112, 112, 112), RGB(51, 51, 51),
            RGB(69, 69, 69), RGB(37, 37, 37), RGB(0, 103, 192), RGB(255, 255, 255),
            RGB(30, 30, 30), RGB(253, 186, 116), RGB(230, 230, 230)};
    }
    return {false, RGB(255, 255, 255), RGB(255, 255, 255), RGB(32, 32, 32),
        RGB(107, 107, 107), RGB(138, 138, 138), RGB(245, 245, 245),
        RGB(229, 241, 251), RGB(204, 228, 247), RGB(0, 103, 192), RGB(255, 255, 255),
        RGB(255, 255, 255), RGB(180, 83, 9), RGB(32, 32, 32)};
}

struct ThemeMonitor::State {
    std::shared_ptr<std::atomic<HWND>> window;
    winrt::Windows::UI::ViewManagement::UISettings settings{nullptr};
    std::optional<winrt::event_token> token;
    bool initialized = false;

    explicit State(HWND target) : window(std::make_shared<std::atomic<HWND>>(target))
    {
        initialized = SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED));
        if (!initialized) {
            return;
        }
        try {
            settings = winrt::Windows::UI::ViewManagement::UISettings();
            const auto destination = window;
            token = settings.ColorValuesChanged([destination](const auto&, const auto&) noexcept {
                if (const HWND targetWindow = destination->load()) {
                    PostMessageW(targetWindow, kSystemThemeMessage, 0, 0);
                }
            });
        } catch (const winrt::hresult_error&) {
            // WM_SETTINGCHANGE remains a fallback if event registration is unavailable.
        }
    }

    ~State()
    {
        window->store(nullptr);
        if (settings && token) {
            try {
                settings.ColorValuesChanged(*token);
            } catch (const winrt::hresult_error&) {
            }
        }
        settings = nullptr;
        if (initialized) {
            RoUninitialize();
        }
    }
};

ThemeMonitor::ThemeMonitor(HWND window) : state_(std::make_unique<State>(window)) {}
ThemeMonitor::~ThemeMonitor() = default;

bool ThemeMonitor::SystemDark() const
{
    if (state_ && state_->settings) {
        try {
            const auto color = state_->settings.GetColorValue(
                winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
            return IsLightColor(color.R, color.G, color.B);
        } catch (const winrt::hresult_error&) {
        }
    }
    return false;
}

} // namespace xtray
