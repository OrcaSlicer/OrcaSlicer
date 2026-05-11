// Unit test for Layer::choose_ironing_extruder — the routing decision that
// drives ironing's extruder selection.
//
// The interaction we care about: when surface_wall_override_filament_target ∈
// {Surfaces, Both}, top/bottom external solid surfaces are routed through
// surface_wall_override_filament; the iron pass over those surfaces must follow the same
// routing so the smear comes from the same nozzle/material. Indirectly this
// also means the per-filament filament_ironing_* settings (looked up by
// extruder index inside make_ironing) come from the outer-wall filament.
//
// We test the static helper directly rather than running a full slice + walking
// extrusions: it's the single point where the choice is made, and a unit test
// keeps the assertion specific to *this* decision instead of getting tangled in
// the rest of the slicer pipeline.

#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

PrintRegionConfig make_region_config(IroningType type,
                                     int surface_wall_override_filament,
                                     SurfaceWallOverrideFilamentTarget target,
                                     int solid_infill_filament = 1,
                                     int top_shell_layers = 3)
{
    PrintRegionConfig cfg;  // default-constructs all fields from the static cache
    cfg.ironing_type.value                          = type;
    cfg.surface_wall_override_filament.value                   = surface_wall_override_filament;
    cfg.surface_wall_override_filament_target.value            = target;
    cfg.top_surface_filament_id.value               = solid_infill_filament;
    cfg.internal_solid_filament_id.value            = solid_infill_filament;
    cfg.bottom_surface_filament_id.value            = solid_infill_filament;
    cfg.outer_wall_filament_id.value                = 1;
    cfg.inner_wall_filament_id.value                = 1;
    cfg.sparse_infill_filament_id.value             = 1;
    cfg.top_shell_layers.value                      = top_shell_layers;
    cfg.bottom_shell_layers.value                   = 1;
    cfg.wall_loops.value                            = 2;
    return cfg;
}

} // namespace

TEST_CASE("Ironing routes through surface_wall_override_filament when surfaces option is on",
          "[surface_wall_override_filament][ironing][surfaces]")
{
    SECTION("TopSurfaces ironing + surfaces option on -> surface_wall_override_filament")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 2);
        // is_topmost_layer doesn't matter for TopSurfaces ironing.
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/true)  == 2);
    }

    SECTION("AllSolid ironing + surfaces option on -> surface_wall_override_filament regardless of layer")
    {
        const auto cfg = make_region_config(IroningType::AllSolid,
                                            /*surface_wall_override_filament=*/3,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 3);
    }

    SECTION("TopmostOnly ironing + surfaces option on, on the topmost layer -> surface_wall_override_filament")
    {
        const auto cfg = make_region_config(IroningType::TopmostOnly,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/true) == 2);
    }
}

TEST_CASE("Ironing falls back to solid_infill_filament when surfaces option is off",
          "[surface_wall_override_filament][ironing][surfaces]")
{
    SECTION("Surfaces option off -> solid_infill_filament even though surface_wall_override_filament is set")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Walls,
                                            /*solid_infill_filament=*/1);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 1);
    }

    SECTION("Surface / outer wall override unset -> solid_infill_filament regardless of surfaces option")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*surface_wall_override_filament=*/0,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both,
                                            /*solid_infill_filament=*/1);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 1);
    }
}

TEST_CASE("Ironing returns -1 when ironing is disabled or gating prevents it",
          "[surface_wall_override_filament][ironing][surfaces]")
{
    SECTION("NoIroning -> disabled regardless of other options")
    {
        const auto cfg = make_region_config(IroningType::NoIroning,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == -1);
    }

    SECTION("TopSurfaces with top_shell_layers=0 (and not in spiral mode) -> disabled")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both,
                                            /*solid_infill_filament=*/1,
                                            /*top_shell_layers=*/0);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == -1);
    }

    SECTION("TopmostOnly on a non-topmost layer -> disabled")
    {
        const auto cfg = make_region_config(IroningType::TopmostOnly,
                                            /*surface_wall_override_filament=*/2,
                                            /*target=*/SurfaceWallOverrideFilamentTarget::Both);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == -1);
    }
}
