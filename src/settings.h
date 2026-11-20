#pragma once

#include "language.h"
#include "theme.h"

#include <filesystem>
#include <ranges>
#include <set>
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
    std::set<size_t> disabledConfigFiles;

    auto EnabledConfigFiles() const
    {
        return std::views::iota(size_t{0}, configFiles.size()) |
            std::views::filter([this](size_t index) { return !disabledConfigFiles.contains(index); }) |
            std::views::transform([this](size_t index) -> const std::wstring& { return configFiles[index]; });
    }

    bool HasConfigSources() const
    {
        return !configDirectory.empty() || !EnabledConfigFiles().empty();
    }

    bool LoadsConfigDirectoryFirst() const
    {
        return configDirectoryFirst && !configDirectory.empty() && !EnabledConfigFiles().empty();
    }
};

struct SettingsResult {
    Settings settings;
    bool success = true;
    std::wstring error;
};

SettingsResult LoadSettings(const std::filesystem::path& root);
bool SaveSettings(const std::filesystem::path& root, const Settings& settings, std::wstring& error);

} // namespace xtray
