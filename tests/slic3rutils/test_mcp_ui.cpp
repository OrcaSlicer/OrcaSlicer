// UiInspector pulls in wx, which pulls in <windows.h>; same prologue as test_imgui_wrapper.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Ahead of the other project headers: the include checker credits the WXK_ key codes to the first
// header that brings in <wx/defs.h>.
#include "slic3r/GUI/SyntheticInput.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Mcp/CanvasTarget.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"
#include "slic3r/GUI/Mcp/UiRefs.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

struct FakeTarget
{
    std::string name;
    json        locator;
};

UiNode node(const std::string& ref, const std::string& kind, const std::string& label, int depth)
{
    UiNode n;
    n.ref   = ref;
    n.kind  = kind;
    n.label = label;
    n.depth = depth;
    return n;
}

} // namespace

TEST_CASE("clean_imgui_label keeps the shown part of an ImGui label", "[Mcp]")
{
    CHECK(clean_imgui_label("Move##gizmo") == "Move");
    CHECK(clean_imgui_label("###hidden") == "");
    CHECK(clean_imgui_label("Plain") == "Plain");
    CHECK(clean_imgui_label("\x01" "Colored\x02##x") == "Colored");
}

TEST_CASE("snapshot text shows depth, ref, kind, label, class and state", "[Mcp]")
{
    UiNode button = node("w2", "button", "Slice plate", 1);
    button.cls    = "Button";
    UiNode check  = node("w3", "checkbox", "Auto arrange", 1);
    check.cls     = "wxCheckBox";
    check.checked = 1;
    check.enabled = false;
    UiNode text   = node("w4", "text", "", 2);
    text.cls      = "TextInput";
    text.value    = "0.2";
    text.focused  = true;
    text.x = 10, text.y = 20, text.w = 80, text.h = 24;
    const std::string out = format_snapshot({node("w1", "window", "OrcaSlicer", 0), button, check, text}, 0);
    CHECK(out ==
          "[w1] window \"OrcaSlicer\"\n"
          "  [w2] button \"Slice plate\" (Button)\n"
          "  [w3] checkbox \"Auto arrange\" (wxCheckBox) [checked] [disabled]\n"
          "    [w4] text = \"0.2\" (TextInput) [focused] @10,20 80x24\n");
}

TEST_CASE("a truncated snapshot ends with a marker naming how much was left out", "[Mcp]")
{
    const std::string out = format_snapshot({node("w1", "window", "Main", 0)}, 37);
    CHECK(out.find("... 37 more nodes") != std::string::npos);
}

TEST_CASE("refs are never reused and resolve to their payload while alive", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i)
        CHECK(seen.insert(refs.add('w', 1, FakeTarget{"t" + std::to_string(i), {}}, [] { return true; })).second);
    std::string err;
    const std::string ref = refs.add('m', 1, FakeTarget{"menu", {{"menu", "File>Open"}}}, [] { return true; });
    CHECK(ref.front() == 'm');
    const FakeTarget* t = refs.resolve(ref, 1, err);
    REQUIRE(t != nullptr);
    CHECK(t->name == "menu");
    CHECK(t->locator == json{{"menu", "File>Open"}});
    CHECK(refs.resolve("w999999", 1, err) == nullptr);
    CHECK(err.find("Unknown ref") != std::string::npos);
}

TEST_CASE("a ref whose element is gone is stale", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    bool              alive = true;
    const std::string ref   = refs.add('w', 1, FakeTarget{"button", {}}, [&] { return alive; });
    std::string       err;
    CHECK(refs.resolve(ref, 1, err) != nullptr);
    alive = false;
    CHECK(refs.resolve(ref, 1, err) == nullptr);
    CHECK(err.find("Stale ref") != std::string::npos);
}

TEST_CASE("a ref from before a UI rebuild is stale", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    const std::string ref = refs.add('w', 3, FakeTarget{"button", {}}, [] { return true; });
    std::string       err;
    CHECK(refs.resolve(ref, 3, err) != nullptr);
    CHECK(refs.resolve(ref, 4, err) == nullptr);
    CHECK(err.find("Stale ref") != std::string::npos);
}

