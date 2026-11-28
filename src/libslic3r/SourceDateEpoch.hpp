#pragma once

#include <ctime>
#include <optional>

namespace Slic3r {

// The time to use in place of the current time: the ORCA_SOURCE_DATE_EPOCH environment variable,
// a whole number of seconds since the Unix epoch. Unset, empty or anything else gives nullopt.
//
// With it set, custom G-code prints that time (in UTC) and random() draws the same numbers on every
// export, so two slices of the same input give the same G-code and can be compared. It is not the
// standard SOURCE_DATE_EPOCH, which packaging shells export for every program they start.
std::optional<std::time_t> source_date_epoch();

} // namespace Slic3r
