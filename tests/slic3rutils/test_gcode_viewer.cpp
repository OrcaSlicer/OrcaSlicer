#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libvgcode/include/GCodeInputData.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

TEST_CASE("Overhang preview selection round trips through saved preferences", "[GCodeViewer][Overhang][Regression]")
{
    using Slic3r::GUI::GCodeViewer;
    const std::string name = GCodeViewer::view_type_to_config_name(libvgcode::EViewType::Overhang);
    REQUIRE(name == "overhang");

    libvgcode::EViewType restored = libvgcode::EViewType::FeatureType;
    REQUIRE(GCodeViewer::view_type_from_config_name(name, restored));
    CHECK(restored == libvgcode::EViewType::Overhang);
}

// Guard values are part of the preview conversion contract. Walls convert their whole contour offset, which may pass
// a full bead, and read horizontal only at the cap; other lines are measured up to a full bead, which is a bridge.
TEST_CASE("Overhang angles convert walls continuously and saturate bridges", "[GCodeViewer][Overhang]")
{
    using libvgcode::EGCodeExtrusionRole;
    struct Case {
        const char         *name;
        EGCodeExtrusionRole role;
        float               percentage;
        float               width;
        float               height;
        float               z_distance;
        float               expected_degree;
    };
    constexpr float degrees = 57.29577951308232f;
    const std::array<Case, 10> cases{{
        {"fully unsupported infill without dimensions", EGCodeExtrusionRole::SolidInfill, 100.0f, 0.0f, 0.0f, 0.0f, 90.0f},
        {"infill above a full bead", EGCodeExtrusionRole::BridgeInfill, 120.0f, 0.4f, 0.2f, 0.2f, 90.0f},
        {"fully supported", EGCodeExtrusionRole::ExternalPerimeter, 0.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"percentage below the supported range", EGCodeExtrusionRole::Perimeter, -10.0f, 0.4f, 0.2f, 0.2f, 0.0f},
        {"missing width", EGCodeExtrusionRole::ExternalPerimeter, 50.0f, 0.0f, 0.2f, 0.2f, 0.0f},
        {"missing vertical separation", EGCodeExtrusionRole::ExternalPerimeter, 50.0f, 0.4f, 0.0f, 0.0f, 0.0f},
        {"wall at a full bead", EGCodeExtrusionRole::ExternalPerimeter, 100.0f, 0.4f, 0.2f, 0.2f, std::atan(2.0f) * degrees},
        {"wall past a full bead", EGCodeExtrusionRole::OverhangPerimeter, 150.0f, 0.4f, 0.2f, 0.2f, std::atan(3.0f) * degrees},
        {"wall at the cap", EGCodeExtrusionRole::Perimeter, libvgcode::PathVertex::MAX_OVERHANG_PERCENTAGE, 0.4f, 0.2f, 0.2f, 90.0f},
        {"wall height used without slice spacing", EGCodeExtrusionRole::ExternalPerimeter, 150.0f, 0.4f, 0.3f, 0.0f, std::atan(2.0f) * degrees},
    }};

    for (const Case &test : cases) {
        DYNAMIC_SECTION(test.name) {
            libvgcode::PathVertex vertex;
            vertex.role                = test.role;
            vertex.overhang_percentage = test.percentage;
            vertex.width               = test.width;
            vertex.height              = test.height;
            vertex.overhang_z_distance = test.z_distance;
            CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(test.expected_degree, 1e-4));
            // The percentage view shows the unsupported share of the bead.
            CHECK_THAT(vertex.overhang_share(), Catch::Matchers::WithinAbs(std::clamp(test.percentage, 0.0f, 100.0f), 1e-6));
        }
    }
}

// Lines without overhang data, such as supports, are not fully supported lines.
TEST_CASE("Overhang preview tells lines without data from supported ones", "[GCodeViewer][Overhang]")
{
    libvgcode::PathVertex vertex;
    vertex.role  = libvgcode::EGCodeExtrusionRole::SupportMaterial;
    vertex.width = 0.4f;
    vertex.height = 0.2f;
    vertex.overhang_percentage = -1.0f;
    CHECK_FALSE(vertex.has_overhang());
    vertex.role = libvgcode::EGCodeExtrusionRole::ExternalPerimeter;
    vertex.overhang_percentage = 0.0f;
    CHECK(vertex.has_overhang());
    CHECK_THAT(vertex.overhang_degree(), Catch::Matchers::WithinAbs(0.0, 1e-6));
}