TEST_CASE("a menu ref is stale when its label path changed", "[Mcp]")
{
    // Menu refs remember the label path they were listed under; a rebuilt menu (another language,
    // another layout) with the same id under a different path no longer matches.
    RefTable<FakeTarget> refs;
    std::string current_path = "File>Export>Export G-code";
    const std::string taken = current_path;
    const std::string ref   = refs.add('m', 1, FakeTarget{"export", {{"menu", taken}}}, [&] { return current_path == taken; });
    std::string err;
    CHECK(refs.resolve(ref, 1, err) != nullptr);
    current_path = "Datei>Exportieren>G-Code exportieren";
    CHECK(refs.resolve(ref, 1, err) == nullptr);
}

TEST_CASE("the ref table forgets the oldest refs beyond its cap", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    const std::string first = refs.add('w', 1, FakeTarget{}, [] { return true; });
    for (std::size_t i = 0; i < RefTable<FakeTarget>::kMaxRefs; ++i)
        refs.add('w', 1, FakeTarget{}, [] { return true; });
    std::string err;
    CHECK(refs.resolve(first, 1, err) == nullptr);
    CHECK(err.find("Unknown ref") != std::string::npos);
}

TEST_CASE("synthetic key state is visible through get_key_state and cleared after a gesture", "[Mcp]")
{
    SyntheticInput::press_key(WXK_SHIFT);
    SyntheticInput::press_key(WXK_CONTROL);
    CHECK(SyntheticInput::is_key_down(WXK_SHIFT));
    CHECK(get_key_state(WXK_SHIFT));
#ifndef __APPLE__
    // Elsewhere Control and raw control are the same key.
    CHECK(SyntheticInput::is_key_down(WXK_RAW_CONTROL));
#endif
    SyntheticInput::release_key(WXK_SHIFT);
    CHECK_FALSE(SyntheticInput::is_key_down(WXK_SHIFT));
    CHECK(SyntheticInput::is_key_down(WXK_CONTROL));
    SyntheticInput::clear();
    CHECK_FALSE(SyntheticInput::is_key_down(WXK_CONTROL));
}

TEST_CASE("an ImGui label's text leaves out Orca's icon glyphs", "[Mcp]")
{
    CHECK(clean_imgui_label("\xe0\xa0\xa9##src") == "");        // U+0829, an icon alone
    CHECK(clean_imgui_label("\xe0\xa0\x90 Circle") == "Circle"); // U+0810 before text
    CHECK(clean_imgui_label("\xe0\xa1\x9b") == "");              // U+085B, the last icon
    CHECK(clean_imgui_label("\xe0\xa1\xa0") == "\xe0\xa1\xa0"); // U+0860 is text
}

TEST_CASE("an ImGui label's id part names its control", "[Mcp]")
{
    CHECK(imgui_label_name("##cursor_radius") == "cursor_radius");
    CHECK(imgui_label_name("Select##select_source_volume") == "select_source_volume");
    CHECK(imgui_label_name("N###extruder_color_N") == "extruder_color_N");
    CHECK(imgui_label_name("Move") == "");
    CHECK(imgui_label_name("Label##") == "");
}

TEST_CASE("an ImGui child window is named by its panel", "[Mcp]")
{
    CHECK(imgui_window_label("Move##gizmo") == "Move");
    CHECK(imgui_window_label("Texture displacement##gizmo/##td_body_1A2B3C4D") == "Texture displacement");
}

TEST_CASE("snapshot text shows an ImGui item's window and note", "[Mcp]")
{
    UiNode n;
    n.ref    = "g3";
    n.kind   = "imgui";
    n.label  = "Brush size";
    n.window = "Paint-on supports";
    n.note   = "clipped";
    CHECK(format_snapshot({n}, 0) == "[g3] imgui \"Brush size\" in \"Paint-on supports\" [clipped]\n");
}

namespace {

ImGuiWrapper::RecordedItem field(const std::string& label, const std::string& kind, float x, float y)
{
    ImGuiWrapper::RecordedItem item;
    item.label  = label;
    item.kind   = kind;
    item.window = "panel";
    item.x = x, item.y = y, item.w = 60.f, item.h = 20.f;
    return item;
}

ImGuiWrapper::RecordedText text(const std::string& s, float x, float y)
{
    ImGuiWrapper::RecordedText t;
    t.text   = s;
    t.window = "panel";
    t.x = x, t.y = y + 3.f, t.w = 70.f, t.h = 14.f;
    return t;
}

} // namespace

