#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libvgcode/include/ColorRange.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/src/Focus.hpp"
#include "libvgcode/src/Shaders.hpp"
#include "libvgcode/src/ShadersES.hpp"
#include "libvgcode/src/ViewerImpl.hpp"

using libvgcode::Color;
using libvgcode::ColorRange;
using libvgcode::EColorRangeType;
using libvgcode::EGCodeExtrusionRole;
using libvgcode::EMoveType;
using libvgcode::EOptionType;
using libvgcode::EViewType;
using libvgcode::Focus;
using libvgcode::PathVertex;
using libvgcode::ViewerImpl;
using libvgcode::encode_focus_key;
using libvgcode::focus_kind;
using libvgcode::is_key_in_focus;
using Catch::Matchers::WithinAbs;

namespace libvgcode {

struct ColorRangeTestAccess
{
    static ColorRange make(EColorRangeType type, float min, float max, const Palette& palette)
    {
        ColorRange range(type);
        range.set_palette(palette);
        range.update(min);
        range.update(max);
        return range;
    }
};

struct ViewerImplTestAccess
{
    static void load(ViewerImpl& viewer, std::vector<PathVertex> vertices)
    {
        viewer.m_vertices = std::move(vertices);
        viewer.m_vertices_colors.resize(viewer.m_vertices.size());
        for (std::size_t i = 0; i < viewer.m_vertices.size(); ++i)
            viewer.m_layers.update(viewer.m_vertices[i], static_cast<uint32_t>(i));
    }
    static void show_travels(ViewerImpl& viewer) { viewer.m_settings.options_visibility[std::size_t(EOptionType::Travels)] = true; }
    static std::optional<float> range_position(const ViewerImpl& viewer, const PathVertex& v) { return viewer.get_vertex_range_position(v); }
    static const std::vector<uint32_t>& focus_keys(const ViewerImpl& viewer) { return viewer.m_vertices_focus; }
    static bool colors_pending(const ViewerImpl& viewer) { return viewer.m_settings.update_colors; }
};

} // namespace libvgcode

namespace {

PathVertex vertex(EMoveType type, EGCodeExtrusionRole role = EGCodeExtrusionRole::None)
{
    PathVertex v;
    v.type = type;
    v.role = role;
    return v;
}

uint32_t extrusion_key(EGCodeExtrusionRole role, uint8_t extruder, std::optional<float> position = std::nullopt)
{
    return encode_focus_key(focus_kind(role), extruder, position);
}

// A layer of an extrusion that took 2 s and a travel that took 1 s, at different speeds and accelerations.
std::vector<PathVertex> extrusion_and_travel()
{
    PathVertex extrusion = vertex(EMoveType::Extrude, EGCodeExtrusionRole::Perimeter);
    extrusion.feedrate = 50.0f;
    extrusion.acceleration = 1000.0f;
    extrusion.times = { 2.0f, 2.0f };
    PathVertex travel = vertex(EMoveType::Travel);
    travel.feedrate = 150.0f;
    travel.acceleration = 3000.0f;
    travel.times = { 1.0f, 1.0f };
    return { extrusion, travel };
}

Focus range_focus(float lo, float hi)
{
    Focus focus;
    focus.range = { lo, hi };
    return focus;
}

} // namespace

TEST_CASE("A focus key gives back the kind, extruder and value position it was made of", "[Libvgcode]")
{
    const uint32_t key = encode_focus_key(focus_kind(EOptionType::Travels), 255, 0.5f);
    CHECK(libvgcode::focus_key_kind(key) == focus_kind(EOptionType::Travels));
    CHECK(libvgcode::focus_key_extruder(key) == 255);
    CHECK(libvgcode::focus_key_value(key) == 131072);

    const uint32_t no_value = encode_focus_key(libvgcode::FOCUS_KIND_NONE, 0, std::nullopt);
    CHECK(libvgcode::focus_key_kind(no_value) == libvgcode::FOCUS_KIND_NONE);
    CHECK(libvgcode::focus_key_extruder(no_value) == 0);
    CHECK(libvgcode::focus_key_value(no_value) == 0);
}

TEST_CASE("A vertex is of its role when it extrudes, of its option otherwise", "[Libvgcode]")
{
    CHECK(focus_kind(vertex(EMoveType::Extrude, EGCodeExtrusionRole::GapFill)) == focus_kind(EGCodeExtrusionRole::GapFill));
    CHECK(focus_kind(vertex(EMoveType::Travel)) == focus_kind(EOptionType::Travels));
    CHECK(focus_kind(vertex(EMoveType::Wipe)) == focus_kind(EOptionType::Wipes));
    CHECK(focus_kind(vertex(EMoveType::Seam)) == focus_kind(EOptionType::Seams));
    CHECK(focus_kind(vertex(EMoveType::Noop)) == libvgcode::FOCUS_KIND_NONE);
}

