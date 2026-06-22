// Catch2 tests for the pure-C++ orca::Events bus (no C ABI involved).
//
// The bus dispatches typed events to subscribers using std::type_info. These
// tests lock in:
//   - basic publish-to-subscriber delivery
//   - multi-subscriber fan-out
//   - unsubscribe correctly removes a handler
//   - re-entrant subscribe (subscribing from inside a handler) is safe and the
//     newly-registered handler does not fire on the in-flight publish
//   - cross-type isolation (a subscriber to event A doesn't receive event B)
//
// Subscribers fire on whichever thread publishes the event; the tests stay on
// the main thread, but counters use std::atomic to be explicit about the
// memory model contract.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"

#include <atomic>
#include <string>

TEST_CASE("Events::publish<E> delivers to a subscriber on the same thread", "[engine][events]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& events = s->events();

    std::atomic<int>      count{0};
    std::atomic<uint64_t> seen_handle{0};
    std::atomic<float>    seen_progress{0.0f};
    std::string           seen_message;

    auto id = events.subscribe<orca::SlicingProgress>(
        [&](const orca::SlicingProgress& e) {
            seen_handle.store(e.handle);
            seen_progress.store(e.progress);
            seen_message = e.message;
            count.fetch_add(1);
        });
    REQUIRE(id != 0);

    events.publish<orca::SlicingProgress>({42, 0.5f, std::string("halfway")});

    REQUIRE(count.load() == 1);
    REQUIRE(seen_handle.load() == 42);
    REQUIRE_THAT(seen_progress.load(), Catch::Matchers::WithinAbs(0.5f, 1e-5f));
    REQUIRE(seen_message == "halfway");
}

TEST_CASE("Events: multiple subscribers all fire", "[engine][events]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& events = s->events();

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};

    events.subscribe<orca::SlicingProgress>([&](const orca::SlicingProgress&) { count_a.fetch_add(1); });
    events.subscribe<orca::SlicingProgress>([&](const orca::SlicingProgress&) { count_b.fetch_add(1); });

    events.publish<orca::SlicingProgress>({1, 0.0f, std::string{}});

    REQUIRE(count_a.load() == 1);
    REQUIRE(count_b.load() == 1);
}

TEST_CASE("Events::unsubscribe stops delivery", "[engine][events]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& events = s->events();

    std::atomic<int> count{0};
    auto id = events.subscribe<orca::SlicingProgress>(
        [&](const orca::SlicingProgress&) { count.fetch_add(1); });
    REQUIRE(id != 0);

    events.publish<orca::SlicingProgress>({1, 0.0f, std::string{}});
    REQUIRE(count.load() == 1);

    events.unsubscribe(id);

    events.publish<orca::SlicingProgress>({2, 0.0f, std::string{}});
    REQUIRE(count.load() == 1);
}

TEST_CASE("Events bus is reentrant-safe for subscribe inside a handler", "[engine][events]") {
    // Confirms the snapshot-under-lock dispatch (Events.cpp publish_erased)
    // does not deadlock when a handler subscribes a new handler during
    // dispatch, AND that the newly-registered handler does not fire on the
    // in-flight publish (it joins the next publish onward).
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& events = s->events();

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};
    std::atomic<bool> b_registered{false};

    events.subscribe<orca::SlicingProgress>(
        [&](const orca::SlicingProgress&) {
            count_a.fetch_add(1);
            if (!b_registered.exchange(true)) {
                events.subscribe<orca::SlicingProgress>(
                    [&](const orca::SlicingProgress&) { count_b.fetch_add(1); });
            }
        });

    events.publish<orca::SlicingProgress>({1, 0.0f, std::string{}}); // A fires, B registers
    events.publish<orca::SlicingProgress>({2, 0.0f, std::string{}}); // A fires again, B fires once

    REQUIRE(count_a.load() == 2);
    REQUIRE(count_b.load() == 1);
}

TEST_CASE("Events: subscribing to one type does not receive other types", "[engine][events]") {
    auto s = orca::Session::create();
    REQUIRE(s);
    auto& events = s->events();

    std::atomic<int> count{0};
    events.subscribe<orca::SlicingProgress>(
        [&](const orca::SlicingProgress&) { count.fetch_add(1); });

    events.publish<orca::SlicingFinished>({1, true, std::string{}});

    REQUIRE(count.load() == 0);
}
