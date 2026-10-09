#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/AMF.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

#include <ios>
#include <string>

using namespace Slic3r;

namespace {

// The smallest AMF the loader accepts: one object holding one volume, a tetrahedron. The metadata
// element of the object is dropped in verbatim, so a test can hand the parser a malformed one.
std::string amf_with_object_metadata(const std::string &object_metadata)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<amf unit=\"millimeter\">\n"
           " <object id=\"0\">\n"
           "  " + object_metadata + "\n"
           "  <mesh>\n"
           "   <vertices>\n"
           "    <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>\n"
           "    <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>\n"
           "    <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>\n"
           "    <vertex><coordinates><x>0</x><y>0</y><z>1</z></coordinates></vertex>\n"
           "   </vertices>\n"
           "   <volume>\n"
           "    <triangle><v1>0</v1><v2>2</v2><v3>1</v3></triangle>\n"
           "    <triangle><v1>0</v1><v2>1</v2><v3>3</v3></triangle>\n"
           "    <triangle><v1>0</v1><v2>3</v2><v3>2</v3></triangle>\n"
           "    <triangle><v1>1</v1><v2>2</v2><v3>3</v3></triangle>\n"
           "   </volume>\n"
           "  </mesh>\n"
           " </object>\n"
           "</amf>\n";
}

void write_file(const std::string &path, const std::string &content)
{
    boost::nowide::ofstream f(path, std::ios::binary);
    f << content;
}

bool load(const std::string &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
    return load_amf(path.c_str(), &config, &substitutions, &model, nullptr);
}

} // namespace

TEST_CASE("An AMF object metadata element with no type attribute is rejected", "[AMF]")
{
    ScopedTemporaryFile tmp(".amf");

    SECTION("with the attribute the file loads")
    {
        write_file(tmp.string(), amf_with_object_metadata("<metadata type=\"name\">tetra</metadata>"));

        Model model;
        REQUIRE(load(tmp.string(), model));
        CHECK(model.objects.size() == 1);
    }

    SECTION("without it the load fails instead of reading a null attribute")
    {
        write_file(tmp.string(), amf_with_object_metadata("<metadata>tetra</metadata>"));

        Model model;
        CHECK_FALSE(load(tmp.string(), model));
    }
}
