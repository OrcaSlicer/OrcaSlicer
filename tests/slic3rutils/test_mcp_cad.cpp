// The Design tab's commands: what the catalog holds of them (schemas, descriptions, examples) and how a call
// of one is journaled, the plane resolver, the kernel's rollback, reading and editing a feature as its card
// does, and the open sketch's tools driven on a bare sketch tool. None of it needs a wx application or a
// Design panel.
// McpTools.hpp reaches wx's MSW headers through GLCanvas3D.hpp after OCCT has hidden the USER and GDI
// declarations they need; the Windows headers first keep them, as in test_plugin_host_api.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CAD/CadDocument.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/ThreadStandards.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/ActionReplayDialog.hpp"
#include "slic3r/GUI/CAD/DesignOffer.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/Mcp/McpArguments.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpToolsCad.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "retired_names.hpp"
#include "test_utils.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ratio>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

std::vector<std::string> row_names(const json& reply)
{
    std::vector<std::string> names;
    for (const json& row : reply.at("items")) names.push_back(row.at("name").get<std::string>());
    std::sort(names.begin(), names.end());
    return names;
}

std::string error_of(const json& plane, const CadDocument& doc, bool drill = false)
{
    try {
        GUI::Mcp::cad_resolve_plane(plane, doc, drill);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

// A 40 x 20 x 10 box on the XY plane through the world origin: z from 0 to 10.
void add_box(CadDocument& doc)
{
    const int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 20, 0, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
}

// The face of body 0 whose outward normal is `n`.
int face_facing(const CadDocument& doc, const Vec3d& n)
{
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(doc.bodies.at(0).shape);
    for (int i = 0; i < int(faces.size()); ++i)
        if ((GeometryEngine::face_normal_world(faces[i]) - n).norm() < 1e-6) return i;
    return -1;
}

// The message a call fails with, or "" when it does not fail.
template<class F> std::string failure(F&& call)
{
    try {
        call();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

// The code the error of a Design command's call carries when `call` fails inside it (GUI::Mcp::run_guarded()), or ""
// when it does not fail.
template<class F> std::string failure_code(F&& call)
{
    try {
        GUI::Mcp::run_guarded([&]() -> json {
            call();
            return json();
        });
    } catch (const GUI::ToolFailure& f) {
        return f.details.value("code", std::string());
    }
    return {};
}

SketchEntity line(double x0, double y0, double x1, double y1)
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(x0, y0);
    e.p1   = Vec2d(x1, y1);
    return e;
}

SketchEntityConstraintDef constraint(SketchConstraintType t, int ea, SketchPointRole ra, int eb, SketchPointRole rb,
                                     double value = 0)
{
    SketchEntityConstraintDef c;
    c.type  = t;
    c.ea    = ea;
    c.ra    = ra;
    c.eb    = eb;
    c.rb    = rb;
    c.value = value;
    return c;
}

// A 40 x 20 rectangle of four joined lines (line 0 constrained to 40 mm), extruded to the variable
// h = 12, with a through hole: features 0 sketch, 1 extrude, 2 hole.
void add_bound_block(CadDocument& doc)
{
    using T = SketchConstraintType;
    using R = SketchPointRole;
    const std::vector<SketchEntity> rect = {line(0, 0, 40, 0), line(40, 0, 40, 20), line(40, 20, 0, 20), line(0, 20, 0, 0)};
    std::vector<SketchEntityConstraintDef> cons;
    for (int i = 0; i < 4; ++i) cons.push_back(constraint(T::Coincident, i, R::P1, (i + 1) % 4, R::P0));
    cons.push_back(constraint(T::Distance, 0, R::P0, 0, R::P1, 40));
    const int sk = doc.add_sketch_entities(rect, SketchPlane::XY(), "Sketch 1", cons);
    const int ex = doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude 2");
    doc.variables["h"] = "12";
    doc.features[ex].expr["distance"] = "h";
    doc.add_hole(5, 10, true, 20, 10, SketchPlane::XY(), "Hole 3");
    REQUIRE(doc.recompute());
}

// The Design commands in a catalog of their own.
GUI::CommandCatalog cad_catalog()
{
    GUI::CommandCatalog catalog;
    GUI::Mcp::register_cad_commands(catalog);
    catalog.seal();
    return catalog;
}

// Every built-in command, with the Design commands among them, as a run with the CAD feature on holds them.
GUI::CommandCatalog catalog_with_cad()
{
    GUI::CommandCatalog catalog;
    GUI::NativeCommands::register_all_commands(catalog, {true});
    catalog.seal();
    return catalog;
}

// The command `key` of `catalog`; the test stops when there is none.
const GUI::NativeCommand& command_of(const GUI::CommandCatalog& catalog, const std::string& key)
{
    const GUI::NativeCommand* command = catalog.find(key);
    INFO(key);
    REQUIRE(command != nullptr);
    return *command;
}

// A property of a command's input schema, or null when the schema has none by that name.
json property_of(const GUI::NativeCommand& command, const std::string& name)
{
    return command.input_schema.at("properties").value(name, json());
}

// The arguments a command's silent form requires, in its order: its schema's, or for a command that also has a
// person's form, those its schema requires once a call gives any argument (with_silent()).
std::vector<std::string> required_of(const GUI::NativeCommand& command)
{
    const json& schema = command.input_schema;
    if (schema.contains("then"))
        return schema.at("then").value("required", std::vector<std::string>());
    return schema.value("required", std::vector<std::string>());
}

// Whether `command` has a person's form beside its silent one: it takes `silent`.
bool two_forms(const GUI::NativeCommand& command) { return command.input_schema.at("properties").contains("silent"); }

// The Design table's row `id`; the test stops when there is none.
const GUI::DesignTool& design_row(const std::string& id)
{
    for (int i = 0; i < GUI::kDesignToolCount; ++i)
        if (id == GUI::kDesignTools[i].id)
            return GUI::kDesignTools[i];
    INFO(id);
    FAIL("no such row");
    throw std::logic_error("unreachable");
}

// The selection kinds the Design tab reports as what is selected (DesignPanel::offer_selection_kind()): in a sketch,
// what is picked there; on the model, nothing, a face, an edge, a vertex, a body or a committed sketch's loop.
std::vector<GUI::OfferSel> reported_kinds(bool sketching)
{
    using S = GUI::OfferSel;
    if (sketching)
        return {S::SkNone, S::SkLine, S::SkArc, S::SkPoint, S::Sk2Ent};
    return {S::None, S::FacePlanar, S::FaceCyl, S::FaceOther, S::EdgeStr, S::EdgeCirc, S::Vertex, S::BodySolid, S::BodySheet, S::SkLoop};
}

// The id of the item a call of `entry`'s command with `args` opens, or "" when the call runs.
std::string opened_id(const GUI::DesignTool& entry, const json& args)
{
    const GUI::DesignTool* item = GUI::Mcp::opened_item(entry, args);
    return item != nullptr ? item->id : "";
}

// Whether a param by this name takes face or edge ids.
bool is_id_param(const std::string& name)
{
    static const std::vector<std::string> ids = {"face",   "faces",  "edge",   "edges", "up_to_face",
                                                 "face_a", "face_b", "edge_a", "edge_b"};
    return std::find(ids.begin(), ids.end(), name) != ids.end();
}

// A call's context that runs its UI closures in place and keeps the arguments it journals.
struct JournalContext final : GUI::ToolContext
{
    explicit JournalContext(bool person = false) : picked(person) {}

    bool                  person() const override { return picked; }
    json                  run_ui(const std::function<json()>& fn) override { return fn(); }
    json                  observe_ui(const std::function<json()>&) override { return true; }
    const std::string&    job_id() const override { return id; }
    bool                  cancelled() const override { return false; }
    GUI::ToolResult       call_child(const std::string&, const json&) override { return GUI::ToolResult::error("no children"); }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json args) override { journaled = std::move(args); }
    void                  begin_operation() override {}

    bool        picked;
    std::string id;
    json        journaled; // null until the call journals its arguments
};

// An id of the Design commands' old vocabulary, which names no command, and whether its entry stays in the
// Design table as a menu item.
struct Retired
{
    const char* id;
    bool        menu_item;
};

// Ids of the old vocabulary: entries renamed, menu items merged with the argument command for their job, the entry that
// did two jobs, and items folded into their command.
const Retired kRetired[] = {
    // Renamed.
    {"describe_scene", false}, {"describe_feature", false}, {"query_topology", false}, {"slice_body", false},
    {"validate_against", false}, {"mass_props", false}, {"sketch_constraints", false}, {"sketch_unconstrain", false},
    {"sketch_heal", false}, {"sketch_add", false}, {"sketch_commit", false}, {"bridge", false}, {"project", false},
    {"commit", false}, {"feature_set_enabled", false}, {"surf_extrude", false}, {"surf_revolve", false},
    {"surf_loft", false}, {"surf_fill", false}, {"surf_offset", false}, {"plane", false}, {"axis", false},
    {"coordsys_v", false}, {"rename", false}, {"zoom_to", false}, {"constrain", false}, {"sk_poly_inscribed", false},
    {"sk_poly_circumscribed", false}, {"sk_text", false}, {"sk_svg", false}, {"sk_dimension", false},
    {"sk_extend", false}, {"mate", false},
    // A menu item and its argument command, merged.
    {"sketch", false}, {"sketch_begin", false}, {"cut", false}, {"delete", false}, {"delete_body", false},
    {"sk_offset", false}, {"sk_trim", false}, {"sk_fillet", false}, {"sk_chamfer", false}, {"sk_mirror", false},
    {"sk_construct", false}, {"sketch_construction", false}, {"sk_delete", false}, {"sk_constrain", false},
    // Split into feature_edit and feature_set_expression.
    {"edit_feature", false}, {"set_feature", false},
    // Merged into hole, as its type and designation.
    {"hole_styled", false}, {"hole_standard", false},
    // Folded into the command for their job: body_set, sketch_set_value, boolean, thicken, pattern, sketch_pattern,
    // sketch_transform, sketch_draw.
    {"colour", true}, {"sk_length", true}, {"sk_radius", true}, {"sk_angdist", true}, {"bool_subtract", true},
    {"bool_intersect", true}, {"thicken_surf", true}, {"pattern_circular", true}, {"sk_array", true},
    {"sk_array_polar", true}, {"sk_move", true}, {"sk_rotate", true}, {"sk_scale", true}, {"sk_line_t", true},
    {"sk_rect", true}, {"sk_circle", true}, {"sk_arc_center", true}, {"sk_slot", true}, {"sk_ellipse", true},
    {"sk_ellipse_arc", true}, {"sk_spline", true}, {"sk_point_t", true}, {"sk_poly_3", true}, {"sk_poly_4", true},
    {"sk_poly_5", true}, {"sk_polygon", true}, {"sk_poly_8", true}, {"sk_poly_12", true}, {"sk_polyline", true},
    {"sk_rect_center", true}, {"sk_rect_oblique", true}, {"sk_rect_rounded", true}, {"sk_circle_2pt", true},
    {"sk_circle_3pt", true}, {"sk_arc_t", true}, {"sk_arc_tangent", true}, {"sk_slot_arc", true},
    // pattern's type curve: the item stays, greyed.
    {"pat_curve", true},
};

} // namespace

TEST_CASE("the CAD commands are the Design table's entries but its greyed and folded items", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    std::set<std::string>     keys;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        CHECK(keys.insert(c.key).second);
        CHECK(c.group == "Design");
        CHECK_FALSE(c.listed);
        // Its title is the item's name, the one the right-click menu and its Speed Dial action show.
        CHECK(c.title == design_row(c.key).name);
    }
    CHECK(keys.size() == 88);
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        INFO(GUI::kDesignTools[i].id);
        CHECK(keys.count(GUI::kDesignTools[i].id) == (GUI::Mcp::is_design_command(GUI::kDesignTools[i]) ? 1u : 0u));
    }
    // The body's move is named apart from the Prepare tool object_transform; undo and redo are the shared commands.
    CHECK(keys.count("body_transform") == 1);
    for (const char* gone : {"transform", "undo", "redo", "align_to", "colour", "sk_length"}) {
        INFO(gone);
        CHECK(keys.count(gone) == 0);
    }
}

TEST_CASE("a name of the old vocabulary is no command", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = catalog_with_cad();
    std::set<std::string>     listed;
    for (const Retired& retired : kRetired) {
        INFO(retired.id);
        CHECK(listed.insert(retired.id).second);
        CHECK(catalog.find(retired.id) == nullptr);
        const GUI::DesignTool* entry = GUI::Mcp::find_design_tool(retired.id);
        if (!retired.menu_item) {
            CHECK(entry == nullptr);
            continue;
        }
        // An item that stays keeps its entry and its place in the menu, and is no command.
        REQUIRE(entry != nullptr);
        CHECK(entry->row != -1);
    }
    // 89 of the 133 names the Design commands had are gone.
    CHECK(std::size(kRetired) == 89);
}

TEST_CASE("a menu item and the command that runs its job are one command", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"sketch_create", "split", "feature_delete", "body_delete", "sketch_offset", "sketch_trim",
                            "sketch_fillet", "sketch_chamfer", "sketch_mirror", "sketch_toggle_construction", "sketch_delete",
                            "sketch_constrain", "feature_edit"}) {
        INFO(key);
        const GUI::NativeCommand& command = command_of(catalog, key);
        CHECK(command.has_own_action);
        CHECK(two_forms(command));
        const GUI::DesignTool* entry = GUI::Mcp::find_design_tool(key);
        REQUIRE(entry != nullptr);
        CHECK(entry->action != nullptr);
        CHECK(entry->doc != nullptr);
    }
}

TEST_CASE("a renamed entry keeps its menu item", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::DesignTool&    plane   = design_row("construct_plane");
    REQUIRE(plane.key != nullptr);
    CHECK(std::string(plane.key) == "Shift+P");
    CHECK(std::string(plane.action) == "key:S+P");
    // The mate command takes the id align; the unbuilt Align to item stays, greyed, as align_to.
    const GUI::DesignTool& align = design_row("align");
    REQUIRE(align.action != nullptr);
    CHECK(std::string(align.action) == "fly:placement#2");
    CHECK(align.doc != nullptr);
    const GUI::DesignTool& align_to = design_row("align_to");
    CHECK(align_to.row != -1);
    CHECK(align_to.action == nullptr);
    CHECK(align_to.doc == nullptr);
    CHECK(catalog.find("align_to") == nullptr);
    // split takes the Cut card as its menu item, and the greyed Split item goes.
    const GUI::DesignTool& split = design_row("split");
    REQUIRE(split.action != nullptr);
    CHECK(std::string(split.action) == "key:S+X");
    CHECK(std::string(split.name) == "Cut");
    const GUI::DesignTool& expression = design_row("feature_set_expression");
    CHECK(expression.row == -1);
    CHECK(expression.doc != nullptr);
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        INFO(GUI::kDesignTools[i].id);
        CHECK(std::string(GUI::kDesignTools[i].name) != "Split");
    }
}

TEST_CASE("the Design table has a form for every row but its greyed align_to and pat_curve", "[Mcp][CAD]")
{
    REQUIRE(GUI::kDesignToolCount == 127);
    const std::set<std::string> greyed = {"align_to", "pat_curve"};
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        INFO(t.id);
        const bool has_form = t.action != nullptr || t.doc != nullptr;
        CHECK(has_form == (greyed.count(t.id) == 0));
        // A row's access is its silent form's, and one with no silent form has none.
        CHECK((t.doc == nullptr) == (t.access == GUI::CadAccess::None));
        if (t.doc != nullptr) {
            const json doc = json::parse(t.doc);
            for (const char* key : {"summary", "details", "params", "examples", "ids"}) {
                INFO(key);
                CHECK(doc.contains(key));
            }
        }
    }
    // Opening Text, SVG or Color opens a dialog.
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const std::string id = GUI::kDesignTools[i].id;
        INFO(id);
        CHECK(GUI::kDesignTools[i].modal == (id == "sketch_text" || id == "sketch_insert_svg" || id == "colour"));
    }
}

TEST_CASE("every block has a silent function and every silent function a block", "[Mcp][CAD]")
{
    std::set<std::string> blocks, functions;
    for (int i = 0; i < GUI::kDesignToolCount; ++i)
        if (GUI::kDesignTools[i].doc != nullptr)
            blocks.insert(GUI::kDesignTools[i].id);
    for (std::size_t i = 0; i < GUI::Mcp::kSilentFormCount; ++i) {
        INFO(GUI::Mcp::kSilentForms[i].first);
        CHECK(functions.insert(GUI::Mcp::kSilentForms[i].first).second);
        CHECK(GUI::Mcp::kSilentForms[i].second != nullptr);
    }
    CHECK(blocks.size() == 83);
    CHECK(blocks == functions);
}

TEST_CASE("a row with no slot has no menu item", "[Mcp][CAD]")
{
    std::size_t commands_only = 0;
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        INFO(t.id);
        CHECK(t.row >= -1);
        CHECK(t.row < GUI::kOfferRowCount);
        if (t.row != -1)
            continue;
        ++commands_only;
        CHECK(t.action == nullptr);
        CHECK(t.doc != nullptr);
    }
    CHECK(commands_only == 35);
}

TEST_CASE("the Design table names the row and the family submenu each verb sits in", "[Mcp][CAD]")
{
    // The offer lists the corner rectangle at Create > Rectangle > Corner rectangle.
    const GUI::DesignTool& rect = design_row("sk_rect");
    CHECK(std::string(GUI::kOfferRowNames[rect.row]) == "Create");
    CHECK(std::string(rect.family) == "Rectangle");
    CHECK(std::string(rect.name) == "Corner rectangle");
    CHECK(rect.sketch_mode);
    CHECK((rect.accepts & GUI::offer_bit(GUI::OfferSel::SkNone)) != 0);
    CHECK(std::string(design_row("sk_rect_center").family) == "Rectangle");
    CHECK(design_row("sk_spline").family == nullptr);   // directly in Create
    CHECK((design_row("sketch_offset").accepts & GUI::offer_bit(GUI::OfferSel::SkNone)) == 0);   // it needs a picked line, curve or pair
    const GUI::DesignTool& sketch = design_row("sketch_create");
    CHECK(std::string(GUI::kOfferRowNames[sketch.row]) == "Create");
    CHECK(sketch.family == nullptr);   // the feature-mode Sketch sits directly in Create
    CHECK_FALSE(sketch.sketch_mode);
    // describe_design names the selection by the atlas's ids, in OfferSel's order.
    REQUIRE(std::size(GUI::kDesignSelectionIds) == std::size_t(GUI::OfferSel::Count));
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::None)]) == "none");
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::FacePlanar)]) == "face_planar");
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::Sk2Ent)]) == "sk_2ent");
}

TEST_CASE("a command has silent when a call with no arguments opens a tool", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        const GUI::DesignTool& entry = design_row(c.key);
        if (entry.doc != nullptr)
            CHECK(two_forms(c) == (GUI::Mcp::opened_item(entry, json::object()) != nullptr));
        // A command that has an action of its own is the one whose entry has a handler, and it carries the item's icon.
        CHECK(c.has_own_action == (entry.action != nullptr));
        CHECK(c.icon == (entry.icon != nullptr ? entry.icon : ""));
        // An action with no icon would be a blank square in the Speed Dial.
        if (c.has_own_action)
            CHECK_FALSE(c.icon.empty());
    }
    // Their items are folded into them, and their actions are the items'.
    for (const char* key : {"body_set", "sketch_set_value"}) {
        INFO(key);
        CHECK(two_forms(command_of(catalog, key)));
        CHECK_FALSE(command_of(catalog, key).has_own_action);
    }
    CHECK(two_forms(command_of(catalog, "shell")));
    CHECK_FALSE(two_forms(command_of(catalog, "describe_design")));
    // A command with only a person's form takes no arguments at all.
    json silent{{"silent", true}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sketch_polygon_inscribed"), silent), ContainsSubstring("silent"));
    json none = json::object();
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_polygon_inscribed"), none).empty());
}

TEST_CASE("a folded item is no command, and its command opens it", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    std::size_t               folded = 0;
    std::set<std::string>     commands;
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& item = GUI::kDesignTools[i];
        if (item.fold_into == nullptr)
            continue;
        INFO(item.id);
        ++folded;
        commands.insert(item.fold_into);
        CHECK(catalog.find(item.id) == nullptr);
        CHECK_FALSE(GUI::Mcp::is_design_command(item));
        const GUI::NativeCommand& command = command_of(catalog, item.fold_into);
        CHECK_THAT(command.description, ContainsSubstring(item.name));
        // The call with the item's arguments is one its command takes, and it opens the item's tool.
        REQUIRE(item.args != nullptr);
        json args = json::parse(item.args);
        CHECK(GUI::Mcp::argument_error(command, args).empty());
        const GUI::DesignTool* entry = GUI::Mcp::find_design_tool(item.fold_into);
        REQUIRE(entry != nullptr);
        const GUI::DesignTool* opened = GUI::Mcp::opened_item(*entry, args);
        REQUIRE(opened != nullptr);
        REQUIRE(opened->action != nullptr);
        CHECK(std::string(opened->action) == item.action);
    }
    // The Color item, into body_set; the three value items, into sketch_set_value; Subtract and Intersect, into
    // boolean; Thicken Surface, into thicken; Circular pattern, into pattern; the two arrays, into sketch_pattern;
    // Move, Rotate and Scale, into sketch_transform; the twenty-four drawing items, into sketch_draw.
    CHECK(folded == 37);
    CHECK(commands.size() == 8);
}

TEST_CASE("a call opens a tool only with the arguments that choose it", "[Mcp][CAD]")
{
    // body_set's Color item opens with no arguments, and any argument runs the command.
    const GUI::DesignTool& body_set = design_row("body_set");
    const GUI::DesignTool* colour   = GUI::Mcp::opened_item(body_set, json::object());
    REQUIRE(colour != nullptr);
    CHECK(std::string(colour->id) == "colour");
    CHECK(GUI::Mcp::opened_item(body_set, json()) == colour);
    for (const json& runs : {json{{"body", 0}}, json{{"silent", true}}, json{{"generation", 1}}}) {
        INFO(runs.dump());
        CHECK(GUI::Mcp::opened_item(body_set, runs) == nullptr);
    }
    // sketch_set_value's three value items share one handler.
    const GUI::DesignTool* value = GUI::Mcp::opened_item(design_row("sketch_set_value"), json::object());
    REQUIRE(value != nullptr);
    REQUIRE(value->action != nullptr);
    CHECK(std::string(value->action) == "key:V");
    // A command with a menu item of its own opens it; one with no menu item and none folded into it opens nothing.
    const GUI::DesignTool& trim = design_row("sketch_trim");
    CHECK(GUI::Mcp::opened_item(trim, json::object()) == &trim);
    CHECK(GUI::Mcp::opened_item(design_row("sketch_draw"), json::object()) == nullptr);
    // boolean's own item, Join, opens with no arguments and with its own; each operation opens the item that does it,
    // and anything beside the operation runs the command.
    const GUI::DesignTool& boolean = design_row("boolean");
    CHECK(opened_id(boolean, json::object()) == "boolean");
    CHECK(opened_id(boolean, {{"operation", "join"}}) == "boolean");
    CHECK(opened_id(boolean, {{"operation", "cut"}}) == "bool_subtract");
    CHECK(opened_id(boolean, {{"operation", "intersect"}}) == "bool_intersect");
    CHECK(opened_id(boolean, {{"operation", "cut"}, {"target", 0}}).empty());
    CHECK(opened_id(boolean, {{"operation", "cut"}, {"silent", true}}).empty());
    // pattern's own item is the linear one and Circular pattern is folded into it; a curve pattern has no item.
    const GUI::DesignTool& pattern = design_row("pattern");
    CHECK(opened_id(pattern, json::object()) == "pattern");
    CHECK(opened_id(pattern, {{"type", "linear"}}) == "pattern");
    CHECK(opened_id(pattern, {{"type", "circular"}}) == "pattern_circular");
    CHECK(opened_id(pattern, {{"type", "curve"}}).empty());
    CHECK(opened_id(design_row("thicken"), {{"type", "surface"}}) == "thicken_surf");
    // A sketch command with no menu item of its own opens a folded item only with its type, and nothing with no arguments.
    const GUI::DesignTool& sketch_pattern = design_row("sketch_pattern");
    CHECK(opened_id(sketch_pattern, {{"type", "linear"}}) == "sk_array");
    CHECK(opened_id(sketch_pattern, {{"type", "circular"}}) == "sk_array_polar");
    CHECK(opened_id(sketch_pattern, {{"type", "linear"}, {"count", 3}}).empty());
    CHECK(opened_id(sketch_pattern, json::object()).empty());
    const GUI::DesignTool& sketch_transform = design_row("sketch_transform");
    CHECK(opened_id(sketch_transform, {{"type", "scale"}}) == "sk_scale");
    CHECK(opened_id(sketch_transform, json::object()).empty());
    // sketch_draw opens a drawing item with the item's type alone, a polygon item with its sides too; a type with its
    // shape's fields draws it.
    const GUI::DesignTool& sketch_draw = design_row("sketch_draw");
    CHECK(opened_id(sketch_draw, {{"type", "circle_center"}}) == "sk_circle");
    CHECK(opened_id(sketch_draw, {{"type", "polygon"}, {"sides", 6}}) == "sk_polygon");
    CHECK(opened_id(sketch_draw, {{"type", "polygon"}}).empty());
    CHECK(opened_id(sketch_draw, {{"type", "circle_center"}, {"center", {0, 0}}, {"radius", 1}}).empty());
    CHECK(opened_id(sketch_draw, {{"type", "rectangle_rounded"}}) == "sk_rect_rounded");
    CHECK(opened_id(sketch_draw, {{"type", "arc_3_point"}}) == "sk_arc_t");
    // The Extend and Dimension items are their commands' own.
    CHECK(opened_id(design_row("sketch_extend"), json::object()) == "sketch_extend");
    CHECK(opened_id(design_row("sketch_dimension"), json::object()) == "sketch_dimension");
}

TEST_CASE("a folded item opens only in its own mode", "[Mcp][CAD]")
{
    const GUI::DesignTool&                colour    = design_row("colour");
    const std::optional<GUI::ToolFailure> in_sketch = GUI::Mcp::open_refusal(colour, true, int(GUI::OfferSel::SkNone));
    REQUIRE(in_sketch.has_value());
    CHECK(std::string(in_sketch->what()) == "This tool works on the model: finish or cancel the sketch first");
    CHECK_FALSE(GUI::Mcp::open_refusal(colour, false, int(GUI::OfferSel::BodySolid)).has_value());
    // Its handler is a btn: one, which the menu greys for a selection it does not take.
    const std::optional<GUI::ToolFailure> nothing = GUI::Mcp::open_refusal(colour, false, int(GUI::OfferSel::None));
    REQUIRE(nothing.has_value());
    CHECK(std::string(nothing->what()) == "This tool does not apply to what is selected");
    // The command's silent says the mode of the item a call with no arguments opens.
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK_THAT(property_of(command_of(catalog, "sketch_set_value"), "silent").value("description", std::string()),
               ContainsSubstring("Called with no arguments, it runs only in an open sketch"));
    CHECK_THAT(property_of(command_of(catalog, "body_set"), "silent").value("description", std::string()),
               ContainsSubstring("Called with no arguments, it runs only outside a sketch"));
}

TEST_CASE("a tool block's schema refuses an argument it does not name", "[Mcp][CAD]")
{
    const json block{{"params", json::array({{{"name", "radius"}, {"type", "number"}, {"default", 1}}})}};
    const json schema = GUI::Mcp::cad_input_schema(block);
    CHECK(schema.at("additionalProperties") == false);
    CHECK_THAT(GUI::Mcp::validate_arguments(schema, {{"radius", 2}, {"edge", 3}}), ContainsSubstring("unknown argument edge"));
    // What it names, the generation among them, it takes.
    CHECK(GUI::Mcp::validate_arguments(schema, {{"radius", 2}, {"generation", 4}}).empty());
}

TEST_CASE("a two-form CAD command publishes what its silent form requires", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& fillet  = command_of(catalog, "fillet");
    CHECK_FALSE(fillet.input_schema.contains("required"));
    CHECK(fillet.input_schema.at("if") == json{{"minProperties", 1}});
    CHECK(fillet.input_schema.at("then").at("required") == json::array({"edges"}));
    // No arguments is the person's form, which opens the Fillet tool and requires nothing.
    json person = json::object();
    CHECK(GUI::Mcp::argument_error(fillet, person).empty());
    json radius{{"radius", 2}};
    CHECK_THAT(GUI::Mcp::argument_error(fillet, radius), ContainsSubstring("edges is required"));
    json silent{{"silent", true}};
    CHECK_THAT(GUI::Mcp::argument_error(fillet, silent), ContainsSubstring("edges is required"));
}

TEST_CASE("a Design command is named by its verb's id", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"surface_extrude", "surface_revolve", "surface_loft", "surface_patch", "surface_offset",
                            "construct_coordinate_system", "body_mass_properties", "interference",
                            "feature_set_expression", "body_transform"}) {
        INFO(key);
        CHECK(catalog.find(key) != nullptr);
    }
    const std::set<std::string> old = {"surface_fill", "thicken_surface", "pattern_on_curve", "coordsys", "mass_properties",
                                       "check_interference", "set_feature_expr", "transform"};
    for (const std::string& key : old) {
        INFO(key);
        CHECK(catalog.find(key) == nullptr);
    }
    // Bound to a name: a range-for over catalog_with_cad().commands() would walk a destroyed catalog.
    const GUI::CommandCatalog with_cad = catalog_with_cad();
    for (const GUI::NativeCommand& c : with_cad.commands())
        for (const json& example : c.examples)
            for (const json& call : example.at("calls")) {
                INFO(c.key << " / " << example.value("title", ""));
                CHECK(old.count(call.at("tool").get<std::string>()) == 0);
            }
    // Nor does a description: the names that are no English word, nor part of a command's key (body_mass_properties).
    for (const GUI::NativeCommand& c : with_cad.commands())
        for (const std::string& name : old)
            if (name != "transform" && name != "coordsys" && name != "mass_properties") {
                INFO(c.key << " / " << name);
                CHECK(c.description.find(name) == std::string::npos);
                CHECK(c.input_schema.dump().find(name) == std::string::npos);
            }
}

TEST_CASE("a Design command carries its menu item's icon", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(command_of(catalog, "sketch_create").icon == "design_sketch");
    CHECK(command_of(catalog, "body_transform").icon == "design_move");
    // Its Speed Dial action, titled as the right-click menu names its entry, in the tests' English UI.
    const std::unique_ptr<GUI::AppAction> action = GUI::NativeCommands::make_action(command_of(catalog, "sketch_create"));
    REQUIRE(action != nullptr);
    CHECK(action->id() == "orca_command:sketch_create:orca");
    CHECK(action->group == "Design");
    CHECK(action->title() == "Sketch");
    CHECK(GUI::NativeCommands::action_title(command_of(catalog, "sketch_create")) == "Sketch");
    CHECK(std::string(GUI::title_context(command_of(catalog, "sketch_create"))) == "Design");
    CHECK(action->icon == "design_sketch");
    CHECK(action->command == "sketch_create");
}

TEST_CASE("the Design tab actions' icons resolve to shipped SVGs", "[Mcp][CAD][SpeedDial]")
{
    // An icon comes from a hand-edited atlas row, so a typo would be a blank tile and a blank menu item.
    const boost::filesystem::path images = boost::filesystem::path(PROFILES_DIR).parent_path() / "images";
    const GUI::CommandCatalog     catalog = catalog_with_cad();
    std::size_t                   actions = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        if (c.group != "Design")
            continue;
        if (!c.icon.empty()) {
            INFO(c.key << " -> " << c.icon);
            CHECK(boost::filesystem::exists(images / (c.icon + ".svg")));
            actions += c.has_own_action ? 1 : 0;
        }
        if (!c.actions.is_array())
            continue;
        for (const json& action : c.actions) {
            const std::string icon = action.at("icon").get<std::string>();
            INFO(c.key << " / " << action.at("key") << " -> " << icon);
            CHECK_FALSE(icon.empty());
            CHECK(boost::filesystem::exists(images / (icon + ".svg")));
            ++actions;
        }
    }
    // One per entry with a handler: the 52 commands' own and the 37 folded items'.
    CHECK(actions == 89);
}

TEST_CASE("a tool refused for the selection says it does not apply and what it takes", "[Mcp][CAD]")
{
    // Delete Face takes a picked face. With nothing picked, the obstacle is the selection, not the document its
    // item's refusal speaks of.
    const GUI::DesignTool&                     delete_face = design_row("delete_face");
    const int                                  none        = int(GUI::OfferSel::None);
    const std::optional<GUI::ToolFailure>      refused     = GUI::Mcp::open_refusal(delete_face, false, none);
    REQUIRE(refused.has_value());
    CHECK(std::string(refused->what()) == "This tool does not apply to what is selected");
    CHECK(refused->details.at("code") == "wrong_selection");
    CHECK(refused->details.at("selection_kind") == 0);
    CHECK(refused->details.at("selection") == "none");
    CHECK(refused->details.at("accepts") == json::array({"face_planar", "face_cyl", "face_other"}));
    CHECK_FALSE(GUI::Mcp::open_refusal(delete_face, false, int(GUI::OfferSel::FacePlanar)).has_value());
    // Every item only the menu reaches says the same for a selection the tab reports in the item's mode and the item
    // does not take. An item that takes none of those is never greyed for the selection there, and opens whatever is
    // selected.
    std::size_t greyed = 0, never_greyed = 0;
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        if (t.action == nullptr || std::string(t.action).rfind("key:", 0) == 0)
            continue;
        INFO(t.id);
        const std::vector<GUI::OfferSel> kinds = reported_kinds(t.sketch_mode);
        const auto takes     = [&t](GUI::OfferSel s) { return (t.accepts & GUI::offer_bit(s)) != 0; };
        const bool takes_any = std::any_of(kinds.begin(), kinds.end(), takes);
        ++(takes_any ? greyed : never_greyed);
        for (const GUI::OfferSel kind : kinds) {
            INFO(GUI::kDesignSelectionIds[int(kind)]);
            const std::optional<GUI::ToolFailure> refused_here = GUI::Mcp::open_refusal(t, t.sketch_mode, int(kind));
            if (takes(kind) || !takes_any) {
                CHECK_FALSE(refused_here.has_value());
                continue;
            }
            REQUIRE(refused_here.has_value());
            CHECK(std::string(refused_here->what()) == "This tool does not apply to what is selected");
            CHECK(refused_here->details.at("selection") == GUI::kDesignSelectionIds[int(kind)]);
        }
    }
    CHECK(greyed > 0);
    CHECK(never_greyed > 0);
    // A key: handler runs whatever is selected, as its key does.
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("shell"), false, int(GUI::OfferSel::None)).has_value());
}

TEST_CASE("a menu item for a selection the tab never reports opens as its button does", "[Mcp][CAD]")
{
    // Boolean's three items take two bodies and Rib a sketch line, which the model never reports as the selection:
    // their toolbar buttons open the card with nothing selected, and so do the items.
    for (const char* id : {"boolean", "bool_subtract", "bool_intersect", "rib"}) {
        INFO(id);
        CHECK_FALSE(GUI::Mcp::open_refusal(design_row(id), false, int(GUI::OfferSel::None)).has_value());
    }
    // Constrain sketch takes a sketch loop, which the model reports, so a face is refused.
    const std::optional<GUI::ToolFailure> refused =
        GUI::Mcp::open_refusal(design_row("sketch_constrain_selected"), false, int(GUI::OfferSel::FacePlanar));
    REQUIRE(refused.has_value());
    CHECK(std::string(refused->what()) == "This tool does not apply to what is selected");
}

TEST_CASE("the menu items that act on the Feature list's row are not refused for the selection when a call targets that row", "[Mcp][CAD]")
{
    const std::set<std::string> feature_rows = {"feature_edit", "rename_selected", "feature_delete", "sketch_constrain_selected"};
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        INFO(GUI::kDesignTools[i].id);
        CHECK(GUI::kDesignTools[i].feature_row == (feature_rows.count(GUI::kDesignTools[i].id) == 1));
    }
    // With nothing picked in the view: refused for the selection as the menu greys them, and nothing when a call
    // targets the selected row of the Feature list, whose handler says itself what it cannot do.
    const int none = int(GUI::OfferSel::None);
    for (const std::string& id : feature_rows) {
        INFO(id);
        const std::optional<GUI::ToolFailure> refused = GUI::Mcp::open_refusal(design_row(id), false, none);
        REQUIRE(refused.has_value());
        CHECK(std::string(refused->what()) == "This tool does not apply to what is selected");
        CHECK_FALSE(GUI::Mcp::open_refusal(design_row(id), false, none, true).has_value());
        // Still only in the mode its item belongs to.
        CHECK(GUI::Mcp::open_refusal(design_row(id), true, int(GUI::OfferSel::SkNone), true).has_value());
    }
    // Zoom to selection frames what is picked in the view, so a Feature list row is nothing to it.
    CHECK(GUI::Mcp::open_refusal(design_row("zoom_to_selection"), false, none).has_value());
    CHECK(GUI::Mcp::open_refusal(design_row("zoom_to_selection"), false, none, true).has_value());
    // A pick in the view, a card's included, is checked against the item's accepts.
    CHECK(GUI::Mcp::open_refusal(design_row("rename_selected"), false, int(GUI::OfferSel::FacePlanar)).has_value());
}

TEST_CASE("a Design tool opens only in the mode its menu item belongs to", "[Mcp][CAD]")
{
    const std::string in_sketch = "This tool works in a sketch: open a sketch first";
    const std::string on_model  = "This tool works on the model: finish or cancel the sketch first";
    std::size_t       sketch_items = 0, model_items = 0;
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        if (t.action == nullptr)
            continue;
        INFO(t.id);
        CHECK(GUI::Mcp::offered_in_mode(t, t.sketch_mode));
        CHECK_FALSE(GUI::Mcp::offered_in_mode(t, !t.sketch_mode));
        // In the other mode it is refused whatever is selected there, before the selection is looked at.
        for (int kind = 0; kind < int(GUI::OfferSel::Count); ++kind) {
            INFO(kind);
            const std::optional<GUI::ToolFailure> refused = GUI::Mcp::open_refusal(t, !t.sketch_mode, kind);
            REQUIRE(refused.has_value());
            CHECK(std::string(refused->what()) == (t.sketch_mode ? in_sketch : on_model));
            CHECK(refused->details.at("code") == "wrong_mode");
            CHECK(refused->details.at("mode") == (t.sketch_mode ? "sketch" : "model"));
            // The person's message names no command; the hint names those that change the mode. A model tool's names
            // the two that end a sketch and Constrain mode alike, where the busy gate refuses sketch_finish.
            const std::string hint = refused->details.at("hint").get<std::string>();
            if (t.sketch_mode) {
                // With a plane, as sketch_create {} opens the plane picker instead of a sketch.
                CHECK_THAT(hint, ContainsSubstring("sketch_create {plane: \"XY\"}"));
            } else {
                CHECK_THAT(hint, ContainsSubstring("confirm_tool"));
                CHECK_THAT(hint, ContainsSubstring("cancel_tool"));
                CHECK_THAT(hint, !ContainsSubstring("sketch_finish"));
            }
        }
        ++(t.sketch_mode ? sketch_items : model_items);
    }
    CHECK(sketch_items == 46);
    CHECK(model_items == 43);
    // Shell opens on the model whatever is picked, and never over an open sketch.
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("shell"), false, int(GUI::OfferSel::None)).has_value());
    CHECK(GUI::Mcp::open_refusal(design_row("shell"), true, int(GUI::OfferSel::SkNone)).has_value());
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("sk_rect"), true, int(GUI::OfferSel::SkNone)).has_value());
}

TEST_CASE("a pinned Design tab action stays pinned while it is not shown", "[Mcp][CAD][SpeedDial]")
{
    CHECK(GUI::is_design_action_id("orca_command:shell:orca"));
    CHECK(GUI::is_design_action_id("orca_command:sk_rect:orca"));
    CHECK(GUI::is_design_action_id("orca_command:sk_array:orca"));                // a folded item's, which runs sketch_pattern
    CHECK_FALSE(GUI::is_design_action_id("orca_command:describe_design:orca"));   // a Design command with no action
    CHECK_FALSE(GUI::is_design_action_id("orca_command:align_to:orca"));          // an entry with no command
    CHECK_FALSE(GUI::is_design_action_id("orca_command:sk_trim:orca"));           // no entry's: the Trim item is sketch_trim's
    CHECK_FALSE(GUI::is_design_action_id("orca_command:save_project:orca"));
    CHECK_FALSE(GUI::is_design_action_id("orca_plate_goto:shell:orca"));
    CHECK_FALSE(GUI::is_design_action_id("shell"));
    // On the Prepare tab no Design tab action is live, and the third plate is gone. A pin under an id no entry has,
    // as the Trim item's action had before it took sketch_trim's key, has no live action either: it is dropped, and
    // the pins on either side of it stay.
    const std::string           shell = "orca_command:shell:orca", rect = "orca_command:sk_rect:orca";
    const std::string           array = "orca_command:sk_array:orca", trim = "orca_command:sk_trim:orca";
    const std::string           save = "orca_command:save_project:orca", plate = "orca_plate_goto:2:orca";
    const std::set<std::string> live = {save};
    std::vector<std::string>    stored, shown;
    const auto                  is_live = [&](const std::string& id) { return live.count(id) != 0; };
    GUI::prune_favourites({shell, plate, save, trim, array, rect}, is_live, stored, shown);
    CHECK(stored == std::vector<std::string>{shell, save, array, rect});
    CHECK(shown == std::vector<std::string>{save});
}

TEST_CASE("a hidden Design pin takes no place on the favourites bar", "[Mcp][CAD][SpeedDial]")
{
    // Ten stored pins on the Prepare tab, three of them Design tab actions, which are not live there.
    const std::vector<std::string> design = {"orca_command:shell:orca", "orca_command:sk_rect:orca", "orca_command:fillet:orca"};
    std::vector<std::string>       favs;
    for (int i = 1; i <= 7; ++i)
        favs.push_back("orca_command:command" + std::to_string(i) + ":orca");
    favs.insert(favs.begin() + 2, design.begin(), design.end());
    REQUIRE(favs.size() == 10);
    const auto is_live = [&](const std::string& id) { return std::find(design.begin(), design.end(), id) == design.end(); };
    std::vector<std::string> stored, shown;
    GUI::prune_favourites(favs, is_live, stored, shown);
    CHECK(shown.size() == 7);
    // Another pin is taken, and all eleven stay stored.
    std::vector<std::string> pinned = favs;
    CHECK(GUI::add_favourite(pinned, "orca_command:new_project:orca", is_live));
    CHECK(pinned.size() == 11);
    GUI::prune_favourites(pinned, is_live, stored, shown);
    CHECK(stored == pinned);
    CHECK(shown.size() == 8);
}

TEST_CASE("the Design tab's actions are its mode's tools, and none with the CAD feature off", "[Mcp][CAD][SpeedDial]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const bool sketching : {false, true}) {
        INFO("sketching " << sketching);
        // The entry of each action, in the order design_actions() gives them.
        std::vector<std::string> ids;
        for (const GUI::Mcp::DesignAction& action : GUI::Mcp::design_actions(catalog, sketching, /*cad_on=*/true)) {
            REQUIRE(action.command != nullptr);
            const std::string id = action.action != nullptr ? action.action->at("key").get<std::string>() : action.command->key;
            INFO(id);
            const GUI::DesignTool& entry = design_row(id);
            if (entry.fold_into != nullptr) {
                // A folded item's action runs the command it is folded into with the item's arguments.
                REQUIRE(action.action != nullptr);
                CHECK(action.command->key == entry.fold_into);
                CHECK(action.action->at("args") == json::parse(entry.args));
            } else {
                CHECK(action.action == nullptr);
                CHECK(action.command->has_own_action);
            }
            ids.push_back(id);
        }
        // Every entry with a handler has its action in its own mode and none in the other, in the table's order.
        std::vector<std::string> offered;
        for (int i = 0; i < GUI::kDesignToolCount; ++i)
            if (GUI::kDesignTools[i].action != nullptr && GUI::kDesignTools[i].sketch_mode == sketching)
                offered.push_back(GUI::kDesignTools[i].id);
        CHECK(ids == offered);
        for (const char* id : sketching ? std::vector<const char*>{"sketch_trim", "sk_array"} : std::vector<const char*>{"shell", "colour"}) {
            INFO(id);
            CHECK(std::count(ids.begin(), ids.end(), id) == 1);
        }
        // Turned off while the app runs, the tab stays until a restart, and has no action.
        CHECK(GUI::Mcp::design_actions(catalog, sketching, /*cad_on=*/false).empty());
    }
}

TEST_CASE("a folded item's action runs its command with the item's arguments", "[Mcp][CAD][SpeedDial]")
{
    const GUI::CommandCatalog catalog        = cad_catalog();
    const GUI::NativeCommand& sketch_pattern = command_of(catalog, "sketch_pattern");
    const auto [pattern_owner, array]        = GUI::find_command_action(catalog, "sk_array");
    REQUIRE(pattern_owner == &sketch_pattern);
    REQUIRE(array != nullptr);
    // The Linear array item's action, under the id it has had, titled and pictured as the item, in the tests' English UI.
    const std::unique_ptr<GUI::AppAction> action = GUI::NativeCommands::make_action(sketch_pattern, *array);
    REQUIRE(action != nullptr);
    CHECK(action->id() == "orca_command:sk_array:orca");
    CHECK(action->title_en == "Linear array");
    CHECK(action->title() == "Linear array");
    CHECK(action->group_en == "Design");
    CHECK(action->icon == "design_array");
    CHECK(action->command == "sketch_pattern");
    CHECK(action->preset == json{{"type", "linear"}});
    CHECK(action->input_schema.at("properties").empty());
    CHECK_THAT(action->automation_description, ContainsSubstring("Pick entities, drag the spacing handle"));
    CHECK_THAT(action->automation_description, ContainsSubstring("It runs only in an open sketch"));
    // An agent runs it with no arguments, or with exactly the item's.
    CHECK(GUI::Mcp::action_args_error(*action, json::object()).empty());
    CHECK(GUI::Mcp::action_args_error(*action, {{"type", "linear"}}).empty());
    CHECK_FALSE(GUI::Mcp::action_args_error(*action, {{"count", 3}}).empty());
    CHECK_FALSE(GUI::Mcp::action_args_error(*action, {{"type", "circular"}}).empty());
    // A call of the command with the item's arguments is titled as the item's action; with any other, as the command.
    CHECK(GUI::call_title(sketch_pattern, {{"type", "linear"}}) == "Linear array");
    CHECK(GUI::call_title(sketch_pattern, {{"type", "linear"}, {"count", 3}}) == sketch_pattern.title);

    // The Color item runs body_set with no arguments: its preset is an empty object, not null.
    const GUI::NativeCommand& body_set     = command_of(catalog, "body_set");
    const auto [colour_owner, colour_action] = GUI::find_command_action(catalog, "colour");
    REQUIRE(colour_owner == &body_set);
    REQUIRE(colour_action != nullptr);
    const std::unique_ptr<GUI::AppAction> colour = GUI::NativeCommands::make_action(body_set, *colour_action);
    REQUIRE(colour != nullptr);
    CHECK(colour->preset.is_object());
    CHECK(colour->preset.empty());
    CHECK(GUI::call_title(body_set, json::object()) == "Color");
    // The three value items are one tool under three labels, all with no arguments: a call with none is titled as
    // the first of them.
    CHECK(GUI::call_title(command_of(catalog, "sketch_set_value"), json::object()) == "Length\xE2\x80\xA6");

    // A command's own action is found under its key, with none of the command's actions; an id no entry has is none.
    const auto [trim, trim_action] = GUI::find_command_action(catalog, "sketch_trim");
    CHECK(trim == &command_of(catalog, "sketch_trim"));
    CHECK(trim_action == nullptr);
    const auto [none, none_action] = GUI::find_command_action(catalog, "sk_trim");
    CHECK(none == nullptr);
    CHECK(none_action == nullptr);

    // Every action's arguments are a call its command takes, and that call opens the action's item.
    std::size_t actions = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        if (c.actions.is_null())
            continue;
        const GUI::DesignTool* entry = GUI::Mcp::find_design_tool(c.key);
        REQUIRE(entry != nullptr);
        for (const json& command_action : c.actions) {
            INFO(c.key << " / " << command_action.at("key"));
            json args = command_action.at("args");
            CHECK(GUI::Mcp::argument_error(c, args).empty());
            const GUI::DesignTool* item = GUI::Mcp::opened_item(*entry, command_action.at("args"));
            REQUIRE(item != nullptr);
            CHECK(std::string(item->action) == design_row(command_action.at("key").get<std::string>()).action);
            ++actions;
        }
    }
    CHECK(actions == 37);   // one per folded item
}

TEST_CASE("the Action Replay window titles a step that calls a folded item's action as that item", "[Mcp][CAD][SpeedDial]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const std::string         pattern = command_of(catalog, "sketch_pattern").title;
    const json                step{{"kind", "command"}, {"key", "sketch_pattern"}};
    // describe_step()'s `what` for a call of sketch_pattern with the Linear array item's arguments names the item's
    // action: the window shows that action's title, in the tests' English UI.
    json with_action      = step;
    with_action["action"] = "sk_array";
    CHECK(GUI::replay_what_text(with_action, catalog).first == "Linear array");
    // With no action, or an action that is not the command's (body_set's Color, or none at all), the command's own title.
    CHECK(GUI::replay_what_text(step, catalog).first == pattern);
    for (const char* other : {"colour", "sk_none"}) {
        INFO(other);
        json given      = step;
        given["action"] = other;
        CHECK(GUI::replay_what_text(given, catalog).first == pattern);
    }
}

TEST_CASE("a Design command with a read block is read-only", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(command_of(catalog, "describe_design").read_only);
    CHECK(command_of(catalog, "body_describe").read_only);
    CHECK_FALSE(command_of(catalog, "extrude").read_only);
    CHECK_FALSE(command_of(catalog, "sketch_draw").read_only);
}

TEST_CASE("select_geometry selects in the model at rest, as a command that changes it", "[Mcp][CAD]")
{
    // A command with no menu item of its own, under Document, with the access of one that edits, so that it is refused
    // where a click means something else: under a card, a sketch, Constrain mode, a body Move or the Text dialog.
    const GUI::DesignTool& entry = design_row("select_geometry");
    CHECK(entry.access == GUI::CadAccess::Change);
    CHECK(entry.row == -1);
    CHECK(entry.action == nullptr);
    REQUIRE(entry.section != nullptr);
    CHECK(std::string(entry.section) == "Document");
    CHECK_FALSE(entry.feature_row);

    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& command = command_of(catalog, "select_geometry");
    CHECK_FALSE(command.read_only);
    CHECK_FALSE(two_forms(command));
    const json& properties = command.input_schema.at("properties");
    CHECK(properties.size() == 9);
    for (const char* name : {"body", "face", "edges", "vertex", "plane", "sketch", "loop", "feature", "generation"}) {
        INFO(name);
        CHECK(properties.contains(name));
    }
    CHECK(property_of(command, "plane").at("enum") == json::array({"XY", "XZ", "YZ", nullptr}));
    // Nothing is required: a call with none clears the selection. A name it does not take is refused.
    CHECK_FALSE(command.input_schema.contains("required"));
    json none = json::object();
    CHECK(GUI::Mcp::argument_error(command, none).empty());
    json faces{{"body", 0}, {"faces", json::array({3})}};
    CHECK_THAT(GUI::Mcp::argument_error(command, faces), ContainsSubstring("unknown argument faces"));
    // zoom_to_selection frames what is picked in the view, which a Feature list row is not.
    CHECK_THAT(command.description, ContainsSubstring("zoom_to_selection acts on what is picked in the view"));
}

TEST_CASE("confirm_tool and cancel_tool act on what the tab has open, which only a rebuild refuses", "[Mcp][CAD]")
{
    // Commands with no menu item of their own, under Document, with the access ui: what is open in the tab (a card, a
    // sketch, a Move) refuses a command that changes the document, but these act on it. They commit or discard it, so
    // neither is read-only.
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"confirm_tool", "cancel_tool"}) {
        INFO(key);
        const GUI::DesignTool& entry = design_row(key);
        CHECK(entry.access == GUI::CadAccess::Ui);
        CHECK(entry.row == -1);
        CHECK(entry.action == nullptr);
        REQUIRE(entry.section != nullptr);
        CHECK(std::string(entry.section) == "Document");
        const GUI::NativeCommand& command = command_of(catalog, key);
        CHECK_FALSE(command.read_only);
        CHECK_FALSE(command.idempotent);
        CHECK_FALSE(two_forms(command));
        // No argument of their own, and a name they do not take is refused.
        CHECK(command.input_schema.at("properties").size() == 1);
        CHECK(command.input_schema.at("properties").contains("generation"));
        json feature{{"feature", 0}};
        CHECK_THAT(GUI::Mcp::argument_error(command, feature), ContainsSubstring("unknown argument feature"));
    }
    // A Confirm the open card refuses, or whose rebuild fails, replies ok false, and the card stays open. The reply
    // has no error, so the note of a silent form that reports a rejected rebuild with ok false or an error is not
    // carried.
    for (const char* key : {"confirm_tool", "cancel_tool"}) {
        INFO(key);
        CHECK_FALSE(design_row(key).ok_false);
        CHECK_THAT(command_of(catalog, key).description, !ContainsSubstring("error is not empty"));
    }
    CHECK_THAT(command_of(catalog, "confirm_tool").description,
               ContainsSubstring("an add the card refuses, or whose rebuild fails, leaves no feature and no undo step and "
                                 "keeps the card open"));
    CHECK_THAT(command_of(catalog, "confirm_tool").description, ContainsSubstring("replies ok false"));
}

TEST_CASE("body_place places a body outside the history, under Bodies", "[Mcp][CAD]")
{
    // A command with no menu item of its own, with the access of one that edits: where a body stands is refused under what a
    // click would feed, as the Move it does from arguments is.
    const GUI::DesignTool& entry = design_row("body_place");
    CHECK(entry.access == GUI::CadAccess::Change);
    CHECK(entry.row == -1);
    CHECK(entry.action == nullptr);
    REQUIRE(entry.section != nullptr);
    CHECK(std::string(entry.section) == "Bodies");

    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& command = command_of(catalog, "body_place");
    CHECK_FALSE(command.read_only);
    CHECK_FALSE(two_forms(command));
    CHECK(required_of(command) == std::vector<std::string>{"body"});
    for (const char* name : {"move", "rotate", "face", "reset"}) {
        INFO(name);
        CHECK(command.input_schema.at("properties").contains(name));
    }
    CHECK(property_of(command, "reset").at("default") == false);
    json moves{{"body", 0}, {"moves", json::array({1, 0, 0})}};
    CHECK_THAT(GUI::Mcp::argument_error(command, moves), ContainsSubstring("unknown argument moves"));
    // Neither undo nor the project keeps it.
    CHECK_THAT(command.description, ContainsSubstring("no undo step, and the project file does not keep it"));
}

TEST_CASE("a CAD command's schema says what its row says", "[Mcp][CAD]")
{
    const GUI::CommandCatalog  catalog = cad_catalog();
    const GUI::NativeCommand&  fillet  = command_of(catalog, "fillet");
    CHECK(required_of(fillet) == std::vector<std::string>{"edges"});
    const json radius = property_of(fillet, "radius");
    CHECK(radius.at("type") == "number");
    CHECK(radius.at("default") == 1);
    CHECK(radius.at("exclusiveMinimum") == 0);
    CHECK_FALSE(radius.contains("minimum"));
    CHECK_THAT(radius.at("description").get<std::string>(), ContainsSubstring("mm"));
    CHECK(property_of(fillet, "edges").at("type") == json::array({"integer", "array", "string"}));
    CHECK(property_of(fillet, "body").at("default") == -1);
    // Every command with a silent form takes the generation its face and edge ids were read at, and none
    // requires it; one with only a person's form takes no argument.
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        if (design_row(c.key).doc == nullptr) {
            CHECK(c.input_schema.at("properties").empty());
            CHECK(c.input_schema.at("additionalProperties") == false);
            continue;
        }
        const json generation = property_of(c, "generation");
        REQUIRE(generation.is_object());
        CHECK(generation.at("type") == "integer");
        CHECK(generation.at("minimum") == 0);
        CHECK_FALSE(generation.at("description").get<std::string>().empty());
        // A param is required when its row gives it no default, and only then.
        const std::vector<std::string> required = required_of(c);
        for (const auto& [name, prop] : c.input_schema.at("properties").items()) {
            INFO(name);
            const bool is_required = std::find(required.begin(), required.end(), name) != required.end();
            CHECK(is_required == (name != "generation" && name != "silent" && !prop.contains("default")));
        }
        // An argument the row does not name is refused, so a misspelt one never leaves its param at the default.
        CHECK(c.input_schema.at("additionalProperties") == false);
    }
    // A type of two kinds, a limit, an enum and a default carry over.
    CHECK(property_of(command_of(catalog, "sketch_create"), "plane").at("type") == json::array({"string", "object"}));
    CHECK(property_of(command_of(catalog, "construct_plane"), "type").at("enum").size() == 6);
    const json count = property_of(command_of(catalog, "sketch_pattern"), "count");
    CHECK(count.at("minimum") == 2);
    CHECK(count.at("maximum") == 1000);
    // export_step's path has no default, so it is required; sketch_transform's angle defaults to null.
    CHECK(required_of(command_of(catalog, "export_step")) == std::vector<std::string>{"path"});
    CHECK(property_of(command_of(catalog, "sketch_transform"), "angle").at("default").is_null());
}

TEST_CASE("a CAD command's limit is the one its function applies", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // A negative distance grows the surface against the sketch plane's normal.
    json backwards{{"sketch", 0}, {"distance", -5}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "surface_extrude"), backwards).empty());
    // A fillet's radius is refused at zero, as its function refuses it, and a small one is not.
    json flat{{"edges", 0}, {"radius", 0}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "fillet"), flat), ContainsSubstring("radius"));
    json small{{"edges", 0}, {"radius", 0.005}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "fillet"), small).empty());
}

TEST_CASE("a CAD param whose default is null takes null, which its function reads as left out", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // No limit; the open sketch's constraints; an extrude that joins the body its profile touches.
    json no_limit{{"limit", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "body_describe"), no_limit).empty());
    json open_sketch{{"sketch", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_list_constraints"), open_sketch).empty());
    json auto_mode{{"operation", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "extrude"), auto_mode).empty());
    // A two-form command given only a null runs its silent form, the default rectangle: the argument stays, so the
    // call is not the person's, which would open the Extrude card.
    json rectangle{{"sketch", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "extrude"), rectangle).empty());
    CHECK(rectangle == json{{"sketch", nullptr}});
    CHECK_FALSE(GUI::Mcp::person_form(rectangle));
    // A param whose default is a value refuses null, as does one whose function would read a null as given.
    json no_distance{{"distance", nullptr}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "extrude"), no_distance), ContainsSubstring("distance"));
    json no_angle{{"type", "rotate"}, {"angle", nullptr}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sketch_transform"), no_angle), ContainsSubstring("angle"));
    // One kind still stands for the list of it.
    json one_kind{{"kinds", "planar"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "body_describe"), one_kind).empty());
    CHECK(one_kind == json{{"kinds", {"planar"}}});

    // Every param whose default is null admits null, but these, whose functions read a null as given: thread on a
    // plane resolves `plane` as the plane named, and sketch_transform and sketch_pattern tell a type's own params by
    // their presence.
    const std::set<std::pair<std::string, std::string>> as_given{
        {"thread", "plane"},          {"sketch_transform", "angle"}, {"sketch_transform", "angle_deg"},
        {"sketch_transform", "scale"}, {"sketch_transform", "pivot"}, {"sketch_pattern", "angle"},
        {"sketch_pattern", "angle_deg"}, {"sketch_pattern", "count"}, {"sketch_pattern", "pivot"}};
    int admitting = 0;
    for (const GUI::NativeCommand& c : catalog.commands())
        for (const auto& [name, prop] : c.input_schema.at("properties").items()) {
            if (!prop.contains("default") || !prop.at("default").is_null())
                continue;
            INFO(c.key << "." << name);
            const json& type   = prop.at("type");
            const bool  admits = type.is_array() && std::find(type.begin(), type.end(), "null") != type.end();
            CHECK(admits == (as_given.count({c.key, name}) == 0));
            if (prop.contains("enum"))
                CHECK((std::find(prop.at("enum").begin(), prop.at("enum").end(), nullptr) != prop.at("enum").end()) == admits);
            admitting += admits;
        }
    // The params with a null default that admit null, sketch_draw's twenty-seven fields of its shapes, sketch_mirror's
    // axis, sketch_project's and sketch_insert_svg's plane, sketch_text's height, select_geometry's eight and body_place's
    // move, rotate and face among them.
    CHECK(admitting == 84);
}

TEST_CASE("a CAD command takes the names its function also reads", "[Mcp][CAD]")
{
    // feature_describe and the tree edits read `index` as `feature`, and feature_edit `params` or `values` as
    // `fields`: what agents often call them. The call is read as the schema names them.
    const GUI::CommandCatalog catalog = cad_catalog();
    json                      set{{"index", 0}, {"params", {{"width", 20}}}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "feature_edit"), set).empty());
    CHECK(set == json{{"feature", 0}, {"fields", {{"width", 20}}}});
    json values{{"feature", 0}, {"values", {{"width", 30}}}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "feature_edit"), values).empty());
    CHECK(values == json{{"feature", 0}, {"fields", {{"width", 30}}}});
    json describe{{"index", 1}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "feature_describe"), describe).empty());
    CHECK(describe == json{{"feature", 1}});
    // A command that takes `index` itself keeps it.
    json unconstrain{{"index", 4}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_delete_constraint"), unconstrain).empty());
    CHECK(unconstrain == json{{"index", 4}});
}

TEST_CASE("a CAD command's failure names its cause in its code", "[Mcp][CAD]")
{
    const auto code_of = [](const std::function<json()>& run) {
        try {
            GUI::Mcp::run_guarded(run);
        } catch (const GUI::ToolFailure& failure) {
            return failure.details.value("code", json()).get<std::string>();
        }
        return std::string("no failure");
    };
    // A silent form checks its arguments with plain exceptions, and the design's state with CadPrecondition.
    CHECK(code_of([]() -> json { throw std::runtime_error("diameter must be > 0"); }) == "invalid_argument");
    CHECK(code_of([]() -> json { throw GUI::Mcp::CadPrecondition("no body to shell"); }) == "precondition");
    // The kernel's own exceptions are told apart from both, as is what it reports it could not compute.
    CHECK(code_of([]() -> json { throw Standard_Failure("BRep_API: command not done"); }) == "kernel_failure");
    CHECK(code_of([]() -> json { throw GUI::Mcp::CadKernelFailure("section failed"); }) == "kernel_failure");
    try {
        GUI::Mcp::run_guarded([]() -> json { throw GUI::Mcp::CadKernelFailure("section failed"); });
    } catch (const GUI::ToolFailure& failure) {
        CHECK(std::string(failure.what()) == "OCCT: section failed");
    }
    // A refusal of its own keeps its code: no Design view, a busy tab with the commands that close what is open. One
    // that names no cause reads as a plain exception does.
    CHECK(code_of([]() -> json { throw GUI::Mcp::no_design_view(); }) == "design_unavailable");
    CHECK(code_of([]() -> json { throw GUI::ToolFailure("no cause named"); }) == "invalid_argument");
    CHECK(code_of([]() -> json { throw GUI::ToolFailure("no cause named", {{"keys", json::array({"a"})}}); }) == "invalid_argument");
    const GUI::ToolFailure busy = GUI::Mcp::design_busy("a sketch is open in the Design tab");
    CHECK(std::string(busy.what()) == "Design tab busy: a sketch is open in the Design tab");
    CHECK(busy.details.at("code") == "design_busy");
    CHECK_THAT(busy.details.at("hint").get<std::string>(), ContainsSubstring("confirm_tool"));
    CHECK_THAT(busy.details.at("hint").get<std::string>(), ContainsSubstring("cancel_tool"));
    CHECK(code_of([]() -> json { return json::object(); }) == "no failure");

    // The real checks: on a design with no body, the commands that work on one refuse for the design's state.
    CadDocument empty;
    SketchPlane frame;
    CHECK(failure_code([&] { GUI::Mcp::cad_face_extrude(empty, {{"face", 0}}, frame); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_pattern(empty, {{"type", "circular"}, {"count", 4}}); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_dressup(empty, {{"edges", json::array({0})}}, false); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_thicken(empty, {{"type", "face"}, {"face", 0}}); }) == "precondition");
    // An argument wrong in itself is the call's, whatever the design holds.
    CHECK(failure_code([&] { GUI::Mcp::cad_pattern(empty, {{"type", "circular"}, {"count", 0}}); }) == "invalid_argument");
    CHECK(failure_code([&] { GUI::Mcp::cad_face_extrude(empty, {{"face", "top"}}, frame); }) == "invalid_argument");
}

TEST_CASE("a CAD call's generation is any integer of 0 or more, signed or unsigned", "[Mcp][CAD]")
{
    // A script's or plugin's Python integers reach the command as signed JSON integers, which must read as the same
    // generation a reply gave.
    CHECK_FALSE(GUI::Mcp::cad_generation(json::object()).has_value());
    CHECK_FALSE(GUI::Mcp::cad_generation(json()).has_value());
    CHECK(GUI::Mcp::cad_generation({{"generation", std::uint64_t(7)}}) == std::optional<std::uint64_t>(7));
    CHECK(GUI::Mcp::cad_generation({{"generation", std::int64_t(7)}}) == std::optional<std::uint64_t>(7));
    CHECK(GUI::Mcp::cad_generation({{"generation", std::int64_t(0)}}) == std::optional<std::uint64_t>(0));
    // A whole number written as a float passes the schema's integer type, so it counts too.
    CHECK(GUI::Mcp::cad_generation({{"generation", 7.0}}) == std::optional<std::uint64_t>(7));
    // The largest unsigned integer is a generation. A float is one where the schema's integer type takes it, below 2^63
    // (is_whole_number()), so the call and a caller that skips the schema read the same numbers.
    CHECK(GUI::Mcp::cad_generation({{"generation", std::uint64_t(18446744073709551615ull)}}) ==
          std::optional<std::uint64_t>(18446744073709551615ull));
    CHECK(GUI::Mcp::cad_generation({{"generation", 9.0e18}}) == std::optional<std::uint64_t>(9000000000000000000ull));
    for (const json& bad : {json(std::int64_t(-1)), json(-1.0), json("7"), json(7.5), json(nullptr), json(9.3e18),
                            json(9223372036854775808.0), json(18446744073709551616.0), json(1.0e300)}) {
        INFO(bad.dump());
        CHECK(failure_code([&] { GUI::Mcp::cad_generation({{"generation", bad}}); }) == "invalid_argument");
    }
}

TEST_CASE("a CAD command refuses a misspelt argument instead of using its default", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    json                      helix{{"radius", 10}, {"pich", 3}, {"height", 30}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "helix"), helix), ContainsSubstring("unknown argument pich"));
    json shell{{"thicknes", 2}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "shell"), shell), ContainsSubstring("unknown argument thicknes"));
}

TEST_CASE("operation takes Fusion's four words", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    auto operation = [&](const char* key) {
        INFO(key);
        const json p = property_of(command_of(catalog, key), "operation");
        REQUIRE(p.is_object());
        return p;
    };
    // boolean names what it does to the target and has no default: a call that gives any argument names it.
    CHECK(operation("boolean").at("enum") == json::array({"join", "cut", "intersect"}));
    CHECK_FALSE(operation("boolean").contains("default"));
    CHECK(required_of(command_of(catalog, "boolean")) == std::vector<std::string>{"operation"});
    json target{{"target", 0}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "boolean"), target), ContainsSubstring("operation"));
    // extrude's auto joins the body the profile touches, and is what a sketch or a face takes when it is left out.
    CHECK(operation("extrude").at("enum") == json::array({"auto", "new_body", "join", "cut", "intersect", nullptr}));
    CHECK(operation("extrude").at("default").is_null());
    for (const char* key : {"revolve", "sweep", "loft"}) {
        CHECK(operation(key).at("enum") == json::array({"new_body", "join", "cut", "intersect"}));
        CHECK(operation(key).at("default") == "new_body");
    }
    CHECK(operation("mirror").at("enum") == json::array({"new_body", "join"}));
    CHECK(operation("mirror").at("default") == "new_body");

    // A committed sketch extruded with cut into the body under it takes its volume away: a 10 x 10 square at the
    // box's bottom, 4 mm up into the 40 x 20 x 10 box.
    CadDocument doc;
    add_box(doc);
    GUI::DesignSketchTool tool;
    const int sk  = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Sketch");
    const int cut = GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", sk}, {"distance", 4}, {"operation", "cut"}})("Extrude");
    CHECK(doc.features[cut].mode == BooleanMode::Cut);
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    CHECK_THAT(GeometryEngine::mass_properties(doc.bodies.at(0).shape).volume, WithinAbs(40.0 * 20.0 * 10.0 - 10.0 * 10.0 * 4.0, 1e-6));
}

TEST_CASE("an old argument is refused, not ignored", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // Each is refused as an argument its command does not take, named, so that the call does not run without it.
    const std::vector<std::tuple<const char*, json, const char*>> old = {
        {"extrude", {{"sketch", 0}, {"boolean", "subtract"}}, "boolean"},
        {"revolve", {{"sketch", 0}, {"boolean", "new"}}, "boolean"},
        {"boolean", {{"op", "subtract"}}, "op"},
        {"mirror", {{"plane", "XZ"}, {"mode", "add"}}, "mode"},
        // revolve's angle is in degrees and never read angle_deg, which would have left a full turn.
        {"revolve", {{"sketch", 0}, {"angle_deg", 90}}, "angle_deg"},
        {"hole", {{"style", 1}}, "style"},
        {"pattern", {{"circular", true}}, "circular"},
        {"align", {{"kind", 2}, {"cs_a", 0}, {"cs_b", 1}}, "kind"},
        {"feature_suppress", {{"feature", 0}, {"enabled", false}}, "enabled"},
        {"fillet", {{"edge", 3}}, "edge"},
        {"sketch_trim", {{"at", {5, 0}}, {"extend", true}}, "extend"},
        {"sketch_transform", {{"op", "move"}}, "op"},
        {"sketch_transform", {{"type", "move"}, {"count", 3}}, "count"},
    };
    for (const auto& [key, given, name] : old) {
        json args = given;
        INFO(key << " " << args.dump());
        CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, key), args), ContainsSubstring(std::string("unknown argument ") + name));
    }
}

TEST_CASE("a guessed type is read as operation, and operation as type", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    json boolean{{"type", "cut"}, {"target", 0}, {"tool", 1}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "boolean"), boolean).empty());
    CHECK(boolean == json{{"operation", "cut"}, {"target", 0}, {"tool", 1}});
    json extrude{{"sketch", 0}, {"type", "cut"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "extrude"), extrude).empty());
    CHECK(extrude == json{{"sketch", 0}, {"operation", "cut"}});
    json plane{{"operation", "offset"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "construct_plane"), plane).empty());
    CHECK(plane == json{{"type", "offset"}});
    json hole{{"operation", "counterbore"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "hole"), hole).empty());
    CHECK(hole == json{{"type", "counterbore"}});
    // Only a value the other takes is moved: one it does not take stays, and is refused under the name given.
    json sideways{{"type", "sideways"}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "boolean"), sideways), ContainsSubstring("unknown argument type"));
    CHECK(sideways == json{{"type", "sideways"}});
}

TEST_CASE("type names a model command's variant", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    auto prop = [&](const char* key, const char* name) { return property_of(command_of(catalog, key), name); };
    CHECK(prop("hole", "type").at("enum") == json::array({"simple", "counterbore", "countersink"}));
    CHECK(prop("hole", "type").at("default") == "simple");
    // thicken's type left out follows from whether a face is given.
    CHECK(prop("thicken", "type").at("enum") == json::array({"face", "surface", nullptr}));
    CHECK(prop("thicken", "type").at("default").is_null());
    CHECK(prop("pattern", "type").at("enum") == json::array({"linear", "circular", "curve"}));
    CHECK(prop("pattern", "type").at("default") == "linear");
    CHECK(prop("align", "type").at("enum") == json::array({"fastened", "planar", "revolute", "slider", "cylindrical"}));
    CHECK(prop("align", "type").at("default") == "fastened");
    // What the type replaced, and the arguments renamed beside it, are no arguments.
    CHECK(prop("hole", "designation").is_object());
    CHECK(prop("hole", "style").is_null());
    CHECK(prop("pattern", "circular").is_null());
    CHECK(prop("align", "kind").is_null());
    CHECK(prop("feature_suppress", "suppressed").is_object());
    CHECK(prop("feature_suppress", "enabled").is_null());
    for (const char* key : {"fillet", "chamfer"}) {
        INFO(key);
        CHECK(prop(key, "edges").is_object());
        CHECK(prop(key, "edge").is_null());
    }
}

TEST_CASE("one hole command drills what three drilled", "[Mcp][CAD]")
{
    // A 20 x 20 x 10 box on the XY plane, z 0..10, drilled from the centre of its top face: the feature added and the
    // volume it takes out.
    const double box   = 20.0 * 20.0 * 10.0;
    auto         drill = [&](json params) {
        CadDocument doc;
        doc.add_extrude(doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Sketch"), 10.0, false,
                        BooleanMode::New, "Extrude");
        REQUIRE(doc.recompute());
        params["plane"] = {{"body", 0}, {"face", face_facing(doc, Vec3d(0, 0, 1))}};
        json      resolved;
        const int h = GUI::Mcp::cad_hole(doc, params, resolved)("Hole");
        CHECK(resolved.at("body") == 0);
        REQUIRE(doc.recompute());
        return std::make_pair(doc.features.at(h), box - GeometryEngine::mass_properties(doc.bodies.at(0).shape).volume);
    };
    const double disc = PI * 2.5 * 2.5;
    // A blind hole when through is left out, and a through one.
    const auto blind = drill({{"diameter", 5}, {"depth", 4}});
    CHECK_FALSE(blind.first.hole_through);
    CHECK(blind.first.hole_style == 0);
    CHECK_THAT(blind.second, WithinAbs(disc * 4.0, 1e-3));
    CHECK_THAT(drill({{"diameter", 5}, {"depth", 4}, {"through", true}}).second, WithinAbs(disc * 10.0, 1e-3));
    // A counterbore seat of the diameter given.
    const CadFeature cbore = drill({{"type", "counterbore"}, {"cbore_diameter", 9}, {"cbore_depth", 3}}).first;
    CHECK(cbore.hole_style == 1);
    CHECK_THAT(cbore.hole_cbore_diameter, WithinAbs(9.0, 1e-12));
    // The M6 clearance diameter, from the designation.
    const CadFeature m6 = drill({{"designation", "M6"}}).first;
    CHECK_THAT(m6.hole_diameter, WithinAbs(6.6, 1e-6));
    CHECK(m6.hole_standard == "M6");

    CadDocument doc;
    add_box(doc);
    json resolved;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_hole(doc, {{"designation", "M6"}, {"diameter", 5}}, resolved); }),
               ContainsSubstring("give diameter or designation, not both"));
}

TEST_CASE("thicken, pattern and align read their type", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int top = face_facing(doc, Vec3d(0, 0, 1));

    // A face given is a face thickened; a type that contradicts the face is refused.
    const int t = GUI::Mcp::cad_thicken(doc, {{"body", 0}, {"face", top}, {"thickness", 1}})("Thicken");
    CHECK(doc.features[t].type == CadFeatureType::Thicken);
    CHECK(doc.features[t].thicken_face == top);
    CHECK(doc.features[t].target_body == 0);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_thicken(doc, {{"type", "surface"}, {"face", top}}); }), ContainsSubstring("takes no face"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_thicken(doc, {{"type", "face"}}); }), ContainsSubstring("needs face"));
    // With no face, the whole sheet body is thickened.
    CHECK(doc.features[GUI::Mcp::cad_thicken(doc, {{"thickness", 1}})("Thicken")].type == CadFeatureType::ThickenSurface);

    // Only a curve pattern takes a curve, and it needs one.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"type", "curve"}}); }), ContainsSubstring("a curve pattern needs sketch"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"type", "linear"}, {"sketch", 0}}); }),
               ContainsSubstring("sketch goes with type curve"));
    CHECK(doc.features[GUI::Mcp::cad_pattern(doc, {{"type", "circular"}, {"count", 4}})("Pattern")].pattern_circular);
    const int curve = GUI::Mcp::cad_pattern(doc, {{"type", "curve"}, {"sketch", 0}, {"entity", 1}})("Pattern");
    CHECK(doc.features[curve].pattern_curve_sketch == 0);
    CHECK(doc.features[curve].pattern_curve_entity == 1);

    // An align of a type is a Mate of that kind: a block 50 mm beside the box, its coordinate system onto the box's.
    CadDocument two;
    add_box(two);
    SketchPlane beside = SketchPlane::XY();
    beside.origin      = Vec3d(50, 0, 0);
    two.add_extrude(two.add_sketch(SketchShape::Rectangle, beside, 10, 10, 0, "Sketch"), 5.0, false, BooleanMode::New, "Extrude");
    const int a = two.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 10), "CS A");
    two.features[a].coordsys_body = 0;
    const int b = two.add_coordsys(CoordSysType::PointWorld, Vec3d(50, 0, 0), "CS B");
    two.features[b].coordsys_body = 1;
    REQUIRE(two.recompute());
    REQUIRE(two.bodies.size() == 2);
    const int m = GUI::Mcp::cad_align(two, {{"type", "revolute"}, {"cs_a", a}, {"cs_b", b}})("Align");
    CHECK(two.features[m].type == CadFeatureType::Mate);
    CHECK(two.features[m].mate_kind == 2);
    REQUIRE(two.recompute());

    // edges takes one edge id or a list of them, as one fillet or chamfer.
    for (const json& edges : {json::array({0}), json(0)}) {
        INFO(edges.dump());
        CadDocument box;
        add_box(box);
        const int f = GUI::Mcp::cad_dressup(box, {{"edges", edges}, {"radius", 1}}, false)("Fillet");
        CHECK(box.features[f].type == CadFeatureType::Fillet);
        CHECK(box.features[f].dressup_edge_ids() == std::vector<int>{0});
        const int c = GUI::Mcp::cad_dressup(box, {{"edges", edges}, {"distance", 0.5}}, true)("Chamfer");
        CHECK(box.features[c].type == CadFeatureType::Chamfer);
        REQUIRE(box.recompute());
    }
}

TEST_CASE("hole and pattern refuse an argument the type they are given does not take", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const std::size_t features = doc.features.size();
    json              resolved;
    // A hole's seat sizes belong to its type: given without it, they are refused, naming the type that takes them.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_hole(doc, {{"cbore_diameter", 9}, {"cbore_depth", 3}, {"through", true}}, resolved); }),
               ContainsSubstring("cbore_diameter goes with type counterbore, not simple"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_hole(doc, {{"type", "counterbore"}, {"csink_angle", 82}}, resolved); }),
               ContainsSubstring("csink_angle goes with type countersink, not counterbore"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_hole(doc, {{"type", "countersink"}, {"csink_diameter", 9}, {"cbore_depth", 3}}, resolved); }),
               ContainsSubstring("cbore_depth goes with type counterbore, not countersink"));
    // Each type of pattern refuses what only another reads.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"angle", 90}}); }), ContainsSubstring("angle goes with type circular, not linear"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"type", "circular"}, {"spacing", 20}}); }),
               ContainsSubstring("spacing goes with type linear, not circular"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"type", "circular"}, {"dir", 1}}); }),
               ContainsSubstring("dir goes with type linear, not circular"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_pattern(doc, {{"type", "curve"}, {"sketch", 0}, {"entity", 1}, {"plane", "XZ"}}); }),
               ContainsSubstring("plane goes with type linear or circular, not curve"));
    CHECK(doc.features.size() == features);
    // What the call gives is checked, not what the schema defaults: a hole or a pattern given its own type's
    // arguments, and a null where the default is null, runs.
    const int h = GUI::Mcp::cad_hole(doc, {{"type", "countersink"}, {"csink_diameter", 9}, {"csink_angle", 82}}, resolved)("Hole");
    CHECK(doc.features[h].hole_style == 2);
    const int p = GUI::Mcp::cad_pattern(doc, {{"type", "linear"}, {"spacing", 30}, {"dir", 1}, {"plane", "XZ"}, {"sketch", nullptr}})("Pattern");
    CHECK_THAT(doc.features[p].pattern_spacing, WithinAbs(30.0, 1e-12));
    CHECK(doc.features[p].pattern_dir == 1);
}

TEST_CASE("a command's description gives each argument set that opens a tool once, with its items' names", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // The three value items are one tool under three names, opened with no arguments.
    const std::string& value = command_of(catalog, "sketch_set_value").description;
    CHECK_THAT(value, ContainsSubstring(std::string(": {} (") + design_row("sk_length").name + ", " + design_row("sk_radius").name +
                                        ", " + design_row("sk_angdist").name + ")."));
    CHECK(value.find("{}") == value.rfind("{}"));
    // boolean's own item and the two folded into it, one argument set each.
    CHECK_THAT(command_of(catalog, "boolean").description,
               ContainsSubstring(": {\"operation\":\"join\"} (Join), {\"operation\":\"cut\"} (Subtract), "
                                 "{\"operation\":\"intersect\"} (Intersect)."));
}

TEST_CASE("a CAD command's summary is one sentence of at most 120 characters", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        const std::string summary = GUI::Mcp::first_sentence(c.description);
        REQUIRE_FALSE(summary.empty());
        CHECK(summary.size() <= 120);
        CHECK(summary.back() == '.');
    }
    // The description goes on with the row's details, and with how a rejected rebuild shows when the reply says it.
    CHECK_THAT(command_of(catalog, "sketch_transform").description, ContainsSubstring("pivot"));
    CHECK_THAT(command_of(catalog, "shell").description, ContainsSubstring("ok is false"));
    CHECK_THAT(command_of(catalog, "extrude").description, !ContainsSubstring("ok is false"));
    // One with a handler only does what its menu item does, which for Zoom to selection is to act at once.
    CHECK_THAT(command_of(catalog, "zoom_to_selection").description, ContainsSubstring("act at once"));
    CHECK_THAT(command_of(catalog, "zoom_to_selection").description, !ContainsSubstring("opens the tool"));
}

TEST_CASE("a Design command with a person's form says which mode it runs in", "[Mcp][CAD]")
{
    const std::string in_sketch = "runs only in an open sketch", outside = "runs only outside a sketch";
    const GUI::CommandCatalog catalog = cad_catalog();
    std::size_t               told    = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        const GUI::DesignTool& item = design_row(c.key);
        if (item.action == nullptr)
            continue;
        INFO(c.key);
        // A handler only: its description says it of the command, which takes no arguments. Both forms: the `silent`
        // property says it of the call with no arguments, whose item's mode the argument form need not share.
        const bool        both = item.doc != nullptr;
        const std::string text = both ? property_of(c, "silent").value("description", std::string()) : c.description;
        const std::string lead = both ? "Called with no arguments, it " : "It ";
        CHECK_THAT(text, ContainsSubstring(lead + (item.sketch_mode ? in_sketch : outside)));
        CHECK_THAT(text, !ContainsSubstring(item.sketch_mode ? outside : in_sketch));
        if (both)
            CHECK_THAT(text, !ContainsSubstring("It runs only"));
        ++told;
    }
    CHECK(told == 52);   // the commands whose entry has a handler
    CHECK_THAT(command_of(catalog, "sketch_polygon_inscribed").description, ContainsSubstring("It " + in_sketch));
    CHECK_THAT(command_of(catalog, "zoom_to_selection").description, ContainsSubstring("It " + outside));
    CHECK_THAT(property_of(command_of(catalog, "shell"), "silent").value("description", std::string()),
               ContainsSubstring("opens nothing"));
    // The argument forms of these run where their menu item does not: the note says which call it is about.
    for (const char* key : {"sketch_text", "sketch_insert_svg", "sketch_project", "body_mass_properties", "interference"}) {
        INFO(key);
        CHECK_THAT(property_of(command_of(catalog, key), "silent").value("description", std::string()),
                   ContainsSubstring("Called with no arguments, it runs only"));
    }
}

TEST_CASE("an empty entities list names the selection, as leaving it out does", "[Mcp][CAD]")
{
    using nlohmann::json;
    const std::vector<int> selection{2, 4};
    const std::vector<int> none, listed{1, 3}, every{0, 1, 2};
    const json             left_out = json::object(), empty{{"entities", json::array()}}, list{{"entities", {1, 3}}};
    // all_if_empty is what sketch_offset and sketch_mirror pass, and sketch_delete and sketch_toggle_construction do not.
    const bool all_if_empty = GENERATE(false, true);
    CAPTURE(all_if_empty);

    // A list names its entities, whatever is selected.
    CHECK(GUI::Mcp::sketch_target_indices(list, selection, 6, all_if_empty) == listed);
    CHECK(GUI::Mcp::sketch_target_indices(list, none, 6, all_if_empty) == listed);
    // The schema gives [] as the default, so it reads as left out: the selection.
    CHECK(GUI::Mcp::sketch_target_indices(left_out, selection, 6, all_if_empty) == selection);
    CHECK(GUI::Mcp::sketch_target_indices(empty, selection, 6, all_if_empty) == selection);
    // With nothing selected, only a command that says so takes every entity.
    CHECK(GUI::Mcp::sketch_target_indices(left_out, none, 3, all_if_empty) == (all_if_empty ? every : none));
    CHECK(GUI::Mcp::sketch_target_indices(empty, none, 3, all_if_empty) == (all_if_empty ? every : none));
    // Only an empty list is read so: anything else that is given names what it holds, a null nothing.
    CHECK(GUI::Mcp::sketch_target_indices({{"entities", nullptr}}, selection, 6, all_if_empty).empty());
}

TEST_CASE("a Design command that only reads is marked idempotent", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = catalog_with_cad();
    std::size_t               reads   = 0;
    for (const GUI::NativeCommand& c : catalog.commands())
        if (c.group == "Design" && c.read_only) {
            INFO(c.key);
            CHECK(c.idempotent);
            ++reads;
        }
    CHECK(reads > 0);
}

TEST_CASE("a sketch command that rebuilds nothing promises no rejected rebuild", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = catalog_with_cad();
    // It toggles flags on the open sketch's entities: its reply says how many changed, and has no error.
    const std::string& description = command_of(catalog, "sketch_toggle_construction").description;
    CHECK_THAT(description, !ContainsSubstring("rebuild"));
    CHECK_THAT(description, ContainsSubstring("changed"));
}

TEST_CASE("no CAD command exists with the feature off", "[Mcp][CAD]")
{
    const auto design_commands = [](bool cad) {
        GUI::CommandCatalog catalog;
        GUI::NativeCommands::register_all_commands(catalog, {cad});
        std::size_t count = 0;
        for (const GUI::NativeCommand& c : catalog.commands())
            if (c.group == "Design")
                ++count;
        return count;
    };
    CHECK(design_commands(false) == 0);
    CHECK(design_commands(true) == 88);
}

TEST_CASE("the Design commands are the design's 88, under its headings", "[Mcp][CAD]")
{
    // The design's command set, heading by heading.
    const std::map<std::string, std::set<std::string>> design = {
        {"Document",
         {"new_design", "describe_design", "import_step", "import_mesh", "export_step", "commit_to_plate", "set_variable",
          "select_geometry", "confirm_tool", "cancel_tool"}},
        {"Sketch",
         {"sketch_create", "sketch_finish", "sketch_cancel", "sketch_describe", "sketch_validate", "sketch_select", "sketch_edit", "sketch_draw",
          "sketch_text", "sketch_insert_svg", "sketch_mirror", "sketch_pattern", "sketch_project", "sketch_dimension",
          "sketch_set_value", "sketch_fillet", "sketch_chamfer", "sketch_trim", "sketch_extend", "sketch_offset",
          "sketch_transform", "sketch_blend_curve", "sketch_toggle_construction", "sketch_delete", "sketch_repair",
          "sketch_constrain", "sketch_list_constraints", "sketch_delete_constraint"}},
        {"Solid",
         {"extrude", "revolve", "sweep", "loft", "rib", "hole", "thread", "thicken", "pattern", "mirror", "fillet", "chamfer",
          "shell", "draft", "delete_face", "boolean", "split", "align", "list_align_types"}},
        {"Surface", {"surface_extrude", "surface_revolve", "surface_loft", "surface_patch", "surface_offset"}},
        {"Construct", {"construct_plane", "construct_axis", "construct_coordinate_system", "helix"}},
        {"Inspect", {"measure", "interference", "body_describe", "body_mass_properties", "body_section", "body_compare"}},
        {"Bodies", {"body_transform", "body_set", "body_delete", "body_place"}},
        {"History",
         {"feature_describe", "feature_edit", "feature_set_expression", "feature_suppress", "feature_move", "feature_rename",
          "feature_delete"}},
        {"Menu items",
         {"rename_selected", "zoom_to_selection", "sketch_polygon_inscribed", "sketch_polygon_circumscribed",
          "sketch_constrain_selected"}},
    };
    std::map<std::string, std::set<std::string>> table;
    std::size_t                                  commands = 0;
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        const std::string      id = t.id;
        INFO(id);
        if (!GUI::Mcp::is_design_command(t)) {
            CHECK(t.section == nullptr);
            continue;
        }
        REQUIRE(t.section != nullptr);
        table[t.section].insert(id);
        ++commands;
        // A command that only does what its menu item does is one of the menu items.
        const bool item_only = t.action != nullptr && t.doc == nullptr;
        CHECK(item_only == (std::string(t.section) == "Menu items"));
    }
    CHECK(table == design);
    CHECK(commands == 88);
    // The headings, in the order the index gives them.
    CHECK(std::vector<std::string>(std::begin(GUI::kDesignSections), std::end(GUI::kDesignSections)) ==
          std::vector<std::string>{"Document", "Sketch", "Solid", "Surface", "Construct", "Inspect", "Bodies", "History",
                                   "Menu items"});
}

TEST_CASE("the tool_list description names every Design command under its heading", "[Mcp][CAD]")
{
    const GUI::CommandCatalog with = catalog_with_cad();
    GUI::CommandCatalog       without;
    GUI::NativeCommands::register_all_commands(without, {false});
    without.seal();
    const std::string told  = command_of(with, "tool_list").description;
    const std::string plain = command_of(without, "tool_list").description;

    // With the feature off the description says nothing of them; with it on, the index follows that text.
    REQUIRE(told.size() > plain.size());
    CHECK(told.compare(0, plain.size(), plain) == 0);
    // The most a client is known to keep of a description.
    CHECK(told.size() <= 4096);
    // An agent reads it before it calls anything: no word of it is a retired command's name, and before the Design tab's
    // index it names no command, as the tool list shows them all.
    CHECK(retired_names_in(told) == std::set<std::string>{});
    std::set<std::string> commands_named;
    std::string           word;
    for (const char ch : plain + ' ') {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
            word += ch;
            continue;
        }
        if (without.find(word) != nullptr)
            commands_named.insert(word);
        word.clear();
    }
    CHECK(commands_named == std::set<std::string>{});
    const std::string index = told.substr(plain.size());
    CHECK_THAT(index, ContainsSubstring("CAD"));
    CHECK_THAT(index, ContainsSubstring("`tool_describe {tool}` gives one's manual"));
    // A command with silent does what its menu item does when it is given no arguments.
    CHECK_THAT(index,
               ContainsSubstring("`silent` does what its menu item does when called with no arguments, unless `silent` is true"));

    // Each heading starts a sentence once, in the design's order, and its names follow its colon to the full stop,
    // apart by ", ", in the table's order, each marked as code.
    std::size_t at    = 0;
    std::size_t named = 0;
    for (const char* heading : GUI::kDesignSections) {
        INFO(heading);
        const std::string start = std::string(". ") + heading;
        const std::size_t head  = index.find(start, at);
        REQUIRE(head != std::string::npos);
        CHECK(index.find(start, head + 1) == std::string::npos);
        const std::size_t colon = index.find(": ", head);
        const std::size_t stop  = index.find('.', head + 2);
        REQUIRE(colon != std::string::npos);
        REQUIRE(stop != std::string::npos);
        REQUIRE(colon < stop);
        std::vector<std::string> names;
        for (std::size_t from = colon + 2; from < stop;) {
            const std::size_t comma = std::min(index.find(", ", from), stop);
            const std::string marked = index.substr(from, comma - from);
            REQUIRE(marked.size() > 2);
            CHECK(marked.front() == '`');
            CHECK(marked.back() == '`');
            names.push_back(marked.substr(1, marked.size() - 2));
            from = comma + 2;
        }
        std::vector<std::string> in_table;
        for (int i = 0; i < GUI::kDesignToolCount; ++i)
            if (GUI::kDesignTools[i].section != nullptr && std::string(GUI::kDesignTools[i].section) == heading)
                in_table.push_back(GUI::kDesignTools[i].id);
        CHECK(names == in_table);
        for (const std::string& name : names) {
            INFO(name);
            CHECK(name.rfind("sk_", 0) == std::string::npos);
        }
        named += names.size();
        at = stop;
    }
    CHECK(named == 88);
    // The menu items' heading says they take no arguments.
    CHECK_THAT(index, ContainsSubstring(". Menu items, which take no arguments"));
}

TEST_CASE("a Design reply keeps a rejected rebuild's ok false and its error", "[Mcp][CAD]")
{
    const json rejected{{"ok", false}, {"replaced", false}, {"error", "the sketch was not replaced: open profile"}};
    const GUI::NativeCommand finish = GUI::Mcp::cad_command("sketch_finish", "Finish sketch",
                                                            json{{"type", "object"}, {"properties", json::object()}},
                                                            [&rejected](const json&, bool) { return rejected; });
    JournalContext        agent;
    const GUI::ToolResult reply = finish.run(json::object(), agent);
    CHECK_FALSE(reply.is_error);
    CHECK(reply.data == rejected);
}

TEST_CASE("a CAD call is journaled without its generation and with its silent", "[Mcp][CAD]")
{
    json                     seen;
    const GUI::NativeCommand fillet = GUI::Mcp::cad_command("fillet", "Fillet", json{{"type", "object"}, {"properties", json::object()}},
                                                            [&seen](const json& args, bool) {
                                                                seen = args;
                                                                return json{{"ok", true}, {"error", ""}, {"feature_index", 3}};
                                                            });
    CHECK(fillet.group == "Design");
    CHECK_FALSE(fillet.listed);

    JournalContext             agent;
    const GUI::ToolResult      reply = fillet.run({{"generation", 4}, {"silent", true}, {"radius", 2}}, agent);
    CHECK(agent.journaled == json{{"silent", true}, {"radius", 2}});
    // The function still checks the generation, and the reply is its value without an ok true and an empty error,
    // which say nothing.
    CHECK(seen == json{{"generation", 4}, {"silent", true}, {"radius", 2}});
    CHECK_FALSE(reply.is_error);
    CHECK(reply.data == json{{"feature_index", 3}});

    // A person's pick runs the function in place, with the same reply, and journals nothing.
    JournalContext person(/*person=*/true);
    CHECK(fillet.run(json::object(), person).data == json{{"feature_index", 3}});
    CHECK(person.journaled.is_null());

    // A call that ran and gave nothing but its generation is journaled silent when a call with no arguments
    // opens a tool: its replay, given none, would open the tool.
    const GUI::NativeCommand shell = GUI::Mcp::cad_command(
        "shell", "Shell", GUI::Mcp::with_silent(json{{"type", "object"}, {"properties", json::object()}}),
        [](const json&, bool) { return json{{"ok", true}}; }, [](const json& args) { return GUI::Mcp::person_form(args); });
    JournalContext generation_only;
    shell.run({{"generation", 4}}, generation_only);
    CHECK(generation_only.journaled == json{{"silent", true}});
    // One for which nothing opens a tool has no `silent` to journal.
    JournalContext read;
    fillet.run({{"generation", 4}}, read);
    CHECK(read.journaled == json::object());
    // body_set's Color item opens with no arguments. The call is journaled before it runs, and it fails here, where
    // there is no Design tab.
    const GUI::CommandCatalog catalog = cad_catalog();
    JournalContext            body;
    CHECK_THROWS_AS(command_of(catalog, "body_set").run({{"generation", 1}}, body), GUI::ToolFailure);
    CHECK(body.journaled == json{{"silent", true}});
    // boolean's operation alone opens the item that does it, so a cut given its generation is replayed as a cut.
    JournalContext cut;
    CHECK_THROWS_AS(command_of(catalog, "boolean").run({{"operation", "cut"}, {"generation", 1}}, cut), GUI::ToolFailure);
    CHECK(cut.journaled == json{{"operation", "cut"}, {"silent", true}});

    // A failure is the call's error.
    const GUI::NativeCommand refused = GUI::Mcp::cad_command("hole", "Hole", json{{"type", "object"}, {"properties", json::object()}},
                                                             [](const json&, bool) -> json {
                                                                 throw GUI::ToolFailure("removed no material", {{"code", "rebuild_failed"}});
                                                             });
    CHECK_THROWS_AS(refused.run(json::object(), agent), GUI::ToolFailure);
}

TEST_CASE("a CAD command tells its function a person's pick from a call", "[Mcp][CAD]")
{
    // The four menu items that act on the Feature list's row are not refused for the selection on a call, and a person's
    // pick keeps the menu's rule: the function is told which it runs for.
    std::vector<bool>        told;
    const GUI::NativeCommand rename = GUI::Mcp::cad_command("rename_selected", "Rename", json{{"type", "object"}, {"properties", json::object()}},
                                                            [&told](const json&, bool person) {
                                                                told.push_back(person);
                                                                return json{{"ok", true}};
                                                            });
    JournalContext person(/*person=*/true);
    rename.run(json::object(), person);
    JournalContext agent;
    rename.run(json::object(), agent);
    CHECK(told == std::vector<bool>{true, false});
}

TEST_CASE("an unknown command is answered with close names only", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const std::string         unknown = catalog.unknown_command("extrud", /*suggest=*/true);
    // The closest key, and no sentence about another tool.
    CHECK(unknown == "Unknown tool: extrud (did you mean extrude?)");
}

namespace {

// Whether `command` takes the argument `key`: a property of its schema, or <p>_deg for a property <p> in
// radians, which the function also reads in degrees.
bool takes_arg(const GUI::NativeCommand& command, const std::string& key)
{
    const json& properties = command.input_schema.at("properties");
    if (properties.contains(key)) return true;
    const std::string suffix = "_deg";
    if (key.size() <= suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const auto radians = properties.find(key.substr(0, key.size() - suffix.size()));
    if (radians == properties.end()) return false;
    // cad_input_schema() writes a param's unit into its description.
    const std::string description = radians->value("description", std::string());
    const std::string unit        = " (rad)";
    return description == "In rad." ||
           (description.size() > unit.size() && description.compare(description.size() - unit.size(), unit.size(), unit) == 0);
}

// Whether an example passes a face or edge id as `key`: a param that takes ids, or a reference that
// names a face or an edge, as plane takes {"body": b, "face": f} (and hole's at, measure's a and b).
bool passes_id(const std::string& key, const json& value)
{
    if (is_id_param(key)) return true;
    if (!value.is_object()) return false;
    for (const char* id : {"face", "faces", "edge", "edges"})
        if (value.contains(id)) return true;
    return false;
}

// Whether `value` is {"from_call": i, "key": k}: the value under k in the reply of call i of the example.
bool from_call(const json& value) { return value.is_object() && value.contains("from_call"); }

} // namespace

TEST_CASE("every example passes its command's schema", "[Mcp][CAD]")
{
    // The Design commands' and undo's and redo's, which a run with the CAD feature on documents with them.
    const GUI::CommandCatalog catalog   = catalog_with_cad();
    std::size_t               described = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        if (c.examples.is_null())
            continue;
        ++described;
        REQUIRE(c.examples.is_array());
        for (const json& example : c.examples) {
            const std::string title = example.value("title", "");
            INFO(c.key << " / " << title);
            CHECK_FALSE(title.empty());
            for (const auto& item : example.items())
                CHECK_THAT(item.key(), Equals("title") || Equals("calls") || Equals("needs"));
            if (example.contains("needs")) CHECK_FALSE(example.at("needs").get<std::string>().empty());
            const json& calls = example.at("calls");
            REQUIRE_FALSE(calls.empty());
            for (size_t i = 0; i < calls.size(); ++i) {
                const std::string tool = calls[i].at("tool").get<std::string>();
                INFO("call " << i << ": " << tool);
                for (const auto& item : calls[i].items())
                    CHECK_THAT(item.key(), Equals("tool") || Equals("args"));
                const GUI::NativeCommand& called = command_of(catalog, tool);
                REQUIRE(calls[i].at("args").is_object());
                // What an earlier call gives is left out; the rest is what the call sends.
                json sent = json::object();
                for (const auto& [key, value] : calls[i].at("args").items()) {
                    INFO("argument " << key);
                    if (!from_call(value)) {
                        sent[key] = value;
                        continue;
                    }
                    const json& from = value.at("from_call");
                    REQUIRE(from.is_number_integer());
                    CHECK(from.get<int>() >= 0);
                    CHECK(from.get<int>() < int(i));
                    REQUIRE(value.contains("key"));
                    REQUIRE(value.at("key").is_string());
                    CHECK_FALSE(value.at("key").get<std::string>().empty());
                    // What a later call is given still goes by a name the command takes.
                    CHECK(takes_arg(called, key));
                }
                // With no arguments, a command that has a person's form would open its tool instead.
                if (two_forms(called))
                    CHECK_FALSE(sent.empty());
                CHECK(GUI::Mcp::argument_error(called, sent) == "");
                // The schema lets other names through, which the function then ignores, so a misspelt one
                // would pass. argument_error() has put an alias under the name it stands for.
                for (const auto& item : sent.items()) {
                    INFO("argument " << item.key());
                    CHECK(takes_arg(called, item.key()));
                }
            }
            // An example ends with the command it documents.
            CHECK(calls.back().at("tool") == c.key);
        }
    }
    // The 83 Design commands with a silent form, undo and redo, the six object and part commands of Parts 4a and 4b,
    // Part 4c's filament_add_mixed, filament_edit_mixed, Part 5a's printer_list_models, printer_install and printer_uninstall, Part 5b's
    // filament_install and filament_uninstall, Part 5c's preferences_set, Part 5d's plugin_list, plugin_enable,
    // plugin_reload and plugin_uninstall, the seven gizmo commands, and the six printing commands (print_start,
    // printer_test_host, printer_status, print_pause, print_resume and print_stop).
    CHECK(described == 116);
}

TEST_CASE("an example's argument goes by a name its command takes", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(takes_arg(command_of(catalog, "fillet"), "radius"));
    CHECK_FALSE(takes_arg(command_of(catalog, "fillet"), "radious"));
    // A param in radians is also read in degrees, and only such a param.
    CHECK(takes_arg(command_of(catalog, "sketch_transform"), "angle_deg"));
    CHECK_FALSE(takes_arg(command_of(catalog, "fillet"), "radius_deg"));
}

// A stale id still names a real face or edge, so an example that passes ids shows the safe pattern:
// read them with body_describe and pass that reply's generation with the call.
TEST_CASE("every example that passes a face or edge id passes the generation it was read at", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const GUI::NativeCommand& c : catalog.commands())
        for (const json& example : c.examples) {
            INFO(c.key << " / " << example.value("title", ""));
            const json& calls = example.at("calls");
            for (size_t i = 0; i < calls.size(); ++i) {
                const json& args = calls[i].at("args");
                for (const auto& [key, value] : args.items()) {
                    if (!passes_id(key, value)) continue;
                    INFO("call " << i << ": " << calls[i].at("tool").get<std::string>() << " passes " << key);
                    const json generation = args.value("generation", json());
                    CHECK(generation.is_object());
                    if (!generation.is_object()) continue;
                    CHECK(generation.value("key", "") == "generation");
                    const int from = generation.value("from_call", -1);
                    CHECK(from >= 0);
                    CHECK(from < int(i));
                    if (from < 0 || from >= int(i)) continue;
                    CHECK(calls[size_t(from)].at("tool") == "body_describe");
                }
            }
        }
}

TEST_CASE("every CAD command with a silent form has one to three examples", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        // A command with a handler and no silent form takes no arguments, so there is no call to show.
        if (design_row(c.key).doc == nullptr) {
            CHECK(c.examples.is_null());
            continue;
        }
        REQUIRE(c.examples.is_array());
        CHECK(c.examples.size() >= 1);
        CHECK(c.examples.size() <= 3);
    }
    // undo and redo document the Design history only where the Design commands are.
    GUI::CommandCatalog without;
    GUI::NativeCommands::register_all_commands(without, {false});
    const GUI::CommandCatalog with = catalog_with_cad();
    for (const char* key : {"undo", "redo"}) {
        INFO(key);
        CHECK(command_of(without, key).examples.is_null());
        CHECK(command_of(with, key).examples.size() == 2);
    }
}

TEST_CASE("rollback takes back a failed edit and leaves nothing to redo", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const size_t features = doc.features.size();
    const size_t depth    = doc.undo_depth();

    doc.checkpoint();
    const int bad = doc.add_extrude(0, 5.0, false, BooleanMode::New, "Bad");
    doc.features[bad].expr["distance"] = "nosuchvar";
    REQUIRE_FALSE(doc.recompute());

    REQUIRE(doc.rollback());
    CHECK(doc.features.size() == features);
    CHECK(doc.undo_depth() == depth);
    CHECK_FALSE(doc.can_redo());
    CHECK(doc.error.empty());
    CHECK(doc.bodies.size() == 1);
}

TEST_CASE("rollback keeps the redo branch the failed edit's checkpoint cleared", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    doc.checkpoint();
    doc.features[1].distance = 20.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.undo());
    REQUIRE(doc.can_redo());

    doc.checkpoint();
    doc.features[0].expr["width"] = "nosuchvar";
    REQUIRE_FALSE(doc.recompute());
    REQUIRE(doc.rollback());

    // The edit that failed is gone, and the step undone before it can still be redone.
    CHECK(doc.features[0].expr.empty());
    REQUIRE(doc.redo());
    CHECK_THAT(doc.features[1].distance, WithinAbs(20.0, 1e-9));
}

TEST_CASE("a hole's face plane points into the body with x along the face's long side", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int top = face_facing(doc, Vec3d(0, 0, 1));
    REQUIRE(top >= 0);

    SketchPlane drill;
    REQUIRE(doc.inward_plane_of_face(0, top, drill));
    CHECK_THAT(drill.origin.z(), WithinAbs(10.0, 1e-6));
    CHECK_THAT(drill.origin.x(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(drill.normal.z(), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(std::abs(drill.x_axis.x()), WithinAbs(1.0, 1e-9));   // the 40 mm side runs along X

    CHECK_FALSE(doc.inward_plane_of_face(0, 999, drill));
    CHECK_FALSE(doc.inward_plane_of_face(3, top, drill));
}

TEST_CASE("plane resolves base planes on the modeling origin, datums and planar faces", "[Mcp][CAD]")
{
    CadDocument doc;
    doc.modeling_origin = Vec3d(100, 50, 0);
    add_box(doc);

    const SketchPlane xy = GUI::Mcp::cad_resolve_plane("XY", doc, false);
    CHECK((xy.origin - Vec3d(100, 50, 0)).norm() < 1e-9);
    CHECK((xy.normal - Vec3d(0, 0, 1)).norm() < 1e-9);
    CHECK((GUI::Mcp::cad_resolve_plane("XZ", doc, false).normal - Vec3d(0, 1, 0)).norm() < 1e-9);

    // A datum is named by its feature index.
    const int datum = doc.add_plane(0 /*XY*/, 30.0, 0.0, 0, "Plane");
    REQUIRE(doc.recompute());
    const SketchPlane dp = GUI::Mcp::cad_resolve_plane({{"datum", datum}}, doc, false);
    CHECK_THAT(dp.origin.z(), WithinAbs(30.0, 1e-9));
    CHECK((dp.normal - Vec3d(0, 0, 1)).norm() < 1e-9);

    // A face gives the sketch-on-face plane, or the drilling plane, and says whose face it is.
    const int top = face_facing(doc, Vec3d(0, 0, 1));
    REQUIRE(top >= 0);
    int body = -1;
    const SketchPlane on = GUI::Mcp::cad_resolve_plane({{"body", 0}, {"face", top}}, doc, false, &body);
    CHECK(body == 0);
    CHECK_THAT(on.origin.z(), WithinAbs(10.0, 1e-6));
    CHECK((on.normal - Vec3d(0, 0, 1)).norm() < 1e-9);
    const SketchPlane into = GUI::Mcp::cad_resolve_plane({{"body", 0}, {"face", top}}, doc, true);
    CHECK((into.normal - Vec3d(0, 0, -1)).norm() < 1e-9);
}

TEST_CASE("plane refuses what is not a plane, naming why", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int datum = doc.add_plane(0, 30.0, 0.0, 0, "Plane");
    REQUIRE(doc.recompute());

    CHECK_THAT(error_of("Top", doc), ContainsSubstring("plane must be"));
    CHECK_THAT(error_of({{"datum", "1"}}, doc), ContainsSubstring("plane must be"));
    CHECK_THAT(error_of({{"datum", 0}}, doc), ContainsSubstring("is not a datum plane"));
    CHECK_THAT(error_of({{"datum", 99}}, doc), ContainsSubstring("is not a feature"));
    CHECK_THAT(error_of({{"body", 4}, {"face", 0}}, doc), ContainsSubstring("out of range"));
    CHECK_THAT(error_of({{"body", 0}, {"face", 999}}, doc), ContainsSubstring("does not exist"));

    CHECK(failure_code([&] { GUI::Mcp::cad_resolve_plane({{"datum", 0}}, doc, false); }) == "invalid_argument");
    doc.features[datum].enabled = false;
    REQUIRE(doc.recompute());
    CHECK_THAT(error_of({{"datum", datum}}, doc), ContainsSubstring("hidden"));
    CHECK(failure_code([&] { GUI::Mcp::cad_resolve_plane({{"datum", datum}}, doc, false); }) == "precondition");

    // A cylinder's side is not a plane, for sketching or for drilling.
    CadDocument cyl;
    const int sk = cyl.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 5.0, "Sketch");
    cyl.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(cyl.recompute());
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(cyl.bodies.at(0).shape);
    int side = -1;
    for (int i = 0; i < int(faces.size()); ++i)
        if (GeometryEngine::cylinder_of_face(faces[i]).ok) side = i;
    REQUIRE(side >= 0);
    CHECK_THAT(error_of({{"body", 0}, {"face", side}}, cyl), ContainsSubstring("is cylindrical"));
    CHECK_THAT(error_of({{"body", 0}, {"face", side}}, cyl, true), ContainsSubstring("is cylindrical"));
}

TEST_CASE("feature_describe reads a sketch and the extrude bound to a variable", "[Mcp][CAD]")
{
    CadDocument doc;
    add_bound_block(doc);

    const json sketch = GUI::Mcp::cad_feature_json(doc, 0);
    CHECK(sketch.at("type") == "Sketch");
    CHECK(sketch.at("name") == "Sketch 1");
    // Whether it is left out of the rebuild, as feature_suppress says it.
    CHECK(sketch.at("suppressed") == false);
    CHECK_FALSE(sketch.contains("enabled"));
    CHECK(sketch.at("plane").at("base") == "XY");
    CHECK(sketch.at("editable").empty());   // the sketch session edits entities, not feature_edit
    REQUIRE(sketch.at("entities").size() == 4);
    CHECK(sketch.at("entities")[0].at("type") == "line");
    const json& cons = sketch.at("constraints");
    REQUIRE(cons.size() == 5);
    CHECK(cons[0].at("type") == "coincident");
    CHECK(cons[0].at("refs") == json::array({json{{"entity", 0}, {"role", "p1"}}, json{{"entity", 1}, {"role", "p0"}}}));
    CHECK(cons[4].at("type") == "distance");
    CHECK_THAT(cons[4].at("value").get<double>(), WithinAbs(40.0, 1e-9));
    CHECK(sketch.at("used_by") == json::array({1}));
    CHECK(sketch.at("bodies_made").empty());

    const json extrude = GUI::Mcp::cad_feature_json(doc, 1);
    CHECK(extrude.at("type") == "Extrude");
    CHECK_THAT(extrude.at("params").at("distance").get<double>(), WithinAbs(12.0, 1e-9));   // the variable's value
    CHECK(extrude.at("expr").at("distance") == "h");
    CHECK(extrude.at("params").at("operation") == "new_body");
    CHECK_FALSE(extrude.at("params").contains("mode"));
    CHECK(extrude.at("params").at("extrude_end") == "blind");
    CHECK(extrude.at("refs").at("sketch_ref") == 0);
    CHECK(extrude.at("ranges").at("distance") == json::array({-1000.0, 1000.0}));
    CHECK(extrude.at("bodies_made") == json::array({0}));
    CHECK_FALSE(extrude.contains("plane"));   // it builds on its sketch's plane

    const json hole = GUI::Mcp::cad_feature_json(doc, 2);
    CHECK_THAT(hole.at("params").at("hole_x").get<double>(), WithinAbs(20.0, 1e-9));
    CHECK(hole.at("params").at("hole_through") == true);
    CHECK(hole.contains("plane"));

    CHECK_THAT(failure([&] { GUI::Mcp::cad_feature_json(doc, 3); }), ContainsSubstring("feature 3 is out of range"));
}

TEST_CASE("feature_describe names constraint references as the solver reads them", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    using R = SketchPointRole;
    CadDocument doc;
    std::vector<SketchEntityConstraintDef> cons = {
        constraint(T::Parallel, 0, R::P0, 1, R::P0),
        constraint(T::Angle, 0, R::P0, 1, R::P0, PI / 2),
        constraint(T::Coincident, 0, R::P0, kSketchRefOrigin, R::P0),
        constraint(T::PointOnLine, 1, R::P1, 0, R::P0),
    };
    doc.add_sketch_entities({line(0, 0, 10, 0), line(0, 5, 10, 5)}, SketchPlane::XY(), "Sketch", cons);

    const json c = GUI::Mcp::cad_feature_json(doc, 0).at("constraints");
    CHECK(c[0].at("refs") == json::array({json{{"entity", 0}}, json{{"entity", 1}}}));   // whole lines, no roles
    CHECK_FALSE(c[0].contains("value"));
    CHECK(c[1].at("type") == "angle");
    CHECK_THAT(c[1].at("value").get<double>(), WithinAbs(90.0, 1e-9));           // degrees, as the Design commands take angles
    CHECK(c[2].at("refs")[1] == json{{"entity", "origin"}});
    CHECK(c[3].at("type") == "point_on_line");
    CHECK(c[3].at("refs") == json::array({json{{"entity", 1}, {"role", "p1"}}, json{{"entity", 0}}}));
}

TEST_CASE("feature_describe lists the features built on a datum plane", "[Mcp][CAD]")
{
    CadDocument doc;
    doc.modeling_origin = Vec3d(100, 50, 0);
    const int first  = doc.add_plane(0 /*XY*/, 30.0, 0.0, 0, "Plane 1");
    const int second = doc.add_plane(3 /*the first datum*/, 10.0, 0.0, 0, "Plane 2");
    doc.recompute();

    const json p = GUI::Mcp::cad_feature_json(doc, first);
    CHECK(p.at("used_by") == json::array({second}));
    CHECK_THAT(p.at("plane").at("origin")[2].get<double>(), WithinAbs(30.0, 1e-9));
    CHECK_FALSE(p.at("plane").contains("base"));   // parallel to XY but not through the modeling origin
    CHECK_THAT(GUI::Mcp::cad_feature_json(doc, second).at("plane").at("origin")[2].get<double>(), WithinAbs(40.0, 1e-9));
}

TEST_CASE("feature_describe gives each imported region's loops and filled area as placed", "[Mcp][CAD]")
{
    // Imported art keeps each region as its outer loop followed by its holes. A 10 x 4 outline with
    // a 2 x 2 hole is 2 loops filling 40 - 4 = 36 mm²; a right triangle with legs 6 and 4, wound
    // clockwise, 1 loop filling 12.
    CadDocument doc;
    CadFeature  art;
    art.type             = CadFeatureType::Sketch;
    art.name             = "SVG 1";
    art.imported_regions = {
        {{Vec2d(0, 0), Vec2d(10, 0), Vec2d(10, 4), Vec2d(0, 4)}, {Vec2d(4, 1), Vec2d(4, 3), Vec2d(6, 3), Vec2d(6, 1)}},
        {{Vec2d(0, 0), Vec2d(0, 6), Vec2d(4, 0)}},
    };
    // Placed 2 wide and 3 tall, every area grows by 2 * 3.
    art.import_scale_x = 2.0;
    art.import_scale_y = 3.0;
    doc.features.push_back(art);

    const json params = GUI::Mcp::cad_feature_json(doc, 0).at("params");
    CHECK(params.at("regions") == 2);
    CHECK(params.at("region_loops") == json::array({2, 1}));
    const json& areas = params.at("region_areas");
    REQUIRE(areas.size() == 2);
    CHECK_THAT(areas[0].get<double>(), WithinAbs(36.0 * 6.0, 1e-9));
    CHECK_THAT(areas[1].get<double>(), WithinAbs(12.0 * 6.0, 1e-9));
}

TEST_CASE("every editable field of every feature type takes back its own value", "[Mcp][CAD]")
{
    std::vector<CadFeature> features;
    for (int t = 0; t <= int(CadFeatureType::Mate); ++t) {
        CadFeature f;
        f.type = CadFeatureType(t);
        if (f.type == CadFeatureType::Boolean) f.mode = BooleanMode::Cut;   // a Boolean never makes a new body
        features.push_back(f);
    }
    // Imported art, whose placement edits, and a Text feature, whose words, font and height edit too.
    CadFeature art;
    art.imported_regions = {{{Vec2d(0, 0), Vec2d(4, 0), Vec2d(0, 3)}}};
    art.import_offset    = Vec2d(5, -2);
    features.push_back(art);
    CadFeature text  = art;
    text.text_string = "A";
    text.text_font   = "a stored font";
    text.text_height = 12;
    features.push_back(text);
    for (const CadFeature& f : features) {
        CadDocument doc;
        doc.features.push_back(f);
        const json card = GUI::Mcp::cad_feature_json(doc, 0);
        INFO(card.at("type").get<std::string>() << (f.is_text() ? " (text)" : !f.imported_regions.empty() ? " (art)" : ""));
        if (!f.imported_regions.empty()) {
            const json& editable = card.at("editable");
            for (const char* key : {"import_offset", "import_scale_x", "import_scale_y"})
                CHECK(std::find(editable.begin(), editable.end(), json(key)) != editable.end());
            for (const char* key : {"text_string", "text_font", "text_height"})
                CHECK((std::find(editable.begin(), editable.end(), json(key)) != editable.end()) == f.is_text());
        }
        // What the result does to the bodies is read under the commands' word for it, operation, in their words.
        using T = CadFeatureType;
        const bool operates = f.type == T::Extrude || f.type == T::Revolve || f.type == T::Sweep || f.type == T::Loft ||
                              f.type == T::Boolean || f.type == T::Mirror;
        CHECK(card.at("params").contains("operation") == operates);
        CHECK_FALSE(card.at("params").contains("mode"));
        if (operates)
            CHECK(card.at("params").at("operation") == (f.type == T::Boolean ? "cut" : "new_body"));

        // A numeric field with a range is one an expression can drive too, and the card edits it.
        for (const auto& [key, range] : card.at("ranges").items()) {
            INFO(key);
            CHECK(CadDocument::is_bindable_field(key));
            CHECK(std::find(card.at("editable").begin(), card.at("editable").end(), json(key)) != card.at("editable").end());
            CHECK(range[0].get<double>() <= card.at("params").at(key).get<double>());
            CHECK(card.at("params").at(key).get<double>() <= range[1].get<double>());
        }
        for (const json& key : card.at("editable")) {
            const std::string k = key.get<std::string>();
            INFO(k);
            CadFeature g = doc.features[0];
            REQUIRE_NOTHROW(GUI::Mcp::cad_apply_feature_fields(g, {{k, card.at("params").at(k)}}));
            CadDocument after;
            after.features.push_back(g);
            CHECK(GUI::Mcp::cad_feature_json(after, 0).at("params") == card.at("params"));
        }
    }
}

TEST_CASE("feature_edit writes literal fields and the card's choices", "[Mcp][CAD]")
{
    CadFeature ex;
    ex.type = CadFeatureType::Extrude;
    GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", 15}, {"operation", "cut"}, {"flip", true}, {"extrude_end", "symmetric"}});
    CHECK(ex.distance == 15.0);
    CHECK(ex.mode == BooleanMode::Cut);
    CHECK(ex.flip);
    CHECK(ex.extrude_end == ExtrudeEnd::Symmetric);

    // A Boolean's and a Mirror's operation, in the words their cards offer.
    CadFeature boolean;
    boolean.type = CadFeatureType::Boolean;
    GUI::Mcp::cad_apply_feature_fields(boolean, {{"operation", "intersect"}});
    CHECK(boolean.mode == BooleanMode::Intersect);
    CadFeature mirror;
    mirror.type = CadFeatureType::Mirror;
    GUI::Mcp::cad_apply_feature_fields(mirror, {{"operation", "join"}});
    CHECK(mirror.mode == BooleanMode::Add);

    // A hole's seat, an align's kind and a pattern's layout are its type, in the words its command takes.
    CadFeature hole;
    hole.type = CadFeatureType::Hole;
    GUI::Mcp::cad_apply_feature_fields(hole, {{"type", "countersink"}});
    CHECK(hole.hole_style == 2);
    CadFeature mate;
    mate.type = CadFeatureType::Mate;
    GUI::Mcp::cad_apply_feature_fields(mate, {{"type", "revolute"}});
    CHECK(mate.mate_kind == 2);
    CadFeature pattern;
    pattern.type = CadFeatureType::Pattern;
    GUI::Mcp::cad_apply_feature_fields(pattern, {{"type", "circular"}});
    CHECK(pattern.pattern_circular);

    CadFeature th;
    th.type = CadFeatureType::Thread;
    GUI::Mcp::cad_apply_feature_fields(th, {{"thread_diameter", 8}});
    CHECK(th.thread_radius == 4.0);   // the card's diameter, halved into the kernel's radius

    CadFeature rv;
    rv.type                = CadFeatureType::Revolve;
    rv.revolve_axis_entity = 2;
    GUI::Mcp::cad_apply_feature_fields(rv, {{"revolve_axis", 1}});
    CHECK(rv.revolve_axis == 1);
    CHECK(rv.revolve_axis_entity == -1);   // a plane axis replaces a sketch-line axis, as on the card

    CadFeature sk;   // a shape sketch, as the Sketch card makes it
    GUI::Mcp::cad_apply_feature_fields(sk, {{"shape", "circle"}, {"radius", 7.5}});
    CHECK(sk.shape == SketchShape::Circle);
    CHECK(sk.radius == 7.5);

    // Imported art moves by its placement; a Text feature takes its words, font and height, which feature_edit then
    // outlines again in the panel.
    CadFeature art;
    art.imported_regions = {{{Vec2d(0, 0), Vec2d(4, 0), Vec2d(0, 3)}}};
    GUI::Mcp::cad_apply_feature_fields(art, {{"import_offset", {12.5, -3}}});
    CHECK((art.import_offset - Vec2d(12.5, -3)).norm() < 1e-12);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(art, {{"import_offset", {1}}}); }), ContainsSubstring("import_offset must be [x, y]"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(art, {{"text_string", "B"}}); }),
               ContainsSubstring("'text_string' is not an editable field"));
    CadFeature text  = art;
    text.text_string = "A";
    text.text_height = 10;
    GUI::Mcp::cad_apply_feature_fields(text, {{"text_string", "AB"}, {"text_height", 20}, {"text_font", "Arial"}});
    CHECK(text.text_string == "AB");
    CHECK_THAT(text.text_height, WithinAbs(20.0, 1e-12));
    CHECK(text.text_font == "Arial");
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(text, {{"text_height", 0.1}}); }), ContainsSubstring("text_height must be 0.5 to 500 mm"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(text, {{"text_string", ""}}); }), ContainsSubstring("text_string must be the words"));
    CHECK(text.text_string == "AB");
}

TEST_CASE("feature_edit refuses what the card would not produce, changing nothing", "[Mcp][CAD]")
{
    CadDocument doc;
    add_bound_block(doc);
    CadFeature ex = doc.features[1];
    ex.expr.clear();

    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[1], {{"distance", 15}}); }),
               ContainsSubstring("driven by the expression 'h'"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[1], {{"distance", 15}}); }),
               ContainsSubstring("remove the binding with feature_set_expression {field: 'distance', expr: ''}"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"depth", 1}}); }),
               ContainsSubstring("editable: distance, distance2, taper_deg, extrude_end, flip, operation"));
    // The member's own name is no field: the card's choice goes by the commands' word.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"mode", "cut"}}); }),
               ContainsSubstring("'mode' is not an editable field"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"sketch_ref", 2}}); }),
               ContainsSubstring("'sketch_ref' is not an editable field"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[0], {{"width", 30}}); }),
               ContainsSubstring("no field feature_edit can change"));   // an entity sketch
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", 2000}}); }),
               ContainsSubstring("distance must be between -1000 and 1000"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", "15"}}); }), ContainsSubstring("must be a number"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"operation", "add"}}); }),
               ContainsSubstring("operation must be one of new_body, join, cut, intersect"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"extrude_end", "up_to_face"}}); }),
               ContainsSubstring("needs a target face"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, json::object()); }), ContainsSubstring("fields must name"));

    CadFeature boolean;
    boolean.type = CadFeatureType::Boolean;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(boolean, {{"operation", "new_body"}}); }),
               ContainsSubstring("join, cut or intersect"));
    CadFeature mirror;
    mirror.type = CadFeatureType::Mirror;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(mirror, {{"operation", "cut"}}); }),
               ContainsSubstring("new_body or join"));
    // The members that hold a variant are no fields: their command's type is.
    const std::vector<std::pair<CadFeatureType, const char*>> members = {
        {CadFeatureType::Hole, "hole_style"}, {CadFeatureType::Mate, "mate_kind"}, {CadFeatureType::Pattern, "pattern_circular"}};
    for (const auto& member : members) {
        const std::string name = member.second;
        INFO(name);
        CadFeature f;
        f.type = member.first;
        CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(f, {{name, 1}}); }),
                   ContainsSubstring("'" + name + "' is not an editable field"));
    }
    CadFeature hole;
    hole.type = CadFeatureType::Hole;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(hole, {{"type", "tapped"}}); }),
               ContainsSubstring("type must be one of simple, counterbore, countersink"));
    // A curve pattern's layout is the curve it follows, which no field changes; a row or a circle is no curve.
    CadFeature on_curve;
    on_curve.type                 = CadFeatureType::Pattern;
    on_curve.pattern_curve_sketch = 0;
    CadDocument curve_doc;
    curve_doc.features.push_back(on_curve);
    CHECK(GUI::Mcp::cad_feature_json(curve_doc, 0).at("params").at("type") == "curve");
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(on_curve, {{"type", "linear"}}); }),
               ContainsSubstring("'type' is not an editable field"));
    CadFeature linear;
    linear.type = CadFeatureType::Pattern;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(linear, {{"type", "curve"}}); }),
               ContainsSubstring("linear or circular"));
    CadFeature thread;
    thread.type = CadFeatureType::Thread;
    thread.expr["thread_diameter"] = "d";
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(thread, {{"thread_radius", 3}}); }),
               ContainsSubstring("thread_diameter"));   // one value: a binding on either drives both
    CadFeature pattern;
    pattern.type = CadFeatureType::Pattern;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(pattern, {{"pattern_count", 2.5}}); }),
               ContainsSubstring("whole number"));

    // Fields are checked before any is written: a refused one leaves the others unwritten too.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", 15}, {"flip", "yes"}}); }),
               ContainsSubstring("flip must be true or false"));
    CHECK_THAT(ex.distance, WithinAbs(12.0, 1e-9));
}

TEST_CASE("a literal edit through replace_feature rebuilds, and undo takes it back", "[Mcp][CAD]")
{
    CadDocument doc;
    const int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Sketch");
    const int ex = doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    CadFeature edited = doc.features[ex];
    GUI::Mcp::cad_apply_feature_fields(edited, {{"distance", 15}});
    doc.checkpoint();
    REQUIRE(doc.replace_feature(ex, edited));
    CHECK(doc.features[ex].sketch_ref == sk);
    CHECK_THAT(doc.body_mass_properties(0).volume, WithinAbs(6000.0, 1e-6));

    REQUIRE(doc.undo());
    CHECK_THAT(doc.body_mass_properties(0).volume, WithinAbs(4000.0, 1e-6));
}

TEST_CASE("a refused literal edit leaves the history as it found it", "[Mcp][CAD]")
{
    CadDocument doc;
    const int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    const int hole = doc.add_hole(4, 10, true, 0, 0, SketchPlane::XY(), "Hole");
    REQUIRE(doc.recompute());
    // An edit undone, so there is a redo branch to keep.
    doc.checkpoint();
    doc.features[hole].hole_diameter = 6;
    REQUIRE(doc.recompute());
    REQUIRE(doc.undo());
    const size_t depth = doc.undo_depth();

    // feature_edit's sequence for an edit the kernel refuses: a hole that misses the body.
    CadFeature edited = doc.features[hole];
    GUI::Mcp::cad_apply_feature_fields(edited, {{"hole_x", 500}});
    doc.checkpoint();
    REQUIRE_FALSE(doc.replace_feature(hole, edited));
    CHECK_THAT(doc.error, ContainsSubstring("no material"));
    REQUIRE(doc.rollback());
    CHECK(doc.undo_depth() == depth);
    CHECK(doc.features[hole].hole_x == 0.0);
    REQUIRE(doc.redo());
    CHECK(doc.features[hole].hole_diameter == 6.0);
}

TEST_CASE("variables evaluate in dependency order, and a cycle is reported", "[Mcp][CAD]")
{
    CadDocument doc;
    doc.variables = {{"a", "2"}, {"b", "a * 3"}};
    std::string err;
    const std::map<std::string, double> values = doc.variable_values(err);
    CHECK(err.empty());
    CHECK_THAT(values.at("b"), WithinAbs(6.0, 1e-12));

    doc.variables = {{"a", "b + 1"}, {"b", "a"}};
    CHECK(doc.variable_values(err).empty());
    CHECK_THAT(err, ContainsSubstring("cycle"));
}

TEST_CASE("literal edits and bindings reach the same numeric fields", "[Mcp][CAD]")
{
    CadFeature f;
    CadDocument::set_numeric_field(f, "hole_diameter", 7.0);
    CHECK(f.hole_diameter == 7.0);
    CadDocument::set_numeric_field(f, "thread_diameter", 6.0);
    CHECK(f.thread_radius == 3.0);
    CadDocument::set_numeric_field(f, "pattern_count", 4.4);
    CHECK(f.pattern_count == 4);
    CHECK_THROWS_AS(CadDocument::set_numeric_field(f, "sketch_ref", 1.0), std::runtime_error);
    CHECK(CadDocument::is_bindable_field("hole_diameter"));
    CHECK_FALSE(CadDocument::is_bindable_field("sketch_ref"));
}

TEST_CASE("commit_to_plate, new_design and export_step change the design or the plate, and fail as errors", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"commit_to_plate", "new_design", "export_step"}) {
        INFO(key);
        const GUI::NativeCommand& c = command_of(catalog, key);
        CHECK_FALSE(c.read_only);
        // A failure is the call's error, not a reply whose ok is false.
        CHECK_THAT(c.description, !ContainsSubstring("ok is false"));
    }
    // export_step's path has no default, so it is required.
    CHECK(required_of(command_of(catalog, "export_step")) == std::vector<std::string>{"path"});
}

TEST_CASE("commit_to_plate's replace takes distinct object indices that are not part of a cut", "[Mcp][CAD]")
{
    Model model;
    for (int i = 0; i < 3; ++i) model.add_object()->name = "object " + std::to_string(i);
    model.objects[2]->cut_id.init();

    CHECK(GUI::Mcp::cad_commit_to_plate_replace(json::array(), model).empty());
    CHECK(GUI::Mcp::cad_commit_to_plate_replace({1, 0}, model) == std::vector<int>{0, 1});

    auto refusal = [&](const json& replace) { return failure([&] { GUI::Mcp::cad_commit_to_plate_replace(replace, model); }); };
    CHECK_THAT(refusal(0), ContainsSubstring("list of object indices"));
    CHECK_THAT(refusal({3}), ContainsSubstring("the project has 3 objects"));
    CHECK_THAT(refusal({-1}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({0, 0}), ContainsSubstring("listed twice"));
    CHECK_THAT(refusal({0.5}), ContainsSubstring("not 0.5"));
    CHECK_THAT(refusal({"0"}), ContainsSubstring("object indices"));
    CHECK_THAT(refusal({0, 2}), ContainsSubstring("part of a cut object"));
    CHECK(failure_code([&] { GUI::Mcp::cad_commit_to_plate_replace({0, 2}, model); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_commit_to_plate_replace({3}, model); }) == "invalid_argument");
}

TEST_CASE("export_step's STEP file reads back with each body where the Move gizmo put it", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const GeometryEngine::MassProps box = GeometryEngine::mass_properties(doc.bodies.at(0).shape);
    const boost::filesystem::path path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mcp_cad_%%%%%%%%.step");

    auto read_back = [&](const std::vector<Transform3d>& xforms) {
        std::string err;
        REQUIRE(doc.export_step(path.string(), xforms, err));
        CHECK(boost::filesystem::file_size(path) > 0);
        const std::vector<TopoDS_Shape> solids = GeometryEngine::read_step_solids(path.string(), err);
        boost::filesystem::remove(path);
        REQUIRE(solids.size() == 1);
        return GeometryEngine::mass_properties(solids[0]);
    };

    const GeometryEngine::MassProps same = read_back({Transform3d::Identity()});
    CHECK_THAT(same.volume, WithinAbs(box.volume, box.volume * 1e-4));
    CHECK((same.center_of_mass - box.center_of_mass).norm() < 1e-6);

    Transform3d moved = Transform3d::Identity();
    moved.translate(Vec3d(50, 0, 0));
    const GeometryEngine::MassProps shifted = read_back({moved});
    CHECK_THAT(shifted.volume, WithinAbs(box.volume, box.volume * 1e-4));
    CHECK((shifted.center_of_mass - box.center_of_mass - Vec3d(50, 0, 0)).norm() < 1e-6);
}

TEST_CASE("the tree and body edits change the design, fail as errors and require what they edit", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"feature_delete", "feature_move", "feature_suppress", "feature_rename", "body_set", "body_delete"}) {
        INFO(key);
        CHECK_FALSE(command_of(catalog, key).read_only);
        CHECK_THAT(command_of(catalog, key).description, !ContainsSubstring("ok is false"));
    }
    // The params each one requires.
    CHECK(required_of(command_of(catalog, "feature_delete")) == std::vector<std::string>{"feature"});
    CHECK(required_of(command_of(catalog, "feature_move")) == std::vector<std::string>{"feature", "delta"});
    CHECK(required_of(command_of(catalog, "feature_suppress")) == std::vector<std::string>{"feature", "suppressed"});
    CHECK(required_of(command_of(catalog, "feature_rename")) == std::vector<std::string>{"feature", "name"});
    CHECK(required_of(command_of(catalog, "body_set")) == std::vector<std::string>{"body"});
    CHECK(required_of(command_of(catalog, "body_delete")) == std::vector<std::string>{"body"});
}

TEST_CASE("abandon_checkpoint drops the refused edit's undo step and keeps what was redoable", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    doc.checkpoint();
    doc.features[1].distance = 20.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.undo());
    const size_t depth = doc.undo_depth();

    doc.checkpoint();   // an edit that then put everything back itself
    doc.abandon_checkpoint();
    CHECK(doc.undo_depth() == depth);
    REQUIRE(doc.redo());
    CHECK_THAT(doc.features[1].distance, WithinAbs(20.0, 1e-9));

    // With no checkpoint there is nothing to drop.
    CadDocument empty;
    empty.abandon_checkpoint();
    CHECK(empty.undo_depth() == 0);
}

TEST_CASE("a tree edit the kernel refuses leaves the features and the history as they were", "[Mcp][CAD]")
{
    // As the panel's tree edits run them: one checkpoint, the kernel op, and abandon_checkpoint()
    // when the op refuses. The step undone first must still be there to redo.
    CadDocument doc;
    const int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Sketch");
    const int ex = doc.add_extrude(sk, 5, false, BooleanMode::New, "Extrude");
    const int ch = doc.add_chamfer(1.0, FaceGroup::All, "Chamfer");
    doc.features[ch].target_body = 0;
    REQUIRE(doc.recompute());
    doc.checkpoint();
    doc.features[ch].name = "Edge break";
    REQUIRE(doc.undo());
    const size_t depth = doc.undo_depth();

    SECTION("the extrude cannot come before the sketch it extrudes")
    {
        doc.checkpoint();
        REQUIRE_FALSE(doc.move_feature(ex, -1));
        doc.abandon_checkpoint();
        CHECK_THAT(doc.error, ContainsSubstring("cannot come before"));
        CHECK(doc.features[sk].type == CadFeatureType::Sketch);
        CHECK(doc.features[ex].sketch_ref == sk);
        CHECK(doc.bodies.size() == 1);
    }
    SECTION("the chamfer cannot be shown while the extrude that made its body is hidden")
    {
        REQUIRE(doc.set_feature_enabled(ch, false));
        REQUIRE(doc.set_feature_enabled(ex, false));
        doc.checkpoint();
        REQUIRE_FALSE(doc.set_feature_enabled(ch, true));
        doc.abandon_checkpoint();
        CHECK_FALSE(doc.features[ch].enabled);
        CHECK(doc.bodies.empty());
    }
    CHECK(doc.undo_depth() == depth);
    REQUIRE(doc.redo());
    CHECK(doc.features[ch].name == "Edge break");
}

TEST_CASE("deleting a sketch takes the features built on it, as one undo step", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    doc.checkpoint();
    REQUIRE(doc.remove_feature(0));
    CHECK(doc.features.empty());
    CHECK(doc.bodies.empty());
    REQUIRE(doc.undo());
    CHECK(doc.features.size() == 2);
    CHECK(doc.bodies.size() == 1);
}

namespace {

SketchEntity circle(double cx, double cy, double r)
{
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = e.p0 = Vec2d(cx, cy);
    e.radius = r;
    return e;
}

// A shelf-pin jig plate: a 160 x 60 rectangle centred on the plane's origin with five 5.2 mm holes
// on 32 mm centres along its middle.
std::vector<SketchEntity> jig_plate()
{
    std::vector<SketchEntity> ents = {line(-80, -30, 80, -30), line(80, -30, 80, 30), line(80, 30, -80, 30), line(-80, 30, -80, -30)};
    for (int i = -2; i <= 2; ++i) ents.push_back(circle(32.0 * i, 0, 2.6));
    return ents;
}

const double kHoleArea  = PI * 2.6 * 2.6;
const double kJigVolume = (160.0 * 60.0 - 5 * kHoleArea) * 8.0;

double body_volume(const CadDocument& doc, size_t body) { return GeometryEngine::mass_properties(doc.bodies.at(body).shape).volume; }

// The loop that has the other loops of the jig plate inside it.
int plate_loop(const GUI::DesignSketchTool& tool, const std::vector<SketchEntity>& ents)
{
    const GUI::DesignSketchTool::LoopReport rep = tool.loop_report(ents);
    for (int i = 0; i < int(rep.loops.size()); ++i)
        if (rep.loops[i].holes.size() == 5) return i;
    return -1;
}

} // namespace

TEST_CASE("a committed sketch's loops are numbered as extrude's loop takes them", "[Mcp][CAD]")
{
    CadDocument doc;
    doc.modeling_origin = Vec3d(100, 50, 0);
    const std::vector<SketchEntity> ents = jig_plate();
    SketchPlane plane = SketchPlane::XY();
    plane.origin += doc.modeling_origin;
    const int sk = doc.add_sketch_entities(ents, plane, "Sketch 1");
    REQUIRE(doc.recompute());
    GUI::DesignSketchTool tool;

    // Six loops: the plate, which lists the five discs as its holes, and each disc.
    const GUI::DesignSketchTool::LoopReport rep = tool.loop_report(ents);
    REQUIRE(rep.loops.size() == 6);
    const int plate = plate_loop(tool, ents);
    REQUIRE(plate >= 0);
    CHECK_THAT(rep.loops[plate].area, WithinAbs(160.0 * 60.0, 1e-6));
    // Each loop's entities, then its holes' entities: the list extrude {loop} hands the kernel.
    const std::vector<std::vector<int>> regions = tool.region_entity_indices_with_holes(ents);
    REQUIRE(regions.size() == rep.loops.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        std::vector<int> expect = rep.loops[i].ents;
        for (int h : rep.loops[i].holes) expect.insert(expect.end(), rep.loops[h].ents.begin(), rep.loops[h].ents.end());
        CHECK(regions[i] == expect);
    }

    const json described = GUI::Mcp::cad_sketch_describe(doc, tool, sk);
    CHECK(described.at("name") == "Sketch 1");
    CHECK(described.at("entities").size() == 9);
    CHECK(described.at("constraints") == 0);
    CHECK(described.at("plane").at("base") == "XY");
    CHECK(described.at("buildable") == true);
    REQUIRE(described.at("closed_loops").size() == 6);
    for (int i = 0; i < 6; ++i) CHECK(described.at("closed_loops")[i].at("index") == i);
    CHECK(described.at("closed_loops")[plate].at("holes").size() == 5);

    // Not a sketch, or one with no entities to describe.
    const int ex = doc.add_extrude(sk, 8.0, false, BooleanMode::New, "Extrude 2");
    const int box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Sketch 3");
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_describe(doc, tool, ex); }), ContainsSubstring("is not a sketch"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_describe(doc, tool, box); }), ContainsSubstring("not drawn from entities"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_describe(doc, tool, 9); }), ContainsSubstring("out of range"));
}

TEST_CASE("extrude takes a committed sketch whole, or one loop with the loops inside it", "[Mcp][CAD]")
{
    CadDocument doc;
    const std::vector<SketchEntity> ents = jig_plate();
    const int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "Sketch 1");
    REQUIRE(doc.recompute());
    GUI::DesignSketchTool tool;
    const int plate = plate_loop(tool, ents);
    REQUIRE(plate >= 0);

    SECTION("the whole sketch is the plate with its holes, a new body as nothing is touched")
    {
        const int e = GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", sk}, {"distance", 8}})("Extrude 2");
        CHECK(doc.features[e].sketch_ref == sk);
        CHECK(doc.features[e].mode == BooleanMode::New);
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 1);
        CHECK_THAT(body_volume(doc, 0), WithinAbs(kJigVolume, 1e-3));
    }
    SECTION("the plate's loop carries its holes; a hole's loop is the disc")
    {
        GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", sk}, {"loop", plate}, {"distance", 8}})("Extrude 2");
        const int disc = plate == 0 ? 1 : 0;
        const json disc_only = {{"sketch", sk}, {"loop", disc}, {"distance", 8}, {"operation", "new_body"}};
        const int  e         = GUI::Mcp::cad_sketch_extrude(doc, tool, disc_only)("Extrude 3");
        CHECK(doc.features[e].sketch_ref == -1);   // the loop travels with the feature
        CHECK(doc.features[e].entities.size() == 1);
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 2);
        CHECK_THAT(body_volume(doc, 0), WithinAbs(kJigVolume, 1e-3));
        CHECK_THAT(body_volume(doc, 1), WithinAbs(kHoleArea * 8.0, 1e-3));
    }
}

TEST_CASE("extrude's auto boolean joins the body the sketch touches, as the card's default", "[Mcp][CAD]")
{
    CadDocument doc;
    GUI::DesignSketchTool tool;
    const int sk = doc.add_sketch_entities(jig_plate(), SketchPlane::XY(), "Sketch 1");
    GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", sk}, {"distance", 8}})("Extrude 2");
    REQUIRE(doc.recompute());

    // A lip on the plate's top joins the plate.
    SketchPlane top = SketchPlane::XY();
    top.origin.z()  = 8;
    const int lip   = doc.add_sketch_entities({line(-80, 20, 80, 20), line(80, 20, 80, 30), line(80, 30, -80, 30), line(-80, 30, -80, 20)},
                                              top, "Sketch 3");
    const int e     = GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", lip}, {"distance", 10}})("Extrude 4");
    CHECK(doc.features[e].mode == BooleanMode::Add);
    CHECK(doc.features[e].target_body == 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    CHECK_THAT(body_volume(doc, 0), WithinAbs(kJigVolume + 160.0 * 10.0 * 10.0, 1e-3));

    // A sketch in free space starts a new body, and an explicit mode is kept.
    SketchPlane above = SketchPlane::XY();
    above.origin.z()  = 50;
    const int loose   = doc.add_sketch(SketchShape::Rectangle, above, 10, 10, 0, "Sketch 5");
    CHECK(doc.features[GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", loose}})("Extrude 6")].mode == BooleanMode::New);
    const int cut = GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", lip}, {"operation", "cut"}, {"distance", 2}})("Extrude 7");
    CHECK(doc.features[cut].mode == BooleanMode::Cut);
    CHECK(doc.features[cut].target_body == 0);
}

TEST_CASE("extrude refuses what the sketch fixes and what is not one of its loops, changing nothing", "[Mcp][CAD]")
{
    CadDocument doc;
    GUI::DesignSketchTool tool;
    const int sk  = doc.add_sketch_entities(jig_plate(), SketchPlane::XY(), "Sketch 1");
    const int box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Sketch 2");
    const int hole = doc.add_hole(5, 5, true, 0, 0, SketchPlane::XY(), "Hole 3");
    const size_t features = doc.features.size();
    auto refusal = [&](json params) {
        return failure([&] { GUI::Mcp::cad_sketch_extrude(doc, tool, params); });
    };

    for (const char* fixed : {"plane", "width", "height"})
        CHECK_THAT(refusal({{"sketch", sk}, {fixed, fixed == std::string("plane") ? json("XZ") : json(20)}}),
                   ContainsSubstring("cannot be given with sketch"));
    CHECK_THAT(refusal({{"sketch", sk}, {"profile", {{0, 0}, {1, 0}, {0, 1}}}}), ContainsSubstring("cannot be given with sketch"));
    CHECK_THAT(refusal({{"sketch", hole}}), ContainsSubstring("(Hole) is not a sketch"));
    CHECK_THAT(refusal({{"sketch", 7}}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({{"sketch", "0"}}), ContainsSubstring("sketch must be a feature index"));
    CHECK_THAT(refusal({{"sketch", sk}, {"loop", 6}}), ContainsSubstring("loop 6 does not exist; sketch 0 has 6 closed loops"));
    CHECK_THAT(refusal({{"sketch", sk}, {"loop", -1}}), ContainsSubstring("does not exist"));
    CHECK_THAT(refusal({{"sketch", box}, {"loop", 0}}), ContainsSubstring("not drawn from entities"));
    CHECK_THAT(refusal({{"sketch", sk}, {"operation", "atuo"}}), ContainsSubstring("operation must be auto, new_body"));
    CHECK_THAT(refusal({{"sketch", sk}, {"body", 3}}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({{"sketch", sk}, {"distance", 0}}), ContainsSubstring("distance must be > 0"));
    CHECK_THAT(refusal({{"sketch", sk}, {"end", "up_to_vertex"}}), ContainsSubstring("end must be"));
    // The row's empty profile means none.
    CHECK(refusal({{"sketch", sk}, {"profile", json::array()}}).empty());
    // Checking adds nothing; only the call it returns does.
    CHECK(doc.features.size() == features);
}

TEST_CASE("revolve takes a line of the sketch, or its only centerline, as the axis", "[Mcp][CAD]")
{
    // A 10 x 10 profile 10 mm off a centerline along the XZ plane's y axis (world Z): a ring.
    CadDocument doc;
    SketchEntity axis_line = line(0, 0, 0, 30);
    axis_line.construction = true;
    std::vector<SketchEntity> ents = {line(10, 0, 20, 0), line(20, 0, 20, 10), line(20, 10, 10, 10), line(10, 10, 10, 0), axis_line};
    const int sk = doc.add_sketch_entities(ents, SketchPlane::XZ(), "Sketch 1");
    REQUIRE(doc.recompute());
    const double ring = PI * (20.0 * 20.0 - 10.0 * 10.0) * 10.0;
    auto revolve = [&](json params) {
        params["sketch"] = sk;
        return GUI::Mcp::cad_sketch_revolve(doc, params)("Revolve");
    };

    SECTION("its only centerline by default")
    {
        const int r = revolve(json::object());
        CHECK(doc.features[r].revolve_axis_entity == 4);
        REQUIRE(doc.recompute());
        CHECK_THAT(body_volume(doc, 0), WithinAbs(ring, ring * 1e-6));
    }
    SECTION("the plane axis when one is asked for")
    {
        const int r = revolve({{"axis", 1}});
        CHECK(doc.features[r].revolve_axis_entity == -1);
        CHECK(doc.features[r].revolve_axis == 1);
        REQUIRE(doc.recompute());
        CHECK_THAT(body_volume(doc, 0), WithinAbs(ring, ring * 1e-6));
    }
    SECTION("a named line, or -1 for the plane axis")
    {
        CHECK(doc.features[revolve({{"axis_entity", 3}})].revolve_axis_entity == 3);
        CHECK(doc.features[revolve({{"axis_entity", -1}})].revolve_axis_entity == -1);
    }
    SECTION("no default when the sketch has two centerlines")
    {
        doc.features[sk].entities[0].construction = true;
        CHECK(doc.features[revolve(json::object())].revolve_axis_entity == -1);
    }
    SECTION("refusals")
    {
        const size_t features = doc.features.size();
        CHECK_THAT(failure([&] { revolve({{"axis_entity", 9}}); }), ContainsSubstring("axis_entity 9 is not a line of sketch 0"));
        CHECK_THAT(failure([&] { revolve({{"axis", 2}}); }), ContainsSubstring("axis must be 0"));
        CHECK_THAT(failure([&] { revolve({{"operation", "join"}}); }), ContainsSubstring("needs a body"));
        CHECK_THAT(failure([&] { revolve({{"plane", "XY"}}); }), ContainsSubstring("cannot be given with sketch"));
        CHECK(doc.features.size() == features);
    }
}

TEST_CASE("revolve without a sketch turns a width by height rectangle lying beside the axis", "[Mcp][CAD]")
{
    // Width runs along the axis and height out from it, so a full turn about either plane axis is a
    // cylinder `width` long of radius `height`.
    const int axis = GENERATE(0, 1);
    INFO("axis " << axis);
    auto revolved_volume = [&](const json& params) {
        CadDocument doc;
        const int sk = doc.add_sketch_profile(GUI::Mcp::cad_revolve_profile(params), SketchPlane::XY(), "Sketch 1");
        doc.add_revolve(sk, 360.0, axis, false, BooleanMode::New, "Revolve");
        REQUIRE(doc.recompute());
        return body_volume(doc, 0);
    };

    const double w = 30, h = 8;
    const double cylinder = PI * h * h * w;
    CHECK_THAT(revolved_volume({{"axis", axis}, {"width", w}, {"height", h}}), WithinAbs(cylinder, cylinder * 1e-6));
    // The row's defaults, 20 x 10.
    const double fallback = PI * 10.0 * 10.0 * 20.0;
    CHECK_THAT(revolved_volume({{"axis", axis}}), WithinAbs(fallback, fallback * 1e-6));
    // An explicit profile is turned as given, offset from the axis: a 10 x 10 square 10 mm out.
    const json   square = axis == 0 ? json::array({{0, 10}, {10, 10}, {10, 20}, {0, 20}})
                                    : json::array({{10, 0}, {20, 0}, {20, 10}, {10, 10}});
    const double ring   = PI * (20.0 * 20.0 - 10.0 * 10.0) * 10.0;
    CHECK_THAT(revolved_volume({{"axis", axis}, {"profile", square}}), WithinAbs(ring, ring * 1e-6));

    CHECK_THAT(failure([&] { GUI::Mcp::cad_revolve_profile({{"axis", axis}, {"height", 0}}); }),
               ContainsSubstring("width and height must be > 0"));
}

TEST_CASE("the sketch-to-solid rows take a sketch, and sketch_create takes every plane form", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"sketch_create", "sketch_describe", "extrude", "revolve"}) {
        INFO(key);
        CHECK_THAT(command_of(catalog, key).description, !ContainsSubstring("ok is false"));
    }
    CHECK(command_of(catalog, "sketch_describe").read_only);   // reads, so it also answers under an open card
    CHECK(property_of(command_of(catalog, "sketch_create"), "plane").at("type") == json::array({"string", "object"}));
    // Optional everywhere: none of these is required.
    CHECK(required_of(command_of(catalog, "sketch_describe")).empty());
    CHECK(required_of(command_of(catalog, "extrude")).empty());
    CHECK(required_of(command_of(catalog, "revolve")).empty());
    for (const char* key : {"sketch", "loop", "body"}) {
        INFO(key);
        CHECK(property_of(command_of(catalog, "extrude"), key).is_object());
    }
    for (const char* key : {"sketch", "axis_entity", "body"}) {
        INFO(key);
        CHECK(property_of(command_of(catalog, "revolve"), key).is_object());
    }
}

TEST_CASE("sketch_edit re-opens a committed sketch by its index, as a sketch command", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& command = command_of(catalog, "sketch_edit");
    const GUI::DesignTool&    entry   = design_row("sketch_edit");
    // A command with no menu item of its own: no menu item, no Speed Dial action and no person's form.
    CHECK(entry.row == -1);
    CHECK(entry.action == nullptr);
    CHECK_FALSE(command.has_own_action);
    CHECK_FALSE(two_forms(command));
    REQUIRE(entry.section != nullptr);
    CHECK(std::string(entry.section) == "Sketch");
    // Sketch access: it passes the gate while a sketch session is open, which it refuses itself, and it changes the
    // Design tab's state.
    CHECK(entry.access == GUI::CadAccess::Sketch);
    CHECK_FALSE(command.read_only);
    // The sketch, a feature index, is all it takes and is required.
    CHECK(required_of(command) == std::vector<std::string>{"sketch"});
    CHECK(property_of(command, "sketch").at("type") == "integer");
    json none = json::object();
    CHECK_THAT(GUI::Mcp::argument_error(command, none), ContainsSubstring("sketch is required"));
    json named = {{"sketch", "Sketch 1"}};
    CHECK_THAT(GUI::Mcp::argument_error(command, named), ContainsSubstring("sketch must be of type"));
    json index = {{"sketch", 0}};
    CHECK(GUI::Mcp::argument_error(command, index).empty());
    // What an agent must know before it edits: a one-loop extrude does not follow, and sketch_finish says it replaced.
    CHECK_THAT(command.description, ContainsSubstring("An extrude of one loop keeps its own copy of that loop and does not follow the edit"));
    CHECK_THAT(command_of(catalog, "sketch_finish").description, ContainsSubstring("replaced: true"));
    // A re-opened sketch whose replace the rebuild refuses ends its edit, which a replay must repeat, so sketch_finish
    // reports it in the reply rather than failing.
    CHECK(design_row("sketch_finish").ok_false);
    CHECK_THAT(command_of(catalog, "sketch_finish").description, ContainsSubstring("ok is false"));
    CHECK_THAT(command_of(catalog, "sketch_finish").description, ContainsSubstring("replies ok false, replaced false"));
}

namespace {

// The face of `body` whose outward normal is `n`.
int face_facing_on(const CadDocument& doc, int body, const Vec3d& n)
{
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(doc.bodies.at(body).shape);
    for (int i = 0; i < int(faces.size()); ++i)
        if ((GeometryEngine::face_normal_world(faces[i]) - n).norm() < 1e-6) return i;
    return -1;
}

// The circular edge of body 0 whose centre is at height z.
int circle_at(const CadDocument& doc, double z)
{
    const std::vector<TopoDS_Edge> edges = GeometryEngine::edges_of(doc.bodies.at(0).shape);
    for (int i = 0; i < int(edges.size()); ++i) {
        const GeometryEngine::CylinderFace c = GeometryEngine::circle_of_edge(edges[i]);
        if (c.ok && std::abs(c.base.z() - z) < 1e-6) return i;
    }
    return -1;
}

// The first face of body 0 that OCCT reads as a cylinder.
int cylinder_face(const CadDocument& doc)
{
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(doc.bodies.at(0).shape);
    for (int i = 0; i < int(faces.size()); ++i)
        if (GeometryEngine::cylinder_of_face(faces[i]).ok) return i;
    return -1;
}

// A cylinder of radius 10 from z 0 to 20 on the world XY plane.
void add_cylinder(CadDocument& doc)
{
    const int sk = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 10.0, "Sketch");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
}

const json& item_with(const json& list, const char* key, const json& value)
{
    for (const json& item : list)
        if (item.value(key, json()) == value) return item;
    FAIL("no item with " << key << " = " << value.dump());
    return list;
}

double vec_dist(const json& a, const Vec3d& b) { return (Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>()) - b).norm(); }

} // namespace

TEST_CASE("fillet and chamfer take an edge group by name", "[Mcp][CAD]")
{
    // A group is the card's Edges choice: the feature keeps it as its face_group, picks no edge, and builds the body
    // the engine's own face-group overload builds.
    for (const bool chamfer : {false, true}) {
        const char* size = chamfer ? "distance" : "radius";
        for (const auto& [word, group] : {std::pair<const char*, FaceGroup>{"top", FaceGroup::Top}, {"all", FaceGroup::All}}) {
            INFO((chamfer ? "chamfer " : "fillet ") << word);
            CadDocument doc;
            add_box(doc);
            const int f = GUI::Mcp::cad_dressup(doc, {{"edges", word}, {size, 1}}, chamfer)("Dress-up");
            CHECK(doc.features[f].type == (chamfer ? CadFeatureType::Chamfer : CadFeatureType::Fillet));
            CHECK(doc.features[f].face_group == group);
            CHECK(doc.features[f].dressup_edge_ids().empty());
            CHECK(GUI::Mcp::cad_feature_json(doc, f).at("refs").at("face_group") == word);
            REQUIRE(doc.recompute());
            CadDocument engine;
            add_box(engine);
            if (chamfer) engine.add_chamfer(1, group, "Chamfer");
            else         engine.add_fillet(1, group, "Fillet");
            REQUIRE(engine.recompute());
            CHECK(body_volume(doc, 0) < 40.0 * 20.0 * 10.0 - 1.0);   // the corners are taken off
            CHECK_THAT(body_volume(doc, 0), WithinAbs(body_volume(engine, 0), 1e-6));
        }
        // Any other word is refused, naming the four, before anything is added.
        CadDocument doc;
        add_box(doc);
        const std::size_t features = doc.features.size();
        CHECK_THAT(failure([&] { GUI::Mcp::cad_dressup(doc, {{"edges", "sideways"}, {size, 1}}, chamfer); }),
                   ContainsSubstring("edges must be an edge id, a list of them, or top, bottom, lateral or all"));
        CHECK(doc.features.size() == features);
    }
}

TEST_CASE("surface_revolve takes a line of the sketch as its axis, and flip", "[Mcp][CAD]")
{
    // On XZ: a profile line 10 mm out from a construction line along the plane's y axis (world Z), and in a sketch of
    // its own an arc.
    CadDocument  doc;
    SketchEntity axis_line = line(0, 0, 0, 30);
    axis_line.construction = true;
    const int    sk        = doc.add_sketch_entities({line(10, 0, 10, 20), axis_line}, SketchPlane::XZ(), "Sketch 1");
    SketchEntity arc;
    arc.type        = SketchEntity::Type::Arc;
    arc.center      = Vec2d(0, 0);
    arc.radius      = 5;
    arc.start_angle = 0;
    arc.end_angle   = PI / 2;
    arc.p0          = Vec2d(5, 0);
    arc.p1          = Vec2d(0, 5);
    const int arcs  = doc.add_sketch_entities({arc}, SketchPlane::XZ(), "Sketch 2");
    REQUIRE(doc.recompute());

    // The sketch's only centerline is the axis by default, as revolve's; flip turns the other way.
    const int f = GUI::Mcp::cad_surface_revolve(doc, {{"sketch", sk}, {"angle", 90}, {"flip", true}})("Surface Revolve");
    CHECK(doc.features[f].type == CadFeatureType::SurfaceRevolve);
    CHECK(doc.features[f].flip);
    CHECK(doc.features[f].revolve_axis_entity == 1);
    CHECK_THAT(doc.features[f].revolve_angle, WithinAbs(90.0, 1e-12));
    REQUIRE(doc.recompute());
    // A quarter of a cylinder's wall of radius 10 and height 20.
    CHECK_THAT(GeometryEngine::mass_properties(doc.bodies.at(0).shape).surface_area, WithinAbs(2 * PI * 10.0 * 20.0 / 4, 1e-3));
    // A plane axis asked for, or -1 for one, leaves the line.
    CHECK(doc.features[GUI::Mcp::cad_surface_revolve(doc, {{"sketch", sk}, {"axis", 1}})("Surface Revolve")].revolve_axis_entity == -1);
    CHECK(doc.features[GUI::Mcp::cad_surface_revolve(doc, {{"sketch", sk}, {"axis_entity", -1}})("Surface Revolve")]
              .revolve_axis_entity == -1);

    // Refused as revolve refuses them, before anything is added.
    const std::size_t features = doc.features.size();
    CHECK_THAT(failure([&] { GUI::Mcp::cad_surface_revolve(doc, {{"sketch", arcs}, {"axis_entity", 0}}); }),
               ContainsSubstring("axis_entity 0 is not a line of sketch 1"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_surface_revolve(doc, {{"sketch", sk}, {"axis", 2}}); }), ContainsSubstring("axis must be 0"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_surface_revolve(doc, {{"sketch", f}}); }), ContainsSubstring("is not a sketch"));
    CHECK(doc.features.size() == features);

    // feature_edit sets its flip and its plane axis, which drops a line chosen as the axis, as the card does.
    CadFeature edited = doc.features[f];
    GUI::Mcp::cad_apply_feature_fields(edited, {{"flip", false}, {"revolve_axis", 1}});
    CHECK_FALSE(edited.flip);
    CHECK(edited.revolve_axis == 1);
    CHECK(edited.revolve_axis_entity == -1);
}

TEST_CASE("list_align_types says which types fit a pair", "[Mcp][CAD]")
{
    // A coordinate system on the top face of the box and one on the top face of a block 50 mm beside it.
    CadDocument doc;
    add_box(doc);
    SketchPlane beside = SketchPlane::XY();
    beside.origin      = Vec3d(50, 0, 0);
    doc.add_extrude(doc.add_sketch(SketchShape::Rectangle, beside, 10, 10, 0, "Sketch"), 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);
    auto on_top = [&](int body, const char* name) {
        const int cs                   = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d::Zero(), name);
        doc.features[cs].coordsys_body = body;
        doc.features[cs].coordsys_face = face_facing_on(doc, body, Vec3d(0, 0, 1));
        return cs;
    };
    const int a = on_top(0, "CS A");
    const int b = on_top(1, "CS B");
    REQUIRE(doc.recompute());

    const json r = GUI::Mcp::cad_align_types(doc, {{"cs_a", a}, {"cs_b", b}});
    CHECK(r.at("ok") == true);
    CHECK(r.at("cs_a") == a);
    CHECK(r.at("cs_b") == b);
    const json& types = r.at("types");
    REQUIRE(types.size() == 5);
    const std::vector<std::string> words = {"fastened", "planar", "revolute", "slider", "cylindrical"};
    for (std::size_t k = 0; k < words.size(); ++k) {
        INFO(words[k]);
        CHECK(types[k].at("type") == words[k]);
        // Two flat faces take any type but those that turn about a cylinder's axis.
        const bool turns = words[k] == "revolute" || words[k] == "cylindrical";
        CHECK(types[k].at("fits") == !turns);
        if (turns)
            CHECK_THAT(types[k].at("reason").get<std::string>(), ContainsSubstring("cylindrical face"));
        else
            CHECK(types[k].at("reason").is_null());
    }
    // A coordinate system aligned to itself fits none.
    for (const json& t : GUI::Mcp::cad_align_types(doc, {{"cs_a", a}, {"cs_b", a}}).at("types")) {
        INFO(t.at("type"));
        CHECK(t.at("fits") == false);
        CHECK(t.at("reason").is_string());
    }
    CHECK_THAT(failure([&] { GUI::Mcp::cad_align_types(doc, {{"cs_a", a}}); }), ContainsSubstring("cs_b"));
}

namespace {

// The ids of body 0's upright edges, those whose ends lie one above the other.
std::vector<int> upright_edges(const CadDocument& doc)
{
    std::vector<int>               ids;
    const std::vector<TopoDS_Edge> edges = GeometryEngine::edges_of(doc.bodies.at(0).shape);
    for (int i = 0; i < int(edges.size()); ++i) {
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(edges[i]);
        if (pts.size() >= 2 && (pts.front() - pts.back()).head<2>().norm() < 1e-6 && std::abs(pts.front().z() - pts.back().z()) > 1.0)
            ids.push_back(i);
    }
    return ids;
}

// The add_box() box with one upright edge rounded 2 mm (the Fillet, feature 2), then shelled 1 mm open at the bottom
// (feature 3), which renumbers its faces and edges; `rounded` is the edge's id on the box.
CadDocument filleted_shelled_box(int& rounded)
{
    CadDocument doc;
    add_box(doc);
    rounded = upright_edges(doc).at(0);
    doc.add_fillet(2.0, rounded, "Fillet");
    REQUIRE(doc.recompute());
    doc.add_shell(1.0, face_facing(doc, Vec3d(0, 0, -1)), 0, "Shell");
    REQUIRE(doc.recompute());
    return doc;
}

} // namespace

TEST_CASE("feature_edit re-points what a feature refers to", "[Mcp][CAD]")
{
    // The box (features 0 and 1), a circle to sweep, upright paths 10 and 20 mm long, a Sweep of the circle along the
    // first (feature 5) and a sketch after it.
    CadDocument doc;
    add_box(doc);
    const int profile    = doc.add_sketch_entities({circle(0, 0, 2)}, SketchPlane::XY(), "Profile");
    const int short_path = doc.add_sketch_entities({line(0, 0, 0, 10)}, SketchPlane::YZ(), "Path 10");
    const int long_path  = doc.add_sketch_entities({line(0, 0, 0, 20)}, SketchPlane::YZ(), "Path 20");
    const int sweep      = doc.add_sweep(profile, short_path, BooleanMode::New, "Sweep");
    const int later      = doc.add_sketch_entities({line(0, 0, 0, 5)}, SketchPlane::YZ(), "Later");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);
    const CadDocument before = GUI::Mcp::cad_document_before(doc, sweep);
    auto refused = [&](const json& refs) {
        CadFeature f = doc.features[sweep];
        const std::string why = failure([&] { GUI::Mcp::cad_apply_feature_refs(f, refs, before, doc, sweep); });
        CHECK(f.sweep_path_ref == short_path);   // as it was
        return why;
    };

    // Re-pointed at the longer path, the sweep is twice as long.
    CadFeature along = doc.features[sweep];
    GUI::Mcp::cad_apply_feature_refs(along, {{"sweep_path_ref", long_path}}, before, doc, sweep);
    CHECK(along.sweep_path_ref == long_path);
    CHECK(along.sketch_ref == profile);

    // A path must come before the sweep and be a sketch or a helix; a name it does not take is listed with those it does.
    CHECK_THAT(refused({{"sweep_path_ref", sweep}}), ContainsSubstring("is not an earlier feature"));
    CHECK_THAT(refused({{"sweep_path_ref", later}}), ContainsSubstring("is not an earlier feature"));
    CHECK_THAT(refused({{"sweep_path_ref", 1}}), ContainsSubstring("sweep_path_ref 1 (Extrude) is not a Sketch or a Helix"));
    CHECK_THAT(refused({{"edges", {0}}}), ContainsSubstring("Sweep takes sweep_path_ref, target_body"));
    CHECK_THAT(refused({{"sweep_path_ref", "2"}}), ContainsSubstring("sweep_path_ref must be a feature index"));
    REQUIRE(doc.replace_feature(sweep, along));
    CHECK_THAT(body_volume(doc, 1), WithinAbs(PI * 4.0 * 20.0, 1e-3));

    // An Extrude keeps its profile and its target, as its card does.
    CadFeature extrude = doc.features[1];
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_refs(extrude, {{"target_body", 0}}, GUI::Mcp::cad_document_before(doc, 1), doc, 1); }),
               ContainsSubstring("Extrude: its profile and its target are fixed"));
}

TEST_CASE("feature_edit's changed names a plane re-pointed", "[Mcp][CAD]")
{
    // The box (features 0 and 1) mirrored about YZ as a new body (feature 2): feature_describe reports the plane it
    // builds on at the top level, beside its params and refs.
    CadDocument doc;
    add_box(doc);
    const int mirror = doc.add_mirror(SketchPlane::YZ(), 0, BooleanMode::New, "Mirror");
    REQUIRE(doc.recompute());
    const json was = GUI::Mcp::cad_feature_json(doc, mirror);
    REQUIRE(was.at("plane").at("base") == "YZ");
    CHECK(GUI::Mcp::cad_feature_changes(was, was).empty());

    // Re-pointed at XZ and made a join, the changes are the plane's, from YZ to XZ, and the operation's, each with its
    // old and new value.
    CadFeature f = doc.features[mirror];
    GUI::Mcp::cad_apply_feature_refs(f, {{"plane", "XZ"}}, GUI::Mcp::cad_document_before(doc, mirror), doc, mirror);
    GUI::Mcp::cad_apply_feature_fields(f, {{"operation", "join"}});
    REQUIRE(doc.replace_feature(mirror, f));
    const json changed = GUI::Mcp::cad_feature_changes(was, GUI::Mcp::cad_feature_json(doc, mirror));
    REQUIRE(changed.contains("plane"));
    CHECK(changed.at("plane").at(0).at("base") == "YZ");
    CHECK(changed.at("plane").at(1).at("base") == "XZ");
    CHECK(changed.value("operation", json()) == json::array({"new_body", "join"}));
    CHECK(changed.size() == 2);
}

TEST_CASE("feature_edit's refs takes what each card can pick again", "[Mcp][CAD]")
{
    // The box (features 0 and 1, body 0), a profile of a line and an arc on XZ (2), a sketch of one line on XY (3), a
    // coordinate system (4) and a datum plane 5 mm above XY (5). Each feature edited is added after them, as feature 6.
    CadDocument  doc;
    add_box(doc);
    SketchEntity arc;
    arc.type        = SketchEntity::Type::Arc;
    arc.center      = Vec2d(15, 0);
    arc.radius      = 5;
    arc.start_angle = 0;
    arc.end_angle   = PI;
    arc.p0          = Vec2d(20, 0);
    arc.p1          = Vec2d(10, 0);
    const int profile = doc.add_sketch_entities({line(10, 0, 20, 0), arc}, SketchPlane::XZ(), "Profile");
    const int other   = doc.add_sketch_entities({line(0, 0, 10, 0)}, SketchPlane::XY(), "Other");
    const int cs      = doc.add_coordsys(CoordSysType::PointWorld, Vec3d::Zero(), "CS");
    const int datum   = doc.add_plane(0, 5.0, 0.0, 0, "Plane");
    REQUIRE(doc.recompute());
    const int top = face_facing(doc, Vec3d(0, 0, 1));
    // The feature of type `t` (`seed` sets what it holds) with `refs` applied, and why they were refused, if they were.
    auto apply = [&](CadFeatureType t, const json& refs, const std::function<void(CadFeature&)>& seed = {}) {
        CadFeature f;
        f.type = t;
        f.name = "Edited";
        if (seed) seed(f);
        doc.features.push_back(f);
        const int   i = int(doc.features.size()) - 1;
        std::string why;
        try {
            GUI::Mcp::cad_apply_feature_refs(f, refs, GUI::Mcp::cad_document_before(doc, i), doc, i);
        } catch (const std::runtime_error& e) {
            why = e.what();
        }
        doc.features.pop_back();
        return std::make_pair(f, why);
    };
    // The feature as apply() leaves it, which must not have been refused.
    auto applied = [&](CadFeatureType t, const json& refs, const std::function<void(CadFeature&)>& seed = {}) {
        const auto [f, why] = apply(t, refs, seed);
        CHECK(why == "");
        return f;
    };
    using T = CadFeatureType;

    // A revolve turns about a line of its own sketch.
    const auto on_profile = [&](CadFeature& f) { f.sketch_ref = profile; };
    CHECK(applied(T::Revolve, {{"revolve_axis_entity", 0}}, on_profile).revolve_axis_entity == 0);
    CHECK_THAT(apply(T::SurfaceRevolve, {{"revolve_axis_entity", 1}}, on_profile).second, ContainsSubstring("revolve_axis_entity 1 is not a line of sketch 2"));
    // A loft goes through two or more earlier sketches.
    CHECK(applied(T::Loft, {{"loft_profile_refs", {profile, other}}}).loft_profile_refs == std::vector<int>({profile, other}));
    CHECK_THAT(apply(T::SurfaceLoft, {{"loft_profile_refs", {profile}}}).second, ContainsSubstring("two or more"));
    CHECK_THAT(apply(T::Loft, {{"loft_profile_refs", {profile, 1}}}).second, ContainsSubstring("loft_profile_refs 1 (Extrude) is not a Sketch"));
    // A plane is a base plane, an earlier datum or a face of the model before; a hole drills the face's body.
    CHECK((applied(T::Mirror, {{"plane", "XZ"}}).plane.normal - Vec3d(0, 1, 0)).norm() < 1e-9);
    CHECK_THAT(applied(T::Helix, {{"plane", {{"datum", datum}}}}).plane.origin.z(), WithinAbs(5.0, 1e-9));
    const CadFeature cut = applied(T::Cut, {{"plane", {{"body", 0}, {"face", top}}}}, [](CadFeature& f) { f.cut_face = 3; });
    CHECK(cut.cut_face == -1);
    CHECK_THAT(cut.plane.origin.z(), WithinAbs(10.0, 1e-6));
    const CadFeature hole = applied(T::Hole, {{"plane", {{"body", 0}, {"face", top}}}});
    CHECK(hole.target_body == 0);
    CHECK((hole.plane.normal - Vec3d(0, 0, -1)).norm() < 1e-9);   // the Hole card's plane points into the body
    CHECK_THAT(apply(T::Thread, {{"plane", {{"body", 0}, {"face", top}}}, {"target_body", 1}}).second,
               ContainsSubstring("target_body 1 is not a body before feature 6 (Edited): there was 1 body"));
    CHECK_THAT(apply(T::Mirror, {{"plane", {{"datum", 7}}}}).second, ContainsSubstring("is not an earlier feature"));
    // A rib grows from a line of an earlier sketch.
    const auto on_other = [&](CadFeature& f) { f.rib_sketch_ref = other; };
    CHECK(applied(T::Rib, {{"rib_entity", 0}}, on_other).rib_entity == 0);
    CHECK_THAT(apply(T::Rib, {{"rib_sketch_ref", profile}, {"rib_entity", 1}}, on_other).second, ContainsSubstring("rib_entity 1 is not a line of sketch 2"));
    // An align joins two different coordinate systems.
    const auto from_cs = [&](CadFeature& f) { f.mate_cs_a = cs; };
    CHECK_THAT(apply(T::Mate, {{"mate_cs_b", cs}}, from_cs).second, ContainsSubstring("two different coordinate systems"));
    CHECK_THAT(apply(T::Mate, {{"mate_cs_b", datum}}, from_cs).second, ContainsSubstring("mate_cs_b 5 (Plane) is not a CoordSys"));
    // Faces of the target body before the feature; a shell may be closed.
    CHECK(applied(T::Shell, {{"shell_face", top}}).shell_face == top);
    CHECK(applied(T::Shell, {{"shell_face", -1}}).shell_face == -1);
    CHECK_THAT(apply(T::Draft, {{"draft_face", 6}}).second, ContainsSubstring("face 6 of body 0 does not exist before feature 6 (Edited): the body had 6 faces there"));
    CHECK_THAT(apply(T::DeleteFace, {{"delete_faces", {top, top}}}).second, ContainsSubstring("listed twice"));
    // A fillet moved to another body keeps edges only that body has.
    CHECK(apply(T::Fillet, {{"target_body", 0}}, [](CadFeature& f) { f.dressup_edge = 11; }).second.empty());
    // -1, the last body, is any type's but a Boolean's, whose bool_tool_body is -1 too.
    CHECK(applied(T::Pattern, {{"target_body", -1}}).target_body == -1);
    CHECK(applied(T::Shell, {{"target_body", -1}}).target_body == -1);
    // A datum's references are the finished model's, and its datum planes earlier ones.
    const CadFeature axis = applied(T::Axis, {{"axis_body", 0}, {"axis_face", top}, {"axis_plane_a", 3}});
    CHECK(axis.axis_face == top);
    CHECK(axis.axis_plane_a == 3);
    CHECK_THAT(apply(T::Axis, {{"axis_plane_a", 4}}).second, ContainsSubstring("axis_plane_a 4 names no datum plane before feature 6"));
    CHECK_THAT(apply(T::CoordSys, {{"coordsys_face", top}}).second, ContainsSubstring("is on no body"));
    // A card that picks nothing again takes no refs.
    CHECK_THAT(apply(T::SurfaceExtrude, {{"target_body", 0}}).second, ContainsSubstring("SurfaceExtrude: its card picks nothing again"));
}

TEST_CASE("ids in refs are those of the model before the feature", "[Mcp][CAD]")
{
    int               rounded = -1;
    CadDocument       doc     = filleted_shelled_box(rounded);
    const int         fillet  = 2;
    const int         edges   = GeometryEngine::edge_count(doc.bodies.at(0).shape);
    const CadDocument before  = GUI::Mcp::cad_document_before(doc, fillet);
    REQUIRE(before.bodies.size() == 1);
    CHECK(GeometryEngine::edge_count(before.bodies[0].shape) == 12);
    CHECK(edges > 12);

    // Another upright edge of the box, by its id there: the rebuilt Fillet rounds that corner.
    const std::vector<int> upright = upright_edges(before);
    REQUIRE(upright.size() == 4);
    const int  other  = upright.back();
    const auto corner = GeometryEngine::sample_edge_world(GeometryEngine::edge_by_index(before.bodies[0].shape, other)).front();
    CadFeature f      = doc.features[fillet];
    GUI::Mcp::cad_apply_feature_refs(f, {{"edges", {other}}}, before, doc, fillet);
    CHECK(f.dressup_edge_ids() == std::vector<int>{other});
    REQUIRE(doc.replace_feature(fillet, f));
    const CadDocument filleted = GUI::Mcp::cad_document_before(doc, fillet + 1);
    int               rounds   = 0;
    for (const TopoDS_Face& face : GeometryEngine::faces_of(filleted.bodies.at(0).shape))
        if (const GeometryEngine::CylinderFace c = GeometryEngine::cylinder_of_face(face); c.ok) {
            ++rounds;
            // The round's axis stands 2 mm in from both sides of that corner.
            CHECK_THAT((c.base - corner).head<2>().norm(), WithinAbs(2.0 * std::sqrt(2.0), 1e-6));
        }
    CHECK(rounds == 1);

    // An id of the finished model that the box does not have names the body, the feature and the edges it had.
    CadFeature g = doc.features[fillet];
    const std::string why = failure([&] { GUI::Mcp::cad_apply_feature_refs(g, {{"edges", {edges - 1}}}, before, doc, fillet); });
    CHECK_THAT(why, ContainsSubstring("edge " + std::to_string(edges - 1) + " of body 0 does not exist before feature 2 (Fillet)"));
    CHECK_THAT(why, ContainsSubstring("the body had 12 edges there"));
    CHECK(g.dressup_edge_ids() == std::vector<int>{other});
    // An edge group in place of the edges, but not both.
    GUI::Mcp::cad_apply_feature_refs(g, {{"face_group", "top"}}, before, doc, fillet);
    CHECK(g.face_group == FaceGroup::Top);
    CHECK(g.dressup_edge_ids().empty());
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_refs(g, {{"edges", {other}}, {"face_group", "top"}}, before, doc, fillet); }),
               ContainsSubstring("give edges or face_group, not both"));

    // A Boolean's bodies are those of the model before it: the box and a block that cuts its corner, two bodies there
    // and one after the cut.
    CadDocument two;
    add_box(two);
    SketchPlane corner_plane = SketchPlane::XY();
    corner_plane.origin      = Vec3d(20, 10, 0);
    two.add_extrude(two.add_sketch(SketchShape::Rectangle, corner_plane, 10, 10, 0, "Sketch"), 5.0, false, BooleanMode::New, "Extrude");
    const int boolean = two.add_boolean(BooleanMode::Cut, 0, 1, false, 0.0, -1, -1, "Boolean");
    REQUIRE(two.recompute());
    REQUIRE(two.bodies.size() == 1);
    const CadDocument two_before = GUI::Mcp::cad_document_before(two, boolean);
    REQUIRE(two_before.bodies.size() == 2);
    CadFeature b = two.features[boolean];
    GUI::Mcp::cad_apply_feature_refs(b, {{"target_body", 1}, {"bool_tool_body", 0}}, two_before, two, boolean);
    CHECK(b.target_body == 1);
    CHECK(b.bool_tool_body == 0);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_refs(b, {{"bool_tool_body", 2}}, two_before, two, boolean); }),
               ContainsSubstring("bool_tool_body 2 is not a body before feature 4 (Boolean): there were 2 bodies"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_refs(b, {{"bool_tool_body", 1}}, two_before, two, boolean); }),
               ContainsSubstring("two different bodies"));
}

TEST_CASE("body_describe reads a body as it stood before a feature", "[Mcp][CAD]")
{
    int         rounded = -1;
    CadDocument doc     = filleted_shelled_box(rounded);
    const int   fillet  = 2;

    const json r = GUI::Mcp::cad_body_describe(doc, {{"body", 0}, {"before", fillet}});
    CHECK(r.at("edge_count") == 12);
    CHECK(r.at("face_count") == 6);
    CHECK(r.at("before") == fillet);
    // The live document's generation, which a call that passes these ids is checked against.
    CHECK(r.at("generation") == doc.topo_generation);
    CHECK_FALSE(r.contains("vertices"));
    CHECK(GUI::Mcp::cad_body_describe(doc, {{"body", 0}}).at("edge_count").get<int>() > 12);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"body", 0}, {"before", 9}}); }), ContainsSubstring("feature 9 is out of range"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"body", 0}, {"before", "2"}}); }), ContainsSubstring("before must be a feature index"));

    // The box's eight corners, each by its place in TopExp::MapShapes' order, as a face's and an edge's id is.
    const json v = GUI::Mcp::cad_body_describe(doc, {{"body", 0}, {"before", fillet}, {"vertices", true}}).at("vertices");
    REQUIRE(v.size() == 8);
    const CadDocument          before = GUI::Mcp::cad_document_before(doc, fillet);
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(before.bodies.at(0).shape, TopAbs_VERTEX, map);
    REQUIRE(map.Extent() == 8);
    std::set<std::tuple<int, int, int>> corners;
    for (int k = 0; k < 8; ++k) {
        INFO(v[k].dump());
        CHECK(v[k].at("id") == k);
        const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(map(k + 1)));
        CHECK(vec_dist(v[k].at("p"), Vec3d(p.X(), p.Y(), p.Z())) < 1e-9);
        corners.insert({int(std::lround(p.X())), int(std::lround(p.Y())), int(std::lround(p.Z()))});
    }
    CHECK(corners == std::set<std::tuple<int, int, int>>{{-20, -10, 0}, {-20, -10, 10}, {-20, 10, 0}, {-20, 10, 10},
                                                         {20, -10, 0},  {20, -10, 10},  {20, 10, 0},  {20, 10, 10}});
    CHECK(GUI::Mcp::cad_body_vertices(before, 0).size() == 8);
}

TEST_CASE("a datum's ids are the finished model's", "[Mcp][CAD]")
{
    // The box (features 0 and 1), a Plane on it (feature 2), a Shell open at the bottom (feature 3), and a Fillet of
    // an upright outside edge, which adds faces. The Plane reads its face in the finished model, as the engine does.
    CadDocument doc;
    add_box(doc);
    const int plane = doc.add_plane(0, 0.0, 0.0, 0, "Plane");
    const int shell = doc.add_shell(1.0, face_facing(doc, Vec3d(0, 0, -1)), 0, "Shell");
    REQUIRE(doc.recompute());
    doc.add_fillet(0.5, upright_edges(doc).at(0), "Fillet");
    REQUIRE(doc.recompute());
    doc.features[plane].plane_type      = PlaneType::Coincident;
    doc.features[plane].plane_face_body = 0;
    doc.features[plane].plane_face      = face_facing(doc, Vec3d(0, 0, 1));
    REQUIRE(doc.recompute());

    // A flat face that only the finished model has: the box has six.
    const std::vector<TopoDS_Face> faces = GeometryEngine::faces_of(doc.bodies.at(0).shape);
    int                            only  = -1;
    for (int i = 6; i < int(faces.size()) && only < 0; ++i)
        if (BRepAdaptor_Surface(faces[i]).GetType() == GeomAbs_Plane) only = i;
    REQUIRE(only >= 0);

    CadFeature p = doc.features[plane];
    GUI::Mcp::cad_apply_feature_refs(p, {{"plane_face", only}}, GUI::Mcp::cad_document_before(doc, plane), doc, plane);
    CHECK(p.plane_face == only);
    CadFeature s = doc.features[shell];
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_refs(s, {{"shell_face", only}}, GUI::Mcp::cad_document_before(doc, shell), doc, shell); }),
               ContainsSubstring("face " + std::to_string(only) + " of body 0 does not exist before feature 3 (Shell): the body had 6 faces there"));

    // feature_describe says which model a feature's ids are read in, and lists the bodies before it.
    const json datum = GUI::Mcp::cad_feature_json(doc, plane);
    CHECK(datum.at("ids_of") == "finished");
    const json shelled = GUI::Mcp::cad_feature_json(doc, shell);
    CHECK(shelled.at("ids_of") == "before");
    REQUIRE(shelled.at("bodies_before").size() == 1);
    CHECK(shelled.at("bodies_before")[0].at("index") == 0);
    CHECK(shelled.at("bodies_before")[0].at("name").is_string());
    CHECK(GUI::Mcp::cad_feature_json(doc, 0).at("bodies_before").empty());
}

TEST_CASE("feature_describe lists the bodies before each feature of a history of thirty holes", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    SketchPlane top;
    REQUIRE(doc.inward_plane_of_face(0, face_facing(doc, Vec3d(0, 0, 1)), top));
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 5; ++j)
            doc.add_hole(1.0, 2.0, false, -15.0 + 6.0 * i, -6.0 + 3.0 * j, top, "Hole");
    REQUIRE(doc.recompute());
    REQUIRE(doc.features.size() == 32);

    // The time is printed, not pinned: it says what a reply costs on a long history.
    const auto start = std::chrono::steady_clock::now();
    for (int f = 0; f < int(doc.features.size()); ++f)
        CHECK(GUI::Mcp::cad_feature_json(doc, f).at("bodies_before").size() == (f < 2 ? 0u : 1u));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    WARN("feature_describe of each of the " << doc.features.size() << " features: " << ms << " ms");
}

TEST_CASE("bodies_before is known up to the feature a rebuild stops at, and null after it", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int fillet = doc.add_fillet(1.0, upright_edges(doc).at(0), "Fillet");
    REQUIRE(doc.recompute());
    doc.features[fillet].enabled = false;
    REQUIRE(doc.recompute());
    // A suppressed feature lists the bodies it would start from, named as the bodies are now.
    doc.bodies.at(0).has_user_name = true;
    doc.bodies.at(0).user_name     = "Plate";
    const json suppressed = GUI::Mcp::cad_feature_json(doc, fillet).at("bodies_before");
    REQUIRE(suppressed.size() == 1);
    CHECK(suppressed[0].at("name") == "Plate");

    // A hole off the box removes nothing, so the rebuild stops there: the Shell after it is not reached.
    const int miss  = doc.add_hole(1.0, 2.0, false, 100.0, 100.0, SketchPlane::XY(), "Hole");
    const int shell = doc.add_shell(1.0, -1, 0, "Shell");
    REQUIRE_FALSE(doc.recompute());
    CHECK(GUI::Mcp::cad_feature_json(doc, miss).at("bodies_before").size() == 1);
    CHECK(GUI::Mcp::cad_feature_json(doc, shell).at("bodies_before").is_null());
}

TEST_CASE("body_describe names a chamfer's cone and a fillet's torus, with their radii and areas", "[Mcp][CAD]")
{
    CadDocument doc;
    add_cylinder(doc);
    const int top = circle_at(doc, 20), bottom = circle_at(doc, 0);
    REQUIRE(top >= 0);
    REQUIRE(bottom >= 0);
    doc.add_fillet(1.0, top, "Fillet");
    REQUIRE(doc.recompute());
    doc.add_chamfer(1.0, circle_at(doc, 0), "Chamfer");
    REQUIRE(doc.recompute());

    const json reply = GUI::Mcp::cad_body_describe(doc, {{"kinds", {"conical", "toroidal"}}});
    CHECK(reply.at("faces_total") == 2);
    CHECK(reply.at("edges").empty());   // no edge filter, no adjacency
    CHECK(reply.at("face_count").get<int>() > 2);

    const json& cone = item_with(reply.at("faces"), "kind", "conical");
    CHECK_THAT(cone.at("half_angle_deg").get<double>(), WithinAbs(45.0, 1e-6));
    CHECK_THAT(cone.at("radius_min").get<double>(), WithinAbs(9.0, 1e-6));
    CHECK_THAT(cone.at("radius_max").get<double>(), WithinAbs(10.0, 1e-6));
    CHECK_THAT(cone.at("area").get<double>(), WithinAbs(PI * 19.0 * std::sqrt(2.0), 1e-3));
    CHECK(cone.at("internal") == false);
    // The apex lies below the chamfer, and the axis runs from it into the opening (+z).
    CHECK_THAT(cone.at("apex")[2].get<double>(), WithinAbs(-9.0, 1e-6));
    CHECK_THAT(cone.at("axis")[2].get<double>(), WithinAbs(1.0, 1e-9));

    const json& torus = item_with(reply.at("faces"), "kind", "toroidal");
    CHECK_THAT(torus.at("major_radius").get<double>(), WithinAbs(9.0, 1e-6));
    CHECK_THAT(torus.at("minor_radius").get<double>(), WithinAbs(1.0, 1e-6));
    CHECK_THAT(torus.at("area").get<double>(), WithinAbs(2 * PI * (9.0 * PI / 2 + 1.0), 1e-3));
    CHECK_THAT(torus.at("center")[2].get<double>(), WithinAbs(19.0, 1e-6));

    // A chamfered cylinder's side is still a cylinder.
    const json sides = GUI::Mcp::cad_body_describe(doc, {{"kinds", {"cylindrical"}}});
    REQUIRE(sides.at("faces_total") == 1);
    CHECK_THAT(sides.at("faces")[0].at("radius").get<double>(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(sides.at("faces")[0].at("area").get<double>(), WithinAbs(2 * PI * 10.0 * 18.0, 1e-3));
}

TEST_CASE("body_describe reads a revolved half disc as a sphere", "[Mcp][CAD]")
{
    CadDocument  doc;
    SketchEntity arc;
    arc.type        = SketchEntity::Type::Arc;
    arc.center      = Vec2d(0, 0);
    arc.radius      = 10;
    arc.start_angle = -PI / 2;
    arc.end_angle   = PI / 2;
    arc.p0          = Vec2d(0, -10);
    arc.p1          = Vec2d(0, 10);
    const int sk = doc.add_sketch_entities({arc, line(0, 10, 0, -10)}, SketchPlane::XZ(), "Sketch");
    doc.add_revolve(sk, 360.0, 1, false, BooleanMode::New, "Revolve");
    REQUIRE(doc.recompute());

    const json reply = GUI::Mcp::cad_body_describe(doc, {{"kinds", {"spherical"}}});
    REQUIRE(reply.at("faces_total").get<int>() >= 1);
    double area = 0;
    for (const json& f : reply.at("faces")) {
        CHECK_THAT(f.at("radius").get<double>(), WithinAbs(10.0, 1e-6));
        CHECK(vec_dist(f.at("center"), Vec3d::Zero()) < 1e-6);
        CHECK(f.at("internal") == false);
        area += f.at("area").get<double>();
    }
    CHECK_THAT(area, WithinAbs(4 * PI * 100.0, 1e-2));
}

TEST_CASE("body_describe's adjacency gives each edge its faces and says which corners are inside", "[Mcp][CAD]")
{
    SECTION("a box: every edge between two faces, every face four edges, every corner outside")
    {
        CadDocument doc;
        add_box(doc);
        const json reply = GUI::Mcp::cad_body_describe(doc, {{"adjacency", true}});
        REQUIRE(reply.at("faces").size() == 6);
        REQUIRE(reply.at("edges").size() == 12);
        for (const json& f : reply.at("faces")) CHECK(f.at("edges").size() == 4);
        for (const json& e : reply.at("edges")) {
            CHECK(e.at("faces").size() == 2);
            CHECK(e.at("convex") == true);
        }
    }
    SECTION("an L profile: one inside corner, and the faces on either side of it")
    {
        CadDocument doc;
        doc.add_sketch_entities({line(0, 0, 40, 0), line(40, 0, 40, 5), line(40, 5, 5, 5), line(5, 5, 5, 30), line(5, 30, 0, 30),
                                 line(0, 30, 0, 0)},
                                SketchPlane::XY(), "Sketch");
        doc.add_extrude(0, 20.0, false, BooleanMode::New, "Extrude");
        REQUIRE(doc.recompute());
        const json concave = GUI::Mcp::cad_body_describe(doc, {{"convex", false}});
        REQUIRE(concave.at("edges_total") == 1);
        const json& edge = concave.at("edges")[0];
        CHECK(edge.at("kind") == "line");
        CHECK(edge.at("convex") == false);
        CHECK_THAT(edge.at("p0")[0].get<double>(), WithinAbs(5.0, 1e-6));
        CHECK_THAT(edge.at("p0")[1].get<double>(), WithinAbs(5.0, 1e-6));
        // Without a face filter, the faces listed are the edge's two neighbours.
        CHECK(concave.at("faces_total") == 2);
        CHECK(concave.at("faces").size() == 2);
        CHECK(GUI::Mcp::cad_body_describe(doc, {{"convex", true}}).at("edges_total") == 17);
    }
    SECTION("a cylinder: its seam has one face, and the rims meet the caps in outside corners")
    {
        CadDocument doc;
        add_cylinder(doc);
        const json reply = GUI::Mcp::cad_body_describe(doc, {{"adjacency", true}});
        int seams = 0;
        for (const json& e : reply.at("edges")) {
            if (e.at("faces").size() == 1) {
                ++seams;
                CHECK_FALSE(e.contains("convex"));
            } else {
                CHECK(e.at("kind") == "circle");
                CHECK(e.at("convex") == true);
            }
        }
        CHECK(seams == 1);
    }
    SECTION("a fillet meets the faces it rounds smoothly")
    {
        CadDocument doc;
        add_cylinder(doc);
        doc.add_fillet(1.0, circle_at(doc, 20), "Fillet");
        REQUIRE(doc.recompute());
        const json torus = GUI::Mcp::cad_body_describe(doc, {{"kinds", {"toroidal"}}, {"adjacency", true}});
        REQUIRE(torus.at("faces_total") == 1);
        int tangent = 0;
        for (const json& e : torus.at("edges"))   // the torus's own edges, as neighbours
            if (e.value("tangent", false)) ++tangent;
        CHECK(tangent == 2);
    }
}

TEST_CASE("body_describe filters by direction, extreme and kind, and pages both lists", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int top = face_facing(doc, Vec3d(0, 0, 1));

    const json up = GUI::Mcp::cad_body_describe(doc, {{"direction", {0, 0, 2}}});
    REQUIRE(up.at("faces_total") == 1);
    CHECK(up.at("faces")[0].at("id") == top);
    CHECK(up.at("edges_total") == 4);   // the vertical edges, whichever way they run
    for (const json& e : up.at("edges")) CHECK_THAT(e.at("length").get<double>(), WithinAbs(10.0, 1e-6));

    const json high = GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "+z"}});
    REQUIRE(high.at("faces_total") == 1);
    CHECK(high.at("faces")[0].at("id") == top);
    CHECK(high.at("edges").empty());
    CHECK(GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "-x"}}).at("faces")[0].at("id") == face_facing(doc, Vec3d(-1, 0, 0)));

    const json top_edges = GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
    CHECK(top_edges.at("edges_total") == 4);
    CHECK(GUI::Mcp::cad_body_describe(doc, {{"kinds", {"planar"}}, {"adjacency", true}}).at("edges_total") == 12);
    CHECK(GUI::Mcp::cad_body_describe(doc, {{"edge_kinds", {"circle"}}}).at("faces").empty());

    const json first = GUI::Mcp::cad_body_describe(doc, {{"limit", 2}});
    CHECK(first.at("faces").size() == 2);
    CHECK(first.at("edges").size() == 2);
    CHECK(first.at("faces_total") == 6);
    CHECK(first.at("edges_total") == 12);
    CHECK(first.at("next_offset") == 2);
    const json last = GUI::Mcp::cad_body_describe(doc, {{"limit", 2}, {"offset", 10}});
    CHECK(last.at("faces").empty());
    CHECK(last.at("edges").size() == 2);
    CHECK(last.at("edges")[0].at("id") == 10);
    CHECK_FALSE(last.contains("next_offset"));

    // A face kind may be named by its surface's noun.
    CHECK(GUI::Mcp::cad_body_describe(doc, {{"kinds", {"plane"}}}).at("faces_total") ==
          GUI::Mcp::cad_body_describe(doc, {{"kinds", {"planar"}}}).at("faces_total"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"kinds", {"face"}}}); }), ContainsSubstring("kinds must be one of planar"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"edge_kinds", "line"}}); }), ContainsSubstring("must be a list"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"direction", {0, 0, 0}}}); }), ContainsSubstring("must not be zero"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"direction", {0, 1}}}); }), ContainsSubstring("[x, y, z]"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"convex", "yes"}}); }), ContainsSubstring("convex must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "top"}}); }), ContainsSubstring("at_extreme must be one of"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"limit", 0}}); }), ContainsSubstring("limit must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"offset", -1}}); }), ContainsSubstring("offset must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_body_describe(doc, {{"body", 3}}); }), ContainsSubstring("out of range"));
}

TEST_CASE("measure keeps the centroid distance and adds the minimum distance, across bodies", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);   // body 0: z 0..10, x -20..20
    SketchPlane above = SketchPlane::XY();
    above.origin      = Vec3d(30, 0, 15);
    const int sk      = doc.add_sketch(SketchShape::Rectangle, above, 40, 20, 0, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");   // body 1: z 15..25, x 10..50
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);
    const int top0 = face_facing_on(doc, 0, Vec3d(0, 0, 1)), bottom1 = face_facing_on(doc, 1, Vec3d(0, 0, -1));

    const json gap = GUI::Mcp::cad_measure(doc, {{"a", {{"face", top0}}}, {"b", {{"body", 1}, {"face", bottom1}}}});
    CHECK_THAT(gap.at("min_distance").get<double>(), WithinAbs(5.0, 1e-6));
    CHECK_THAT(gap.at("distance").get<double>(), WithinAbs(std::sqrt(30.0 * 30.0 + 5.0 * 5.0), 1e-6));
    CHECK_THAT(gap.at("parallel_gap").get<double>(), WithinAbs(5.0, 1e-6));
    CHECK_THAT(gap.at("angle_deg").get<double>(), WithinAbs(180.0, 1e-6));
    CHECK_THAT(gap.at("closest_b")[2].get<double>() - gap.at("closest_a")[2].get<double>(), WithinAbs(5.0, 1e-6));

    // A point to a face: the perpendicular, with no angle (a point has no direction).
    const json drop = GUI::Mcp::cad_measure(doc, {{"a", {{"point", {0, 0, 30}}}}, {"b", {{"face", top0}}}});
    CHECK_THAT(drop.at("min_distance").get<double>(), WithinAbs(20.0, 1e-6));
    CHECK_FALSE(drop.contains("angle_deg"));
    CHECK_FALSE(drop.contains("parallel_gap"));

    // Two edges of one face meet at a corner.
    const json edges = GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
    const json& e0 = edges.at("edges")[0];
    int corner = -1;
    for (const json& e : edges.at("edges"))
        if (vec_dist(e.at("p0"), Vec3d(e0.at("p0")[0], e0.at("p0")[1], e0.at("p0")[2])) > 1e-6 &&
            std::abs(Vec3d(e.at("p1")[0].get<double>() - e.at("p0")[0].get<double>(), e.at("p1")[1].get<double>() - e.at("p0")[1].get<double>(), 0)
                         .normalized()
                         .dot(Vec3d(e0.at("p1")[0].get<double>() - e0.at("p0")[0].get<double>(), e0.at("p1")[1].get<double>() - e0.at("p0")[1].get<double>(), 0)
                                  .normalized())) < 0.5)
            corner = e.at("id");
    REQUIRE(corner >= 0);
    CHECK_THAT(GUI::Mcp::cad_measure(doc, {{"a", {{"edge", e0.at("id")}}}, {"b", {{"edge", corner}}}}).at("min_distance").get<double>(),
               WithinAbs(0.0, 1e-6));

    SECTION("a cylinder's axis stands for the hole")
    {
        doc.features[doc.add_hole(5, 10, true, 10, 0, SketchPlane::XY(), "Hole")].target_body = 0;
        REQUIRE(doc.recompute());
        const int bore = cylinder_face(doc), right = face_facing_on(doc, 0, Vec3d(1, 0, 0));
        const json r = GUI::Mcp::cad_measure(doc, {{"a", {{"face", bore}, {"axis", true}}}, {"b", {{"face", right}}}});
        CHECK_THAT(r.at("min_distance").get<double>(), WithinAbs(10.0, 1e-6));
        CHECK_THAT(r.at("distance").get<double>(), WithinAbs(10.0, 1e-6));   // the axis's middle is the bore's centroid
        CHECK_THAT(r.at("angle_deg").get<double>(), WithinAbs(90.0, 1e-6));
    }
    SECTION("refusals name the ref")
    {
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"face", top0}}}}); }), ContainsSubstring("needs refs 'a' and 'b'"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"face", top0}, {"edge", 1}}}, {"b", {{"face", 0}}}}); }),
                   ContainsSubstring("could not resolve ref \"a\""));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"face", top0}}}, {"b", {{"body", 1}, {"face", 99}}}}); }),
                   ContainsSubstring("ref \"b\": face 99 of body 1 does not exist"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"face", top0}, {"axis", true}}}, {"b", {{"face", 0}}}}); }),
                   ContainsSubstring("is planar; axis takes a cylindrical face"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"body", 5}, {"edge", 0}}}, {"b", {{"face", 0}}}}); }),
                   ContainsSubstring("body 5 is out of range"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_measure(doc, {{"a", {{"point", {1, 2}}}}, {"b", {{"face", 0}}}}); }),
                   ContainsSubstring("point must be [x, y, z]"));
    }
}

TEST_CASE("a hole drills a face's body at a world point or on a round reference's axis", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);   // 40 x 20 x 10, centred on the origin, z 0..10
    const int  top = face_facing(doc, Vec3d(0, 0, 1));
    SketchPlane pl;
    int         body = -2;

    const json at = GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", top}}}, {"at", {5, 3, 12}}}, pl, body);
    CHECK(body == 0);
    CHECK(at.at("body") == 0);
    CHECK_THAT(at.at("off_plane").get<double>(), WithinAbs(-2.0, 1e-9));   // 2 mm above, against the inward normal
    const Vec3d landed = pl.origin + pl.x_axis * at.at("x").get<double>() + pl.y_axis * at.at("y").get<double>();
    CHECK((landed - Vec3d(5, 3, 10)).norm() < 1e-9);

    // x and y as given, the last body by default on a base plane.
    const json xy = GUI::Mcp::cad_hole_placement(doc, {{"x", 4}, {"y", -2}}, pl, body);
    CHECK(body == -1);
    CHECK(xy.at("body") == 0);
    CHECK(xy.at("x") == 4.0);
    CHECK_FALSE(xy.contains("off_plane"));

    SECTION("on the axis of a bore, whatever the bed")
    {
        doc.modeling_origin = Vec3d(100, 50, 0);
        doc.add_hole(3, 10, true, 10, 0, SketchPlane::XY(), "Hole");
        REQUIRE(doc.recompute());
        const int bore = cylinder_face(doc), roof = face_facing(doc, Vec3d(0, 0, 1));
        const json r = GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", roof}}}, {"at", {{"body", 0}, {"face", bore}}}}, pl, body);
        const Vec3d on = pl.origin + pl.x_axis * r.at("x").get<double>() + pl.y_axis * r.at("y").get<double>();
        CHECK((on - Vec3d(10, 0, 10)).norm() < 1e-6);
        // A counterbore there widens the bore's top.
        const double before = body_volume(doc, 0);
        const int    h      = doc.add_hole_styled(3, 10, true, r.at("x"), r.at("y"), pl, 1, 6, 2, 0, 90, "", "Hole");
        doc.features[h].target_body = body;
        REQUIRE(doc.recompute());
        CHECK_THAT(before - body_volume(doc, 0), WithinAbs(PI * (9.0 - 2.25) * 2.0, 1e-3));
        // Its rim, a circular edge, names the same axis.
        const json rim = GUI::Mcp::cad_body_describe(doc, {{"edge_kinds", {"circle"}}});
        int        e   = -1;
        for (const json& c : rim.at("edges"))
            if (std::abs(c.at("radius").get<double>() - 3.0) < 1e-6) e = c.at("id");
        REQUIRE(e >= 0);
        const json r2 = GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", face_facing(doc, Vec3d(0, 0, 1))}}}, {"at", {{"body", 0}, {"edge", e}}}}, pl, body);
        CHECK((pl.origin + pl.x_axis * r2.at("x").get<double>() + pl.y_axis * r2.at("y").get<double>() - Vec3d(10, 0, 10)).norm() < 1e-6);
    }
    SECTION("a countersink from a side face leaves its cone in that face")
    {
        const int side = face_facing(doc, Vec3d(0, 1, 0));   // y = 10, 40 x 10
        const json r = GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", side}}}}, pl, body);
        CHECK(body == 0);
        const int h = doc.add_hole_styled(4, 20, true, r.at("x"), r.at("y"), pl, 2, 0, 0, 8, 90, "", "Hole");
        doc.features[h].target_body = body;
        REQUIRE(doc.recompute());
        // A 4 mm bore through 20 mm, and a 90 degree seat 2 mm deep from 8 mm to 4 mm.
        const double removed = PI * 4.0 * 20.0 + PI * 2.0 / 3.0 * (16.0 + 8.0 + 4.0) - PI * 4.0 * 2.0;
        CHECK_THAT(body_volume(doc, 0), WithinAbs(8000.0 - removed, 1e-2));
        const json cones = GUI::Mcp::cad_body_describe(doc, {{"kinds", {"conical"}}});
        REQUIRE(cones.at("faces_total") == 1);
        const json& cone = cones.at("faces")[0];
        CHECK(cone.at("internal") == true);
        CHECK_THAT(cone.at("radius_max").get<double>(), WithinAbs(4.0, 1e-6));
        CHECK_THAT(cone.at("half_angle_deg").get<double>(), WithinAbs(45.0, 1e-6));
        CHECK_THAT(cone.at("apex")[1].get<double>(), WithinAbs(6.0, 1e-6));   // inside, below the y = 10 face
        CHECK_THAT(cone.at("axis")[1].get<double>(), WithinAbs(1.0, 1e-9));   // opening out of that face
    }
    SECTION("refusals")
    {
        add_box(doc);   // a second body
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", top}}}, {"body", 1}}, pl, body); }),
                   ContainsSubstring("the plane is a face of body 0"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(doc, {{"at", {1, 2, 3}}, {"x", 1}}, pl, body); }), ContainsSubstring("not both"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(doc, {{"at", {1, 2}}}, pl, body); }), ContainsSubstring("at must be [x, y, z]"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(doc, {{"at", {{"body", 0}, {"face", top}}}}, pl, body); }),
                   ContainsSubstring("is planar; \"at\" takes a cylindrical face or a circular edge"));
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(doc, {{"body", 7}}, pl, body); }), ContainsSubstring("out of range"));
        doc.features[doc.add_hole(3, 10, true, 0, 0, SketchPlane::XY(), "Hole")].target_body = 0;
        REQUIRE(doc.recompute());
        const int bore = cylinder_face(doc), side = face_facing(doc, Vec3d(1, 0, 0));
        CHECK_THAT(failure([&] {
                       GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", side}}}, {"at", {{"body", 0}, {"face", bore}}}}, pl, body);
                   }),
                   ContainsSubstring("is not square to the plane"));
        // Where the named references lie, and what they are, is the call's choice; a design with no body is the state.
        CHECK(failure_code([&] {
                  GUI::Mcp::cad_hole_placement(doc, {{"plane", {{"body", 0}, {"face", side}}}, {"at", {{"body", 0}, {"face", bore}}}}, pl, body);
              }) == "invalid_argument");
        CHECK(failure_code([&] { GUI::Mcp::cad_hole_placement(doc, {{"at", {{"body", 0}, {"face", top}}}}, pl, body); }) ==
              "invalid_argument");
        CadDocument empty;
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(empty, json::object(), pl, body); }), ContainsSubstring("no body to drill"));
        CHECK(failure_code([&] { GUI::Mcp::cad_hole_placement(empty, json::object(), pl, body); }) == "precondition");
    }
}

TEST_CASE("extrude pushes or pulls a planar face of a body", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int   top = face_facing(doc, Vec3d(0, 0, 1));
    SketchPlane frame;

    const int e = GUI::Mcp::cad_face_extrude(doc, {{"face", top}, {"distance", 5}}, frame)("Extrude");
    CHECK(doc.features[e].mode == BooleanMode::Add);   // joins the face's body, as the card does
    CHECK(doc.features[e].target_body == 0);
    CHECK_THAT(frame.origin.z(), WithinAbs(10.0, 1e-6));
    CHECK((frame.normal - Vec3d(0, 0, 1)).norm() < 1e-9);
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    CHECK_THAT(body_volume(doc, 0), WithinAbs(40.0 * 20.0 * 15.0, 1e-6));

    // Inward and cutting: 3 mm off the new top.
    const int roof = face_facing(doc, Vec3d(0, 0, 1));
    GUI::Mcp::cad_face_extrude(doc, {{"face", roof}, {"distance", 3}, {"flip", true}, {"operation", "cut"}}, frame)("Extrude");
    REQUIRE(doc.recompute());
    CHECK_THAT(body_volume(doc, 0), WithinAbs(40.0 * 20.0 * 12.0, 1e-6));

    const size_t features = doc.features.size();
    auto refusal = [&](json params) {
        params["face"] = params.value("face", json(roof));
        return failure([&] { GUI::Mcp::cad_face_extrude(doc, params, frame); });
    };
    CHECK_THAT(refusal({{"sketch", 0}}), ContainsSubstring("sketch cannot be given with face"));
    CHECK_THAT(refusal({{"plane", "XY"}}), ContainsSubstring("plane cannot be given with face"));
    CHECK_THAT(refusal({{"end", "two_sided"}}), ContainsSubstring("ends blind or symmetric"));
    CHECK_THAT(refusal({{"taper", 5}}), ContainsSubstring("taper does not apply"));
    CHECK_THAT(refusal({{"face", 99}}), ContainsSubstring("face 99 of body 0 does not exist"));
    CHECK_THAT(refusal({{"face", "2"}}), ContainsSubstring("face must be a face id"));
    CHECK_THAT(refusal({{"operation", "atuo"}}), ContainsSubstring("operation must be"));
    CHECK(doc.features.size() == features);

    CadDocument cyl;
    add_cylinder(cyl);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_face_extrude(cyl, {{"face", cylinder_face(cyl)}}, frame); }),
               ContainsSubstring("is cylindrical; a push/pull needs a planar face"));
}

TEST_CASE("construct_plane adds the datum plane each construction gives, as the Plane card does", "[Mcp][CAD]")
{
    CadDocument doc;
    doc.modeling_origin = Vec3d(100, 50, 0);
    add_box(doc);   // centred on the world origin, z 0..10
    auto add_plane = [&](const json& params) {
        const int i = GUI::Mcp::cad_datum_plane(doc, params)("Plane");
        REQUIRE(doc.recompute());
        int ordinal = 0;
        for (int k = 0; k < i; ++k)
            if (doc.features[k].type == CadFeatureType::Plane && doc.features[k].enabled) ++ordinal;
        return std::make_pair(i, doc.resolve_datum_planes().at(ordinal).second);
    };
    const int top = face_facing(doc, Vec3d(0, 0, 1)), bottom = face_facing(doc, Vec3d(0, 0, -1));

    const auto [p40, offset] = add_plane({{"offset", 40}});
    CHECK((offset.origin - Vec3d(100, 50, 40)).norm() < 1e-9);
    CHECK(doc.features[p40].plane_type == PlaneType::Offset);
    CHECK(doc.features[p40].plane_u_size == 60.0);

    const auto [p60, stacked] = add_plane({{"base", {{"datum", p40}}}, {"size", {30, 20}}});   // the card's 20 mm
    CHECK_THAT(stacked.origin.z(), WithinAbs(60.0, 1e-9));
    CHECK(doc.features[p60].plane_base == 3);
    CHECK(doc.features[p60].plane_v_size == 20.0);

    const auto [pmid, mid] = add_plane({{"type", "midplane"}, {"face_a", {{"body", 0}, {"face", top}}}, {"face_b", {{"body", 0}, {"face", bottom}}}});
    CHECK_THAT(mid.origin.z(), WithinAbs(5.0, 1e-9));
    CHECK(std::abs(mid.normal.z()) > 1 - 1e-9);

    const auto [pon, on] = add_plane({{"type", "offset"}, {"face_a", {{"body", 0}, {"face", top}}}, {"offset", 2}});
    CHECK_THAT(on.origin.z(), WithinAbs(12.0, 1e-9));

    const auto [pxz, xz] = add_plane({{"type", "coincident"}, {"base", "XZ"}});
    CHECK((xz.normal - Vec3d(0, 1, 0)).norm() < 1e-9);
    CHECK_THAT(xz.origin.y(), WithinAbs(50.0, 1e-9));

    const json top_edges = GUI::Mcp::cad_body_describe(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
    const int  ea = top_edges.at("edges")[0].at("id"), eb = top_edges.at("edges")[1].at("id");
    const auto [p2, two] = add_plane({{"type", "two_edges"}, {"edge_a", {{"body", 0}, {"edge", ea}}}, {"edge_b", {{"body", 0}, {"edge", eb}}}});
    CHECK_THAT(two.origin.z(), WithinAbs(10.0, 1e-9));
    CHECK(std::abs(two.normal.z()) > 1 - 1e-9);

    // An edge of the top face, tilted 90 degrees from the top: a vertical plane through it.
    const auto [pang, angled] = add_plane({{"type", "angle"}, {"edge_a", {{"body", 0}, {"edge", ea}}}, {"face_a", {{"body", 0}, {"face", top}}}, {"angle", 90}});
    CHECK(std::abs(angled.normal.z()) < 1e-9);
    CHECK_THAT(angled.origin.z(), WithinAbs(10.0, 1e-9));

    CadDocument cyl;
    add_cylinder(cyl);
    const int i = GUI::Mcp::cad_datum_plane(cyl, {{"type", "tangent"}, {"face_a", {{"body", 0}, {"face", cylinder_face(cyl)}}}})("Plane");
    REQUIRE(cyl.recompute());
    const SketchPlane touch = cyl.resolve_datum_planes().at(0).second;
    CHECK(i == 2);
    CHECK_THAT(std::hypot(touch.origin.x(), touch.origin.y()), WithinAbs(10.0, 1e-9));
    CHECK(std::abs(touch.normal.z()) < 1e-9);
}

TEST_CASE("construct_plane refuses what the card refuses, and what a construction would not use", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int top = face_facing(doc, Vec3d(0, 0, 1));
    const json face_a = {{"body", 0}, {"face", top}};
    const size_t features = doc.features.size();
    auto refusal = [&](const json& params) { return failure([&] { GUI::Mcp::cad_datum_plane(doc, params); }); };

    CHECK_THAT(refusal({{"type", "angle"}}), ContainsSubstring("An angled plane needs an edge"));
    CHECK_THAT(refusal({{"type", "midplane"}, {"face_a", face_a}}), ContainsSubstring("A midplane needs two faces"));
    CHECK_THAT(refusal({{"type", "midplane"}, {"face_a", face_a}, {"face_b", face_a}}), ContainsSubstring("the same face"));
    CHECK_THAT(refusal({{"type", "tangent"}, {"face_a", face_a}}), ContainsSubstring("A tangent plane needs a cylindrical face"));
    CHECK_THAT(refusal({{"type", "two_edges"}, {"edge_a", {{"body", 0}, {"edge", 0}}}}), ContainsSubstring("This plane needs two edges"));
    CHECK_THAT(refusal({{"type", "slanted"}}), ContainsSubstring("type must be one of offset"));
    CHECK_THAT(refusal({{"type", "midplane"}, {"offset", 5}}), ContainsSubstring("offset is not used by a midplane plane"));
    CHECK_THAT(refusal({{"face_a", face_a}, {"base", "XZ"}}), ContainsSubstring("base is not used by a offset plane on face_a"));
    CHECK_THAT(refusal({{"type", "two_edges"}, {"face_a", face_a}}), ContainsSubstring("face_a is not used"));
    CHECK_THAT(refusal({{"face_a", {{"body", 0}, {"face", 99}}}}), ContainsSubstring("face_a: face 99 of body 0 does not exist"));
    CHECK_THAT(refusal({{"face_a", {{"face", top}}}}), ContainsSubstring("face_a must be {\"body\": b, \"face\": id}"));
    CHECK_THAT(refusal({{"base", {{"body", 0}, {"face", top}}}}), ContainsSubstring("a face goes in face_a"));
    CHECK_THAT(refusal({{"base", {{"datum", 0}}}}), ContainsSubstring("is not a datum plane"));
    CHECK_THAT(refusal({{"base", "ZX"}}), ContainsSubstring("plane must be"));
    CHECK_THAT(refusal({{"angle", 200}}), ContainsSubstring("angle must be a number from -180 to 180"));
    CHECK_THAT(refusal({{"axis", 2}}), ContainsSubstring("axis must be 0"));
    CHECK_THAT(refusal({{"size", {0, 10}}}), ContainsSubstring("size must be [u, v]"));
    CHECK(doc.features.size() == features);

    // The kernel check the Plane card shares: a reference that is gone is refused, not replaced.
    CadFeature stale;
    stale.type       = CadFeatureType::Plane;
    stale.plane_type = PlaneType::Offset;
    stale.plane_face_body = 0;
    stale.plane_face      = 99;
    CHECK_THAT(doc.datum_plane_refusal(stale), ContainsSubstring("no longer on the model"));
    stale.plane_face = top;
    CHECK(doc.datum_plane_refusal(stale).empty());
    stale.plane_type = PlaneType::TwoEdges;
    stale.plane_edge_body = stale.plane_edge2_body = 0;
    stale.plane_edge = 0;
    stale.plane_edge2 = 500;
    CHECK_THAT(doc.datum_plane_refusal(stale), ContainsSubstring("no longer on the model"));
}

TEST_CASE("the rows for topology, measure, holes, push/pull and construct_plane", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // The property `name` of command `key`, which the test needs it to have.
    auto param = [&](const char* key, const char* name) {
        const json p = property_of(command_of(catalog, key), name);
        INFO(key << " " << name);
        REQUIRE(p.is_object());
        return p;
    };
    auto required = [&](const char* key, const char* name) {
        const std::vector<std::string> req = required_of(command_of(catalog, key));
        return std::find(req.begin(), req.end(), name) != req.end();
    };
    CHECK_FALSE(command_of(catalog, "construct_plane").read_only);
    CHECK(command_of(catalog, "body_describe").read_only);
    CHECK(command_of(catalog, "measure").read_only);
    for (const char* t : {"hole", "extrude", "construct_plane"}) {
        INFO(t);
        CHECK_THAT(command_of(catalog, t).description, !ContainsSubstring("ok is false"));
    }
    // Optional everywhere: none of these is required.
    for (const char* k : {"kinds", "edge_kinds", "direction", "at_extreme", "convex", "limit"}) {
        param("body_describe", k);
        CHECK_FALSE(required("body_describe", k));
    }
    param("hole", "at");
    CHECK_FALSE(required("hole", "at"));
    CHECK(param("hole", "body").at("default") == -1);
    param("extrude", "face");
    CHECK_FALSE(required("extrude", "face"));
    for (const char* k : {"face_a", "face_b", "edge_a", "edge_b"}) {
        param("construct_plane", k);
        CHECK_FALSE(required("construct_plane", k));
    }
    CHECK(param("construct_plane", "type").at("enum").size() == 6);
}

namespace {

// The bench's M5 hex standoff: a hexagon 8 mm across flats, 15 mm tall on the world XY plane, with
// a 4.134 mm through bore (M5's tap drill) on its axis.
void add_standoff(CadDocument& doc)
{
    SketchProfile hex;
    for (int i = 0; i < 6; ++i) hex.points.emplace_back(4.618802 * std::cos(PI / 3 * i), 4.618802 * std::sin(PI / 3 * i));
    hex.closed = true;
    doc.add_extrude(doc.add_sketch_profile(hex, SketchPlane::XY(), "Sketch"), 15.0, false, BooleanMode::New, "Extrude");
    doc.add_hole(4.134, 15, true, 0, 0, SketchPlane::XY(), "Hole");
    REQUIRE(doc.recompute());
}

// The first circular edge of body 0 of radius r.
int circle_edge(const CadDocument& doc, double r)
{
    const json rims = GUI::Mcp::cad_body_describe(doc, {{"edge_kinds", {"circle"}}});
    for (const json& e : rims.at("edges"))
        if (std::abs(e.at("radius").get<double>() - r) < 1e-6) return e.at("id").get<int>();
    return -1;
}

// The first straight edge of body 0.
int line_edge(const CadDocument& doc) { return GUI::Mcp::cad_body_describe(doc, {{"edge_kinds", {"line"}}}).at("edges").at(0).at("id").get<int>(); }

BoundingBoxf3 body_box(const CadDocument& doc, size_t body) { return doc.display_body_meshes.at(body).bounding_box(); }

} // namespace

TEST_CASE("the nearest standard thread is matched across series, a bore on its minor diameter", "[Mcp][CAD]")
{
    CHECK(nearest_thread_standard(9.9, false)->name == "M10");
    CHECK(nearest_thread_standard(4.134, true)->name == "M5");   // M5's tap drill
    // Metric and UTS sizes are scanned together, as the Thread card always has: a 5 mm bore is
    // nearer 1/4-20 UNC's minor diameter (4.975) than M6's (4.918).
    CHECK(nearest_thread_standard(5.0, true)->name == "1/4-20 UNC");
    CHECK(nearest_thread_standard(6.35, false)->name == "1/4-20 UNC");   // the first of the equally near 1/4" sizes
}

TEST_CASE("a thread runs on the axis of a picked bore, else of a picked rim", "[Mcp][CAD]")
{
    CadDocument doc;
    add_standoff(doc);
    const int bore = cylinder_face(doc), top = face_facing(doc, Vec3d(0, 0, 1)), rim = circle_edge(doc, 2.067);
    REQUIRE(bore >= 0);
    REQUIRE(rim >= 0);
    SketchPlane                  pl;
    GeometryEngine::CylinderFace cyl;
    bool                         from_face = false;

    REQUIRE(doc.thread_target(0, bore, -1, pl, cyl, &from_face));
    CHECK(from_face);
    CHECK(cyl.internal);
    CHECK_THAT(cyl.radius, WithinAbs(2.067, 1e-9));
    CHECK_THAT(cyl.height, WithinAbs(15.0, 1e-6));
    // On the axis at the bore's base, rising along it through the part's 15 mm.
    CHECK((pl.normal - cyl.axis).norm() < 1e-12);
    CHECK((pl.origin - cyl.base).norm() < 1e-12);
    CHECK_THAT(std::hypot(pl.origin.x(), pl.origin.y()), WithinAbs(0.0, 1e-9));
    CHECK_THAT(std::min(pl.origin.z(), (pl.origin + pl.normal * 15.0).z()), WithinAbs(0.0, 1e-6));
    CHECK_THAT(pl.x_axis.dot(pl.normal), WithinAbs(0.0, 1e-12));
    CHECK_THAT(pl.y_axis.dot(pl.x_axis), WithinAbs(0.0, 1e-12));

    // A face that is not round falls through to the edge, as the card's pick does.
    REQUIRE(doc.thread_target(0, top, rim, pl, cyl, &from_face));
    CHECK_FALSE(from_face);
    CHECK_FALSE(cyl.internal);   // a rim cannot tell
    CHECK_THAT(cyl.height, WithinAbs(0.0, 1e-12));
    CHECK_THAT(std::hypot(pl.origin.x(), pl.origin.y()), WithinAbs(0.0, 1e-9));

    CHECK_FALSE(doc.thread_target(0, top, -1, pl, cyl, &from_face));
    CHECK_FALSE(doc.thread_target(1, bore, -1, pl, cyl));
}

TEST_CASE("thread on a bore takes the nearest standard and the whole bore, as the card does", "[Mcp][CAD]")
{
    CadDocument doc;
    add_standoff(doc);
    json       r;
    const auto add = GUI::Mcp::cad_thread(doc, {{"face", cylinder_face(doc)}}, r);
    CHECK(r.at("designation") == "M5");
    CHECK(r.at("internal") == true);
    CHECK_THAT(r.at("diameter").get<double>(), WithinAbs(5.0, 1e-12));
    CHECK_THAT(r.at("pitch").get<double>(), WithinAbs(0.8, 1e-12));
    CHECK_THAT(r.at("depth").get<double>(), WithinAbs(find_thread_standard("M5")->internal_depth_mm(), 1e-12));
    CHECK_THAT(r.at("length").get<double>(), WithinAbs(15.0, 1e-6));
    CHECK(r.at("x") == 0.0);
    const int t = add("Thread");
    CHECK(doc.features[t].target_body == 0);
    CHECK(doc.features[t].thread_internal);
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    const double tapped = body_volume(doc, 0);
    CHECK(tapped > 600.0);   // the bench case's bounds
    CHECK(tapped < 614.0);

    SECTION("the same thread placed on the XY plane")
    {
        CadDocument plane_doc;
        add_standoff(plane_doc);
        json r2;
        GUI::Mcp::cad_thread(plane_doc, {{"plane", "XY"}, {"designation", "M5"}, {"internal", true}, {"length", 15}}, r2)("Thread");
        CHECK(r2.at("body") == 0);
        CHECK(r2.at("x") == 0.0);
        REQUIRE(plane_doc.recompute());
        CHECK_THAT(body_volume(plane_doc, 0), WithinAbs(tapped, 1.0));
    }
    SECTION("a designation and a pitch replace what was inferred")
    {
        json r3;
        GUI::Mcp::cad_thread(doc, {{"face", cylinder_face(doc)}, {"designation", "M6"}}, r3);
        CHECK(r3.at("designation") == "M6");
        CHECK_THAT(r3.at("depth").get<double>(), WithinAbs(find_thread_standard("M6")->internal_depth_mm(), 1e-12));
        GUI::Mcp::cad_thread(doc, {{"face", cylinder_face(doc)}, {"pitch", 0.5}}, r3);
        CHECK(r3.at("designation").is_null());   // no longer the standard's
        CHECK_THAT(r3.at("depth").get<double>(), WithinAbs(0.5 * 1.0825 * 0.5, 1e-12));
    }
}

TEST_CASE("a custom thread on a plane threads a rod from nothing", "[Mcp][CAD]")
{
    CadDocument doc;
    json        r;
    GUI::Mcp::cad_thread(doc, {{"plane", "XY"}, {"internal", false}, {"diameter", 6}, {"pitch", 1}, {"length", 10}}, r)("Thread");
    CHECK(r.at("designation").is_null());
    CHECK_THAT(r.at("depth").get<double>(), WithinAbs(0.6134, 1e-12));
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    const double v = body_volume(doc, 0);
    CHECK(v < PI * 9.0 * 10.0);
    CHECK(v > PI * (3.0 - 0.6134) * (3.0 - 0.6134) * 10.0);
}

TEST_CASE("thread refuses a target it cannot cut, before anything changes", "[Mcp][CAD]")
{
    CadDocument doc;
    add_standoff(doc);
    const int    bore = cylinder_face(doc), top = face_facing(doc, Vec3d(0, 0, 1));
    const size_t n    = doc.features.size();
    auto refusal = [&](const json& params) {
        json r;
        return failure([&] { GUI::Mcp::cad_thread(doc, params, r); });
    };
    CHECK_THAT(refusal({{"face", top}}), ContainsSubstring("is planar; a thread needs a cylindrical face or a circular edge"));
    CHECK_THAT(refusal({{"face", 99}}), ContainsSubstring("face 99 of body 0 does not exist"));
    CHECK_THAT(refusal({{"edge", line_edge(doc)}}), ContainsSubstring("is a line; a thread needs"));
    CHECK_THAT(refusal({{"face", bore}, {"edge", line_edge(doc)}}), ContainsSubstring("not both"));
    CHECK_THAT(refusal({{"face", bore}, {"x", 1}}), ContainsSubstring("x cannot be given with face"));
    CHECK_THAT(refusal({{"face", bore}, {"designation", "M5x1"}}), ContainsSubstring("unknown designation \"M5x1\"; close: "));
    CHECK_THAT(refusal({{"face", bore}, {"designation", "M5x1"}}), ContainsSubstring("close: M5, M8x1"));
    CHECK_THAT(refusal({{"face", bore}, {"designation", "m5"}}), ContainsSubstring("\"m5\"; close: M5"));
    CHECK_THAT(refusal({{"face", bore}, {"designation", "1/4-20"}}), ContainsSubstring("close: 1/4-20 UNC"));
    CHECK_THAT(refusal({{"face", bore}, {"designation", 5}}), ContainsSubstring("designation must be a string"));
    CHECK_THAT(refusal({{"face", bore}, {"diameter", -1}}), ContainsSubstring("diameter must be a number > 0"));
    CHECK_THAT(refusal({{"face", bore}, {"internal", "yes"}}), ContainsSubstring("internal must be true or false"));
    CHECK_THAT(refusal({{"edge", circle_edge(doc, 2.067)}}), ContainsSubstring("length is required for a thread on an edge"));
    CHECK_THAT(refusal({{"designation", "M5"}, {"length", 15}}), ContainsSubstring("internal is required"));
    CHECK_THAT(refusal({{"internal", true}, {"length", 15}}), ContainsSubstring("needs designation, or diameter and pitch"));
    CHECK_THAT(refusal({{"internal", true}, {"designation", "M5"}}), ContainsSubstring("length is required for a thread on a plane"));
    CHECK_THAT(refusal({{"internal", true}, {"designation", "M5"}, {"length", 5}, {"body", 3}}), ContainsSubstring("out of range"));
    CHECK(doc.features.size() == n);

    CadDocument empty;
    json        r;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_thread(empty, {{"internal", true}, {"designation", "M5"}, {"length", 5}}, r); }),
               ContainsSubstring("needs a body to tap"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_thread(empty, {{"face", 0}}, r); }), ContainsSubstring("no body to thread"));
    // No body is the design's state; a face of the wrong kind is the call's choice.
    CHECK(failure_code([&] { GUI::Mcp::cad_thread(empty, {{"internal", true}, {"designation", "M5"}, {"length", 5}}, r); }) ==
          "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_thread(empty, {{"face", 0}}, r); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_thread(doc, {{"face", top}}, r); }) == "invalid_argument");
}

TEST_CASE("a sweep along a helix keeps its profile at the same angle to the helix axis", "[Mcp][CAD]")
{
    // A spring: radius 10, pitch 5, six turns, its profile drawn on the XZ plane at the helix start.
    CadDocument doc;
    const int   helix = doc.add_helix(SketchPlane::XY(), 10.0, 5.0, 30.0, false, 0.0, "Helix");
    const double turn = std::hypot(2.0 * PI * 10.0, 5.0), length = 6.0 * turn;
    // The XZ plane is tilted off the helix's normal plane by its lead angle.
    const double across = 2.0 * PI * 10.0 / turn;

    SECTION("a round wire")
    {
        const int s = doc.add_sketch_entities({circle(10, 0, 1.5)}, SketchPlane::XZ(), "Sketch");
        GUI::Mcp::cad_sweep(doc, {{"sketch", s}, {"path", helix}})("Sweep");
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 1);
        CHECK_THAT(body_volume(doc, 0), Catch::Matchers::WithinRel(PI * 1.5 * 1.5 * across * length, 0.01));
    }
    SECTION("a square wire stays square to the axis: its outer face stays at radius 11")
    {
        const int s = doc.add_sketch_entities({line(9, -1, 11, -1), line(11, -1, 11, 1), line(11, 1, 9, 1), line(9, 1, 9, -1)},
                                              SketchPlane::XZ(), "Sketch");
        GUI::Mcp::cad_sweep(doc, {{"sketch", s}, {"path", helix}, {"operation", "new_body"}})("Sweep");
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 1);
        CHECK_THAT(body_volume(doc, 0), Catch::Matchers::WithinRel(4.0 * across * length, 0.01));
        // A profile that turned about the path would reach out to 10 + sqrt(2) at its corners.
        const BoundingBoxf3 bb = body_box(doc, 0);
        CHECK_THAT(bb.max.x(), WithinAbs(11.0, 0.02));
        CHECK_THAT(bb.min.x(), WithinAbs(-11.0, 0.02));
        CHECK_THAT(bb.max.y(), WithinAbs(11.0, 0.02));
        CHECK_THAT(bb.min.y(), WithinAbs(-11.0, 0.02));
    }
}

TEST_CASE("sweep takes a sketch or a helix as its path and refuses anything else", "[Mcp][CAD]")
{
    CadDocument doc;
    const int helix = doc.add_helix(SketchPlane::XY(), 10.0, 5.0, 10.0, false, 0.0, "Helix");
    const int s     = doc.add_sketch_entities({circle(10, 0, 1.5)}, SketchPlane::XZ(), "Sketch");
    const int spine = doc.add_sketch_entities({line(0, 0, 0, 30)}, SketchPlane::YZ(), "Spine");
    const size_t n  = doc.features.size();
    auto refusal = [&](const json& params) { return failure([&] { GUI::Mcp::cad_sweep(doc, params); }); };
    CHECK(refusal({{"sketch", s}, {"path", spine}}).empty());
    CHECK_THAT(refusal({{"sketch", s}}), ContainsSubstring("path must be the feature index of a sketch or a helix"));
    CHECK_THAT(refusal({{"sketch", s}, {"path", s}}), ContainsSubstring("cannot be the profile sketch"));
    CHECK_THAT(refusal({{"sketch", s}, {"path", 9}}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({{"sketch", helix}, {"path", spine}}), ContainsSubstring("is not a sketch"));
    CHECK_THAT(refusal({{"sketch", s}, {"path", helix}, {"operation", "join"}}), ContainsSubstring("needs a body"));
    CHECK_THAT(refusal({{"sketch", s}, {"path", helix}, {"operation", "union"}}), ContainsSubstring("operation must be"));
    add_box(doc);
    const int ex = int(doc.features.size()) - 1;
    CHECK_THAT(refusal({{"sketch", s}, {"path", ex}}), ContainsSubstring("(Extrude) is not a sketch or a helix"));
    CHECK(doc.features.size() == n + 2);
}

TEST_CASE("loft skins sketches on two planes in order, and refuses what is not a sketch", "[Mcp][CAD]")
{
    CadDocument doc;
    SketchPlane up = SketchPlane::XY();
    up.origin.z()  = 40;
    const int base = doc.add_sketch_entities({line(-10, -10, 10, -10), line(10, -10, 10, 10), line(10, 10, -10, 10), line(-10, 10, -10, -10)},
                                             SketchPlane::XY(), "Base");
    const int top  = doc.add_sketch_entities({circle(0, 0, 8)}, up, "Top");
    const size_t n = doc.features.size();
    auto refusal = [&](const json& params) { return failure([&] { GUI::Mcp::cad_loft(doc, params); }); };
    CHECK_THAT(refusal({{"profiles", json::array({base})}}), ContainsSubstring("2 or more"));
    CHECK_THAT(refusal({{"profiles", base}}), ContainsSubstring("profiles must be a list"));
    CHECK_THAT(refusal({{"profiles", {base, base}}}), ContainsSubstring("listed twice"));
    CHECK_THAT(refusal({{"profiles", {base, "1"}}}), ContainsSubstring("profiles must hold sketch feature indices"));
    CHECK_THAT(refusal({{"profiles", {base, 7}}}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({{"profiles", {base, top}}, {"ruled", "yes"}}), ContainsSubstring("ruled must be true or false"));
    CHECK_THAT(refusal({{"profiles", {base, top}}, {"operation", "cut"}}), ContainsSubstring("needs a body"));
    CHECK(doc.features.size() == n);

    const int l = GUI::Mcp::cad_loft(doc, {{"profiles", {base, top}}})("Loft");
    CHECK(doc.features[l].loft_profile_refs == std::vector<int>({base, top}));
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    const BoundingBoxf3 bb = body_box(doc, 0);
    CHECK_THAT(bb.min.z(), WithinAbs(0.0, 1e-3));
    CHECK_THAT(bb.max.z(), WithinAbs(40.0, 1e-3));
    const double v = body_volume(doc, 0);
    CHECK(v > PI * 64.0 * 40.0);   // between a cylinder of the top and a prism of the base
    CHECK(v < 400.0 * 40.0);
    CHECK_THAT(refusal({{"profiles", {base, l}}}), ContainsSubstring("is not a sketch"));
}

TEST_CASE("split cuts a body with a plane and keeps either piece", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);   // 40 x 20 x 10, z 0..10
    json r;

    SECTION("both pieces, the upper one in the body's place")
    {
        GUI::Mcp::cad_split(doc, {{"plane", "XY"}, {"offset", 4}}, r)("Cut");
        CHECK(doc.features.back().type == CadFeatureType::Cut);
        CHECK(r.at("upper_body") == 0);
        CHECK(r.at("lower_body") == 1);
        CHECK_THAT(r.at("frame").at("origin")[2].get<double>(), WithinAbs(4.0, 1e-12));
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 2);
        CHECK_THAT(body_volume(doc, 0), WithinAbs(800.0 * 6.0, 1e-6));
        CHECK_THAT(body_volume(doc, 1), WithinAbs(800.0 * 4.0, 1e-6));
    }
    SECTION("flip turns the plane over")
    {
        GUI::Mcp::cad_split(doc, {{"plane", "XY"}, {"offset", 4}, {"flip", true}}, r)("Cut");
        CHECK_THAT(r.at("frame").at("normal")[2].get<double>(), WithinAbs(-1.0, 1e-12));
        REQUIRE(doc.recompute());
        CHECK_THAT(body_volume(doc, 0), WithinAbs(800.0 * 4.0, 1e-6));
    }
    SECTION("one piece")
    {
        GUI::Mcp::cad_split(doc, {{"plane", "XY"}, {"offset", 4}, {"keep_upper", false}}, r)("Cut");
        CHECK_FALSE(r.contains("upper_body"));
        CHECK(r.at("lower_body") == 0);
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 1);
        CHECK_THAT(body_volume(doc, 0), WithinAbs(800.0 * 4.0, 1e-6));
    }
    SECTION("a face's plane, 3 mm into the body")
    {
        GUI::Mcp::cad_split(doc, {{"plane", {{"body", 0}, {"face", face_facing(doc, Vec3d(0, 0, 1))}}}, {"offset", -3}}, r)("Cut");
        REQUIRE(doc.recompute());
        REQUIRE(doc.bodies.size() == 2);
        CHECK_THAT(body_volume(doc, 0), WithinAbs(800.0 * 3.0, 1e-6));   // outside the face's plane
    }
    SECTION("along a face, as before")
    {
        GUI::Mcp::cad_split(doc, {{"face", 2}, {"keep_lower", false}}, r)("Split");
        CHECK(doc.features.back().cut_face == 2);
        CHECK(doc.features.back().cut_keep_lower == false);
        CHECK(r.at("upper_body") == 0);
        CHECK_FALSE(r.contains("frame"));
    }
    SECTION("refusals")
    {
        const size_t n = doc.features.size();
        auto refusal = [&](const json& params) { return failure([&] { GUI::Mcp::cad_split(doc, params, r); }); };
        CHECK_THAT(refusal({{"plane", "XY"}, {"face", 2}}), ContainsSubstring("not both"));
        CHECK_THAT(refusal(json::object()), ContainsSubstring("split needs plane"));
        CHECK_THAT(refusal({{"plane", "XY"}, {"offset", 20}}), ContainsSubstring("the plane misses body 0: all of it lies on its lower side"));
        CHECK_THAT(refusal({{"plane", "XY"}}), ContainsSubstring("the plane misses body 0: all of it lies on its upper side"));
        CHECK_THAT(refusal({{"plane", "XY"}, {"offset", 4}, {"keep_upper", false}, {"keep_lower", false}}), ContainsSubstring("nothing would be kept"));
        CHECK_THAT(refusal({{"plane", "XY"}, {"offset", "4"}}), ContainsSubstring("offset must be a number"));
        CHECK_THAT(refusal({{"plane", "XY"}, {"offset", 4}, {"face_body", 0}}), ContainsSubstring("face_body goes with face"));
        CHECK_THAT(refusal({{"face", 2}, {"offset", 1}}), ContainsSubstring("offset goes with plane"));
        CHECK_THAT(refusal({{"face", 99}}), ContainsSubstring("face 99 of body 0 does not exist"));
        CHECK_THAT(refusal({{"plane", "XY"}, {"offset", 4}, {"body", 2}}), ContainsSubstring("out of range"));
        CHECK_THAT(refusal({{"plane", "QQ"}}), ContainsSubstring("plane must be"));
        CHECK(doc.features.size() == n);
        CadDocument empty;
        CHECK_THAT(failure([&] { GUI::Mcp::cad_split(empty, {{"plane", "XY"}}, r); }), ContainsSubstring("no body to split"));
        // A plane that misses the body is where the call put it; no body is the design's state.
        CHECK(failure_code([&] { GUI::Mcp::cad_split(doc, {{"plane", "XY"}, {"offset", 20}}, r); }) == "invalid_argument");
        CHECK(failure_code([&] { GUI::Mcp::cad_split(doc, {{"face", 99}}, r); }) == "invalid_argument");
        CHECK(failure_code([&] { GUI::Mcp::cad_split(empty, {{"plane", "XY"}}, r); }) == "precondition");
    }
}

TEST_CASE("the rows for thread, sweep, loft, split and helix", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* t : {"thread", "sweep", "loft", "split"}) {
        INFO(t);
        CHECK_FALSE(command_of(catalog, t).read_only);
        CHECK_THAT(command_of(catalog, t).description, !ContainsSubstring("ok is false"));
    }
    // These of thread and split are optional; sweep's and loft's, which have no default, are required.
    for (const char* k : {"face", "edge", "designation", "internal", "diameter", "pitch", "depth", "length", "plane", "at"}) {
        INFO(k);
        CHECK(property_of(command_of(catalog, "thread"), k).is_object());
    }
    CHECK(required_of(command_of(catalog, "thread")).empty());
    CHECK(required_of(command_of(catalog, "sweep")) == std::vector<std::string>{"sketch", "path"});
    CHECK(required_of(command_of(catalog, "loft")) == std::vector<std::string>{"profiles"});
    CHECK(required_of(command_of(catalog, "split")).empty());
    CHECK(property_of(command_of(catalog, "split"), "plane").is_object());
    CHECK(property_of(command_of(catalog, "split"), "face").is_object());
    CHECK(property_of(command_of(catalog, "split"), "face_body").at("default") == -1);
    CHECK_THAT(command_of(catalog, "helix").description, ContainsSubstring("start and tangent"));
}

// --- The open sketch: shapes, edit ops and constraints (cad_model A5, A7, A8) -----------------

namespace {

// A fresh sketch session on XY, as sketch_create leaves it: the Select tool armed.
void begin_sketch(GUI::DesignSketchTool& tool) { tool.begin(SketchPlane::XY(), GUI::DesignSketchTool::Mode::Select); }

json sketch(GUI::DesignSketchTool& tool, const char* name, const json& params)
{
    return GUI::Mcp::cad_sketch_call(tool, name, params);
}

// The L-bracket's profile: 40 x 30, 4 thick, its inside corner between lines 2 and 3 at (4, 4).
json l_bracket()
{
    return json::array({
        {{"type", "line"}, {"p0", {0, 0}}, {"p1", {40, 0}}},
        {{"type", "line"}, {"p0", {40, 0}}, {"p1", {40, 4}}},
        {{"type", "line"}, {"p0", {40, 4}}, {"p1", {4, 4}}},
        {{"type", "line"}, {"p0", {4, 4}}, {"p1", {4, 30}}},
        {{"type", "line"}, {"p0", {4, 30}}, {"p1", {0, 30}}},
        {{"type", "line"}, {"p0", {0, 30}}, {"p1", {0, 0}}},
    });
}

double only_loop_area(const GUI::DesignSketchTool& tool)
{
    const GUI::DesignSketchTool::LoopReport rep = tool.loop_report();
    REQUIRE(rep.loops.size() == 1);
    CHECK(rep.open_ends.empty());
    return rep.loops[0].area;
}

BoundingBoxf entities_box(const std::vector<SketchEntity>& ents, size_t first, size_t count)
{
    BoundingBoxf box;
    for (size_t i = first; i < first + count; ++i) {
        box.merge(ents[i].p0);
        box.merge(ents[i].p1);
    }
    return box;
}

} // namespace

TEST_CASE("sketch_draw draws polygons, slots, ellipses and splines as their tools do", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    using ET = SketchEntity::Type;

    // A hexagon 8 across the flats: six lines, the first edge's midpoint on +x at the apothem.
    json r = sketch(tool, "sketch_draw", {{"type", "polygon"}, {"center", {0, 0}}, {"sides", 6}, {"across_flats", 8}});
    CHECK(r.at("first_index") == 0);
    CHECK(r.at("added") == 6);
    CHECK(r.at("shapes") == json::array({{{"first", 0}, {"count", 6}}}));
    const auto& ents = tool.entities();
    REQUIRE(ents.size() == 6);
    for (const SketchEntity& e : ents) CHECK(e.type == ET::Line);
    const Vec2d mid = 0.5 * (ents[0].p0 + ents[0].p1);
    CHECK_THAT(mid.x(), WithinAbs(4.0, 1e-9));
    CHECK_THAT(mid.y(), WithinAbs(0.0, 1e-9));
    const BoundingBoxf hex = entities_box(ents, 0, 6);
    CHECK_THAT(hex.size().x(), WithinAbs(8.0, 1e-9));   // opposite flats 8 apart
    CHECK_THAT(hex.size().y(), WithinAbs(8.0 / std::cos(PI / 6.0), 1e-9));
    CHECK(only_loop_area(tool) > 0);

    // The inscribed form puts its first vertex at the radius, turned by angle_deg.
    r = sketch(tool, "sketch_draw", {{"type", "polygon"}, {"center", {50, 0}}, {"sides", 4}, {"radius", 5}, {"angle_deg", 90}});
    CHECK_THAT(ents[6].p0.x(), WithinAbs(50.0, 1e-9));
    CHECK_THAT(ents[6].p0.y(), WithinAbs(5.0, 1e-9));

    // A slot, an ellipse and its arc, and a spline in one list, with a line between them.
    tool.cancel();
    begin_sketch(tool);
    r = sketch(tool, "sketch_draw",
               {{"entities",
                 {{{"type", "slot"}, {"c0", {0, 0}}, {"c1", {20, 0}}, {"width", 5}},
                  {{"type", "line"}, {"p0", {0, -20}}, {"p1", {10, -20}}},
                  {{"type", "ellipse"}, {"center", {40, 0}}, {"radius", 10}, {"rminor", 6}, {"rotation", 0.5236}},
                  {{"type", "ellipse_arc"}, {"center", {70, 0}}, {"radius", 10}, {"rminor", 6}, {"start_angle", 0}, {"end_angle_deg", 180}},
                  {{"type", "spline"}, {"ctrl", {{0, 20}, {10, 30}, {20, 20}, {30, 30}}}}}}});
    CHECK(r.at("shapes") == json::parse(R"([{"first":0,"count":4},{"first":4,"count":1},{"first":5,"count":1},
                                             {"first":6,"count":1},{"first":7,"count":1}])"));
    CHECK(r.at("entities") == 8);
    const GUI::DesignSketchTool::LoopReport rep = tool.loop_report();
    bool slot_found = false;
    for (const auto& l : rep.loops)
        if (l.ents.size() == 4) {
            slot_found = true;
            CHECK_THAT(l.area, WithinAbs(20.0 * 5.0 + PI * 2.5 * 2.5, 1e-6));
        }
    CHECK(slot_found);
    const SketchEntity& el = ents[5];
    CHECK(el.type == ET::Ellipse);
    CHECK_THAT(el.radius, WithinAbs(10.0, 1e-9));
    CHECK_THAT(el.rminor, WithinAbs(6.0, 1e-9));
    CHECK_THAT(el.rotation, WithinAbs(0.5236, 1e-9));
    const SketchEntity& arc = ents[6];
    CHECK(arc.type == ET::EllipseArc);
    CHECK_THAT(arc.start_angle, WithinAbs(0.0, 1e-9));
    CHECK_THAT(arc.end_angle, WithinAbs(PI, 1e-9));
    CHECK_THAT((arc.p0 - Vec2d(80, 0)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((arc.p1 - Vec2d(60, 0)).norm(), WithinAbs(0.0, 1e-9));
    CHECK(ents[7].type == ET::BSpline);
    CHECK(ents[7].ctrl.size() == 4);
    CHECK(ents[7].ctrl[3] == Vec2d(30, 30));

    // Construction comes from the item, whatever the tool's sticky setting says.
    tool.set_construction(true);
    tool.set_polygon_circumscribed(true);
    sketch(tool, "sketch_draw", {{"type", "polygon"}, {"center", {0, 60}}, {"sides", 3}, {"radius", 5}});
    CHECK_FALSE(ents.back().construction);
    CHECK_THAT((ents[8].p0 - Vec2d(5, 60)).norm(), WithinAbs(0.0, 1e-9));   // inscribed: a vertex at the radius
    tool.set_tool(GUI::DesignSketchTool::Mode::Polygon);
    sketch(tool, "sketch_draw", {{"type", "slot"}, {"c0", {0, 80}}, {"c1", {10, 80}}, {"width", 2}, {"construction", true}});
    CHECK(ents.back().construction);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Polygon);   // the armed tool is left as it was
    CHECK(tool.pending_points() == 0);

    // Refusals change nothing, even when only a later item of a list is wrong.
    const size_t n = ents.size();
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_draw", params); }); };
    CHECK_THAT(refused({{"type", "polygon"}, {"center", {0, 0}}, {"sides", 2}, {"radius", 5}}), ContainsSubstring("3 to 1000"));
    CHECK_THAT(refused({{"type", "polygon"}, {"center", {0, 0}}, {"sides", 6}, {"radius", 5}, {"across_flats", 8}}),
               ContainsSubstring("one of them"));
    CHECK_THAT(refused({{"type", "polygon"}, {"sides", 6}, {"radius", 5}}), ContainsSubstring("center must be [x, y]"));
    CHECK_THAT(refused({{"type", "slot"}, {"c0", {0, 0}}, {"c1", {0, 0}}, {"width", 5}}), ContainsSubstring("must be apart"));
    CHECK_THAT(refused({{"type", "slot"}, {"c0", {0, 0}}, {"c1", {9, 0}}, {"width", 0}}), ContainsSubstring("at least"));
    CHECK_THAT(refused({{"type", "ellipse"}, {"center", {0, 0}}, {"radius", 5}, {"rminor", 6}}), ContainsSubstring("must not exceed"));
    CHECK_THAT(refused({{"type", "ellipse_arc"}, {"center", {0, 0}}, {"radius", 5}, {"rminor", 3}, {"start_angle", 0}}),
               ContainsSubstring("end_angle"));
    CHECK_THAT(refused({{"type", "ellipse_arc"}, {"center", {0, 0}}, {"radius", 5}, {"rminor", 3}, {"start_angle", 0}, {"end_angle", 2 * PI}}),
               ContainsSubstring("whole turn"));
    CHECK_THAT(refused({{"type", "arc_center"}, {"center", {0, 0}}, {"radius", 5}, {"start_angle", 0}, {"start_angle_deg", 0}}),
               ContainsSubstring("not both"));
    CHECK_THAT(refused({{"type", "spline"}, {"ctrl", {{0, 0}}}}), ContainsSubstring("2 or more poles"));
    CHECK_THAT(refused({{"type", "spline"}, {"ctrl", {{0, 0}, {0, 0}}}}), ContainsSubstring("coincide"));
    CHECK_THAT(refused({{"entities", {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {1, 0}}}, {{"type", "gear"}}}}}),
               ContainsSubstring("unknown type 'gear'"));
    CHECK_THAT(refused(json::object()), ContainsSubstring("needs"));
    CHECK(ents.size() == n);

    // An arc's angles may be given in degrees too.
    sketch(tool, "sketch_draw", {{"type", "arc_center"}, {"center", {0, 0}}, {"radius", 5}, {"start_angle_deg", 0}, {"end_angle_deg", 90}});
    CHECK_THAT(ents.back().end_angle, WithinAbs(PI / 2, 1e-12));
}

TEST_CASE("a rectangle is a type, drawn as the Corner rectangle tool draws it", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    const json r = sketch(tool, "sketch_draw", {{"type", "rectangle_2_point"}, {"p0", {-15, -10}}, {"p1", {15, 10}}});
    CHECK(r.at("added") == 4);
    CHECK(r.at("shapes") == json::array({{{"first", 0}, {"count", 4}}}));
    const auto& ents = tool.entities();
    REQUIRE(ents.size() == 4);
    // Four lines from p0 round to p1 and back, one closed loop of 30 x 20.
    const std::vector<Vec2d> corners = {{-15, -10}, {15, -10}, {15, 10}, {-15, 10}};
    for (size_t i = 0; i < 4; ++i) {
        INFO(i);
        CHECK(ents[i].type == SketchEntity::Type::Line);
        CHECK_THAT((ents[i].p0 - corners[i]).norm(), WithinAbs(0.0, 1e-12));
        CHECK_THAT((ents[i].p1 - corners[(i + 1) % 4]).norm(), WithinAbs(0.0, 1e-12));
    }
    CHECK_THAT(only_loop_area(tool), WithinAbs(600.0, 1e-9));
    // Its corners are joined and its sides kept horizontal and vertical: of the sixteen degrees of freedom of four
    // free lines, its position, width and height are left.
    auto count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(),
                             [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::Horizontal) == 2);
    CHECK(count(T::Vertical) == 2);
    CHECK(r.at("dof").get<int>() < 16);
    CHECK(r.at("dof") == 4);
    // One undo of the sketch takes the four lines off together: they are one shape.
    REQUIRE(tool.undo_last_entity());
    CHECK(tool.entities().empty());

    // Corners on one line make no rectangle, and nothing is drawn.
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", {{"type", "rectangle_2_point"}, {"p0", {0, 0}}, {"p1", {5, 0}}}); }),
               ContainsSubstring("must differ in x and in y"));
    CHECK(tool.entities().empty());

    // rect is no argument: the schema refuses it as unknown. The block declares each field of its shapes, none
    // required, and names in the field's description the types that take it.
    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& draw    = command_of(catalog, "sketch_draw");
    CHECK(property_of(draw, "rect").is_null());
    json rect{{"rect", {0, 0, 1, 1}}};
    CHECK_THAT(GUI::Mcp::argument_error(draw, rect), ContainsSubstring("unknown argument rect"));
    const json types = property_of(draw, "type").at("enum");
    for (const char* t : {"rectangle_2_point", "circle_center", "arc_center"}) {
        INFO(t);
        CHECK(std::find(types.begin(), types.end(), json(t)) != types.end());
    }
    for (const char* t : {"rect", "circle", "arc"}) {
        INFO(t);
        CHECK(std::find(types.begin(), types.end(), json(t)) == types.end());
    }
    CHECK(required_of(draw).empty());
    const std::vector<const char*> two_points = {"line",           "rectangle_2_point", "rectangle_3_point", "rectangle_rounded",
                                                 "circle_2_point", "circle_3_point",    "arc_3_point"};
    const std::vector<std::pair<const char*, std::vector<const char*>>> fields = {
        {"p0", two_points},
        {"p1", two_points},
        {"points", {"polyline"}},
        {"corner", {"rectangle_center"}},
        {"p2", {"circle_3_point"}},
        {"on_arc", {"arc_3_point"}},
        {"end", {"arc_tangent", "slot_arc"}},
        {"entity", {"arc_tangent"}},
        {"start", {"slot_arc"}},
        {"center", {"rectangle_center", "circle_center", "arc_center", "slot_arc", "ellipse", "ellipse_arc", "polygon"}},
        {"radius", {"rectangle_rounded", "circle_center", "arc_center", "ellipse", "ellipse_arc", "polygon"}},
        {"start_angle", {"arc_center", "ellipse_arc"}},
        {"start_angle_deg", {"arc_center", "ellipse_arc"}},
        {"end_angle", {"arc_center", "ellipse_arc"}},
        {"end_angle_deg", {"arc_center", "ellipse_arc"}},
        {"c0", {"slot"}},
        {"c1", {"slot"}},
        {"width", {"rectangle_3_point", "slot", "slot_arc"}},
        {"rminor", {"ellipse", "ellipse_arc"}},
        {"rotation", {"ellipse", "ellipse_arc"}},
        {"rotation_deg", {"ellipse", "ellipse_arc"}},
        {"ctrl", {"spline"}},
        {"sides", {"polygon"}},
        {"across_flats", {"polygon"}},
        {"angle", {"polygon"}},
        {"angle_deg", {"polygon"}},
        {"p", {"point"}},
    };
    for (const auto& [field, takers] : fields) {
        INFO(field);
        const json p = property_of(draw, field);
        REQUIRE(p.is_object());
        CHECK(p.contains("default"));
        for (const char* t : takers)
            CHECK_THAT(p.value("description", std::string()), ContainsSubstring(t));
    }
}

TEST_CASE("every drawing tool's shape is drawn from arguments", "[Mcp][CAD]")
{
    using ET = SketchEntity::Type;
    using T  = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    const auto& ents    = tool.entities();
    auto        draw    = [&](const json& params) { return sketch(tool, "sketch_draw", params); };
    auto        refused = [&](const json& params) { return failure([&] { draw(params); }); };
    auto        dist    = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm(); };
    auto        of_type = [&](ET type) {
        return std::count_if(ents.begin(), ents.end(), [type](const SketchEntity& e) { return e.type == type; });
    };
    // How many constraints of `type` bind entities a and b to each other.
    auto binding = [&](T type, int a, int b) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [&](const SketchEntityConstraintDef& c) {
            return c.type == type && ((c.ea == a && c.eb == b) || (c.ea == b && c.eb == a));
        });
    };
    // A rectangle's four lines, from its first corner round: each starts where `corners` says.
    auto check_corners = [&](size_t first, const std::vector<Vec2d>& corners) {
        REQUIRE(ents.size() >= first + 4);
        for (size_t i = 0; i < 4; ++i) {
            INFO("corner " << i);
            CHECK(ents[first + i].type == ET::Line);
            CHECK_THAT(dist(ents[first + i].p0, corners[i]), WithinAbs(0.0, 1e-12));
            CHECK_THAT(dist(ents[first + i].p1, corners[(i + 1) % 4]), WithinAbs(0.0, 1e-12));
        }
    };

    SECTION("polyline")
    {
        // Two lines sharing an end, open.
        json r = draw({{"type", "polyline"}, {"points", {{0, 0}, {10, 0}, {10, 5}}}});
        CHECK(r.at("shapes") == json::array({{{"first", 0}, {"count", 2}}}));
        REQUIRE(ents.size() == 2);
        CHECK(of_type(ET::Line) == 2);
        CHECK_THAT(dist(ents[0].p1, ents[1].p0), WithinAbs(0.0, 0.0));
        CHECK(binding(T::Coincident, 0, 1) == 1);
        CHECK(tool.loop_report().loops.empty());
        // With a fourth point on the first, three lines in one closed loop.
        tool.cancel();
        begin_sketch(tool);
        r = draw({{"type", "polyline"}, {"points", {{0, 0}, {10, 0}, {10, 5}, {0, 0}}}});
        REQUIRE(ents.size() == 3);
        CHECK(of_type(ET::Line) == 3);
        const GUI::DesignSketchTool::LoopReport rep = tool.loop_report();
        REQUIRE(rep.loops.size() == 1);
        CHECK(rep.loops[0].ents.size() == 3);
        CHECK(rep.open_ends.empty());
        CHECK_THAT(std::abs(rep.loops[0].area), WithinAbs(25.0, 1e-9));
    }
    SECTION("rectangle_center")
    {
        const json r = draw({{"type", "rectangle_center"}, {"center", {0, 0}}, {"corner", {6, 4}}});
        CHECK(r.at("shapes") == json::array({{{"first", 0}, {"count", 4}}}));
        CHECK(ents.size() == 4);
        check_corners(0, {{-6, -4}, {6, -4}, {6, 4}, {-6, 4}});
    }
    SECTION("rectangle_3_point")
    {
        draw({{"type", "rectangle_3_point"}, {"p0", {0, 0}}, {"p1", {10, 0}}, {"width", 4}});
        CHECK(ents.size() == 4);
        check_corners(0, {{0, 0}, {10, 0}, {10, 4}, {0, 4}});
        // A negative width puts the far side to the right of p0 to p1.
        tool.cancel();
        begin_sketch(tool);
        draw({{"type", "rectangle_3_point"}, {"p0", {0, 0}}, {"p1", {10, 0}}, {"width", -4}});
        check_corners(0, {{0, 0}, {10, 0}, {10, -4}, {0, -4}});
    }
    SECTION("rectangle_rounded")
    {
        draw({{"type", "rectangle_rounded"}, {"p0", {0, 0}}, {"p1", {20, 10}}, {"radius", 2}});
        CHECK(ents.size() == 8);
        CHECK(of_type(ET::Line) == 4);
        CHECK(of_type(ET::Arc) == 4);
        for (const SketchEntity& e : ents)
            if (e.type == ET::Arc)
                CHECK_THAT(e.radius, WithinAbs(2.0, 1e-12));
        // The 20 x 10 box less its four corner squares, plus their quarter discs.
        CHECK_THAT(std::abs(only_loop_area(tool)), WithinAbs(200.0 - 4.0 * (4.0 - PI), 1e-9));
    }
    SECTION("circle_2_point")
    {
        draw({{"type", "circle_2_point"}, {"p0", {0, 0}}, {"p1", {10, 0}}});
        REQUIRE(ents.size() == 1);
        CHECK(ents[0].type == ET::Circle);
        CHECK_THAT(dist(ents[0].center, Vec2d(5, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[0].radius, WithinAbs(5.0, 1e-12));
    }
    SECTION("circle_3_point")
    {
        draw({{"type", "circle_3_point"}, {"p0", {5, 0}}, {"p1", {0, 5}}, {"p2", {-5, 0}}});
        REQUIRE(ents.size() == 1);
        CHECK(ents[0].type == ET::Circle);
        CHECK_THAT(dist(ents[0].center, Vec2d(0, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[0].radius, WithinAbs(5.0, 1e-12));
    }
    SECTION("arc_3_point")
    {
        draw({{"type", "arc_3_point"}, {"p0", {5, 0}}, {"p1", {-5, 0}}, {"on_arc", {0, 5}}});
        REQUIRE(ents.size() == 1);
        const SketchEntity& a = ents[0];
        CHECK(a.type == ET::Arc);
        CHECK_THAT(dist(a.center, Vec2d(0, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(a.radius, WithinAbs(5.0, 1e-12));
        CHECK_THAT(dist(a.p0, Vec2d(5, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(dist(a.p1, Vec2d(-5, 0)), WithinAbs(0.0, 1e-12));
        // It runs through on_arc: the point half way along its sweep.
        const double mid = 0.5 * (a.start_angle + a.end_angle);
        CHECK_THAT(dist(a.center + a.radius * Vec2d(std::cos(mid), std::sin(mid)), Vec2d(0, 5)), WithinAbs(0.0, 1e-12));
    }
    SECTION("arc_tangent")
    {
        draw({{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 0}}});
        json r = draw({{"type", "arc_tangent"}, {"end", {15, 5}}});
        CHECK(r.at("shapes") == json::array({{{"first", 1}, {"count", 1}}}));
        REQUIRE(ents.size() == 2);
        const SketchEntity& a = ents[1];
        CHECK(a.type == ET::Arc);
        CHECK_THAT(dist(a.p0, Vec2d(10, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(dist(a.p1, Vec2d(15, 5)), WithinAbs(0.0, 1e-12));
        // It leaves the line's end along +x: square to its radius there, in the way it sweeps.
        const Vec2d  radial = a.p0 - a.center;
        const Vec2d  along  = (a.end_angle > a.start_angle ? Vec2d(-radial.y(), radial.x()) : Vec2d(radial.y(), -radial.x())).normalized();
        CHECK_THAT(dist(along, Vec2d(1, 0)), WithinAbs(0.0, 1e-12));
        CHECK(binding(T::Coincident, 0, 1) == 1);
        CHECK(binding(T::Tangent, 0, 1) == 1);
        // After another line, entity names the line it leaves: it starts at the first line's end.
        draw({{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}});
        draw({{"type", "arc_tangent"}, {"end", {15, 5}}, {"entity", 0}});
        REQUIRE(ents.size() == 4);
        CHECK_THAT(dist(ents[3].p0, Vec2d(10, 0)), WithinAbs(0.0, 1e-12));

        // It needs a line or an arc to leave: none in an empty sketch, and a circle is neither.
        tool.cancel();
        begin_sketch(tool);
        CHECK_THAT(refused({{"type", "arc_tangent"}, {"end", {5, 5}}}), ContainsSubstring("needs a line or an arc to leave"));
        draw({{"type", "circle_center"}, {"center", {0, 0}}, {"radius", 3}});
        CHECK_THAT(refused({{"type", "arc_tangent"}, {"end", {5, 5}}, {"entity", 0}}), ContainsSubstring("circle 0 is neither"));
        CHECK_THAT(refused({{"type", "arc_tangent"}, {"end", {5, 5}}}), ContainsSubstring("is neither"));
        CHECK_THAT(refused({{"type", "arc_tangent"}, {"end", {5, 5}}, {"entity", 4}}), ContainsSubstring("entity 4 does not exist"));
        // Its end on its start is no arc, and a list whose arc_tangent follows a circle draws nothing.
        draw({{"type", "line"}, {"p0", {10, 10}}, {"p1", {20, 10}}});
        CHECK_THAT(refused({{"type", "arc_tangent"}, {"end", {20, 10}}}), ContainsSubstring("apart from its start"));
        CHECK_THAT(refused({{"entities",
                             {{{"type", "line"}, {"p0", {0, 30}}, {"p1", {10, 30}}},
                              {{"type", "circle_2_point"}, {"p0", {20, 30}}, {"p1", {30, 30}}},
                              {{"type", "arc_tangent"}, {"end", {40, 40}}}}}}),
                   ContainsSubstring("is neither"));
        CHECK(ents.size() == 2);
    }
    SECTION("slot_arc")
    {
        const json r = draw({{"type", "slot_arc"}, {"center", {0, 0}}, {"start", {10, 0}}, {"end", {0, 10}}, {"width", 2}});
        CHECK(r.at("shapes") == json::array({{{"first", 0}, {"count", 4}}}));
        REQUIRE(ents.size() == 4);
        CHECK(of_type(ET::Arc) == 4);
        // Rails of 11 and 9 about the centre, and caps of 1 about the centreline's ends at (0, 10) and (10, 0).
        CHECK_THAT(dist(ents[0].center, Vec2d(0, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[0].radius, WithinAbs(11.0, 1e-12));
        CHECK_THAT(dist(ents[1].center, Vec2d(0, 10)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[1].radius, WithinAbs(1.0, 1e-12));
        CHECK_THAT(dist(ents[2].center, Vec2d(0, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[2].radius, WithinAbs(9.0, 1e-12));
        CHECK_THAT(dist(ents[3].center, Vec2d(10, 0)), WithinAbs(0.0, 1e-12));
        CHECK_THAT(ents[3].radius, WithinAbs(1.0, 1e-12));
        // A quarter of the annulus between the rails, and the two half discs of the caps.
        CHECK_THAT(std::abs(only_loop_area(tool)), WithinAbs(PI / 4.0 * (11.0 * 11.0 - 9.0 * 9.0) + PI, 1e-9));
    }
    SECTION("a type takes its own fields")
    {
        CHECK_THAT(refused({{"type", "circle_3_point"}, {"p0", {5, 0}}, {"p1", {0, 5}}}), ContainsSubstring("p2"));
        CHECK_THAT(refused({{"type", "circle_center"}, {"center", {0, 0}}, {"radius", 1}, {"p2", {1, 1}}}),
                   ContainsSubstring("circle_center takes no p2"));
        CHECK_THAT(refused({{"entities", {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {1, 0}}}, {{"type", "point"}, {"p", {2, 2}}, {"radius", 1}}}}}),
                   ContainsSubstring("point takes no radius"));
        const std::string unknown = refused({{"type", "gear"}});
        for (const char* t : {"line", "polyline", "rectangle_2_point", "rectangle_center", "rectangle_3_point", "rectangle_rounded",
                              "circle_center", "circle_2_point", "circle_3_point", "arc_3_point", "arc_center", "arc_tangent",
                              "slot", "slot_arc", "ellipse", "ellipse_arc", "spline", "polygon", "point"}) {
            INFO(t);
            CHECK_THAT(unknown, ContainsSubstring(t));
        }
        // Shapes their tools could not build.
        CHECK_THAT(refused({{"type", "circle_3_point"}, {"p0", {0, 0}}, {"p1", {5, 0}}, {"p2", {10, 0}}}), ContainsSubstring("one line"));
        CHECK_THAT(refused({{"type", "rectangle_rounded"}, {"p0", {0, 0}}, {"p1", {20, 10}}, {"radius", 5}}),
                   ContainsSubstring("less than half"));
        CHECK_THAT(refused({{"type", "slot_arc"}, {"center", {0, 0}}, {"start", {10, 0}}, {"end", {5, 0}}, {"width", 2}}),
                   ContainsSubstring("another direction"));
        CHECK_THAT(refused({{"type", "polyline"}, {"points", {{0, 0}, {10, 0}, {0, 0}}}}), ContainsSubstring("three points"));
        CHECK(ents.empty());
    }
    SECTION("the nineteen types, and a list of them")
    {
        const GUI::CommandCatalog catalog = cad_catalog();
        CHECK(property_of(command_of(catalog, "sketch_draw"), "type").at("enum") ==
              json::array({"line", "polyline", "rectangle_2_point", "rectangle_center", "rectangle_3_point", "rectangle_rounded",
                           "circle_center", "circle_2_point", "circle_3_point", "arc_3_point", "arc_center", "arc_tangent", "slot",
                           "slot_arc", "ellipse", "ellipse_arc", "spline", "polygon", "point"}));
        // A tangent arc in a list leaves the shape before it.
        const json r = draw({{"entities",
                              {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 0}}},
                               {{"type", "arc_tangent"}, {"end", {15, 5}}},
                               {{"type", "rectangle_center"}, {"center", {40, 0}}, {"corner", {45, 3}}},
                               {{"type", "circle_3_point"}, {"p0", {65, 0}}, {"p1", {60, 5}}, {"p2", {55, 0}}},
                               {{"type", "polyline"}, {"points", {{0, 20}, {10, 20}, {10, 30}}}},
                               {{"type", "slot_arc"}, {"center", {40, 20}}, {"start", {50, 20}}, {"end", {40, 30}}, {"width", 2}},
                               {{"type", "point"}, {"p", {70, 20}}}}}});
        CHECK(r.at("shapes") == json::parse(R"([{"first":0,"count":1},{"first":1,"count":1},{"first":2,"count":4},
                                                 {"first":6,"count":1},{"first":7,"count":2},{"first":9,"count":4},
                                                 {"first":13,"count":1}])"));
        CHECK_THAT(dist(ents[1].p0, Vec2d(10, 0)), WithinAbs(0.0, 1e-12));
        CHECK(ents[13].type == ET::Point);
    }
    SECTION("a grouped shape comes off whole with one undo")
    {
        draw({{"type", "line"}, {"p0", {0, -20}}, {"p1", {10, -20}}});
        for (const json& shape : {json{{"type", "rectangle_center"}, {"center", {0, 0}}, {"corner", {6, 4}}},
                                  json{{"type", "rectangle_3_point"}, {"p0", {0, 0}}, {"p1", {10, 5}}, {"width", 4}},
                                  json{{"type", "rectangle_rounded"}, {"p0", {0, 0}}, {"p1", {20, 10}}, {"radius", 2}},
                                  json{{"type", "slot_arc"}, {"center", {0, 0}}, {"start", {10, 0}}, {"end", {0, 10}}, {"width", 2}}}) {
            INFO(shape.dump());
            draw(shape);
            CHECK(ents.size() > 1);
            REQUIRE(tool.undo_last_entity());
            CHECK(ents.size() == 1);
        }
        // A polyline is its lines, as its tool draws it: an undo takes its last line off.
        draw({{"type", "polyline"}, {"points", {{0, 0}, {10, 0}, {10, 5}, {0, 5}}}});
        REQUIRE(ents.size() == 4);
        REQUIRE(tool.undo_last_entity());
        CHECK(ents.size() == 3);
    }
}

TEST_CASE("a rectangle_3_point and an arc_tangent at a slant carry the constraints their tools give them", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    const auto count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    const auto& ents = tool.entities();

    // Sides off the axes: four lines, 16 degrees of freedom, less 2 for each of four corners and 1 for each of three
    // right angles, the fourth being implied, as the Oblique rectangle tool infers them. Its corners stay where the call
    // put them.
    json r = sketch(tool, "sketch_draw", {{"type", "rectangle_3_point"}, {"p0", {0, 0}}, {"p1", {10, 3}}, {"width", 4}});
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::Perpendicular) == 3);
    CHECK(tool.constraints().size() == 7);
    CHECK(r.at("dof") == 5);
    const Vec2d u = Vec2d(10, 3).normalized(), side = 4.0 * Vec2d(-u.y(), u.x());
    const std::vector<Vec2d> corners = {Vec2d(0, 0), Vec2d(10, 3), Vec2d(10, 3) + side, side};
    REQUIRE(ents.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        INFO("corner " << i);
        CHECK_THAT((ents[i].p0 - corners[i]).norm(), WithinAbs(0.0, 1e-9));
        CHECK_THAT((ents[i].p1 - corners[(i + 1) % 4]).norm(), WithinAbs(0.0, 1e-9));
    }
    // On the axes, the right angles would add nothing to the Horizontal and Vertical the sides carry: 4 degrees of
    // freedom left, and the sketch solves.
    tool.cancel();
    begin_sketch(tool);
    r = sketch(tool, "sketch_draw", {{"type", "rectangle_3_point"}, {"p0", {0, 0}}, {"p1", {10, 0}}, {"width", 4}});
    CHECK(r.at("dof") == 4);
    CHECK(tool.solve_ok());

    // A tangent arc leaving a slanted line is bound Coincident and Tangent to it, as the Tangent arc tool binds it: a
    // line's 4 and an arc's 5 degrees of freedom, less 2 and 1. Its ends stay where the line and the call put them.
    tool.cancel();
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 3}}});
    r = sketch(tool, "sketch_draw", {{"type", "arc_tangent"}, {"end", {15, 9}}});
    CHECK(count(T::Coincident) == 1);
    CHECK(count(T::Tangent) == 1);
    CHECK(r.at("dof") == 6);
    REQUIRE(ents.size() == 2);
    CHECK_THAT((ents[1].p0 - Vec2d(10, 3)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[1].p1 - Vec2d(15, 9)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[0].p1 - Vec2d(10, 3)).norm(), WithinAbs(0.0, 1e-9));
    // Leaving an arc it is bound Coincident only, as the tool's is: the tool infers tangency between a line and a curve.
    tool.cancel();
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"type", "arc_3_point"}, {"p0", {0, 0}}, {"p1", {10, 0}}, {"on_arc", {5, 3}}});
    sketch(tool, "sketch_draw", {{"type", "arc_tangent"}, {"end", {20, 5}}});
    CHECK(count(T::Coincident) == 1);
    CHECK(count(T::Tangent) == 0);
}

TEST_CASE("a slot and a four-sided polygon at a slant carry the constraints their tools give them", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    const double          deg = GENERATE(17.0, 33.0, 61.0, 128.0, 200.0, 341.0);
    const double          a   = deg * M_PI / 180.0;
    GUI::DesignSketchTool tool;
    const auto            count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    const auto& ents = tool.entities();
    INFO("slant " << deg << " degrees");

    // A slot: two sides and two ends, 18 degrees of freedom, less 2 for each of four joints, 1 for each tangency the
    // Slot tool infers (the first end to both sides, the second end to the first side) and 1 for the ends' equal radius.
    // Its sides and ends stay where the call put them.
    begin_sketch(tool);
    const Vec2d u(std::cos(a), std::sin(a)), n(-u.y(), u.x()), c1 = 10.0 * u;
    json        r = sketch(tool, "sketch_draw", {{"type", "slot"}, {"c0", {0, 0}}, {"c1", {c1.x(), c1.y()}}, {"width", 4}});
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::Tangent) == 3);
    CHECK(count(T::EqualRadius) == 1);
    CHECK(tool.constraints().size() == 8);
    CHECK(r.at("dof") == 6);
    REQUIRE(ents.size() == 4);
    CHECK_THAT((ents[0].p0 - 2.0 * n).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[0].p1 - (c1 + 2.0 * n)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[2].p0 - (c1 - 2.0 * n)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[2].p1 + 2.0 * n).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[1].center - c1).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(ents[3].center.norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(ents[1].radius, WithinAbs(2.0, 1e-9));
    CHECK_THAT(ents[3].radius, WithinAbs(2.0, 1e-9));

    // A four-sided polygon, to a vertex or across its flats: as the rectangle_3_point, four lines and four joints, less
    // the three right angles the Polygon tool infers, the fourth being implied. Its vertices stay where the call put them.
    for (const bool flats : {false, true}) {
        INFO((flats ? "across_flats 12" : "radius 10"));
        tool.cancel();
        begin_sketch(tool);
        json args = {{"type", "polygon"}, {"center", {0, 0}}, {"sides", 4}, {"angle_deg", deg}};
        args[flats ? "across_flats" : "radius"] = flats ? 12 : 10;
        r                                       = sketch(tool, "sketch_draw", args);
        CHECK(count(T::Coincident) == 4);
        CHECK(count(T::Perpendicular) == 3);
        CHECK(tool.constraints().size() == 7);
        CHECK(r.at("dof") == 5);
        REQUIRE(ents.size() == 4);
        const double R = flats ? 6.0 / std::cos(M_PI / 4.0) : 10.0, a0 = flats ? a - M_PI / 4.0 : a;
        for (int k = 0; k < 4; ++k) {
            INFO("vertex " << k);
            CHECK_THAT((ents[k].p0 - R * Vec2d(std::cos(a0 + k * M_PI / 2.0), std::sin(a0 + k * M_PI / 2.0))).norm(), WithinAbs(0.0, 1e-9));
        }
    }
}

TEST_CASE("a rounded rectangle and an arc slot carry the constraints their tools give them", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    const auto [x0, y0, x1, y1, radius] = GENERATE(table<double, double, double, double, double>(
        {{0.3, -1.7, 29.1, 17.9, 3.3}, {-12.6, 4.2, 7.9, 31.5, 6.1}, {5.5, 5.5, 18.25, 9.75, 1.9}, {-30.1, -22.7, -3.3, -8.9, 2.45},
         {1.0 / 3.0, 2.0 / 7.0, 40.0 / 3.0, 50.0 / 7.0, 1.1}, {-7.77, 0.91, 23.4, 26.2, 9.4}}));
    INFO("rounded rectangle " << x0 << ", " << y0 << " to " << x1 << ", " << y1 << ", radius " << radius);
    GUI::DesignSketchTool tool;
    const auto            count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    const auto& ents = tool.entities();

    // Four sides and four corner arcs, 36 degrees of freedom, less 2 for each of eight joints, 1 for each side's
    // Horizontal or Vertical, 1 for each tangency the Rounded rectangle tool infers (every corner to the side before it,
    // and the last to the first side) and 1 for each corner's radius equal to the first's. Its sides stay where the
    // call put them.
    begin_sketch(tool);
    json r = sketch(tool, "sketch_draw", {{"type", "rectangle_rounded"}, {"p0", {x0, y0}}, {"p1", {x1, y1}}, {"radius", radius}});
    CHECK(count(T::Coincident) == 8);
    CHECK(count(T::Horizontal) == 2);
    CHECK(count(T::Vertical) == 2);
    CHECK(count(T::Tangent) == 7);
    CHECK(count(T::EqualRadius) == 3);
    CHECK(tool.constraints().size() == 22);
    CHECK(r.at("dof") == 6);
    REQUIRE(ents.size() == 8);
    CHECK_THAT((ents[0].p0 - Vec2d(x0 + radius, y0)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[2].p0 - Vec2d(x1, y0 + radius)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[4].p0 - Vec2d(x1 - radius, y1)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[6].p0 - Vec2d(x0, y1 - radius)).norm(), WithinAbs(0.0, 1e-9));
    for (int k = 1; k < 8; k += 2) CHECK_THAT(ents[k].radius, WithinAbs(radius, 1e-9));

    // An arc slot: four arcs, 20 degrees of freedom, less 2 for each of four joints and 1 for its ends' equal radius, as
    // the Arc slot tool infers it. Its arcs stay where the call put them.
    tool.cancel();
    begin_sketch(tool);
    const Vec2d c(x0, y0), s(x1, y1);
    const double rc = (s - c).norm(), w = std::min(2.0 * radius, rc);
    const Vec2d d = s - c, e = c + Vec2d(std::cos(1.3) * d.x() - std::sin(1.3) * d.y(), std::sin(1.3) * d.x() + std::cos(1.3) * d.y());
    r             = sketch(tool, "sketch_draw", {{"type", "slot_arc"}, {"center", {c.x(), c.y()}}, {"start", {s.x(), s.y()}}, {"end", {e.x(), e.y()}}, {"width", w}});
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::EqualRadius) == 1);
    CHECK(tool.constraints().size() == 5);
    CHECK(r.at("dof") == 11);
    REQUIRE(ents.size() == 4);
    CHECK_THAT((ents[0].center - c).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(ents[0].radius, WithinAbs(rc + w / 2.0, 1e-9));
    CHECK_THAT((ents[3].center - s).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT((ents[1].center - e).norm(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(ents[1].radius, WithinAbs(w / 2.0, 1e-9));
    CHECK_THAT(ents[3].radius, WithinAbs(w / 2.0, 1e-9));
}

TEST_CASE("a slot and a square on the axes have the degrees of freedom their tools leave", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    const auto            count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    // A slot's sides carry Horizontal or Vertical, and the Slot tool's slot there carries no tangency or equal radius
    // beside them, as its inference adds those as one batch, which the solver refuses: 8 degrees of freedom.
    for (const bool vertical : {false, true}) {
        INFO((vertical ? "vertical" : "horizontal"));
        tool.cancel();
        begin_sketch(tool);
        const json c1 = vertical ? json{0, 20} : json{20, 0};
        const json r  = sketch(tool, "sketch_draw", {{"type", "slot"}, {"c0", {0, 0}}, {"c1", c1}, {"width", 6}});
        CHECK(count(T::Coincident) == 4);
        CHECK(count(vertical ? T::Vertical : T::Horizontal) == 2);
        CHECK(tool.constraints().size() == 6);
        CHECK(r.at("dof") == 8);
        CHECK(tool.solve_ok());
    }
    // A square's sides lie a rounding step off the axes, so exact inference levels only the sides that lie exactly on
    // one, and right angles hold the others, as the Polygon tool's four level and upright sides hold all of them: 4
    // degrees of freedom left, and its vertices where the call put them.
    for (const auto& [flats, deg] : {std::pair{true, 0.0}, std::pair{false, 45.0}, std::pair{true, 90.0}}) {
        INFO((flats ? "across_flats 20, angle " : "radius 10, angle ") << deg);
        tool.cancel();
        begin_sketch(tool);
        json args = {{"type", "polygon"}, {"center", {0, 0}}, {"sides", 4}, {"angle_deg", deg}};
        args[flats ? "across_flats" : "radius"] = flats ? 20 : 10;
        const json r = sketch(tool, "sketch_draw", args);
        CHECK(tool.constraints().size() == 8);
        CHECK(r.at("dof") == 4);
        CHECK(tool.solve_ok());
        const double R = flats ? 10.0 / std::cos(M_PI / 4.0) : 10.0, a0 = deg * M_PI / 180.0 - (flats ? M_PI / 4.0 : 0.0);
        REQUIRE(tool.entities().size() == 4);
        for (int k = 0; k < 4; ++k)
            CHECK_THAT((tool.entities()[k].p0 - R * Vec2d(std::cos(a0 + k * M_PI / 2.0), std::sin(a0 + k * M_PI / 2.0))).norm(), WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("sketch_draw refuses a shape's field beside its list of shapes", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    const json line = {{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 0}}};
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", {{"entities", json::array({line})}, {"center", {5, 5}}}); }),
               ContainsSubstring("center belongs in an item of entities"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", {{"entities", json::array({line})}, {"type", "circle_center"}}); }),
               ContainsSubstring("type belongs in an item of entities"));
    CHECK(tool.entities().empty());
    // construction is no shape's field: beside the list it is each item's that gives none of its own.
    json kept            = line;
    kept["p0"]           = {0, 5};
    kept["p1"]           = {10, 5};
    kept["construction"] = false;
    sketch(tool, "sketch_draw", {{"entities", json::array({line, kept})}, {"construction", true}});
    REQUIRE(tool.entities().size() == 2);
    CHECK(tool.entities()[0].construction);
    CHECK_FALSE(tool.entities()[1].construction);
}

TEST_CASE("sketch_draw takes an entities given as its list, an empty one included", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 0}}});
    const std::vector<SketchEntity>              ents_before = tool.entities();
    const std::vector<SketchEntityConstraintDef> cons_before = tool.constraints();
    const auto unchanged = [&] {
        CHECK(tool.entities().size() == ents_before.size());
        CHECK(tool.constraints().size() == cons_before.size());
    };
    // A shape's field beside an empty list is refused as beside one with items, and an empty list alone says so.
    const json line = {{"p0", {0, 5}}, {"p1", {10, 5}}};
    json       beside = line;
    beside["entities"] = json::array();
    beside["type"]     = "line";
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", beside); }), ContainsSubstring("belongs in an item of entities"));
    unchanged();
    json fields         = line;
    fields["entities"]  = json::array();
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", fields); }), ContainsSubstring("belongs in an item of entities"));
    unchanged();
    CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", {{"entities", json::array()}}); }), ContainsSubstring("entities is empty"));
    unchanged();
    // A null entities is not given: the shape beside it is drawn.
    json one        = line;
    one["entities"] = nullptr;
    one["type"]     = "line";
    sketch(tool, "sketch_draw", one);
    CHECK(tool.entities().size() == ents_before.size() + 1);
}

TEST_CASE("an arc_tangent leaves an entity an earlier item of the same list adds", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    const auto&           ents  = tool.entities();
    const auto            bound = [&](T type, int a, int b) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(), [&](const SketchEntityConstraintDef& c) {
            return c.type == type && ((c.ea == a && c.eb == b) || (c.ea == b && c.eb == a));
        });
    };
    // A line and a tangent arc naming it, in one list, on an empty sketch and on one with two entities already: the arc
    // starts at the line's end, bound to it there and tangent to it.
    for (const int already : {0, 2}) {
        INFO(already << " entities before");
        tool.cancel();
        begin_sketch(tool);
        if (already > 0)
            sketch(tool, "sketch_draw", {{"entities", {{{"type", "circle_center"}, {"center", {-20, 0}}, {"radius", 3}},
                                                       {{"type", "line"}, {"p0", {-20, 10}}, {"p1", {-10, 12}}}}}});
        REQUIRE(int(ents.size()) == already);
        sketch(tool, "sketch_draw", {{"entities", {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 3}}},
                                                   {{"type", "arc_tangent"}, {"entity", already}, {"end", {15, 9}}}}}});
        REQUIRE(int(ents.size()) == already + 2);
        CHECK(ents[already + 1].type == SketchEntity::Type::Arc);
        CHECK_THAT((ents[already + 1].p0 - Vec2d(10, 3)).norm(), WithinAbs(0.0, 1e-9));
        CHECK_THAT((ents[already + 1].p1 - Vec2d(15, 9)).norm(), WithinAbs(0.0, 1e-9));
        CHECK(bound(T::Coincident, already, already + 1) == 1);
        CHECK(bound(T::Tangent, already, already + 1) == 1);
    }

    // An index past what the sketch and the items before it hold, and a circle an item before it adds, are refused,
    // and the sketch is as it was.
    const size_t before = ents.size();
    CHECK_THAT(failure([&] {
                   sketch(tool, "sketch_draw", {{"entities", {{{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 23}}},
                                                              {{"type", "arc_tangent"}, {"entity", int(before) + 1}, {"end", {15, 29}}}}}});
               }),
               ContainsSubstring("entity " + std::to_string(before + 1) + " does not exist"));
    CHECK(ents.size() == before);
    CHECK_THAT(failure([&] {
                   sketch(tool, "sketch_draw", {{"entities", {{{"type", "circle_center"}, {"center", {30, 0}}, {"radius", 3}},
                                                              {{"type", "arc_tangent"}, {"entity", int(before)}, {"end", {40, 5}}}}}});
               }),
               ContainsSubstring("circle " + std::to_string(before) + " is neither"));
    CHECK(ents.size() == before);
}

TEST_CASE("a refused sketch_draw leaves the sketch as it was", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    // A corner rectangle, one shape, already in the sketch, and a line after it.
    sketch(tool, "sketch_draw", {{"type", "rectangle_2_point"}, {"p0", {-15, -10}}, {"p1", {15, 10}}});
    sketch(tool, "sketch_draw", {{"type", "line"}, {"p0", {20, 0}}, {"p1", {30, 0}}});
    const std::vector<SketchEntity>              ents_before = tool.entities();
    const std::vector<SketchEntityConstraintDef> cons_before = tool.constraints();
    const int                                    dof_before  = tool.dof();
    const bool                                   ok_before   = tool.solve_ok();
    REQUIRE(ents_before.size() == 5);

    // A tangent arc whose end lies on the line it leaves makes no arc: refused once the items before it are drawn,
    // in a list, and on its own after the line in the sketch.
    const std::vector<json> refused_calls = {
        {{"entities", {{{"type", "line"}, {"p0", {0, 30}}, {"p1", {10, 30}}}, {{"type", "arc_tangent"}, {"end", {20, 30}}}}}},
        {{"entities",
          {{{"type", "rectangle_center"}, {"center", {40, 0}}, {"corner", {45, 3}}}, {{"type", "arc_tangent"}, {"end", {35, -8}}}}}},
        {{"type", "arc_tangent"}, {"end", {40, 0}}},
    };
    for (const json& call : refused_calls) {
        INFO(call.dump());
        CHECK_THAT(failure([&] { sketch(tool, "sketch_draw", call); }), ContainsSubstring("makes no arc"));
        // Its entities, constraints and solve are the sketch's before the call.
        const auto& ents = tool.entities();
        REQUIRE(ents.size() == ents_before.size());
        for (size_t i = 0; i < ents.size(); ++i) {
            INFO("entity " << i);
            CHECK(ents[i].type == ents_before[i].type);
            CHECK_THAT((ents[i].p0 - ents_before[i].p0).norm(), WithinAbs(0.0, 0.0));
            CHECK_THAT((ents[i].p1 - ents_before[i].p1).norm(), WithinAbs(0.0, 0.0));
        }
        const auto& cons = tool.constraints();
        REQUIRE(cons.size() == cons_before.size());
        for (size_t i = 0; i < cons.size(); ++i) {
            INFO("constraint " << i);
            CHECK(cons[i].type == cons_before[i].type);
            CHECK(cons[i].ea == cons_before[i].ea);
            CHECK(cons[i].eb == cons_before[i].eb);
        }
        CHECK(tool.dof() == dof_before);
        CHECK(tool.solve_ok() == ok_before);
    }
    // One undo of the sketch takes off what it took off before: the line, then the rectangle whole.
    REQUIRE(tool.undo_last_entity());
    CHECK(tool.entities().size() == 4);
    REQUIRE(tool.undo_last_entity());
    CHECK(tool.entities().empty());
}

TEST_CASE("sketch_fillet rounds an inside corner with an arc bound tangent to both legs", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"entities", l_bracket()}});
    CHECK_THAT(only_loop_area(tool), WithinAbs(264.0, 1e-9));

    const json r = sketch(tool, "sketch_fillet", {{"entities", {2, 3}}, {"radius", 3}});
    CHECK(r.at("entity") == 6);
    CHECK(r.at("lines") == json::array({2, 3}));
    CHECK(r.at("released_constraints") == 1);   // the sharp corner's coincidence
    const auto& ents = tool.entities();
    REQUIRE(ents.size() == 7);
    CHECK(ents[6].type == SketchEntity::Type::Arc);
    CHECK_THAT(ents[6].radius, WithinAbs(3.0, 1e-9));
    CHECK_THAT((ents[6].center - Vec2d(7, 7)).norm(), WithinAbs(0.0, 1e-9));
    // The arc's ends sit on the legs and the loop stays closed, now 9 (1 - pi/4) larger.
    CHECK_THAT(only_loop_area(tool), WithinAbs(264.0 + 9.0 * (1.0 - PI / 4.0), 1e-6));
    int coincident = 0, tangent = 0;
    for (const SketchEntityConstraintDef& c : tool.constraints())
        if (c.ea == 6 || c.eb == 6) {
            coincident += c.type == T::Coincident;
            tangent += c.type == T::Tangent;
        }
    CHECK(coincident == 2);
    CHECK(tangent == 2);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Select);

    // A chamfer on the outer corner between lines 0 and 1 adds a line and keeps the loop closed,
    // with the person's armed tool still armed afterwards.
    tool.set_tool(GUI::DesignSketchTool::Mode::Line);
    const json c = sketch(tool, "sketch_chamfer", {{"entities", {0, 1}}, {"distance", 2}});
    CHECK(c.at("entity") == 7);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Line);
    CHECK_FALSE(tool.value_field_open());
    CHECK(tool.entities()[7].type == SketchEntity::Type::Line);
    CHECK_THAT(only_loop_area(tool), WithinAbs(264.0 + 9.0 * (1.0 - PI / 4.0) - 2.0, 1e-6));

    // Refusals change nothing.
    const size_t n = tool.entities().size(), nc = tool.constraints().size();
    auto refused = [&](const char* m, const json& params) { return failure([&] { sketch(tool, m, params); }); };
    CHECK_THAT(refused("sketch_fillet", {{"entities", {4, 5}}, {"radius", 50}}), ContainsSubstring("overruns a leg"));
    CHECK_THAT(refused("sketch_fillet", {{"entities", {0, 2}}, {"radius", 1}}), ContainsSubstring("parallel"));
    CHECK_THAT(refused("sketch_fillet", {{"entities", {6, 3}}, {"radius", 1}}), ContainsSubstring("arc 6 is not a line"));
    CHECK_THAT(refused("sketch_fillet", {{"entities", {4}}, {"radius", 1}}), ContainsSubstring("two lines"));
    CHECK_THAT(refused("sketch_fillet", {{"entities", {4, 5}}, {"radius", -1}}), ContainsSubstring("radius must be > 0"));
    CHECK_THAT(refused("sketch_fillet", {{"entities", {4, 9}}, {"radius", 1}}), ContainsSubstring("entity 9 does not exist"));
    CHECK_THAT(refused("sketch_chamfer", {{"entities", {4, 5}}}), ContainsSubstring("distance must be a number"));
    CHECK(tool.entities().size() == n);
    CHECK(tool.constraints().size() == nc);

    // The selection stands in for entities, as the tool's picks would.
    tool.select_indices({4, 5});
    CHECK(sketch(tool, "sketch_fillet", {{"radius", 1}}).at("entity") == int(n));
}

TEST_CASE("sketch_trim cuts a line back to a crossing and sketch_extend runs one out to the next entity", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"entities",
                                 {{{"type", "line"}, {"p0", {-10, 0}}, {"p1", {10, 0}}},
                                  {{"type", "line"}, {"p0", {0, -10}}, {"p1", {0, 10}}},
                                  {{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}},
                                  {{"type", "line"}, {"p0", {30, 15}}, {"p1", {30, 25}}},
                                  {{"type", "circle_center"}, {"center", {60, 0}}, {"radius", 3}},
                                  {{"type", "line"}, {"p0", {100, -50}}, {"p1", {110, -50}}}}}});
    const auto& ents = tool.entities();

    json r = sketch(tool, "sketch_trim", {{"at", {5, 0}}});
    CHECK(r.at("entity") == 0);
    CHECK(r.at("geometry").at("type") == "line");
    CHECK(ents[0].p0 == Vec2d(-10, 0));
    CHECK(ents[0].p1.norm() < 1e-9);
    CHECK_FALSE(r.contains("extend"));

    r = sketch(tool, "sketch_extend", {{"at", {9, 20}}});
    CHECK(r.at("entity") == 2);
    CHECK_THAT((ents[2].p1 - Vec2d(30, 20)).norm(), WithinAbs(0.0, 1e-9));
    CHECK_FALSE(r.contains("extend"));

    const std::vector<SketchEntity> before = ents;
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_trim", params); }); };
    CHECK_THAT(refused({{"at", {100, 100}}}), ContainsSubstring("no sketch entity within 0.05 mm"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_extend", {{"at", {63, 0}}}); }),
               ContainsSubstring("extend works on lines and arcs; circle 4"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_extend", {{"at", {100, 100}}}); }), ContainsSubstring("no sketch entity within 0.05 mm"));
    CHECK_THAT(refused({{"at", {63, 0}}}), ContainsSubstring("nothing to cut on circle 4"));
    CHECK_THAT(refused({{"at", {105, -50}}}), ContainsSubstring("nothing to cut on line 5"));
    CHECK_THAT(refused({{"at", {5, 0}}, {"tolerance", 0}}), ContainsSubstring("tolerance must be > 0"));
    CHECK_THAT(refused({{"at", 5}}), ContainsSubstring("at must be [x, y]"));
    for (size_t i = 0; i < before.size(); ++i) CHECK((ents[i].p0 == before[i].p0 && ents[i].p1 == before[i].p1));

    // A wider tolerance reaches an entity a computed point misses: line 1 loses its part below line 0.
    CHECK_THAT(refused({{"at", {0.3, -5}}}), ContainsSubstring("no sketch entity"));
    r = sketch(tool, "sketch_trim", {{"at", {0.3, -5}}, {"tolerance", 0.5}});
    CHECK(r.at("entity") == 1);
    CHECK(ents[1].p0.norm() < 1e-9);
}

TEST_CASE("sketch_transform moves, rotates and scales, and sketch_pattern repeats, as the transform tools do", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"entities",
                                 {{{"type", "line"}, {"p0", {8, 0}}, {"p1", {12, 0}}},
                                  {{"type", "line"}, {"p0", {1, 0}}, {"p1", {2, 0}}},
                                  {{"type", "circle_center"}, {"center", {30, 0}}, {"radius", 2}}}}});
    const auto& ents = tool.entities();

    // Six around the origin: five copies every 60 degrees, each bound equal in length.
    json r = sketch(tool, "sketch_pattern", {{"type", "circular"}, {"entities", {0}}, {"count", 6}, {"pivot", {0, 0}}});
    CHECK(r.at("type") == "circular");
    CHECK(r.at("first_index") == 3);
    CHECK(r.at("added") == 5);
    CHECK(r.at("bound") == 5);
    CHECK(r.at("pivot") == json::array({0.0, 0.0}));
    for (int k = 1; k <= 5; ++k) {
        const SketchEntity& e = ents[2 + k];
        CHECK_THAT((e.p0 - 8.0 * Vec2d(std::cos(k * PI / 3), std::sin(k * PI / 3))).norm(), WithinAbs(0.0, 1e-6));
        CHECK(tool.constraints()[tool.constraints().size() - 6 + k].type == T::EqualLength);
    }

    // A linear pattern of the circle: two copies 5 apart, each with the source's radius.
    r = sketch(tool, "sketch_pattern", {{"type", "linear"}, {"entities", {2}}, {"dy", 5}, {"count", 3}});
    CHECK(r.at("added") == 2);
    CHECK((ents[8].center - Vec2d(30, 5)).norm() < 1e-9);
    CHECK((ents[9].center - Vec2d(30, 10)).norm() < 1e-9);
    CHECK_FALSE(r.contains("pivot"));

    // Rotate a quarter turn about the origin, then scale twice about it, then move.
    r = sketch(tool, "sketch_transform", {{"type", "rotate"}, {"entities", {1}}, {"angle_deg", 90}, {"pivot", {0, 0}}});
    CHECK(r.at("type") == "rotate");
    CHECK(r.at("moved") == json::array({1}));
    CHECK((ents[1].p0 - Vec2d(0, 1)).norm() < 1e-9);
    CHECK((ents[1].p1 - Vec2d(0, 2)).norm() < 1e-9);
    sketch(tool, "sketch_transform", {{"type", "scale"}, {"entities", {1}}, {"scale", 2}, {"pivot", {0, 0}}});
    CHECK((ents[1].p1 - Vec2d(0, 4)).norm() < 1e-9);
    sketch(tool, "sketch_transform", {{"type", "move"}, {"entities", {1}}, {"dx", 3}});
    CHECK((ents[1].p1 - Vec2d(3, 4)).norm() < 1e-9);
    // Without a pivot, rotate turns about the entities' centroid, the gizmo's pivot.
    r = sketch(tool, "sketch_transform", {{"type", "rotate"}, {"entities", {1}}, {"angle", PI}});
    CHECK_THAT(r.at("pivot")[0].get<double>(), WithinAbs(3.0, 1e-9));
    CHECK_THAT(r.at("pivot")[1].get<double>(), WithinAbs(3.0, 1e-9));
    CHECK((ents[1].p1 - Vec2d(3, 2)).norm() < 1e-9);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Select);

    const size_t n = ents.size();
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_transform", params); }); };
    auto unrepeated = [&](const json& params) { return failure([&] { sketch(tool, "sketch_pattern", params); }); };
    CHECK_THAT(refused({{"type", "spin"}, {"entities", {1}}}), ContainsSubstring("type must be move, rotate or scale"));
    CHECK_THAT(refused({{"type", "linear"}, {"entities", {1}}, {"dx", 1}}), ContainsSubstring("type must be move, rotate or scale"));
    CHECK_THAT(refused({{"type", "move"}, {"entities", {1}}, {"dx", 1}, {"angle", 1}}), ContainsSubstring("move takes no angle"));
    CHECK_THAT(refused({{"type", "move"}, {"entities", {1}}, {"dx", 1}, {"count", 3}}), ContainsSubstring("move takes no count"));
    CHECK_THAT(refused({{"type", "move"}, {"entities", {1}}}), ContainsSubstring("non-zero dx or dy"));
    CHECK_THAT(refused({{"type", "rotate"}, {"entities", {1}}}), ContainsSubstring("non-zero angle"));
    CHECK_THAT(refused({{"type", "scale"}, {"entities", {1}}, {"scale", 1}}), ContainsSubstring("other than 1"));
    CHECK_THAT(refused({{"type", "move"}, {"dx", 1}}), ContainsSubstring("nothing selected"));
    CHECK_THAT(unrepeated({{"type", "move"}, {"entities", {1}}, {"dx", 1}, {"count", 2}}), ContainsSubstring("type must be linear or circular"));
    CHECK_THAT(unrepeated({{"type", "circular"}, {"entities", {1}}, {"count", 4}}), ContainsSubstring("needs pivot"));
    CHECK_THAT(unrepeated({{"type", "circular"}, {"entities", {1}}, {"count", 4}, {"pivot", {0, 0}}, {"dx", 1}}),
               ContainsSubstring("circular takes no dx"));
    CHECK_THAT(unrepeated({{"type", "linear"}, {"entities", {1}}, {"dx", 1}, {"count", 3}, {"pivot", {0, 0}}}),
               ContainsSubstring("linear takes no pivot"));
    CHECK_THAT(unrepeated({{"type", "linear"}, {"entities", {1}}, {"dx", 1}}), ContainsSubstring("2 to 1000"));
    CHECK_THAT(unrepeated({{"type", "linear"}, {"entities", {1}}, {"dx", 1}, {"count", 1}}), ContainsSubstring("2 to 1000"));
    CHECK_THAT(unrepeated({{"type", "linear"}, {"entities", {1, 1}}, {"dx", 1}, {"count", 2}}), ContainsSubstring("listed twice"));
    CHECK(ents.size() == n);
}

TEST_CASE("a transform keeps the constraints among the entities it moves together", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"type", "rectangle_2_point"}, {"p0", {-15, -10}}, {"p1", {15, 10}}});
    auto count = [&](T type) {
        return std::count_if(tool.constraints().begin(), tool.constraints().end(),
                             [type](const SketchEntityConstraintDef& c) { return c.type == type; });
    };
    REQUIRE(count(T::Coincident) == 4);   // the corners
    REQUIRE(count(T::Horizontal) == 2);
    REQUIRE(count(T::Vertical) == 2);
    const int  rect_dof = tool.dof();     // position, width and height
    const json whole    = {0, 1, 2, 3};

    // Moved or scaled whole, it is still the same rectangle: nothing is released.
    json r = sketch(tool, "sketch_transform", {{"type", "move"}, {"entities", whole}, {"dx", 5}, {"dy", 3}});
    CHECK(r.at("released_constraints") == 0);
    CHECK(r.at("dof") == rect_dof);
    r = sketch(tool, "sketch_transform", {{"type", "scale"}, {"entities", whole}, {"scale", 2}});
    CHECK(r.at("released_constraints") == 0);
    CHECK(r.at("dof") == rect_dof);

    // Turned 30 degrees, its edges are neither horizontal nor vertical, but its corners still meet.
    r = sketch(tool, "sketch_transform", {{"type", "rotate"}, {"entities", whole}, {"angle_deg", 30}});
    CHECK(r.at("released_constraints") == 4);
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::Horizontal) == 0);
    CHECK(count(T::Vertical) == 0);
    CHECK(r.at("dof") == 4 * 4 - 4 * 2);   // four lines' ends, less two per joined corner

    // A corner pinned to the origin stays behind when the whole rectangle moves: only the pin goes.
    sketch(tool, "sketch_constrain", {{"type", "coincident"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", "origin"}}}}});
    REQUIRE(count(T::Coincident) == 5);
    r = sketch(tool, "sketch_transform", {{"type", "move"}, {"entities", whole}, {"dx", 2}});
    CHECK(r.at("released_constraints") == 1);
    CHECK(count(T::Coincident) == 4);

    // One edge moved on its own leaves its neighbours: the two corners it shares are released.
    r = sketch(tool, "sketch_transform", {{"type", "move"}, {"entities", {0}}, {"dx", 1}});
    CHECK(r.at("released_constraints") == 2);
    CHECK(count(T::Coincident) == 2);
}

TEST_CASE("sketch_mirror about an axis binds the copies as the Mirror tool does", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    const auto off = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm(); };
    // `p` reflected across the vertical line x = `x`.
    const auto across = [](const Vec2d& p, double x) { return Vec2d(2.0 * x - p.x(), p.y()); };
    // The two Symmetric constraints the Mirror tool keeps for a line, one for each end: the line's end, the copy's end that
    // is its reflection (the copy runs the other way), the axis.
    const auto both_ends = [](const json& added, int line, int copy, const json& axis) {
        REQUIRE(added.size() == 2);
        for (int k = 0; k < 2; ++k) {
            const char* role = k == 0 ? "p0" : "p1";
            const char* its  = k == 0 ? "p1" : "p0";
            INFO(role);
            CHECK(added[k].at("type") == "symmetric");
            CHECK(added[k].at("refs") == json::array({{{"entity", line}, {"role", role}}, {{"entity", copy}, {"role", its}}, {{"entity", axis}}}));
        }
    };
    const json slanted = {{"type", "line"}, {"p0", {5, 0}}, {"p1", {10, 5}}};
    // Line 0 held 10 long: a distance between its ends, which the solver meets by moving them.
    const json ten_long = {{"type", "distance"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 0}, {"role", "p1"}}}}, {"value", 10}};

    // About the sketch's y axis.
    {
        GUI::DesignSketchTool tool;
        begin_sketch(tool);
        sketch(tool, "sketch_draw", slanted);
        const json  r    = sketch(tool, "sketch_mirror", {{"axis", "y_axis"}, {"entities", {0}}});
        const auto& ents = tool.entities();
        REQUIRE(ents.size() == 2);
        CHECK(r.at("first_index") == 1);
        CHECK(r.at("added") == 1);
        // (-5, 0) to (-10, 5), run the other way, as a mirrored chain continues head to tail.
        CHECK_THAT(off(ents[1].p1, Vec2d(-5, 0)), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[1].p0, Vec2d(-10, 5)), WithinAbs(0.0, 1e-9));
        both_ends(r.at("constraints_added"), 0, 1, "y_axis");
        for (const json& c : r.at("constraints_added"))
            CHECK(tool.constraints().at(c.at("index").get<std::size_t>()).type == T::Symmetric);
        // A move would let go of a Symmetric constraint that crosses what it moves; a length the solver meets by moving
        // the line keeps them, and the copy follows it.
        sketch(tool, "sketch_dimension", ten_long);
        CHECK_THAT(off(ents[0].p0, ents[0].p1), WithinAbs(10.0, 1e-6));
        CHECK_THAT(off(ents[1].p1, across(ents[0].p0, 0.0)), WithinAbs(0.0, 1e-6));
        CHECK_THAT(off(ents[1].p0, across(ents[0].p1, 0.0)), WithinAbs(0.0, 1e-6));
    }
    // About a vertical line of the sketch, x = 20.
    {
        GUI::DesignSketchTool tool;
        begin_sketch(tool);
        sketch(tool, "sketch_draw", {{"entities", {slanted, {{"type", "line"}, {"p0", {20, -10}}, {"p1", {20, 10}}}}}});
        const json  r    = sketch(tool, "sketch_mirror", {{"axis", 1}, {"entities", {0}}});
        const auto& ents = tool.entities();
        REQUIRE(ents.size() == 3);
        CHECK_THAT(off(ents[2].p1, Vec2d(35, 0)), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[2].p0, Vec2d(30, 5)), WithinAbs(0.0, 1e-9));
        both_ends(r.at("constraints_added"), 0, 2, 1);
        sketch(tool, "sketch_dimension", ten_long);
        CHECK_THAT(off(ents[0].p0, ents[0].p1), WithinAbs(10.0, 1e-6));
        CHECK_THAT(ents[1].p1.x() - ents[1].p0.x(), WithinAbs(0.0, 1e-9));   // the axis stays upright
        const double x = ents[1].p0.x();
        CHECK_THAT(off(ents[2].p1, across(ents[0].p0, x)), WithinAbs(0.0, 1e-6));
        CHECK_THAT(off(ents[2].p0, across(ents[0].p1, x)), WithinAbs(0.0, 1e-6));
    }
    // An arc's copy runs the other way too, and is bound by its two ends.
    {
        GUI::DesignSketchTool tool;
        begin_sketch(tool);
        sketch(tool, "sketch_draw", {{"type", "arc_3_point"}, {"p0", {10, 0}}, {"p1", {5, 5}},
                                     {"on_arc", {5 + 5 * std::cos(PI / 4), 5 * std::sin(PI / 4)}}});
        const json  r    = sketch(tool, "sketch_mirror", {{"axis", "y_axis"}, {"entities", {0}}});
        const auto& ents = tool.entities();
        REQUIRE(ents.size() == 2);
        CHECK(ents[1].type == SketchEntity::Type::Arc);
        both_ends(r.at("constraints_added"), 0, 1, "y_axis");
        CHECK_THAT(off(ents[1].p1, across(ents[0].p0, 0.0)), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[1].p0, across(ents[0].p1, 0.0)), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[1].center, Vec2d(-5, 0)), WithinAbs(0.0, 1e-9));
    }
    // What is refused adds nothing; two free points bind nothing.
    {
        GUI::DesignSketchTool tool;
        begin_sketch(tool);
        sketch(tool, "sketch_draw", {{"entities", {slanted, {{"type", "circle_center"}, {"center", {30, 0}}, {"radius", 3}}}}});
        const std::size_t n = tool.entities().size(), nc = tool.constraints().size();
        auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_mirror", params); }); };
        CHECK_THAT(refused({{"axis", 1}, {"entities", {0}}}), ContainsSubstring("circle 1 is not a line"));
        CHECK_THAT(refused({{"axis", "y_axis"}, {"axis_a", {0, 0}}, {"entities", {0}}}), ContainsSubstring("not both"));
        CHECK_THAT(refused({{"axis", "z_axis"}, {"entities", {0}}}), ContainsSubstring("x_axis"));
        CHECK_THAT(refused({{"axis", 0}, {"entities", {0}}}), ContainsSubstring("is the axis"));
        CHECK(tool.entities().size() == n);
        CHECK(tool.constraints().size() == nc);
        const json r = sketch(tool, "sketch_mirror", {{"axis_a", {0, 0}}, {"axis_b", {0, 1}}, {"entities", {0}}});
        CHECK(r.at("added") == 1);
        CHECK(r.at("constraints_added").empty());
        CHECK(tool.constraints().size() == nc);
    }
}

TEST_CASE("a fillet or chamfer of a mirrored source's corner gives the source what it gives with no mirror, and frees the copy there",
          "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    using R = SketchPointRole;
    const auto off = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm(); };
    // Two lines meeting at the corner (15, 0); mirrored about the y axis, their copies meet at (-15, 0).
    const json corner = {{"entities", {{{"type", "line"}, {"p0", {5, 0}}, {"p1", {15, 0}}},
                                       {{"type", "line"}, {"p0", {15, 0}}, {"p1", {15, 10}}}}}};
    // The corner rounded by radius 2, or cut by 2, and the degrees of freedom the sketch has after it with no mirror
    // (each line 3, held Horizontal or Vertical, and the arc 5 or the segment 4, less its ends' Coincident and the
    // arc's Tangent constraints).
    struct Corner { const char* command; json params; int dof; };
    const Corner ops[] = {{"sketch_fillet", {{"entities", {0, 1}}, {"radius", 2}}, 5},
                          {"sketch_chamfer", {{"entities", {0, 1}}, {"distance", 2}}, 6}};
    for (const Corner& op : ops) {
        INFO(op.command);
        GUI::DesignSketchTool without;
        begin_sketch(without);
        sketch(without, "sketch_draw", corner);
        sketch(without, op.command, op.params);
        CHECK(without.dof() == op.dof);
        GUI::DesignSketchTool with;
        begin_sketch(with);
        sketch(with, "sketch_draw", corner);
        const json m = sketch(with, "sketch_mirror", {{"axis", "y_axis"}, {"entities", {0, 1}}});
        REQUIRE(m.at("constraints_added").size() == 4);
        CHECK(m.at("dof") == 4);   // the source's 4; each copy is held by its source's two ends
        const auto&                     ents   = with.entities();
        const std::vector<SketchEntity> copies = {ents.at(2), ents.at(3)};
        const json r = sketch(with, op.command, op.params);
        CHECK(r.at("solve_ok") == true);
        // The two lines and the arc or segment between them (index 4 here, 2 with no mirror) are the unmirrored ones.
        REQUIRE(ents.size() == 5);
        const std::pair<int, int> source[] = {{0, 0}, {1, 1}, {4, 2}};
        for (const auto& [i, j] : source) {
            INFO("entity " << i);
            const SketchEntity &a = ents[i], &b = without.entities().at(j);
            CHECK(a.type == b.type);
            CHECK_THAT(off(a.p0, b.p0), WithinAbs(0.0, 1e-9));
            CHECK_THAT(off(a.p1, b.p1), WithinAbs(0.0, 1e-9));
            CHECK_THAT(off(a.center, b.center), WithinAbs(0.0, 1e-9));
            CHECK_THAT(a.radius, WithinAbs(b.radius, 1e-9));
        }
        if (std::string(op.command) == "sketch_fillet") CHECK_THAT(ents[4].radius, WithinAbs(2.0, 1e-9));
        // The copies stay where the mirror put them: their ends at the corner are let go of with the trimmed ends'
        // Symmetric constraints, and the far ends keep theirs.
        for (int k = 0; k < 2; ++k) {
            INFO("copy " << k + 2);
            CHECK_THAT(off(ents[2 + k].p0, copies[k].p0), WithinAbs(0.0, 1e-9));
            CHECK_THAT(off(ents[2 + k].p1, copies[k].p1), WithinAbs(0.0, 1e-9));
        }
        std::vector<std::pair<int, R>> held;   // the source end each Symmetric constraint left holds
        for (const SketchEntityConstraintDef& c : with.constraints())
            if (c.type == T::Symmetric) held.emplace_back(c.ea, c.ra);
        CHECK(held == std::vector<std::pair<int, R>>{{0, R::P0}, {1, R::P1}});
        CHECK(r.at("dof") == op.dof + 4);   // and each copy's freed end
    }
}

TEST_CASE("an extend of a mirrored source gives the source what it gives with no mirror, and frees the copy", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    const auto off = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm(); };
    // A line ending short of a crossing line at x = 10, both mirrored about the y axis, the line then extended to it.
    const json reaching = {{"entities", {{{"type", "line"}, {"p0", {5, 0}}, {"p1", {8, 0}}},
                                         {{"type", "line"}, {"p0", {10, -5}}, {"p1", {10, 5}}}}}};
    const json reach    = {{"at", {8, 0}}};
    GUI::DesignSketchTool without;
    begin_sketch(without);
    sketch(without, "sketch_draw", reaching);
    sketch(without, "sketch_extend", reach);
    GUI::DesignSketchTool with;
    begin_sketch(with);
    sketch(with, "sketch_draw", reaching);
    const json m = sketch(with, "sketch_mirror", {{"axis", "y_axis"}, {"entities", {0, 1}}});
    REQUIRE(m.at("constraints_added").size() == 4);
    CHECK(m.at("dof") == 6);   // each source line held Horizontal or Vertical, each copy held by its source
    const auto&                     ents   = with.entities();
    const std::vector<SketchEntity> copies = {ents.at(2), ents.at(3)};
    const json e = sketch(with, "sketch_extend", reach);
    CHECK(e.at("solve_ok") == true);
    // The source is what the same extend gives with no mirror: the line runs on to (10, 0).
    REQUIRE(ents.size() == 4);
    CHECK_THAT(off(ents[0].p1, Vec2d(10, 0)), WithinAbs(0.0, 1e-9));
    for (int i : {0, 1}) {
        INFO("entity " << i);
        CHECK_THAT(off(ents[i].p0, without.entities().at(i).p0), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[i].p1, without.entities().at(i).p1), WithinAbs(0.0, 1e-9));
    }
    // The extend lets go of the extended line's constraints, its Symmetric ones with them, as it does with no mirror:
    // its copy stays where it was, short of its crossing and bound to nothing; the crossing line's copy stays bound.
    CHECK(e.at("released_constraints") == 3);
    CHECK(e.at("dof") == 11);
    CHECK(without.dof() == 7);
    for (int k = 0; k < 2; ++k) {
        INFO("copy " << k + 2);
        CHECK_THAT(off(ents[2 + k].p0, copies[k].p0), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[2 + k].p1, copies[k].p1), WithinAbs(0.0, 1e-9));
    }
    int symmetric_of_line = 0, symmetric_of_crossing = 0;
    for (const SketchEntityConstraintDef& c : with.constraints())
        if (c.type == T::Symmetric) {
            symmetric_of_line += c.ea == 0;
            symmetric_of_crossing += c.ea == 1 && c.eb == 3;
        }
    CHECK(symmetric_of_line == 0);
    CHECK(symmetric_of_crossing == 2);
}

TEST_CASE("a trim of a mirrored source gives the source what it gives with no mirror, and frees the copy", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    const auto off = [](const Vec2d& a, const Vec2d& b) { return (a - b).norm(); };
    // A line crossed at (10, 0) by another, both mirrored about the y axis, the line then trimmed back to the crossing.
    const json crossing = {{"entities", {{{"type", "line"}, {"p0", {5, 0}}, {"p1", {15, 0}}},
                                         {{"type", "line"}, {"p0", {10, -5}}, {"p1", {10, 5}}}}}};
    const json cut      = {{"at", {13, 0}}};
    GUI::DesignSketchTool without;
    begin_sketch(without);
    sketch(without, "sketch_draw", crossing);
    sketch(without, "sketch_trim", cut);
    GUI::DesignSketchTool with;
    begin_sketch(with);
    sketch(with, "sketch_draw", crossing);
    const json m = sketch(with, "sketch_mirror", {{"axis", "y_axis"}, {"entities", {0, 1}}});
    REQUIRE(m.at("constraints_added").size() == 4);
    CHECK(m.at("dof") == 6);   // each source line held Horizontal or Vertical, each copy held by its source
    const auto&                     ents   = with.entities();
    const std::vector<SketchEntity> copies = {ents.at(2), ents.at(3)};
    const json t = sketch(with, "sketch_trim", cut);
    CHECK(t.at("solve_ok") == true);
    // The source is what the same trim leaves with no mirror.
    REQUIRE(ents.size() == 4);
    for (int i : {0, 1}) {
        INFO("entity " << i);
        CHECK_THAT(off(ents[i].p0, without.entities().at(i).p0), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[i].p1, without.entities().at(i).p1), WithinAbs(0.0, 1e-9));
    }
    // The trim lets go of the trimmed line's constraints, its Symmetric ones with them, as it does with no mirror: its
    // copy stays where it was, untrimmed and bound to nothing; the crossing line's copy stays bound to it.
    CHECK(t.at("released_constraints") == 3);
    CHECK(t.at("dof") == 11);
    CHECK(without.dof() == 7);
    for (int k = 0; k < 2; ++k) {
        INFO("copy " << k + 2);
        CHECK_THAT(off(ents[2 + k].p0, copies[k].p0), WithinAbs(0.0, 1e-9));
        CHECK_THAT(off(ents[2 + k].p1, copies[k].p1), WithinAbs(0.0, 1e-9));
    }
    int symmetric_of_line = 0, symmetric_of_crossing = 0;
    for (const SketchEntityConstraintDef& c : with.constraints())
        if (c.type == T::Symmetric) {
            symmetric_of_line += c.ea == 0;
            symmetric_of_crossing += c.ea == 1 && c.eb == 3;
        }
    CHECK(symmetric_of_line == 0);
    CHECK(symmetric_of_crossing == 2);
}

TEST_CASE("sketch_offset binds a single entity's copy as the Offset tool does", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"entities",
                                 {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {10, 5}}},
                                  {{"type", "circle_center"}, {"center", {30, 0}}, {"radius", 5}},
                                  {{"type", "line"}, {"p0", {50, 0}}, {"p1", {60, 3}}},
                                  {{"type", "line"}, {"p0", {50, 10}}, {"p1", {60, 14}}},
                                  {{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}},
                                  {{"type", "line"}, {"p0", {20, 20}}, {"p1", {20, 30}}},
                                  {{"type", "arc_3_point"}, {"p0", {40, 20}}, {"p1", {50, 20}}, {"on_arc", {45, 25}}}}}});
    // A line's copy Parallel to it, the line first, whichever way the line lies, a horizontal or vertical one included,
    // whose copy is not held Horizontal or Vertical as well; a circle's and an arc's Concentric with it.
    for (const auto& [entity, type] : std::vector<std::pair<int, std::string>>{
             {0, "parallel"}, {4, "parallel"}, {5, "parallel"}, {1, "concentric"}, {6, "concentric"}}) {
        INFO(entity << " " << type);
        const json r = sketch(tool, "sketch_offset", {{"distance", 2}, {"entities", {entity}}});
        REQUIRE(r.at("added") == 1);
        const int  copy  = r.at("first_index").get<int>();
        const json added = r.at("constraints_added");
        REQUIRE(added.size() == 1);
        CHECK(added[0].at("type") == type);
        CHECK(added[0].at("refs") == json::array({{{"entity", entity}}, {{"entity", copy}}}));
        CHECK(added[0].at("index").get<std::size_t>() == tool.constraints().size() - 1);
    }
    // Two entities' copies are left unbound.
    const std::size_t nc = tool.constraints().size();
    const json        r  = sketch(tool, "sketch_offset", {{"distance", 2}, {"entities", {2, 3}}});
    CHECK(r.at("added") == 2);
    CHECK(r.at("constraints_added").empty());
    CHECK(tool.constraints().size() == nc);
}

TEST_CASE("sketch_project adds a body's edges to the open sketch as construction", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);   // 40 x 20 x 10, centred on the origin, z 0..10
    const int top = face_facing(doc, Vec3d(0, 0, 1));
    REQUIRE(top >= 0);
    GUI::DesignSketchTool tool;
    begin_sketch(tool);

    // The top face's four edges, construction lines closing the box's 40 x 20 outline: each corner ends two of them.
    json r = GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"face", top}});
    CHECK(r.at("ok") == true);
    CHECK(r.at("scope") == "sketch");
    CHECK(r.at("first_index") == 0);
    CHECK(r.at("added") == 4);
    CHECK(r.at("entities") == 4);
    const std::vector<SketchEntity>& ents = tool.entities();
    REQUIRE(ents.size() == 4);
    std::vector<Vec2d> ends;
    for (const SketchEntity& e : ents) {
        CHECK(e.type == SketchEntity::Type::Line);
        CHECK(e.construction);
        ends.push_back(e.p0);
        ends.push_back(e.p1);
    }
    for (const Vec2d& p : ends)
        CHECK(std::count_if(ends.begin(), ends.end(), [&p](const Vec2d& q) { return (p - q).norm() < 1e-9; }) == 2);
    const BoundingBoxf outline = entities_box(ents, 0, 4);
    CHECK_THAT(outline.size().x(), WithinAbs(40.0, 1e-9));
    CHECK_THAT(outline.size().y(), WithinAbs(20.0, 1e-9));

    // One edge of the top: one line.
    const TopoDS_Shape& box = doc.bodies[0].shape;
    int top_edge = -1;
    for (int i = 0; i < GeometryEngine::edge_count(box) && top_edge < 0; ++i) {
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(GeometryEngine::edge_by_index(box, i));
        if (!pts.empty() && std::all_of(pts.begin(), pts.end(), [](const Vec3d& p) { return p.z() > 10.0 - 1e-6; }))
            top_edge = i;
    }
    REQUIRE(top_edge >= 0);
    r = GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"edges", {top_edge}}});
    CHECK(r.at("first_index") == 4);
    CHECK(r.at("added") == 1);
    REQUIRE(tool.entities().size() == 5);
    CHECK(tool.entities()[4].construction);

    // The plane is the open sketch's: another is refused, and null is the same as leaving it out. A refusal leaves the
    // sketch as it was.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"face", top}, {"plane", "XZ"}}); }),
               ContainsSubstring("plane is the open sketch's: leave it out"));
    CHECK(tool.entities().size() == 5);
    r = GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"face", top}, {"plane", nullptr}});
    CHECK(r.at("added") == 4);
    CHECK(tool.entities().size() == 9);
    // No source body, and a body the engine does not have.
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_project(CadDocument(), tool, json::object()); }),
               ContainsSubstring("no source body"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 3}}); }), ContainsSubstring("body 3"));
    CHECK(failure_code([&] { GUI::Mcp::cad_sketch_project(CadDocument(), tool, json::object()); }) == "precondition");
    CHECK(failure_code([&] { GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 3}}); }) == "invalid_argument");
    // A vertical edge projects to a point on the XY sketch: the edge the call names is the wrong one.
    int upright = -1;
    for (int i = 0; i < GeometryEngine::edge_count(box) && upright < 0; ++i) {
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(GeometryEngine::edge_by_index(box, i));
        if (pts.size() >= 2 && (pts.front() - pts.back()).head<2>().norm() < 1e-9) upright = i;
    }
    REQUIRE(upright >= 0);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"edges", {upright}}}); }),
               ContainsSubstring("project to a point"));
    CHECK(failure_code([&] { GUI::Mcp::cad_sketch_project(doc, tool, {{"source_body", 0}, {"edges", {upright}}}); }) ==
          "invalid_argument");
    CHECK(tool.entities().size() == 9);

    // Construction only: the sketch has no closed loop to build from.
    CHECK(tool.loop_report().loops.empty());
}

TEST_CASE("sketch_insert_svg adds an SVG's outlines to the open sketch", "[Mcp][CAD]")
{
    // A drawing 40 mm square with one filled 10 x 10 mm square off its centre, and the same square unfilled.
    const ScopedTemporaryFile square(".svg"), unfilled(".svg");
    const auto write = [](const ScopedTemporaryFile& file, const char* fill) {
        std::ofstream(file.string()) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="40mm" height="40mm" viewBox="0 0 40 40">)"
                                     << R"(<rect x="20" y="5" width="10" height="10" fill=")" << fill << R"("/></svg>)";
    };
    write(square, "black");
    write(unfilled, "none");
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    const auto sides = [&tool](std::size_t first, double length, const Vec2d& centre) {
        const std::vector<SketchEntity>& ents = tool.entities();
        REQUIRE(ents.size() == first + 4);
        for (std::size_t i = first; i < first + 4; ++i) {
            CHECK(ents[i].type == SketchEntity::Type::Line);
            CHECK_FALSE(ents[i].construction);
            CHECK_THAT((ents[i].p1 - ents[i].p0).norm(), WithinAbs(length, 1e-4));
        }
        const BoundingBoxf box = entities_box(ents, first, 4);
        CHECK_THAT(box.center().x(), WithinAbs(centre.x(), 1e-4));
        CHECK_THAT(box.center().y(), WithinAbs(centre.y(), 1e-4));
    };

    // Its four sides, 10 long, about the origin.
    json r = sketch(tool, "sketch_insert_svg", {{"path", square.string()}});
    CHECK(r.at("ok") == true);
    CHECK(r.at("scope") == "sketch");
    CHECK(r.at("added") == 4);
    CHECK(r.at("entities") == 4);
    sides(0, 10.0, Vec2d(0, 0));
    // Twice the size, about (5, 5).
    r = sketch(tool, "sketch_insert_svg", {{"path", square.string()}, {"scale", 2}, {"position", {5, 5}}});
    CHECK(r.at("added") == 4);
    CHECK(r.at("entities") == 8);
    sides(4, 20.0, Vec2d(5, 5));

    // Refused, leaving the sketch as it was: a plane, which is the open sketch's, a drawing with no fill, and a file
    // that is not there.
    CHECK_THAT(failure([&] { sketch(tool, "sketch_insert_svg", {{"path", square.string()}, {"plane", "XY"}}); }),
               ContainsSubstring("plane is the open sketch's: leave it out"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_insert_svg", {{"path", unfilled.string()}}); }),
               ContainsSubstring("gives no outline"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_insert_svg", {{"path", square.string() + ".missing"}}); }),
               ContainsSubstring("not found"));
    CHECK(tool.entities().size() == 8);
}

TEST_CASE("a loop reports the area it encloses whichever way it runs", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    // A 10 mm square drawn clockwise (entities 0-3), and a slot, which the slot tool lays out
    // clockwise whichever way its centres are given (4-7).
    sketch(tool, "sketch_draw", {{"entities",
                                 {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {0, 10}}},
                                  {{"type", "line"}, {"p0", {0, 10}}, {"p1", {10, 10}}},
                                  {{"type", "line"}, {"p0", {10, 10}}, {"p1", {10, 0}}},
                                  {{"type", "line"}, {"p0", {10, 0}}, {"p1", {0, 0}}},
                                  {{"type", "slot"}, {"c0", {30, 0}}, {"c1", {50, 0}}, {"width", 6}}}}});
    // The report sketch_validate and sketch_describe print as closed_loops.
    const GUI::DesignSketchTool::LoopReport rep = tool.loop_report();
    REQUIRE(rep.loops.size() == 2);
    for (const auto& l : rep.loops) {
        const bool square = l.ents.at(0) < 4;
        CHECK_THAT(l.area, WithinAbs(square ? 10.0 * 10.0 : 20.0 * 6.0 + PI * 3.0 * 3.0, 1e-6));
    }
}

TEST_CASE("a sketch with no constraints reports every entity's own degrees of freedom", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    CHECK(sketch(tool, "sketch_list_constraints", json::object()).at("dof") == 0);   // nothing drawn, nothing free

    // A circle: its centre and its radius.
    json r = sketch(tool, "sketch_draw", {{"type", "circle_center"}, {"center", {0, 0}}, {"radius", 5}});
    REQUIRE(tool.constraints().empty());
    CHECK(r.at("dof") == 3);
    CHECK(sketch(tool, "sketch_list_constraints", json::object()).at("dof") == 3);

    // An arc beside it: its centre and two ends, the ends held at one radius.
    r = sketch(tool, "sketch_draw", {{"type", "arc_center"}, {"center", {20, 0}}, {"radius", 3}, {"start_angle_deg", 0}, {"end_angle_deg", 90}});
    REQUIRE(tool.constraints().empty());
    CHECK(r.at("dof") == 3 + 5);

    // A horizontal line brings the first constraint, and the solver's count agrees: the curves
    // keep their 8, and the line has 3 of its 4 left.
    r = sketch(tool, "sketch_draw", {{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}});
    REQUIRE(tool.constraints().size() == 1);
    CHECK(r.at("dof") == 3 + 5 + 3);
}

TEST_CASE("constraints are planned from picks as the CONSTRAIN bar does, or taken from refs as written", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    using R = SketchPointRole;
    // 0 and 1: two lines at 45 degrees; 2 a circle; 3 an arc; 4 a point.
    std::vector<SketchEntity> ents = {line(0, 0, 10, 0), line(0, 0, 10, 10)};
    SketchEntity circ;
    circ.type   = SketchEntity::Type::Circle;
    circ.center = circ.p0 = Vec2d(20, 0);
    circ.radius = 3;
    SketchEntity arc;
    arc.type        = SketchEntity::Type::Arc;
    arc.center      = Vec2d(30, 0);
    arc.radius      = 2;
    arc.start_angle = 0;
    arc.end_angle   = PI;
    arc.p0          = Vec2d(32, 0);
    arc.p1          = Vec2d(28, 0);
    SketchEntity pt;
    pt.type = SketchEntity::Type::Point;
    pt.p0   = Vec2d(5, 5);
    ents.insert(ents.end(), {circ, arc, pt});
    auto defs = [&](const json& params, const std::vector<int>& sel = {}) { return GUI::Mcp::cad_constraint_defs(params, ents, sel); };
    auto refused = [&](const json& params) { return failure([&] { defs(params); }); };

    // Picks: the planner's points and the current value, an angle in degrees stored in radians.
    auto d = defs({{"type", "angle"}, {"entities", {0, 1}}});
    REQUIRE(d.size() == 1);
    CHECK(d[0].type == T::Angle);
    CHECK_THAT(d[0].value, WithinAbs(PI / 4, 1e-9));
    CHECK_THAT(defs({{"type", "angle"}, {"entities", {0, 1}}, {"value", 30}})[0].value, WithinAbs(PI / 6, 1e-12));
    CHECK(defs({{"type", "parallel"}}, {0, 1})[0].eb == 1);   // the selection stands in for entities
    CHECK(defs({{"type", "equal"}, {"entities", {2, 3}}})[0].type == T::EqualRadius);
    CHECK(defs({{"type", "equal"}, {"entities", {0, 1}}})[0].type == T::EqualLength);
    d = defs({{"type", "coincident"}, {"entities", {0, 3}}});
    CHECK((d[0].ea == 0 && d[0].eb == 3));
    CHECK_THAT(defs({{"type", "radius"}, {"entities", {2}}})[0].value, WithinAbs(3.0, 1e-12));

    // Refs: points with roles, the origin and the axes, and the types the bar has no button for.
    d = defs({{"type", "coincident"}, {"refs", {{{"entity", 2}}, {{"entity", "origin"}}}}});
    CHECK((d[0].ea == 2 && d[0].ra == R::Center && d[0].eb == kSketchRefOrigin));
    d = defs({{"type", "distance"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 0}, {"role", "p1"}}}}, {"value", 40}});
    CHECK((d[0].type == T::Distance && d[0].ea == 0 && d[0].rb == R::P1 && d[0].value == 40.0));
    d = defs({{"type", "lock_x"}, {"refs", {{{"entity", 3}, {"role", "center"}}}}, {"value", -5}});
    CHECK((d[0].ra == R::Center && d[0].value == -5.0));
    CHECK(defs({{"type", "point_on_line"}, {"refs", {{{"entity", 4}}, {{"entity", "x_axis"}}}}})[0].value == 0.0);
    d = defs({{"type", "parallel"}, {"refs", {{{"entity", 0}}, {{"entity", "y_axis"}}}}});
    CHECK(d[0].eb == kSketchRefAxisY);
    d = defs({{"type", "concentric"}, {"refs", {{{"entity", 2}}, {{"entity", 3}}}}});
    CHECK((d[0].ra == R::Center && d[0].rb == R::Center));
    d = defs({{"type", "symmetric"}, {"refs", {{{"entity", 0}, {"role", "p1"}}, {{"entity", 1}, {"role", "p1"}}, {{"entity", "x_axis"}}}}});
    CHECK(d[0].ec == kSketchRefAxisX);

    CHECK_THAT(refused({{"type", "parallell"}, {"entities", {0, 1}}}), ContainsSubstring("did you mean \"parallel\""));
    CHECK_THAT(refused({{"type", "parallel"}, {"entities", {0, 2}}}), ContainsSubstring("got line 0, circle 2 (need_two_lines)"));
    CHECK_THAT(refused({{"type", "distance"}, {"entities", {0, 1}}}), ContainsSubstring("(unsupported)"));
    CHECK_THAT(refused({{"type", "parallel"}}), ContainsSubstring("(need_two_entities)"));
    CHECK_THAT(refused({{"type", "horizontal"}, {"entities", {0, 1}}}), ContainsSubstring("takes 1 entity"));
    CHECK_THAT(refused({{"type", "parallel"}, {"entities", {0, 7}}}), ContainsSubstring("entity 7 does not exist"));
    CHECK_THAT(refused({{"type", "parallel"}, {"entities", {0, 1}}, {"value", 3}}), ContainsSubstring("takes no value"));
    CHECK_THAT(refused({{"type", "parallel"}, {"entities", {0, 1}}, {"refs", {{{"entity", 0}}}}}), ContainsSubstring("not both"));
    CHECK_THAT(refused({{"type", "distance"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 0}, {"role", "p1"}}}}}),
               ContainsSubstring("distance needs value"));
    CHECK_THAT(refused({{"type", "coincident"}, {"refs", {{{"entity", 0}, {"role", "center"}}, {{"entity", "origin"}}}}}),
               ContainsSubstring("role \"center\" does not exist on line 0"));
    CHECK_THAT(refused({{"type", "coincident"}, {"refs", {{{"entity", 0}}, {{"entity", "origin"}}}}}),
               ContainsSubstring("needs a role: line 0 has p0, p1"));
    CHECK_THAT(refused({{"type", "parallel"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 1}}}}}),
               ContainsSubstring("takes no role"));
    CHECK_THAT(refused({{"type", "parallel"}, {"refs", {{{"entity", 0}}, {{"entity", 3}}}}}),
               ContainsSubstring("parallel needs two lines; arc 3 is not one"));
    CHECK_THAT(refused({{"type", "tangent"}, {"refs", {{{"entity", 0}}, {{"entity", 1}}}}}), ContainsSubstring("got line 0 and line 1"));
    CHECK_THAT(refused({{"type", "radius"}, {"refs", {{{"entity", 2}}}}, {"value", 0}}), ContainsSubstring("must be > 0"));
    CHECK_THAT(refused({{"type", "fix"}, {"refs", {{{"entity", 4}}, {{"entity", 0}, {"role", "p0"}}}}}),
               ContainsSubstring("fix takes refs: one point"));
    CHECK_THAT(refused({{"type", "coincident"}, {"refs", {{{"entity", "x_axis"}}, {{"entity", 4}}}}}),
               ContainsSubstring("names a point"));

    // The planner's value rule, which the panel's live and committed constraint paths share.
    std::vector<SketchEntityConstraintDef> planned(2);
    planned[0].type = T::Angle;
    planned[1].type = T::Distance;
    write_plan_value(planned, 90.0);
    CHECK_THAT(planned[0].value, WithinAbs(PI / 2, 1e-12));
    CHECK(planned[1].value == 90.0);
}

TEST_CASE("sketch_dimension and sketch_constrain solve with the new constraint or roll it back, and sketch_delete_constraint frees it", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"type", "rectangle_2_point"}, {"p0", {0, 0}}, {"p1", {30, 20}}});
    const int free_dof = tool.dof();   // position, width and height
    CHECK(free_dof == 4);
    const size_t inferred = tool.constraints().size();

    const json width = {{"type", "distance"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 0}, {"role", "p1"}}}}, {"value", 40}};
    // A dimension is sketch_dimension's, and sketch_constrain adds the geometric constraints only.
    CHECK_THAT(failure([&] { sketch(tool, "sketch_constrain", width); }), ContainsSubstring("sketch_dimension"));
    json r = sketch(tool, "sketch_dimension", width);
    CHECK(r.at("added") == json::array({int(inferred)}));
    CHECK(r.at("items")[0].at("type") == "distance");
    CHECK(r.at("items")[0].at("value") == 40.0);
    CHECK(r.at("dof") == free_dof - 1);
    CHECK_THAT((tool.entities()[0].p1 - tool.entities()[0].p0).norm(), WithinAbs(40.0, 1e-6));

    // A rival width cannot be met: rolled back, and the sketch is as it was.
    json rival = width;
    rival["value"] = 25;
    CHECK_THAT(failure([&] { sketch(tool, "sketch_dimension", rival); }), ContainsSubstring("over-constrained"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_dimension", {{"type", "coincident"}, {"entities", {0, 1}}}); }),
               ContainsSubstring("sketch_constrain"));
    CHECK(tool.constraints().size() == inferred + 1);
    CHECK(tool.dof() == free_dof - 1);

    // Pinning the corner to the origin through the CONSTRAIN bar's picks and refs.
    r = sketch(tool, "sketch_constrain", {{"type", "coincident"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", "origin"}}}}});
    CHECK(r.at("dof") == free_dof - 3);
    CHECK(tool.entities()[0].p0.norm() < 1e-6);

    const json list = sketch(tool, "sketch_list_constraints", json::object());
    CHECK(list.at("scope") == "live");
    REQUIRE(list.at("items").size() == inferred + 2);
    CHECK(list.at("items").back().at("refs")[1] == json{{"entity", "origin"}});

    r = sketch(tool, "sketch_delete_constraint", {{"index", int(inferred)}});   // the width
    CHECK(r.at("removed").at("type") == "distance");
    CHECK(r.at("constraints") == int(inferred + 1));
    CHECK(r.at("dof") == free_dof - 2);
    CHECK_THAT(failure([&] { sketch(tool, "sketch_delete_constraint", {{"index", 99}}); }), ContainsSubstring("out of range"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_delete_constraint", json::object()); }), ContainsSubstring("index must be"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_move", json::object()); }), ContainsSubstring("not a Design command of the open sketch"));
}

TEST_CASE("a sketch command's type names its variant", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    auto type = [&](const char* key) { return property_of(command_of(catalog, key), "type"); };
    CHECK(type("sketch_transform").at("enum") == json::array({"move", "rotate", "scale"}));
    CHECK(type("sketch_pattern").at("enum") == json::array({"linear", "circular"}));
    CHECK(type("sketch_dimension").at("enum") ==
          json::array({"distance", "distance_x", "distance_y", "angle", "radius", "diameter"}));
    CHECK(type("sketch_constrain").at("enum") ==
          json::array({"fix", "coincident", "horizontal", "vertical", "lock_x", "lock_y", "equal_length", "parallel",
                       "perpendicular", "concentric", "tangent", "midpoint", "symmetric", "point_on_line", "point_on_object",
                       "equal_radius", "collinear", "symmetric_about_y", "symmetric_about_x", "equal"}));
    for (const char* key : {"sketch_transform", "sketch_pattern", "sketch_dimension", "sketch_constrain"}) {
        INFO(key);
        CHECK_FALSE(type(key).contains("default"));
        const std::vector<std::string> required = required_of(command_of(catalog, key));
        CHECK(std::find(required.begin(), required.end(), "type") != required.end());
    }
    // A dimension is no geometric constraint, and a geometric constraint no dimension.
    json distance{{"type", "distance"}, {"entities", {0}}, {"value", 5}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sketch_constrain"), distance), ContainsSubstring("type must be one of"));
    json parallel{{"type", "parallel"}, {"entities", {0, 1}}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sketch_dimension"), parallel), ContainsSubstring("type must be one of"));
    // What each takes, with its type alone or with the value a lock takes.
    json linear{{"type", "linear"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_pattern"), linear).empty());
    json dimension{{"type", "distance"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_dimension"), dimension).empty());
    json lock{{"type", "lock_x"}, {"entities", {0}}, {"value", 5}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_constrain"), lock).empty());
    // sketch_extend is the Extend item and Extend's argument form.
    const GUI::NativeCommand& extend = command_of(catalog, "sketch_extend");
    CHECK(property_of(extend, "at").is_object());
    CHECK(property_of(extend, "tolerance").at("default") == 0.05);
    CHECK(two_forms(extend));
    // Each new block has one to three examples.
    for (const char* key : {"sketch_pattern", "sketch_extend", "sketch_dimension"}) {
        INFO(key);
        CHECK(command_of(catalog, key).examples.size() >= 1);
        CHECK(command_of(catalog, key).examples.size() <= 3);
    }

    // The functions: a linear pattern takes no angle; parallel takes no value; a quarter turn read in degrees.
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_draw", {{"entities", {{{"type", "line"}, {"p0", {1, 0}}, {"p1", {2, 0}}}, {{"type", "line"}, {"p0", {0, 5}}, {"p1", {4, 7}}}}}});
    CHECK_THAT(failure([&] { sketch(tool, "sketch_pattern", {{"type", "linear"}, {"entities", {0}}, {"count", 3}, {"dx", 5}, {"angle", 1}}); }),
               ContainsSubstring("linear takes no angle"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_constrain", {{"type", "parallel"}, {"entities", {0, 1}}, {"value", 5}}); }),
               ContainsSubstring("parallel takes no value"));
    json quarter{{"type", "rotate"}, {"entities", {0}}, {"pivot", {0, 0}}, {"angle_deg", 90}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_transform"), quarter).empty());
    sketch(tool, "sketch_transform", quarter);
    CHECK((tool.entities()[0].p0 - Vec2d(0, 1)).norm() < 1e-9);
    CHECK((tool.entities()[0].p1 - Vec2d(0, 2)).norm() < 1e-9);
}

TEST_CASE("the rows for the sketch's shapes, edit ops and constraints", "[Mcp][CAD]")
{
    const std::vector<const char*> tools = {"sketch_draw",      "sketch_fillet",           "sketch_chamfer",          "sketch_trim",
                                            "sketch_extend",    "sketch_transform",        "sketch_pattern",          "sketch_constrain",
                                            "sketch_dimension", "sketch_list_constraints", "sketch_delete_constraint"};
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* t : tools) {
        INFO(t);
        CHECK(command_of(catalog, t).read_only == (std::string(t) == "sketch_list_constraints"));
        CHECK_THAT(command_of(catalog, t).description, !ContainsSubstring("ok is false"));
    }
    auto param = [&](const char* key, const char* name) {
        const json p = property_of(command_of(catalog, key), name);
        INFO(key << " " << name);
        REQUIRE(p.is_object());
        return p;
    };
    auto required = [&](const char* key, const char* name) {
        const std::vector<std::string> req = required_of(command_of(catalog, key));
        return std::find(req.begin(), req.end(), name) != req.end();
    };
    // Required, as a param without a default is; then optional ones.
    for (const auto& [t, k] : std::vector<std::pair<const char*, const char*>>{
             {"sketch_fillet", "radius"}, {"sketch_chamfer", "distance"}, {"sketch_trim", "at"}, {"sketch_extend", "at"},
             {"sketch_transform", "type"}, {"sketch_pattern", "type"}, {"sketch_constrain", "type"}, {"sketch_dimension", "type"},
             {"sketch_delete_constraint", "index"}}) {
        INFO(t << " " << k);
        CHECK(required(t, k));
    }
    for (const auto& [t, k] : std::vector<std::pair<const char*, const char*>>{
             {"sketch_transform", "angle"}, {"sketch_transform", "angle_deg"}, {"sketch_transform", "scale"},
             {"sketch_transform", "pivot"}, {"sketch_pattern", "angle"}, {"sketch_pattern", "angle_deg"},
             {"sketch_pattern", "count"}, {"sketch_pattern", "pivot"}}) {
        param(t, k);
        CHECK_FALSE(required(t, k));
    }
    param("sketch_list_constraints", "sketch");
    CHECK_FALSE(required("sketch_list_constraints", "sketch"));
    const json types = param("sketch_draw", "type").at("enum");
    for (const char* t : {"polygon", "slot", "ellipse", "ellipse_arc", "spline"})
        CHECK(std::find(types.begin(), types.end(), json(t)) != types.end());
    // Every constraint type the recipe stores has its name in one of the two enums, and none is in both.
    const json geometric = param("sketch_constrain", "type").at("enum"), dimensions = param("sketch_dimension", "type").at("enum");
    CHECK(geometric.size() == 20);   // "equal" among them
    CHECK(dimensions.size() == 6);
    CHECK(geometric.size() + dimensions.size() == size_t(SketchConstraintType::SymmetricAboutX) + 2);
    for (const json& d : dimensions) {
        INFO(d.dump());
        CHECK(std::find(geometric.begin(), geometric.end(), d) == geometric.end());
    }
}
