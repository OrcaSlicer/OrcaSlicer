#include <catch2/catch_all.hpp>

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentPicPrint.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

ColorRGB decode_hex_or_fail(const std::string &hex)
{
    ColorRGB c;
    REQUIRE(decode_color(hex, c));
    return c;
}

std::vector<ColorRGB> panchroma_physicals()
{
    return {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
    };
}

void put_rgb(std::vector<std::uint8_t> &rgb, int w, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 3;
    rgb[i]     = r;
    rgb[i + 1] = g;
    rgb[i + 2] = b;
}

std::vector<std::uint8_t> two_colour_8x2()
{
    std::vector<std::uint8_t> rgb(size_t(8) * 2 * 3, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (x < 4)
                put_rgb(rgb, 8, x, y, 0x08, 0xAB, 0xFB);
            else
                put_rgb(rgb, 8, x, y, 0xD9, 0x3B, 0x90);
        }
    }
    return rgb;
}

size_t unique_dest_count(const SpectrumPicPrintPlan &plan)
{
    std::set<unsigned> dests(plan.dest_id.begin(), plan.dest_id.end());
    return dests.size();
}

} // namespace

TEST_CASE("PicPrint 8x2 two-colour buffer clusters and samples", "[spectrum_picprint]")
{
    const std::vector<std::uint8_t> rgb  = two_colour_8x2();
    const std::vector<ColorRGB>     phys = panchroma_physicals();
    const SpectrumPicPrintPlan plan = plan_spectrum_picprint(rgb.data(), 8, 2, phys, 4);

    REQUIRE(plan.valid);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.width == 8);
    REQUIRE(plan.height == 2);
    REQUIRE(plan.dest_id.size() == size_t(16));

    SECTION("cluster_count is 2")
    {
        REQUIRE(plan.cluster_count == 2);
        REQUIRE(unique_dest_count(plan) == 2);
    }

    SECTION("sample left vs right dest IDs differ")
    {
        const unsigned left  = spectrum_picprint_sample_facet(plan, 0.2, 0.5);
        const unsigned right = spectrum_picprint_sample_facet(plan, 0.8, 0.5);
        REQUIRE(left != 0);
        REQUIRE(right != 0);
        REQUIRE(left != right);
    }

    SECTION("mix defs ZR grammar or both Physical")
    {
        const unsigned left  = spectrum_picprint_sample_facet(plan, 0.2, 0.5);
        const unsigned right = spectrum_picprint_sample_facet(plan, 0.8, 0.5);
        REQUIRE(left != right);
        if (plan.mixed_filament_definitions.empty()) {
            REQUIRE(left <= 4);
            REQUIRE(right <= 4);
        } else {
            MixedFilamentManager mgr;
            mgr.load_definitions(plan.mixed_filament_definitions);
            REQUIRE(mgr.enabled_count() == plan.mix_count);
            REQUIRE(mgr.serialize_definitions() == plan.mixed_filament_definitions);
            REQUIRE_THAT(plan.mixed_filament_definitions, Catch::Matchers::ContainsSubstring(","));
        }
    }
}

TEST_CASE("PicPrint Y-flip samples bottom image row at v=0", "[spectrum_picprint]")
{
    // 2x2: top row (y=0) cyan, bottom row (y=1) magenta. pixel_y uses (1-v).
    std::vector<std::uint8_t> rgb(size_t(2) * 2 * 3, 0);
    put_rgb(rgb, 2, 0, 0, 0x08, 0xAB, 0xFB);
    put_rgb(rgb, 2, 1, 0, 0x08, 0xAB, 0xFB);
    put_rgb(rgb, 2, 0, 1, 0xD9, 0x3B, 0x90);
    put_rgb(rgb, 2, 1, 1, 0xD9, 0x3B, 0x90);

    const SpectrumPicPrintPlan plan = plan_spectrum_picprint(rgb.data(), 2, 2, panchroma_physicals(), 4);
    REQUIRE(plan.valid);
    REQUIRE(plan.width == 2);
    REQUIRE(plan.height == 2);

    const unsigned at_v0 = spectrum_picprint_sample_facet(plan, 0.5, 0.0);
    const unsigned at_v1 = spectrum_picprint_sample_facet(plan, 0.5, 1.0);
    REQUIRE(at_v0 != 0);
    REQUIRE(at_v1 != 0);
    REQUIRE(at_v0 != at_v1);
    REQUIRE(at_v0 == plan.dest_id[size_t(1) * 2 + 0]); // bottom row y=1
    REQUIRE(at_v1 == plan.dest_id[0]);                 // top row y=0
}

