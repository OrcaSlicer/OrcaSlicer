#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r {

// Bambu report deltas retain omitted keys, including null values. Arrays replace the whole value.
// Kept independent of Device state so a monitor can reconstruct reports without issuing commands.
inline void merge_json_report(const nlohmann::json& delta, nlohmann::json& report)
{
    if (!delta.is_object() || !report.is_object()) { report = delta; return; }
    for (const auto& item : delta.items()) {
        if (item.value().is_object() && report.contains(item.key()) && report[item.key()].is_object())
            merge_json_report(item.value(), report[item.key()]);
        else
            report[item.key()] = item.value();
    }
}

} // namespace Slic3r
