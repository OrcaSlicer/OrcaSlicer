#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PressureEqualizer.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

// A comment line holding `tag` and its value, as the G-code generator writes it in the active tag dialect.
std::string tag_line(GCodeProcessor::ETags tag, const std::string &value)
{
    return ";" + GCodeProcessor::reserved_tag(tag) + value + "\n";
}

// The output of an equalizer limiting the slope of the volumetric rate to `slope` mm^3/s^2 for `layer`, the last one.
std::string equalize(double slope, bool overhang_metadata, const std::string &layer)
{
    GCodeConfig config;
    config.max_volumetric_extrusion_rate_slope.value                = slope;
    config.max_volumetric_extrusion_rate_slope_segment_length.value = 2.;
    config.use_relative_e_distances.value                           = true;
    PressureEqualizer equalizer(config, overhang_metadata);
    // The equalizer holds a layer back until the next one comes, here the end of the print.
    const std::string output = equalizer.process_layer(LayerResult{layer, 0, false, false, false}).gcode;
    return output + equalizer.process_layer(LayerResult::make_nop_layer_result()).gcode;
}

// The printer commands of equalized G-code, without their comments.
std::vector<std::string> equalized_commands(const std::string &gcode)
{
    std::vector<std::string> commands;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (!line.cmd().empty())
            commands.push_back(line.raw().substr(0, line.raw().find(';')));
    });
    return commands;
}

// How many comment lines of `gcode` mention `text`.
size_t comment_lines_with(const std::string &gcode, std::string_view text)
{
    size_t count = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.cmd().empty() && line.comment().find(text) != std::string_view::npos)
            ++count;
    });
    return count;
}

// The split-line layer below runs its fast line from x = 10 to here.
double split_line_end(bool slow_after) { return slow_after ? 70. : 40.; }

// At 1 mm^3/s^2 a 30 mm line cannot reach its flow at all. At 20 mm^3/s^2 the flow ramps over about 24 mm at each end
// of a 60 mm line, leaving a steady part between the ramps.
double split_line_slope(bool slow_after) { return slow_after ? 20. : 1.; }

// Lines of one cross section at 10 mm/s and 100 mm/s, the fast one marked for an overhang profile and `metadata`
// written before it, then with `slow_after` another slow line. The equalizer never adjusts the last extrusion of a
// segment, so a short move without an X word ends it.
std::string split_line_layer(bool slow_after, const std::string &metadata)
{
    const std::string marker = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Line_Apply);
    std::string layer = ";_EXTRUSION_ROLE:" + std::to_string(int(erSolidInfill)) + "\n"
        "G1 X0 Y0 F6000\n"
        "G1 E1 F2400\n"
        "G1 F600;_EXTRUDE_SET_SPEED\n"
        "G1 X10 Y0 E0.4\n"
        ";_EXTRUDE_END\n" + metadata +
        "G1 F6000;_EXTRUDE_SET_SPEED\n" +
        std::string(slow_after ? "G1 X70 Y0 E2.4" : "G1 X40 Y0 E1.2") + " ; solid infill" + marker + "\n"
        ";_EXTRUDE_END\n";
    if (slow_after)
        layer += "G1 F600;_EXTRUDE_SET_SPEED\n"
                 "G1 X80 Y0 E0.4\n"
                 ";_EXTRUDE_END\n";
    layer += "G1 F600;_EXTRUDE_SET_SPEED\n"
             "G1 Y1 E0.04\n"
             ";_EXTRUDE_END\n";
    return layer;
}

// How many pieces the fast line of split_line_layer() was written as, and how many of them carry the marker.
std::pair<size_t, size_t> fast_line_pieces(const std::string &output, bool slow_after)
{
    const double fast_end = split_line_end(slow_after);
    std::pair<size_t, size_t> pieces{0, 0};
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (!line.cmd_is("G1") || !line.has_e() || !line.has_x() || line.x() <= 10. + EPSILON || line.x() > fast_end + EPSILON)
            return;
        ++pieces.first;
        if (GCodeProcessor::has_inline_tag(line.comment(), GCodeProcessor::ETags::Overhang_Line_Apply))
            ++pieces.second;
    });
    return pieces;
}

// The look-back layer below writes its comments this many times between its moves.
constexpr size_t commented_moves = 103;

// Three moves, the middle one slower: with no line between them the equalizer leaves them alone at 1 mm^3/s^2, while a
// comment line between them would bring them into one of its windows. Then a run of short fast moves before a slow
// one: more than half of the look-back window once comments count.
std::string commented_layer(const std::string &comments)
{
    std::string layer = ";_EXTRUSION_ROLE:" + std::to_string(int(erSolidInfill)) + "\n"
        "G1 X0 Y0 F6000\n"
        "G1 E1 F2400\n"
        "G1 F6000;_EXTRUDE_SET_SPEED\n"
        "G1 X10 Y0 E0.4\n" + comments +
        "G1 F600\n"
        "G1 X20 Y0 E0.4\n" + comments +
        "G1 F6000\n"
        "G1 X30 Y0 E0.4\n"
        ";_EXTRUDE_END\n";
    layer += "G1 F6000;_EXTRUDE_SET_SPEED\n";
    for (int i = 1; i <= 100; ++i)
        layer += comments + "G1 X" + std::to_string(30 + i) + " Y0 E0.04\n";
    layer += comments + "G1 F600\nG1 X140 Y0 E0.4\nG1 Y1 E0.04\n;_EXTRUDE_END\n";
    return layer;
}

} // namespace

