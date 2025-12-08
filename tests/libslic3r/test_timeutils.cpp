#include <catch2/catch_all.hpp>

#include "libslic3r/Time.hpp"

#include <sstream>
#include <iomanip>
#include <locale>

using namespace Slic3r;

static void test_time_fmt(Slic3r::Utils::TimeFormat fmt) {
    using namespace Slic3r::Utils;
    time_t t = get_current_time_utc();
    
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
