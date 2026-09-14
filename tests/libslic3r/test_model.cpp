#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;

TEST_CASE("Native textured imports retain closed geometry through color application", "[Model][ModelImport]")
{
    const std::string fixture = GENERATE("textured.glb", "outward-textured.glb", "transformed.glb", "nested-negative-nodes.glb");
    const std::string path = std::string(TEST_DATA_DIR) + "/model_artifact/" + fixture;
    CAPTURE(fixture);

    TexturedMesh original;
    REQUIRE(load_assimp_textured_model(path, original));
    // glTF is Y-up; the ordinary importer supplies Z-up geometry to Orca.
    // Its existing unit-conversion choice remains separate from orientation.
    for (auto& vertex : original.vertices)
        vertex = {vertex[0], -vertex[2], vertex[1]};
    indexed_triangle_set original_its;
    for (const auto& vertex : original.vertices)
        original_its.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : original.indices)
        original_its.indices.emplace_back(face[0], face[1], face[2]);
    REQUIRE(its_num_open_edges(original_its) == 0);
    const float expected_volume = std::abs(its_volume(original_its));
    REQUIRE(expected_volume > 0.f);

    Model model = Model::read_from_file(path);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const ModelVolume* volume = model.objects.front()->volumes.front();
    CHECK_THAT(volume->mesh().stats().volume, Catch::Matchers::WithinRel(expected_volume, 1e-5f));
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    REQUIRE(model.texture_mesh);
    const TexturedMesh& textured = *model.texture_mesh;

    // Winding repair must preserve each face's material and vertex/UV pairing.
    REQUIRE(textured.vertices.size() == original.vertices.size());
    REQUIRE(textured.uvs.size() == original.uvs.size());
    REQUIRE(textured.indices.size() == original.indices.size());
    CHECK(textured.material_ids == original.material_ids);
    CHECK(textured.material_texture_map == original.material_texture_map);
    REQUIRE(textured.textures.size() == original.textures.size());
    for (size_t i = 0; i < original.vertices.size(); ++i) {
        for (size_t axis = 0; axis < 3; ++axis)
            CHECK_THAT(textured.vertices[i][axis], Catch::Matchers::WithinAbs(original.vertices[i][axis], 1e-7));
        for (size_t axis = 0; axis < 2; ++axis)
            CHECK_THAT(textured.uvs[i][axis], Catch::Matchers::WithinAbs(original.uvs[i][axis], 1e-7));
    }
    for (size_t i = 0; i < original.indices.size(); ++i) {
        auto actual_face = textured.indices[i];
        auto original_face = original.indices[i];
        std::sort(actual_face.begin(), actual_face.end());
        std::sort(original_face.begin(), original_face.end());
        CHECK(actual_face == original_face);
    }

    const int unit_conversion = GENERATE(0, 1, 2);
    CAPTURE(unit_conversion);
    float unit_scale = 1.f;
    if (unit_conversion == 1) {
        model.convert_from_imperial_units(true);
        unit_scale = 25.4f;
    } else if (unit_conversion == 2) {
        model.convert_from_meters(true);
        unit_scale = 1000.f;
    }
    const Vec3d expected_size = volume->mesh().bounding_box().size();
    const Vec3d expected_offset = volume->get_offset();

    // The color dialog rebuilds the volume from texture_mesh. Validate that handoff,
    // as well as the initial geometry checked by the main window's cleanup step.
    PaintedMesh painted;
    REQUIRE(texture_to_painting(textured, painted));
    std::vector<FilamentMatch> matches;
    for (size_t i = 0; i < painted.cluster_colors.size(); ++i) {
        FilamentMatch match;
        match.cluster_index = int(i);
        match.filament_index = int(i);
        matches.push_back(match);
    }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *model.objects.front()->volumes.front()));
    CHECK(volume->mesh().stats().volume > 0.f);
    // Automatic texture subdivision creates thousands of triangles. Accumulate
    // their geometric volume in double precision instead of comparing the
    // single-precision cached sum with the original four-face mesh.
    const auto& applied = volume->mesh().its;
    const Vec3d origin = applied.vertices.front().cast<double>();
    double applied_volume = 0.;
    for (const auto& face : applied.indices) {
        const Vec3d a = applied.vertices[face[0]].cast<double>() - origin;
        const Vec3d b = applied.vertices[face[1]].cast<double>() - origin;
        const Vec3d c = applied.vertices[face[2]].cast<double>() - origin;
        applied_volume += a.dot(b.cross(c)) / 6.;
    }
    CHECK_THAT(applied_volume,
               Catch::Matchers::WithinRel(double(expected_volume) * unit_scale * unit_scale * unit_scale, 1e-5));
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK_THAT(volume->mesh().bounding_box().size()[axis], Catch::Matchers::WithinRel(expected_size[axis], 1e-5));
        CHECK_THAT(volume->get_offset()[axis], Catch::Matchers::WithinAbs(expected_offset[axis], 1e-4));
    }
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    CHECK(model.removed_objects_with_zero_volume() == 0);
    CHECK(model.objects.size() == 1);
}

TEST_CASE("Native GLB imports use print axes independently of the meter conversion choice", "[Model][ModelImport][GltfAxes]")
{
    const std::string path = std::string(TEST_DATA_DIR) + "/model_artifact/baseline.glb";
    Model model = Model::read_from_file(path);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    REQUIRE(model.texture_mesh);
    // This fixture spans 0.06 m in X, 0.10 m in glTF Y (up), and 0.04 m in Z.
    const Vec3d expected_size(0.06, 0.04, 0.10);
    for (const float unit_scale : {1.f, 1000.f}) {
        if (unit_scale == 1000.f)
            model.convert_from_meters(true);
        CAPTURE(unit_scale);
        const auto& mesh = model.objects.front()->volumes.front()->mesh();
        const Vec3d actual_size = mesh.bounding_box().size();
        BoundingBoxf3 textured_bounds;
        for (const auto& vertex : model.texture_mesh->vertices)
            textured_bounds.merge(Vec3d(vertex[0], vertex[1], vertex[2]));
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK_THAT(actual_size[axis], Catch::Matchers::WithinAbs(expected_size[axis] * unit_scale, 1e-5));
            // The color handoff applies the volume's unit-conversion flag later.
            CHECK_THAT(textured_bounds.size()[axis] * unit_scale, Catch::Matchers::WithinAbs(actual_size[axis], 1e-5));
        }
        CHECK(mesh.stats().volume > 0.f);
        CHECK_THAT(textured_bounds.max.z(), Catch::Matchers::WithinAbs(0.10, 1e-5));
        CHECK_THAT(textured_bounds.min.z(), Catch::Matchers::WithinAbs(0., 1e-5));
    }
}

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}
