#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// The IMEX mode placeholders ({imex_mode}, {imex_mode_index}, {imex_mode_gcode}) and the mode
// script that consumes them are set up in GCode.cpp's _do_export, immediately before
// machine_start_gcode is processed. Two contracts live in that ordering:
//   * the three placeholders exist for every G-code script the export runs, IMEX or not;
//   * the mode script is itself run through the placeholder parser, and it runs FIRST, so a
//     {global ...} it declares is in scope by the time machine_start_gcode is processed.
// Nothing else in the suite exercises them.

// Offset of the first line of `gcode` that equals `expected` once trailing whitespace is
// stripped, or npos. Whole-line matching is deliberate on both counts: prefix matching would
// let ";IMEX_MODE_INDEX:2" pass for an emitted index of 20, and the config block the exporter
// appends to every file repeats machine_start_gcode and imex_mode_gcodes verbatim (as
// "; key = value"), which would make any substring search for the templates below trivially
// true and every "not emitted" assertion trivially false.
static std::size_t find_exact_line(const std::string &gcode, const std::string &expected)
{
    std::size_t pos = 0;
    while (pos <= gcode.size()) {
        const std::size_t eol = gcode.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? gcode.size() : eol;
        std::string       line = gcode.substr(pos, end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line == expected)
            return pos;
        if (eol == std::string::npos)
            break;
        pos = eol + 1;
    }
    return std::string::npos;
}

static bool has_line(const std::string &gcode, const std::string &expected)
{
    return find_exact_line(gcode, expected) != std::string::npos;
}

// Mode scripts. Free of placeholders so that a test can tell "the script was emitted" apart
// from "the script was expanded"; the expansion case below supplies its own template.
static const char *kCopyScript = "SET_DUAL_CARRIAGE MODE=COPY";
static const char *kQuadScript = "SET_QUAD_CARRIAGE MODE=COPY";

// Reports all three placeholders on their own lines so find_exact_line can pin each value.
static const char *kReportStartGcode = ";IMEX_MODE:{imex_mode}\n"
                                       ";IMEX_MODE_INDEX:{imex_mode_index}\n"
                                       ";IMEX_MODE_GCODE:{imex_mode_gcode}\n";

// Shared IMEX printer geometry: 7 logical extruders across 4 physical heads, three modes.
// physical_extruder_map is only honoured when its length matches the nozzle count
// (PrintApply feeds effective_physical_extruder_map the nozzle_diameter size), so the nozzle
// keys must be sized to 7 or the map is silently replaced with the identity and IMEX quietly
// stops happening at all. Mirrors imex_7x4_printer() in test_multifilament.cpp; `iq-copy` is
// carried over from the IQEX case there, which is a mode combination known to slice.
//
// imex_mode_gcodes is left to each test: the position a mode's script occupies in this list is
// exactly what {imex_mode_index} is asserted against.
static void imex_7x4_printer(DynamicPrintConfig &config)
{
    config.set_deserialize_strict({
        { "nozzle_diameter",           "0.4,0.4,0.4,0.4,0.4,0.4,0.4" },
        { "printer_extruder_id",       "1,2,3,4,5,6,7" },
        { "printer_extruder_variant",  "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard,Direct Drive Standard,Direct Drive Standard,"
                                       "Direct Drive Standard" },
        { "extruder_printable_height", "0,0,0,0,0,0,0" },
        { "physical_extruder_map",     "0,0,0,0,1,2,3" },
        { "is_imex",                   "1" },
        { "imex_mode_names",           "primary;copy;iq-copy" },
        { "imex_mode_active_tools",    "0:P;0:P,1:C;0:P,1:C,2:C,3:C" },
        { "skirt_loops",               "0" },
        { "brim_type",                 "no_brim" },
        // Klipper skips the bed/extruder temperature block that would otherwise be written
        // between the mode script and machine_start_gcode, so the ordering assertion below
        // compares the two scripts and nothing else.
        { "gcode_flavor",              "klipper" },
    });
}

// Set the mode scripts positionally, one per name in imex_mode_names. Assigning the vector
// beats set_deserialize_strict here: a coStrings round trip through a ';'-joined string has to
// quote and escape empty entries and embedded braces, and the empty first entry (primary, which
// deliberately has no script) is exactly the entry that would be lost.
static void set_mode_gcodes(DynamicPrintConfig &config, const std::vector<std::string> &gcodes)
{
    config.option<ConfigOptionStrings>("imex_mode_gcodes", true)->values = gcodes;
}

// Route every region to one filament. An unset *_filament_id is not "inherit":
// clamp_feature_filament_to_valid rewrites <=0 to 1, which would drag tool 0 into
// tool_ordering. Filament 1 is logical slot 0, which pem routes to head 0 -- the head every
// mode here declares Primary -- so Print::validate() accepts the plate.
static void all_regions_on_filament(DynamicPrintConfig &config, int filament_1based)
{
    for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id",
                             "sparse_infill_filament_id", "internal_solid_filament_id",
                             "top_surface_filament_id", "bottom_surface_filament_id" })
        config.set_deserialize_strict({ { key, std::to_string(filament_1based) } });
}