TEST_CASE("a field with no label of its own is listed by the text left of it on its row", "[Mcp]")
{
    const std::vector<ImGuiWrapper::RecordedItem> items{field("##cursor_radius", "slider", 100.f, 0.f),
                                                        field("##cursor_radius_input", "drag", 170.f, 0.f),
                                                        field("##height", "drag", 100.f, 40.f), field("Apply", "", 100.f, 80.f),
                                                        field("##reset", "", 170.f, 40.f)};
    const std::vector<ImGuiWrapper::RecordedText> texts{text("Brush size", 0.f, 0.f), text("Height", 0.f, 40.f)};
    const auto labels = imgui_display_labels(items, texts);
    CHECK(labels[0] == "Brush size");
    CHECK(labels[1] == ""); // the slider between them takes the row's label
    CHECK(labels[2] == "Height");
    CHECK(labels[3] == "Apply");
    CHECK(labels[4] == ""); // buttons are named, not labelled by their row
}

TEST_CASE("an item whose label shows its value is found again by its name", "[Mcp]")
{
    // Mesh Boolean's "Select##select_tool_volume" becomes "1 selected##select_tool_volume", a new id.
    std::vector<ImGuiWrapper::RecordedItem> items{field("1 selected##select_tool_volume", "", 0.f, 0.f), field("##other", "", 0.f, 30.f)};
    items[0].id = 7, items[1].id = 8;
    CHECK(imgui_find_item(items, 8, "other", "panel") == std::optional<std::size_t>(1));
    CHECK(imgui_find_item(items, 3, "select_tool_volume", "panel") == std::optional<std::size_t>(0));
    CHECK_FALSE(imgui_find_item(items, 3, "select_tool_volume", "another window").has_value());
    // Two with the name: neither is the one meant.
    items.push_back(field("Select##select_tool_volume", "", 0.f, 60.f));
    CHECK_FALSE(imgui_find_item(items, 3, "select_tool_volume", "panel").has_value());
}

TEST_CASE("an item one of several same-named ones is not found once their number changes", "[Mcp]")
{
    // Controls drawn without an ImGui item take their ids by order among the same-named ones: when the
    // first goes, the second takes its id.
    std::vector<ImGuiWrapper::RecordedItem> rows{field("", "button", 0.f, 0.f), field("", "button", 0.f, 30.f), field("", "button", 0.f, 60.f)};
    rows[0].id = 11, rows[1].id = 12, rows[2].id = 13;
    rows[0].name = rows[1].name = "Remove", rows[2].name = "Add";
    REQUIRE(imgui_twins(rows, "Remove", "panel") == 2);
    CHECK(imgui_find_item(rows, 12, "Remove", "panel", 2) == std::optional<std::size_t>(1));
    rows.erase(rows.begin());
    rows[0].id = 11;
    CHECK_FALSE(imgui_find_item(rows, 11, "Remove", "panel", 2).has_value());
    // An item whose name no other shares is found by its id whatever comes and goes around it, and so
    // is one with no name, however many unnamed ones there are.
    CHECK(imgui_find_item(rows, 13, "Add", "panel", 1) == std::optional<std::size_t>(1));
    rows.push_back(field("Done", "button", 0.f, 90.f));
    rows.push_back(field("Close", "button", 0.f, 120.f));
    rows[2].id = 14, rows[3].id = 15;
    CHECK(imgui_twins(rows, "", "panel") == 0);
    CHECK(imgui_find_item(rows, 14, "", "panel", imgui_twins(rows, "", "panel")) == std::optional<std::size_t>(2));
}

TEST_CASE("an item's name is the one it was given, else its label's id part", "[Mcp]")
{
    ImGuiWrapper::RecordedItem item = field("##cursor_radius", "slider", 0.f, 0.f);
    CHECK(imgui_item_name(item) == "cursor_radius");
    item.name = "Circle";
    CHECK(imgui_item_name(item) == "Circle");
}

