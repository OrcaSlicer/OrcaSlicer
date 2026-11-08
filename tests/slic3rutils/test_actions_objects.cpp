// Automation action tests. Owner: work package S2 (objects). Tag: [actions].
//
// Headless: the object/selection descriptors (registration builds only data and lambdas) and
// the model tree (pure over a Model). The runners themselves are exercised end to end by
// tools/bridge_e2e/cases/objects.yaml against a running slicer.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/ActionTargets.hpp"
#include "slic3r/GUI/Actions/ModelTree.hpp"
#include "slic3r/GUI/Actions/ObjectListLogic.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::Actions;
using json = nlohmann::json;

namespace {

Catalog s2_catalog()
{
    Catalog c;
    register_selection_actions(c);
    register_object_actions(c);
    return c;
}

ModelObject* add_cube(Model& model, const std::string& name, const Vec3d& offset, double size = 10.)
{
    ModelObject* mo = model.add_object();
    mo->name        = name;
    mo->add_volume(TriangleMesh(its_make_cube(size, size, size)));
    mo->add_instance();
    mo->instances.front()->set_offset(offset);
    mo->invalidate_bounding_box();
    return mo;
}

const json* find_object(const json& tree, size_t id)
{
    for (const json& o : tree["objects"])
        if (o["id"].get<size_t>() == id)
            return &o;
    return nullptr;
}

} // namespace

TEST_CASE("object and selection actions register as valid descriptors", "[actions][objects]")
{
    Catalog c;
    REQUIRE_NOTHROW(register_selection_actions(c));
    REQUIRE_NOTHROW(register_object_actions(c));

    const std::set<std::string> expected = {
        "model.getTree",         "selection.set",          "object.rename",        "object.delete",       "object.clone",
        "object.addInstance",    "object.removeInstance",  "object.instancesToObjects", "object.center",  "object.drop",
        "object.setAutoDrop",    "object.setPrintable",    "object.setVisible",    "object.mirror",       "object.order",
        "object.splitToObjects", "object.merge",           "object.convertUnits",  "object.reloadFromDisk", "object.copySettings",
        "object.pasteSettings",  "object.setTransform",    "object.layOnFace",     "plate.clear",         "edit.cut",
        "edit.copy",             "edit.paste",             "part.rename",          "part.delete",         "object.deleteConnectors"};
    std::set<std::string> ids;
    for (const ActionEntry& e : c.entries())
        ids.insert(e.descriptor.id);
    CHECK(ids == expected);

    for (const ActionEntry& e : c.entries()) {
        const ActionDescriptor& d = e.descriptor;
        INFO(d.id);
        CHECK(is_valid_action_id(d.id));
        CHECK(!d.summary.empty());
        CHECK(d.summary.find("Agent") == std::string::npos); // public-safe wording
        CHECK(d.summary.find("agent") == std::string::npos);
        CHECK(bool(e.runner));
        SchemaError err;
        CHECK(check_schema(d.params, err));
        for (const std::string& eff : d.effects)
            CHECK(is_known_effect(eff));
        // Nothing here is destructive or outward: every model edit is undoable.
        CHECK((d.risk == Risk::Read || d.risk == Risk::Ui || d.risk == Risk::Mutate));
        CHECK(d.confirm == Confirm::None);
        REQUIRE(d.examples.is_array());
        CHECK(!d.examples.empty());
        for (const json& ex : d.examples) {
            INFO(ex.dump());
            SchemaError verr;
            CHECK(validate(d.params, apply_defaults(d.params, ex.value("params", json::object())), verr, "/params"));
        }
    }
}

