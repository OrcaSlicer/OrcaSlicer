#pragma once
#include <cstddef>
#include <ios>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Slic3r::Gpu {

inline std::vector<uint32_t> read_shader_words(const std::string& utf8_path)
{
    // Application resource paths are UTF-8, including portable installations
    // below Korean/other non-ACP Windows user folders. A narrow ifstream path
    // would reinterpret those bytes using the active Windows code page.
    std::ifstream stream(std::filesystem::u8path(utf8_path), std::ios::binary | std::ios::ate);
    if (!stream)
        return {};
    const std::streamsize byte_count = stream.tellg();
    constexpr std::streamsize maximum_shader_bytes = 16 * 1024 * 1024;
    if (byte_count < 5 * std::streamsize(sizeof(uint32_t)) || byte_count > maximum_shader_bytes ||
        byte_count % std::streamsize(sizeof(uint32_t)) != 0)
        return {};
    std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(words.data()), byte_count) || words.front() != 0x07230203)
        return {};
    return words;
}

} // namespace Slic3r::Gpu