namespace {

// A vertex of G-code line `gcode_id` reading `percentage`, negative for a line without overhang data.
libvgcode::PathVertex line_vertex(uint32_t gcode_id, float percentage)
{
    libvgcode::PathVertex vertex;
    vertex.type                = libvgcode::EMoveType::Extrude;
    vertex.role                = libvgcode::EGCodeExtrusionRole::ExternalPerimeter;
    vertex.gcode_id            = gcode_id;
    vertex.overhang_percentage = percentage;
    return vertex;
}

// The overhang the G-code line of vertex `id` among `vertices` reads, in percent or in degrees.
std::optional<float> overhang_value_of(const std::vector<libvgcode::PathVertex> &vertices, size_t id, bool percentage)
{
    return Slic3r::GUI::GCodeViewer::line_overhang_value(id, vertices.size(), percentage,
        [&vertices](size_t i) -> const libvgcode::PathVertex & { return vertices[i]; });
}

// The share the G-code line of vertex `id` among `vertices` reads.
std::optional<float> overhang_share_of(const std::vector<libvgcode::PathVertex> &vertices, size_t id)
{
    return overhang_value_of(vertices, id, true);
}

} // namespace

// A line with a local overhang profile is drawn as several vertices of one G-code line. The marker reads the line by
// its largest measured part, whichever of its vertices it is on, and never by the lines on either side.
TEST_CASE("A line drawn as several vertices reads the overhang of its largest measured part",
          "[GCodeViewer][Overhang][Regression]")
{
    // line 7 bridges in its middle part only; lines 6 and 8 around it read more
    const std::vector<libvgcode::PathVertex> vertices{line_vertex(6, 100.0f), line_vertex(7, 0.0f),
        line_vertex(7, 80.0f), line_vertex(7, 0.0f), line_vertex(8, 100.0f)};
    for (size_t id = 1; id <= 3; ++id) {
        INFO("vertex " << id);
        CHECK(overhang_share_of(vertices, id) == 80.0f);
    }
    // a line of one vertex reads that vertex
    CHECK(overhang_share_of(vertices, 0) == 100.0f);
    CHECK(overhang_share_of(vertices, 4) == 100.0f);
}

// The search for the other parts of a line stops at either end of the vertices: a line may be the first or the last
// one drawn.
TEST_CASE("A line at either end of the vertices reads its largest part without leaving them",
          "[GCodeViewer][Overhang][Regression]")
{
    const std::vector<libvgcode::PathVertex> peak_first{line_vertex(7, 80.0f), line_vertex(7, 0.0f),
        line_vertex(7, 0.0f)};
    const std::vector<libvgcode::PathVertex> peak_last{line_vertex(7, 0.0f), line_vertex(7, 0.0f),
        line_vertex(7, 80.0f)};
    for (size_t id = 0; id < 3; ++id) {
        INFO("vertex " << id);
        CHECK(overhang_share_of(peak_first, id) == 80.0f);
        CHECK(overhang_share_of(peak_last, id) == 80.0f);
    }
    const std::vector<libvgcode::PathVertex> single{line_vertex(7, 30.0f)};
    CHECK(overhang_share_of(single, 0) == 30.0f);
}

// A part without data is not a supported one: a measured part, even at 0 %, wins over it, and a line with no measured
// part has no value, which the marker shows as N/A.
TEST_CASE("A line reads its overhang only from its measured parts", "[GCodeViewer][Overhang][Regression]")
{
    const std::vector<libvgcode::PathVertex> unmeasured{line_vertex(6, 100.0f), line_vertex(7, -1.0f),
        line_vertex(7, -1.0f), line_vertex(8, 100.0f)};
    CHECK_FALSE(overhang_share_of(unmeasured, 1).has_value());
    CHECK_FALSE(overhang_share_of(unmeasured, 2).has_value());
    const std::vector<libvgcode::PathVertex> partly{line_vertex(7, -1.0f), line_vertex(7, 0.0f),
        line_vertex(7, -1.0f)};
    CHECK(overhang_share_of(partly, 0) == 0.0f);
    CHECK(overhang_share_of(partly, 2) == 0.0f);
    const std::vector<libvgcode::PathVertex> single{line_vertex(6, 100.0f), line_vertex(7, 50.0f),
        line_vertex(8, 100.0f)};
    CHECK(overhang_share_of(single, 1) == 50.0f);
}

