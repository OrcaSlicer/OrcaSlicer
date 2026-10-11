#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Slic3r { namespace Bench {

// The text without the spaces and tabs around it.
inline std::string trimmed(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string() : std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}

}} // namespace Slic3r::Bench
