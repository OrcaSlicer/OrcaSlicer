#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {

// Orca: type-checked readers for printer JSON, whose field types vary by firmware and loaded
// hardware. Use these instead of json::value(), which throws when a present key has another type.
inline int json_number_or(const nlohmann::json& object, const char* key, int fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<int>() : fallback;
}

inline std::string json_string_or(const nlohmann::json& object, const char* key, const std::string& fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

} // namespace Slic3r
