#include <catch2/catch_all.hpp>

#include <boost/nowide/fstream.hpp>
#include <string>
#include <ios>
#include <array>
#include <cstddef>
#include <cmath>
#include <functional>
#include <iterator>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Standard_Handle.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDataStd_Name.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Compound.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/ProjectStrategy.hpp"
#include "libslic3r/TriangleMesh.hpp"
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

TEST_CASE("A security classification assignment does not crash import", "[Step]")
{
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "security_classification.step";

    Model model;
    bool  cancel = false;
    Step  step(path);

    REQUIRE(step.load() == Step::Step_Status::LOAD_SUCCESS);
    REQUIRE(step.mesh(&model, cancel, false) == Step::Step_Status::MESH_SUCCESS);

    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() == 4); // a tetrahedron
}

// The fixture is a truncated cone whose seam pcurves have the direction (2.1e-16, -1).
TEST_CASE("A cone with a slightly tilted seam imports as a closed mesh", "[Step]")
{
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "cone_tilted_seam_pcurve.step";

    Model model;
    bool  cancel = false;
    Step  step(path);

    REQUIRE(step.load() == Step::Step_Status::LOAD_SUCCESS);
    REQUIRE(step.mesh(&model, cancel, false) == Step::Step_Status::MESH_SUCCESS);

    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(its_num_open_edges(model.objects.front()->volumes.front()->mesh().its) == 0);
}

// The fixture holds two trimmed planes in a geometric set and no shells or solids, as some CAD exporters write
// surface models, so OCCT reads it as a compound of loose faces.
TEST_CASE("A surface model imports whether or not compounds are split", "[Step]")
{
    const std::string path  = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "surface_model_geometric_set.step";
    const bool        split = GENERATE(false, true);
    CAPTURE(split);

    Model model;
    bool  cancel = false;
    Step  step(path);

    REQUIRE(step.load() == Step::Step_Status::LOAD_SUCCESS);
    REQUIRE(step.mesh(&model, cancel, split) == Step::Step_Status::MESH_SUCCESS);

    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() > 0);
}

// Three tetrahedra share one product, while each BREP carries its own independent name.
TEST_CASE("Original STEP body names stay attached to their own compound geometry", "[Step][OriginalNames]")
{
    const bool keep_names = GENERATE(false, true);
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_compound_bodies.step";
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances;
    if (keep_names)
        strategy = strategy | LoadStrategy::KeepOriginalNames;

    Model model = Model::read_from_step(path, strategy, nullptr, nullptr, nullptr, 0.003, 0.5, true);
    REQUIRE(model.objects.size() == 1);
    const ModelObject *object = model.objects.front();
    REQUIRE(object->volumes.size() == 3);
    REQUIRE(object->instances.size() == 1);

    const std::array<std::string, 3> names = {"Part10", "Part2", "\xD0\xA2\xD0\xB5\xD0\xBB\xD0\xBE & shell"};
    for (size_t i = 0; i < names.size(); ++i) {
        CAPTURE(i, keep_names);
        const ModelVolume *volume = object->volumes[i];
        CHECK(volume->name == (keep_names ? names[i] : "Body collection-SOLID-" + std::to_string(i + 1)));
        const BoundingBoxf3 bounds = volume->mesh().transformed_bounding_box(volume->get_matrix());
        // Distinct X positions verify identity, rather than only comparing the list of names.
        CHECK_THAT(bounds.min.x(), Catch::Matchers::WithinAbs(25.0 * i, 1e-5));
        CHECK_THAT(bounds.max.x(), Catch::Matchers::WithinAbs(25.0 * i + 10.0, 1e-5));
        CHECK_THAT(bounds.size().y(), Catch::Matchers::WithinAbs(10.0, 1e-5));
        CHECK_THAT(bounds.size().z(), Catch::Matchers::WithinAbs(10.0, 1e-5));
        CHECK(volume->mesh().facets_count() == 4);
    }
}

TEST_CASE("A STEP body name takes precedence over its enclosing product name only when requested", "[Step][OriginalNames]")
{
    const bool keep_names = GENERATE(false, true);
    const bool split = GENERATE(false, true);
    CAPTURE(keep_names, split);
    LoadStrategy strategy = LoadStrategy::LoadModel;
    if (keep_names)
        strategy = strategy | LoadStrategy::KeepOriginalNames;

    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_single_body.step";
    Model model = Model::read_from_step(path, strategy, nullptr, nullptr, nullptr, 0.003, 0.5, split);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->name == (keep_names ? "Body name" : "Body collection"));
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() == 4);
    // A single part is not listed, so the object carries the body name; otherwise the file name stays.
    CHECK(model.objects.front()->name == (keep_names ? std::string("Body name") : std::string("named_single_body.step")));
}

