#include "instance.h"

#include <bcrypt.h>

#include <array>

namespace xtray {
namespace {

std::wstring DirectoryKey(const std::filesystem::path& directory)
{
    Handle handle(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot identify the XTray directory", L"无法识别 XTray 目录"))));
    }
    std::wstring finalPath(512, L'\0');
    for (;;) {
        const DWORD size = GetFinalPathNameByHandleW(handle.Get(), finalPath.data(),
            static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (size == 0) {
            throw std::runtime_error(ToUtf8(WindowsError(
                Text(L"Cannot resolve the XTray directory", L"无法解析 XTray 目录"))));
        }
        if (size < finalPath.size()) {
            finalPath.resize(size);
            break;
        }
        finalPath.resize(static_cast<size_t>(size) + 1);
    }
    while (!finalPath.empty() && finalPath.back() == L'\\') {
        finalPath.pop_back();
    }
    const int length = static_cast<int>(finalPath.size());
    const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        finalPath.data(), length, nullptr, 0, nullptr, nullptr, 0);
    if (mapped == 0) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot normalize the XTray directory", L"无法规范化 XTray 目录"))));
    }
    std::wstring identity(static_cast<size_t>(mapped), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, finalPath.data(), length,
        identity.data(), mapped, nullptr, nullptr, 0) != mapped) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot normalize the XTray directory", L"无法规范化 XTray 目录"))));
    }
    std::array<UCHAR, 32> hash{};
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(identity.data()),
        static_cast<ULONG>(identity.size() * sizeof(wchar_t)),
        hash.data(), static_cast<ULONG>(hash.size()));
    if (status < 0) {
        throw std::runtime_error(Text("Cannot hash the XTray directory identity.",
            "无法计算 XTray 目录标识。"));
    }
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring key;
    key.reserve(hash.size() * 2);
    for (const auto byte : hash) {
        key.push_back(digits[byte >> 4]);
        key.push_back(digits[byte & 15]);
    }
    return key;
}

std::vector<BYTE> UserSid(HANDLE process)
{
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) {
        return {};
    }
    token.Reset(raw);
    DWORD size = 0;
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return {};
    }
    std::vector<BYTE> user(size);
    if (!GetTokenInformation(token.Get(), TokenUser, user.data(), size, &size)) {
        return {};
    }
    return user;
}

bool IsCurrentUserAndSession(DWORD pid)
{
    DWORD session = 0, currentSession = 0;
    if (!ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) || session != currentSession) {
        return false;
    }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return false;
    }
    const auto peer = UserSid(process.Get());
    const auto current = UserSid(GetCurrentProcess());
    return !peer.empty() && !current.empty() &&
        EqualSid(reinterpret_cast<const TOKEN_USER*>(peer.data())->User.Sid,
            reinterpret_cast<const TOKEN_USER*>(current.data())->User.Sid) != FALSE;
}

} // namespace

DirectoryInstance::DirectoryInstance(const std::filesystem::path& directory)
{
    const auto key = DirectoryKey(directory);
    windowClass_ = L"XTray.MainWindow." + key;
    const auto name = L"Global\\XTray.Directory." + key;
    SetLastError(ERROR_SUCCESS);
    mutex_.Reset(CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE));
    const DWORD error = GetLastError();
    if (!mutex_) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot access this directory's instance lock; another user may be using XTray",
                L"无法访问此目录的实例锁；其他用户可能正在使用 XTray"), error)));
    }
    // Keeping the handle open reserves the directory across Windows sessions.
    primary_ = error != ERROR_ALREADY_EXISTS;
}

bool DirectoryInstance::ActivateExisting(DWORD timeout) const
{
    if (primary_) {
        return false;
    }
    const auto deadline = GetTickCount64() + timeout;
    do {
        const HWND window = FindWindowW(windowClass_.c_str(), nullptr);
        if (window != nullptr) {
            DWORD pid = 0;
            if (GetWindowThreadProcessId(window, &pid) == 0 || !IsCurrentUserAndSession(pid)) {
                return false;
            }
            AllowSetForegroundWindow(pid);
            DWORD_PTR response = 0;
            return SendMessageTimeoutW(window, kShowInstanceMessage, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &response) != 0 &&
                response == static_cast<DWORD_PTR>(kInstanceShown);
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(20);
    } while (true);
    return false;
}

} // namespace xtray
