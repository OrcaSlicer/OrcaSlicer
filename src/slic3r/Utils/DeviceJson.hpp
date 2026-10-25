#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace Slic3r {

// Orca: type-checked readers for printer JSON, whose field types vary by firmware and loaded
// hardware. Use these instead of json::value(), which throws when a present key has another type.
// Some firmware writes whole numbers as strings, so a string of digits counts as one.
inline bool json_int(const nlohmann::json& object, const char* key, int& out)
{
    const auto it = object.find(key);
    if (it == object.end())
        return false;
    if (it->is_number()) {
        out = it->get<int>();
        return true;
    }
    if (!it->is_string())
        return false;
    const std::string& digits = it->get_ref<const std::string&>();
    if (digits.empty() || digits.size() > 9 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    out = std::stoi(digits);
    return true;
}

inline int json_number_or(const nlohmann::json& object, const char* key, int fallback)
{
    int value = fallback;
    return json_int(object, key, value) ? value : fallback;
}

inline std::string json_string_or(const nlohmann::json& object, const char* key, const std::string& fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

} // namespace Slic3r
