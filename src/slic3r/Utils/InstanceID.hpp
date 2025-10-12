#pragma once

#include <string>

namespace Slic3r {

class AppConfig;

namespace instance_id {

// Returns the canonical IID, generating and storing one when missing.
std::string ensure(AppConfig& config);

// Clears the cached IID (primarily for tests).
void reset_cache_for_tests();

} // namespace instance_id

} // namespace Slic3r

