#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Actions/PlatesPreviewLogic.hpp"

using namespace Slic3r;
namespace PP = Slic3r::GUI::Actions::PlatesPreview;
using nlohmann::json;
using Catch::Approx;

TEST_CASE("bed type ids round-trip and accept config names", "[actions][plates]")
{
    for (const std::string& id : PP::bed_type_ids()) {
        auto t = PP::bed_type_from_id(id);
        REQUIRE(t.has_value());
        CHECK(PP::bed_type_id(*t) == id);
    }
    CHECK(PP::bed_type_from_id("Textured PEI Plate") == btPTE);
    CHECK(PP::bed_type_from_id("texturedPEI") == btPTE);
    CHECK_FALSE(PP::bed_type_from_id("glass").has_value());
}

TEST_CASE("print sequence ids", "[actions][plates]")
{
    CHECK(PP::print_sequence_from_id("byObject") == PrintSequence::ByObject);
    CHECK(PP::print_sequence_from_id("default") == PrintSequence::ByDefault);
    CHECK(PP::print_sequence_from_id("byGroup") == PrintSequence::ByGroup);
    CHECK_FALSE(PP::print_sequence_from_id("byPlane").has_value());
    CHECK(PP::print_sequence_id(PrintSequence::ByLayer) == "byLayer");
    CHECK(PP::print_sequence_id(PrintSequence::ByGroup) == "byGroup");
}

TEST_CASE("plate skirt start angle range", "[actions][plates]")
{
    CHECK(PP::valid_skirt_start_angle(-135.));
    CHECK(PP::valid_skirt_start_angle(180.));
    CHECK_FALSE(PP::valid_skirt_start_angle(181.));
    CHECK_FALSE(PP::valid_skirt_start_angle(std::nan("")));
}

TEST_CASE("filament grouping modes and manual maps", "[actions][plates]")
{
    for (const char* id : {"default", "autoForFlush", "autoForMatch", "manual"}) {
        auto m = PP::filament_map_mode_from_id(id);
        REQUIRE(m.has_value());
        CHECK(PP::filament_map_mode_id(*m) == id);
    }
    CHECK_FALSE(PP::filament_map_mode_from_id("nozzleManual").has_value());
    std::string why;
    CHECK(PP::valid_filament_maps({1, 2, 2}, 3, 2, why));
    CHECK_FALSE(PP::valid_filament_maps({1, 2}, 3, 2, why));
    CHECK_FALSE(PP::valid_filament_maps({1, 3, 2}, 3, 2, why));
}

TEST_CASE("plate names and filament orders", "[actions][plates]")
{
    std::string why;
    CHECK(PP::valid_plate_name("Second copy", why));
    CHECK(PP::valid_plate_name("", why));
    CHECK_FALSE(PP::valid_plate_name("bad\nname", why));
    CHECK_FALSE(PP::valid_plate_name(std::string(121, 'a'), why));

    CHECK(PP::valid_filament_order({2, 1, 3}, 3, why));
    CHECK_FALSE(PP::valid_filament_order({1, 1}, 3, why));
    CHECK_FALSE(PP::valid_filament_order({4}, 3, why));
}

TEST_CASE("layer event parsing", "[actions][layers]")
{
    PP::LayerEvent ev;
    std::string    why;
    CHECK(PP::parse_layer_event(json{{"type", "pause"}, {"z", 15.0}}, ev, why));
    CHECK(ev.type == CustomGCode::PausePrint);
    CHECK(ev.z == 15.0);

    CHECK(PP::parse_layer_event(json{{"type", "toolChange"}, {"layer", 10}, {"filament", 2}}, ev, why));
    CHECK(ev.layer == 10);

    // exactly one of z / layer
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "pause"}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "pause"}, {"z", 1.0}, {"layer", 2}}, ev, why));
    // fields that belong to other types
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "pause"}, {"z", 1.0}, {"filament", 2}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "toolChange"}, {"z", 1.0}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "custom"}, {"z", 1.0}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "pause"}, {"z", 1.0}, {"gcode", "M117"}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "colorChange"}, {"z", 1.0}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "toolChange"}, {"z", 1.0}, {"filament", 2}, {"color", "red"}}, ev, why));
    CHECK(PP::parse_layer_event(json{{"type", "toolChange"}, {"z", 1.0}, {"filament", 2}, {"color", "#FF0000"}}, ev, why));
    CHECK_FALSE(PP::parse_layer_event(json{{"type", "pause"}, {"z", -1.0}}, ev, why));
}

