#include "startup.h"
#include "win32.h"

#include <objbase.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cwchar>
#include <memory>

namespace xtray {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr size_t kMaximumCommand = 260;

[[noreturn]] void Fail(const std::wstring& message)
{
    throw std::runtime_error(ToUtf8(message));
}

[[noreturn]] void FailWindows(const std::wstring& message, DWORD code = GetLastError())
{
    Fail(WindowsError(message, code));
}

class RegistryKey {
public:
    ~RegistryKey() { if (value != nullptr) { RegCloseKey(value); } }
    HKEY value = nullptr;
};

class CurrentUserRegistry final : public StartupRegistry {
public:
    bool Read(const std::wstring& name, std::optional<std::wstring>& command,
        std::wstring& error) override
    {
        command.reset();
        RegistryKey key;
        auto code = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key.value);
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (code != ERROR_SUCCESS) {
            error = WindowsError(Text(L"Cannot read the login startup key", L"无法读取登录自启注册项"), code);
            return false;
        }
        DWORD type = 0;
        DWORD size = 0;
        code = RegQueryValueExW(key.value, name.c_str(), nullptr, &type, nullptr, &size);
        if (code == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (code != ERROR_SUCCESS) {
            error = WindowsError(Text(L"Cannot read the login startup entry", L"无法读取登录自启记录"), code);
            return false;
        }
        if (type != REG_SZ || size < sizeof(wchar_t) ||
            size > (kMaximumCommand + 1) * sizeof(wchar_t) || size % sizeof(wchar_t) != 0) {
            error = Text(L"The login startup entry is not a valid REG_SZ command; it was not changed.",
                L"登录自启记录不是有效的 REG_SZ 命令，未进行修改。");
            return false;
        }
        std::wstring text(size / sizeof(wchar_t), L'\0');
        const DWORD capacity = size;
        code = RegQueryValueExW(key.value, name.c_str(), nullptr, &type,
            reinterpret_cast<BYTE*>(text.data()), &size);
        if (code != ERROR_SUCCESS) {
            error = WindowsError(Text(L"Cannot read the complete login startup entry", L"无法完整读取登录自启记录"), code);
            return false;
        }
        if (type != REG_SZ || size != capacity || text.back() != L'\0' ||
            text.find(L'\0') != text.size() - 1) {
            error = Text(L"The login startup entry is malformed or changed while reading; it was not changed.",
                L"登录自启记录格式损坏或在读取期间发生变化，未进行修改。");
            return false;
        }
        text.pop_back();
        command = std::move(text);
        return true;
    }

    bool Write(const std::wstring& name, const std::optional<std::wstring>& command,
        std::wstring& error) override
    {
        RegistryKey key;
        LSTATUS code = ERROR_SUCCESS;
        if (command) {
            if (command->size() > kMaximumCommand ||
                command->find(L'\0') != std::wstring::npos) {
                error = Text(L"The login startup command exceeds the Windows Run limit.",
                    L"登录自启命令超过 Windows Run 项长度限制。");
                return false;
            }
            code = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                KEY_SET_VALUE, nullptr, &key.value, nullptr);
        } else {
            code = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key.value);
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return true;
            }
        }
        if (code == ERROR_SUCCESS) {
            code = command
                ? RegSetValueExW(key.value, name.c_str(), 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command->c_str()),
                    static_cast<DWORD>((command->size() + 1) * sizeof(wchar_t)))
                : RegDeleteValueW(key.value, name.c_str());
        }
        if (code != ERROR_SUCCESS && !(code == ERROR_FILE_NOT_FOUND && !command)) {
            error = WindowsError(Text(L"Cannot update the login startup entry", L"无法更新登录自启记录"), code);
            return false;
        }
        return true;
    }
};

StartupRegistry& DefaultRegistry()
{
    static CurrentUserRegistry registry;
    return registry;
}

