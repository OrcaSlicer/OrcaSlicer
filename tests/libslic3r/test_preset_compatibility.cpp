#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>

#include "test_utils.hpp"

using namespace Slic3r;

// A printer preset saved with "Detach from parent" stores a full 1:1 copy of its source printer's
// config and drops the "inherits" link, so later updates to the source can no longer change it.
// The source's name is kept in "cloned_from" so that the process and filament profiles listing the
// source printer in "compatible_printers" stay available for the copy.
namespace {

Preset make_printer(const std::string &name, const std::string &inherits, const std::string &cloned_from)
{
    Preset printer(Preset::TYPE_PRINTER, name);
    printer.inherits() = inherits;
    printer.cloned_from() = cloned_from;
    return printer;
}

Preset make_process_for(const std::string &printer_name)
{
    Preset process(Preset::TYPE_PRINT, "0.20mm Standard @" + printer_name);
    process.config.set_key_value("compatible_printers", new ConfigOptionStrings{printer_name});
    return process;
}

bool compatible(const Preset &process, const Preset &printer)
{
    return is_compatible_with_printer(PresetWithVendorProfile(process, nullptr), PresetWithVendorProfile(printer, nullptr));
}

} // namespace

TEST_CASE("a printer preset stays compatible with the profiles of the printer it was cloned from", "[PresetCompatibility]")
{
    const Preset process = make_process_for("Source Printer 0.4 nozzle");

    SECTION("an inheriting copy is compatible, as before") {
        CHECK(compatible(process, make_printer("My Printer", "Source Printer 0.4 nozzle", "")));
    }
    SECTION("a detached copy is compatible through cloned_from") {
        CHECK(compatible(process, make_printer("My Printer", "", "Source Printer 0.4 nozzle")));
    }
    SECTION("a copy of a different printer is not compatible") {
        CHECK_FALSE(compatible(process, make_printer("My Printer", "", "Other Printer 0.6 nozzle")));
    }
    SECTION("a printer with neither link is not compatible") {
        CHECK_FALSE(compatible(process, make_printer("My Printer", "", "")));
    }
}

TEST_CASE("saving a printer preset detached copies the config and records the source printer", "[PresetCompatibility]")
{
    ScopedTemporaryDir         temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    // Loading an empty directory just points the collection at it, so the saved preset lands there.
    bundle.printers.load_presets(temp_dir.path().string(), PRESET_PRINTER_NAME, substitutions,
                                 ForwardCompatibilitySubstitutionRule::Disable);

    // Stand in for a system printer, carrying one value the copy has to bring over on its own.
    DynamicPrintConfig source_config(bundle.printers.default_preset().config);
    source_config.set_key_value("printable_height", new ConfigOptionFloat(123.0));
    Preset &source = bundle.printers.load_preset(std::string(), "Source Printer 0.4 nozzle", source_config, /*select=*/true);
    source.is_system = true;

    // Tab::save_preset seeds "cloned_from" on the edited preset before saving a detached copy.
    Preset::cloned_from(bundle.printers.get_edited_preset().config) = "Source Printer 0.4 nozzle";
    bundle.printers.save_current_preset("My Printer", /*detach=*/true, /*save_to_project=*/false);

    const Preset *copy = bundle.printers.find_preset("My Printer");
    REQUIRE(copy != nullptr);
    CHECK(copy->inherits().empty());
    CHECK(copy->cloned_from() == "Source Printer 0.4 nozzle");
    CHECK_THAT(copy->config.opt_float("printable_height"), Catch::Matchers::WithinAbs(123.0, 1e-9));
    CHECK(compatible(make_process_for("Source Printer 0.4 nozzle"), *copy));
}

