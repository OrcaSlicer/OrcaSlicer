#include <catch2/catch_all.hpp>
#include <wx/app.h>
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/3DScene.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace Slic3r {
namespace GUI {
struct SelectionTest {
    static void set_valid(Selection& sel, bool valid) {
        sel.m_valid = valid;
    }
    static void set_model(Selection& sel, Model* model) {
        sel.m_model = model;
    }
    static void set_volumes(Selection& sel, GLVolumePtrs* volumes) {
        sel.m_volumes = volumes;
    }
    static void set_mode(Selection& sel, Selection::EMode mode) {
        sel.m_mode = mode;
    }
    static void set_type(Selection& sel, Selection::EType type) {
        sel.m_type = type;
    }
    static void clear_selection(Selection& sel) {
        sel.m_list.clear();
        sel.m_selection_order.clear();
        sel.m_cache.content.clear();
        sel.m_cache.volumes_data.clear();
    }
    static void add_to_selection(Selection& sel, unsigned int vol_idx, int obj_idx, int inst_idx, int vol_id) {
        sel.m_list.insert(vol_idx);
        sel.m_selection_order.push_back(vol_idx);
        sel.m_cache.content[obj_idx].insert(inst_idx);
    }
    static void sync_cache(Selection& sel) {
        sel.set_caches();
    }
    static void set_is_full_selection(Selection& sel, bool full) {
        sel.m_is_full_selection = full;
    }
};
} // namespace GUI
} // namespace Slic3r

static GLVolume* create_mock_gl_volume(ModelObject* obj, int obj_idx, int inst_idx, int vol_idx) {
    GLVolume* v = new GLVolume();
    v->composite_id = GLVolume::CompositeID(obj_idx, vol_idx, inst_idx);
    ModelInstance* inst = obj->instances[inst_idx];
    ModelVolume* vol = obj->volumes[vol_idx];
    v->set_instance_transformation(inst->get_transformation());
    v->set_volume_transformation(vol->get_transformation());
    v->set_convex_hull(vol->mesh());
    v->geometry_id = std::make_pair(vol->id().id, inst->id().id);
    return v;
}

TEST_CASE("Selection: align and distribute instances", "[Selection]") {
    TriangleMesh mesh = make_cube(10.0, 10.0, 10.0);

    Model model;

    // Create 3 objects, each with 1 instance.
    ModelObject* obj0 = model.add_object();
    obj0->add_volume(mesh);
    ModelInstance* inst0 = obj0->add_instance();
    inst0->set_offset(Vec3d(0.0, 0.0, 0.0)); // Local bbox center initially at (5,5,5)

    ModelObject* obj1 = model.add_object();
    obj1->add_volume(mesh);
    ModelInstance* inst1 = obj1->add_instance();
    inst1->set_offset(Vec3d(5.0, 0.0, 0.0)); // Center at (10,5,5)

    ModelObject* obj2 = model.add_object();
    obj2->add_volume(mesh);
    ModelInstance* inst2 = obj2->add_instance();
    inst2->set_offset(Vec3d(20.0, 0.0, 0.0)); // Center at (25,5,5)

    GLVolumePtrs volumes;
    volumes.push_back(create_mock_gl_volume(obj0, 0, 0, 0));
    volumes.push_back(create_mock_gl_volume(obj1, 1, 0, 0));
    volumes.push_back(create_mock_gl_volume(obj2, 2, 0, 0));

    Selection selection;
    SelectionTest::set_model(selection, &model);
    SelectionTest::set_volumes(selection, &volumes);
    SelectionTest::set_valid(selection, true);
    SelectionTest::set_mode(selection, Selection::Instance);
    SelectionTest::set_type(selection, Selection::Mixed);

    SECTION("Distribute instances along X axis with center alignment") {
        SelectionTest::clear_selection(selection);
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 1, 1, 0, 0);
        SelectionTest::add_to_selection(selection, 2, 2, 0, 0);
        SelectionTest::sync_cache(selection);

        // Distribute: axis=0, align_type=1 (center), distribute=true
        selection.align(0, 1, true);

        // Min center is 5 (vol 0), Max center is 25 (vol 2).
        // Total distance is 20.
        // Distributed center for middle (vol 1) should be 5 + 10 = 15.
        // Bounding box center of vol 1 is offset.x() + 5.
        // So offset.x() of vol 1 should become 10.
        REQUIRE_THAT(volumes[0]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_instance_offset().x(), WithinAbs(10.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_instance_offset().x(), WithinAbs(20.0, 1e-4));
    }

    SECTION("Align instances to anchor (local alignment)") {
        SelectionTest::clear_selection(selection);
        // Anchor is volume 0
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 1, 1, 0, 0);
        SelectionTest::add_to_selection(selection, 2, 2, 0, 0);
        SelectionTest::sync_cache(selection);

        // Align center: axis=0, align_type=1, distribute=false
        selection.align(0, 1, false);

        // Anchor center is 5 (offset = 0).
        // Vol 1 and 2 should align their centers to 5.
        // Since local center of mesh is 5, their offset should be 0.
        REQUIRE_THAT(volumes[0]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
    }

    SECTION("Align instances to global bounding box") {
        SelectionTest::clear_selection(selection);
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 1, 1, 0, 0);
        SelectionTest::add_to_selection(selection, 2, 2, 0, 0);
        SelectionTest::sync_cache(selection);

        // Force single full object to trigger global alignment
        SelectionTest::set_type(selection, Selection::SingleFullInstance);

        // Align min: axis=0, align_type=0, distribute=false
        selection.align(0, 0, false);

        // Global min is 0 (vol 0: 0 to 10, vol 1: 5 to 15, vol 2: 20 to 30).
        // All instances should align their min boundary to X=0.
        // Local min of mesh is 0. So offset of all volumes should become 0.
        REQUIRE_THAT(volumes[0]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_instance_offset().x(), WithinAbs(0.0, 1e-4));
    }

    // Cleanup
    for (auto v : volumes) {
        delete v;
    }
}