// The preview spreads an overhang profile over every piece of its G1 that carries the profile's marker. A line the
// equalizer ramps is written as several pieces, so each one must keep the marker: a fast line after a slow one only
// ramps up, and one between two slow lines also has a steady part and ramps down again.
TEST_CASE("Split extrusions keep the overhang profile marker on every piece", "[PressureEqualizer][Overhang][Regression]")
{
    const bool slow_after = GENERATE(false, true);
    CAPTURE(slow_after);
    const std::string output = equalize(split_line_slope(slow_after), true, split_line_layer(slow_after, {}));
    const auto [pieces, marked_pieces] = fast_line_pieces(output, slow_after);
    INFO(output);
    REQUIRE(pieces > 1);
    CHECK(marked_pieces == pieces);
}

// With the option off the marker can only be a comment that looks like it, which the equalizer writes as it writes any
// comment of a line it splits: on the first piece alone.
TEST_CASE("A split extrusion keeps a look-alike marker on its first piece only when the metadata is off",
          "[PressureEqualizer][Overhang][Regression]")
{
    const bool slow_after = GENERATE(false, true);
    CAPTURE(slow_after);
    const std::string output = equalize(split_line_slope(slow_after), false, split_line_layer(slow_after, {}));
    const auto [pieces, marked_pieces] = fast_line_pieces(output, slow_after);
    INFO(output);
    REQUIRE(pieces > 1);
    CHECK(marked_pieces == 1u);
}

// Overhang metadata comments sit between the moves only when the option is on. The equalizer looks back over a
// number of lines, so it must not count them, or turning the metadata on would change the ramps it writes. It tells
// them by the four tags the generator writes, which the two dialects spell differently.
TEST_CASE("Overhang metadata comments leave the equalized commands unchanged", "[PressureEqualizer][Overhang][Regression]")
{
    const bool bbl_printer = GENERATE(false, true);
    CAPTURE(bbl_printer);
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = bbl_printer;
    using ETags = GCodeProcessor::ETags;
    const std::string metadata = tag_line(ETags::Overhang_Z_Distance, "0.2") + tag_line(ETags::Overhang, "100") +
        tag_line(ETags::Overhang_Line, "4,0,0*3,100") + tag_line(ETags::Overhang_Arc, "3,0,0,50,100");
    const std::string with    = equalize(1., true, commented_layer(metadata));
    const std::string without = equalize(1., true, commented_layer({}));
    // All four lines are kept, each time they were written.
    CHECK(comment_lines_with(with, "OVERHANG") == 4u * commented_moves);
    CHECK(comment_lines_with(without, "OVERHANG") == 0u);
    const std::vector<std::string> commands = equalized_commands(without);
    REQUIRE_FALSE(commands.empty());
    CHECK(equalized_commands(with) == commands);
}

// With the option off the G-code carries no overhang metadata, so a comment that only looks like it, as a custom
// G-code may write, is an ordinary line: the equalizer counts it in its look-back windows like any other comment and
// ramps exactly as it does around the same comment under another name.
TEST_CASE("Comments that look like overhang metadata are ordinary lines when the metadata is off",
          "[PressureEqualizer][Overhang][Regression]")
{
    using ETags = GCodeProcessor::ETags;
    const std::string look_alike = tag_line(ETags::Overhang, "100") + tag_line(ETags::Overhang_Line, "4,0,0*3,100");
    std::string renamed = look_alike;
    const std::string_view tag_word = "OVERHANG";
    for (size_t at = renamed.find(tag_word); at != std::string::npos; at = renamed.find(tag_word, at))
        renamed.replace(at, tag_word.size(), "NOTEHANG");
    CAPTURE(look_alike, renamed);
    const std::vector<std::string> ordinary = equalized_commands(equalize(1., false, commented_layer(renamed)));
    // The comments count: without them the equalizer writes other commands.
    REQUIRE(ordinary != equalized_commands(equalize(1., false, commented_layer({}))));
    CHECK(equalized_commands(equalize(1., false, commented_layer(look_alike))) == ordinary);
}

