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
            throw std::runtime_error("Cannot read settings.ini.");
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
        throw std::runtime_error("Invalid numeric setting.");
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
            result.error = WindowsError(L"读取设置失败", code);
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
            throw std::runtime_error("NUL is not valid in settings.ini.");
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
                        throw std::runtime_error("Unknown settings section.");
                    }
                } else {
                    const auto equals = line.find(L'=');
                    if (section.empty() || equals == std::wstring::npos || equals == 0) {
                        throw std::runtime_error("Invalid settings.ini line.");
                    }
                    auto key = std::make_pair(section, line.substr(0, equals));
                    if (!values.emplace(std::move(key), line.substr(equals + 1)).second) {
                        throw std::runtime_error("Duplicate settings key.");
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
                throw std::runtime_error("Missing settings key.");
            }
            auto value = found->second;
            values.erase(found);
            return value;
        };
        if (take(L"XTray", L"Version") != L"1") {
            throw std::runtime_error("Unsupported settings version.");
        }
        auto& settings = result.settings;
        settings.executable = take(L"XTray", L"Executable");
        settings.configDirectory = take(L"XTray", L"ConfigDirectory");
        settings.logLines = ParseNumber(take(L"XTray", L"LogLines"));
        const size_t count = ParseNumber(take(L"XTray", L"ConfigCount"));
        if (count > values.size()) {
            throw std::runtime_error("Incomplete configuration file list.");
        }
        for (size_t index = 0; index < count; ++index) {
            settings.configFiles.push_back(take(L"ConfigFiles", std::to_wstring(index)));
        }
        if (!values.empty() || IsBlank(settings.executable) ||
            !IsPathValue(settings.executable) || !IsPathValue(settings.configDirectory) ||
            settings.logLines == 0 || settings.logLines > INT_MAX) {
            throw std::runtime_error("Invalid settings values.");
        }
        for (const auto& config : settings.configFiles) {
            if (IsBlank(config) || !IsPathValue(config)) {
                throw std::runtime_error("Invalid configuration file path.");
            }
        }
    } catch (const std::exception& exception) {
        result.success = false;
        result.settings = {};
        result.error = L"无法读取 settings.ini：" + FromUtf8(exception.what()) +
            L" 请在设置中重新填写并保存；原文件未改动。";
    }
    return result;
}

bool SaveSettings(const std::filesystem::path& root, const Settings& settings, std::wstring& error)
{
    if (IsBlank(settings.executable) || !IsPathValue(settings.executable) ||
        !IsPathValue(settings.configDirectory) || settings.logLines == 0 ||
        settings.logLines > INT_MAX) {
        error = L"请填写 Xray 程序路径和有效的日志行数。路径不能包含换行或空字符。";
        return false;
    }
    for (const auto& config : settings.configFiles) {
        if (IsBlank(config) || !IsPathValue(config)) {
            error = L"配置文件路径不能为空或包含换行、空字符。";
            return false;
        }
    }

    std::string bytes;
    try {
        std::wstring text = L"[XTray]\r\nVersion=1\r\nExecutable=" + settings.executable +
            L"\r\nConfigDirectory=" + settings.configDirectory +
            L"\r\nLogLines=" + std::to_wstring(settings.logLines) +
            L"\r\nConfigCount=" + std::to_wstring(settings.configFiles.size()) +
            L"\r\n\r\n[ConfigFiles]\r\n";
        for (size_t index = 0; index < settings.configFiles.size(); ++index) {
            text += std::to_wstring(index) + L"=" + settings.configFiles[index] + L"\r\n";
        }
        bytes = ToUtf8(text);
    } catch (const std::exception&) {
        error = L"设置包含无法编码的 Unicode 字符，未保存。";
        return false;
    }

    const auto directory = root / L"data";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = WindowsError(L"无法创建 XTray 目录中的 data 文件夹");
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
            error = WindowsError(L"无法写入临时设置");
            return false;
        }
    }
    if (!file) {
        error = L"无法创建临时设置文件。";
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
            return fail(L"写入设置失败");
        }
        position += written;
    }
    if (!FlushFileBuffers(file.Get())) {
        return fail(L"刷新设置失败");
    }
    file.Reset();
    // Both paths are in data/: a same-volume rename commits the complete file.
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(L"提交设置失败，未更新当前设置");
    }
    return true;
}

} // namespace xtray