// A vertex of another kind that carries the same G-code line number, as a marker may, is no part of the line.
TEST_CASE("A line reads its overhang only from vertices of its own kind", "[GCodeViewer][Overhang][Regression]")
{
    libvgcode::PathVertex marker = line_vertex(7, 90.0f);
    marker.type = libvgcode::EMoveType::Seam;
    const std::vector<libvgcode::PathVertex> vertices{marker, line_vertex(7, 30.0f), line_vertex(7, 10.0f)};
    CHECK(overhang_share_of(vertices, 2) == 30.0f);
    CHECK(overhang_share_of(vertices, 0) == 90.0f);
}

// A line whose profile widens is drawn with widths in between, so its largest share and its largest angle can lie on
// different parts. The marker reads each unit from the part largest in that unit.
TEST_CASE("A line reads its largest overhang in each unit", "[GCodeViewer][Overhang][Regression]")
{
    // Over slice planes 0.2 mm apart, 60 % of a 0.35 mm bead hangs 0.21 mm and 50 % of a 0.6 mm bead 0.3 mm.
    libvgcode::PathVertex narrow = line_vertex(7, 60.0f);
    narrow.width = 0.35f;
    narrow.overhang_z_distance = 0.2f;
    libvgcode::PathVertex wide = line_vertex(7, 50.0f);
    wide.width = 0.6f;
    wide.overhang_z_distance = 0.2f;
    const std::vector<libvgcode::PathVertex> vertices{line_vertex(6, 100.0f), narrow, wide, line_vertex(8, 100.0f)};
    constexpr float degrees = 57.29577951308232f;
    for (size_t id = 1; id <= 2; ++id) {
        INFO("vertex " << id);
        const std::optional<float> share = overhang_share_of(vertices, id);
        REQUIRE(share.has_value());
        CHECK_THAT(*share, Catch::Matchers::WithinAbs(60.0f, 1e-4f));
        const std::optional<float> angle = overhang_value_of(vertices, id, false);
        REQUIRE(angle.has_value());
        CHECK_THAT(*angle, Catch::Matchers::WithinAbs(std::atan(1.5f) * degrees, 1e-3f));
    }
}

// The Overhang view grades a measured line by its unsupported share or by its angle. A line without data, such as a
// support, has no value in either unit, so it is greyed rather than read as supported.
TEST_CASE("The overhang view value follows the selected unit and is absent without data",
          "[GCodeViewer][Overhang][Regression]")
{
    using libvgcode::EGCodeExtrusionRole;
    struct Case {
        const char          *name;
        EGCodeExtrusionRole  role;
        float                percentage;
        std::optional<float> share;
        std::optional<float> degree;
    };
    constexpr float degrees = 57.29577951308232f;
    // Lines 0.4 mm wide over slice planes 0.2 mm apart: x % of the width is x / 50 of the spacing.
    const std::array<Case, 4> cases{{
        {"support without data", EGCodeExtrusionRole::SupportMaterial, -1.0f, std::nullopt, std::nullopt},
        {"supported wall", EGCodeExtrusionRole::ExternalPerimeter, 0.0f, 0.0f, 0.0f},
        // the share stops at a full bead, a wall's angle goes on: atan(150 / 50)
        {"wall past a full bead", EGCodeExtrusionRole::ExternalPerimeter, 150.0f, 100.0f, std::atan(3.0f) * degrees},
        // atan(50 / 50)
        {"half-supported infill", EGCodeExtrusionRole::InternalInfill, 50.0f, 50.0f, 45.0f},
    }};
    for (const Case &test : cases) {
        DYNAMIC_SECTION(test.name) {
            libvgcode::PathVertex vertex;
            vertex.type                = libvgcode::EMoveType::Extrude;
            vertex.role                = test.role;
            vertex.width               = 0.4f;
            vertex.height              = 0.2f;
            vertex.overhang_z_distance = 0.2f;
            vertex.overhang_percentage = test.percentage;
            const std::optional<float> share  = vertex.overhang_view_value(true);
            const std::optional<float> degree = vertex.overhang_view_value(false);
            REQUIRE(share.has_value() == test.share.has_value());
            REQUIRE(degree.has_value() == test.degree.has_value());
            if (test.share)
                CHECK_THAT(*share, Catch::Matchers::WithinAbs(*test.share, 1e-4));
            if (test.degree)
                CHECK_THAT(*degree, Catch::Matchers::WithinAbs(*test.degree, 1e-4));
        }
    }
}

