#include "xray_config.h"
#include "win32.h"

#include <algorithm>
#include <array>

namespace xtray {
namespace {

bool IsConfigurationName(std::wstring_view name)
{
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) { return false; }
    const auto extension = name.substr(dot);
    constexpr std::array extensions{L".json", L".jsonc", L".toml", L".yaml", L".yml"};
    return std::any_of(extensions.begin(), extensions.end(),
        [extension](const wchar_t* value) { return extension == value; });
}

[[noreturn]] void DirectoryError(const std::wstring& directory, DWORD error)
{
    throw std::runtime_error(ToUtf8(WindowsError(
        Text(L"Cannot read configuration directory: ", L"无法读取配置目录：") + directory, error)));
}

std::wstring DirectoryArgument(std::wstring_view directory, std::wstring_view filename)
{
    std::wstring joined(directory);
    if (!filename.empty()) {
        if (!joined.empty()) { joined.push_back(L'/'); }
        joined.append(filename);
    }
    if (joined.empty()) { return {}; }
    // Match Xray's Go path.Join: only '/' separates segments; backslashes are literal.
    const bool rooted = joined.front() == L'/';
    std::vector<std::wstring_view> parts;
    const std::wstring_view path(joined);
    for (size_t begin = 0; begin < path.size();) {
        const auto slash = path.find(L'/', begin);
        const auto end = slash == std::wstring_view::npos ? path.size() : slash;
        const auto part = path.substr(begin, end - begin);
        if (part == L"..") {
            if (!parts.empty() && parts.back() != L"..") {
                parts.pop_back();
            } else if (!rooted) {
                parts.push_back(part);
            }
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        begin = end + 1;
    }
    std::wstring result = rooted ? L"/" : L"";
    for (const auto part : parts) {
        if (!result.empty() && result.back() != L'/') { result.push_back(L'/'); }
        result.append(part);
    }
    return result.empty() ? L"." : result;
}

std::vector<std::wstring> DirectoryFiles(
    const std::filesystem::path& root, const std::wstring& directory)
{
    const auto resolved = ResolvePath(root, directory);
    const DWORD attributes = GetFileAttributesW(resolved.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { DirectoryError(directory, GetLastError()); }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) { DirectoryError(directory, ERROR_DIRECTORY); }

    WIN32_FIND_DATAW data{};
    const HANDLE handle = FindFirstFileW((resolved / L"*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { return {}; }
        DirectoryError(directory, error);
    }
    struct Search {
        HANDLE handle;
        ~Search() { FindClose(handle); }
    } search{handle};
    struct Entry { std::wstring name; std::string order; };
    std::vector<Entry> entries;
    do {
        if (IsConfigurationName(data.cFileName)) {
            entries.push_back({data.cFileName, ToUtf8(data.cFileName)});
        }
    } while (FindNextFileW(handle, &data));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) { DirectoryError(directory, error); }

    // Go's os.ReadDir sorts names by UTF-8 bytes, without locale or natural ordering.
    std::sort(entries.begin(), entries.end(),
        [](const Entry& left, const Entry& right) { return left.order < right.order; });
    std::vector<std::wstring> files;
    files.reserve(entries.size());
    for (const auto& entry : entries) {
        files.push_back(DirectoryArgument(directory, entry.name));
    }
    return files;
}

bool IsConfdirVariable(std::wstring_view entry)
{
    const auto name = entry.substr(0, entry.find(L'='));
    constexpr std::array<std::wstring_view, 2> names{L"xray.location.confdir", L"XRAY_LOCATION_CONFDIR"};
    return std::any_of(names.begin(), names.end(), [name](std::wstring_view expected) {
        return name.size() == expected.size() && CompareStringOrdinal(name.data(),
            static_cast<int>(name.size()), expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
    });
}

std::vector<wchar_t> ChildEnvironment()
{
    struct Environment {
        LPWCH block;
        ~Environment() { if (block != nullptr) { FreeEnvironmentStringsW(block); } }
    } environment{GetEnvironmentStringsW()};
    if (environment.block == nullptr) {
        const DWORD error = GetLastError();
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot read the environment for Xray", L"无法读取 Xray 进程环境"), error)));
    }
    std::vector<wchar_t> result;
    for (const wchar_t* next = environment.block; *next != L'\0';) {
        const std::wstring_view entry(next);
        if (!IsConfdirVariable(entry)) {
            result.insert(result.end(), entry.begin(), entry.end());
            result.push_back(L'\0');
        }
        next += entry.size() + 1;
    }
    result.push_back(L'\0');
    if (result.size() == 1) { result.push_back(L'\0'); }
    return result;
}

} // namespace

XrayConfigArguments PrepareXrayConfigArguments(
    const std::filesystem::path& root, const Settings& settings)
{
    XrayConfigArguments result;
    const bool expand = settings.configDirectoryFirst &&
        !settings.configDirectory.empty() && !settings.configFiles.empty();
    if (expand) {
        for (const auto& file : DirectoryFiles(root, settings.configDirectory)) {
            result.arguments += L" -config " + QuoteArgument(file);
        }
        result.environment = ChildEnvironment();
    } else if (!settings.configDirectory.empty()) {
        result.arguments += L" -confdir " + QuoteArgument(settings.configDirectory);
    }
    for (const auto& file : settings.configFiles) {
        result.arguments += L" -config " + QuoteArgument(file);
    }
    return result;
}

} // namespace xtray