TEST_CASE("Without a focus every path keeps its colour", "[Libvgcode]")
{
    CHECK_FALSE(Focus().any());
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 3), Focus()));
    CHECK(is_key_in_focus(encode_focus_key(libvgcode::FOCUS_KIND_NONE, 0, std::nullopt), Focus()));
}

TEST_CASE("Every part of a focus that is set has to match", "[Libvgcode]")
{
    Focus focus;
    focus.kind = focus_kind(EGCodeExtrusionRole::Perimeter);
    focus.extruder = 1;
    focus.range = { 0.5f, 1.0f };
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 1, 0.75f), focus));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::InternalInfill, 1, 0.75f), focus));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 2, 0.75f), focus));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 1, 0.25f), focus));

    Focus travels;
    travels.kind = focus_kind(EOptionType::Travels);
    CHECK(is_key_in_focus(encode_focus_key(focus_kind(EOptionType::Travels), 4, std::nullopt), travels));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 4), travels));
}

TEST_CASE("A range focus keeps its ends and leaves out the paths with no value", "[Libvgcode]")
{
    const Focus middle = range_focus(0.25f, 0.75f);
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 0.25f), middle));
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 0.75f), middle));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 0.24f), middle));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 0.76f), middle));

    const Focus all = range_focus(0.0f, 1.0f);
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 0.0f), all));
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0, 1.0f), all));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, 0), all));
}

TEST_CASE("An extruder focus keeps only the extrusions of that extruder", "[Libvgcode]")
{
    const int extruder = GENERATE(0, 255);
    Focus focus;
    focus.extruder = extruder;
    const uint8_t id = static_cast<uint8_t>(extruder);
    const uint8_t other = static_cast<uint8_t>(255 - extruder);
    CHECK(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, id), focus));
    CHECK_FALSE(is_key_in_focus(extrusion_key(EGCodeExtrusionRole::Perimeter, other), focus));
    CHECK_FALSE(is_key_in_focus(encode_focus_key(focus_kind(EOptionType::Travels), id, std::nullopt), focus));
    CHECK_FALSE(is_key_in_focus(encode_focus_key(focus_kind(EOptionType::Wipes), id, std::nullopt), focus));
    CHECK_FALSE(is_key_in_focus(encode_focus_key(focus_kind(EOptionType::Retractions), id, std::nullopt), focus));
}

TEST_CASE("The position on a colour range is where the colour of the value is taken", "[Libvgcode]")
{
    SECTION("linear") {
        const ColorRange range = libvgcode::ColorRangeTestAccess::make(EColorRangeType::Linear, 10.0f, 30.0f,
            { { 0, 0, 0 }, { 100, 200, 0 }, { 200, 0, 100 } });
        CHECK_THAT(range.get_position_at(10.0f), WithinAbs(0.0, 1e-6));
        CHECK_THAT(range.get_position_at(15.0f), WithinAbs(0.25, 1e-6));
        CHECK_THAT(range.get_position_at(30.0f), WithinAbs(1.0, 1e-6));
        CHECK_THAT(range.get_position_at(5.0f), WithinAbs(0.0, 1e-6));
        CHECK_THAT(range.get_position_at(40.0f), WithinAbs(1.0, 1e-6));
        CHECK(range.get_color_at(15.0f) == Color{ 50, 100, 0 });
    }
    SECTION("logarithmic") {
        // the colour halfway falls clear of a rounding step, the position being a float
        const ColorRange range = libvgcode::ColorRangeTestAccess::make(EColorRangeType::Logarithmic, 1.0f, 100.0f, { { 0, 0, 0 }, { 101, 201, 1 } });
        CHECK_THAT(range.get_position_at(1.0f), WithinAbs(0.0, 1e-6));
        CHECK_THAT(range.get_position_at(100.0f), WithinAbs(1.0, 1e-6));
        CHECK_THAT(range.get_position_at(10.0f), WithinAbs(0.5, 1e-5));
        CHECK(range.get_color_at(10.0f) == Color{ 50, 100, 0 });
    }
}

TEST_CASE("A colour range with no values puts every value at its start", "[Libvgcode]")
{
    const EColorRangeType type = GENERATE(EColorRangeType::Linear, EColorRangeType::Logarithmic);
    const ColorRange      range(type);
    REQUIRE(range.get_range()[0] > range.get_range()[1]);
    CHECK_THAT(range.get_position_at(5.0f), WithinAbs(0.0, 1e-6));
    CHECK(range.get_color_at(5.0f) == range.get_palette().front());
}

TEST_CASE("The shaders read the focus key with the layout of Focus.hpp, not with numbers of their own", "[Libvgcode]")
{
    const std::string shader = GENERATE(std::string(libvgcode::Segments_Vertex_Shader), std::string(libvgcode::Options_Vertex_Shader),
                                        std::string(libvgcode::Segments_Vertex_Shader_ES), std::string(libvgcode::Options_Vertex_Shader_ES));
    for (const char* number : {"63u", "255u", "6u)", "14u)"})
        CHECK(shader.find(number) == std::string::npos);
}