// Detaching exists so the copy survives its parent going away: disabling the source printer in the
// system profiles, or a vendor dropping it. An inheriting preset is skipped when its parent cannot
// be resolved, which silently loses the printer and falls back to a system profile.
TEST_CASE("a detached printer preset still loads once its source printer is gone", "[PresetCompatibility]")
{
    ScopedTemporaryDir         temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    const auto write_printer = [&](const std::string &name, const std::string &inherits, const std::string &cloned_from) {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        Preset::inherits(config)    = inherits;
        Preset::cloned_from(config) = cloned_from;
        config.set_key_value("printable_height", new ConfigOptionFloat(123.0));
        const boost::filesystem::path file = temp_dir.path() / PRESET_PRINTER_NAME / (name + ".json");
        boost::filesystem::create_directories(file.parent_path());
        config.save_to_json(file.string(), name, "User", "1.0.0");
    };

    // Neither parent is present in the collection, standing in for a printer the user disabled.
    write_printer("Detached Copy", /*inherits=*/"", /*cloned_from=*/"Source Printer 0.4 nozzle");
    write_printer("Inheriting Copy", /*inherits=*/"Source Printer 0.4 nozzle", /*cloned_from=*/"");

    bundle.printers.load_presets(temp_dir.path().string(), PRESET_PRINTER_NAME, substitutions,
                                 ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *detached = bundle.printers.find_preset("Detached Copy");
    REQUIRE(detached != nullptr);
    CHECK(detached->cloned_from() == "Source Printer 0.4 nozzle");
    CHECK_THAT(detached->config.opt_float("printable_height"), Catch::Matchers::WithinAbs(123.0, 1e-9));
    // Nothing hides it: a user preset has no vendor, so the app config cannot mark it invisible.
    CHECK(detached->vendor == nullptr);
    // It stays compatible with the profiles of the printer it was cloned from.
    CHECK(compatible(make_process_for("Source Printer 0.4 nozzle"), *detached));

    // Contrast: the inheriting preset is dropped, which is the failure detaching avoids.
    CHECK(bundle.printers.find_preset("Inheriting Copy") == nullptr);
}

// Nozzle variants of a detached printer are children of it, so they inherit its "cloned_from".
// Saving one with a different nozzle has to re-point that at the matching system sibling, or the
// child keeps offering the source variant's process profiles.
TEST_CASE("the system sibling for a nozzle size is found by diameter", "[PresetCompatibility]")
{
    PresetBundle bundle;

    const auto add_system_variant = [&](const std::string &name, const std::string &variant, double nozzle) {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.set_key_value("printer_model", new ConfigOptionString("Elegoo OrangeStorm Giga"));
        config.set_key_value("printer_variant", new ConfigOptionString(variant));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats{nozzle});
        bundle.printers.load_preset(std::string(), name, config, /*select=*/false).is_system = true;
    };
    add_system_variant("Elegoo OrangeStorm Giga 0.4 nozzle", "0.4", 0.4);
    add_system_variant("Elegoo OrangeStorm Giga 0.6 nozzle", "0.6", 0.6);

    const Preset *match = bundle.printers.find_system_preset_by_model_and_nozzle("Elegoo OrangeStorm Giga", 0.4);
    REQUIRE(match != nullptr);
    CHECK(match->name == "Elegoo OrangeStorm Giga 0.4 nozzle");
    CHECK(match->config.opt_string("printer_variant") == "0.4");

    // A nozzle no system variant provides has no sibling, so cloned_from is left alone.
    CHECK(bundle.printers.find_system_preset_by_model_and_nozzle("Elegoo OrangeStorm Giga", 1.2) == nullptr);
    // The model has to match too, so an unrelated printer never gets re-pointed.
    CHECK(bundle.printers.find_system_preset_by_model_and_nozzle("Some Other Printer", 0.4) == nullptr);
}

