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
#include "slic3r/GUI/CAD/DesignOffer.hpp"
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/McpToolsCad.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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

// Whether a param by this name takes face or edge ids.
bool is_id_param(const std::string& name)
{
    static const std::vector<std::string> ids = {"face",   "faces",  "edge",   "edges", "up_to_face",
                                                 "face_a", "face_b", "edge_a", "edge_b"};
    return std::find(ids.begin(), ids.end(), name) != ids.end();
}

// A call's context that runs its UI closures in place and keeps the arguments it journals.
struct JournalContext final : GUI::Mcp::ToolContext
{
    explicit JournalContext(bool person = false) : picked(person) {}

    bool                  person() const override { return picked; }
    json                  run_ui(const std::function<json()>& fn) override { return fn(); }
    json                  observe_ui(const std::function<json()>&) override { return true; }
    const std::string&    job_id() const override { return id; }
    bool                  cancelled() const override { return false; }
    GUI::Mcp::ToolResult  call_child(const std::string&, const json&) override { return GUI::Mcp::ToolResult::error("no children"); }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(json args) override { journaled = std::move(args); }
    void                  begin_operation() override {}

    bool        picked;
    std::string id;
    json        journaled; // null until the call journals its arguments
};

} // namespace

TEST_CASE("the CAD commands are the Design table's rows less align", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    std::set<std::string>     keys;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        CHECK(keys.insert(c.key).second);
        CHECK(c.group == "Design");
        CHECK_FALSE(c.listed);
        // Its title is the row's name, the one the right-click menu and its tile show.
        CHECK(c.title == design_row(c.key).name);
    }
    CHECK(keys.size() == 133);
    // The body's move is named apart from the Prepare tool transform; undo and redo are the shared commands.
    CHECK(keys.count("body_transform") == 1);
    for (const char* gone : {"transform", "undo", "redo", "align"}) {
        INFO(gone);
        CHECK(keys.count(gone) == 0);
    }
}

TEST_CASE("the Design table has a form for every row but align", "[Mcp][CAD]")
{
    REQUIRE(GUI::kDesignToolCount == 134);
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        INFO(t.id);
        const bool has_form = t.action != nullptr || t.doc != nullptr;
        CHECK(has_form == (std::string(t.id) != "align"));
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
        CHECK(GUI::kDesignTools[i].modal == (id == "sk_text" || id == "sk_svg" || id == "colour"));
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
    CHECK(blocks.size() == 76);
    CHECK(blocks == functions);
}

TEST_CASE("a row with no slot is in no menu row", "[Mcp][CAD]")
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
    CHECK(commands_only == 41);
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
    CHECK((design_row("sk_offset").accepts & GUI::offer_bit(GUI::OfferSel::SkNone)) == 0);   // it needs a picked line, curve or pair
    const GUI::DesignTool& sketch = design_row("sketch");
    CHECK(std::string(GUI::kOfferRowNames[sketch.row]) == "Create");
    CHECK(sketch.family == nullptr);   // the feature-mode Sketch sits directly in Create
    CHECK_FALSE(sketch.sketch_mode);
    // describe_scene names the selection by the atlas's ids, in OfferSel's order.
    REQUIRE(std::size(GUI::kDesignSelectionIds) == std::size_t(GUI::OfferSel::Count));
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::None)]) == "none");
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::FacePlanar)]) == "face_planar");
    CHECK(std::string(GUI::kDesignSelectionIds[int(GUI::OfferSel::Sk2Ent)]) == "sk_2ent");
}

TEST_CASE("a verb with both forms accepts silent, a verb with one does not", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(two_forms(command_of(catalog, "shell")));
    CHECK_FALSE(two_forms(command_of(catalog, "describe_scene")));
    CHECK_FALSE(two_forms(command_of(catalog, "sk_rect")));
    // A command with only a person's form takes no arguments at all.
    json silent{{"silent", true}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sk_rect"), silent), ContainsSubstring("silent"));
    json none = json::object();
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sk_rect"), none).empty());
    // A command that has a tile is the one whose row has an action, and it carries the row's icon.
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        const GUI::DesignTool& row = design_row(c.key);
        CHECK(c.in_palette == (row.action != nullptr));
        CHECK(c.icon == (row.icon != nullptr ? row.icon : ""));
        CHECK(two_forms(c) == (row.action != nullptr && row.doc != nullptr));
        // A tile with no icon would be a blank square in the Speed Dial.
        if (c.in_palette)
            CHECK_FALSE(c.icon.empty());
    }
}

TEST_CASE("a two-form CAD command publishes what its silent form requires", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    const GUI::NativeCommand& fillet  = command_of(catalog, "fillet");
    CHECK_FALSE(fillet.input_schema.contains("required"));
    CHECK(fillet.input_schema.at("if") == json{{"minProperties", 1}});
    CHECK(fillet.input_schema.at("then").at("required") == json::array({"edge"}));
    // No arguments is the person's form, which opens the Fillet tool and requires nothing.
    json person = json::object();
    CHECK(GUI::Mcp::argument_error(fillet, person).empty());
    json radius{{"radius", 2}};
    CHECK_THAT(GUI::Mcp::argument_error(fillet, radius), ContainsSubstring("edge"));
    json silent{{"silent", true}};
    CHECK_THAT(GUI::Mcp::argument_error(fillet, silent), ContainsSubstring("edge"));
}

TEST_CASE("a Design command is named by its verb's id", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"surf_extrude", "surf_revolve", "surf_loft", "surf_fill", "thicken_surf", "surf_offset", "pat_curve",
                            "coordsys_v", "mass_props", "interference", "edit_feature", "body_transform"}) {
        INFO(key);
        CHECK(catalog.find(key) != nullptr);
    }
    const std::set<std::string> old = {"surface_extrude", "surface_revolve", "surface_loft",  "surface_fill",
                                       "thicken_surface", "surface_offset",  "pattern_on_curve", "coordsys",
                                       "mass_properties", "check_interference", "set_feature_expr", "transform"};
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
    // Nor does a description: the names that are no English word.
    for (const GUI::NativeCommand& c : with_cad.commands())
        for (const std::string& name : old)
            if (name != "transform" && name != "coordsys") {
                INFO(c.key << " / " << name);
                CHECK(c.description.find(name) == std::string::npos);
                CHECK(c.input_schema.dump().find(name) == std::string::npos);
            }
}

TEST_CASE("a Design command carries its row's icon", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(command_of(catalog, "sketch").icon == "design_sketch");
    CHECK(command_of(catalog, "body_transform").icon == "design_move");
    // Its tile, titled as the right-click menu names its entry, in the tests' English UI.
    const std::unique_ptr<GUI::AppAction> tile = GUI::NativeCommands::make_action(command_of(catalog, "sketch"));
    REQUIRE(tile != nullptr);
    CHECK(tile->id() == "orca_command:sketch:orca");
    CHECK(tile->group == "Design");
    CHECK(tile->title() == "Sketch");
    CHECK(GUI::NativeCommands::tile_title(command_of(catalog, "sketch")) == "Sketch");
    CHECK(std::string(GUI::title_context(command_of(catalog, "sketch"))) == "Design");
    CHECK(tile->icon == "design_sketch");
    CHECK(tile->command == "sketch");
}

