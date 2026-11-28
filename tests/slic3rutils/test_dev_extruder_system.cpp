// Match the include environment that libslic3r_gui TUs get from pchheader.hpp: Windows.h with
// WIN32_LEAN_AND_MEAN/NOMINMAX must come first so rpcndr.h's `byte` is processed before <cstddef>
// makes std::byte a competing candidate (otherwise the Windows COM headers pulled in via
// DeviceManager.hpp error with an ambiguous `byte`). wx/timer.h must precede DeviceManager.hpp,
// which includes DeviceErrorDialog.hpp (uses wxTimerEvent) before its own wx/timer.h include.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

// An extruder status from a Bambu Lab H3C. Its entry has no filam_bak.
TEST_CASE("An extruder status without filam_bak is read with no filament backup", "[DevExtderSystem][H3C]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    const json extruder = json::parse(R"({
        "state": 1,
        "info": [
            { "hnow": 0, "hpre": 0, "htar": 0, "id": 0, "info": 1547, "snow": 8192, "spre": 8192,
              "star": 8192, "stat": 393984, "temp": 162, "z_bias": 0.0 }
        ]
    })");
    ExtderSystemParser::ParseV2_0(extruder, obj.GetExtderSystem());

    const std::vector<DevExtder>& extruders = obj.GetExtderSystem()->GetExtruders();
    REQUIRE(extruders.size() == 1);
    CHECK_FALSE(extruders[0].HasFilamBackup());
    CHECK(extruders[0].GetCurrentTemp() == 162);
}

TEST_CASE("An extruder status with filam_bak keeps its backup slots", "[DevExtderSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    const json extruder = json::parse(R"({
        "state": 1,
        "info": [
            { "filam_bak": [1, 2], "hnow": 0, "id": 0, "info": 1547, "snow": 8192, "spre": 8192,
              "star": 8192, "stat": 393984, "temp": 162 }
        ]
    })");
    ExtderSystemParser::ParseV2_0(extruder, obj.GetExtderSystem());

    const std::vector<DevExtder>& extruders = obj.GetExtderSystem()->GetExtruders();
    REQUIRE(extruders.size() == 1);
    CHECK(extruders[0].GetFilamBackup() == std::vector<int>{1, 2});
}