class StartupLock {
public:
    StartupLock()
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
            FailWindows(Text(L"Cannot identify the startup user", L"无法识别自启所属用户"));
        }
        Handle token(rawToken);
        DWORD size = 0;
        GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
        if (size == 0) {
            FailWindows(Text(L"Cannot measure the startup user identity", L"无法读取自启用户身份长度"));
        }
        std::vector<BYTE> buffer(size);
        if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), size, &size)) {
            FailWindows(Text(L"Cannot read the startup user identity", L"无法读取自启用户身份"));
        }
        wchar_t* sidText = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sidText)) {
            FailWindows(Text(L"Cannot format the startup user identity", L"无法转换自启用户身份"));
        }
        const std::wstring name = std::wstring(L"Global\\XTray.Startup.") + sidText;
        LocalFree(sidText);
        mutex_.Reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!mutex_) {
            FailWindows(Text(L"Cannot lock login startup changes", L"无法锁定登录自启设置"));
        }
        const DWORD wait = WaitForSingleObject(mutex_.Get(), 5000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owned_ = true;
            return;
        }
        if (wait == WAIT_TIMEOUT) {
            Fail(Text(L"Another XTray is changing login startup. Try saving again.",
                L"另一个 XTray 正在修改登录自启，请稍后重新保存。"));
        }
        FailWindows(Text(L"Cannot wait for the login startup lock", L"等待登录自启锁失败"));
    }
    ~StartupLock() { if (owned_) { ReleaseMutex(mutex_.Get()); } }

private:
    Handle mutex_;
    bool owned_ = false;
};

struct Identity {
    std::wstring id;
    std::filesystem::path executable;
};

bool IsPath(const std::wstring& value)
{
    return !value.empty() && value.find_first_of(L"\r\n\"") == std::wstring::npos &&
        value.find(L'\0') == std::wstring::npos && std::filesystem::path(value).is_absolute();
}

bool SameText(std::wstring_view left, std::wstring_view right)
{
    return left.size() == right.size() && left.size() <= INT_MAX &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return SameText(left.lexically_normal().native(), right.lexically_normal().native());
}

std::wstring NewId()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) {
        Fail(Text(L"Cannot create a login startup ID.", L"无法生成登录自启标识。"));
    }
    std::array<wchar_t, 39> text{};
    if (StringFromGUID2(value, text.data(), static_cast<int>(text.size())) != 39) {
        Fail(Text(L"Cannot format a login startup ID.", L"无法转换登录自启标识。"));
    }
    return text.data();
}

std::optional<std::string> ReadBytes(const std::filesystem::path& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return {};
        }
        FailWindows(Text(L"Cannot read startup.ini", L"无法读取 startup.ini"), code);
    }
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            FailWindows(Text(L"Cannot read startup.ini", L"无法读取 startup.ini"));
        }
        if (read == 0) {
            return bytes;
        }
        bytes.append(buffer.data(), read);
        if (bytes.size() > 65536) {
            Fail(Text(L"startup.ini is too large; it was not changed.", L"startup.ini 过大，未进行修改。"));
        }
    }
}

std::optional<Identity> ParseIdentity(const std::optional<std::string>& bytes)
{
    if (!bytes) {
        return {};
    }
    const auto text = FromUtf8(*bytes);
    constexpr std::wstring_view prefix = L"[XTray]\r\nVersion=1\r\nId=";
    constexpr std::wstring_view separator = L"\r\nExecutable=";
    if (!text.starts_with(prefix) || !text.ends_with(L"\r\n")) {
        Fail(Text(L"startup.ini is invalid; its startup entry was not changed.",
            L"startup.ini 格式无效，未修改其自启记录。"));
    }
    const auto split = text.find(separator, prefix.size());
    if (split == std::wstring::npos) {
        Fail(Text(L"startup.ini has no executable identity.", L"startup.ini 缺少程序身份。"));
    }
    Identity result;
    result.id = text.substr(prefix.size(), split - prefix.size());
    result.executable = text.substr(split + separator.size(),
        text.size() - split - separator.size() - 2);
    GUID id{};
    const GUID empty{};
    std::array<wchar_t, 39> canonical{};
    if (result.id.size() != 38 || FAILED(CLSIDFromString(result.id.c_str(), &id)) ||
        IsEqualGUID(id, empty) ||
        StringFromGUID2(id, canonical.data(), static_cast<int>(canonical.size())) != 39 ||
        !SameText(result.id, canonical.data()) || !IsPath(result.executable.native())) {
        Fail(Text(L"startup.ini contains an invalid ID or executable path; it was not changed.",
            L"startup.ini 中的标识或程序路径无效，未进行修改。"));
    }
    result.id = canonical.data();
    return result;
}

std::string IdentityBytes(const Identity& identity)
{
    return ToUtf8(L"[XTray]\r\nVersion=1\r\nId=" + identity.id +
        L"\r\nExecutable=" + identity.executable.native() + L"\r\n");
}