TEST_CASE("the Design tiles' icons resolve to shipped SVGs", "[Mcp][CAD][SpeedDial]")
{
    // An icon comes from a hand-edited atlas row, so a typo would be a blank tile and a blank menu row.
    const boost::filesystem::path images = boost::filesystem::path(PROFILES_DIR).parent_path() / "images";
    const GUI::CommandCatalog     catalog = catalog_with_cad();
    std::size_t                   tiles   = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        if (c.group != "Design" || c.icon.empty())
            continue;
        INFO(c.key << " -> " << c.icon);
        CHECK(boost::filesystem::exists(images / (c.icon + ".svg")));
        tiles += c.in_palette ? 1 : 0;
    }
    CHECK(tiles == 89);
}

TEST_CASE("a tool refused for the selection says it does not apply and what it takes", "[Mcp][CAD]")
{
    // Delete Face takes a picked face. With nothing picked, the obstacle is the selection, not the document its
    // row's refusal speaks of.
    const GUI::DesignTool&                     delete_face = design_row("delete_face");
    const int                                  none        = int(GUI::OfferSel::None);
    const std::optional<GUI::Mcp::ToolFailure> refused     = GUI::Mcp::open_refusal(delete_face, false, none);
    REQUIRE(refused.has_value());
    CHECK(std::string(refused->what()) == "This tool does not apply to what is selected");
    CHECK(refused->details.at("code") == -32000);
    CHECK(refused->details.at("selection_kind") == 0);
    CHECK(refused->details.at("selection") == "none");
    CHECK(refused->details.at("accepts") == json::array({"face_planar", "face_cyl", "face_other"}));
    CHECK_FALSE(GUI::Mcp::open_refusal(delete_face, false, int(GUI::OfferSel::FacePlanar)).has_value());
    // Every row only the menu reaches says the same for a selection it does not take, in its own mode.
    for (int i = 0; i < GUI::kDesignToolCount; ++i) {
        const GUI::DesignTool& t = GUI::kDesignTools[i];
        if (t.action == nullptr || std::string(t.action).rfind("key:", 0) == 0)
            continue;
        INFO(t.id);
        int miss = 0;
        while (miss < int(GUI::OfferSel::Count) && (t.accepts & GUI::offer_bit(GUI::OfferSel(miss))) != 0)
            ++miss;
        REQUIRE(miss < int(GUI::OfferSel::Count));
        const std::optional<GUI::Mcp::ToolFailure> missed = GUI::Mcp::open_refusal(t, t.sketch_mode, miss);
        REQUIRE(missed.has_value());
        CHECK(std::string(missed->what()) == "This tool does not apply to what is selected");
        CHECK(missed->details.at("selection") == GUI::kDesignSelectionIds[miss]);
    }
    // A key: action runs whatever is selected, as its key does.
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("shell"), false, int(GUI::OfferSel::None)).has_value());
}

TEST_CASE("a Design tool opens only in the mode its row belongs to", "[Mcp][CAD]")
{
    const std::string in_sketch = "This tool works in a sketch: open a sketch first";
    const std::string on_model  = "This tool works on the model: finish or cancel the sketch first";
    std::size_t       sketch_rows = 0, model_rows = 0;
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
            const std::optional<GUI::Mcp::ToolFailure> refused = GUI::Mcp::open_refusal(t, !t.sketch_mode, kind);
            REQUIRE(refused.has_value());
            CHECK(std::string(refused->what()) == (t.sketch_mode ? in_sketch : on_model));
            CHECK(refused->details.at("code") == -32000);
            CHECK(refused->details.at("mode") == (t.sketch_mode ? "sketch" : "model"));
        }
        ++(t.sketch_mode ? sketch_rows : model_rows);
    }
    CHECK(sketch_rows == 46);
    CHECK(model_rows == 43);
    // Shell opens on the model whatever is picked, and never over an open sketch.
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("shell"), false, int(GUI::OfferSel::None)).has_value());
    CHECK(GUI::Mcp::open_refusal(design_row("shell"), true, int(GUI::OfferSel::SkNone)).has_value());
    CHECK_FALSE(GUI::Mcp::open_refusal(design_row("sk_rect"), true, int(GUI::OfferSel::SkNone)).has_value());
}

TEST_CASE("a pinned Design tile stays pinned while its tile is not shown", "[Mcp][CAD][SpeedDial]")
{
    CHECK(GUI::is_design_tile_id("orca_command:shell:orca"));
    CHECK(GUI::is_design_tile_id("orca_command:sk_rect:orca"));
    CHECK_FALSE(GUI::is_design_tile_id("orca_command:describe_scene:orca"));   // a Design command with no tile
    CHECK_FALSE(GUI::is_design_tile_id("orca_command:align:orca"));            // an entry with no command
    CHECK_FALSE(GUI::is_design_tile_id("orca_command:save_project:orca"));
    CHECK_FALSE(GUI::is_design_tile_id("orca_plate_goto:shell:orca"));
    CHECK_FALSE(GUI::is_design_tile_id("shell"));
    // On the Prepare tab no Design tile is live, and the third plate is gone.
    const std::string           shell = "orca_command:shell:orca", rect = "orca_command:sk_rect:orca";
    const std::string           save = "orca_command:save_project:orca", plate = "orca_plate_goto:2:orca";
    const std::set<std::string> live = {save};
    std::vector<std::string>    stored, shown;
    const auto                  is_live = [&](const std::string& id) { return live.count(id) != 0; };
    GUI::prune_favourites({shell, plate, save, rect}, is_live, stored, shown);
    CHECK(stored == std::vector<std::string>{shell, save, rect});
    CHECK(shown == std::vector<std::string>{save});
}

TEST_CASE("a hidden Design pin takes no place on the favourites bar", "[Mcp][CAD][SpeedDial]")
{
    // Ten stored pins on the Prepare tab, three of them Design tiles, which are not live there.
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

TEST_CASE("the Design tab's tiles are its mode's tools, and none with the CAD feature off", "[Mcp][CAD][SpeedDial]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const bool sketching : {false, true}) {
        INFO("sketching " << sketching);
        std::set<std::string> tiles;
        for (const GUI::NativeCommand* command : GUI::Mcp::design_tiles(catalog, sketching, /*cad_on=*/true))
            tiles.insert(command->key);
        for (const GUI::NativeCommand& command : catalog.commands()) {
            INFO(command.key);
            const GUI::DesignTool* entry = GUI::Mcp::find_design_tool(command.key);
            REQUIRE(entry != nullptr);
            CHECK(tiles.count(command.key) == (command.in_palette && entry->sketch_mode == sketching ? 1u : 0u));
        }
        CHECK(tiles.count(sketching ? "sk_rect" : "shell") == 1);
        // Turned off while the app runs, the tab stays until a restart, and has no tile.
        CHECK(GUI::Mcp::design_tiles(catalog, sketching, /*cad_on=*/false).empty());
    }
}

TEST_CASE("a Design command with a read block is read-only", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    CHECK(command_of(catalog, "describe_scene").read_only);
    CHECK(command_of(catalog, "query_topology").read_only);
    CHECK_FALSE(command_of(catalog, "extrude").read_only);
    CHECK_FALSE(command_of(catalog, "sketch_add").read_only);
}

