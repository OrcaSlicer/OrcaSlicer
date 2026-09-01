#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

ModelVolume *add_painted_cube(Model &model, const std::vector<std::pair<int, int>> &facet_states, double size = 20.)
{
    ModelObject *obj = model.add_object("remap_paint_cube", "", make_cube(size, size, size));
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

bool its_contains_vertex_triple(const indexed_triangle_set &its, const Vec3f &a, const Vec3f &b, const Vec3f &c)
{
    auto same = [](const Vec3f &p, const Vec3f &q) {
        return (p - q).squaredNorm() < 1e-8f;
    };
    for (const Vec3i32 &f : its.indices) {
        const Vec3f &v0 = its.vertices[f(0)];
        const Vec3f &v1 = its.vertices[f(1)];
        const Vec3f &v2 = its.vertices[f(2)];
        int hits = 0;
        for (const Vec3f *want : {&a, &b, &c}) {
            if (same(v0, *want) || same(v1, *want) || same(v2, *want))
                ++hits;
        }
        if (hits == 3)
            return true;
    }
    return false;
}

void require_mix_not_on_cube_top(const TriangleSelector::TriangleSplittingData &result, const indexed_triangle_set &src_its)
{
    REQUIRE(src_its.indices.size() > 3);
    TriangleMesh mesh(src_its);
    TriangleSelector dest(mesh);
    dest.deserialize(result, false);
    const auto f5 = dest.get_facets(EnforcerBlockerType(5));
    const auto f6 = dest.get_facets(EnforcerBlockerType(6));
    const Vec3i32 &o2 = src_its.indices[2];
    const Vec3i32 &o3 = src_its.indices[3];
    const Vec3f &a2 = src_its.vertices[o2(0)];
    const Vec3f &b2 = src_its.vertices[o2(1)];
    const Vec3f &c2 = src_its.vertices[o2(2)];
    const Vec3f &a3 = src_its.vertices[o3(0)];
    const Vec3f &b3 = src_its.vertices[o3(1)];
    const Vec3f &c3 = src_its.vertices[o3(2)];
    REQUIRE_FALSE(its_contains_vertex_triple(f5, a2, b2, c2));
    REQUIRE_FALSE(its_contains_vertex_triple(f5, a3, b3, c3));
    REQUIRE_FALSE(its_contains_vertex_triple(f6, a2, b2, c2));
    REQUIRE_FALSE(its_contains_vertex_triple(f6, a3, b3, c3));
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

        TriangleSelector::RemapBuckets buckets;
        const auto t0 = std::chrono::steady_clock::now();
        const TriangleSelector::TriangleSplittingData result = TriangleSelector::remap_painting(
            its, src, its, Transform3d::Identity(), std::nullopt, nullptr, &buckets);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

        WARN("faces=" << n << " remap_ms=" << ms);
        if (n == 7920) {
            WARN("aabb_build_ms=" << buckets.aabb_build_ms
                 << " collect_ms=" << buckets.collect_ms
                 << " apply_ms=" << buckets.apply_ms);
        }
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

TEST_CASE("remap_painting mid-loop cancel returns empty", "[spectrum_remap_painting][spectrum_remap_cancel]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    const auto   source_painting = vol->mmu_segmentation_facets.get_data();
    const auto  &its = vol->mesh().its;
    REQUIRE_FALSE(source_painting.bitstream.empty());

    std::atomic<bool> canceled{false};
    TriangleSelector::TriangleSplittingData result;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        result = TriangleSelector::remap_painting(its, source_painting, its, Transform3d::Identity(), std::nullopt, &canceled);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    canceled.store(true, std::memory_order_relaxed);
    worker.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    WARN("cancel_join_ms=" << ms);
    if (ms > 500)
        WARN("cancel join exceeded 500 ms: " << ms);
    REQUIRE(ms < 2000);
    REQUIRE(result.bitstream.empty());
    const bool source_unchanged = vol->mmu_segmentation_facets.get_data() == source_painting;
    REQUIRE(source_unchanged);
}

TEST_CASE("remap_painting already canceled returns empty fast", "[spectrum_remap_painting][spectrum_remap_cancel]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    const auto  &src = vol->mmu_segmentation_facets.get_data();
    const auto  &its = vol->mesh().its;
    std::atomic<bool> canceled{true};

    const auto t0 = std::chrono::steady_clock::now();
    const TriangleSelector::TriangleSplittingData result = TriangleSelector::remap_painting(
        its, src, its, Transform3d::Identity(), std::nullopt, &canceled);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    WARN("already_canceled_ms=" << ms);
    REQUIRE(ms < 2000);
    REQUIRE(result.bitstream.empty());
}

TEST_CASE("Cut KeepPaint cancel joins quickly", "[spectrum_remap_painting][spectrum_remap_cancel]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    REQUIRE(vol->get_object() != nullptr);
    ModelObject *object = vol->get_object();

    const ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper |
                                           ModelObjectCutAttribute::KeepLower |
                                           ModelObjectCutAttribute::KeepPaint;
    std::atomic<bool> canceled{false};
    Cut cut(object, 0, Geometry::translation_transform(1.0 * Vec3d::UnitZ()), attrs);
    cut.set_cancel(&canceled);

    const auto t0 = std::chrono::steady_clock::now();
    std::thread worker([&]() { (void) cut.perform_with_plane(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    canceled.store(true, std::memory_order_relaxed);
    worker.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    WARN("cut_cancel_join_ms=" << ms);
    if (ms > 500)
        WARN("Cut cancel join exceeded 500 ms: " << ms);
    REQUIRE(ms < 2000);
}

TEST_CASE("cut_mesh source-face ids match indices and caps are -1", "[spectrum_remap_painting][spectrum_cut_provenance]")
{
    TriangleMesh cube = make_cube(20., 20., 20.);
    indexed_triangle_set upper, lower;
    std::vector<int> upper_ids, lower_ids;
    cut_mesh(cube.its, 1.0f, &upper, &lower, true, &upper_ids, &lower_ids);

    REQUIRE(upper_ids.size() == upper.indices.size());
    REQUIRE(lower_ids.size() == lower.indices.size());
    const int nsrc = int(cube.its.indices.size());
    for (int id : upper_ids) {
        REQUIRE(id >= -1);
        REQUIRE(id < nsrc);
    }
    for (int id : lower_ids) {
        REQUIRE(id >= -1);
        REQUIRE(id < nsrc);
    }
    bool upper_has_cap = false;
    for (int id : upper_ids) {
        if (id == -1) {
            upper_has_cap = true;
            break;
        }
    }
    bool lower_has_cap = false;
    for (int id : lower_ids) {
        if (id == -1) {
            lower_has_cap = true;
            break;
        }
    }
    REQUIRE(upper_has_cap);
    REQUIRE(lower_has_cap);
}

TEST_CASE("Cut KeepPaint inherit preserves Mix 5 and Mix 6", "[spectrum_remap_painting][spectrum_cut_provenance]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    REQUIRE(vol->get_object() != nullptr);
    ModelObject *object = vol->get_object();

    const ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper |
                                           ModelObjectCutAttribute::KeepLower |
                                           ModelObjectCutAttribute::KeepPaint;
    Cut cut(object, 0, Geometry::translation_transform(1.0 * Vec3d::UnitZ()), attrs);
    const ModelObjectPtrs &result = cut.perform_with_plane();
    REQUIRE_FALSE(result.empty());

    bool saw_mix5 = false;
    bool saw_mix6 = false;
    for (const ModelObject *obj : result) {
        REQUIRE(obj != nullptr);
        for (const ModelVolume *v : obj->volumes) {
            if (!v->is_model_part() || v->is_cut_connector())
                continue;
            REQUIRE(v->skip_restore_painting);
            const auto &data = v->mmu_segmentation_facets.get_data();
            if (data.used_states.size() > 6) {
                if (data.used_states[5])
                    saw_mix5 = true;
                if (data.used_states[6])
                    saw_mix6 = true;
            }
        }
    }
    REQUIRE(saw_mix5);
    REQUIRE(saw_mix6);
}

TEST_CASE("Cut KeepPaint inherit is faster than remap", "[spectrum_remap_painting][spectrum_cut_provenance]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    REQUIRE(vol->get_object() != nullptr);
    ModelObject *object = vol->get_object();
    const size_t n      = vol->mesh().its.indices.size();

    const ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper |
                                           ModelObjectCutAttribute::KeepLower |
                                           ModelObjectCutAttribute::KeepPaint;
    Cut cut(object, 0, Geometry::translation_transform(1.0 * Vec3d::UnitZ()), attrs);
    const auto t0 = std::chrono::steady_clock::now();
    const ModelObjectPtrs &result = cut.perform_with_plane();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    WARN("faces=" << n << " inherit_cut_ms=" << ms);
    REQUIRE_FALSE(result.empty());
    REQUIRE(ms < 1000);
}

TEST_CASE("classify_painting identity keeps Mix 5 and Mix 6", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    const auto  &src  = vol->mmu_segmentation_facets.get_data();
    const auto  &its  = vol->mesh().its;

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);

    REQUIRE(result.used_states.size() > 6);
    REQUIRE(result.used_states[5]);
    REQUIRE(result.used_states[6]);
    REQUIRE_FALSE(result.bitstream.empty());
    require_mix_not_on_cube_top(result, its);
}

TEST_CASE("classify_painting thin cube keeps Mix 5 and Mix 6", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}}, 2.);
    const auto  &src  = vol->mmu_segmentation_facets.get_data();
    const auto  &its  = vol->mesh().its;

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);

    REQUIRE(result.used_states.size() > 6);
    REQUIRE(result.used_states[5]);
    REQUIRE(result.used_states[6]);
    REQUIRE_FALSE(result.bitstream.empty());
    require_mix_not_on_cube_top(result, its);
}

