#pragma once

#include "language.h"
#include "theme.h"

#include <filesystem>
#include <string>
#include <vector>

namespace xtray {

struct Settings {
    std::wstring executable = L".\\bin\\xray.exe";
    std::vector<std::wstring> configFiles;
    std::wstring configDirectory;
    bool configDirectoryFirst = false;
    size_t logLines = 5000;
    Language language = Language::Automatic;
    ThemeMode theme = ThemeMode::System;
    bool logWordWrap = true;
};

struct SettingsResult {
    Settings settings;
    bool success = true;
    std::wstring error;
};

SettingsResult LoadSettings(const std::filesystem::path& root);
bool SaveSettings(const std::filesystem::path& root, const Settings& settings, std::wstring& error);
std::vector<std::wstring> SplitConfigFiles(const std::wstring& text);

} // namespace xtray