TEST_CASE("a CAD command's schema says what its row says", "[Mcp][CAD]")
{
    const GUI::CommandCatalog  catalog = cad_catalog();
    const GUI::NativeCommand&  fillet  = command_of(catalog, "fillet");
    CHECK(required_of(fillet) == std::vector<std::string>{"edge"});
    const json radius = property_of(fillet, "radius");
    CHECK(radius.at("type") == "number");
    CHECK(radius.at("default") == 1);
    CHECK(radius.at("exclusiveMinimum") == 0);
    CHECK_FALSE(radius.contains("minimum"));
    CHECK_THAT(radius.at("description").get<std::string>(), ContainsSubstring("mm"));
    CHECK(property_of(fillet, "edge").at("type") == json::array({"integer", "array"}));
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
        // An argument the row does not name is not refused: the function reads the names it knows.
        CHECK(c.input_schema.value("additionalProperties", true) == true);
    }
    // A type of two kinds, a limit, an enum and a default carry over.
    CHECK(property_of(command_of(catalog, "sketch_begin"), "plane").at("type") == json::array({"string", "object"}));
    CHECK(property_of(command_of(catalog, "plane"), "type").at("enum").size() == 6);
    const json count = property_of(command_of(catalog, "sketch_transform"), "count");
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
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "surf_extrude"), backwards).empty());
    // A fillet's radius is refused at zero, as its function refuses it, and a small one is not.
    json flat{{"edge", 0}, {"radius", 0}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "fillet"), flat), ContainsSubstring("radius"));
    json small{{"edge", 0}, {"radius", 0.005}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "fillet"), small).empty());
}

TEST_CASE("a CAD param whose default is null takes null, which its function reads as left out", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    // No limit; the open sketch's constraints; an extrude that joins the body its profile touches.
    json no_limit{{"limit", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "query_topology"), no_limit).empty());
    json open_sketch{{"sketch", nullptr}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_constraints"), open_sketch).empty());
    json auto_mode{{"boolean", nullptr}};
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
    json no_angle{{"op", "rotate"}, {"angle", nullptr}};
    CHECK_THAT(GUI::Mcp::argument_error(command_of(catalog, "sketch_transform"), no_angle), ContainsSubstring("angle"));
    // One kind still stands for the list of it.
    json one_kind{{"kinds", "planar"}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "query_topology"), one_kind).empty());
    CHECK(one_kind == json{{"kinds", {"planar"}}});

    // Every param whose default is null admits null, but these, whose functions read a null as given: thread on a
    // plane resolves `plane` as the plane named, and sketch_transform tells an op's own params by their presence.
    const std::set<std::pair<std::string, std::string>> as_given{
        {"thread", "plane"}, {"sketch_transform", "angle"}, {"sketch_transform", "scale"},
        {"sketch_transform", "count"}, {"sketch_transform", "pivot"}};
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
    CHECK(admitting == 36);
}

TEST_CASE("a CAD command takes the names its function also reads", "[Mcp][CAD]")
{
    // describe_feature and the tree edits read `index` as `feature`, and set_feature `params` or `values` as
    // `fields`: what agents often call them. The call is read as the schema names them.
    const GUI::CommandCatalog catalog = cad_catalog();
    json                      set{{"index", 0}, {"params", {{"width", 20}}}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "set_feature"), set).empty());
    CHECK(set == json{{"feature", 0}, {"fields", {{"width", 20}}}});
    json values{{"feature", 0}, {"values", {{"width", 30}}}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "set_feature"), values).empty());
    CHECK(values == json{{"feature", 0}, {"fields", {{"width", 30}}}});
    json describe{{"index", 1}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "describe_feature"), describe).empty());
    CHECK(describe == json{{"feature", 1}});
    // A command that takes `index` itself keeps it.
    json unconstrain{{"index", 4}};
    CHECK(GUI::Mcp::argument_error(command_of(catalog, "sketch_unconstrain"), unconstrain).empty());
    CHECK(unconstrain == json{{"index", 4}});
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
    CHECK_THAT(command_of(catalog, "sketch_transform").description, ContainsSubstring("polar"));
    CHECK_THAT(command_of(catalog, "fillet").description, ContainsSubstring("ok is false"));
    CHECK_THAT(command_of(catalog, "extrude").description, !ContainsSubstring("ok is false"));
    // One with an action only does what its row does, which for Delete Body is to delete it at once.
    CHECK_THAT(command_of(catalog, "delete_body").description, ContainsSubstring("or act at once"));
    CHECK_THAT(command_of(catalog, "delete_body").description, !ContainsSubstring("opens the tool"));
}

TEST_CASE("a Design command with a person's form says which mode it runs in", "[Mcp][CAD]")
{
    const std::string   in_sketch = "It runs only in an open sketch", outside = "It runs only outside a sketch";
    const GUI::CommandCatalog catalog = cad_catalog();
    std::size_t               told    = 0;
    for (const GUI::NativeCommand& c : catalog.commands()) {
        const GUI::DesignTool& row = design_row(c.key);
        if (row.action == nullptr)
            continue;
        INFO(c.key);
        // An action only: its description says it. Both forms: the `silent` property, which says what the call
        // with no arguments does, as the description is the silent form's.
        const std::string text = row.doc == nullptr ? c.description
                                                    : property_of(c, "silent").value("description", std::string());
        CHECK_THAT(text, ContainsSubstring(row.sketch_mode ? in_sketch : outside));
        CHECK_THAT(text, !ContainsSubstring(row.sketch_mode ? outside : in_sketch));
        ++told;
    }
    CHECK(told == 89);
    CHECK_THAT(command_of(catalog, "sk_rect").description, ContainsSubstring(in_sketch));
    CHECK_THAT(command_of(catalog, "delete_body").description, ContainsSubstring(outside));
    CHECK_THAT(property_of(command_of(catalog, "shell"), "silent").value("description", std::string()),
               ContainsSubstring("opens nothing"));
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
    CHECK(design_commands(true) == 133);
}