TEST_CASE("classify_painting far translation does not flood Mix IDs", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelVolume *vol = add_painted_cube(model, {{0, 5}, {1, 6}});
    const auto  &src  = vol->mmu_segmentation_facets.get_data();
    const auto  &its  = vol->mesh().its;
    const Transform3d xf = Geometry::translation_transform(Vec3d(1000., 0., 0.));

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, xf, std::nullopt);

    REQUIRE(result.bitstream.empty());
}

TEST_CASE("classify_painting identity Mix 1 covers dest original faces", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelObject *obj = model.add_object("classify_paint_cube", "", make_cube(20., 20., 20.));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector src_sel(vol->mesh());
    const int n = int(vol->mesh().its.indices.size());
    REQUIRE(n > 0);
    for (int i = 0; i < n; ++i)
        src_sel.set_facet(i, EnforcerBlockerType(1));
    REQUIRE(vol->mmu_segmentation_facets.set(src_sel));

    const auto &src = vol->mmu_segmentation_facets.get_data();
    const auto &its = vol->mesh().its;
    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);
    REQUIRE_FALSE(result.bitstream.empty());

    TriangleSelector dest_sel(vol->mesh());
    dest_sel.deserialize(result, false);
    REQUIRE(dest_sel.num_facets(EnforcerBlockerType(1)) == n);
}

