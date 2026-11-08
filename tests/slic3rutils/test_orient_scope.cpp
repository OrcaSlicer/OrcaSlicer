// Which instances auto-orient takes (OrientJob::selected_instances). Tag: [orient].
// Toolbar Orient with nothing selected and every plate-scoped orient (plate menu, the Agent
// bridge) take the current plate only; nothing defaults to every plate.

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Jobs/OrientJob.hpp"

#include <vector>

using Slic3r::GUI::OrientJob;

namespace {

// Object 0: instance 0 on the current plate, instance 1 on another plate.
// Object 1: one instance on another plate. Object 2: one instance on the current plate.
const std::vector<size_t> kCounts = {2, 1, 1};

bool on_current(int o, int i) { return (o == 0 && i == 0) || o == 2; }

using Sel = std::vector<std::vector<bool>>;

} // namespace

TEST_CASE("Orient scope: nothing selected takes the current plate only", "[orient]")
{
    const Sel sel = OrientJob::selected_instances(kCounts, {false, false, false}, false, on_current);
    CHECK(sel == Sel{{true, false}, {false}, {true}});
}

TEST_CASE("Orient scope: a selection takes every instance of the selected objects", "[orient]")
{
    const Sel sel = OrientJob::selected_instances(kCounts, {true, true, false}, false, on_current);
    CHECK(sel == Sel{{true, true}, {true}, {false}});
}

TEST_CASE("Orient scope: plate-scoped takes the current plate's instances, whatever the selection", "[orient]")
{
    CHECK(OrientJob::selected_instances(kCounts, {true, true, true}, true, on_current) == Sel{{true, false}, {false}, {true}});
    CHECK(OrientJob::selected_instances(kCounts, {false, false, false}, true, on_current) == Sel{{true, false}, {false}, {true}});
}

TEST_CASE("Orient scope: an empty current plate takes nothing", "[orient]")
{
    const auto none = [](int, int) { return false; };
    CHECK(OrientJob::selected_instances(kCounts, {false, false, false}, false, none) == Sel{{false, false}, {false}, {false}});
    CHECK(OrientJob::selected_instances(kCounts, {false, false, false}, true, none) == Sel{{false, false}, {false}, {false}});
}
