// Match the include environment that libslic3r_gui TUs get from pchheader.hpp: Windows.h with
// WIN32_LEAN_AND_MEAN/NOMINMAX must come first so rpcndr.h's `byte` is processed before <cstddef>
// makes std::byte a competing candidate (otherwise the Windows COM headers pulled in via
// DeviceManager.hpp error with an ambiguous `byte`). wx/timer.h must precede DeviceManager.hpp,
// which includes DeviceErrorDialog.hpp (uses wxTimerEvent) before its own wx/timer.h include.
#include "catch2/catch_message.hpp"
#include <string>
#include <utility>
#include <vector>
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
#include <catch2/catch_all.hpp>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("Extruder state decodes a generic toolhead set", "[DevExtderSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // device.extruder.state packing: count 0..3, current 4..7, target 8..11,
    // switch 12..14, loading 15..18, busy 19.
    const int extruder_count   = 4;
    const int current_extruder = 2;
    const int switch_state     = ES_BUSY;
    const int loading_extruder = 1;
    const int state = extruder_count | (current_extruder << 4) | (3 << 8) |
                      (switch_state << 12) | (loading_extruder << 15) | (1 << 19);

    json extruder = { {"state", state}, {"info", json::array()} };
    for (int id = 0; id < extruder_count; ++id) {
        // info: bit 1 has filament, bit 3 nozzle present.
        // temp: current 0..15, target 16..31. spre/snow/star: slot 0..7, ams 8..15.
        extruder["info"].push_back({
            {"id", id},
            {"filam_bak", json::array({id})},
            {"info", (id % 2 == 0 ? 2 : 0) | 8},
            {"temp", (200 + id) | ((250 + id) << 16)},
            {"spre", 10 << 8},
            {"snow", id | ((20 + id) << 8)},
            {"star", 30 << 8},
            {"stat", 0},
            {"hnow", id},
        });
    }

    ExtderSystemParser::ParseV2_0(extruder, obj.GetExtderSystem());

    REQUIRE(obj.GetExtderSystem()->GetTotalExtderCount() == extruder_count);
    CHECK(obj.GetExtderSystem()->GetCurrentExtderId() == current_extruder);
    CHECK(static_cast<int>(obj.GetExtderSystem()->GetSwitchState()) == switch_state);
    CHECK(obj.GetExtderSystem()->GetLoadingExtderId() == loading_extruder);
    CHECK(obj.GetExtderSystem()->IsBusyLoading());

    for (int id = 0; id < extruder_count; ++id) {
        const auto ext = obj.GetExtderSystem()->GetExtderById(id);
        REQUIRE(ext.has_value());
        INFO("extruder " << id);
        CHECK(ext->GetExtId() == id);
        CHECK(ext->GetCurrentTemp() == 200 + id);
        CHECK(ext->GetTargetTemp() == 250 + id);
        CHECK(ext->HasFilamentInExt() == (id % 2 == 0));
        CHECK(ext->GetFilamBackup() == std::vector<int>{id});
        CHECK(ext->GetSlotPre().ams_id == "10");
        CHECK(ext->GetSlotNow().ams_id == std::to_string(20 + id));
        CHECK(ext->GetSlotNow().slot_id == std::to_string(id));
        CHECK(ext->GetSlotTarget().ams_id == "30");
        CHECK(ext->GetNozzleId() == id);
    }
}