TEST_CASE("Selection: align and distribute volumes", "[Selection]") {
    TriangleMesh mesh = make_cube(10.0, 10.0, 10.0);

    Model model;

    // Create 1 object with 3 volumes.
    ModelObject* obj = model.add_object();
    obj->add_volume(mesh);
    obj->add_volume(mesh);
    obj->add_volume(mesh);
    ModelInstance* inst = obj->add_instance();
    inst->set_offset(Vec3d(0.0, 0.0, 0.0));

    // Place volumes at offsets relative to instance
    obj->volumes[0]->set_offset(Vec3d(0.0, 0.0, 0.0));  // local center 5
    obj->volumes[1]->set_offset(Vec3d(5.0, 0.0, 0.0));  // local center 10
    obj->volumes[2]->set_offset(Vec3d(20.0, 0.0, 0.0)); // local center 25

    GLVolumePtrs volumes;
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 0));
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 1));
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 2));

    Selection selection;
    SelectionTest::set_model(selection, &model);
    SelectionTest::set_volumes(selection, &volumes);
    SelectionTest::set_valid(selection, true);
    SelectionTest::set_mode(selection, Selection::Volume);
    SelectionTest::set_type(selection, Selection::Mixed);

    SECTION("Distribute volumes along X axis") {
        SelectionTest::clear_selection(selection);
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 1, 0, 0, 1);
        SelectionTest::add_to_selection(selection, 2, 0, 0, 2);
        SelectionTest::sync_cache(selection);

        // Distribute center: axis=0, align_type=1, distribute=true
        selection.align(0, 1, true);

        // Min center is 5 (vol 0), Max center is 25 (vol 2).
        // Total distance is 20.
        // Distributed center for middle volume should be 15.
        // Bounding box local center is offset.x() + 5.
        // So offset.x() of volume 1 should become 10.
        REQUIRE_THAT(volumes[0]->get_volume_offset().x(), WithinAbs(0.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_volume_offset().x(), WithinAbs(10.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_volume_offset().x(), WithinAbs(20.0, 1e-4));
    }

    SECTION("Align volumes to object bounding box") {
        SelectionTest::clear_selection(selection);
        SelectionTest::set_is_full_selection(selection, true);
        // Anchor is volume 0
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 1, 0, 0, 1);
        SelectionTest::add_to_selection(selection, 2, 0, 0, 2);
        SelectionTest::sync_cache(selection);

        // Align center: axis=0, align_type=1, distribute=false
        selection.align(0, 1, false);

        // Object bounding box is [0, 30], so object center is 15.
        // All volumes should align their centers to 15.
        // Since local centers of mesh parts are 5, 10, and 25 respectively,
        // local volume offset of all volumes should become 10.
        REQUIRE_THAT(volumes[0]->get_volume_offset().x(), WithinAbs(10.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_volume_offset().x(), WithinAbs(10.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_volume_offset().x(), WithinAbs(10.0, 1e-4));
    }

    SECTION("Align several volumes of the object (local alignment to anchor part)") {
        SelectionTest::clear_selection(selection);
        SelectionTest::set_is_full_selection(selection, false);
        // Select only volume 1 and volume 2 (volume 0 is not selected)
        // Anchor is volume 1
        SelectionTest::add_to_selection(selection, 1, 0, 0, 1);
        SelectionTest::add_to_selection(selection, 2, 0, 0, 2);
        SelectionTest::sync_cache(selection);

        // Align center: axis=0, align_type=1, distribute=false
        selection.align(0, 1, false);

        // Anchor is volume 1 (local center 10, offset 5, so world center is 10).
        // Volume 2 (local center 25, offset 20, so world center is 25) should align to center 10.
        // New offset of volume 2 should become 20 + (10 - 25) = 5.
        // Volume 1 (anchor) should be skipped and its offset should remain 5.
        REQUIRE_THAT(volumes[1]->get_volume_offset().x(), WithinAbs(5.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_volume_offset().x(), WithinAbs(5.0, 1e-4));
    }

    SECTION("Align all volumes of the object selected by clicking (local alignment to anchor part)") {
        SelectionTest::clear_selection(selection);
        SelectionTest::set_is_full_selection(selection, false);
        selection.set_volume_selection_mode(Selection::Volume);
        // Anchor is volume 1 (clicked first)
        SelectionTest::add_to_selection(selection, 1, 0, 0, 1);
        SelectionTest::add_to_selection(selection, 0, 0, 0, 0);
        SelectionTest::add_to_selection(selection, 2, 0, 0, 2);
        SelectionTest::sync_cache(selection);

        // Align center: axis=0, align_type=1, distribute=false
        selection.align(0, 1, false);

        // Even though all volumes are selected, since m_is_full_selection is false,
        // it should count as clicked selection (second option).
        // Anchor is volume 1 (local center 10, offset 5, so world center is 10).
        // Volume 0 (local center 5, offset 0) should align to center 10. New offset: 0 + (10 - 5) = 5.
        // Volume 2 (local center 25, offset 20) should align to center 10. New offset: 20 + (10 - 25) = 5.
        // Volume 1 (anchor) should be skipped and remain 5.
        REQUIRE_THAT(volumes[0]->get_volume_offset().x(), WithinAbs(5.0, 1e-4));
        REQUIRE_THAT(volumes[1]->get_volume_offset().x(), WithinAbs(5.0, 1e-4));
        REQUIRE_THAT(volumes[2]->get_volume_offset().x(), WithinAbs(5.0, 1e-4));
    }

    // Cleanup
    for (auto v : volumes) {
        delete v;
    }
}

TEST_CASE("Selection: set_deserialized (Undo preservation)", "[Selection]") {
    TriangleMesh mesh = make_cube(10.0, 10.0, 10.0);
    Model model;
    ModelObject* obj = model.add_object();
    obj->add_volume(mesh);
    obj->add_volume(mesh);
    obj->add_volume(mesh);
    ModelInstance* inst = obj->add_instance();
    inst->set_offset(Vec3d(0.0, 0.0, 0.0));

    GLVolumePtrs volumes;
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 0));
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 1));
    volumes.push_back(create_mock_gl_volume(obj, 0, 0, 2));

    Selection selection;
    SelectionTest::set_model(selection, &model);
    SelectionTest::set_volumes(selection, &volumes);
    SelectionTest::set_valid(selection, true);

    SECTION("Restore selection order correctly") {
        std::vector<std::pair<size_t, size_t>> volumes_and_instances = {
            volumes[0]->geometry_id,
            volumes[1]->geometry_id,
            volumes[2]->geometry_id
        };
        // Deserialize with volume 2 first, then volume 0
        std::vector<std::pair<size_t, size_t>> selection_order = {
            volumes[2]->geometry_id,
            volumes[0]->geometry_id
        };

        selection.set_deserialized(Selection::Volume, volumes_and_instances, selection_order);

        // Verify selection contains elements
        REQUIRE(selection.get_volume_idxs().size() == 2);
        REQUIRE(selection.contains_volume(2));
        REQUIRE(selection.contains_volume(0));

        // Verify order is preserved: volume 2 then volume 0
        REQUIRE(selection.get_selection_order().size() == 2);
        REQUIRE(selection.get_selection_order()[0] == 2);
        REQUIRE(selection.get_selection_order()[1] == 0);
    }

    SECTION("Backward compatibility fallback when selection_order is empty") {
        std::vector<std::pair<size_t, size_t>> volumes_and_instances = {
            volumes[0]->geometry_id,
            volumes[1]->geometry_id
        };
        std::vector<std::pair<size_t, size_t>> selection_order = {};

        selection.set_deserialized(Selection::Volume, volumes_and_instances, selection_order);

        // Verify selection falls back to selecting volumes in volumes_and_instances
        REQUIRE(selection.get_volume_idxs().size() == 2);
        REQUIRE(selection.contains_volume(0));
        REQUIRE(selection.contains_volume(1));
        
        // Verify selection order lists both
        REQUIRE(selection.get_selection_order().size() == 2);
        REQUIRE(selection.get_selection_order()[0] == 0);
        REQUIRE(selection.get_selection_order()[1] == 1);
    }

    // Cleanup
    for (auto v : volumes) {
        delete v;
    }
}
