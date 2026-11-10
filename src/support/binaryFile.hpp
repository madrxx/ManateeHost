#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace manatee {

using Bytes = std::vector<std::uint8_t>;

// reads 4 LE bytes and combines them into a uint32
inline std::uint32_t u32(const std::uint8_t *bytes) {
    return std::uint32_t(bytes[0]) |
           (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) |
           (std::uint32_t(bytes[3]) << 24);
}

// checks first 4 bytes against tag
inline bool signature(std::span<const std::uint8_t> bytes, const char *tag) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), tag, 4) == 0;
}

inline Bytes readFile(const std::filesystem::path &path) {
    constexpr auto maxFileSize = 16 * 1024 * 1024;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open file.");

    auto size = file.tellg();
    if (size <= 0 || size > maxFileSize)
        throw std::runtime_error("File is empty or exceeds 16 MiB.");

    Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);

    if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
        throw std::runtime_error("Cannot read file.");

    return bytes;
}
} // namespace manatee