TEST_CASE("a CAD call is journaled without its generation and with its silent", "[Mcp][CAD]")
{
    json                     seen;
    const GUI::NativeCommand fillet = GUI::Mcp::cad_command("fillet", "Fillet", json{{"type", "object"}, {"properties", json::object()}},
                                                            [&seen](const json& args) {
                                                                seen = args;
                                                                return json{{"ok", true}};
                                                            });
    CHECK(fillet.group == "Design");
    CHECK_FALSE(fillet.listed);

    JournalContext             agent;
    const GUI::Mcp::ToolResult reply = fillet.run({{"generation", 4}, {"silent", true}, {"radius", 2}}, agent);
    CHECK(agent.journaled == json{{"silent", true}, {"radius", 2}});
    // The function still checks the generation, and the reply wraps its value.
    CHECK(seen == json{{"generation", 4}, {"silent", true}, {"radius", 2}});
    CHECK_FALSE(reply.is_error);
    CHECK(reply.data == json{{"result", {{"ok", true}}}});

    // A person's pick runs the function in place, with the same reply, and journals nothing.
    JournalContext person(/*person=*/true);
    CHECK(fillet.run(json::object(), person).data == json{{"result", {{"ok", true}}}});
    CHECK(person.journaled.is_null());

    // A silent call of a command that also has a person's form, which gives nothing but its generation, is
    // journaled silent: with no arguments, its replay would open the tool.
    const GUI::NativeCommand shell = GUI::Mcp::cad_command(
        "shell", "Shell", GUI::Mcp::with_silent(json{{"type", "object"}, {"properties", json::object()}}),
        [](const json&) { return json{{"ok", true}}; });
    JournalContext generation_only;
    shell.run({{"generation", 4}}, generation_only);
    CHECK(generation_only.journaled == json{{"silent", true}});
    // One with a silent form only has no `silent` to journal.
    JournalContext read;
    fillet.run({{"generation", 4}}, read);
    CHECK(read.journaled == json::object());

    // A failure is the call's error.
    const GUI::NativeCommand refused = GUI::Mcp::cad_command("hole", "Hole", json{{"type", "object"}, {"properties", json::object()}},
                                                             [](const json&) -> json {
                                                                 throw GUI::Mcp::ToolFailure("removed no material", {{"code", -32000}});
                                                             });
    CHECK_THROWS_AS(refused.run(json::object(), agent), GUI::Mcp::ToolFailure);
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
    // The 76 Design commands with a silent form, undo and redo.
    CHECK(described == 78);
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
// read them with query_topology and pass that reply's generation with the call.
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
                    CHECK(calls[size_t(from)].at("tool") == "query_topology");
                }
            }
        }
}

TEST_CASE("every CAD command with a silent form has one to three examples", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const GUI::NativeCommand& c : catalog.commands()) {
        INFO(c.key);
        // A command with an action and no silent form takes no arguments, so there is no call to show.
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

    doc.features[datum].enabled = false;
    REQUIRE(doc.recompute());
    CHECK_THAT(error_of({{"datum", datum}}, doc), ContainsSubstring("hidden"));

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

TEST_CASE("describe_feature reads a sketch and the extrude bound to a variable", "[Mcp][CAD]")
{
    CadDocument doc;
    add_bound_block(doc);

    const json sketch = GUI::Mcp::cad_feature_json(doc, 0);
    CHECK(sketch.at("type") == "Sketch");
    CHECK(sketch.at("name") == "Sketch 1");
    CHECK(sketch.at("plane").at("base") == "XY");
    CHECK(sketch.at("editable").empty());   // the sketch session edits entities, not set_feature
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
    CHECK(extrude.at("params").at("mode") == "new");
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

TEST_CASE("describe_feature names constraint references as the solver reads them", "[Mcp][CAD]")
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

TEST_CASE("describe_feature lists the features built on a datum plane", "[Mcp][CAD]")
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

TEST_CASE("describe_feature gives each imported region's loops and filled area as placed", "[Mcp][CAD]")
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
    for (int t = 0; t <= int(CadFeatureType::Mate); ++t) {
        CadDocument doc;
        CadFeature  f;
        f.type = CadFeatureType(t);
        if (f.type == CadFeatureType::Boolean) f.mode = BooleanMode::Cut;   // a Boolean never makes a new body
        doc.features.push_back(f);
        const json card = GUI::Mcp::cad_feature_json(doc, 0);
        INFO(card.at("type").get<std::string>());

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

TEST_CASE("set_feature writes literal fields and the card's choices", "[Mcp][CAD]")
{
    CadFeature ex;
    ex.type = CadFeatureType::Extrude;
    GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", 15}, {"mode", "subtract"}, {"flip", true}, {"extrude_end", "symmetric"}});
    CHECK(ex.distance == 15.0);
    CHECK(ex.mode == BooleanMode::Cut);
    CHECK(ex.flip);
    CHECK(ex.extrude_end == ExtrudeEnd::Symmetric);

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
}

TEST_CASE("set_feature refuses what the card would not produce, changing nothing", "[Mcp][CAD]")
{
    CadDocument doc;
    add_bound_block(doc);
    CadFeature ex = doc.features[1];
    ex.expr.clear();

    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[1], {{"distance", 15}}); }),
               ContainsSubstring("driven by the expression 'h'"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[1], {{"distance", 15}}); }),
               ContainsSubstring("remove the binding with edit_feature {field: 'distance', expr: ''}"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"depth", 1}}); }),
               ContainsSubstring("editable: distance, distance2, taper_deg, extrude_end, flip, mode"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"sketch_ref", 2}}); }),
               ContainsSubstring("'sketch_ref' is not an editable field"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(doc.features[0], {{"width", 30}}); }),
               ContainsSubstring("no field set_feature can change"));   // an entity sketch
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", 2000}}); }),
               ContainsSubstring("distance must be between -1000 and 1000"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"distance", "15"}}); }), ContainsSubstring("must be a number"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"mode", "add"}}); }),
               ContainsSubstring("mode must be one of new, union, subtract, intersect"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, {{"extrude_end", "up_to_face"}}); }),
               ContainsSubstring("needs a target face"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(ex, json::object()); }), ContainsSubstring("fields must name"));

    CadFeature boolean;
    boolean.type = CadFeatureType::Boolean;
    CHECK_THAT(failure([&] { GUI::Mcp::cad_apply_feature_fields(boolean, {{"mode", "new"}}); }),
               ContainsSubstring("union, subtract or intersect"));
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

    // set_feature's sequence for an edit the kernel refuses: a hole that misses the body.
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

TEST_CASE("commit, new_design and export_step change the design or the plate, and fail as errors", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"commit", "new_design", "export_step"}) {
        INFO(key);
        const GUI::NativeCommand& c = command_of(catalog, key);
        CHECK_FALSE(c.read_only);
        // A failure is the call's error, not a reply whose ok is false.
        CHECK_THAT(c.description, !ContainsSubstring("ok is false"));
    }
    // export_step's path has no default, so it is required.
    CHECK(required_of(command_of(catalog, "export_step")) == std::vector<std::string>{"path"});
}

