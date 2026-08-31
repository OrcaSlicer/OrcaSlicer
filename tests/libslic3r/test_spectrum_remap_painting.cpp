#include <catch2/catch_all.hpp>

#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

ModelVolume *add_painted_cube(Model &model, const std::vector<std::pair<int, int>> &facet_states)
{
    ModelObject *obj = model.add_object("remap_paint_cube", "", make_cube(20., 20., 20.));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector sel(vol->mesh());
    for (const auto &facet_state : facet_states)
        sel.set_facet(facet_state.first, EnforcerBlockerType(facet_state.second));
    REQUIRE(vol->mmu_segmentation_facets.set(sel));
    return vol;
}

} // namespace

TEST_CASE("remap_painting identity keeps Mix 5 and Mix 6", "[spectrum_remap_painting]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    const auto  &src  = vol->mmu_segmentation_facets.get_data();
    const auto  &its  = vol->mesh().its;

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::remap_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);

    REQUIRE(result.used_states.size() > 6);
    REQUIRE(result.used_states[5]);
    REQUIRE(result.used_states[6]);
    REQUIRE_FALSE(result.bitstream.empty());
}

TEST_CASE("remap_painting far translation does not flood Mix IDs", "[spectrum_remap_painting]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    const auto  &src  = vol->mmu_segmentation_facets.get_data();
    const auto  &its  = vol->mesh().its;
    const Transform3d xf = Geometry::translation_transform(Vec3d(1000., 0., 0.));

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::remap_painting(
        its, src, its, xf, std::nullopt);

    REQUIRE(result.bitstream.empty());
}
