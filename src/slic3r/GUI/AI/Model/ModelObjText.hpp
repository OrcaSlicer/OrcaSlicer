#pragma once

#include <fast_float/fast_float.h>
#include <cctype>
#include <cmath>
#include <string_view>

namespace Slic3r::AI::ObjText {
// Token views keep OBJ spelling intact and avoid allocating a locale/stream for
// each coordinate and RGB channel in multi-million-line generated models.
inline std::string_view next(std::string_view line, size_t& at)
{
    while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) ++at;
    const size_t begin = at;
    while (at < line.size() && !std::isspace(static_cast<unsigned char>(line[at])) && line[at] != '#') ++at;
    return line.substr(begin, at - begin);
}

inline bool number(std::string_view token, double& value)
{
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
        if (!token.empty() && token.front() == '-') return false;
    }
    if (token.empty() || token.front() == '+') return false;
    const auto parsed = fast_float::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc() && parsed.ptr == token.data() + token.size() && std::isfinite(value);
}
} // namespace Slic3r::AI::ObjText
