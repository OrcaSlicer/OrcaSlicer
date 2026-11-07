#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/CAD/CadFaceSelection.hpp"
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
    CHECK_FALSE(state.selected_face());
    state.update(face(2));
    auto first = state.selected_face();
    REQUIRE(first);
    const auto revision = first->selection_revision;
    state.update(face(3));
    CHECK(first->face_index == 2);
    REQUIRE(state.selected_face());
    CHECK(state.selected_face()->face_index == 3);
    CHECK(state.selected_face()->selection_revision > revision);
    // Changing the copy cannot mutate the cached native selection.
    first->centroid[0] = 500.;
    CHECK_THAT(state.selected_face()->centroid[0], Catch::Matchers::WithinAbs(1., 1e-12));
    state.update(std::nullopt);
    CHECK_FALSE(state.selected_face());
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
        REQUIRE(state.selected_face());
        CHECK(state.selected_face()->face_index == value->face_index);
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
    const std::uint64_t revision = state.selected_face()->selection_revision;
    CHECK_FALSE(state.update(face(0)));
    auto moved = face(0);
    moved.body_to_design[0][3] = 10.;
    CHECK(bool(state.update(moved)));
    CHECK(state.selected_face()->selection_revision > revision);
    CHECK(bool(state.update(std::nullopt)));
    CHECK_FALSE(state.update(std::nullopt));
}