TEST_CASE("commit's replace takes distinct object indices that are not part of a cut", "[Mcp][CAD]")
{
    Model model;
    for (int i = 0; i < 3; ++i) model.add_object()->name = "object " + std::to_string(i);
    model.objects[2]->cut_id.init();

    CHECK(GUI::Mcp::cad_commit_replace(json::array(), model).empty());
    CHECK(GUI::Mcp::cad_commit_replace({1, 0}, model) == std::vector<int>{0, 1});

    auto refusal = [&](const json& replace) { return failure([&] { GUI::Mcp::cad_commit_replace(replace, model); }); };
    CHECK_THAT(refusal(0), ContainsSubstring("list of object indices"));
    CHECK_THAT(refusal({3}), ContainsSubstring("the project has 3 objects"));
    CHECK_THAT(refusal({-1}), ContainsSubstring("out of range"));
    CHECK_THAT(refusal({0, 0}), ContainsSubstring("listed twice"));
    CHECK_THAT(refusal({0.5}), ContainsSubstring("not 0.5"));
    CHECK_THAT(refusal({"0"}), ContainsSubstring("object indices"));
    CHECK_THAT(refusal({0, 2}), ContainsSubstring("part of a cut object"));
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
    for (const char* key : {"feature_delete", "feature_move", "feature_set_enabled", "feature_rename", "body_set", "body_delete"}) {
        INFO(key);
        CHECK_FALSE(command_of(catalog, key).read_only);
        CHECK_THAT(command_of(catalog, key).description, !ContainsSubstring("ok is false"));
    }
    // The params each one requires.
    CHECK(required_of(command_of(catalog, "feature_delete")) == std::vector<std::string>{"feature"});
    CHECK(required_of(command_of(catalog, "feature_move")) == std::vector<std::string>{"feature", "delta"});
    CHECK(required_of(command_of(catalog, "feature_set_enabled")) == std::vector<std::string>{"feature", "enabled"});
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
        const json disc_only = {{"sketch", sk}, {"loop", disc}, {"distance", 8}, {"boolean", "new"}};
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
    const int cut = GUI::Mcp::cad_sketch_extrude(doc, tool, {{"sketch", lip}, {"boolean", "subtract"}, {"distance", 2}})("Extrude 7");
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
    CHECK_THAT(refusal({{"sketch", sk}, {"boolean", "atuo"}}), ContainsSubstring("boolean must be auto, new"));
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
        CHECK_THAT(failure([&] { revolve({{"boolean", "union"}}); }), ContainsSubstring("needs a body"));
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

TEST_CASE("the sketch-to-solid rows take a sketch, and sketch_begin takes every plane form", "[Mcp][CAD]")
{
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* key : {"sketch_begin", "sketch_describe", "extrude", "revolve"}) {
        INFO(key);
        CHECK_THAT(command_of(catalog, key).description, !ContainsSubstring("ok is false"));
    }
    CHECK(command_of(catalog, "sketch_describe").read_only);   // reads, so it also answers under an open card
    CHECK(property_of(command_of(catalog, "sketch_begin"), "plane").at("type") == json::array({"string", "object"}));
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

TEST_CASE("query_topology names a chamfer's cone and a fillet's torus, with their radii and areas", "[Mcp][CAD]")
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

    const json reply = GUI::Mcp::cad_query_topology(doc, {{"kinds", {"conical", "toroidal"}}});
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
    const json sides = GUI::Mcp::cad_query_topology(doc, {{"kinds", {"cylindrical"}}});
    REQUIRE(sides.at("faces_total") == 1);
    CHECK_THAT(sides.at("faces")[0].at("radius").get<double>(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(sides.at("faces")[0].at("area").get<double>(), WithinAbs(2 * PI * 10.0 * 18.0, 1e-3));
}

TEST_CASE("query_topology reads a revolved half disc as a sphere", "[Mcp][CAD]")
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

    const json reply = GUI::Mcp::cad_query_topology(doc, {{"kinds", {"spherical"}}});
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

TEST_CASE("query_topology's adjacency gives each edge its faces and says which corners are inside", "[Mcp][CAD]")
{
    SECTION("a box: every edge between two faces, every face four edges, every corner outside")
    {
        CadDocument doc;
        add_box(doc);
        const json reply = GUI::Mcp::cad_query_topology(doc, {{"adjacency", true}});
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
        const json concave = GUI::Mcp::cad_query_topology(doc, {{"convex", false}});
        REQUIRE(concave.at("edges_total") == 1);
        const json& edge = concave.at("edges")[0];
        CHECK(edge.at("kind") == "line");
        CHECK(edge.at("convex") == false);
        CHECK_THAT(edge.at("p0")[0].get<double>(), WithinAbs(5.0, 1e-6));
        CHECK_THAT(edge.at("p0")[1].get<double>(), WithinAbs(5.0, 1e-6));
        // Without a face filter, the faces listed are the edge's two neighbours.
        CHECK(concave.at("faces_total") == 2);
        CHECK(concave.at("faces").size() == 2);
        CHECK(GUI::Mcp::cad_query_topology(doc, {{"convex", true}}).at("edges_total") == 17);
    }
    SECTION("a cylinder: its seam has one face, and the rims meet the caps in outside corners")
    {
        CadDocument doc;
        add_cylinder(doc);
        const json reply = GUI::Mcp::cad_query_topology(doc, {{"adjacency", true}});
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
        const json torus = GUI::Mcp::cad_query_topology(doc, {{"kinds", {"toroidal"}}, {"adjacency", true}});
        REQUIRE(torus.at("faces_total") == 1);
        int tangent = 0;
        for (const json& e : torus.at("edges"))   // the torus's own edges, as neighbours
            if (e.value("tangent", false)) ++tangent;
        CHECK(tangent == 2);
    }
}

TEST_CASE("query_topology filters by direction, extreme and kind, and pages both lists", "[Mcp][CAD]")
{
    CadDocument doc;
    add_box(doc);
    const int top = face_facing(doc, Vec3d(0, 0, 1));

    const json up = GUI::Mcp::cad_query_topology(doc, {{"direction", {0, 0, 2}}});
    REQUIRE(up.at("faces_total") == 1);
    CHECK(up.at("faces")[0].at("id") == top);
    CHECK(up.at("edges_total") == 4);   // the vertical edges, whichever way they run
    for (const json& e : up.at("edges")) CHECK_THAT(e.at("length").get<double>(), WithinAbs(10.0, 1e-6));

    const json high = GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "+z"}});
    REQUIRE(high.at("faces_total") == 1);
    CHECK(high.at("faces")[0].at("id") == top);
    CHECK(high.at("edges").empty());
    CHECK(GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "-x"}}).at("faces")[0].at("id") == face_facing(doc, Vec3d(-1, 0, 0)));

    const json top_edges = GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
    CHECK(top_edges.at("edges_total") == 4);
    CHECK(GUI::Mcp::cad_query_topology(doc, {{"kinds", {"planar"}}, {"adjacency", true}}).at("edges_total") == 12);
    CHECK(GUI::Mcp::cad_query_topology(doc, {{"edge_kinds", {"circle"}}}).at("faces").empty());

    const json first = GUI::Mcp::cad_query_topology(doc, {{"limit", 2}});
    CHECK(first.at("faces").size() == 2);
    CHECK(first.at("edges").size() == 2);
    CHECK(first.at("faces_total") == 6);
    CHECK(first.at("edges_total") == 12);
    CHECK(first.at("next_offset") == 2);
    const json last = GUI::Mcp::cad_query_topology(doc, {{"limit", 2}, {"offset", 10}});
    CHECK(last.at("faces").empty());
    CHECK(last.at("edges").size() == 2);
    CHECK(last.at("edges")[0].at("id") == 10);
    CHECK_FALSE(last.contains("next_offset"));

    // A face kind may be named by its surface's noun.
    CHECK(GUI::Mcp::cad_query_topology(doc, {{"kinds", {"plane"}}}).at("faces_total") ==
          GUI::Mcp::cad_query_topology(doc, {{"kinds", {"planar"}}}).at("faces_total"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"kinds", {"face"}}}); }), ContainsSubstring("kinds must be one of planar"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"edge_kinds", "line"}}); }), ContainsSubstring("must be a list"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"direction", {0, 0, 0}}}); }), ContainsSubstring("must not be zero"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"direction", {0, 1}}}); }), ContainsSubstring("[x, y, z]"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"convex", "yes"}}); }), ContainsSubstring("convex must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "top"}}); }), ContainsSubstring("at_extreme must be one of"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"limit", 0}}); }), ContainsSubstring("limit must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"offset", -1}}); }), ContainsSubstring("offset must be"));
    CHECK_THAT(failure([&] { GUI::Mcp::cad_query_topology(doc, {{"body", 3}}); }), ContainsSubstring("out of range"));
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
    const json edges = GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
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
        const json rim = GUI::Mcp::cad_query_topology(doc, {{"edge_kinds", {"circle"}}});
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
        const json cones = GUI::Mcp::cad_query_topology(doc, {{"kinds", {"conical"}}});
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
        CadDocument empty;
        CHECK_THAT(failure([&] { GUI::Mcp::cad_hole_placement(empty, json::object(), pl, body); }), ContainsSubstring("no body to drill"));
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
    GUI::Mcp::cad_face_extrude(doc, {{"face", roof}, {"distance", 3}, {"flip", true}, {"boolean", "subtract"}}, frame)("Extrude");
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
    CHECK_THAT(refusal({{"boolean", "atuo"}}), ContainsSubstring("boolean must be"));
    CHECK(doc.features.size() == features);

    CadDocument cyl;
    add_cylinder(cyl);
    CHECK_THAT(failure([&] { GUI::Mcp::cad_face_extrude(cyl, {{"face", cylinder_face(cyl)}}, frame); }),
               ContainsSubstring("is cylindrical; a push/pull needs a planar face"));
}

