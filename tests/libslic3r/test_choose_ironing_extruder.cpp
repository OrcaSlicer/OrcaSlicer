// Unit test for Layer::choose_ironing_extruder — the routing decision that
// drives ironing's extruder selection.
//
// Before this helper was extracted, the gating was a 5-line nested conditional
// inlined at the top of make_ironing(), with no isolated test coverage. Pulling
// it out lets us assert each gate (NoIroning, top-shell-layers cutoff, spiral
// mode, topmost-layer requirement, AllSolid bypass) without spinning up the
// whole slicing pipeline. Pure refactor — no behavior change vs the prior
// inline form.

#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

PrintRegionConfig make_region_config(IroningType type,
                                     int top_surface_filament_id = 1,
                                     int top_shell_layers        = 3,
                                     int bottom_shell_layers     = 1)
{
    PrintRegionConfig cfg;  // default-constructs all fields from the static cache
    cfg.ironing_type.value            = type;
    cfg.top_surface_filament_id.value = top_surface_filament_id;
    cfg.top_shell_layers.value        = top_shell_layers;
    cfg.bottom_shell_layers.value     = bottom_shell_layers;
    cfg.outer_wall_filament_id.value  = 1;
    cfg.wall_loops.value              = 2;
    return cfg;
}

} // namespace

TEST_CASE("choose_ironing_extruder routes enabled ironing to top_surface_filament_id",
          "[ironing]")
{
    SECTION("AllSolid -> top_surface_filament_id regardless of layer position")
    {
        const auto cfg = make_region_config(IroningType::AllSolid,
                                            /*top_surface_filament_id=*/2);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 2);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/true)  == 2);
    }

    SECTION("TopSurfaces with top_shell_layers > 0 -> top_surface_filament_id")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*top_surface_filament_id=*/3,
                                            /*top_shell_layers=*/2);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == 3);
    }

    SECTION("TopSurfaces with top_shell_layers=0 BUT spiral mode + bottom_shell_layers>1 -> enabled")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*top_surface_filament_id=*/1,
                                            /*top_shell_layers=*/0,
                                            /*bottom_shell_layers=*/2);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/true, /*topmost=*/false) == 1);
    }

    SECTION("TopmostOnly + topmost layer -> top_surface_filament_id")
    {
        const auto cfg = make_region_config(IroningType::TopmostOnly,
                                            /*top_surface_filament_id=*/4);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/true) == 4);
    }
}

TEST_CASE("choose_ironing_extruder returns -1 when ironing is disabled or gating prevents it",
          "[ironing]")
{
    SECTION("NoIroning -> -1 even with otherwise-permissive config")
    {
        const auto cfg = make_region_config(IroningType::NoIroning);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/true) == -1);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/true,  /*topmost=*/true) == -1);
    }

    SECTION("TopSurfaces with top_shell_layers=0 (and not spiral) -> -1")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*top_surface_filament_id=*/1,
                                            /*top_shell_layers=*/0,
                                            /*bottom_shell_layers=*/0);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == -1);
    }

    SECTION("TopSurfaces, spiral mode, but bottom_shell_layers=1 -> -1 (spiral path requires >1)")
    {
        const auto cfg = make_region_config(IroningType::TopSurfaces,
                                            /*top_surface_filament_id=*/1,
                                            /*top_shell_layers=*/0,
                                            /*bottom_shell_layers=*/1);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/true, /*topmost=*/false) == -1);
    }

    SECTION("TopmostOnly on a non-topmost layer -> -1")
    {
        const auto cfg = make_region_config(IroningType::TopmostOnly);
        REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral=*/false, /*topmost=*/false) == -1);
    }
}