std::wstring EntryName(const Identity& identity)
{
    return L"XTray-" + identity.id;
}

std::wstring CommandFor(const std::filesystem::path& executable)
{
    if (!IsPath(executable.native())) {
        Fail(Text(L"The XTray executable path must be absolute and contain no quotes or line breaks.",
            L"XTray 程序路径必须为绝对路径，且不能包含引号或换行。"));
    }
    return QuoteArgument(executable.native()) + L" " + kLogonArgument;
}

bool CommandMatches(const std::wstring& command, const std::filesystem::path& executable)
{
    const auto expected = CommandFor(executable);
    constexpr std::wstring_view suffix = L" --logon";
    return command.size() == expected.size() && command.ends_with(suffix) &&
        SameText(std::wstring_view(command).substr(0, command.size() - suffix.size()),
            std::wstring_view(expected).substr(0, expected.size() - suffix.size()));
}

void RequireOwnership(const Identity& identity, const std::optional<std::wstring>& command)
{
    if (command && !CommandMatches(*command, identity.executable)) {
        Fail(Text(L"The startup ID belongs to a different command. No startup entry was changed.",
            L"自启标识对应的命令与身份记录不一致，未修改任何自启记录。"));
    }
}

bool OldExecutableExists(const std::filesystem::path& executable)
{
    const DWORD attributes = GetFileAttributesW(executable.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            Fail(Text(L"The old executable path is now a directory; startup ownership is uncertain.",
                L"旧程序路径现在是目录，无法确认自启归属。"));
        }
        return true;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    FailWindows(Text(L"Cannot verify the old XTray executable; its startup entry was not changed",
        L"无法核实旧 XTray 程序，未修改其自启记录"), code);
}

void ReplaceBytes(const std::filesystem::path& destination, const std::optional<std::string>& bytes)
{
    if (!bytes) {
        if (!DeleteFileW(destination.c_str())) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                FailWindows(Text(L"Cannot remove startup.ini", L"无法删除 startup.ini"), code);
            }
        }
        return;
    }
    const auto directory = destination.parent_path();
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        FailWindows(Text(L"Cannot create the startup data folder", L"无法创建自启数据目录"));
    }
    const auto temporary = directory / (L"startup.ini.tmp." + NewId());
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        FailWindows(Text(L"Cannot create a temporary startup identity", L"无法创建临时自启身份文件"));
    }
    try {
        size_t position = 0;
        while (position < bytes->size()) {
            DWORD written = 0;
            const auto count = static_cast<DWORD>((std::min)(bytes->size() - position, size_t{65536}));
            if (!WriteFile(file.Get(), bytes->data() + position, count, &written, nullptr) || written == 0) {
                FailWindows(Text(L"Cannot write the startup identity", L"无法写入自启身份"));
            }
            position += written;
        }
        if (!FlushFileBuffers(file.Get())) {
            FailWindows(Text(L"Cannot flush the startup identity", L"无法刷新自启身份"));
        }
        file.Reset();
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            FailWindows(Text(L"Cannot commit the startup identity", L"无法提交自启身份"));
        }
    } catch (...) {
        file.Reset();
        DeleteFileW(temporary.c_str());
        throw;
    }
}

std::optional<std::wstring> ReadEntry(StartupRegistry& registry, const std::wstring& name)
{
    std::optional<std::wstring> command;
    std::wstring error;
    if (!registry.Read(name, command, error)) {
        Fail(error);
    }
    return command;
}

} // namespace

bool IsLogonCommandLine(std::wstring_view commandLine)
{
    constexpr std::wstring_view whitespace = L" \t\r\n";
    const auto first = commandLine.find_first_not_of(whitespace);
    if (first == std::wstring_view::npos) {
        return false;
    }
    const auto last = commandLine.find_last_not_of(whitespace);
    return commandLine.substr(first, last - first + 1) == kLogonArgument;
}

StartupRegistration::StartupRegistration(std::filesystem::path root,
    std::filesystem::path executable, StartupRegistry* registry)
    : root_(std::move(root)), executable_(executable.lexically_normal()),
      registry_(registry == nullptr ? &DefaultRegistry() : registry)
{
}