TEST_CASE("custom G-code lines for the confirmation", "[actions][layers]")
{
    std::vector<std::string> lines;
    std::string              why;
    REQUIRE(PP::custom_gcode_lines("M117 Hello\r\nG4 S5\n\n", lines, why));
    CHECK(lines == std::vector<std::string>{"M117 Hello", "G4 S5"});
    CHECK_FALSE(PP::custom_gcode_lines("", lines, why));
    CHECK_FALSE(PP::custom_gcode_lines("\n\n", lines, why));
    CHECK_FALSE(PP::custom_gcode_lines(std::string("M117\x01"), lines, why));
    CHECK_FALSE(PP::custom_gcode_lines(std::string(4097, 'G'), lines, why));
    std::string many;
    for (int i = 0; i < 65; ++i)
        many += "G4 S0\n";
    CHECK_FALSE(PP::custom_gcode_lines(many, lines, why));
}

TEST_CASE("event z resolves against the sliced layers", "[actions][layers]")
{
    const std::vector<double> zs = {0.2, 0.4, 0.6, 0.8, 1.0};
    PP::LayerEvent            ev;
    double                    z     = 0.;
    int                       layer = 0;
    std::string               why;

    ev.z = 0.55;
    REQUIRE(PP::resolve_event_z(ev, zs, z, layer, why));
    CHECK(z == Approx(0.6));
    CHECK(layer == 3);

    ev.z = 0.05; // below the first layer snaps to it
    REQUIRE(PP::resolve_event_z(ev, zs, z, layer, why));
    CHECK(layer == 1);

    ev.z = 5.0; // above the top layer
    CHECK_FALSE(PP::resolve_event_z(ev, zs, z, layer, why));

    ev.z.reset();
    ev.layer = 5;
    REQUIRE(PP::resolve_event_z(ev, zs, z, layer, why));
    CHECK(z == Approx(1.0));
    ev.layer = 6;
    CHECK_FALSE(PP::resolve_event_z(ev, zs, z, layer, why));

    // unsliced: z kept, layer numbers refused
    ev.layer.reset();
    ev.z = 15.0;
    REQUIRE(PP::resolve_event_z(ev, {}, z, layer, why));
    CHECK(z == 15.0);
    CHECK(layer == 0);
    ev.z.reset();
    ev.layer = 3;
    CHECK_FALSE(PP::resolve_event_z(ev, {}, z, layer, why));
}

TEST_CASE("slider mode follows the plate's filaments", "[actions][layers]")
{
    CHECK(PP::slider_mode(1, {1}) == CustomGCode::SingleExtruder);
    CHECK(PP::slider_mode(3, {}) == CustomGCode::SingleExtruder);
    CHECK(PP::slider_mode(3, {2}) == CustomGCode::MultiAsSingle);
}

static CustomGCode::Item item(double z, CustomGCode::Type t, int ex = 1, std::string extra = {})
{
    CustomGCode::Item i;
    i.print_z  = z;
    i.type     = t;
    i.extruder = ex;
    i.extra    = std::move(extra);
    return i;
}

