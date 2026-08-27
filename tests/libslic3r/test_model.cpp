#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"

using namespace Slic3r;

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

TEST_CASE("Merging selected volumes yields one part and keeps the rest", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Raw coordinates ([0,10] cubes): the default add_volume re-centers the
    // geometry, which would obscure the baked-in volume offsets checked below.
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    ModelVolume* second = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    second->set_offset(Vec3d(20., 0., 0.)); // second cube spans [20,30] in X
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false); // stays out of the merge

    const size_t faces_first  = object->volumes[0]->mesh().its.indices.size();
    const size_t faces_second = object->volumes[1]->mesh().its.indices.size();

    std::vector<int> vol_indeces = {0, 1};
    const ModelObjectPtrs merged = object->merge_volumes(vol_indeces);

    REQUIRE(merged.size() == 1);
    REQUIRE(merged.front()->volumes.size() == 2);

    const ModelVolume* vol = merged.front()->volumes.front();
    CHECK(vol->name == "Merged Parts");
    CHECK(vol->mesh().its.indices.size() == faces_first + faces_second);

    // The second cube's +20 X offset is baked into the merged vertices, so the
    // merged part spans both cubes. add_volume re-centers the mesh and puts the
    // shift on the volume transform, so measure the transformed box.
    const BoundingBoxf3 bb = vol->mesh().transformed_bounding_box(vol->get_matrix());
    CHECK_THAT(bb.min.x(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(bb.max.x(), Catch::Matchers::WithinAbs(30., 1e-6));

    // The volume left out of the merge is carried over untouched.
    CHECK(merged.front()->volumes[1]->mesh().its.indices.size() == faces_first);
}

TEST_CASE("Merging keeps the part type", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::NEGATIVE_VOLUME, false);
    ModelVolume* second = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::NEGATIVE_VOLUME, false);
    second->set_offset(Vec3d(20., 0., 0.));
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);

    std::vector<int> vol_indeces = {0, 1};
    const ModelObjectPtrs merged = object->merge_volumes(vol_indeces);

    REQUIRE(merged.size() == 1);
    // Defaulting to MODEL_PART here would print geometry that was meant to be
    // subtracted.
    CHECK(merged.front()->volumes.front()->type() == ModelVolumeType::NEGATIVE_VOLUME);
}

TEST_CASE("Painting survives merging volumes", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* first  = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    ModelVolume* second = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    second->set_offset(Vec3d(20., 0., 0.));

    const int faces_first = (int)first->mesh().its.indices.size();

    // Leaf states as serialized by TriangleSelector: "4" = first filament/state,
    // "8" = second. its_merge appends faces in order, so the second volume's
    // paint must reappear at its face index offset by the first volume's count.
    first->mmu_segmentation_facets.set_triangle_from_string(2, "4");
    second->mmu_segmentation_facets.set_triangle_from_string(0, "8");
    second->seam_facets.set_triangle_from_string(1, "4");

    std::vector<int> vol_indeces = {0, 1};
    const ModelObjectPtrs merged = object->merge_volumes(vol_indeces);
    REQUIRE(merged.size() == 1);
    const ModelVolume* vol = merged.front()->volumes.front();

    CHECK(vol->mmu_segmentation_facets.get_triangle_as_string(2) == "4");
    CHECK(vol->mmu_segmentation_facets.get_triangle_as_string(faces_first + 0) == "8");
    CHECK(vol->seam_facets.get_triangle_as_string(faces_first + 1) == "4");
    // A face that was never painted stays unpainted.
    CHECK(vol->mmu_segmentation_facets.get_triangle_as_string(3).empty());
}
