#pragma once

#include "language.h"

#include <windows.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xtray {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() { return std::exchange(value_, nullptr); }
    void Reset(HANDLE value = nullptr)
    {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

inline std::wstring WindowsError(const std::wstring& operation, DWORD error = GetLastError())
{
    wchar_t* message = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = operation + Text(L" (error ", L"（错误 ") + std::to_wstring(error) + Text(L")", L"）");
    if (size != 0 && message != nullptr) {
        std::wstring detail(message, size);
        LocalFree(message);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) {
            detail.pop_back();
        }
        result += Text(L": ", L"：") + detail;
    }
    return result;
}

inline std::filesystem::path ExecutablePath()
{
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            throw std::runtime_error(Text("GetModuleFileNameW failed.", "GetModuleFileNameW 调用失败。"));
        }
        if (size < buffer.size()) {
            buffer.resize(size);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

inline std::filesystem::path ResolvePath(
    const std::filesystem::path& root, const std::wstring& value)
{
    std::filesystem::path path(value);
    return (path.is_absolute() ? path : root / path).lexically_normal();
}

// Quote each argument using the Windows C/C++ command-line parsing rules.
inline std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else {
            result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
            slashes = 0;
            if (character == L'"') {
                result.push_back(L'\\');
            }
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

inline std::string ToUtf8(std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error(Text("Text is too large.", "文本过长。"));
    }
    const int length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error(Text("Invalid UTF-16 text.", "无效的 UTF-16 文本。"));
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        result.data(), size, nullptr, nullptr) == 0) {
        throw std::runtime_error(Text("UTF-8 conversion failed.", "UTF-8 转换失败。"));
    }
    return result;
}

inline std::wstring FromUtf8(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error(Text("Text is too large.", "文本过长。"));
    }
    const int length = static_cast<int>(value.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0);
    if (size == 0) {
        throw std::runtime_error(Text("Invalid UTF-8 text.", "无效的 UTF-8 文本。"));
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
        result.data(), size) == 0) {
        throw std::runtime_error(Text("UTF-16 conversion failed.", "UTF-16 转换失败。"));
    }
    return result;
}

} // namespace xtray