TEST_CASE("merging layer events", "[actions][layers]")
{
    CustomGCode::Info existing;
    existing.mode   = CustomGCode::SingleExtruder;
    existing.gcodes = {item(1.0, CustomGCode::PausePrint), item(3.0, CustomGCode::Custom, 1, "M117 a")};

    SECTION("add replaces the same layer and keeps the rest, sorted")
    {
        auto out = PP::merge_layer_events(existing, {item(3.0, CustomGCode::PausePrint), item(2.0, CustomGCode::PausePrint)}, false,
                                          CustomGCode::SingleExtruder);
        REQUIRE(out.gcodes.size() == 3);
        CHECK(out.gcodes[0].print_z == 1.0);
        CHECK(out.gcodes[1].print_z == 2.0);
        CHECK(out.gcodes[2].print_z == 3.0);
        CHECK(out.gcodes[2].type == CustomGCode::PausePrint);
    }
    SECTION("replace drops everything else")
    {
        auto out = PP::merge_layer_events(existing, {item(2.0, CustomGCode::PausePrint)}, true, CustomGCode::MultiAsSingle);
        REQUIRE(out.gcodes.size() == 1);
        CHECK(out.mode == CustomGCode::MultiAsSingle);
    }
    SECTION("replace with nothing clears")
    {
        auto out = PP::merge_layer_events(existing, {}, true, CustomGCode::SingleExtruder);
        CHECK(out.gcodes.empty());
    }
    SECTION("remove by z")
    {
        CustomGCode::Info info = existing;
        CHECK(PP::remove_layer_events(info, {3.00001, 7.0}) == 1);
        CHECK(info.gcodes.size() == 1);
    }
}

TEST_CASE("layer events serialise with their layer number", "[actions][layers]")
{
    CustomGCode::Info info;
    info.gcodes = {item(0.6, CustomGCode::PausePrint), item(0.7, CustomGCode::Custom, 1, "M117 x"),
                   item(0.8, CustomGCode::ToolChange, 2)};
    const json j = PP::layer_events_json(info, {0.2, 0.4, 0.6, 0.8});
    REQUIRE(j.size() == 3);
    CHECK(j[0]["type"] == "pause");
    CHECK(j[0]["layer"] == 3);
    CHECK(j[1]["layer"].is_null()); // between layers
    CHECK(j[1]["gcode"] == "M117 x");
    CHECK(j[2]["type"] == "toolChange");
    CHECK(j[2]["filament"] == 2);
}

TEST_CASE("move summary per feature and per layer", "[actions][preview]")
{
    using MV = GCodeProcessorResult::MoveVertex;
    std::vector<MV> moves(5);
    moves[0].type     = EMoveType::Travel;
    moves[0].position = Vec3f(0, 0, 0.2f);
    moves[1].type     = EMoveType::Travel;
    moves[1].position = Vec3f(10, 0, 0.2f);
    moves[1].time[0]  = 1.f;
    moves[2].type           = EMoveType::Extrude;
    moves[2].extrusion_role = erExternalPerimeter;
    moves[2].position       = Vec3f(20, 0, 0.2f);
    moves[2].print_z        = 0.2f;
    moves[2].time[0]        = 2.f;
    moves[2].delta_extruder = 0.5f;
    moves[2].mm3_per_mm     = 0.1f;
    moves[3].type     = EMoveType::Pause_Print;
    moves[3].position = Vec3f(20, 0, 0.2f);
    moves[3].layer_id = 1;
    moves[4].type           = EMoveType::Extrude;
    moves[4].extrusion_role = erInternalInfill;
    moves[4].position       = Vec3f(20, 5, 0.4f);
    moves[4].print_z        = 0.4f;
    moves[4].layer_id       = 1;
    moves[4].time[0]        = 4.f;

    const PP::MoveSummary s = PP::summarize_moves(moves);
    CHECK(s.total_time_s == Approx(7.));
    CHECK(s.travel_time_s == Approx(1.));
    CHECK(s.travel_mm == Approx(10.));
    CHECK(s.pauses == 1);
    REQUIRE(s.roles.count("outerWall") == 1);
    CHECK(s.roles.at("outerWall").time_s == Approx(2.));
    CHECK(s.roles.at("outerWall").volume_mm3 == Approx(1.));
    REQUIRE(s.layers.size() == 2);
    CHECK(s.layers[0].first == Approx(0.2));
    CHECK(s.layers[0].second == Approx(3.));
    CHECK(s.layers[1].first == Approx(0.4));

    const json j = PP::move_summary_json(s, true);
    CHECK(j["features"]["sparseInfill"]["timeS"] == Approx(4.));
    CHECK(j["layers"].size() == 2);
    CHECK(j["slowestLayer"]["layer"] == 2);
    CHECK(j["events"]["pauses"] == 1);
}