StartupStatus StartupRegistration::Query() const
{
    StartupStatus result;
    try {
        const auto identity = ParseIdentity(ReadBytes(root_ / L"data" / L"startup.ini"));
        if (identity) {
            const auto command = ReadEntry(*registry_, EntryName(*identity));
            RequireOwnership(*identity, command);
            result.enabled = command.has_value() && SamePath(identity->executable, executable_) &&
                CommandMatches(*command, executable_);
        }
    } catch (const std::exception& exception) {
        result.success = false;
        result.error = Text(L"Cannot determine login startup: ", L"无法读取登录自启状态：") +
            FromUtf8(exception.what());
    }
    return result;
}

bool StartupRegistration::Save(const Settings& settings, bool enabled, std::wstring& error)
{
    try {
        StartupLock lock;
        const auto identityPath = root_ / L"data" / L"startup.ini";
        const auto originalBytes = ReadBytes(identityPath);
        auto identity = ParseIdentity(originalBytes);
        std::optional<std::wstring> originalCommand;
        if (identity) {
            originalCommand = ReadEntry(*registry_, EntryName(*identity));
            RequireOwnership(*identity, originalCommand);
        }

        std::wstring name;
        std::optional<std::wstring> replacement;
        std::optional<std::wstring> prior;
        auto newBytes = originalBytes;
        if (enabled) {
            const auto command = CommandFor(executable_);
            if (command.size() > kMaximumCommand) {
                Fail(Text(L"The quoted XTray path and --logon exceed the 260-character Windows Run limit.",
                    L"加引号的 XTray 路径及 --logon 超过 Windows Run 项的 260 字符限制。"));
            }
            if (!identity || (!SamePath(identity->executable, executable_) &&
                OldExecutableExists(identity->executable))) {
                identity = Identity{NewId(), executable_};
                bool available = false;
                for (unsigned attempt = 0; attempt < 10; ++attempt) {
                    if (!ReadEntry(*registry_, EntryName(*identity))) {
                        available = true;
                        break;
                    }
                    identity->id = NewId();
                }
                if (!available) {
                    Fail(Text(L"Cannot allocate a unique login startup ID.", L"无法分配独立的登录自启标识。"));
                }
                prior.reset();
            } else {
                prior = originalCommand;
            }
            identity->executable = executable_;
            name = EntryName(*identity);
            replacement = command;
            newBytes = IdentityBytes(*identity);
        } else if (identity && originalCommand && SamePath(identity->executable, executable_)) {
            name = EntryName(*identity);
            prior = originalCommand;
        }

        // All XTray saves for this user hold the same cross-session mutex. Recheck
        // immediately before writing so externally edited entries are not overwritten.
        if (ReadBytes(identityPath) != originalBytes ||
            (!name.empty() && ReadEntry(*registry_, name) != prior)) {
            Fail(Text(L"Login startup changed while saving. Reopen settings and try again.",
                L"保存期间登录自启发生变化，请重新打开设置后重试。"));
        }

        bool registryChanged = false;
        bool identityChanged = false;
        try {
            if (!name.empty() && prior != replacement) {
                if (!registry_->Write(name, replacement, error)) {
                    return false;
                }
                registryChanged = true;
            }
            if (newBytes != originalBytes) {
                ReplaceBytes(identityPath, newBytes);
                identityChanged = true;
            }
            if (!SaveSettings(root_, settings, error)) {
                Fail(error);
            }
            return true;
        } catch (const std::exception& exception) {
            error = FromUtf8(exception.what());
            std::wstring recovery;
            if (identityChanged) {
                try {
                    if (ReadBytes(identityPath) != newBytes) {
                        Fail(Text(L"startup.ini changed outside this save.", L"startup.ini 已在此次保存之外发生变化。"));
                    }
                    ReplaceBytes(identityPath, originalBytes);
                } catch (const std::exception& rollback) {
                    recovery += Text(L" Identity rollback failed: ", L" 身份文件恢复失败：") + FromUtf8(rollback.what());
                }
            }
            if (registryChanged) {
                try {
                    if (ReadEntry(*registry_, name) != replacement) {
                        Fail(Text(L"The startup entry changed outside this save.", L"自启记录已在此次保存之外发生变化。"));
                    }
                    std::wstring rollbackError;
                    if (!registry_->Write(name, prior, rollbackError)) {
                        Fail(rollbackError);
                    }
                } catch (const std::exception& rollback) {
                    recovery += Text(L" Registry rollback failed: ", L" 注册项恢复失败：") + FromUtf8(rollback.what());
                }
            }
            error += recovery;
            return false;
        }
    } catch (const std::exception& exception) {
        error = FromUtf8(exception.what());
        return false;
    }
}

} // namespace xtray