// {imex_mode} is the plate's active mode, {imex_mode_index} its position in imex_mode_names and
// {imex_mode_gcode} the raw script at that same position. Two modes are exercised because a
// single one cannot tell a resolved index apart from a constant: `copy` sits at 1 and `iq-copy`
// at 2, so an index that stopped tracking imex_mode_names fails at least one of them.
TEST_CASE("IMEX mode placeholders resolve to the active mode's name, index and script",
          "[ImexModeGcode][IMEX]")
{
    auto [mode, mode_index, mode_script] = GENERATE(table<std::string, int, std::string>({
        { "copy",    1, kCopyScript },
        { "iq-copy", 2, kQuadScript },
    }));
    INFO("active mode: " << mode);

    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    set_mode_gcodes(config, { "", kCopyScript, kQuadScript });
    config.set_deserialize_strict({
        { "imex_parallel_mode",  mode },
        { "machine_start_gcode", kReportStartGcode },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(has_line(gcode, ";IMEX_MODE:" + mode));
    CHECK(has_line(gcode, ";IMEX_MODE_INDEX:" + std::to_string(mode_index)));
    CHECK(has_line(gcode, ";IMEX_MODE_GCODE:" + mode_script));
    // The script for the active mode -- and only that one -- reaches the file.
    CHECK(has_line(gcode, mode_script));
    CHECK_FALSE(has_line(gcode, mode == "copy" ? kQuadScript : kCopyScript));
}

// The mode script is a G-code template, not a literal: it goes through
// placeholder_parser_process like machine_start_gcode does. Because the three IMEX placeholders
// are set before that call, the script can also read its own mode -- which is what
// distinguishes "the script is processed" from "the script is processed too early to see them".
TEST_CASE("A placeholder inside an IMEX mode's G-code script is expanded",
          "[ImexModeGcode][IMEX]")
{
    const std::string templ = "SET_DUAL_CARRIAGE MODE={imex_mode} INDEX={imex_mode_index}"
                              " TEMP={nozzle_temperature_initial_layer[0]}";

    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    set_mode_gcodes(config, { "", templ, kQuadScript });
    config.set_deserialize_strict({
        { "imex_parallel_mode",               "copy" },
        // The multi-extruder normalization collapses the per-filament temperature vector to a
        // single value, so [0] is this literal 200 rather than a per-slot default.
        { "nozzle_temperature_initial_layer", "200" },
        { "machine_start_gcode",              ";START\n" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(has_line(gcode, "SET_DUAL_CARRIAGE MODE=copy INDEX=1 TEMP=200"));
    // If the script were written straight to the file the braces would survive verbatim.
    CHECK_FALSE(has_line(gcode, templ));
}

// The ordering guarantee, and the reason the placeholder block sits where it does: the mode
// script is processed BEFORE machine_start_gcode, so a {global} the mode declares is in scope
// for machine_start_gcode. Reorder the two and machine_start_gcode references an undefined
// variable, which the placeholder parser raises as an error out of the export -- so a
// regression here fails this test whether the export throws or merely writes nothing useful.
TEST_CASE("A global declared in the IMEX mode G-code is visible to machine_start_gcode",
          "[ImexModeGcode][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    set_mode_gcodes(config, { "", std::string("{global imex_carriage_count = 2}") + kCopyScript, kQuadScript });
    config.set_deserialize_strict({
        { "imex_parallel_mode",  "copy" },
        { "machine_start_gcode", ";IMEX_CARRIAGES:{imex_carriage_count}\n" },
    });

    const std::string gcode = slice({ cube(20) }, config);

    // The value is the mode script's, not a default: nothing else in this config defines it.
    const std::size_t start_gcode_at = find_exact_line(gcode, ";IMEX_CARRIAGES:2");
    REQUIRE(start_gcode_at != std::string::npos);

    // ...and the two land in the file in that same order. Ordering is the contract here, so it
    // is asserted directly rather than left implicit in the global resolving.
    const std::size_t mode_script_at = find_exact_line(gcode, kCopyScript);
    REQUIRE(mode_script_at != std::string::npos);
    CHECK(mode_script_at < start_gcode_at);
}

// Primary is single-carriage printing: it names a real mode (position 0 of imex_mode_names, not
// the not-found fallback) but carries no script, so the export must add nothing. This is what
// keeps the mode machinery out of the way of an IMEX printer that is not printing in parallel.
TEST_CASE("Primary-mode IMEX prints emit no mode G-code", "[ImexModeGcode][IMEX]")
{
    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    set_mode_gcodes(config, { "", kCopyScript, kQuadScript });
    config.set_deserialize_strict({
        { "imex_parallel_mode",  "primary" },
        { "machine_start_gcode", kReportStartGcode },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(has_line(gcode, ";IMEX_MODE:primary"));
    CHECK(has_line(gcode, ";IMEX_MODE_INDEX:0"));
    CHECK(has_line(gcode, ";IMEX_MODE_GCODE:"));
    CHECK_FALSE(has_line(gcode, kCopyScript));
    CHECK_FALSE(has_line(gcode, kQuadScript));
}

// A plate stores its IMEX mode as the mode's *name* and the exporter resolves that name against
// the printer's imex_mode_names at slice time, so the two drift apart whenever the mode is
// renamed or deleted after a plate was set to it, or the project is opened against a printer
// preset that names its modes differently. A name matching no row must be treated as Primary --
// not as a parallel mode whose every name-keyed lookup happens to come back empty, which is how
// it used to behave: no mode script (there is none to find), no active-tool roster, and yet the
// parallel branches taken all the way through the export.
//
// Primary is a row like any other, so falling back to it means running its script too; a
// non-empty script is used here so "fell back to Primary" is distinguishable from "resolved
// nothing and emitted nothing".
TEST_CASE("An IMEX mode the printer no longer defines falls back to Primary",
          "[ImexModeGcode][IMEX]")
{
    static const char *kPrimaryScript = "SET_DUAL_CARRIAGE MODE=PRIMARY";

    DynamicPrintConfig config = multifilament_config(7);
    imex_7x4_printer(config);
    all_regions_on_filament(config, 1);
    set_mode_gcodes(config, { kPrimaryScript, kCopyScript, kQuadScript });
    config.set_deserialize_strict({
        // A name no row in imex_mode_names carries -- what `copy` becomes once it is renamed.
        { "imex_parallel_mode",  "copy-renamed" },
        { "machine_start_gcode", kReportStartGcode },
    });

    const std::string gcode = slice({ cube(20) }, config);

    // The placeholders report the mode the export actually ran, so the stale name must not
    // survive into them -- a mode script keyed on {imex_mode} would otherwise set the printer
    // up for a mode the slicer did not slice for.
    CHECK(has_line(gcode, ";IMEX_MODE:primary"));
    CHECK(has_line(gcode, ";IMEX_MODE_INDEX:0"));
    CHECK(has_line(gcode, ";IMEX_MODE_GCODE:" + std::string(kPrimaryScript)));

    // Primary's own script runs; neither parallel mode's does.
    CHECK(has_line(gcode, kPrimaryScript));
    CHECK_FALSE(has_line(gcode, kCopyScript));
    CHECK_FALSE(has_line(gcode, kQuadScript));
}

// The whole block is gated on is_imex. A printer preset can carry a filled-in mode table and
// still not be an IMEX machine -- and a plate config can still carry a stale imex_parallel_mode
// -- so with the flag off the placeholders stay empty and no mode script is injected. Run on an
// ordinary single-extruder printer on purpose: the guard is one boolean, and the multi-head
// geometry the other cases need would only add ways for this one to fail for another reason.
TEST_CASE("A printer with IMEX disabled resolves the mode placeholders to nothing",
          "[ImexModeGcode][IMEX]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    set_mode_gcodes(config, { "", kCopyScript, kQuadScript });
    config.set_deserialize_strict({
        { "imex_mode_names",        "primary;copy;iq-copy" },
        { "imex_mode_active_tools", "0:P;0:P,1:C;0:P,1:C,2:C,3:C" },
        { "is_imex",                "0" },
        { "imex_parallel_mode",     "copy" },
        { "skirt_loops",            "0" },
        { "brim_type",              "no_brim" },
        { "gcode_flavor",           "klipper" },
        { "machine_start_gcode",    kReportStartGcode },
    });

    const std::string gcode = slice({ cube(20) }, config);

    CHECK(has_line(gcode, ";IMEX_MODE:"));
    CHECK(has_line(gcode, ";IMEX_MODE_INDEX:0"));
    CHECK(has_line(gcode, ";IMEX_MODE_GCODE:"));
    CHECK_FALSE(has_line(gcode, kCopyScript));
    CHECK_FALSE(has_line(gcode, kQuadScript));
}

// The placeholders are set unconditionally, ahead of the is_imex check, so they are defined for
// every printer. A single-extruder preset that mentions {imex_mode} must expand it to an empty
// string rather than fail the export on an unknown variable.
TEST_CASE("IMEX mode placeholders are defined on an ordinary single-extruder printer",
          "[ImexModeGcode][IMEX]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "skirt_loops",         "0" },
        { "brim_type",           "no_brim" },
        { "gcode_flavor",        "klipper" },
        { "machine_start_gcode", kReportStartGcode },
    });

    CHECK(has_line(gcode, ";IMEX_MODE:"));
    CHECK(has_line(gcode, ";IMEX_MODE_INDEX:0"));
    CHECK(has_line(gcode, ";IMEX_MODE_GCODE:"));
}
