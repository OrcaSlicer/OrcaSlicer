#pragma once
// Phase 2.3.1 — bridge for the PlaceholderProvider host vtable.
// Print::export_gcode publishes a pointer to its PlaceholderParser here for
// the duration of an on_provide callback. The host thunks in
// engine/src/PluginManager.cpp read this pointer to resolve "where do I set
// this variable?". This keeps libslic3r free of any orca:: namespace pollution
// at the call site and gives the host thunks a stable handle.

namespace Slic3r { class PlaceholderParser; }

namespace orca::placeholder_tls {

// Returns the parser scoped to the current placeholder-provider invocation,
// or nullptr if none is active. Thread-local; setting happens via Scope below.
Slic3r::PlaceholderParser* current();

// RAII guard that publishes the parser for the lifetime of the object.
class Scope {
public:
    explicit Scope(Slic3r::PlaceholderParser* parser);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace orca::placeholder_tls
