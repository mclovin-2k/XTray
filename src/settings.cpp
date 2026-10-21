#include "settings.h"
#include "win32.h"

#include <charconv>
#include <climits>
#include <map>

namespace xtray {
namespace {

bool IsBlank(std::wstring_view text)
{
    return text.find_first_not_of(L" \t\r") == std::wstring_view::npos;
}

bool IsPathValue(const std::wstring& text)
{
    return text.find_first_of(L"\r\n") == std::wstring::npos &&
        text.find(L'\0') == std::wstring::npos;
}

std::string ReadFileBytes(HANDLE file)
{
    std::string bytes;
    char buffer[8192];
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            throw std::runtime_error(Text("Cannot read settings.ini.", "无法读取 settings.ini。"));
        }
        if (read == 0) {
            return bytes;
        }
        bytes.append(buffer, read);
    }
}

size_t ParseNumber(const std::wstring& value)
{
    const auto text = ToUtf8(value);
    size_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error(Text("Invalid numeric setting.", "无效的数字设置。"));
    }
    return number;
}

} // namespace

std::vector<std::wstring> SplitConfigFiles(const std::wstring& text)
{
    std::vector<std::wstring> result;
    size_t begin = 0;
    while (begin < text.size()) {
        const auto newline = text.find(L'\n', begin);
        const auto end = newline == std::wstring::npos ? text.size() : newline;
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (!IsBlank(line)) {
            result.push_back(std::move(line));
        }
        if (newline == std::wstring::npos) {
            break;
        }
        begin = newline + 1;
    }
    return result;
}

SettingsResult LoadSettings(const std::filesystem::path& root)
{
    SettingsResult result;
    const auto path = root / L"data" / L"settings.ini";
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            result.success = false;
            result.error = WindowsError(Text(L"Cannot read settings", L"读取设置失败"), code);
        }
        return result;
    }
    try {
        auto bytes = ReadFileBytes(file.Get());
        if (bytes.starts_with("\xEF\xBB\xBF")) {
            bytes.erase(0, 3);
        }
        const auto text = FromUtf8(bytes);
        if (text.find(L'\0') != std::wstring::npos) {
            throw std::runtime_error(Text("NUL is not valid in settings.ini.", "settings.ini 不能包含空字符。"));
        }
        std::map<std::pair<std::wstring, std::wstring>, std::wstring> values;
        std::wstring section;
        size_t begin = 0;
        while (begin < text.size()) {
            const auto newline = text.find(L'\n', begin);
            const auto end = newline == std::wstring::npos ? text.size() : newline;
            auto line = text.substr(begin, end - begin);
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            if (!line.empty() && !IsBlank(line)) {
                if (line.front() == L'[' && line.back() == L']') {
                    section = line.substr(1, line.size() - 2);
                    if (section != L"XTray" && section != L"ConfigFiles") {
                        throw std::runtime_error(Text("Unknown settings section.", "未知的设置节。"));
                    }
                } else {
                    const auto equals = line.find(L'=');
                    if (section.empty() || equals == std::wstring::npos || equals == 0) {
                        throw std::runtime_error(Text("Invalid settings.ini line.", "无效的 settings.ini 行。"));
                    }
                    auto key = std::make_pair(section, line.substr(0, equals));
                    if (!values.emplace(std::move(key), line.substr(equals + 1)).second) {
                        throw std::runtime_error(Text("Duplicate settings key.", "设置键重复。"));
                    }
                }
            }
            if (newline == std::wstring::npos) {
                break;
            }
            begin = newline + 1;
        }
        const auto take = [&values](const std::wstring& group, const std::wstring& key) {
            const auto found = values.find({group, key});
            if (found == values.end()) {
                throw std::runtime_error(Text("Missing settings key.", "缺少必需的设置键。"));
            }
            auto value = found->second;
            values.erase(found);
            return value;
        };
        if (take(L"XTray", L"Version") != L"1") {
            throw std::runtime_error(Text("Unsupported settings version.", "不支持的设置文件版本。"));
        }
        auto& settings = result.settings;
        settings.executable = take(L"XTray", L"Executable");
        settings.configDirectory = take(L"XTray", L"ConfigDirectory");
        settings.logLines = ParseNumber(take(L"XTray", L"LogLines"));
        if (values.contains({L"XTray", L"Language"})) {
            const auto language = take(L"XTray", L"Language");
            if (language == L"auto") {
                settings.language = Language::Automatic;
            } else if (language == L"en") {
                settings.language = Language::English;
            } else if (language == L"zh") {
                settings.language = Language::Chinese;
            } else {
                throw std::runtime_error(Text("Invalid language setting.", "无效的语言设置。"));
            }
        }
        if (values.contains({L"XTray", L"Theme"})) {
            const auto theme = take(L"XTray", L"Theme");
            if (theme == L"system") {
                settings.theme = ThemeMode::System;
            } else if (theme == L"light") {
                settings.theme = ThemeMode::Light;
            } else if (theme == L"dark") {
                settings.theme = ThemeMode::Dark;
            } else {
                throw std::runtime_error(Text("Invalid appearance setting.", "无效的外观设置。"));
            }
        }
        if (values.contains({L"XTray", L"LogWordWrap"})) {
            const auto wrap = take(L"XTray", L"LogWordWrap");
            if (wrap != L"0" && wrap != L"1") {
                throw std::runtime_error(Text("Invalid log word wrap setting.", "无效的日志自动换行设置。"));
            }
            settings.logWordWrap = wrap == L"1";
        }
        const size_t count = ParseNumber(take(L"XTray", L"ConfigCount"));
        if (count > values.size()) {
            throw std::runtime_error(Text("Incomplete configuration file list.", "配置文件列表不完整。"));
        }
        for (size_t index = 0; index < count; ++index) {
            settings.configFiles.push_back(take(L"ConfigFiles", std::to_wstring(index)));
        }
        if (!values.empty() || IsBlank(settings.executable) ||
            !IsPathValue(settings.executable) || !IsPathValue(settings.configDirectory) ||
            settings.logLines == 0 || settings.logLines > INT_MAX) {
            throw std::runtime_error(Text("Invalid settings values.", "设置值无效。"));
        }
        for (const auto& config : settings.configFiles) {
            if (IsBlank(config) || !IsPathValue(config)) {
                throw std::runtime_error(Text("Invalid configuration file path.", "配置文件路径无效。"));
            }
        }
    } catch (const std::exception& exception) {
        result.success = false;
        result.settings = {};
        result.error = Text(L"Cannot read settings.ini: ", L"无法读取 settings.ini：") + FromUtf8(exception.what()) +
            Text(L" Re-enter and save the settings; the original file was not changed.", L" 请在设置中重新填写并保存；原文件未改动。");
    }
    return result;
}

