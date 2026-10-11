#pragma once

#include "core/Workload.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Bench {

// Thrown for a catalog that cannot be read.
class CatalogError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Reads a catalog, an object holding an entries array, refusing an entry validate() refuses, a field it
// does not know, so a misspelled one is never dropped, and a name that repeats.
std::vector<CatalogEntry> read_catalog(std::string_view text);

// Reads every .json file in the directory, named in UTF-8, as one catalog, in file name order, naming
// the file in any error.
std::vector<CatalogEntry> read_catalog_dir(const std::string& directory);

// Which entries a run or a listing takes, where an unset field takes every entry.
struct Selection
{
    // Patterns as matches() reads them, for the names to take and the names to leave out.
    std::optional<std::string> filter;
    std::optional<std::string> exclude;
    std::optional<std::string> kind;
    std::optional<std::string> tag;
};

// The entries that meet every field of the selection, in catalog order, throwing CatalogError, which
// names the fields, when no entry does.
std::vector<CatalogEntry> select_entries(const std::vector<CatalogEntry>& catalog, const Selection& selection);

}} // namespace Slic3r::Bench
