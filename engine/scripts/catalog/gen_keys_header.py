#!/usr/bin/env python3
"""Generate engine/include/orca/ConfigKeys.hpp from the ConfigDef catalog TSV.

Usage:
    gen_keys_header.py <config_keys.raw.tsv> <output_header.hpp>

TSV format (tab-separated): <PrintConfig.cpp line>  <key>  <coType>
"""

import sys

# Map libslic3r ConfigOptionType -> (primitive C++ type, is_vector, is_nullable)
TYPEMAP = {
    "coFloat":            ("double",                       False, False),
    "coFloats":           ("double",                       True,  False),
    "coFloatsNullable":   ("double",                       True,  True),
    "coInt":              ("int",                          False, False),
    "coInts":             ("int",                          True,  False),
    "coIntsNullable":     ("int",                          True,  True),
    "coString":           ("std::string",                  False, False),
    "coStrings":          ("std::string",                  True,  False),
    "coBool":             ("bool",                         False, False),
    "coBools":            ("unsigned char",                True,  False),
    "coBoolsNullable":    ("unsigned char",                True,  True),
    "coPercent":          ("double",                       False, False),
    "coPercents":         ("double",                       True,  False),
    "coFloatOrPercent":   ("Slic3r::FloatOrPercent",       False, False),
    "coFloatsOrPercents": ("Slic3r::FloatOrPercent",       True,  False),
    "coPoint":            ("Slic3r::Vec2d",                False, False),
    "coPoints":           ("Slic3r::Vec2d",                True,  False),
    "coPointsGroups":     ("std::vector<Slic3r::Vec2d>",   True,  False),
    "coEnum":             ("int",                          False, False),
    "coEnums":            ("int",                          True,  False),
}


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <input.tsv> <output.hpp>")

    catalog_path, out_path = sys.argv[1], sys.argv[2]

    # Some keys are registered multiple times in PrintConfig.cpp — sometimes
    # because one of the registrations is inside a /* */ block comment our
    # grep doesn't detect, sometimes because a key was migrated to a new type
    # and the old registration was left orphaned. The runtime takes the LAST
    # registration; mirror that behavior here.
    keys_by_name: dict[str, tuple[int, str, str]] = {}
    with open(catalog_path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3:
                continue
            line_no, key, co_type = int(parts[0]), parts[1], parts[2]
            keys_by_name[key] = (line_no, key, co_type)

    keys = sorted(keys_by_name.values(), key=lambda x: x[1])

    unknown = sorted({k[2] for k in keys if k[2] not in TYPEMAP})
    if unknown:
        sys.exit(f"unknown co_type(s) in catalog: {unknown}")

    with open(out_path, "w") as f:
        f.write(HEADER_PROLOGUE)
        for _line, key, co_type in keys:
            prim, is_vec, is_null = TYPEMAP[co_type]
            co_enum = co_type[2:]
            f.write(
                f"struct {key} {{ "
                f"using type = {prim}; "
                f'static constexpr std::string_view name = "{key}"; '
                f"static constexpr CoType co_type = CoType::{co_enum}; "
                f"static constexpr bool is_vector = {'true' if is_vec else 'false'}; "
                f"static constexpr bool is_nullable = {'true' if is_null else 'false'}; "
                f"}};\n"
            )
        f.write(HEADER_EPILOGUE)

    print(f"gen_keys_header: wrote {out_path} ({len(keys)} key tags)")


HEADER_PROLOGUE = '''#pragma once

// orca/ConfigKeys.hpp — typed-config key registry.
//
// Generated from libslic3r/PrintConfig.cpp's ConfigDef. DO NOT HAND-EDIT.
// To regenerate: engine/scripts/catalog/regen_keys.sh
//
// For each registered ConfigDef key, this header provides:
//   - struct orca::keys::<key>            (compile-time key tag)
//   - using ::type      = primitive C++ type
//   - static ::name     = string view "<key>"
//   - static ::co_type  = libslic3r ConfigOptionType (the coXXX enum)
//   - static ::is_vector / ::is_nullable
//
// Typed Presets methods accept the key tag as a template argument:
//   double h = orca::session().presets().opt<keys::layer_height>(ConfigScope::Full).value_or(0.2);

// ConfigKeys is self-contained: it only declares POD tag structs. Consumers
// who want to ACT on these keys (Presets methods, orca::config:: helpers)
// pull in orca/Config.hpp instead — that's the umbrella that brings keys +
// scoped templates + adhoc helpers together. Keeping ConfigKeys minimal
// avoids a circular include with Presets.hpp.

#include <string_view>
#include <vector>
#include <Eigen/Core>

// Forward declarations of libslic3r types referenced in the type traits.
//
// Vec2d alias matches libslic3r/Point.hpp exactly (DontAlign matters — the
// default alignment would conflict at include time with libslic3r's own
// using-declaration in TU's that pull both headers).
namespace Slic3r {
struct FloatOrPercent;
using Vec2d = Eigen::Matrix<double, 2, 1, Eigen::DontAlign>;
} // namespace Slic3r

namespace orca {

// Mirrors libslic3r's ConfigOptionType enum (coXXX values). Codified here so
// the typed registry doesn't pull in libslic3r/Config.hpp transitively.
enum class CoType : int {
    Float,
    Floats,
    FloatsNullable,
    Int,
    Ints,
    IntsNullable,
    String,
    Strings,
    Bool,
    Bools,
    BoolsNullable,
    Percent,
    Percents,
    FloatOrPercent,
    FloatsOrPercents,
    Point,
    Points,
    PointsGroups,
    Enum,
    Enums,
};

namespace keys {

// Each key is a struct so it can be passed as a template tag (zero runtime cost).
'''

HEADER_EPILOGUE = '''
} // namespace keys

} // namespace orca
'''


if __name__ == "__main__":
    main()
