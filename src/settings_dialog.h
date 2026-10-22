#pragma once

#include "logs.h"
#include "settings.h"
#include "startup.h"
#include <windows.h>

namespace xtray {

class XrayToolWindow;

bool EditSettings(HWND owner, const std::filesystem::path& root, Settings& settings, Logs& logs,
    StartupRegistration& startup, XrayToolWindow* tools = nullptr);
std::wstring WindowText(HWND window);
bool SmokeTestSettingsLayout(HWND owner, const std::filesystem::path& root);
bool SmokeTestSettingsTheme(HWND owner, const std::filesystem::path& root);
bool SmokeTestSettingsStartup(HWND owner, const std::filesystem::path& root);

} // namespace xtray
