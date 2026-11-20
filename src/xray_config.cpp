#include "xray_config.h"
#include "win32.h"

#include <bcrypt.h>

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

bool IsMissing(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_DRIVE;
}

bool PathExists(const std::filesystem::path& path, bool directory)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    }
    const DWORD error = GetLastError();
    if (IsMissing(error)) { return false; }
    throw std::runtime_error(ToUtf8(WindowsError(Text(L"Cannot inspect configuration path", L"无法检查配置路径"), error)));
}

std::optional<std::wstring> EnvironmentValue(const wchar_t* name)
{
    DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    while (required != 0) {
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied < required) {
            value.resize(copied);
            return value;
        }
        required = copied;
    }
    return std::nullopt;
}

std::wstring XrayEnvironmentValue(const wchar_t* name, const wchar_t* alias, std::wstring fallback = {})
{
    if (auto value = EnvironmentValue(name)) { return std::move(*value); }
    return EnvironmentValue(alias).value_or(std::move(fallback));
}

std::vector<std::wstring> EffectiveConfigFiles(const std::filesystem::path& root, const Settings& settings,
    const XrayConfigArguments* prepared)
{
    const auto existingDirectory = [&](const std::wstring& path) {
        return !path.empty() && PathExists(ResolvePath(root, path), true);
    };
    const bool expanded = settings.LoadsConfigDirectoryFirst();
    std::vector<std::wstring> directoryFiles;
    if (expanded && prepared != nullptr) {
        directoryFiles = prepared->expandedDirectoryFiles;
    } else if (existingDirectory(settings.configDirectory)) {
        directoryFiles = DirectoryFiles(root, settings.configDirectory);
    } else if (!expanded) {
        const auto inherited = XrayEnvironmentValue(L"xray.location.confdir", L"XRAY_LOCATION_CONFDIR");
        if (existingDirectory(inherited)) { directoryFiles = DirectoryFiles(root, inherited); }
    }

    auto enabled = settings.EnabledConfigFiles();
    directoryFiles.insert(expanded ? directoryFiles.end() : directoryFiles.begin(), enabled.begin(), enabled.end());
    if (!directoryFiles.empty()) { return directoryFiles; }

    for (const auto* name : {L"config.json", L"config.jsonc", L"config.toml", L"config.yaml", L"config.yml"}) {
        const auto path = root / name;
        if (PathExists(path, false)) { return {path.native()}; }
    }
    const auto configLocation = XrayEnvironmentValue(L"xray.location.config", L"XRAY_LOCATION_CONFIG",
        ResolvePath(root, settings.executable).parent_path().native());
    const auto fallback = ResolvePath(root, configLocation) / L"config.json";
    if (PathExists(fallback, false)) { return {fallback.native()}; }
    return {};
}

bool ReadConfigHash(XrayConfigFileFingerprint& file, std::stop_token stop)
{
    if (!PathExists(file.path, false)) { return true; }
    Handle input(CreateFileW(file.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!input) { return false; }

    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (GetFileType(input.Get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(input.Get(), &before)) {
        return false;
    }
    struct Hash {
        BCRYPT_HASH_HANDLE handle = nullptr;
        ~Hash() { if (handle != nullptr) { BCryptDestroyHash(handle); } }
    } hash;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0) {
        return false;
    }
    std::array<unsigned char, 65536> buffer;
    while (!stop.stop_requested()) {
        DWORD count = 0;
        if (!ReadFile(input.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            return false;
        }
        if (count == 0) {
            if (!GetFileInformationByHandle(input.Get(), &after) ||
                CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0 ||
                before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow) {
                return false;
            }
            auto& result = file.hash.emplace();
            return BCryptFinishHash(hash.handle, result.data(), static_cast<ULONG>(result.size()), 0) >= 0;
        }
        if (BCryptHashData(hash.handle, buffer.data(), count, 0) < 0) { return false; }
    }
    return false;
}

} // namespace

std::optional<XrayConfigFingerprint> CaptureXrayConfigFiles(
    const std::filesystem::path& root, const Settings& settings, std::stop_token stop,
    const XrayConfigArguments* prepared)
{
    try {
        if (stop.stop_requested()) { return std::nullopt; }
        XrayConfigFingerprint fingerprint;
        for (const auto& value : EffectiveConfigFiles(root, settings, prepared)) {
            if (stop.stop_requested()) { return std::nullopt; }
            if (value == L"stdin:" || value.starts_with(L"http://") || value.starts_with(L"https://") ||
                value.starts_with(L"http+unix://")) {
                continue;
            }
            auto& file = fingerprint.emplace_back(XrayConfigFileFingerprint{ResolvePath(root, value), std::nullopt});
            if (!ReadConfigHash(file, stop)) { return std::nullopt; }
        }
        return fingerprint;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool XrayConfigPathUsesTail(std::wstring_view path, bool directory)
{
    // Xray matches "tail" in the entire input name, after joining directory entries.
    auto name = directory ? DirectoryArgument(path, L"config.json") : std::wstring(path);
    for (wchar_t& letter : name) {
        if (letter >= L'A' && letter <= L'Z') { letter += L'a' - L'A'; }
        if (letter == L'\u0130') { letter = L'i'; }
    }
    return name.find(L"tail") != std::wstring::npos;
}

XrayConfigArguments PrepareXrayConfigArguments(
    const std::filesystem::path& root, const Settings& settings)
{
    XrayConfigArguments result;
    if (settings.LoadsConfigDirectoryFirst()) {
        result.expandedDirectoryFiles = DirectoryFiles(root, settings.configDirectory);
        for (const auto& file : result.expandedDirectoryFiles) {
            result.arguments += L" -config " + QuoteArgument(file);
        }
        result.environment = ChildEnvironment();
    } else if (!settings.configDirectory.empty()) {
        result.arguments += L" -confdir " + QuoteArgument(settings.configDirectory);
    }
    for (const auto& file : settings.EnabledConfigFiles()) {
        result.arguments += L" -config " + QuoteArgument(file);
    }
    return result;
}

} // namespace xtray
