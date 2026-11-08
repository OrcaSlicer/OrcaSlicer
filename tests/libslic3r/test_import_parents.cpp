#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz/miniz.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TempTree
{
    fs::path path;
    TempTree()
    {
        path = fs::temp_directory_path() / fs::unique_path("orca-import-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempTree()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path &p, const std::string &text)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string());
    out << text;
}

void add_to_zip(const fs::path &zip, const char *entry, const std::string &text)
{
    REQUIRE(mz_zip_add_mem_to_archive_file_in_place(zip.string().c_str(), entry, text.data(), text.size(), nullptr, 0, MZ_BEST_SPEED));
}

const std::string printer_json  = R"({"name":"Micron+","inherits":"Voron 2.4 250 0.4 nozzle","printer_settings_id":"Micron+","from":"User"})";
const std::string filament_json = R"({"name":"Micron ASA","inherits":"Generic ASA @System","filament_settings_id":["Micron ASA"]})";
const std::string process_json  = R"({"name":"0.2mm Voron Spec","inherits":"0.20mm Standard @Voron","print_settings_id":"0.2mm Voron Spec"})";
const std::string root_json     = R"({"name":"Root","printer_settings_id":"Root"})";
const std::string structure     = R"({"bundle_type":"printer config bundle","printer_config":["printer/Micron+.json"]})";

} // namespace

TEST_CASE("collect_import_parents reads a preset .json and every JSON inside a bundle", "[preset_import]")
{
    TempTree t;
    write_file(t.path / "single.json", printer_json);
    const fs::path zip = t.path / "Micron+.orca_printer";
    add_to_zip(zip, "printer/Micron+.json", printer_json);
    add_to_zip(zip, "filament/Micron ASA.json", filament_json);
    add_to_zip(zip, "process/0.2mm Voron Spec.json", process_json);
    add_to_zip(zip, "printer/Root.json", root_json); // no "inherits": nothing to resolve
    add_to_zip(zip, "bundle_structure.json", structure);

    using Parent = std::pair<std::string, std::string>;
    const auto parents = PresetBundle::collect_import_parents({(t.path / "single.json").string(), zip.string()});
    REQUIRE(parents.size() == 4);
    CHECK(parents[0] == Parent{PRESET_PRINTER_NAME, "Voron 2.4 250 0.4 nozzle"});
    CHECK(parents[1] == Parent{PRESET_PRINTER_NAME, "Voron 2.4 250 0.4 nozzle"});
    CHECK(parents[2] == Parent{PRESET_FILAMENT_NAME, "Generic ASA @System"});
    CHECK(parents[3] == Parent{PRESET_PRINT_NAME, "0.20mm Standard @Voron"});

    // A file that is not a zip at all is skipped rather than throwing.
    write_file(t.path / "junk.orca_printer", "not a zip");
    CHECK(PresetBundle::collect_import_parents({(t.path / "junk.orca_printer").string()}).empty());
}

TEST_CASE("find_resource_vendor_for_preset resolves a shipped preset to its vendor", "[preset_import]")
{
    TempTree          rsrc;
    const std::string previous = resources_dir();
    set_resources_dir(rsrc.path.string());
    struct Restore { const std::string &p; ~Restore() { set_resources_dir(p); } } restore{previous};

    write_file(rsrc.path / "profiles" / "Acme" / "machine" / "Acme X 0.4 nozzle.json",
               R"({"name":"Acme X 0.4 nozzle","printer_model":"Acme X","printer_variant":"0.4"})");
    write_file(rsrc.path / "profiles" / "Acme" / "machine" / "Acme Y 0.6 nozzle.json",
               R"({"name":"Acme Y 0.6 nozzle","printer_model":"Acme Y","nozzle_diameter":["0.6"]})");
    write_file(rsrc.path / "profiles" / "Acme" / "process" / "0.20mm Standard @Acme.json", R"({"name":"0.20mm Standard @Acme"})");
    write_file(rsrc.path / "profiles" / "Acme.json", R"({"name":"Acme"})");

    std::string model, variant;
    CHECK(PresetBundle::find_resource_vendor_for_preset(PRESET_PRINTER_NAME, "Acme X 0.4 nozzle", &model, &variant) == "Acme");
    CHECK(model == "Acme X");
    CHECK(variant == "0.4");

    // No printer_variant: the nozzle diameter is the variant.
    CHECK(PresetBundle::find_resource_vendor_for_preset(PRESET_PRINTER_NAME, "Acme Y 0.6 nozzle", &model, &variant) == "Acme");
    CHECK(model == "Acme Y");
    CHECK(variant == "0.6");

    // A process parent names the vendor but no printer model.
    model.clear();
    CHECK(PresetBundle::find_resource_vendor_for_preset(PRESET_PRINT_NAME, "0.20mm Standard @Acme", &model, &variant) == "Acme");
    CHECK(model.empty());

    CHECK(PresetBundle::find_resource_vendor_for_preset(PRESET_PRINTER_NAME, "Nobody 0.4 nozzle").empty());
    CHECK(PresetBundle::find_resource_vendor_for_preset(PRESET_FILAMENT_NAME, "Acme X 0.4 nozzle").empty()); // wrong type
}
