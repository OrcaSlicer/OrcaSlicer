#include <catch2/catch_all.hpp>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleSelector.hpp"

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

TEST_CASE("model_custom_seam_data_changed tracks model parts and negative volumes", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* part = object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART);
    ModelVolume* neg  = object->add_volume(make_cube(5, 5, 20), ModelVolumeType::NEGATIVE_VOLUME);

    Model model_copy = model;
    ModelObject* object_copy = model_copy.objects.front();
    CHECK_FALSE(model_custom_seam_data_changed(*object, *object_copy));

    // Changing seam on negative volume is detected
    ModelVolume* neg_copy = object_copy->volumes[1];
    TriangleSelector sel(neg_copy->mesh());
    sel.set_facet(0, EnforcerBlockerType::ENFORCER);
    neg_copy->seam_facets.set(sel);
    CHECK(model_custom_seam_data_changed(*object, *object_copy));

    // Changing seam on modifier is ignored
    Model model_mod = model;
    ModelObject* object_mod = model_mod.objects.front();
    ModelVolume* mod = object_mod->add_volume(make_cube(2, 2, 2), ModelVolumeType::PARAMETER_MODIFIER);
    Model model_mod_copy = model_mod;
    ModelObject* object_mod_copy = model_mod_copy.objects.front();
    ModelVolume* mod_copy = object_mod_copy->volumes.back();
    TriangleSelector sel_mod(mod_copy->mesh());
    sel_mod.set_facet(0, EnforcerBlockerType::ENFORCER);
    mod_copy->seam_facets.set(sel_mod);
    CHECK_FALSE(model_custom_seam_data_changed(*object_mod, *object_mod_copy));
}

