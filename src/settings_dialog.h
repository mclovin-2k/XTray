#pragma once

#include "logs.h"
#include "settings.h"
#include "startup.h"
#include <windows.h>

namespace xtray {

bool EditSettings(HWND owner, const std::filesystem::path& root, Settings& settings, Logs& logs,
    StartupRegistration& startup);
std::wstring WindowText(HWND window);
bool SmokeTestSettingsTheme(HWND owner, const std::filesystem::path& root);
bool SmokeTestSettingsStartup(HWND owner, const std::filesystem::path& root);

} // namespace xtray