TEST_CASE("object action params reject what the runners cannot do", "[actions][objects]")
{
    const Catalog c         = s2_catalog();
    auto          params_of = [&c](const char* id) -> const json& { return c.find(id)->descriptor.params; };
    SchemaError   err;

    CHECK_FALSE(validate(params_of("object.mirror"), json{{"axis", "w"}}, err));
    CHECK_FALSE(validate(params_of("object.mirror"), json::object(), err));
    CHECK(err.needs == "axis");
    CHECK_FALSE(validate(params_of("object.clone"), json{{"count", 0}}, err));
    CHECK_FALSE(validate(params_of("object.clone"), json{{"count", 101}}, err));
    CHECK_FALSE(validate(params_of("object.setTransform"), json{{"scale", {1, 0, 1}}}, err)); // zero scale
    CHECK_FALSE(validate(params_of("object.setTransform"), json{{"offset", {1, 2}}}, err));   // not a 3-vector
    CHECK(validate(params_of("object.setTransform"), json{{"offset", {10, 20, 0}}, {"instance", 0}}, err));
    // layOnFace: exactly one of normal / largestFace.
    CHECK_FALSE(validate(params_of("object.layOnFace"), json::object(), err));
    CHECK_FALSE(validate(params_of("object.layOnFace"), json{{"normal", {1, 0, 0}}, {"largestFace", true}}, err));
    CHECK(validate(params_of("object.layOnFace"), json{{"normal", {1, 0, 0}}}, err));
    CHECK_FALSE(validate(params_of("object.rename"), json{{"name", ""}}, err));
    CHECK_FALSE(validate(params_of("plate.clear"), json{{"scope", "everything"}}, err));
    CHECK_FALSE(validate(params_of("object.order"), json{{"move", "sideways"}}, err));
    CHECK_FALSE(validate(params_of("object.convertUnits"), json{{"conversion", "fromFeet"}}, err));
    CHECK_FALSE(validate(params_of("selection.set"), json{{"mode", "some"}}, err));
    // order: exactly one of move / toIndex / before; before names one object.
    CHECK(validate(params_of("object.order"), json{{"toIndex", 2}}, err));
    CHECK(validate(params_of("object.order"), json{{"before", {{"name", "left"}}}}, err));
    CHECK_FALSE(validate(params_of("object.order"), json::object(), err));
    CHECK_FALSE(validate(params_of("object.order"), json{{"move", "up"}, {"toIndex", 0}}, err));
    CHECK_FALSE(validate(params_of("object.order"), json{{"before", {{"name", "left"}, {"index", 0}}}}, err));
    CHECK_FALSE(validate(params_of("part.rename"), json::object(), err));
    CHECK(validate(params_of("part.delete"), json{{"breakCutLink", true}}, err));
}

TEST_CASE("object actions that also take parts or instances say so", "[actions][objects]")
{
    const Catalog c = s2_catalog();
    for (const char* id : {"object.mirror", "object.center", "object.drop"}) {
        INFO(id);
        const json& t = c.find(id)->descriptor.target;
        CHECK(target_accepts(t, "objects"));
        CHECK(target_accepts(t, "volumes"));
        CHECK(target_accepts(t, "instances"));
    }
    CHECK(target_accepts(c.find("object.order")->descriptor.target, "volumes"));
    CHECK_FALSE(target_accepts(c.find("object.order")->descriptor.target, "instances"));
    CHECK(target_accepts(c.find("object.setPrintable")->descriptor.target, "instances"));
    CHECK(target_accepts(c.find("object.removeInstance")->descriptor.target, "instances"));
    CHECK(target_accepts(c.find("selection.set")->descriptor.target, "volumes"));
    CHECK_FALSE(target_accepts(c.find("object.delete")->descriptor.target, "volumes"));

    // Catalog::add refuses accepts on a non-object kind, or a kind it cannot widen to.
    Catalog          bad;
    ActionDescriptor d;
    d.id      = "test.widen";
    d.label   = "Widen";
    d.summary = "A test action.";
    d.target  = {{"kind", "volumes"}, {"required", true}, {"accepts", json::array({"instances"})}};
    CHECK_THROWS(bad.add(d));
    d.target = {{"kind", "objects"}, {"required", true}, {"accepts", json::array({"plate"})}};
    CHECK_THROWS(bad.add(d));
    d.target = {{"kind", "objects"}, {"required", true}, {"accepts", json::array({"volumes"})}};
    CHECK_NOTHROW(bad.add(d));
}

