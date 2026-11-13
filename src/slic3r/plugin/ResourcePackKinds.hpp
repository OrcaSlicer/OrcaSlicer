#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {

// What a plugin file is for. A consumer (the Bed Shape dialog, the displacement texture picker) lists only the
// files of its kind.
inline constexpr std::string_view RESOURCE_KIND_BED_MODEL       = "bed_model";
inline constexpr std::string_view RESOURCE_KIND_BED_TEXTURE     = "bed_texture";
inline constexpr std::string_view RESOURCE_KIND_DISPLACEMENT_TEXTURE = "displacement_texture";

// Kind -> validated absolute paths of the files of that kind, sorted, at most ResourceKindSpec::max_files each.
using ResourceFilesByKind = std::map<std::string, std::vector<std::string>>;

struct ResourceKindSpec
{
    std::string_view              name;
    std::vector<std::string_view> extensions; // lower case, with the dot
    size_t                        max_files;  // per kind, applied after sorting
};

// nullptr for a kind this slicer does not know.
inline const ResourceKindSpec* find_resource_kind(std::string_view name)
{
    static const std::vector<ResourceKindSpec> specs = {
        {RESOURCE_KIND_BED_MODEL, {".stl"}, 16},
        {RESOURCE_KIND_BED_TEXTURE, {".png", ".svg"}, 16},
        {RESOURCE_KIND_DISPLACEMENT_TEXTURE, {".png", ".jpg", ".jpeg", ".bmp"}, 64},
    };
    for (const ResourceKindSpec& spec : specs)
        if (spec.name == name)
            return &spec;
    return nullptr;
}

} // namespace Slic3r