namespace {

// A comment line holding `tag` in the current dialect, as GCode.cpp writes it.
std::string tag_line(Slic3r::GCodeProcessor::ETags tag, const std::string &value)
{
    return ";" + Slic3r::GCodeProcessor::reserved_tag(tag) + value + "\n";
}

// The processor's result for G-code written for the default printer, timed as for an export.
const Slic3r::GCodeProcessorResult &processed(Slic3r::GCodeProcessor &processor, const std::string &gcode)
{
    processor.apply_config(Slic3r::FullPrintConfig());
    processor.initialize_result_moves();
    processor.process_buffer(gcode);
    processor.finalize(false);
    return processor.get_result();
}

// The vertices the preview draws for `result`. convert() asks the viewer nothing, and a viewer is built and destroyed
// without an OpenGL context: it reaches no OpenGL call before init().
std::vector<libvgcode::PathVertex> preview_vertices(const Slic3r::GCodeProcessorResult &result)
{
    const libvgcode::Viewer viewer;
    return libvgcode::convert(result, {"#FF0000"}, {}, viewer).vertices;
}

// The start of a perimeter 0.4 mm wide on a 0.2 mm first layer, from the origin.
std::string perimeter_start()
{
    using Slic3r::GCodeProcessor;
    return "G90\nM83\nT0\n" +
        tag_line(GCodeProcessor::ETags::Role, Slic3r::ExtrusionEntity::role_to_string(Slic3r::erPerimeter)) +
        tag_line(GCodeProcessor::ETags::Height, "0.2") + tag_line(GCodeProcessor::ETags::Width, "0.4") +
        "G1 X0 Y0 Z0.2 F600\n";
}

} // namespace

// libvgcode starts a path, with a phantom vertex in front of it, where a value the preview colours by changes. With
// overhang metadata a new width, height or percentage starts one, as the angle reads them. G-code without it keeps the
// paths it had, where a width change alone starts none.
TEST_CASE("Preview paths split on overhang values only when the G-code has overhang metadata",
          "[GCodeViewer][Overhang][Regression]")
{
    using Catch::Matchers::WithinAbs;
    using Slic3r::GCodeProcessor;
    // Three 10 mm lines of the same flow, the given tags in front of the first and the second. The second line runs at
    // full speed throughout, so the processor adds no actual-speed vertex in it, which would take a width between the
    // first line's and its own.
    const auto vertices_of = [](const std::string &first, const std::string &second, bool metadata) {
        GCodeProcessor processor;
        const Slic3r::GCodeProcessorResult &result =
            processed(processor, perimeter_start() + first + "G1 X10 E1\n" + second + "G1 X20 E1\nG1 X30 E1\n");
        REQUIRE(result.has_overhang_metadata == metadata);
        return preview_vertices(result);
    };
    const std::string wider     = tag_line(GCodeProcessor::ETags::Width, "0.5");
    const std::string supported = tag_line(GCodeProcessor::ETags::Overhang, "0");
    const std::string half      = tag_line(GCodeProcessor::ETags::Overhang, "50");

    CHECK(vertices_of("", wider, false).size() == vertices_of("", "", false).size());
    CHECK(vertices_of(supported, wider, true).size() == vertices_of(supported, "", true).size() + 1);

    const std::vector<libvgcode::PathVertex> split = vertices_of(supported, half, true);
    REQUIRE(split.size() == vertices_of(supported, supported, true).size() + 1);
    // the phantom vertex sits where the first line ends, with the second line's values and no time of its own
    const auto phantom = std::find_if(split.begin(), split.end(),
        [](const libvgcode::PathVertex &v) { return std::abs(v.overhang_percentage - 50.0f) < 1e-3f; });
    REQUIRE(phantom != split.end());
    REQUIRE(phantom != split.begin());
    const libvgcode::PathVertex &before = *(phantom - 1);
    CHECK_THAT(before.overhang_percentage, WithinAbs(0.0, 1e-6));
    for (size_t axis = 0; axis < phantom->position.size(); ++axis)
        CHECK_THAT(phantom->position[axis], WithinAbs(before.position[axis], 1e-6));
    CHECK_THAT(phantom->position[0], WithinAbs(10.0, 1e-4));
    for (const float time : phantom->times)
        CHECK_THAT(time, WithinAbs(0.0, 1e-9));
}

