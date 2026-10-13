#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {

// Orca: checked readers for JSON a printer sends. A printer's reply is EXTERNAL INPUT -- its
// shape varies by firmware, by hardware revision, and by what is physically loaded -- so every
// field must be type-checked before it is converted. nlohmann's value(key, fallback) does NOT
// do that: it falls back only when the key is ABSENT, and converts (throwing type_error) when
// the key is present with another type. A U1 lane holding an NFC-tagged spool reports CARD_UID
// as an array where an untagged lane reports the number 0; reading it with value() took the
// whole slicer down the moment a tagged spool was present. Any agent reading device JSON should
// come through these instead of value().

// `key` read as an integer, or `fallback` if it is missing or not a number.
inline int json_number_or(const nlohmann::json& object, const char* key, int fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<int>() : fallback;
}

// `key` read as a string, or `fallback` if it is missing or not a string.
inline std::string json_string_or(const nlohmann::json& object, const char* key, const std::string& fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}

} // namespace Slic3r
