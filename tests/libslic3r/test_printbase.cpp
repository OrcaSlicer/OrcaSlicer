#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintBase.hpp"

#include <chrono>
#include <mutex>

using namespace Slic3r;

namespace {

// A local enum rather than PrintStep or PrintObjectStep, because the timestamps are a
// property of the state machine and hold for any step list.
enum TestStep { tsFirst, tsSecond, tsCount };

// PrintState takes only a mutex and a cancel callback from its Print, so no Print is
// needed to drive a real step transition.
struct Steps
{
    PrintState<TestStep, tsCount> state;
    std::mutex                    mutex;

    bool start(TestStep step)  { return state.set_started(step, mutex, [](){}); }
    void finish(TestStep step) { state.set_done(step, mutex, [](){}); }

    PrintStateBase::StateWithTimeStamp read(TestStep step) { return state.state_with_timestamp(step, mutex); }
};

constexpr std::chrono::steady_clock::time_point never {};

} // namespace

TEST_CASE("a step that never ran has no timestamps", "[PrintBase]")
{
    Steps steps;

    const auto state = steps.read(tsFirst);
    CHECK(state.state == PrintStateBase::INVALID);
    // The other cases tell a recorded time from an absent one by comparing against epoch,
    // so that premise is checked here rather than assumed.
    CHECK(state.started_at == never);
    CHECK(state.done_at == never);
}

TEST_CASE("a step records when it started", "[PrintBase]")
{
    Steps steps;

    const auto before = std::chrono::steady_clock::now();
    REQUIRE(steps.start(tsFirst));
    const auto after = std::chrono::steady_clock::now();

    const auto started_at = steps.read(tsFirst).started_at;
    CHECK(started_at >= before);
    CHECK(started_at <= after);
}

TEST_CASE("a step records when it finished", "[PrintBase]")
{
    Steps steps;
    REQUIRE(steps.start(tsFirst));

    const auto before = std::chrono::steady_clock::now();
    steps.finish(tsFirst);
    const auto after = std::chrono::steady_clock::now();

    const auto state = steps.read(tsFirst);
    CHECK(state.done_at >= before);
    CHECK(state.done_at <= after);
    CHECK(state.done_at >= state.started_at);
}

TEST_CASE("timestamps land only on the step that ran", "[PrintBase]")
{
    Steps steps;
    REQUIRE(steps.start(tsFirst));
    steps.finish(tsFirst);

    const auto untouched = steps.read(tsSecond);
    CHECK(untouched.started_at == never);
    CHECK(untouched.done_at == never);
}

TEST_CASE("re-entering a finished step leaves its timestamps alone", "[PrintBase]")
{
    Steps steps;
    REQUIRE(steps.start(tsFirst));
    steps.finish(tsFirst);

    const auto finished = steps.read(tsFirst);
    REQUIRE(finished.started_at > never);
    REQUIRE(finished.done_at > never);

    CHECK_FALSE(steps.start(tsFirst));

    const auto after_reentry = steps.read(tsFirst);
    CHECK(after_reentry.started_at == finished.started_at);
    CHECK(after_reentry.done_at == finished.done_at);
}

TEST_CASE("restarting a step clears the finish time of its previous run", "[PrintBase]")
{
    Steps steps;
    REQUIRE(steps.start(tsFirst));
    steps.finish(tsFirst);
    REQUIRE(steps.read(tsFirst).done_at > never);

    REQUIRE(steps.state.invalidate(tsFirst, [](){}));
    REQUIRE(steps.start(tsFirst));

    // A leftover done_at would predate the new started_at and read as a negative span.
    CHECK(steps.read(tsFirst).done_at == never);
}

TEST_CASE("a step canceled on entry records nothing", "[PrintBase]")
{
    Steps steps;

    CHECK_THROWS_AS(steps.state.set_started(tsFirst, steps.mutex, [](){ throw CanceledException(); }),
                    CanceledException);

    CHECK(steps.read(tsFirst).started_at == never);
}

TEST_CASE("invalidating a step keeps the timestamps of its last run", "[PrintBase]")
{
    Steps steps;
    REQUIRE(steps.start(tsFirst));
    steps.finish(tsFirst);

    const auto ran = steps.read(tsFirst);
    REQUIRE(ran.done_at > never);

    // Every path that resets step state, since the header promises the timestamps survive
    // all of them.
    SECTION("one step") {
        REQUIRE(steps.state.invalidate(tsFirst, [](){}));
    }
    SECTION("a range of steps") {
        const TestStep range[] = { tsFirst };
        REQUIRE(steps.state.invalidate_multiple(range, range + 1, [](){}));
    }
    SECTION("every step") {
        REQUIRE(steps.state.invalidate_all([](){}));
    }

    const auto state = steps.read(tsFirst);
    CHECK(state.state == PrintStateBase::INVALID);
    CHECK(state.started_at == ran.started_at);
    CHECK(state.done_at == ran.done_at);
}