TEST_CASE("preview layers follow the layer slider's numbering", "[actions][preview]")
{
    using MV = GCodeProcessorResult::MoveVertex;
    // ids 0, 0, 2, 2, 5: the file skips ids 1, 3 and 4. The slider shows three layers, not six.
    std::vector<MV> moves(6);
    const unsigned ids[]  = {0, 0, 2, 2, 5, 5};
    const float    zs[]   = {0.f, 0.2f, 0.4f, 0.4f, 0.6f, 0.6f};
    for (size_t i = 0; i < moves.size(); ++i) {
        moves[i].type           = EMoveType::Extrude;
        moves[i].extrusion_role = erPerimeter;
        moves[i].layer_id       = ids[i];
        moves[i].position       = Vec3f(float(i), 0.f, zs[i]);
        moves[i].time[0]        = 1.f;
        moves[i].time[1]        = 2.f;
    }
    moves[5].extrusion_role = erCustom; // custom extrusion does not set the layer's z
    const auto layers = PP::preview_layers(moves);
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].z == Approx(0.2));
    CHECK(layers[0].time_s[0] == Approx(1.)); // moves[0] is not a preview vertex
    CHECK(layers[1].z == Approx(0.4));
    CHECK(layers[1].time_s[1] == Approx(4.));
    CHECK(layers[2].z == Approx(0.6));
    CHECK(PP::preview_layer_zs(layers).size() == 3);

    // No zero-time phantom layers for the skipped ids.
    const PP::MoveSummary s = PP::summarize_moves(moves, 1);
    REQUIRE(s.layers.size() == 3);
    const json j = PP::move_summary_json(s, true);
    CHECK(j["fastestLayer"]["timeS"] == Approx(2.));
    CHECK(j["layerCount"] == 3);

    // A tilted print's layer zs can step back down: nearest still finds the closest one.
    CHECK(PP::nearest_layer_index({0.2, 0.4, 0.3, 0.8}, 0.31) == 2);
    CHECK(PP::nearest_layer_index({0.2, 0.4, 0.6}, 0.45) == 1);
}

TEST_CASE("move summary option rows, colour ranges and filament units", "[actions][preview]")
{
    using MV = GCodeProcessorResult::MoveVertex;
    std::vector<MV> moves(6);
    moves[1].type           = EMoveType::Retract;
    moves[1].delta_extruder = -0.8f;
    moves[1].time[0]        = 0.5f;
    moves[2].type           = EMoveType::Unretract;
    moves[2].delta_extruder = 0.8f;
    moves[3].type           = EMoveType::Wipe;
    moves[3].travel_dist    = 2.f;
    moves[4].type           = EMoveType::Color_change;
    moves[5].type           = EMoveType::Extrude;
    moves[5].extrusion_role = erExternalPerimeter;
    moves[5].feedrate       = 50.f;
    moves[5].mm3_per_mm     = 0.1f;
    moves[5].height         = 0.2f;
    moves[5].width          = 0.45f;
    moves[5].temperature    = 210.f;
    const PP::MoveSummary s = PP::summarize_moves(moves);
    CHECK(s.retract.count == 1);
    CHECK(s.retract.length_mm == Approx(0.8));
    CHECK(s.retract.time_s == Approx(0.5));
    CHECK(s.wipe.length_mm == Approx(2.));
    CHECK(s.color_changes == 1);
    const json j = PP::move_summary_json(s, false);
    CHECK(j["events"]["colorChanges"] == 1);
    CHECK(j["options"]["unretract"]["filamentMm"] == Approx(0.8));

    const json r = PP::color_ranges_json(moves);
    CHECK(r["speed"]["max"] == Approx(50.));
    CHECK(r["flow"]["max"] == Approx(5.));
    CHECK(r["temperature"]["min"] == Approx(210.));

    // 1000 mm3 of 1.75 mm PLA at 1.24 g/cm3 and 20/kg
    const PP::FilamentUse u = PP::filament_use(1000., 1.75, 1.24, 20.);
    CHECK(u.m == Approx(0.4158).epsilon(0.001));
    CHECK(u.g == Approx(1.24));
    CHECK(u.cost == Approx(0.0248));

    PrintEstimatedStatistics a, b;
    a.total_volumes_per_extruder[0] = 1000.;
    a.model_volumes_per_extruder[0] = 900.;
    b.total_volumes_per_extruder[0] = 500.;
    b.total_volumes_per_extruder[1] = 100.;
    const json all = PP::all_plates_json({{0, &a, 60., 1.}, {1, &b, 30., 0.5}}, {1.75f, 1.75f}, {1.24f, 1.24f}, {20.f, 20.f});
    CHECK(all["timeS"] == Approx(90.));
    CHECK(all["cost"] == Approx(1.5));
    REQUIRE(all["filaments"].size() == 2);
    CHECK(all["filaments"][0]["total"]["mm3"] == Approx(1500.));
    CHECK(all["filaments"][0]["model"]["g"] == Approx(1.116));
    CHECK(all["filaments"][1]["filament"] == 2);
}