TEST_CASE("Every vertex shader the viewer compiles that reads the focus key has its layout defined after its version line", "[Libvgcode]")
{
    const libvgcode::FocusVertexShaders shaders = libvgcode::focus_vertex_shaders();
    const auto unsigned_define = [](const char* name, uint32_t value) { return "\n#define " + std::string(name) + " " + std::to_string(value) + "u\n"; };
    const std::vector<std::string> defines = {
        "\n#define FOCUS_EXTRUSION_KINDS " + std::to_string(libvgcode::GCODE_EXTRUSION_ROLES_COUNT) + "\n",
        unsigned_define("FOCUS_KIND_MASK", libvgcode::FOCUS_KIND_MASK),
        unsigned_define("FOCUS_EXTRUDER_SHIFT", libvgcode::FOCUS_EXTRUDER_SHIFT),
        unsigned_define("FOCUS_EXTRUDER_MASK", libvgcode::FOCUS_EXTRUDER_MASK),
        unsigned_define("FOCUS_VALUE_SHIFT", libvgcode::FOCUS_VALUE_SHIFT)};
    size_t compiled = 0;
    for (const std::string* source : {&shaders.segments, &shaders.segments_depth, &shaders.options}) {
        if (source->empty())
            continue;
        ++compiled;
        CHECK(source->rfind("#version", 0) == 0);
        for (const std::string& define : defines) {
            CAPTURE(define);
            CHECK(source->find(define) != std::string::npos);
        }
    }
    CHECK(compiled >= 2);
}

TEST_CASE("Whether a vertex's value counts on a scale decides its value, its colour and the position in its focus key alike", "[Libvgcode]")
{
    ViewerImpl viewer;
    libvgcode::ViewerImplTestAccess::load(viewer, extrusion_and_travel());
    const PathVertex& extrusion = viewer.get_vertex_at(0);
    const PathVertex& travel = viewer.get_vertex_at(1);

    CHECK_FALSE(viewer.get_range_value(travel, EViewType::Speed).has_value());
    libvgcode::ViewerImplTestAccess::show_travels(viewer);
    CHECK(viewer.get_range_value(travel, EViewType::Speed) == std::optional<float>(150.0f));
    CHECK(viewer.get_range_value(travel, EViewType::Acceleration) == std::optional<float>(3000.0f));
    CHECK_FALSE(viewer.get_range_value(travel, EViewType::LayerTimeLinear).has_value());
    CHECK(viewer.get_range_value(extrusion, EViewType::Speed) == std::optional<float>(50.0f));
    CHECK(viewer.get_range_value(extrusion, EViewType::LayerTimeLinear) == std::optional<float>(3.0f));

    const EViewType type = GENERATE(EViewType::Speed, EViewType::Acceleration, EViewType::LayerTimeLinear);
    viewer.set_view_type(type);
    viewer.update_colors();
    for (const PathVertex* v : { &extrusion, &travel }) {
        const std::optional<float> value = viewer.get_range_value(*v, type);
        CHECK(libvgcode::ViewerImplTestAccess::range_position(viewer, *v).has_value() == value.has_value());
        if (value.has_value())
            CHECK(viewer.get_vertex_color(*v) == viewer.get_color_range(type).get_color_at(*value));
        else
            CHECK(viewer.get_vertex_color(*v) == viewer.get_option_color(EOptionType::Travels));
    }
}

TEST_CASE("The focus keys are built once a focus is set and kept after it is cleared", "[Libvgcode]")
{
    ViewerImpl viewer;
    libvgcode::ViewerImplTestAccess::load(viewer, extrusion_and_travel());
    viewer.set_view_type(EViewType::Speed);
    viewer.update_colors();
    viewer.set_focus(Focus());
    CHECK_FALSE(libvgcode::ViewerImplTestAccess::colors_pending(viewer));
    CHECK(libvgcode::ViewerImplTestAccess::focus_keys(viewer).empty());

    Focus perimeters;
    perimeters.kind = focus_kind(EGCodeExtrusionRole::Perimeter);
    viewer.set_focus(perimeters);
    CHECK(libvgcode::ViewerImplTestAccess::colors_pending(viewer));
    viewer.update_colors();
    const std::vector<uint32_t>& keys = libvgcode::ViewerImplTestAccess::focus_keys(viewer);
    REQUIRE(keys.size() == 2);
    CHECK(is_key_in_focus(keys[0], perimeters));
    CHECK_FALSE(is_key_in_focus(keys[1], perimeters));

    viewer.set_focus(Focus());
    viewer.update_colors();
    CHECK(libvgcode::ViewerImplTestAccess::focus_keys(viewer).size() == 2);
}
