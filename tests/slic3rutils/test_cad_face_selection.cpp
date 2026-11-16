#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/CAD/CadFaceSelection.hpp"
#include "slic3r/GUI/CAD/CadShapeSelection.hpp"
#include "slic3r/plugin/host/CadFaceSelectionState.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {
GUI::CadFaceSelection face(int index)
{
    GUI::CadFaceSelection result;
    result.body_index = 0;
    result.face_index = index;
    result.centroid = {1., 2., 3.};
    result.normal = std::array<double, 3>{0., 0., 1.};
    result.area = 20.;
    result.is_planar = true;
    return result;
}
} // namespace

TEST_CASE("Face snapshots survive subsequent selections and deselection", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    CHECK_FALSE(state.selected());
    state.update(face(2));
    auto first = state.selected();
    REQUIRE(first);
    const auto revision = first->selection_revision;
    state.update(face(3));
    CHECK(first->face_index == 2);
    REQUIRE(state.selected());
    CHECK(state.selected()->face_index == 3);
    CHECK(state.selected()->selection_revision > revision);
    // Changing the copy cannot mutate the cached native selection.
    first->centroid[0] = 500.;
    CHECK_THAT(state.selected()->centroid[0], Catch::Matchers::WithinAbs(1., 1e-12));
    state.update(std::nullopt);
    CHECK_FALSE(state.selected());
    CHECK(first->face_index == 2);
}

TEST_CASE("Selection events retain their own snapshots while delivery is deferred", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    std::vector<CadFaceSnapshot> delivered;
    auto subscription = state.subscribe("plugin", [&](const CadFaceSnapshot& value, const CadFaceSubscription&) {
        delivered.push_back(value);
    });
    auto first = state.update(face(0));
    auto second = state.update(face(1));
    auto cleared = state.update(std::nullopt);
    CHECK(delivered.empty());
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(cleared);
    first();
    second();
    cleared();
    REQUIRE(delivered.size() == 3);
    REQUIRE(delivered[0]);
    REQUIRE(delivered[1]);
    CHECK(delivered[0]->face_index == 0);
    CHECK(delivered[1]->face_index == 1);
    CHECK_FALSE(delivered[2]);
    CHECK(subscription->is_active());
}

TEST_CASE("Closing a subscription discards queued deliveries", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    int calls = 0;
    auto subscription = state.subscribe("plugin", [&](const CadFaceSnapshot&, const CadFaceSubscription&) { ++calls; });
    auto pending = state.update(face(0));
    subscription->close();
    subscription->close();
    CHECK_FALSE(subscription->is_active());
    pending();
    CHECK(calls == 0);
}

TEST_CASE("Plugin teardown closes only that plugin's subscriptions", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    int a_calls = 0, b_calls = 0;
    auto a = state.subscribe("a", [&](const CadFaceSnapshot&, const CadFaceSubscription&) { ++a_calls; });
    auto b = state.subscribe("b", [&](const CadFaceSnapshot&, const CadFaceSubscription&) { ++b_calls; });
    auto pending = state.update(face(0));
    state.close_for_plugin("a");
    pending();
    CHECK(a_calls == 0);
    CHECK(b_calls == 1);
    CHECK_FALSE(a->is_active());
    CHECK(b->is_active());
}

TEST_CASE("Selection callbacks can unsubscribe and query state without deadlocking", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    int calls = 0;
    std::shared_ptr<CadFaceSubscription> subscription;
    subscription = state.subscribe("plugin", [&](const CadFaceSnapshot& value, const CadFaceSubscription&) {
        ++calls;
        REQUIRE(value);
        REQUIRE(state.selected());
        CHECK(state.selected()->face_index == value->face_index);
        subscription->close();
    });
    state.update(face(0))();
    state.update(face(1))();
    CHECK(calls == 1);
}

TEST_CASE("Queued events neither retain forgotten subscriptions nor reach later subscribers", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    int calls = 0;
    auto subscription = state.subscribe("plugin", [&](const CadFaceSnapshot&, const CadFaceSubscription&) { ++calls; });
    std::weak_ptr<CadFaceSubscription> weak = subscription;
    auto pending = state.update(face(0));
    subscription.reset();
    CHECK(weak.expired());
    auto later = state.subscribe("plugin", [&](const CadFaceSnapshot&, const CadFaceSubscription&) { ++calls; });
    pending();
    CHECK(calls == 0);
    state.update(face(1))();
    CHECK(calls == 1);
}

TEST_CASE("Unchanged selections are silent and changed geometry gets a new revision", "[CadFaceSelection]")
{
    CadFaceSelectionState state;
    CHECK_FALSE(state.update(std::nullopt));
    CHECK(bool(state.update(face(0))));
    const std::uint64_t revision = state.selected()->selection_revision;
    CHECK_FALSE(state.update(face(0)));
    auto moved = face(0);
    moved.body_to_design[0][3] = 10.;
    CHECK(bool(state.update(moved)));
    CHECK(state.selected()->selection_revision > revision);
    CHECK(bool(state.update(std::nullopt)));
    CHECK_FALSE(state.update(std::nullopt));
}

TEST_CASE("Generic selection events preserve each pick type and extended edge sets", "[CadFaceSelection]")
{
    CadShapeSelectionState state;
    std::vector<std::optional<GUI::CadShapeSelection>> delivered;
    auto subscription = state.subscribe("plugin", [&](const auto& value, const auto&) { delivered.push_back(value); });
    GUI::CadShapeSelection selection;
    selection.kind = "face";
    selection.body_index = 0;
    selection.face_index = 3;
    auto face_event = state.update(selection);
    selection.kind = "edge";
    selection.face_index = -1;
    selection.edge_index = 2;
    selection.edge_indices = {2};
    auto edge_event = state.update(selection);
    const auto revision = state.selected()->selection_revision;
    selection.edge_indices = {1, 2};
    auto extended_event = state.update(selection);
    REQUIRE(extended_event);
    CHECK(state.selected()->selection_revision > revision);
    CHECK_FALSE(state.update(selection));
    selection.kind = "vertex";
    selection.edge_index = -1;
    selection.edge_indices.clear();
    selection.vertex_index = 5;
    auto vertex_event = state.update(selection);
    selection.kind = "body";
    selection.vertex_index = -1;
    auto body_event = state.update(selection);
    auto cleared_event = state.update(std::nullopt);
    face_event(); edge_event(); extended_event(); vertex_event(); body_event(); cleared_event();
    REQUIRE(delivered.size() == 6);
    REQUIRE(delivered[0]);
    REQUIRE(delivered[1]);
    REQUIRE(delivered[2]);
    REQUIRE(delivered[3]);
    REQUIRE(delivered[4]);
    CHECK(delivered[0]->kind == "face");
    CHECK(delivered[1]->kind == "edge");
    CHECK(delivered[1]->edge_indices.size() == 1);
    CHECK(delivered[2]->edge_indices.size() == 2);
    CHECK(delivered[3]->vertex_index == 5);
    CHECK(delivered[4]->kind == "body");
    CHECK_FALSE(delivered[5]);
    state.close_for_plugin("plugin");
    CHECK_FALSE(subscription->is_active());
}
