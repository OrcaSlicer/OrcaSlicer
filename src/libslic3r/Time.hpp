#ifndef slic3r_Utils_Time_hpp_
#define slic3r_Utils_Time_hpp_

#include <string>
#include <ctime>

namespace Slic3r {
namespace Utils {

// Should be thread safe.
time_t get_current_time_utc();

enum class TimeZone { local, utc };
enum class TimeFormat { gcode, iso8601Z };

// time_t to string functions...

std::string time2str(const time_t &t, TimeZone zone, TimeFormat fmt);

inline std::string time2str(TimeZone zone, TimeFormat fmt)
{
    return time2str(get_current_time_utc(), zone, fmt);
}

inline std::string utc_timestamp(time_t t)
{
    return time2str(t, TimeZone::utc, TimeFormat::gcode);
}

inline std::string utc_timestamp()
{
    return utc_timestamp(get_current_time_utc());
}

inline std::string local_timestamp(TimeFormat fmt = TimeFormat::gcode) {
     return time2str(get_current_time_utc(), TimeZone::local, fmt);
}

// String to time_t function. Returns time_t(-1) if fails to parse the input.
time_t str2time(const std::string &str, TimeZone zone, TimeFormat fmt);


// /////////////////////////////////////////////////////////////////////////////
// Utilities to convert an UTC time_t to/from an ISO8601 time format,
// useful for putting timestamps into file and directory names.
// Returns (time_t)-1 on error.

// Use these functions to convert safely to and from the ISO8601 format on
// all platforms

inline std::string iso_utc_timestamp(time_t t)
{
    return time2str(t, TimeZone::utc, TimeFormat::iso8601Z);
}

inline std::string iso_utc_timestamp()
{
    return iso_utc_timestamp(get_current_time_utc());
}

inline time_t parse_iso_utc_timestamp(const std::string &str)
{
    return str2time(str, TimeZone::utc, TimeFormat::iso8601Z);
}

// /////////////////////////////////////////////////////////////////////////////
// Millisecond timestamps for cloud sync protocol
// Format: "2025-11-28T14:30:00.123Z" (ISO 8601 with milliseconds)

// Lossless conversion: Unix milliseconds <-> ISO 8601
// Format: "YYYY-MM-DDTHH:MM:SS.sssZ" (always 3 decimal places for milliseconds)
std::string millis_to_iso8601(long long unix_millis);
long long iso8601_to_millis(const std::string& iso_time);  // Returns -1 on parse error

// /////////////////////////////////////////////////////////////////////////////

} // namespace Utils
} // namespace Slic3r

#endif /* slic3r_Utils_Time_hpp_ */