TEST_CASE("a field's shown value matches a number at the precision it prints", "[Mcp]")
{
    CHECK(imgui_value_matches("5.00", json(5)));
    CHECK(imgui_value_matches("5.00 mm", json(5.004)));
    CHECK(imgui_value_matches("12", json("12")));
    CHECK_FALSE(imgui_value_matches("5.01", json(5)));
    CHECK_FALSE(imgui_value_matches("100", json(1000)));
    CHECK(imgui_value_matches("Hello\nworld", json("Hello\nworld")));
    CHECK_FALSE(imgui_value_matches("Hello", json("Hello ")));
}

namespace {

// An orthographic camera at the origin looking down -Z at a 100 x 100 mm field, into a
// 200 x 200 logical pixel canvas drawn at `scale` physical pixels per logical one.
CanvasView ortho_view(double scale)
{
    CanvasView view;
    view.view = Transform3d::Identity();
    Transform3d projection = Transform3d::Identity();
    projection.matrix().diagonal() << 1. / 50., 1. / 50., -1. / 100., 1.;
    view.projection = projection;
    view.viewport   = {0, 0, int(200 * scale), int(200 * scale)};
    view.scale      = scale;
    return view;
}

// A 10 mm cube centred on x = y = 0, from z = z0 - 10 up to z0.
Transform3d cube_at(double z0) { return Transform3d(Eigen::Translation3d(-5., -5., z0 - 10.)); }

} // namespace

TEST_CASE("a model point projects to the same logical pixel at every screen scale", "[Mcp]")
{
    const double scale = GENERATE(1., 2.);
    const auto   pixel = project_point(ortho_view(scale), Vec3d(25., 0., -40.));
    REQUIRE(pixel.has_value());
    CHECK_THAT(pixel->x(), Catch::Matchers::WithinAbs(150., 1e-6));
    CHECK_THAT(pixel->y(), Catch::Matchers::WithinAbs(100., 1e-6));
    CHECK_FALSE(project_point(ortho_view(scale), Vec3d(80., 0., -40.)).has_value());
    // Behind the camera's near plane, and beyond its far plane.
    CHECK_FALSE(project_point(ortho_view(scale), Vec3d(25., 0., 150.)).has_value());
    CHECK_FALSE(project_point(ortho_view(scale), Vec3d(25., 0., -150.)).has_value());

    const auto [origin, dir] = pixel_ray(ortho_view(scale), *pixel);
    CHECK_THAT(origin.x(), Catch::Matchers::WithinAbs(25., 1e-6));
    CHECK_THAT(dir.z(), Catch::Matchers::WithinAbs(-1., 1e-9));
}

TEST_CASE("a spot is where a ray along its axis first meets the part", "[Mcp]")
{
    const indexed_triangle_set mesh = its_make_cube(10., 10., 10.); // AABBMesh keeps a reference
    const AABBMesh             cube(mesh);
    const Transform3d          world = cube_at(-40.);
    const auto         top   = spot_point(cube, world, "top");
    const auto         front = spot_point(cube, world, "front");
    REQUIRE(top.has_value());
    CHECK_THAT(top->z(), Catch::Matchers::WithinAbs(-40., 1e-6));
    REQUIRE(front.has_value());
    CHECK_THAT(front->y(), Catch::Matchers::WithinAbs(-5., 1e-6));
    CHECK_FALSE(spot_point(cube, world, "inside").has_value());
}