TEST_CASE("plane adds the datum plane each construction gives, as the Plane card does", "[Mcp][CAD]")
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

    const json top_edges = GUI::Mcp::cad_query_topology(doc, {{"at_extreme", "+z"}, {"adjacency", true}});
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

TEST_CASE("plane refuses what the card refuses, and what a construction would not use", "[Mcp][CAD]")
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

TEST_CASE("the rows for topology, measure, holes, push/pull and plane", "[Mcp][CAD]")
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
    CHECK_FALSE(command_of(catalog, "plane").read_only);
    CHECK(command_of(catalog, "query_topology").read_only);
    CHECK(command_of(catalog, "measure").read_only);
    for (const char* t : {"hole", "hole_styled", "hole_standard", "extrude", "plane"}) {
        INFO(t);
        CHECK_THAT(command_of(catalog, t).description, !ContainsSubstring("ok is false"));
    }
    // Optional everywhere: none of these is required.
    for (const char* k : {"kinds", "edge_kinds", "direction", "at_extreme", "convex", "limit"}) {
        param("query_topology", k);
        CHECK_FALSE(required("query_topology", k));
    }
    for (const char* hole : {"hole", "hole_styled", "hole_standard"}) {
        param(hole, "at");
        CHECK_FALSE(required(hole, "at"));
        CHECK(param(hole, "body").at("default") == -1);
    }
    param("extrude", "face");
    CHECK_FALSE(required("extrude", "face"));
    for (const char* k : {"face_a", "face_b", "edge_a", "edge_b"}) {
        param("plane", k);
        CHECK_FALSE(required("plane", k));
    }
    CHECK(param("plane", "type").at("enum").size() == 6);
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
    const json rims = GUI::Mcp::cad_query_topology(doc, {{"edge_kinds", {"circle"}}});
    for (const json& e : rims.at("edges"))
        if (std::abs(e.at("radius").get<double>() - r) < 1e-6) return e.at("id").get<int>();
    return -1;
}

// The first straight edge of body 0.
int line_edge(const CadDocument& doc) { return GUI::Mcp::cad_query_topology(doc, {{"edge_kinds", {"line"}}}).at("edges").at(0).at("id").get<int>(); }

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
        GUI::Mcp::cad_sweep(doc, {{"sketch", s}, {"path", helix}, {"boolean", "new"}})("Sweep");
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
    CHECK_THAT(refusal({{"sketch", s}, {"path", helix}, {"boolean", "union"}}), ContainsSubstring("needs a body"));
    CHECK_THAT(refusal({{"sketch", s}, {"path", helix}, {"boolean", "join"}}), ContainsSubstring("boolean must be"));
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
    CHECK_THAT(refusal({{"profiles", {base, top}}, {"boolean", "subtract"}}), ContainsSubstring("needs a body"));
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

// A fresh sketch session on XY, as sketch_begin leaves it: the Select tool armed.
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

TEST_CASE("sketch_add draws polygons, slots, ellipses and splines as their tools do", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    using ET = SketchEntity::Type;

    // A hexagon 8 across the flats: six lines, the first edge's midpoint on +x at the apothem.
    json r = sketch(tool, "sketch_add", {{"type", "polygon"}, {"center", {0, 0}}, {"sides", 6}, {"across_flats", 8}});
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
    r = sketch(tool, "sketch_add", {{"type", "polygon"}, {"center", {50, 0}}, {"sides", 4}, {"radius", 5}, {"angle_deg", 90}});
    CHECK_THAT(ents[6].p0.x(), WithinAbs(50.0, 1e-9));
    CHECK_THAT(ents[6].p0.y(), WithinAbs(5.0, 1e-9));

    // A slot, an ellipse and its arc, and a spline in one list, with a line between them.
    tool.cancel();
    begin_sketch(tool);
    r = sketch(tool, "sketch_add",
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
    sketch(tool, "sketch_add", {{"type", "polygon"}, {"center", {0, 60}}, {"sides", 3}, {"radius", 5}});
    CHECK_FALSE(ents.back().construction);
    CHECK_THAT((ents[8].p0 - Vec2d(5, 60)).norm(), WithinAbs(0.0, 1e-9));   // inscribed: a vertex at the radius
    tool.set_tool(GUI::DesignSketchTool::Mode::Polygon);
    sketch(tool, "sketch_add", {{"type", "slot"}, {"c0", {0, 80}}, {"c1", {10, 80}}, {"width", 2}, {"construction", true}});
    CHECK(ents.back().construction);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Polygon);   // the armed tool is left as it was
    CHECK(tool.pending_points() == 0);

    // Refusals change nothing, even when only a later item of a list is wrong.
    const size_t n = ents.size();
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_add", params); }); };
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
    CHECK_THAT(refused({{"type", "arc"}, {"center", {0, 0}}, {"radius", 5}, {"start_angle", 0}, {"start_angle_deg", 0}}),
               ContainsSubstring("not both"));
    CHECK_THAT(refused({{"type", "spline"}, {"ctrl", {{0, 0}}}}), ContainsSubstring("2 or more poles"));
    CHECK_THAT(refused({{"type", "spline"}, {"ctrl", {{0, 0}, {0, 0}}}}), ContainsSubstring("coincide"));
    CHECK_THAT(refused({{"entities", {{{"type", "line"}, {"p0", {0, 0}}, {"p1", {1, 0}}}, {{"type", "gear"}}}}}),
               ContainsSubstring("unknown entity type"));
    CHECK_THAT(refused(json::object()), ContainsSubstring("needs"));
    CHECK(ents.size() == n);

    // An arc's angles may be given in degrees too.
    sketch(tool, "sketch_add", {{"type", "arc"}, {"center", {0, 0}}, {"radius", 5}, {"start_angle_deg", 0}, {"end_angle_deg", 90}});
    CHECK_THAT(ents.back().end_angle, WithinAbs(PI / 2, 1e-12));
}

