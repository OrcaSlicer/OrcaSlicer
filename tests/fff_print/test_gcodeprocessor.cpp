#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"

#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Writes the bytes verbatim: the G-code export is binary, so no newline translation must creep in
// here either, otherwise the CRLF cases below would silently test LF.
void write_gcode(const std::string &path, const std::string &content)
{
    boost::nowide::ofstream f(path, std::ios::binary);
    f << content;
}

// Cuts line `id` (1-based) out of the file the way GCodeViewer's G-code window does: the text it
// shows is bytes [lines_ends[id - 2], lines_ends[id - 1]). Reproducing that here is what makes these
// assertions about the rendered result rather than about the offsets themselves.
std::string line_at(const std::string &path, const std::vector<size_t> &lines_ends, size_t id)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    const std::string       bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t            start = id == 1 ? 0 : lines_ends[id - 2];
    return bytes.substr(start, lines_ends[id - 1] - start);
}

} // namespace

TEST_CASE("rebuild_lines_ends realigns the line offsets after the G-code is rewritten in place", "[GCodeProcessor]")
{
    ScopedTemporaryFile  tmp(".gcode");
    GCodeProcessorResult result;
    result.filename = tmp.string();

    write_gcode(tmp.string(), "G1 X1\nG1 X2\nG1 X3\n");
    REQUIRE(result.rebuild_lines_ends());
    REQUIRE(result.lines_ends.size() == 3);
    CHECK(line_at(tmp.string(), result.lines_ends, 2) == "G1 X2\n");

    // What a post-processing script that opens the file in Python text mode on Windows does: prepend a
    // comment and translate every LF to CRLF. Every byte offset shifts, and cumulatively.
    write_gcode(tmp.string(), ";EDITED\r\nG1 X1\r\nG1 X2\r\nG1 X3\r\n");
    CHECK(line_at(tmp.string(), result.lines_ends, 2) != "G1 X1\r\n");

    REQUIRE(result.rebuild_lines_ends());
    REQUIRE(result.lines_ends.size() == 4);
    CHECK(line_at(tmp.string(), result.lines_ends, 1) == ";EDITED\r\n");
    CHECK(line_at(tmp.string(), result.lines_ends, 2) == "G1 X1\r\n");
    CHECK(line_at(tmp.string(), result.lines_ends, 4) == "G1 X3\r\n");
}

TEST_CASE("rebuild_lines_ends maps a file longer than one read chunk", "[GCodeProcessor]")
{
    ScopedTemporaryFile  tmp(".gcode");
    GCodeProcessorResult result;
    result.filename = tmp.string();

    const size_t line_count = 20000;
    std::string  content;
    for (size_t i = 0; i < line_count; ++i)
        content += "G1 X" + std::to_string(i) + "\n";
    // The scan reads the file in chunks, so it has to span at least one chunk boundary to be meaningful.
    REQUIRE(content.size() > 65536);
    write_gcode(tmp.string(), content);

    REQUIRE(result.rebuild_lines_ends());
    REQUIRE(result.lines_ends.size() == line_count);
    CHECK(result.lines_ends.back() == content.size());
    CHECK(line_at(tmp.string(), result.lines_ends, line_count) == "G1 X" + std::to_string(line_count - 1) + "\n");
}

TEST_CASE("rebuild_lines_ends ignores a trailing line that has no newline", "[GCodeProcessor]")
{
    ScopedTemporaryFile  tmp(".gcode");
    GCodeProcessorResult result;
    result.filename = tmp.string();

    // Matches the export pass, which only ever records the offset one past a '\n'.
    write_gcode(tmp.string(), "G1 X1\nG1 X2");
    REQUIRE(result.rebuild_lines_ends());
    CHECK(result.lines_ends.size() == 1);
    CHECK(line_at(tmp.string(), result.lines_ends, 1) == "G1 X1\n");
}

TEST_CASE("rebuild_lines_ends empties the map when the G-code file cannot be read", "[GCodeProcessor]")
{
    // A partially rebuilt map would have the G-code window slicing lines at wrong offsets again; an
    // empty one just hides the window.
    GCodeProcessorResult result;
    result.filename   = (boost::filesystem::temp_directory_path() / "orca-nonexistent-gcode-file.gcode").string();
    result.lines_ends = {1, 2, 3};

    CHECK_FALSE(result.rebuild_lines_ends());
    CHECK(result.lines_ends.empty());
}