TEST_CASE("the surface check passes a visible point and names what hides another", "[Mcp]")
{
    const indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    const AABBMesh             cube(mesh);
    const CanvasView           view = ortho_view(1.);
    const auto        ray_to = [&](const Vec3d& p) { return pixel_ray(view, *project_point(view, p)); };
    const std::vector<SurfaceMesh> alone{{&cube, cube_at(-40.), true, false}};

    CHECK(check_surface(Vec3d(0., 0., -40.), ray_to(Vec3d(0., 0., -40.)), alone, nullptr, {}, 0.1).empty());
    CHECK(check_surface(Vec3d(0., 0., -50.), ray_to(Vec3d(0., 0., -50.)), alone, nullptr, {}, 0.1) ==
          "a nearer surface of the same part is in front of the point");
    CHECK(check_surface(Vec3d(0., 0., -45.), ray_to(Vec3d(0., 0., -45.)), alone, nullptr, {}, 0.1) ==
          "the point is not on the part's surface");
    CHECK(check_surface(Vec3d(30., 0., -40.), ray_to(Vec3d(30., 0., -40.)), alone, nullptr, {}, 0.1) ==
          "the point is not on the part's surface");
    // On the shared edge of the top face's two triangles, where a ray can slip between them.
    CHECK(check_surface(Vec3d(2., 2., -40.), ray_to(Vec3d(2., 2., -40.)), alone, nullptr, {}, 0.1).empty());

    // Another part above it: in front, unless the caller leaves it out (hidden by the gizmo).
    const std::vector<SurfaceMesh> covered{{&cube, cube_at(-40.), true, false}, {&cube, cube_at(-20.), false, false}};
    CHECK(check_surface(Vec3d(0., 0., -40.), ray_to(Vec3d(0., 0., -40.)), covered, nullptr, {}, 0.1) ==
          "another part or object is in front of the point");

    // The bed: in front of a part looked at from below it, and the floor under a part sunk into it.
    // Slic3r:: because windows.h declares a global Polygon() as well.
    const Slic3r::Polygon          bed = Slic3r::Polygon::new_scale({Vec2d(-50., -50.), Vec2d(50., -50.), Vec2d(50., 50.), Vec2d(-50., 50.)});
    const std::vector<SurfaceMesh> raised{{&cube, cube_at(-1.).pretranslate(Vec3d(0., 0., 20.)), true, false}};
    const CanvasView               below = [&] {
        CanvasView v = ortho_view(1.);
        v.view       = Transform3d(Eigen::AngleAxisd(PI, Vec3d::UnitX())); // looking up from under the bed
        return v;
    }();
    const auto from_below = [&](const Vec3d& p) { return pixel_ray(below, *project_point(below, p)); };
    CHECK(check_surface(Vec3d(0., 0., 9.), from_below(Vec3d(0., 0., 9.)), raised, nullptr, {bed}, 0.1) == "the bed is in front of the point");
    const std::vector<SurfaceMesh> sunk{{&cube, cube_at(-1.), true, false}}; // its top 1 mm under the bed
    CHECK(check_surface(Vec3d(0., 0., -1.), ray_to(Vec3d(0., 0., -1.)), sunk, nullptr, {bed}, 0.1) == "the point is below the bed");
    // Plates are separate outlines: the gap between two is no bed.
    const std::vector<Slic3r::Polygon> plates{Slic3r::Polygon::new_scale({Vec2d(-50., -50.), Vec2d(-6., -50.), Vec2d(-6., 50.), Vec2d(-50., 50.)}),
                                              Slic3r::Polygon::new_scale({Vec2d(6., -50.), Vec2d(50., -50.), Vec2d(50., 50.), Vec2d(6., 50.)})};
    CHECK(check_surface(Vec3d(0., 0., 9.), from_below(Vec3d(0., 0., 9.)), raised, nullptr, plates, 0.1).empty());
    CHECK(check_surface(Vec3d(0., 0., -1.), ray_to(Vec3d(0., 0., -1.)), sunk, nullptr, plates, 0.1).empty());
    // Nor is the part of a round plate's bounding box outside the circle.
    std::vector<Vec2d> circle;
    for (int i = 0; i < 64; ++i)
        circle.emplace_back(40. + 45. * std::cos(2. * PI * i / 64.), 40. + 45. * std::sin(2. * PI * i / 64.));
    CHECK(check_surface(Vec3d(0., 0., -1.), ray_to(Vec3d(0., 0., -1.)), sunk, nullptr, {Slic3r::Polygon::new_scale(circle)}, 0.1).empty());
    CHECK(check_surface(Vec3d(0., 0., 9.), from_below(Vec3d(0., 0., 9.)), raised, nullptr, {Slic3r::Polygon::new_scale(circle)}, 0.1).empty());
}

