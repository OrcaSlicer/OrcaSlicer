#include "orca/PlaceholderProvider.hpp"

namespace orca::placeholder_tls {

namespace { thread_local Slic3r::PlaceholderParser* g_current = nullptr; }

Slic3r::PlaceholderParser* current() { return g_current; }

Scope::Scope(Slic3r::PlaceholderParser* parser) { g_current = parser; }
Scope::~Scope() { g_current = nullptr; }

} // namespace orca::placeholder_tls
