#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Time.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/date_time/gregorian/gregorian_types.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>

#include <ctime>
#include <sstream>
#include <iomanip>
#include <locale>
#include <string>

static void test_time_fmt(Slic3r::Utils::TimeFormat fmt) {
    using namespace Slic3r::Utils;
    // A fixed mid-year time (2019-07-10 08:50:00 UTC) rather than the current one: local
    // time text can't tell apart the hour that repeats when daylight saving ends, so the
    // local round trip would fail during that hour.
    time_t t = 1562748600;

    std::string tstr = time2str(t, TimeZone::local, fmt);
    time_t parsedtime = str2time(tstr, TimeZone::local, fmt);
    REQUIRE(t == parsedtime);
    
    tstr = time2str(t, TimeZone::utc, fmt);
    parsedtime = str2time(tstr, TimeZone::utc, fmt);
    REQUIRE(t == parsedtime);
    
    parsedtime = str2time("not valid string", TimeZone::local, fmt);
    REQUIRE(parsedtime == time_t(-1));
    
    parsedtime = str2time("not valid string", TimeZone::utc, fmt);
    REQUIRE(parsedtime == time_t(-1));
}

TEST_CASE("ISO8601Z", "[Timeutils]") {
    test_time_fmt(Slic3r::Utils::TimeFormat::iso8601Z);
    
    std::string mydate = "20190710T085000Z";
    time_t t = Slic3r::Utils::parse_iso_utc_timestamp(mydate);
    std::string date = Slic3r::Utils::iso_utc_timestamp(t);
    
    REQUIRE(date == mydate);
}

TEST_CASE("Slic3r_UTC_Time_Format", "[Timeutils]") {
    using namespace Slic3r::Utils;
    test_time_fmt(TimeFormat::gcode);
    
    std::string mydate = "2019-07-10 at 08:50:00 UTC";
    time_t t = Slic3r::Utils::str2time(mydate, TimeZone::utc, TimeFormat::gcode);
    std::string date = Slic3r::Utils::utc_timestamp(t);
    
    REQUIRE(date == mydate);
}

TEST_CASE("format_clock_time", "[Timeutils]") {
    using Slic3r::format_clock_time;

    // 24-hour output is the unchanged default look.
    REQUIRE(format_clock_time(0, 5, false) == "00:05");
    REQUIRE(format_clock_time(13, 30, false) == "13:30");
    REQUIRE(format_clock_time(23, 59, false, 7) == "23:59:07");

    // 12-hour: midnight is 12 AM, noon is 12 PM.
    REQUIRE(format_clock_time(0, 5, true) == "12:05 AM");
    REQUIRE(format_clock_time(9, 0, true) == "09:00 AM");
    REQUIRE(format_clock_time(12, 0, true) == "12:00 PM");
    REQUIRE(format_clock_time(13, 30, true) == "01:30 PM");
    REQUIRE(format_clock_time(23, 59, true, 7) == "11:59:07 PM");
}

TEST_CASE("The print end time follows the clock format and carries the days it is away", "[Timeutils]") {
    using namespace boost::posix_time;
    const ptime now(boost::gregorian::date(2026, 10, 16), hours(10) + minutes(0));

    REQUIRE(Slic3r::get_bbl_monitor_end_time_dhm(0.f).empty());
    REQUIRE(Slic3r::get_bbl_monitor_end_time_dhm(0.f, true, now).empty());

    // 24-hour, with and without a day offset
    CHECK(Slic3r::get_bbl_monitor_end_time_dhm(3600.f, false, now) == "11:00");
    CHECK(Slic3r::get_bbl_monitor_end_time_dhm(15 * 3600.f, false, now) == "01:00 +1");
    CHECK(Slic3r::get_bbl_monitor_end_time_dhm(63 * 3600.f, false, now) == "01:00 +3");

    // 12-hour keeps the same offset after AM/PM
    CHECK(Slic3r::get_bbl_monitor_end_time_dhm(3600.f, true, now) == "11:00 AM");
    CHECK(Slic3r::get_bbl_monitor_end_time_dhm(15 * 3600.f, true, now) == "01:00 AM +1");
}
