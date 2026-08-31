#include <catch2/catch_all.hpp>

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/CutUtils.hpp"
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

ModelVolume *add_fully_painted_sphere(Model &model, double fa)
{
    ModelObject *obj = model.add_object("remap_paint_sphere", "", make_sphere(10., fa));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector sel(vol->mesh());
    const int n = int(vol->mesh().its.indices.size());
    REQUIRE(n > 0);
    for (int i = 0; i < n; ++i)
        sel.set_facet(i, EnforcerBlockerType(1));
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

TEST_CASE("remap_painting identity timing on fully painted sphere", "[spectrum_remap_painting][spectrum_remap_perf]")
{
    const double fa = GENERATE(2. * PI / 45., 2. * PI / 90., 2. * PI / 128.);
    DYNAMIC_SECTION("fa=" << fa)
    {
        Model        model;
        ModelVolume *vol = add_fully_painted_sphere(model, fa);
        const auto  &src = vol->mmu_segmentation_facets.get_data();
        const auto  &its = vol->mesh().its;
        const size_t n   = its.indices.size();

        const auto t0 = std::chrono::steady_clock::now();
        const TriangleSelector::TriangleSplittingData result = TriangleSelector::remap_painting(
            its, src, its, Transform3d::Identity(), std::nullopt);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

        WARN("faces=" << n << " remap_ms=" << ms);
        REQUIRE(result.used_states.size() > 1);
        REQUIRE(result.used_states[1]);
        REQUIRE_FALSE(result.bitstream.empty());
    }
}

TEST_CASE("Cut perform_with_plane KeepPaint vs not timing", "[spectrum_remap_painting][spectrum_remap_perf]")
{
    const bool keep_paint = GENERATE(false, true);
    DYNAMIC_SECTION("keep_paint=" << keep_paint)
    {
        Model        model;
        ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
        REQUIRE(vol->get_object() != nullptr);
        ModelObject *object = vol->get_object();
        const size_t n      = vol->mesh().its.indices.size();

        ModelObjectCutAttributes attrs =
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
        if (keep_paint)
            attrs = attrs | ModelObjectCutAttribute::KeepPaint;

        Cut cut(object, 0, Geometry::translation_transform(1.0 * Vec3d::UnitZ()), attrs);
        const auto t0 = std::chrono::steady_clock::now();
        const ModelObjectPtrs &result = cut.perform_with_plane();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

        WARN("faces=" << n << " keep_paint=" << (keep_paint ? 1 : 0) << " cut_ms=" << ms);
        REQUIRE_FALSE(result.empty());
    }
}

TEST_CASE("Cut KeepPaint remaps paint onto result volumes", "[spectrum_remap_painting]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 20.);
    REQUIRE(vol->get_object() != nullptr);
    ModelObject *object = vol->get_object();

    const ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper |
                                           ModelObjectCutAttribute::KeepLower |
                                           ModelObjectCutAttribute::KeepPaint;
    Cut cut(object, 0, Geometry::translation_transform(1.0 * Vec3d::UnitZ()), attrs);
    const ModelObjectPtrs &result = cut.perform_with_plane();
    REQUIRE_FALSE(result.empty());

    bool paint_survived = false;
    for (const ModelObject *obj : result) {
        REQUIRE(obj != nullptr);
        for (const ModelVolume *v : obj->volumes) {
            if (!v->is_model_part() || v->is_cut_connector())
                continue;
            if (!v->mmu_segmentation_facets.get_data().bitstream.empty()) {
                paint_survived = true;
                break;
            }
        }
        if (paint_survived)
            break;
    }
    REQUIRE(paint_survived);
}