TEST_CASE("PicPrint collapse unique mix recipes within persist 32", "[spectrum_picprint]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    std::vector<std::string> rows;
    static const int k_pairs[][2]  = {{1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4}};
    static const int k_ratios[][2] = {
        {1, 1}, {2, 1}, {1, 2}, {3, 1}, {1, 3}, {4, 1}, {1, 4}, {5, 1}, {1, 5}, {3, 2},
    };
    for (const auto &pair : k_pairs) {
        for (const auto &ratio : k_ratios) {
            MixedFilament mf;
            mf.component_a = unsigned(pair[0]);
            mf.component_b = unsigned(pair[1]);
            mf.ratio_a     = ratio[0];
            mf.ratio_b     = ratio[1];
            mf.enabled     = true;
            MixedFilamentManager mgr;
            mgr.load_definitions(std::to_string(mf.component_a) + "," + std::to_string(mf.component_b) + ",1," +
                                 std::to_string(mf.ratio_a) + "," + std::to_string(mf.ratio_b));
            const std::string row = mgr.serialize_definitions();
            if (!row.empty())
                rows.push_back(row);
        }
    }
    REQUIRE(rows.size() > 28);

    const std::string collapsed = spectrum_collapse_mix_recipe_rows(rows, phys, 4);
    MixedFilamentManager mgr;
    mgr.load_definitions(collapsed);
    REQUIRE(mgr.enabled_count() >= 1);
    REQUIRE(4 + mgr.enabled_count() <= SPECTRUM_PAINT_ID_PERSIST_CAP);
    REQUIRE(mgr.serialize_definitions() == collapsed);
}

TEST_CASE("PicPrint keeps existing project mix rows", "[spectrum_picprint]")
{
    MixedFilamentManager seed;
    seed.load_definitions("1,2,1,1,1");
    const std::string existing = seed.serialize_definitions();
    REQUIRE_FALSE(existing.empty());

    const std::vector<std::uint8_t> rgb = two_colour_8x2();
    const SpectrumPicPrintPlan plan = plan_spectrum_picprint(rgb.data(), 8, 2, panchroma_physicals(), 4,
                                                             SPECTRUM_PAINT_ID_PERSIST_CAP, existing);
    REQUIRE(plan.valid);
    MixedFilamentManager out;
    out.load_definitions(plan.mixed_filament_definitions);
    REQUIRE(out.enabled_count() >= 1);
    REQUIRE(out.enabled_count() == plan.mix_count);
    REQUIRE_THAT(plan.mixed_filament_definitions, Catch::Matchers::ContainsSubstring("1,2,"));

    const unsigned left  = spectrum_picprint_sample_facet(plan, 0.2, 0.5);
    const unsigned right = spectrum_picprint_sample_facet(plan, 0.8, 0.5);
    REQUIRE(left != 0);
    REQUIRE(right != 0);
    REQUIRE(left != right);
}

