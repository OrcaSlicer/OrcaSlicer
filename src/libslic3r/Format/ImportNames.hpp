#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// Names follow Orca's rename restrictions as well as XML 1.0 and UTF-8 requirements.
bool has_unsupported_name_characters(const std::string& name);
bool valid_import_name_replacement(const std::string& replacement);

// Empty replacement means removal; only collisions introduced by cleaning receive suffixes.
std::vector<std::string> sanitize_import_names(const std::vector<std::string>& names,
                                             const std::string& replacement,
                                             const std::string& fallback = "Part");

} // namespace Slic3r