// Nozzle variants of a detached printer are children of it, and only the root gets copies of the bed
// artwork. Resolving an asset has to walk up the chain, or a child shows no bed and a placeholder
// icon once the source vendor is uninstalled.
TEST_CASE("a nozzle variant inherits the bed artwork copied beside its root", "[PresetCompatibility]")
{
    ScopedTemporaryDir         temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    // A detached root lives in "base/", which load_presets loads first so parents precede children.
    const auto write_printer = [&](const std::string &name, const std::string &inherits, bool is_root) {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        Preset::inherits(config) = inherits;
        boost::filesystem::path dir = temp_dir.path() / PRESET_PRINTER_NAME;
        if (is_root)
            dir /= "base";
        const boost::filesystem::path file = dir / (name + ".json");
        boost::filesystem::create_directories(file.parent_path());
        config.save_to_json(file.string(), name, "User", "1.0.0");
        return file;
    };

    const boost::filesystem::path root_file = write_printer("SirPrintALot", "", /*is_root=*/true);
    write_printer("SirPrintALot 0.8", "SirPrintALot", /*is_root=*/false);
    // Only the root carries the copies, exactly as Tab::save_preset writes them on detach.
    const boost::filesystem::path texture = root_file.parent_path() / "SirPrintALot_bed_texture.svg";
    boost::filesystem::ofstream(texture) << "<svg/>";

    bundle.printers.load_presets(temp_dir.path().string(), PRESET_PRINTER_NAME, substitutions,
                                 ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root  = bundle.printers.find_preset("SirPrintALot");
    const Preset *child = bundle.printers.find_preset("SirPrintALot 0.8");
    REQUIRE(root != nullptr);
    REQUIRE(child != nullptr);

    CHECK(PresetUtils::detached_printer_asset(bundle.printers, *root, "_bed_texture") == texture.string());
    // The child has no copy of its own and has to find the root's.
    CHECK(PresetUtils::detached_printer_asset(bundle.printers, *child, "_bed_texture") == texture.string());
    // An asset nobody in the family provides stays unresolved rather than matching something else.
    CHECK(PresetUtils::detached_printer_asset(bundle.printers, *child, "_bed_model").empty());
}

// The printer dropdown collapses a custom printer's nozzle variants into one entry keyed on the
// inheritance root, mirroring how system presets collapse by printer_model. Detaching starts a new
// root, so detaching once per nozzle size yields separate entries - variants have to be children.
TEST_CASE("nozzle variants group under one root, separate detaches do not", "[PresetCompatibility]")
{
    PresetBundle bundle;

    const auto add_printer = [&](const std::string &name, const std::string &inherits) {
        DynamicPrintConfig config(bundle.printers.default_preset().config);
        config.set_key_value("printer_model", new ConfigOptionString("Elegoo OrangeStorm Giga"));
        Preset::inherits(config) = inherits;
        bundle.printers.load_preset(std::string(), name, config, /*select=*/false);
    };

    add_printer("SirPrintALot", "");                    // detached once: the family root
    add_printer("SirPrintALot 0.8", "SirPrintALot");    // child, same family
    add_printer("SirPrintALot 0.4", "SirPrintALot 0.8");// grandchild, still the same family
    add_printer("Detached Again", "");                  // a second detach: its own family

    auto &printers = bundle.printers;
    const auto root_of = [&](const std::string &name) {
        const Preset *preset = printers.find_preset(name);
        REQUIRE(preset != nullptr);
        return printers.family_root_name(*preset);
    };

    // Naming is irrelevant - lineage decides. All three collapse to the one entry "SirPrintALot".
    CHECK(root_of("SirPrintALot") == "SirPrintALot");
    CHECK(root_of("SirPrintALot 0.8") == "SirPrintALot");
    CHECK(root_of("SirPrintALot 0.4") == "SirPrintALot");
    // Detaching a second time makes a separate entry even though the printer_model is identical.
    CHECK(root_of("Detached Again") == "Detached Again");
}

// Deleting a detached printer has to take its copied artwork with it, or the profile directory
// accumulates bed models, textures and covers that no preset refers to any more.
TEST_CASE("deleting a detached printer removes the artwork copied beside it", "[PresetCompatibility]")
{
    ScopedTemporaryDir temp_dir;
    PresetBundle       bundle;

    const boost::filesystem::path dir = temp_dir.path() / PRESET_PRINTER_NAME / "base";
    boost::filesystem::create_directories(dir);

    DynamicPrintConfig config(bundle.printers.default_preset().config);
    const boost::filesystem::path file = dir / "SirPrintALot.json";
    config.save_to_json(file.string(), "SirPrintALot", "User", "1.0.0");

    std::vector<boost::filesystem::path> assets;
    for (const std::string &name : {"SirPrintALot_bed_model.stl", "SirPrintALot_bed_texture.svg", "SirPrintALot_cover.png"}) {
        assets.push_back(dir / name);
        boost::filesystem::ofstream(assets.back()) << "x";
    }
    // A neighbouring preset's artwork must survive: the prefix match has to be exact.
    const boost::filesystem::path other = dir / "SirPrintALotOther_cover.png";
    boost::filesystem::ofstream(other) << "x";

    PresetsConfigSubstitutions substitutions;
    bundle.printers.load_presets(temp_dir.path().string(), PRESET_PRINTER_NAME, substitutions,
                                 ForwardCompatibilitySubstitutionRule::Disable);
    Preset *preset = bundle.printers.find_preset("SirPrintALot");
    REQUIRE(preset != nullptr);

    preset->remove_files(/*cloud_already_deleted=*/true);

    CHECK_FALSE(boost::filesystem::exists(file));
    for (const auto &asset : assets)
        CHECK_FALSE(boost::filesystem::exists(asset));
    CHECK(boost::filesystem::exists(other));
}