bool SaveSettings(const std::filesystem::path& root, const Settings& settings, std::wstring& error)
{
    if (IsBlank(settings.executable) || !IsPathValue(settings.executable) ||
        !IsPathValue(settings.configDirectory) || settings.logLines == 0 ||
        settings.logLines > INT_MAX ||
        (settings.language != Language::Automatic && settings.language != Language::English &&
            settings.language != Language::Chinese) ||
        (settings.theme != ThemeMode::System && settings.theme != ThemeMode::Light &&
            settings.theme != ThemeMode::Dark)) {
        error = Text(L"Enter an Xray executable path, a positive log limit, and valid language and appearance choices. Paths cannot contain line breaks or NUL characters.", L"请填写 Xray 程序路径、有效的日志行数并选择语言和外观。路径不能包含换行或空字符。");
        return false;
    }
    for (const auto& config : settings.configFiles) {
        if (IsBlank(config) || !IsPathValue(config)) {
            error = Text(L"Configuration file paths cannot be empty or contain line breaks or NUL characters.", L"配置文件路径不能为空或包含换行、空字符。");
            return false;
        }
    }

    std::string bytes;
    try {
        std::wstring text = L"[XTray]\r\nVersion=1\r\nExecutable=" + settings.executable +
            L"\r\nConfigDirectory=" + settings.configDirectory +
            L"\r\nLogLines=" + std::to_wstring(settings.logLines) +
            L"\r\nLanguage=" + (settings.language == Language::Automatic ? L"auto"
                : settings.language == Language::English ? L"en" : L"zh") +
            L"\r\nTheme=" + (settings.theme == ThemeMode::System ? L"system"
                : settings.theme == ThemeMode::Light ? L"light" : L"dark") +
            L"\r\nLogWordWrap=" + (settings.logWordWrap ? L"1" : L"0") +
            L"\r\nConfigCount=" + std::to_wstring(settings.configFiles.size()) +
            L"\r\n\r\n[ConfigFiles]\r\n";
        for (size_t index = 0; index < settings.configFiles.size(); ++index) {
            text += std::to_wstring(index) + L"=" + settings.configFiles[index] + L"\r\n";
        }
        bytes = ToUtf8(text);
    } catch (const std::exception&) {
        error = Text(L"The settings contain invalid Unicode characters and were not saved.", L"设置包含无法编码的 Unicode 字符，未保存。");
        return false;
    }

    const auto directory = root / L"data";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = WindowsError(Text(L"Cannot create the data folder in the XTray directory", L"无法创建 XTray 目录中的 data 文件夹"));
        return false;
    }
    const auto destination = directory / L"settings.ini";
    std::filesystem::path temporary;
    Handle file;
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        temporary = directory / (L"settings.ini.tmp." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt));
        file.Reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file) {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            error = WindowsError(Text(L"Cannot write temporary settings", L"无法写入临时设置"));
            return false;
        }
    }
    if (!file) {
        error = Text(L"Cannot create a temporary settings file.", L"无法创建临时设置文件。");
        return false;
    }
    const auto fail = [&file, &temporary, &error](const std::wstring& operation) {
        error = WindowsError(operation);
        file.Reset();
        DeleteFileW(temporary.c_str());
        return false;
    };
    size_t position = 0;
    while (position < bytes.size()) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>((std::min)(bytes.size() - position, size_t{65536}));
        if (!WriteFile(file.Get(), bytes.data() + position, count, &written, nullptr) || written == 0) {
            return fail(Text(L"Cannot write settings", L"写入设置失败"));
        }
        position += written;
    }
    if (!FlushFileBuffers(file.Get())) {
        return fail(Text(L"Cannot flush settings", L"刷新设置失败"));
    }
    file.Reset();
    // Both paths are in data/: a same-volume rename commits the complete file.
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(Text(L"Cannot commit settings; the active settings were not changed", L"提交设置失败，未更新当前设置"));
    }
    return true;
}

} // namespace xtray