// A line with a local profile reaches the preview as one piece per run of equal cells, and the piece after a change
// of value starts a path. The pieces share out the line's time, so the preview's total stays the processor's.
TEST_CASE("A line overhang profile becomes one preview piece per run", "[GCodeViewer][Overhang][Regression]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;
    using libvgcode::EMoveType;
    using libvgcode::PathVertex;
    using Slic3r::GCodeProcessor;
    const std::string start = perimeter_start() + tag_line(GCodeProcessor::ETags::Overhang, "0");
    GCodeProcessor plain_processor;
    const std::vector<PathVertex> plain = preview_vertices(processed(plain_processor, start + "G1 X8 E1\n"));
    // four 2 mm cells over X0 to X8, the last one bridging
    GCodeProcessor processor;
    const Slic3r::GCodeProcessorResult &result = processed(processor, start +
        tag_line(GCodeProcessor::ETags::Overhang_Line, "4,0,0*3,100") + "G1 X8 E1 ;" +
        GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Line_Apply) + "\n");
    REQUIRE(result.overhang_line_profiles.size() == 1);
    const std::vector<PathVertex> vertices = preview_vertices(result);
    // the piece over the bridging cell, and the phantom vertex in front of it
    CHECK(vertices.size() == plain.size() + 2);
    for (size_t i = 1; i < vertices.size(); ++i) {
        const float from = vertices[i - 1].position[0];
        const float to   = vertices[i].position[0];
        // a phantom vertex has no length of its own
        if (vertices[i].type != EMoveType::Extrude || to - from < 1e-6f)
            continue;
        INFO("piece from x = " << from << " to x = " << to);
        CHECK_THAT(vertices[i].overhang_percentage, WithinAbs(0.5f * (from + to) < 6.0f ? 0.0 : 100.0, 1e-6));
    }
    // the supported cells end at x = 6, three quarters of the line
    const auto cells_end = std::find_if(vertices.begin(), vertices.end(), [](const PathVertex &v) {
        return v.type == EMoveType::Extrude && std::abs(v.position[0] - 6.0f) < 1e-4f;
    });
    REQUIRE(cells_end != vertices.end());
    CHECK_THAT(cells_end->overhang_percentage, WithinAbs(0.0, 1e-6));
    const auto line_end = std::find_if(vertices.rbegin(), vertices.rend(),
        [](const PathVertex &v) { return v.type == EMoveType::Extrude; });
    REQUIRE(line_end != vertices.rend());
    CHECK_THAT(line_end->position[0], WithinAbs(8.0, 1e-4));
    CHECK_THAT(line_end->overhang_percentage, WithinAbs(100.0, 1e-6));

    double preview_time = 0.0;
    for (const PathVertex &v : vertices)
        preview_time += v.times[0];
    double moves_time = 0.0;
    for (const Slic3r::GCodeProcessorResult::MoveVertex &move : result.moves)
        moves_time += move.time[0];
    REQUIRE(moves_time > 0.0);
    CHECK_THAT(preview_time, WithinRel(moves_time, 1e-5));
}

// Lines the G-code does not measure, such as supports, reach the preview without a value, which the Overhang view
// greys, while the measured lines of the same file keep theirs.
TEST_CASE("Preview vertices of lines without overhang data carry none", "[GCodeViewer][Overhang][Regression]")
{
    using libvgcode::EGCodeExtrusionRole;
    using Slic3r::GCodeProcessor;
    GCodeProcessor processor;
    const Slic3r::GCodeProcessorResult &result = processed(processor, "G90\nM83\nT0\n" +
        tag_line(GCodeProcessor::ETags::Role, Slic3r::ExtrusionEntity::role_to_string(Slic3r::erSupportMaterial)) +
        tag_line(GCodeProcessor::ETags::Height, "0.2") + tag_line(GCodeProcessor::ETags::Width, "0.4") +
        "G1 X0 Y0 Z0.2 F600\nG1 X10 E1\n" +
        tag_line(GCodeProcessor::ETags::Role, Slic3r::ExtrusionEntity::role_to_string(Slic3r::erPerimeter)) +
        tag_line(GCodeProcessor::ETags::Overhang, "30") + "G1 X20 E1\n");
    REQUIRE(result.has_overhang_metadata);
    size_t supports = 0;
    size_t walls    = 0;
    for (const libvgcode::PathVertex &v : preview_vertices(result)) {
        if (v.type != libvgcode::EMoveType::Extrude)
            continue;
        if (v.role == EGCodeExtrusionRole::SupportMaterial) {
            ++supports;
            CHECK_FALSE(v.has_overhang());
        } else if (v.role == EGCodeExtrusionRole::Perimeter) {
            ++walls;
            CHECK_THAT(v.overhang_percentage, Catch::Matchers::WithinAbs(30.0, 1e-6));
        }
    }
    CHECK(supports > 0);
    CHECK(walls > 0);
}
