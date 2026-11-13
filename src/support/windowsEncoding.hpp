#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <stdexcept>

// UTF-16 <-> UTF-8 encoding helpers with error handling

namespace manatee {
inline std::string toUtf8(std::wstring_view text) {
    if (text.empty())
        return {};

    auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size)
        throw std::runtime_error("Failed to convert UTF-16 to UTF-8.");

    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("Failed to convert UTF-16 to UTF-8.");
    return result;
}

inline std::wstring fromUtf8(std::string_view text) {
    if (text.empty())
        return {};

    auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size)
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");

    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size))
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
    return result;
}
} // namespace manatee