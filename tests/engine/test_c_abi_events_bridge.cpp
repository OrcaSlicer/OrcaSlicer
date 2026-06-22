// Catch2 tests for the C ABI events bridge.
//
// The bridge lives in engine/src/c_api.cpp: orca_events_subscribe() registers a
// C-typed callback by subscribing to the underlying C++ bus with a lambda that
// translates the C++ event into a C POD payload. These tests verify the
// translation round-trips correctly for two representative event kinds, that
// unsubscribe is honoured by the bridge, and that the C ABI guards against
// null callback / null events.
//
// We obtain the orca_events_t* by reinterpret_cast'ing &session->events() —
// the same mechanism orca_session_events() uses internally — so the tests can
// publish on the C++ bus and observe via the C subscriber, exercising the
// bridge end-to-end.

#include <catch2/catch_all.hpp>

#include "orca/c_api.h"
#include "orca/Session.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"

#include <cstdint>
#include <string>

namespace {

struct ProgressCapture {
    int         count = 0;
    uint64_t    handle = 0;
    float       progress = 0.0f;
    std::string message;
};

struct ExportFinishedCapture {
    int         count = 0;
    uint64_t    handle = 0;
    bool        success = false;
    std::size_t line_count = 0;
    std::string error;
};

} // namespace

TEST_CASE("C ABI events bridge: SlicingProgress is delivered with correct payload",
          "[engine][c_abi][events]") {
    auto sess = orca::Session::create();
    REQUIRE(sess);

    // Mirror orca_session_events(): the events handle is just a reinterpret of
    // &Session::events(). We do this directly because the public C API for
    // session lookup expects an orca_session_t built via orca_session_create,
    // which is a different object than our C++ std::unique_ptr<Session>.
    auto* events = reinterpret_cast<orca_events_t*>(&sess->events());

    ProgressCapture cap;

    auto cb = +[](orca_event_kind_t kind, const void* payload, void* ud) {
        auto* c = static_cast<ProgressCapture*>(ud);
        // Cannot REQUIRE inside a C-callback path on a worker thread, but here
        // we're on the main thread (publish is synchronous) — still, record
        // a discrepancy via a sentinel value rather than crashing the suite.
        if (kind != ORCA_EVT_SLICING_PROGRESS) {
            c->message = "WRONG_KIND";
            return;
        }
        const auto* p = static_cast<const orca_evt_slicing_progress_t*>(payload);
        c->count++;
        c->handle   = p->handle;
        c->progress = p->progress;
        c->message  = p->message ? std::string(p->message) : std::string{};
    };

    auto id = orca_events_subscribe(events, ORCA_EVT_SLICING_PROGRESS, cb, &cap);
    REQUIRE(id != 0);

    sess->events().publish<orca::SlicingProgress>({77, 0.42f, std::string("warming up")});

    REQUIRE(cap.count == 1);
    REQUIRE(cap.handle == 77);
    REQUIRE_THAT(cap.progress, Catch::Matchers::WithinAbs(0.42f, 1e-5f));
    REQUIRE(cap.message == "warming up");

    orca_events_unsubscribe(events, id);

    sess->events().publish<orca::SlicingProgress>({78, 1.0f, std::string("done")});
    REQUIRE(cap.count == 1);
}

TEST_CASE("C ABI events bridge: ExportFinished payload round-trips",
          "[engine][c_abi][events]") {
    auto sess = orca::Session::create();
    REQUIRE(sess);

    auto* events = reinterpret_cast<orca_events_t*>(&sess->events());

    ExportFinishedCapture cap;

    auto cb = +[](orca_event_kind_t kind, const void* payload, void* ud) {
        auto* c = static_cast<ExportFinishedCapture*>(ud);
        if (kind != ORCA_EVT_EXPORT_FINISHED) {
            c->error = "WRONG_KIND";
            return;
        }
        const auto* p = static_cast<const orca_evt_export_finished_t*>(payload);
        c->count++;
        c->handle     = p->handle;
        c->success    = p->success;
        c->line_count = p->line_count;
        c->error      = p->error ? std::string(p->error) : std::string{};
    };

    auto id = orca_events_subscribe(events, ORCA_EVT_EXPORT_FINISHED, cb, &cap);
    REQUIRE(id != 0);

    sess->events().publish<orca::ExportFinished>({9, true, 0, std::string{}});

    REQUIRE(cap.count == 1);
    REQUIRE(cap.handle == 9);
    REQUIRE(cap.success == true);
    REQUIRE(cap.line_count == 0);
    REQUIRE(cap.error.empty());

    orca_events_unsubscribe(events, id);
}

TEST_CASE("C ABI events subscribe with null callback returns 0",
          "[engine][c_abi][events]") {
    auto sess = orca::Session::create();
    REQUIRE(sess);
    auto* events = reinterpret_cast<orca_events_t*>(&sess->events());

    auto id = orca_events_subscribe(events, ORCA_EVT_SLICING_PROGRESS, nullptr, nullptr);
    REQUIRE(id == 0);
}

TEST_CASE("C ABI events subscribe with null events returns 0",
          "[engine][c_abi][events]") {
    auto cb = +[](orca_event_kind_t, const void*, void*) {};
    auto id = orca_events_subscribe(nullptr, ORCA_EVT_SLICING_PROGRESS, cb, nullptr);
    REQUIRE(id == 0);
}
