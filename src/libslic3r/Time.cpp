#include "Time.hpp"

#include <iomanip>
#include <sstream>
#include <chrono>
#include <cassert>
#include <ctime>
#include <cstdio>

#ifdef _MSC_VER
#include <map>
#endif

// #include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace Utils {

// "YYYY-MM-DD at HH:MM::SS [UTC]"
// If TimeZone::utc is used with the conversion functions, it will append the
// UTC letters to the end.
static const constexpr char *const SLICER_UTC_TIME_FMT = "%Y-%m-%d at %T";

// ISO8601Z representation of time, without time zone info
static const constexpr char *const ISO8601Z_TIME_FMT = "%Y%m%dT%H%M%SZ";

static const char * get_fmtstr(TimeFormat fmt)
{
    switch (fmt) {
    case TimeFormat::gcode: return SLICER_UTC_TIME_FMT;
    case TimeFormat::iso8601Z: return ISO8601Z_TIME_FMT;
    }

    return "";
}

namespace __get_put_time_emulation {
// FIXME: Implementations with the cpp11 put_time and get_time either not
// compile or do not pass the tests on the build server. If we switch to newer
// compilers, this namespace can be deleted with all its content.

#ifdef _MSC_VER
// VS2019 implementation fails with ISO8601Z_TIME_FMT.
// VS2019 does not have std::strptime either. See bug:
// https://developercommunity.visualstudio.com/content/problem/140618/c-stdget-time-not-parsing-correctly.html

static const std::map<std::string, std::string> sscanf_fmt_map = {
    {SLICER_UTC_TIME_FMT, "%04d-%02d-%02d at %02d:%02d:%02d"},
    {std::string(SLICER_UTC_TIME_FMT) + " UTC", "%04d-%02d-%02d at %02d:%02d:%02d UTC"},
    {ISO8601Z_TIME_FMT, "%04d%02d%02dT%02d%02d%02dZ"}
};

static const char * strptime(const char *str, const char *const fmt, std::tm *tms)
{
    auto it = sscanf_fmt_map.find(fmt);
    if (it == sscanf_fmt_map.end()) return nullptr;

    int y, M, d, h, m, s;
    if (sscanf(str, it->second.c_str(), &y, &M, &d, &h, &m, &s) != 6)
        return nullptr;

    tms->tm_year = y - 1900;  // Year since 1900
    tms->tm_mon  = M - 1;     // 0-11
    tms->tm_mday = d;         // 1-31
    tms->tm_hour = h;         // 0-23
    tms->tm_min  = m;         // 0-59
    tms->tm_sec  = s;         // 0-61 (0-60 in C++11)

    return str; // WARN strptime return val should point after the parsed string
}
#endif

template<class Ttm>
struct GetPutTimeReturnT {
    Ttm *tms;
    const char *fmt;
    GetPutTimeReturnT(Ttm *_tms, const char *_fmt): tms(_tms), fmt(_fmt) {}
};

using GetTimeReturnT = GetPutTimeReturnT<std::tm>;
using PutTimeReturnT = GetPutTimeReturnT<const std::tm>;

std::ostream &operator<<(std::ostream &stream, PutTimeReturnT &&pt)
{
    static const constexpr int MAX_CHARS = 200;
    char _out[MAX_CHARS];
    strftime(_out, MAX_CHARS, pt.fmt, pt.tms);
    stream << _out;
    return stream;
}

inline PutTimeReturnT put_time(const std::tm *tms, const char *fmt)
{
    return {tms, fmt};
}

std::istream &operator>>(std::istream &stream, GetTimeReturnT &&gt)
{
    std::string line;
    std::getline(stream, line);

    if (strptime(line.c_str(), gt.fmt, gt.tms) == nullptr)
        stream.setstate(std::ios::failbit);

    return stream;
}

inline GetTimeReturnT get_time(std::tm *tms, const char *fmt)
{
    return {tms, fmt};
}

}

namespace {

// Platform independent versions of gmtime and localtime. Completely thread
// safe only on Linux. MSVC gtime_s and localtime_s sets global errno thus not
// thread safe.
struct std::tm * _gmtime_r(const time_t *timep, struct tm *result)
{
    assert(timep != nullptr && result != nullptr);
#ifdef WIN32
    time_t t = *timep;
    gmtime_s(result, &t);
    return result;
#else
    return gmtime_r(timep, result);
#endif
}

struct std::tm * _localtime_r(const time_t *timep, struct tm *result)
{
    assert(timep != nullptr && result != nullptr);
#ifdef WIN32
    // Converts a time_t time value to a tm structure, and corrects for the
    // local time zone.
    time_t t = *timep;
    localtime_s(result, &t);
    return result;
#else
    return localtime_r(timep, result);
#endif
}

time_t _mktime(const struct std::tm *tms)
{
    assert(tms != nullptr);
    std::tm _tms = *tms;
    return mktime(&_tms);
}

time_t _timegm(const struct std::tm *tms)
{
    std::tm _tms = *tms;
#ifdef WIN32
    return _mkgmtime(&_tms);
#else /* WIN32 */
    return timegm(&_tms);
#endif /* WIN32 */
}

std::string process_format(const char *fmt, TimeZone zone)
{
    std::string fmtstr(fmt);

    if (fmtstr == SLICER_UTC_TIME_FMT && zone == TimeZone::utc)
        fmtstr += " UTC";

    return fmtstr;
}

} // namespace

time_t get_current_time_utc()
{
    using clk = std::chrono::system_clock;
    return clk::to_time_t(clk::now());
}

static std::string tm2str(const std::tm *tms, const char *fmt)
{
    std::stringstream ss;
    ss.imbue(std::locale("C"));
    ss << __get_put_time_emulation::put_time(tms, fmt);
    return ss.str();
}

std::string time2str(const time_t &t, TimeZone zone, TimeFormat fmt)
{
    std::string ret;
    std::tm tms = {};
    tms.tm_isdst = -1;
    std::string fmtstr = process_format(get_fmtstr(fmt), zone);

    switch (zone) {
    case TimeZone::local:
        ret = tm2str(_localtime_r(&t, &tms), fmtstr.c_str()); break;
    case TimeZone::utc:
        ret = tm2str(_gmtime_r(&t, &tms), fmtstr.c_str()); break;
    }

    return ret;
}

static time_t str2time(std::istream &stream, TimeZone zone, const char *fmt)
{
    std::tm tms = {};
    tms.tm_isdst = -1;

    stream >> __get_put_time_emulation::get_time(&tms, fmt);
    time_t ret = time_t(-1);

    switch (zone) {
    case TimeZone::local: ret = _mktime(&tms); break;
    case TimeZone::utc:   ret = _timegm(&tms); break;
    }

    if (stream.fail() || ret < time_t(0)) ret = time_t(-1);

    return ret;
}

time_t str2time(const std::string &str, TimeZone zone, TimeFormat fmt)
{
    std::string fmtstr = process_format(get_fmtstr(fmt), zone).c_str();
    std::stringstream ss(str);

    ss.imbue(std::locale("C"));
    return str2time(ss, zone, fmtstr.c_str());
}

}; // namespace Utils
}; // namespace Slic3r
