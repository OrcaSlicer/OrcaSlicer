#include "SourceDateEpoch.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <system_error>

#include <boost/nowide/cstdlib.hpp>

namespace Slic3r {

std::optional<std::time_t> source_date_epoch()
{
    // The last second every platform's gmtime() accepts: Windows stops at the end of the year 3000.
    // A later time would make gmtime() return null.
    constexpr std::int64_t last_second = 32535215999;

    // Boost.Nowide's getenv, not std::getenv: on Windows it reads the process environment, so it also
    // sees a value set after start-up, which the C runtime's copy does not.
    const char *text = boost::nowide::getenv("ORCA_SOURCE_DATE_EPOCH");
    if (text == nullptr)
        return std::nullopt;
    const char  *end     = text + std::strlen(text);
    std::int64_t seconds = 0;
    const auto [parsed_to, error] = std::from_chars(text, end, seconds);
    if (error != std::errc() || parsed_to == text || parsed_to != end || seconds < 0 || seconds > last_second)
        return std::nullopt;
    return static_cast<std::time_t>(seconds);
}

} // namespace Slic3r