TEST_CASE("The STEP import dialog can change original names after loading the CAD document", "[Step][OriginalNames]")
{
    const bool initial_keep = GENERATE(false, true);
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::AssembleParts;
    if (initial_keep)
        strategy = strategy | LoadStrategy::KeepOriginalNames;
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_single_body.step";
    auto callback = [initial_keep](Step& step, double&, double&, bool&) {
        CHECK(step.keep_original_names() == initial_keep);
        step.set_keep_original_names(!initial_keep);
        return 1;
    };
    Model model = Model::read_from_step(path, strategy, nullptr, nullptr, callback, 0.003, 0.5, true);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->name == (initial_keep ? "Body collection" : "Body name"));
}

TEST_CASE("STEP geometry import honours Split compound like the legacy import", "[Step][OriginalNames]")
{
    const bool split = GENERATE(false, true);
    CAPTURE(split);
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_compound_bodies.step";
    Model model = Model::read_from_step(path, LoadStrategy::LoadModel | LoadStrategy::AssembleParts,
                                       nullptr, nullptr, nullptr, 0.003, 0.5, split);
    REQUIRE(model.objects.size() == 1);
    if (split) {
        REQUIRE(model.objects.front()->volumes.size() == 3);
        CHECK(model.objects.front()->volumes[0]->name == "Body collection-SOLID-1");
        CHECK(model.objects.front()->volumes[1]->name == "Body collection-SOLID-2");
    } else {
        // An unsplit compound stays one part, as before this feature.
        REQUIRE(model.objects.front()->volumes.size() == 1);
        CHECK(model.objects.front()->volumes.front()->name == "Body collection");
    }
}

TEST_CASE("One body beside helper geometry keeps its name without Split compound", "[Step][OriginalNames]")
{
    const std::string source_path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_single_body.step";
    boost::nowide::ifstream input(source_path, std::ios::binary);
    REQUIRE(input.good());
    std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    // A curve set next to the body makes OCCT return a compound that holds a single solid.
    const std::string representation = "#20=FACETED_BREP_SHAPE_REPRESENTATION('',(#21,#22),#17);";
    const size_t representation_pos = data.find(representation);
    REQUIRE(representation_pos != std::string::npos);
    data.replace(representation_pos, representation.size(),
                 "#20=SHAPE_REPRESENTATION('',(#21,#22,#24),#17);\n#24=GEOMETRIC_CURVE_SET('',(#25));\n#25=POLYLINE('',(#30,#31));");
    ScopedTemporaryFile file(".step");
    {
        boost::nowide::ofstream output(file.string(), std::ios::binary);
        REQUIRE(output.good());
        output << data;
    }
    // Prove the fixture reaches the compound branch rather than the plain solid one.
    STEPControl_Reader reader;
    REQUIRE(reader.ReadFile(file.string().c_str()) == IFSelect_RetDone);
    REQUIRE(reader.TransferRoots() > 0);
    REQUIRE(reader.OneShape().ShapeType() == TopAbs_COMPOUND);

    Step step(file.string());
    REQUIRE(step.load(true) == Step::Step_Status::LOAD_SUCCESS);
    CHECK_FALSE(step.has_multi_body_compound());

    Model model = Model::read_from_step(file.string(), LoadStrategy::LoadModel | LoadStrategy::KeepOriginalNames,
                                       nullptr, nullptr, nullptr, 0.003, 0.5, false);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->name == "Body name");
    CHECK(model.objects.front()->name == "Body name");
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() == 4);
}

TEST_CASE("A loaded STEP document reports its body labels and multi-body compounds", "[Step][OriginalNames]")
{
    const bool compound = GENERATE(false, true);
    CAPTURE(compound);
    const std::string path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR +
        (compound ? "named_compound_bodies.step" : "named_single_body.step");
    Step step(path);
    REQUIRE(step.load(true) == Step::Step_Status::LOAD_SUCCESS);
    CHECK(step.has_original_names());
    // Only a compound of several bodies needs Split compound for its names.
    CHECK(step.has_multi_body_compound() == compound);
}

TEST_CASE("Empty STEP body names retain the product fallback and raw Unicode names survive", "[Step][OriginalNames]")
{
    const std::string body_name = GENERATE(std::string(), std::string("\xD0\xA2\xD0\xB5\xD0\xBB\xD0\xBE"));
    const std::string source_path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_single_body.step";
    boost::nowide::ifstream input(source_path, std::ios::binary);
    REQUIRE(input.good());
    std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const size_t name_pos = data.find("'Body name'");
    REQUIRE(name_pos != std::string::npos);
    // Change only the body metadata; the product label and geometry remain independent controls.
    data.replace(name_pos, std::string("'Body name'").size(), "'" + body_name + "'");
    ScopedTemporaryFile file(".STEP");
    {
        boost::nowide::ofstream output(file.string(), std::ios::binary);
        REQUIRE(output.good());
        output << data;
    }
    Model model = Model::read_from_step(file.string(), LoadStrategy::LoadModel | LoadStrategy::KeepOriginalNames,
                                       nullptr, nullptr, nullptr, 0.003, 0.5, true);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->name == (body_name.empty() ? "Body collection" : body_name));
}

