#include "ImportNames.hpp"

#include <boost/locale/encoding_errors.hpp>
#include <boost/locale/encoding_utf.hpp>

#include <map>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {
namespace {

std::u32string decode_name(const std::string& name)
{
    try {
        return boost::locale::conv::utf_to_utf<char32_t>(name, boost::locale::conv::stop);
    } catch (const boost::locale::conv::conversion_error&) {
        throw std::invalid_argument("An imported name contains invalid UTF-8.");
    }
}

bool unsupported_character(char32_t c)
{
    // XML permits tab/newline/CR, but excludes other C0 controls and noncharacters FFFE/FFFF.
    const bool valid_xml = c == 9 || c == 10 || c == 13 ||
                           (c >= 0x20 && c <= 0xd7ff) ||
                           (c >= 0xe000 && c <= 0xfffd) ||
                           (c >= 0x10000 && c <= 0x10ffff);
    constexpr std::u32string_view forbidden = U"<>:/\\|?*\"";
    return !valid_xml || forbidden.find(c) != std::u32string_view::npos;
}

std::string replace_characters(const std::string& name, const std::string& replacement)
{
    std::u32string output;
    const std::u32string substitute = decode_name(replacement);
    for (char32_t c : decode_name(name)) {
        if (unsupported_character(c))
            output += substitute;
        else
            output += c;
    }
    return boost::locale::conv::utf_to_utf<char>(output);
}

} // namespace

bool has_unsupported_name_characters(const std::string& name)
{
    for (char32_t c : decode_name(name))
        if (unsupported_character(c))
            return true;
    return false;
}

bool valid_import_name_replacement(const std::string& replacement)
{
    if (replacement.empty())
        return false;
    try {
        return !has_unsupported_name_characters(replacement);
    } catch (const std::invalid_argument&) {
        return false;
    }
}

std::vector<std::string> sanitize_import_names(const std::vector<std::string>& names,
                                             const std::string& replacement,
                                             const std::string& fallback)
{
    if (!replacement.empty() && !valid_import_name_replacement(replacement))
        throw std::invalid_argument("The name replacement contains unsupported characters.");

    // Reserve all existing valid names before processing, including those appearing later.
    std::set<std::string> reserved;
    for (const auto& name : names)
        if (!has_unsupported_name_characters(name))
            reserved.insert(name);

    std::string safe_fallback = replace_characters(fallback, replacement);
    if (safe_fallback.empty())
        safe_fallback = "Part";

    std::map<std::string, std::string> cleaned;
    std::vector<std::string> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        if (!has_unsupported_name_characters(name)) {
            result.push_back(name);
            continue;
        }
        // Equal original names stay equal even when both require the same cleaning.
        const auto previous = cleaned.find(name);
        if (previous != cleaned.end()) {
            result.push_back(previous->second);
            continue;
        }
        std::string base = replace_characters(name, replacement);
        if (base.empty())
            base = safe_fallback;
        std::string candidate = base;
        for (size_t suffix = 2; reserved.count(candidate) != 0; ++suffix)
            candidate = base + " (" + std::to_string(suffix) + ")";
        reserved.insert(candidate);
        cleaned.emplace(name, candidate);
        result.push_back(candidate);
    }
    return result;
}

} // namespace Slic3r
