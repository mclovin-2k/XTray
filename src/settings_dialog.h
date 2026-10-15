#pragma once

#include "logs.h"
#include "settings.h"
#include <windows.h>

namespace xtray {

bool EditSettings(HWND owner, const std::filesystem::path& root, Settings& settings, Logs& logs);
std::wstring WindowText(HWND window);

} // namespace xtray