TEST_CASE("feature and option ids", "[actions][preview]")
{
    for (const std::string& id : PP::extrusion_role_ids()) {
        INFO(id);
        const auto role = PP::extrusion_role_from_id(id);
        REQUIRE(role.has_value());
        CHECK(std::string(PP::extrusion_role_id(*role)) == id);
    }
    CHECK_FALSE(PP::extrusion_role_from_id("mixed").has_value());
    CHECK(PP::preview_option_ids().size() == 6);
}

TEST_CASE("G-code lines around a move", "[actions][preview]")
{
    CHECK(PP::gcode_line_span(100, 50, 5, 5) == std::array<size_t, 2>{45, 55});
    CHECK(PP::gcode_line_span(100, 2, 5, 5) == std::array<size_t, 2>{1, 7});
    CHECK(PP::gcode_line_span(100, 99, 5, 5) == std::array<size_t, 2>{94, 100});
    CHECK(PP::gcode_line_span(100, 0, 5, 5) == std::array<size_t, 2>{0, 0});
    // "G1 X1\nG1 X2\n" -> ends {6, 12}
    const std::vector<size_t> ends = {6, 12};
    CHECK(PP::gcode_line_bytes(ends, 1) == std::array<size_t, 2>{0, 6});
    CHECK(PP::gcode_line_bytes(ends, 2) == std::array<size_t, 2>{6, 6});
    CHECK(PP::clip_gcode_line("G1 X1\r\n") == "G1 X1");
    const std::string clipped = PP::clip_gcode_line("; \xC3\xA9\xC3\xA9", 3); // cut inside the first é
    CHECK(clipped == "; ...");
}