TEST_CASE("the surface check accepts a click whose pixel meets a slope nearer than the point", "[Mcp]")
{
    // A 20 mm cube turned 45 degrees about y, so the face seen from above is the slope z = x - 40. A
    // pixel is half a millimetre: the point x = -0.2 projects to pixel 99.6, and the click goes to
    // pixel 100, whose ray meets the slope at x = 0, 0.2 mm nearer the camera and 0.28 mm away.
    const indexed_triangle_set its = its_make_cube(20., 20., 20.);
    const AABBMesh             cube(its);
    const Transform3d          world = Eigen::Translation3d(5., 0., -49.142135623730951) * Eigen::AngleAxisd(-PI / 4., Vec3d::UnitY()) *
                              Eigen::Translation3d(-10., -10., -10.);
    const CanvasView               view = ortho_view(GENERATE(1., 2.));
    const std::vector<SurfaceMesh> slope{{&cube, world, true, false, "S"}};
    const Vec3d                    point(-0.2, -0.5, -40.2);
    const Vec2d                    pixel = *project_point(view, point);
    CHECK_THAT(pixel.x(), Catch::Matchers::WithinAbs(99.6, 1e-6));
    CHECK(check_surface(point, pixel_ray(view, Vec2d(std::round(pixel.x()), std::round(pixel.y()))), slope, nullptr, {}, 0.1).empty());
    // The face below it, z = -x - 58.28, stays hidden behind the slope.
    CHECK(check_surface(Vec3d(-0.2, -0.5, -58.084), pixel_ray(view, Vec2d(100., 101.)), slope, nullptr, {}, 0.1) ==
          "a nearer surface of the same part is in front of the point");
    // So does the underside of a 0.2 mm plate, though the click lands on its top within the slack.
    const indexed_triangle_set     plate_its = its_make_cube(10., 10., 0.2);
    const AABBMesh                 plate(plate_its);
    const std::vector<SurfaceMesh> thin{{&plate, Transform3d(Eigen::Translation3d(-5., -5., -40.2)), true, false, "P"}};
    CHECK(check_surface(Vec3d(-0.2, -0.5, -40.2), pixel_ray(view, Vec2d(100., 101.)), thin, nullptr, {}, 0.1) ==
          "a nearer surface of the same part is in front of the point");
    CHECK(check_surface(Vec3d(-0.2, -0.5, -40.), pixel_ray(view, Vec2d(100., 101.)), thin, nullptr, {}, 0.1).empty());
    // And the top of a slab under another one 0.2 mm above it, both in one mesh.
    indexed_triangle_set slabs = its_make_cube(10., 10., 0.05), lower = its_make_cube(10., 10., 0.05);
    its_translate(slabs, Vec3f(-5.f, -5.f, -40.05f));
    its_translate(lower, Vec3f(-5.f, -5.f, -40.25f));
    its_merge(slabs, lower);
    const AABBMesh                 stacked(slabs);
    const std::vector<SurfaceMesh> two{{&stacked, Transform3d::Identity(), true, false, "S"}};
    CHECK(check_surface(Vec3d(-0.2, -0.5, -40.2), pixel_ray(view, Vec2d(100., 101.)), two, nullptr, {}, 0.1) ==
          "a nearer surface of the same part is in front of the point");

    // Past a convex edge: the slope's ridge, moved to x = 5.35. The point x = 5.3 projects to pixel
    // 110.6, and the click at pixel 111 meets the face beyond the ridge, 0.07 mm off the point's plane.
    const Transform3d ridged = Eigen::Translation3d(5.35, 0., -48.792135623730951) * Eigen::AngleAxisd(-PI / 4., Vec3d::UnitY()) *
                               Eigen::Translation3d(-10., -10., -10.);
    const std::vector<SurfaceMesh> edge{{&cube, ridged, true, false, "S"}};
    const Vec3d                    near_edge(5.3, -0.5, -34.7);
    const Vec2d                    edge_pixel = *project_point(view, near_edge);
    const auto                     edge_ray   = pixel_ray(view, Vec2d(std::round(edge_pixel.x()), std::round(edge_pixel.y())));
    CHECK(check_surface(near_edge, edge_ray, edge, nullptr, {}, 0.05) == "the click at the point's pixel lands on another face of the part");
    // Within a looser tolerance the point is on that face's plane too: at the edge, as far as it can tell.
    CHECK(check_surface(near_edge, edge_ray, edge, nullptr, {}, 0.1).empty());
}