TEST_CASE("classify_painting 7920 faces is faster than a second", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    const auto  &src = vol->mmu_segmentation_facets.get_data();
    const auto  &its = vol->mesh().its;
    const size_t n   = its.indices.size();
    REQUIRE(n == 7920);

    const auto t0 = std::chrono::steady_clock::now();
    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    WARN("faces=" << n << " classify_ms=" << ms);
    REQUIRE(ms < 1000);
    REQUIRE(result.used_states.size() > 1);
    REQUIRE(result.used_states[1]);
    REQUIRE_FALSE(result.bitstream.empty());
}

TEST_CASE("classify_painting r=2 mm 7920 Mix 1 covers dest original faces", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelObject *obj = model.add_object("classify_paint_sphere_r2", "", make_sphere(2., 2. * PI / 90.));
    REQUIRE(obj != nullptr);
    obj->add_instance();
    REQUIRE_FALSE(obj->volumes.empty());
    ModelVolume *vol = obj->volumes.front();
    TriangleSelector src_sel(vol->mesh());
    const size_t n = vol->mesh().its.indices.size();
    REQUIRE(n == 7920);
    for (size_t i = 0; i < n; ++i)
        src_sel.set_facet(int(i), EnforcerBlockerType(1));
    REQUIRE(vol->mmu_segmentation_facets.set(src_sel));

    const auto &src = vol->mmu_segmentation_facets.get_data();
    const auto &its = vol->mesh().its;
    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt);
    REQUIRE_FALSE(result.bitstream.empty());

    TriangleSelector dest_sel(vol->mesh());
    dest_sel.deserialize(result, false);
    REQUIRE(dest_sel.num_facets(EnforcerBlockerType(1)) == int(n));
}

TEST_CASE("classify_painting already canceled returns empty", "[spectrum_remap_painting][spectrum_paint_field]")
{
    Model        model;
    ModelVolume *vol = add_fully_painted_sphere(model, 2. * PI / 90.);
    const auto  &src = vol->mmu_segmentation_facets.get_data();
    const auto  &its = vol->mesh().its;
    std::atomic<bool> canceled{true};

    const TriangleSelector::TriangleSplittingData result = TriangleSelector::classify_painting(
        its, src, its, Transform3d::Identity(), std::nullopt, &canceled);

    REQUIRE(result.bitstream.empty());
}