TEST_CASE("PicPrint world XY uses passed bbox min/max", "[spectrum_picprint]")
{
    using Catch::Matchers::WithinAbs;

    BoundingBoxf3 bb;
    bb.min = Vec3d(0., 0., 0.);
    bb.max = Vec3d(10., 10., 10.);

    SECTION("plan fixture bbox (0,0)-(10,10)")
    {
        double u = -1.;
        double v = -1.;
        REQUIRE(spectrum_picprint_world_to_uv(Vec3d(2., 5., 0.), bb, u, v));
        REQUIRE_THAT(u, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(v, WithinAbs(0.5, 1e-12));
    }

    SECTION("non-zero origin is not x/extent")
    {
        BoundingBoxf3 shifted;
        shifted.min = Vec3d(5., 7., 0.);
        shifted.max = Vec3d(15., 17., 10.);
        double u = -1.;
        double v = -1.;
        REQUIRE(spectrum_picprint_world_to_uv(Vec3d(7., 12., 0.), shifted, u, v));
        REQUIRE_THAT(u, WithinAbs(0.2, 1e-12));
        REQUIRE_THAT(v, WithinAbs(0.5, 1e-12));
        // origin-at-0 would be 7/10 and 12/10
        REQUIRE(std::abs(u - 0.7) > 1e-6);
        REQUIRE(std::abs(v - 1.2) > 1e-6);
    }

    SECTION("thin X extent refuses")
    {
        BoundingBoxf3 thin;
        thin.min = Vec3d(1., 0., 0.);
        thin.max = Vec3d(1.0 + 1e-7, 10., 0.);
        double u = 0.;
        double v = 0.;
        REQUIRE_FALSE(spectrum_picprint_world_to_uv(Vec3d(1., 5., 0.), thin, u, v));
    }

    SECTION("thin Y extent refuses")
    {
        BoundingBoxf3 thin;
        thin.min = Vec3d(0., 1., 0.);
        thin.max = Vec3d(10., 1.0 + 1e-7, 0.);
        double u = 0.;
        double v = 0.;
        REQUIRE_FALSE(spectrum_picprint_world_to_uv(Vec3d(5., 1., 0.), thin, u, v));
    }
}

TEST_CASE("PicPrint apply paints original facets from world XY", "[spectrum_picprint]")
{
    const std::vector<std::uint8_t> rgb  = two_colour_8x2();
    const SpectrumPicPrintPlan      plan = plan_spectrum_picprint(rgb.data(), 8, 2, panchroma_physicals(), 4);
    REQUIRE(plan.valid);

    Model model;
    ModelObject *obj = model.add_object("picprint_cube", "", make_cube(10., 10., 10.));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();

    BoundingBoxf3 bb = vol->mesh().bounding_box();
    REQUIRE(spectrum_picprint_apply_to_volume(*vol, Transform3d::Identity(), bb, plan));

    TriangleSelector sel(vol->mesh());
    sel.deserialize(vol->mmu_segmentation_facets.get_data());

    const indexed_triangle_set &its = vol->mesh().its;
    std::set<unsigned> painted;
    for (int i = 0; i < int(its.indices.size()); ++i) {
        const stl_triangle_vertex_indices &tri = its.indices[size_t(i)];
        Vec3d centroid = Vec3d::Zero();
        for (int k = 0; k < 3; ++k)
            centroid += its.vertices[size_t(tri(k))].cast<double>();
        centroid /= 3.0;
        double u = 0.;
        double v = 0.;
        REQUIRE(spectrum_picprint_world_to_uv(centroid, bb, u, v));
        const unsigned dest = spectrum_picprint_sample_facet(plan, u, v);
        REQUIRE(dest != 0);
        painted.insert(dest);
        REQUIRE(sel.num_facets(EnforcerBlockerType(dest)) > 0);
    }
    REQUIRE(painted.size() >= 2);

    SECTION("thin bbox refuses apply")
    {
        BoundingBoxf3 thin;
        thin.min = Vec3d(0., 0., 0.);
        thin.max = Vec3d(1e-7, 10., 10.);
        REQUIRE_FALSE(spectrum_picprint_apply_to_volume(*vol, Transform3d::Identity(), thin, plan));
    }
}