TEST_CASE("the surface check sees through what the clipping plane cuts away", "[Mcp]")
{
    const indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    const AABBMesh             cube(mesh);
    const CanvasView           view   = ortho_view(1.);
    const auto       ray_to = [&](const Vec3d& p) { return pixel_ray(view, *project_point(view, p)); };
    // Cuts away z > -45, the top half of the cube.
    const ClippingPlane              clip(Vec3d(0., 0., 1.), -45.);
    const std::vector<SurfaceMesh>   meshes{{&cube, cube_at(-40.), true, true}};
    CHECK(check_surface(Vec3d(0., 0., -40.), ray_to(Vec3d(0., 0., -40.)), meshes, &clip, {}, 0.1) ==
          "the point is on the side of the clipping plane that is cut away");
    CHECK(check_surface(Vec3d(0., 0., -50.), ray_to(Vec3d(0., 0., -50.)), meshes, &clip, {}, 0.1).empty());
    // On the cut itself: a click can land on either side.
    CHECK(check_surface(Vec3d(0., -5., -45.), ray_to(Vec3d(0., -5., -45.)), meshes, &clip, {}, 0.1) ==
          "the point is on the side of the clipping plane that is cut away");
}

TEST_CASE("the surface check judges the ray through the whole pixel a click goes to", "[Mcp]")
{
    // Two parts side by side on the top: A over x in [-5, -0.25], B over x in [-0.25, 5]. A pixel is
    // half a millimetre here, so the point x = -0.4 on A projects to pixel 99.2, and the click goes
    // to pixel 99, whose ray is at x = -0.5, still on A; at x = -0.3 it goes to pixel 99.4 -> 99 too.
    const indexed_triangle_set a_mesh = its_make_cube(4.75, 10., 10.), b_mesh = its_make_cube(5.25, 10., 10.);
    const AABBMesh             a(a_mesh), b(b_mesh);
    const Transform3d          a_world(Eigen::Translation3d(-5., -5., -50.)), b_world(Eigen::Translation3d(-0.25, -5., -50.));
    const CanvasView           view = ortho_view(GENERATE(1., 2.));
    const auto                 clicked_ray = [&](const Vec3d& p) {
        const Vec2d pixel = *project_point(view, p);
        return pixel_ray(view, Vec2d(std::round(pixel.x()), std::round(pixel.y())));
    };
    const std::vector<SurfaceMesh> meshes{{&a, a_world, true, false, "A"}, {&b, b_world, false, false, "B"}};
    CHECK(check_surface(Vec3d(-0.4, 0., -40.), clicked_ray(Vec3d(-0.4, 0., -40.)), meshes, nullptr, {}, 0.1).empty());
    // B as the target: its point x = -0.2 projects to pixel 99.6, and the click goes to pixel 100,
    // x = 0, on B. Its point x = -0.15 near A's edge from the other side: pixel 99.7 -> 100, on B.
    const std::vector<SurfaceMesh> b_target{{&a, a_world, false, false, "A"}, {&b, b_world, true, false, "B"}};
    CHECK(check_surface(Vec3d(-0.2, 0., -40.), clicked_ray(Vec3d(-0.2, 0., -40.)), b_target, nullptr, {}, 0.1).empty());
    // A ray a whole pixel from the point, across the seam, lands on A instead.
    const auto across = pixel_ray(view, Vec2d(99., 100.));
    CHECK(check_surface(Vec3d(-0.2, 0., -40.), across, b_target, nullptr, {}, 0.1) ==
          "the click at the point's pixel lands on A beside the part");
    // Past the part's edge, the ray misses it.
    const std::vector<SurfaceMesh> a_alone{{&a, a_world, true, false, "A"}};
    CHECK(check_surface(Vec3d(-0.3, 0., -40.), pixel_ray(view, Vec2d(100., 100.)), a_alone, nullptr, {}, 0.1) ==
          "the click at the point's pixel misses the part");
}