TEST_CASE("Equal STEP body names remain equal without losing their individual geometry", "[Step][OriginalNames]")
{
    const std::string source_path = std::string(TEST_DATA_DIR) + PATH_SEPARATOR "named_compound_bodies.step";
    boost::nowide::ifstream input(source_path, std::ios::binary);
    REQUIRE(input.good());
    std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const size_t name_pos = data.find("'Part2'");
    REQUIRE(name_pos != std::string::npos);
    // Duplicate only the label, so matching by name would assign the wrong geometry to a sibling.
    data.replace(name_pos, std::string("'Part2'").size(), "'Part10'");
    ScopedTemporaryFile file(".stp");
    {
        boost::nowide::ofstream output(file.string(), std::ios::binary);
        REQUIRE(output.good());
        output << data;
    }
    Model model = Model::read_from_step(file.string(), LoadStrategy::LoadModel | LoadStrategy::KeepOriginalNames,
                                       nullptr, nullptr, nullptr, 0.003, 0.5, true);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 3);
    for (size_t i = 0; i < 2; ++i) {
        const ModelVolume *volume = model.objects.front()->volumes[i];
        CHECK(volume->name == "Part10");
        CHECK_THAT(volume->mesh().transformed_bounding_box(volume->get_matrix()).min.x(),
                   Catch::Matchers::WithinAbs(25.0 * i, 1e-5));
    }
}

TEST_CASE("Repeated STEP assembly prototypes keep each instance placement", "[Step][OriginalNames]")
{
    ScopedTemporaryFile file(".step");
    Handle(TDocStd_Document) document;
    const auto application = XCAFApp_Application::GetApplication();
    application->NewDocument(TCollection_ExtendedString("BinXCAF"), document);
    const auto shape_tool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const TDF_Label prototype = shape_tool->AddShape(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), false);
    TDataStd_Name::Set(prototype, "Repeated prototype");

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const TDF_Label assembly = shape_tool->AddShape(compound, true);
    TDataStd_Name::Set(assembly, "Assembly");
    gp_Trsf first_location;
    first_location.SetTranslation(gp_Vec(25.0, 0.0, 5.0));
    shape_tool->AddComponent(assembly, prototype, TopLoc_Location(first_location));
    gp_Trsf second_location;
    second_location.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 0.5 * std::acos(-1.0));
    second_location.SetTranslationPart(gp_Vec(100.0, 40.0, 0.0));
    shape_tool->AddComponent(assembly, prototype, TopLoc_Location(second_location));
    shape_tool->UpdateAssemblies();

    STEPCAFControl_Writer writer;
    REQUIRE(writer.Transfer(document));
    REQUIRE(writer.Write(file.string().c_str()) == IFSelect_RetDone);
    application->Close(document);

    Model model = Model::read_from_step(file.string(), LoadStrategy::LoadModel | LoadStrategy::KeepOriginalNames,
                                       nullptr, nullptr, nullptr, 0.003, 0.5, true);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 2);
    // Both volumes use the shared prototype name; their independent transforms must not be merged.
    for (const ModelVolume *volume : model.objects.front()->volumes)
        CHECK(volume->name == "Repeated prototype");
    const auto first = model.objects.front()->volumes[0];
    const auto second = model.objects.front()->volumes[1];
    const auto first_bounds = first->mesh().transformed_bounding_box(first->get_matrix());
    const auto second_bounds = second->mesh().transformed_bounding_box(second->get_matrix());
    CHECK_THAT(first_bounds.min.x(), Catch::Matchers::WithinAbs(25.0, 1e-5));
    CHECK_THAT(first_bounds.min.z(), Catch::Matchers::WithinAbs(5.0, 1e-5));
    CHECK_THAT(first_bounds.size().x(), Catch::Matchers::WithinAbs(10.0, 1e-5));
    CHECK_THAT(second_bounds.min.x(), Catch::Matchers::WithinAbs(80.0, 1e-5));
    CHECK_THAT(second_bounds.min.y(), Catch::Matchers::WithinAbs(40.0, 1e-5));
    CHECK_THAT(second_bounds.size().x(), Catch::Matchers::WithinAbs(20.0, 1e-5));
    CHECK_THAT(second_bounds.size().y(), Catch::Matchers::WithinAbs(10.0, 1e-5));
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
