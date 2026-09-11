#include <catch2/catch_all.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/OrcaPrinterAgent.hpp"

using json = nlohmann::json;
using namespace Slic3r;

namespace {

// Point resources_dir() at the repo's own tree for the lifetime of a test and restore it
// afterwards, mirroring ScopedResourcesDir (which only ever makes a throwaway directory).
// PROFILES_DIR is <repo>/resources/profiles; the map lives in <repo>/resources/printers.
struct ScopedRepoResourcesDir
{
    std::string previous{resources_dir()};

    ScopedRepoResourcesDir() { set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string()); }
    ~ScopedRepoResourcesDir() { set_resources_dir(previous); }

    ScopedRepoResourcesDir(const ScopedRepoResourcesDir&)            = delete;
    ScopedRepoResourcesDir& operator=(const ScopedRepoResourcesDir&) = delete;
};

std::string orca_id_of(const std::string& bambu_id)
{
    boost::nowide::ifstream file(resources_dir() + "/printers/bambu_filament_ids.json");
    json doc;
    file >> doc;
    for (const auto& [orca_id, row] : doc["filaments"].items())
        if (row["bambu_id"] == bambu_id)
            return orca_id;
    FAIL("no map row for " << bambu_id);
    return {};
}

} // namespace

TEST_CASE("Bambu filament id map is one-to-one and leaves unmapped ids alone", "[BambuFilamentIds]")
{
    const ScopedRepoResourcesDir repo_resources;
    const BBLPrinterAgent bbl;
    const std::string abs = orca_id_of("GFB00");                      // Bambu ABS
    REQUIRE(abs.rfind("OF", 0) == 0);
    CHECK(bbl.from_orca_filament_id(abs) == "GFB00");
    CHECK(bbl.to_orca_filament_id("GFB00") == abs);
    CHECK(bbl.from_orca_filament_id("OFnotarow") == "OFnotarow");
    CHECK(bbl.to_orca_filament_id("GFZZ99") == "GFZZ99");             // Bambu id we do not ship
    CHECK(bbl.to_orca_filament_id("P1234567") == "P1234567");         // user root
    CHECK(bbl.from_orca_filament_id("") == "");

    // An agent whose printers already speak our ids inherits IPrinterAgent's identity default.
    const OrcaPrinterAgent other{""};
    CHECK(other.from_orca_filament_id(abs) == abs);
    CHECK(other.to_orca_filament_id("GFB00") == "GFB00");
}

TEST_CASE("Payload rewrite covers nested trays, calibration lists and mapping info", "[BambuFilamentIds]")
{
    const ScopedRepoResourcesDir repo_resources;
    const std::string abs = orca_id_of("GFB00"), pla = orca_id_of("GFA00");
    const std::string status =
        R"({"print":{"ams":{"ams":[{"tray":[{"tray_info_idx":"GFB00"},{"tray_info_idx":"GFZZ99"}]}]},)"
        R"("vt_tray":{"tray_info_idx":"GFA00"},"filaments":[{"filament_id":"GFA00","setting_id":"GFB00"}]}})";
    json inbound = json::parse(BBLPrinterAgent::to_orca_payload(status));
    CHECK(inbound["print"]["ams"]["ams"][0]["tray"][0]["tray_info_idx"] == abs);
    CHECK(inbound["print"]["ams"]["ams"][0]["tray"][1]["tray_info_idx"] == "GFZZ99");    // no row: untouched
    CHECK(inbound["print"]["vt_tray"]["tray_info_idx"] == pla);
    CHECK(inbound["print"]["filaments"][0]["filament_id"] == pla);
    CHECK(inbound["print"]["filaments"][0]["setting_id"] == "GFB00");                    // not an id key: has a map row but must stay put
    CHECK(json::parse(BBLPrinterAgent::from_orca_payload(inbound.dump())) == json::parse(status));    // round trip

    const std::string mapping = R"([{"ams":0,"filamentId":")" + abs + R"(","filamentType":"ABS"}])";
    CHECK(BBLPrinterAgent::from_orca_payload(mapping) == R"([{"ams":0,"filamentId":"GFB00","filamentType":"ABS"}])");
    CHECK(BBLPrinterAgent::from_orca_payload("not json") == "not json");
    CHECK(BBLPrinterAgent::from_orca_payload(R"({"print":{"command":"pushall"}})") == R"({"print":{"command":"pushall"}})");
}
