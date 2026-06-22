#include "orca/Globals.hpp"

#include "orca/Session.hpp"

#include <cassert>
#include <atomic>

namespace orca {

namespace {
// Atomic so set_session() from one thread is reliably visible to subsequent
// readers without us having to reason about C++'s data-race rules every time
// the rewriter inserts an `orca::session()` call. Loads/stores are relaxed —
// happens-before is established by the higher-level bootstrap protocol
// (set_session() runs once on the main thread before any consumer fires).
std::atomic<Session*> g_session{nullptr};
} // namespace

void set_session(Session* s) noexcept {
    g_session.store(s, std::memory_order_relaxed);
}

bool has_session() noexcept {
    return g_session.load(std::memory_order_relaxed) != nullptr;
}

Session& session() {
    Session* s = g_session.load(std::memory_order_relaxed);
    assert(s && "orca::session() called before orca::set_session() — bootstrap order bug");
    return *s;
}

} // namespace orca
