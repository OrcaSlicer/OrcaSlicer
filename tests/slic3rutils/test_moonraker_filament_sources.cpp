#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include "slic3r/Utils/MoonrakerFilamentSources.hpp"

using namespace Slic3r;

namespace {
struct Tray
{
    bool has_filament;
    int slot_index;
    std::string material;
};

struct Reader
{
    bool success = false;
    std::vector<Tray> result;
    int max_index = 0;
    int calls     = 0;
    bool operator()(std::vector<Tray>& trays, int& max_lane_index)
    {
        ++calls;
        trays          = result;
        max_lane_index = max_index;
        return success;
    }
};

struct Sources
{
    Reader lanes, hh, cfs;
    std::vector<Tray> trays;
    int max_index = 0;
    MoonrakerFilamentSource read()
    {
        return select_moonraker_filament_data(
            trays, max_index, [&](auto& out, int& index) { return lanes(out, index); },
            [&](auto& out, int& index) { return hh(out, index); }, [&](auto& out, int& index) { return cfs(out, index); });
    }
};
} // namespace

TEST_CASE("Populated lane data keeps priority over Happy Hare and CFS", "[MoonrakerFilamentSources]")
{
    Sources sources;
    sources.lanes = {true, {{false, 0, ""}, {true, 4, "PLA"}}, 7};
    sources.hh    = {true, {{true, 2, "PETG"}}, 2};
    sources.cfs   = {true, {{true, 1, "ABS"}}, 1};
    REQUIRE(sources.read() == MoonrakerFilamentSource::Lanes);
    REQUIRE(sources.trays.size() == 2);
    CHECK(sources.trays[1].material == "PLA");
    CHECK(sources.max_index == 7);
    CHECK(sources.hh.calls == 0);
    CHECK(sources.cfs.calls == 0);
}

TEST_CASE("Empty lane data lets Happy Hare supply loaded gates and keeps the widest topology", "[MoonrakerFilamentSources][Regression]")
{
    Sources sources;
    const int loaded_gate = GENERATE(2, 9);
    sources.lanes         = {true, {{false, 0, ""}, {false, 7, ""}}, 7};
    sources.hh            = {true, {{true, loaded_gate, "PETG"}}, loaded_gate};
    REQUIRE(sources.read() == MoonrakerFilamentSource::HappyHare);
    REQUIRE(sources.trays.size() == 1);
    CHECK(sources.trays.front().has_filament);
    CHECK(sources.trays.front().slot_index == loaded_gate);
    CHECK(sources.trays.front().material == "PETG");
    CHECK(sources.max_index == (loaded_gate > 7 ? loaded_gate : 7));
    CHECK(sources.cfs.calls == 0);
}

TEST_CASE("An empty lane topology survives an unavailable Happy Hare and CFS", "[MoonrakerFilamentSources][Regression]")
{
    Sources sources;
    sources.lanes = {true, {{false, 0, ""}, {false, 7, ""}}, 7};
    sources.hh    = {false, {{true, 19, "discard partial HH"}}, 19};
    sources.cfs   = {false, {{true, 23, "discard partial CFS"}}, 23};
    REQUIRE(sources.read() == MoonrakerFilamentSource::EmptyLanes);
    CHECK(sources.trays.empty());
    CHECK(sources.max_index == 7);
    CHECK(sources.hh.calls == 1);
    CHECK(sources.cfs.calls == 1);
}

TEST_CASE("CFS stays reachable after empty lanes and failed Happy Hare", "[MoonrakerFilamentSources][Regression]")
{
    Sources sources;
    sources.lanes = {true, {{false, 7, ""}}, 7};
    sources.hh    = {false, {{true, 12, "discard partial HH"}}, 12};
    sources.cfs   = {true, {{true, 1, "ABS"}}, 3};
    REQUIRE(sources.read() == MoonrakerFilamentSource::CrealityCfs);
    REQUIRE(sources.trays.size() == 1);
    CHECK(sources.trays.front().material == "ABS");
    CHECK(sources.max_index == 3);
}

TEST_CASE("A missing lane namespace leaves the existing fallback sources available", "[MoonrakerFilamentSources]")
{
    Sources sources;
    sources.lanes = {false, {{true, 27, "discard invalid lanes"}}, 27};
    sources.hh    = {true, {{true, 4, "PETG"}}, 4};
    REQUIRE(sources.read() == MoonrakerFilamentSource::HappyHare);
    REQUIRE(sources.trays.size() == 1);
    CHECK(sources.max_index == 4);
    CHECK(sources.cfs.calls == 0);
}

TEST_CASE("No successful source leaves no filament system or partial reader data", "[MoonrakerFilamentSources]")
{
    Sources sources;
    sources.lanes = {false, {{true, 27, "invalid lanes"}}, 27};
    sources.hh    = {false, {{true, 19, "partial HH"}}, 19};
    sources.cfs   = {false, {{true, 23, "partial CFS"}}, 23};
    CHECK(sources.read() == MoonrakerFilamentSource::None);
    CHECK(sources.trays.empty());
    CHECK(sources.max_index == 0);
}