TEST_CASE("targets: accepts lets parts or instances satisfy an object target", "[actions][objects]")
{
    Model model;
    add_cube(model, "left", Vec3d(50, 50, 5));
    ModelObject* right = add_cube(model, "right", Vec3d(120, 50, 5));
    right->add_instance()->set_offset(Vec3d(160, 50, 5));

    const json plain    = {{"kind", "objects"}, {"required", true}};
    const json widened  = {{"kind", "objects"}, {"required", true}, {"accepts", json::array({"volumes", "instances"})}};
    const json part     = {{"volumes", json::array({{{"object", {{"name", "right"}}}, {"index", 0}}})}};
    const json instance = {{"instances", json::array({{{"object", {{"name", "right"}}}, {"index", 1}}})}};
    ResolvedTargets out;
    std::string     message;
    json            data;

    CHECK(resolve_targets(model, 1, plain, part, nullptr, out, message, data) == 1002); // objects only
    REQUIRE(resolve_targets(model, 1, widened, part, nullptr, out, message, data) == 0);
    CHECK(out.objects.empty());
    REQUIRE(out.volumes.size() == 1);
    CHECK(out.volumes.front() == std::make_pair(1, 0));
    REQUIRE(resolve_targets(model, 1, widened, instance, nullptr, out, message, data) == 0);
    REQUIRE(out.instances.size() == 1);
    CHECK(out.instances.front() == std::make_pair(1, 1));
    // An object target still works, and nothing at all is still refused.
    REQUIRE(resolve_targets(model, 1, widened, json{{"objects", json::array({{{"index", 0}}})}}, nullptr, out, message, data) == 0);
    CHECK(out.objects == std::vector<int>{0});
    CHECK(resolve_targets(model, 1, widened, nullptr, nullptr, out, message, data) == 1002);
}

TEST_CASE("object list order rules", "[actions][objects]")
{
    using T = ModelVolumeType;
    // Two solid parts, a negative part, a modifier, a blocker.
    const std::vector<T> types = {T::MODEL_PART, T::MODEL_PART, T::NEGATIVE_VOLUME, T::PARAMETER_MODIFIER, T::SUPPORT_BLOCKER};
    CHECK(objlist::part_slots(types, 1) == std::vector<int>{0, 1}); // solid parts swap among themselves
    CHECK(objlist::part_slots(types, 2) == std::vector<int>{2});    // the only negative part stays
    CHECK(objlist::part_slots(types, 4) == std::vector<int>{4});    // blockers stay put
    CHECK_FALSE(objlist::can_move_part(types, 2, 0));               // slot 0 is a solid part's
    // One solid part: it cannot move at all.
    CHECK(objlist::part_slots({T::MODEL_PART, T::PARAMETER_MODIFIER, T::PARAMETER_MODIFIER}, 0) == std::vector<int>{0});
    CHECK_FALSE(objlist::can_move_part({T::MODEL_PART, T::PARAMETER_MODIFIER, T::PARAMETER_MODIFIER}, 0, 1));
    // Two modifiers still swap with each other, as ObjectList::can_move_volume allows any same-type non-solid pair.
    CHECK(objlist::can_move_part({T::MODEL_PART, T::PARAMETER_MODIFIER, T::PARAMETER_MODIFIER}, 1, 2));

    // Objects 3, 5, 8 and 9 share a plate.
    const std::vector<int> slots = {3, 5, 8, 9};
    CHECK(objlist::slot_at(slots, 0) == 3);
    CHECK(objlist::slot_at(slots, 3) == 9);
    CHECK(objlist::slot_at(slots, 4) == -1);
    CHECK(objlist::slot_before(slots, 3, 8) == 5); // moving down lands just before 8
    CHECK(objlist::slot_before(slots, 9, 5) == 5); // moving up takes 5's place
    CHECK(objlist::slot_before(slots, 3, 5) == 3); // already right before: no move
    CHECK(objlist::slot_before(slots, 3, 3) == -1);
    CHECK(objlist::slot_before(slots, 3, 4) == -1); // not on the plate
}

