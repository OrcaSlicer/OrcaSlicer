#pragma once

// Globals — process-wide access to the engine Session.
//
// Transform A rewrites every `wxGetApp().preset_bundle` call site to
// `orca::session().presets().raw_ptr()`. That requires a stable, header-only
// accessor that any GUI/utility TU can call. The Session itself is created
// once during GUI/CLI bootstrap (typically in GUI_App::OnInit) and registered
// via set_session(). Threading: set/clear happen on the main thread before
// any consumer touches it; reads are unsynchronised because they happen after
// initialization.
//
// This is intentionally a thin global, not a singleton — the Session is
// owned externally and Globals just borrows the pointer. set_session(nullptr)
// during shutdown clears the borrow.

namespace orca {

class Session;

// Returns the registered Session. Asserts a Session has been registered.
// Phase 1 may collapse this into a true singleton owned by the engine.
Session& session();

// True after set_session(non_null). Useful for code that runs both during
// bootstrap (no session yet) and afterwards.
bool has_session() noexcept;

void set_session(Session* s) noexcept;

} // namespace orca