// The preview binds an overhang profile to the next G1 that carries its marker, so the profile's lines must stay after
// the line before and in front of the first piece of the line the equalizer splits. Read back, the pieces then share
// the one profile: four equal cells, the last unsupported. The equalizer tells the lines by their tags, which the two
// dialects spell differently.
TEST_CASE("Overhang chunks stay in front of the first piece of the line they describe", "[PressureEqualizer][Overhang][Regression]")
{
    const bool slow_after  = GENERATE(false, true);
    const bool bbl_printer = GENERATE(false, true);
    CAPTURE(slow_after, bbl_printer);
    const bool       was_bbl_printer = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl_printer([was_bbl_printer] { GCodeProcessor::s_IsBBLPrinter = was_bbl_printer; });
    GCodeProcessor::s_IsBBLPrinter = bbl_printer;
    using ETags = GCodeProcessor::ETags;
    // In the order the generator writes them: the slice spacing, the scalar, then the profile it is the maximum of.
    const std::vector<std::string> metadata = {tag_line(ETags::Overhang_Z_Distance, "0.2"),
        tag_line(ETags::Overhang, "100"), tag_line(ETags::Overhang_Line, "4,0,0*3,100")};
    const std::string output = equalize(split_line_slope(slow_after), true,
        split_line_layer(slow_after, metadata[0] + metadata[1] + metadata[2]));
    const double fast_end = split_line_end(slow_after);
    INFO(output);

    constexpr size_t none = std::numeric_limits<size_t>::max();
    std::vector<std::vector<size_t>> metadata_at(metadata.size());
    size_t line_idx         = 0;
    size_t last_slow_piece  = none;
    size_t first_fast_piece = none;
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        for (size_t i = 0; i < metadata.size(); ++i)
            if (line.raw() + "\n" == metadata[i])
                metadata_at[i].push_back(line_idx);
        if (line.cmd_is("G1") && line.has_x() && line.has_e() && first_fast_piece == none) {
            if (line.x() <= 10. + EPSILON)
                last_slow_piece = line_idx;
            else
                first_fast_piece = line_idx;
        }
        ++line_idx;
    });
    REQUIRE(last_slow_piece != none);
    REQUIRE(first_fast_piece != none);
    for (size_t i = 0; i < metadata.size(); ++i) {
        CAPTURE(metadata[i]);
        REQUIRE(metadata_at[i].size() == 1);
        CHECK(metadata_at[i].front() > last_slow_piece);
        CHECK(metadata_at[i].front() < first_fast_piece);
        if (i > 0)
            CHECK(metadata_at[i].front() > metadata_at[i - 1].front());
    }

    // Read back after the start of a print in the line's role.
    GCodeProcessor processor;
    processor.apply_config(FullPrintConfig());
    processor.initialize_result_moves();
    processor.process_buffer("G90\nM83\nT0\n" + tag_line(ETags::Role, ExtrusionEntity::role_to_string(erSolidInfill)) +
                             "G1 Z0.2 F600\n" + output);
    processor.finalize(false);
    const GCodeProcessorResult &result = processor.get_result();
    REQUIRE(result.overhang_line_profiles.size() == 1);
    CHECK_THAT(result.overhang_line_profiles.front().start.x(), Catch::Matchers::WithinAbs(10., 1e-3));
    CHECK_THAT(result.overhang_line_profiles.front().end.x(), Catch::Matchers::WithinAbs(fast_end, 1e-3));
    // The four cells of the fast line are each (fast_end - 10) / 4 long, so from 10 + 0.75 * (fast_end - 10) on it
    // reads 100. The scalar 100 holds for the moves after it, and nothing before the metadata reads an overhang.
    const double unsupported_from   = 10. + 0.75 * (fast_end - 10.);
    double       unsupported_length = 0.;
    result.for_each_preview_move([&](const auto &previous, const auto &current, bool) {
        if (current.type != EMoveType::Extrude)
            return;
        const double middle   = 0.5 * (previous.position.x() + current.position.x());
        const double expected = middle > unsupported_from ? 100. : 0.;
        CHECK_THAT(current.overhang_percentage, Catch::Matchers::WithinAbs(expected, 1e-3));
        // The pieces of the fast line, which runs along y = 0 up to fast_end.
        if (middle > unsupported_from && current.position.x() < fast_end + EPSILON && current.position.y() < EPSILON)
            unsupported_length += (current.position - previous.position).norm();
    });
    CHECK_THAT(unsupported_length, Catch::Matchers::WithinAbs(0.25 * (fast_end - 10.), 1e-3));
}

// Overhang metadata closing the last layer has no line after it to ride with. The equalizer writes it when the print
// ends, once and after the moves before it, so the G-code keeps every metadata line in its place.
TEST_CASE("Overhang metadata at the end of the last layer is flushed", "[PressureEqualizer][Overhang]")
{
    const std::string metadata = tag_line(GCodeProcessor::ETags::Overhang, "5");
    const std::string output   = equalize(1., true, ";_EXTRUSION_ROLE:" + std::to_string(int(erSolidInfill)) + "\n"
        "G1 X0 Y0 F6000\n"
        "G1 E1 F2400\n"
        "G1 F600;_EXTRUDE_SET_SPEED\n"
        "G1 X10 Y0 E0.4\n"
        ";_EXTRUDE_END\n" + metadata);
    INFO(output);
    std::vector<size_t> metadata_at;
    size_t line_idx  = 0;
    size_t last_move = 0;
    size_t moves     = 0;
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.raw() + "\n" == metadata)
            metadata_at.push_back(line_idx);
        else if (line.cmd_is("G1") && line.has_x()) {
            last_move = line_idx;
            ++moves;
        }
        ++line_idx;
    });
    REQUIRE(moves > 0);
    REQUIRE(metadata_at.size() == 1);
    CHECK(metadata_at.front() > last_move);
}