TEST_CASE("part delete refusals mirror the object list's dialogs", "[actions][objects]")
{
    using T = ModelVolumeType;
    using B = objlist::PartDeleteBlock;
    const std::vector<T> one_solid = {T::MODEL_PART, T::PARAMETER_MODIFIER, T::NEGATIVE_VOLUME};
    CHECK(objlist::part_delete_block(one_solid, {0}, false) == B::LastSolidPart);
    CHECK(objlist::part_delete_block(one_solid, {1, 2}, false) == B::None);
    CHECK(objlist::part_delete_block(one_solid, {2}, true) == B::CutObject); // negative part of a cut
    CHECK(objlist::part_delete_block(one_solid, {1}, true) == B::None);      // a modifier of a cut is fine
    const std::vector<T> two_solid = {T::MODEL_PART, T::MODEL_PART};
    CHECK(objlist::part_delete_block(two_solid, {1}, false) == B::None);
    CHECK(objlist::part_delete_block(two_solid, {0, 1}, false) == B::LastSolidPart);
}

TEST_CASE("plate move renumbers the plates", "[actions][objects][plates]")
{
    CHECK(objlist::plate_order_after_move(4, 2, 0) == std::vector<int>{2, 0, 1, 3}); // move to the front
    CHECK(objlist::plate_order_after_move(4, 0, 3) == std::vector<int>{1, 2, 3, 0});
    CHECK(objlist::plate_order_after_move(3, 1, 1) == std::vector<int>{0, 1, 2});
    CHECK(objlist::plate_order_after_move(3, 5, 0) == std::vector<int>{0, 1, 2});
}

TEST_CASE("model.getTree is the typed slicer.getModelTree method", "[actions][objects]")
{
    const Catalog      c = s2_catalog();
    const ActionEntry* e = c.find_by_method("slicer.getModelTree");
    REQUIRE(e != nullptr);
    CHECK(e->descriptor.id == "model.getTree");
    CHECK(e->descriptor.risk == Risk::Read);
}

TEST_CASE("model tree reports objects, instances and volumes", "[actions][objects]")
{
    Model        model;
    ModelObject* a = add_cube(model, "left", Vec3d(50, 100, 5));
    ModelObject* b = add_cube(model, "right", Vec3d(150, 100, 5), 20.);
    b->add_instance()->set_offset(Vec3d(200, 100, 10));
    b->config.set_key_value("wall_loops", new ConfigOptionInt(5));
    b->config.set_key_value("extruder", new ConfigOptionInt(2));
    ModelVolume* mod = b->add_volume(TriangleMesh(its_make_cube(5, 5, 5)), ModelVolumeType::PARAMETER_MODIFIER);
    mod->name        = "Modifier";
    mod->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(50));
    b->layer_config_ranges[{0., 5.}].set_key_value("layer_height", new ConfigOptionFloat(0.1));

    const json tree = build_model_tree(model, nullptr, -1);
    REQUIRE(tree["objects"].size() == 2);
    CHECK(tree["objectCount"] == 2);
    CHECK(tree["truncated"] == false);
    CHECK_FALSE(tree["treeVersion"].get<std::string>().empty());
    CHECK_FALSE(tree.contains("plateCount")); // headless: no plates

    const json& ja = tree["objects"][0];
    CHECK(ja["id"].get<size_t>() == a->id().id);
    CHECK(ja["index"] == 0);
    CHECK(ja["name"] == "left");
    CHECK(ja["filament"] == 0); // inherits
    CHECK(ja["overrides"].empty());
    CHECK(ja["volumes"].size() == 1);
    CHECK(ja["volumes"][0]["type"] == "part");
    CHECK(ja["instances"][0]["offset"] == json::array({50., 100., 5.}));
    CHECK(ja["instances"][0]["scale"] == json::array({1., 1., 1.}));
    CHECK(ja["instances"][0]["mirrored"] == false);

    const json& jb = tree["objects"][1];
    CHECK(jb["filament"] == 2);
    CHECK(jb["overrides"] == json::array({"extruder", "wall_loops"}));
    CHECK(jb["instances"].size() == 2);
    CHECK(jb["instances"][1]["id"].get<size_t>() == b->instances[1]->id().id);
    REQUIRE(jb["volumes"].size() == 2);
    CHECK(jb["volumes"][1]["type"] == "modifier");
    CHECK(jb["volumes"][1]["name"] == "Modifier");
    CHECK(jb["volumes"][1]["overrides"] == json::array({"sparse_infill_density"}));
    CHECK(jb["volumes"][1]["paint"]["support"] == false);
    REQUIRE(jb["layerRanges"].size() == 1);
    CHECK(jb["layerRanges"][0]["zMax"] == 5.);
    CHECK(jb["layerRanges"][0]["overrides"] == json::array({"layer_height"}));
    // World bbox of the 20 mm cube: its size, whatever its offset.
    CHECK(jb["bboxMm"]["size"] == json::array({20., 20., 20.}));
}

