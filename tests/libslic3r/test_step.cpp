#include <catch2/catch_all.hpp>

#include <boost/nowide/fstream.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

static void write_step_line(const std::string &path, const std::string &line)
{
    boost::nowide::ofstream file(path, std::ios::binary);
    file << "ISO-10303-21;\n" << line << "\nEND-ISO-10303-21;\n";
}

// preprocess() hands back the input path unless it transcoded into a temporary.
static std::string preprocess_result(const std::string &line)
{
    ScopedSlic3rTemporaryDir scratch;

    ScopedTemporaryFile step(".step");
    write_step_line(step.string(), line);

    std::string      output_path;
    StepPreProcessor preprocessor;
    REQUIRE(preprocessor.preprocess(step.string().c_str(), output_path));

    return output_path == step.string() ? "untouched" : "transcoded";
}

// data/utf8_part_names.step is three boxes written by OCCT's own STEP writer, whose
// PRODUCT names were then patched to raw UTF-8. Most CAD exporters write non-ASCII names
// that way rather than in the \X2\ escape form. The third part is ASCII, as a control.
TEST_CASE("Part names with multi-byte UTF-8 survive import", "[Step]")
{
    // getNamedSolids() replaces a name that isUtf8() rejects with a running number.
    const std::string path = TEST_DATA_DIR PATH_SEPARATOR "utf8_part_names.step";

    Model model;
    bool  cancel = false;
    Step  step(path); // no isUtf8Fn, matching how Model::read_from_step builds it

    REQUIRE(step.load() == Step::Step_Status::LOAD_SUCCESS);
    REQUIRE(step.mesh(&model, cancel, false) == Step::Step_Status::MESH_SUCCESS);

    REQUIRE(model.objects.size() == 1);
    const ModelObject *object = model.objects.front();
    REQUIRE(object->volumes.size() == 3);
    // "ce" is split off, or the hex escape would swallow it as further hex digits.
    CHECK(object->volumes[0]->name == "pi\xC3\xA8" "ce");
    CHECK(object->volumes[1]->name == "Geh\xC3\xA4use");
    CHECK(object->volumes[2]->name == "bracket");
}

TEST_CASE("isUtf8 recognises two, three and four byte sequences", "[Step]")
{
    CHECK(StepPreProcessor::isUtf8("\xC3\xA9"));         // U+00E9
    CHECK(StepPreProcessor::isUtf8("\xE4\xB8\xAD"));     // U+4E2D
    CHECK(StepPreProcessor::isUtf8("\xF0\x9F\x94\xA9")); // U+1F529
    CHECK_FALSE(StepPreProcessor::isUtf8("\x81\x30"));   // 0x81 is not a lead byte
    CHECK_FALSE(StepPreProcessor::isUtf8("\xC3"));       // truncated sequence
}

// The only caller of isGBK is preprocess(), which nothing calls today.
TEST_CASE("Encoding detection decides whether a step file is transcoded", "[Step]")
{
    SECTION("UTF-8, so left alone")
    {
        // A two byte sequence also satisfies every GBK range, so misdetecting it as
        // not-UTF-8 sends it to be transcoded.
        const std::string sequence = GENERATE(std::string("\xC3\xA9"),          // U+00E9
                                              std::string("\xE4\xB8\xAD"),      // U+4E2D
                                              std::string("\xF0\x9F\x94\xA9")); // U+1F529

        CHECK(preprocess_result("NAME('" + sequence + "');") == "untouched");
    }

    SECTION("neither UTF-8 nor GBK, so left alone")
    {
        // 0x81 is not a UTF-8 lead byte, and 0x30 is below the 0x40 floor for a GBK trail.
        CHECK(preprocess_result("NAME('\x81\x30');") == "untouched");
    }

    SECTION("GBK, so transcoded")
    {
        // U+554A in GBK, whose lead byte is not valid UTF-8. Pins the other direction,
        // since a detector that never reports GBK would pass every case above.
        CHECK(preprocess_result("NAME('\xB0\xA1');") == "transcoded");
    }

    SECTION("plain ASCII, so left alone") { CHECK(preprocess_result("NAME('bracket');") == "untouched"); }
}