TEST_CASE("sketch_fillet rounds an inside corner with an arc bound tangent to both legs", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_add", {{"entities", l_bracket()}});
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

TEST_CASE("sketch_trim cuts a line back to a crossing and extend runs one out to the next entity", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_add", {{"entities",
                                 {{{"type", "line"}, {"p0", {-10, 0}}, {"p1", {10, 0}}},
                                  {{"type", "line"}, {"p0", {0, -10}}, {"p1", {0, 10}}},
                                  {{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}},
                                  {{"type", "line"}, {"p0", {30, 15}}, {"p1", {30, 25}}},
                                  {{"type", "circle"}, {"center", {60, 0}}, {"radius", 3}},
                                  {{"type", "line"}, {"p0", {100, -50}}, {"p1", {110, -50}}}}}});
    const auto& ents = tool.entities();

    json r = sketch(tool, "sketch_trim", {{"at", {5, 0}}});
    CHECK(r.at("entity") == 0);
    CHECK(r.at("geometry").at("type") == "line");
    CHECK(ents[0].p0 == Vec2d(-10, 0));
    CHECK(ents[0].p1.norm() < 1e-9);

    r = sketch(tool, "sketch_trim", {{"at", {9, 20}}, {"extend", true}});
    CHECK(r.at("entity") == 2);
    CHECK_THAT((ents[2].p1 - Vec2d(30, 20)).norm(), WithinAbs(0.0, 1e-9));

    const std::vector<SketchEntity> before = ents;
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_trim", params); }); };
    CHECK_THAT(refused({{"at", {100, 100}}}), ContainsSubstring("no sketch entity within 0.05 mm"));
    CHECK_THAT(refused({{"at", {63, 0}}, {"extend", true}}), ContainsSubstring("extend works on lines and arcs; circle 4"));
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

TEST_CASE("sketch_transform moves, rotates, scales and arrays as the transform tools do", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_add", {{"entities",
                                 {{{"type", "line"}, {"p0", {8, 0}}, {"p1", {12, 0}}},
                                  {{"type", "line"}, {"p0", {1, 0}}, {"p1", {2, 0}}},
                                  {{"type", "circle"}, {"center", {30, 0}}, {"radius", 2}}}}});
    const auto& ents = tool.entities();

    // Six around the origin: five copies every 60 degrees, each bound equal in length.
    json r = sketch(tool, "sketch_transform", {{"op", "polar_array"}, {"entities", {0}}, {"count", 6}, {"pivot", {0, 0}}});
    CHECK(r.at("first_index") == 3);
    CHECK(r.at("added") == 5);
    CHECK(r.at("bound") == 5);
    CHECK(r.at("pivot") == json::array({0.0, 0.0}));
    for (int k = 1; k <= 5; ++k) {
        const SketchEntity& e = ents[2 + k];
        CHECK_THAT((e.p0 - 8.0 * Vec2d(std::cos(k * PI / 3), std::sin(k * PI / 3))).norm(), WithinAbs(0.0, 1e-6));
        CHECK(tool.constraints()[tool.constraints().size() - 6 + k].type == T::EqualLength);
    }

    // A linear array of the circle: two copies 5 apart, each with the source's radius.
    r = sketch(tool, "sketch_transform", {{"op", "array"}, {"entities", {2}}, {"dy", 5}, {"count", 3}});
    CHECK(r.at("added") == 2);
    CHECK((ents[8].center - Vec2d(30, 5)).norm() < 1e-9);
    CHECK((ents[9].center - Vec2d(30, 10)).norm() < 1e-9);
    CHECK_FALSE(r.contains("pivot"));

    // Rotate a quarter turn about the origin, then scale twice about it, then move.
    r = sketch(tool, "sketch_transform", {{"op", "rotate"}, {"entities", {1}}, {"angle_deg", 90}, {"pivot", {0, 0}}});
    CHECK(r.at("moved") == json::array({1}));
    CHECK((ents[1].p0 - Vec2d(0, 1)).norm() < 1e-9);
    CHECK((ents[1].p1 - Vec2d(0, 2)).norm() < 1e-9);
    sketch(tool, "sketch_transform", {{"op", "scale"}, {"entities", {1}}, {"scale", 2}, {"pivot", {0, 0}}});
    CHECK((ents[1].p1 - Vec2d(0, 4)).norm() < 1e-9);
    sketch(tool, "sketch_transform", {{"op", "move"}, {"entities", {1}}, {"dx", 3}});
    CHECK((ents[1].p1 - Vec2d(3, 4)).norm() < 1e-9);
    // Without a pivot, rotate turns about the entities' centroid, the gizmo's pivot.
    r = sketch(tool, "sketch_transform", {{"op", "rotate"}, {"entities", {1}}, {"angle", PI}});
    CHECK_THAT(r.at("pivot")[0].get<double>(), WithinAbs(3.0, 1e-9));
    CHECK_THAT(r.at("pivot")[1].get<double>(), WithinAbs(3.0, 1e-9));
    CHECK((ents[1].p1 - Vec2d(3, 2)).norm() < 1e-9);
    CHECK(tool.mode() == GUI::DesignSketchTool::Mode::Select);

    const size_t n = ents.size();
    auto refused = [&](const json& params) { return failure([&] { sketch(tool, "sketch_transform", params); }); };
    CHECK_THAT(refused({{"op", "spin"}, {"entities", {1}}}), ContainsSubstring("op must be"));
    CHECK_THAT(refused({{"op", "move"}, {"entities", {1}}, {"dx", 1}, {"angle", 1}}), ContainsSubstring("move takes no angle"));
    CHECK_THAT(refused({{"op", "move"}, {"entities", {1}}}), ContainsSubstring("non-zero dx or dy"));
    CHECK_THAT(refused({{"op", "rotate"}, {"entities", {1}}}), ContainsSubstring("non-zero angle"));
    CHECK_THAT(refused({{"op", "scale"}, {"entities", {1}}, {"scale", 1}}), ContainsSubstring("other than 1"));
    CHECK_THAT(refused({{"op", "polar_array"}, {"entities", {1}}, {"count", 4}}), ContainsSubstring("needs pivot"));
    CHECK_THAT(refused({{"op", "array"}, {"entities", {1}}, {"dx", 1}, {"count", 1}}), ContainsSubstring("2 to 1000"));
    CHECK_THAT(refused({{"op", "array"}, {"entities", {1, 1}}, {"dx", 1}, {"count", 2}}), ContainsSubstring("listed twice"));
    CHECK_THAT(refused({{"op", "move"}, {"dx", 1}}), ContainsSubstring("nothing selected"));
    CHECK(ents.size() == n);
}

