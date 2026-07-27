#include <catch2/catch_all.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

PrintRegionConfig ironing_config(IroningType type,
                                 int top_surface_filament_id = 1,
                                 int top_shell_layers        = 3,
                                 int bottom_shell_layers     = 1)
{
    PrintRegionConfig cfg;
    cfg.ironing_type.value            = type;
    cfg.top_surface_filament_id.value = top_surface_filament_id;
    cfg.top_shell_layers.value        = top_shell_layers;
    cfg.bottom_shell_layers.value     = bottom_shell_layers;
    cfg.outer_wall_filament_id.value  = 1;
    cfg.wall_loops.value              = 2;
    return cfg;
}

} // namespace

TEST_CASE("Ironing an all-solid region uses the top surface filament on every layer", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::AllSolid, /*top_surface_filament_id=*/2);
    const bool is_topmost_layer = GENERATE(false, true);
    CAPTURE(is_topmost_layer);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, is_topmost_layer) == 2);
}

TEST_CASE("Ironing top surfaces uses the top surface filament when the region has top shells", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::TopSurfaces,
                                                 /*top_surface_filament_id=*/3,
                                                 /*top_shell_layers=*/2);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == 3);
}

TEST_CASE("Ironing top surfaces without top shells needs spiral mode and more than one bottom shell", "[Fill]")
{
    const PrintRegionConfig one_bottom_shell = ironing_config(IroningType::TopSurfaces,
                                                              /*top_surface_filament_id=*/1,
                                                              /*top_shell_layers=*/0,
                                                              /*bottom_shell_layers=*/1);
    const PrintRegionConfig two_bottom_shells = ironing_config(IroningType::TopSurfaces,
                                                               /*top_surface_filament_id=*/1,
                                                               /*top_shell_layers=*/0,
                                                               /*bottom_shell_layers=*/2);

    REQUIRE(Layer::choose_ironing_extruder(two_bottom_shells, /*spiral_mode=*/true, /*is_topmost_layer=*/false) == 1);
    REQUIRE(Layer::choose_ironing_extruder(one_bottom_shell, /*spiral_mode=*/true, /*is_topmost_layer=*/false) == -1);
    REQUIRE(Layer::choose_ironing_extruder(two_bottom_shells, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == -1);
}

TEST_CASE("Ironing the topmost surface only applies to the topmost layer", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::TopmostOnly, /*top_surface_filament_id=*/4);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/true) == 4);
    REQUIRE(Layer::choose_ironing_extruder(cfg, /*spiral_mode=*/false, /*is_topmost_layer=*/false) == -1);
}

TEST_CASE("A region with ironing turned off is never ironed", "[Fill]")
{
    const PrintRegionConfig cfg = ironing_config(IroningType::NoIroning);
    const bool spiral_mode      = GENERATE(false, true);
    CAPTURE(spiral_mode);
    REQUIRE(Layer::choose_ironing_extruder(cfg, spiral_mode, /*is_topmost_layer=*/true) == -1);
}