TEST_CASE("capture sizing and downscale", "[actions][preview]")
{
    CHECK(PP::fit_capture_size(2048, 1024) == std::array<unsigned, 2>{1024, 512});
    CHECK(PP::fit_capture_size(800, 600) == std::array<unsigned, 2>{800, 600});
    CHECK(PP::fit_capture_size(800, 600, 400) == std::array<unsigned, 2>{400, 300});

    // 2x2 -> 1x1 averages
    std::vector<unsigned char> src = {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    auto dst = PP::downscale_rgba(src, 2, 2, 1, 1);
    REQUIRE(dst.size() == 4);
    CHECK(int(dst[0]) == 127);
    CHECK(int(dst[3]) == 255);
}

// ---------------------------------------------------------------------------------------------
// The S5 descriptors themselves: Catalog::add rejects a bad id, schema, effect or example, so
// registering into a fresh catalog checks every one of them.
// ---------------------------------------------------------------------------------------------

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/PlatesPreviewDescriptors.hpp"

TEST_CASE("plate, layer-event and preview descriptors register cleanly", "[actions][plates]")
{
    namespace A = Slic3r::GUI::Actions;
    A::Catalog c;
    std::map<std::string, PP::Binding> dummy;
    auto runner = [](A::InvokeContext&) { return A::InvokeResult::ok(); };
    for (auto descs : {PP::plate_descriptors(), PP::layer_event_descriptors(), PP::preview_descriptors()})
        for (const A::ActionDescriptor& d : descs)
            dummy[d.id] = {runner, {}};
    REQUIRE_NOTHROW(PP::add_bound(c, PP::plate_descriptors(), dummy));
    REQUIRE_NOTHROW(PP::add_bound(c, PP::layer_event_descriptors(), dummy));
    REQUIRE_NOTHROW(PP::add_bound(c, PP::preview_descriptors(), dummy));
    CHECK(c.size() == 22);
    // A descriptor without a runner is caught at registration.
    A::Catalog c2;
    CHECK_THROWS(PP::add_bound(c2, PP::preview_descriptors(), {}));

    for (const char* id : {"plate.list", "plate.add", "plate.delete", "plate.duplicate", "plate.select", "plate.rename", "plate.lock",
                           "plate.setSettings", "plate.move", "plate.setWipeTower", "plate.selectAll", "plate.reloadAll", "plate.autoOrient",
                           "object.autoOrient", "object.toPlate", "slice.all", "layer.getEvents", "layer.setEvents",
                           "preview.getStats", "preview.setView", "preview.getMove", "view.capture"}) {
        INFO(id);
        const A::ActionEntry* e = c.find(id);
        REQUIRE(e != nullptr);
        CHECK(e->runner);
        CHECK_FALSE(e->descriptor.examples.empty());
        // Nothing here is destructive or outward: every edit is undoable or a view change, and the
        // one confirmation (custom G-code) is asked by the runner itself.
        CHECK(e->descriptor.confirm == A::Confirm::None);
    }

    // The schema refuses what the runners would refuse anyway.
    const json& set_events = c.find("layer.setEvents")->descriptor.params;
    A::SchemaError err;
    CHECK_FALSE(A::validate(set_events, json{{"items", json::array({{{"type", "pause"}}})}}, err));
    CHECK_FALSE(A::validate(set_events, json{{"items", json::array({{{"type", "pause"}, {"z", 1.0}, {"layer", 3}}})}}, err));
    CHECK(A::validate(set_events, json{{"items", json::array({{{"type", "pause"}, {"layer", 3}}})}}, err));
    CHECK_FALSE(A::validate(set_events, json{{"items", json::array({{{"type", "colorChange"}, {"z", 1.0}}})}}, err));

    const json& set_view = c.find("preview.setView")->descriptor.params;
    CHECK(A::validate(set_view, json{{"move", {{"to", "end"}}}}, err));
    CHECK(A::validate(set_view, json{{"move", {{"index", 12}}}, {"options", {{"travel", true}}}, {"features", {{"support", false}}}}, err));
    CHECK_FALSE(A::validate(set_view, json{{"move", {{"index", 12}, {"to", "end"}}}}, err));
    CHECK_FALSE(A::validate(set_view, json{{"move", 12}}, err));
    CHECK_FALSE(A::validate(set_view, json{{"features", {{"mixed", false}}}}, err));
    CHECK_FALSE(A::validate(set_view, json{{"viewType", "tool"}}, err)); // the legend's combo never lists it
    CHECK(A::validate(set_view, json{{"viewType", "section"}, {"sections", json::array({1, 3})}}, err));

    const json& to_plate = c.find("object.toPlate")->descriptor.params;
    CHECK_FALSE(A::validate(to_plate, json::object(), err));
    CHECK_FALSE(A::validate(to_plate, json{{"plate", 1}, {"newPlate", true}}, err));
    CHECK(A::validate(to_plate, json{{"plate", 1}}, err));

    const json& plate_settings = c.find("plate.setSettings")->descriptor.params;
    CHECK(A::validate(plate_settings, json{{"printSequence", "byGroup"}}, err));
    CHECK(A::validate(plate_settings, json{{"skirtStartAngle", -45}}, err));
    CHECK_FALSE(A::validate(plate_settings, json{{"skirtStartAngle", 200}}, err));
    CHECK(A::validate(plate_settings, json{{"resetSkirtStartAngle", true}}, err));
    CHECK_FALSE(A::validate(c.find("plate.move")->descriptor.params, json::object(), err));
    CHECK_FALSE(A::validate(c.find("plate.setWipeTower")->descriptor.params, json{{"x", 10}}, err));

    const json& capture = c.find("view.capture")->descriptor.params;
    CHECK_FALSE(A::validate(capture, json{{"width", 2048}}, err));
    CHECK_FALSE(A::validate(capture, json{{"camera", "sideways"}}, err));
}
