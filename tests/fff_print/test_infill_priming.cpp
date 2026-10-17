#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Print.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;

namespace {
struct PrimingAudit
{
    size_t reduced        = 0;
    size_t full           = 0;
    size_t primed         = 0;
    size_t tower_moves    = 0;
    size_t priming_passes = 0;
    double xy_extrusion   = 0.;
};

PrimingAudit audit_priming(const std::string& gcode, const PrintConfig& config, bool shared_nozzle)
{
    PrimingAudit audit;
    GCodeReader reader;
    reader.apply_config(config);
    std::string role;
    bool awaiting_priming    = false;
    const double full_length = 140. / (M_PI * 1.75 * 1.75 / 4.);
    reader.parse_buffer(gcode, [&](GCodeReader& state, const GCodeReader::GCodeLine& line) {
        const std::string comment(line.comment());
        if (comment == " PURGING FINISHED")
            ++audit.priming_passes;
        if (shared_nozzle && comment.rfind(" REMAINING_PURGE ", 0) == 0) {
            const double remaining = std::stod(comment.substr(comment.find("REMAINING_PURGE ") + 16));
            REQUIRE(remaining >= 0.);
            REQUIRE(remaining <= full_length + 0.01);
            awaiting_priming = remaining < full_length - 0.01;
            audit.reduced += awaiting_priming;
            audit.full += !awaiting_priming;
        }
        if (comment.substr(0, 5) == "TYPE:")
            role = comment.substr(5);
        else if (comment.substr(0, 10) == " FEATURE: ")
            role = comment.substr(10);
        if (!line.extruding(state) || !(line.has_x() || line.has_y()))
            return;
        audit.tower_moves += role == "Prime tower";
        audit.xy_extrusion += line.dist_E(state);
        if (awaiting_priming) {
            CHECK(role == "Sparse infill");
            ++audit.primed;
            awaiting_priming = false;
        }
    });
    return audit;
}
} // namespace

TEST_CASE("Infill priming works without a prime tower", "[InfillPriming]")
{
    const bool shared_nozzle = GENERATE(false, true);
    const int mode           = GENERATE(0, 1, 2, 3, 4);
    // off, on, no sparse infill, soluble transition, support-material transition
    CAPTURE(shared_nozzle, mode);
    DynamicPrintConfig config = Test::multifilament_config(2);
    config.set_deserialize_strict({
        {"enable_prime_tower", false},
        {"single_extruder_multi_material", shared_nozzle},
        {"nozzle_diameter", shared_nozzle ? "0.4" : "0.4,0.4"},
        {"filament_diameter", "1.75,1.75"},
        {"flush_volumes_matrix", "0,140,140,0"},
        {"flush_multiplier", "1"},
        {"grab_length", "0"},
        {"flush_into_infill", mode != 0},
        {"sparse_infill_density", mode == 2 ? 0 : 20},
        {"prime_volume", 35},
        {"filament_prime_volume", "35,35"},
        {"filament_map", shared_nozzle ? "1,1" : "1,2"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"change_filament_gcode",
         "{if previous_extruder >= 0 && previous_extruder != next_extruder}; REMAINING_PURGE {flush_length}\n{endif}T[next_extruder]\n"},
    });
    config.set_num_extruders(shared_nozzle ? 1 : 2);
    Test::size_flush_to_nozzles(config);
    if (mode == 3)
        config.set_deserialize_strict("filament_soluble", "0,1");
    if (mode == 4)
        config.set_deserialize_strict("filament_is_support", "0,1");
    Print print;
    print.is_BBL_printer() = false;
    Model model;
    Test::init_print({Test::cube(20), Test::cube(20)}, print, model, config);
    model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    const PrimingAudit first = audit_priming(Test::gcode(print), print.config(), shared_nozzle);
    CHECK(first.tower_moves == 0);
    CHECK((first.priming_passes > 0) == (mode == 1));
    if (shared_nozzle) {
        CHECK(first.full > 0); // Solid first layers cannot absorb sparse-infill priming.
        CHECK((first.reduced > 0) == (mode == 1));
        CHECK(first.primed == first.reduced);
    }
    if (mode == 1) {
        const PrimingAudit second = audit_priming(Test::gcode(print), print.config(), shared_nozzle);
        CHECK(second.priming_passes == first.priming_passes);
        CHECK(second.reduced == first.reduced);
        CHECK(second.primed == first.primed);
        CHECK(second.xy_extrusion == Catch::Approx(first.xy_extrusion).margin(0.01));
    }
}