TEST_CASE("a transform keeps the constraints among the entities it moves together", "[Mcp][CAD]")
{
    using T = SketchConstraintType;
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_add", {{"rect", {-15, -10, 15, 10}}});
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
    json r = sketch(tool, "sketch_transform", {{"op", "move"}, {"entities", whole}, {"dx", 5}, {"dy", 3}});
    CHECK(r.at("released_constraints") == 0);
    CHECK(r.at("dof") == rect_dof);
    r = sketch(tool, "sketch_transform", {{"op", "scale"}, {"entities", whole}, {"scale", 2}});
    CHECK(r.at("released_constraints") == 0);
    CHECK(r.at("dof") == rect_dof);

    // Turned 30 degrees, its edges are neither horizontal nor vertical, but its corners still meet.
    r = sketch(tool, "sketch_transform", {{"op", "rotate"}, {"entities", whole}, {"angle_deg", 30}});
    CHECK(r.at("released_constraints") == 4);
    CHECK(count(T::Coincident) == 4);
    CHECK(count(T::Horizontal) == 0);
    CHECK(count(T::Vertical) == 0);
    CHECK(r.at("dof") == 4 * 4 - 4 * 2);   // four lines' ends, less two per joined corner

    // A corner pinned to the origin stays behind when the whole rectangle moves: only the pin goes.
    sketch(tool, "sketch_constrain", {{"type", "coincident"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", "origin"}}}}});
    REQUIRE(count(T::Coincident) == 5);
    r = sketch(tool, "sketch_transform", {{"op", "move"}, {"entities", whole}, {"dx", 2}});
    CHECK(r.at("released_constraints") == 1);
    CHECK(count(T::Coincident) == 4);

    // One edge moved on its own leaves its neighbours: the two corners it shares are released.
    r = sketch(tool, "sketch_transform", {{"op", "move"}, {"entities", {0}}, {"dx", 1}});
    CHECK(r.at("released_constraints") == 2);
    CHECK(count(T::Coincident) == 2);
}

TEST_CASE("a loop reports the area it encloses whichever way it runs", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    // A 10 mm square drawn clockwise (entities 0-3), and a slot, which the slot tool lays out
    // clockwise whichever way its centres are given (4-7).
    sketch(tool, "sketch_add", {{"entities",
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
    CHECK(sketch(tool, "sketch_constraints", json::object()).at("dof") == 0);   // nothing drawn, nothing free

    // A circle: its centre and its radius.
    json r = sketch(tool, "sketch_add", {{"type", "circle"}, {"center", {0, 0}}, {"radius", 5}});
    REQUIRE(tool.constraints().empty());
    CHECK(r.at("dof") == 3);
    CHECK(sketch(tool, "sketch_constraints", json::object()).at("dof") == 3);

    // An arc beside it: its centre and two ends, the ends held at one radius.
    r = sketch(tool, "sketch_add", {{"type", "arc"}, {"center", {20, 0}}, {"radius", 3}, {"start_angle_deg", 0}, {"end_angle_deg", 90}});
    REQUIRE(tool.constraints().empty());
    CHECK(r.at("dof") == 3 + 5);

    // A horizontal line brings the first constraint, and the solver's count agrees: the curves
    // keep their 8, and the line has 3 of its 4 left.
    r = sketch(tool, "sketch_add", {{"type", "line"}, {"p0", {0, 20}}, {"p1", {10, 20}}});
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

TEST_CASE("sketch_constrain solves with the new constraint or rolls it back, and sketch_unconstrain frees it", "[Mcp][CAD]")
{
    GUI::DesignSketchTool tool;
    begin_sketch(tool);
    sketch(tool, "sketch_add", {{"rect", {0, 0, 30, 20}}});
    const int free_dof = tool.dof();   // position, width and height
    CHECK(free_dof == 4);
    const size_t inferred = tool.constraints().size();

    const json width = {{"type", "distance"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", 0}, {"role", "p1"}}}}, {"value", 40}};
    json r = sketch(tool, "sketch_constrain", width);
    CHECK(r.at("added") == json::array({int(inferred)}));
    CHECK(r.at("items")[0].at("type") == "distance");
    CHECK(r.at("items")[0].at("value") == 40.0);
    CHECK(r.at("dof") == free_dof - 1);
    CHECK_THAT((tool.entities()[0].p1 - tool.entities()[0].p0).norm(), WithinAbs(40.0, 1e-6));

    // A rival width cannot be met: rolled back, and the sketch is as it was.
    json rival = width;
    rival["value"] = 25;
    CHECK_THAT(failure([&] { sketch(tool, "sketch_constrain", rival); }), ContainsSubstring("over-constrained"));
    CHECK(tool.constraints().size() == inferred + 1);
    CHECK(tool.dof() == free_dof - 1);

    // Pinning the corner to the origin through the CONSTRAIN bar's picks and refs.
    r = sketch(tool, "sketch_constrain", {{"type", "coincident"}, {"refs", {{{"entity", 0}, {"role", "p0"}}, {{"entity", "origin"}}}}});
    CHECK(r.at("dof") == free_dof - 3);
    CHECK(tool.entities()[0].p0.norm() < 1e-6);

    const json list = sketch(tool, "sketch_constraints", json::object());
    CHECK(list.at("scope") == "live");
    REQUIRE(list.at("items").size() == inferred + 2);
    CHECK(list.at("items").back().at("refs")[1] == json{{"entity", "origin"}});

    r = sketch(tool, "sketch_unconstrain", {{"index", int(inferred)}});   // the width
    CHECK(r.at("removed").at("type") == "distance");
    CHECK(r.at("constraints") == int(inferred + 1));
    CHECK(r.at("dof") == free_dof - 2);
    CHECK_THAT(failure([&] { sketch(tool, "sketch_unconstrain", {{"index", 99}}); }), ContainsSubstring("out of range"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_unconstrain", json::object()); }), ContainsSubstring("index must be"));
    CHECK_THAT(failure([&] { sketch(tool, "sketch_move", json::object()); }), ContainsSubstring("not a Design command of the open sketch"));
}

TEST_CASE("the rows for the sketch's shapes, edit ops and constraints", "[Mcp][CAD]")
{
    const std::vector<const char*> tools = {"sketch_add",       "sketch_fillet",    "sketch_chamfer",     "sketch_trim",
                                            "sketch_transform", "sketch_constrain", "sketch_constraints", "sketch_unconstrain"};
    const GUI::CommandCatalog catalog = cad_catalog();
    for (const char* t : tools) {
        INFO(t);
        CHECK(command_of(catalog, t).read_only == (std::string(t) == "sketch_constraints"));
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
             {"sketch_fillet", "radius"}, {"sketch_chamfer", "distance"}, {"sketch_trim", "at"}, {"sketch_transform", "op"},
             {"sketch_constrain", "type"}, {"sketch_unconstrain", "index"}}) {
        INFO(t << " " << k);
        CHECK(required(t, k));
    }
    for (const char* k : {"angle", "scale", "count", "pivot"}) {
        param("sketch_transform", k);
        CHECK_FALSE(required("sketch_transform", k));
    }
    param("sketch_constraints", "sketch");
    CHECK_FALSE(required("sketch_constraints", "sketch"));
    const json types = param("sketch_add", "type").at("enum");
    for (const char* t : {"polygon", "slot", "ellipse", "ellipse_arc", "spline"})
        CHECK(std::find(types.begin(), types.end(), json(t)) != types.end());
    // Every constraint type the recipe stores has its name in the enum.
    const json ctypes = param("sketch_constrain", "type").at("enum");
    CHECK(ctypes.size() == size_t(SketchConstraintType::SymmetricAboutX) + 2);   // and "equal"
}