TEST_CASE("model tree ids survive deleting another object", "[actions][objects]")
{
    Model model;
    add_cube(model, "first", Vec3d(50, 50, 5));
    ModelObject* keep    = add_cube(model, "second", Vec3d(120, 50, 5));
    const size_t keep_id = keep->id().id;
    const size_t vol_id  = keep->volumes.front()->id().id;

    const json before = build_model_tree(model, nullptr, -1);
    REQUIRE(find_object(before, keep_id) != nullptr);
    CHECK((*find_object(before, keep_id))["index"] == 1);

    model.delete_object(size_t(0));
    const json  after = build_model_tree(model, nullptr, -1);
    const json* o     = find_object(after, keep_id);
    REQUIRE(o != nullptr);
    CHECK((*o)["index"] == 0);       // the index moved ...
    CHECK((*o)["name"] == "second"); // ... the id still names the same object
    CHECK((*o)["volumes"][0]["id"].get<size_t>() == vol_id);
    CHECK(after["treeVersion"] != before["treeVersion"]);
}

TEST_CASE("tree version follows edits and comes back with them", "[actions][objects]")
{
    Model             model;
    ModelObject*      mo = add_cube(model, "cube", Vec3d(100, 100, 5));
    const std::string v0 = tree_version(model);
    CHECK(v0 == tree_version(model)); // deterministic

    mo->instances.front()->set_offset(Vec3d(110, 100, 5));
    CHECK(tree_version(model) != v0);
    mo->instances.front()->set_offset(Vec3d(100, 100, 5));
    CHECK(tree_version(model) == v0); // same state, same version

    mo->name = "renamed";
    CHECK(tree_version(model) != v0);
    mo->name = "cube";
    mo->config.set_key_value("wall_loops", new ConfigOptionInt(4));
    CHECK(tree_version(model) != v0);
    mo->config.erase("wall_loops");
    CHECK(tree_version(model) == v0);

    mo->instances.front()->printable = false;
    CHECK(tree_version(model) != v0);
    mo->instances.front()->printable = true;
    mo->instances.front()->set_scaling_factor(Vec3d(2, 1, 1));
    CHECK(tree_version(model) != v0);
}

TEST_CASE("volume type wire names", "[actions][objects]")
{
    CHECK(std::string(volume_type_name(ModelVolumeType::MODEL_PART)) == "part");
    CHECK(std::string(volume_type_name(ModelVolumeType::NEGATIVE_VOLUME)) == "negative");
    CHECK(std::string(volume_type_name(ModelVolumeType::PARAMETER_MODIFIER)) == "modifier");
    CHECK(std::string(volume_type_name(ModelVolumeType::SUPPORT_BLOCKER)) == "blocker");
    CHECK(std::string(volume_type_name(ModelVolumeType::SUPPORT_ENFORCER)) == "enforcer");
    CHECK(std::string(volume_type_name(ModelVolumeType::INFILL_MESH)) == "infillMesh");
}
