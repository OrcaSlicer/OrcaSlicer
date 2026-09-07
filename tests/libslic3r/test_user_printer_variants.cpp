// ORCA #12105: unit tests for the user-printer per-model variant machinery —
// PrinterPresetCollection::migrate_user_models_for_variants() and
// find_custom_preset_by_model_and_variant().
//
// The migration/finder key off `is_system` printer models. Rather than build a full vendor config
// bundle, we write plain printer presets into two dirs and load them in two passes — system presets
// first, then user presets — so a user preset's `inherits` resolves against an already-loaded parent
// (a single mixed dir would sort "<name> - Copy.json" before its parent "<name>.json" and drop it).
// After loading we mark the system presets is_system=true, reproducing the exact collection state
// the code under test inspects, without the vendor loader.

#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;
    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-variants-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Write one printer preset file under <root>/machine/<name>.json with the given identity fields.
void write_printer_preset(const DynamicPrintConfig &default_config, const fs::path &root, const std::string &name,
                          const std::string &printer_model, const std::string &printer_variant,
                          double nozzle_dia, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("printer_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    config.option<ConfigOptionString>("printer_model", true)->value = printer_model;
    config.option<ConfigOptionString>("printer_variant", true)->value = printer_variant;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { nozzle_dia };

    const fs::path file = root / PRESET_PRINTER_NAME / (name + ".json");
    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Load <sys_root>/machine (system presets), mark the given names is_system, then load
// <usr_root>/machine (user presets) so their inherits resolve against the loaded system parents.
void load_printers(PresetBundle &bundle, const fs::path &sys_root, const fs::path &usr_root,
                   const std::set<std::string> &system_names)
{
    PresetsConfigSubstitutions subs;
    bundle.printers.load_presets(sys_root.string(), PRESET_PRINTER_NAME, subs, ForwardCompatibilitySubstitutionRule::Disable);
    for (Preset &p : bundle.printers)
        if (system_names.count(p.name)) {
            p.is_system  = true;
            p.is_visible = true;
        }
    bundle.printers.load_presets(usr_root.string(), PRESET_PRINTER_NAME, subs, ForwardCompatibilitySubstitutionRule::Disable);
}

} // namespace

TEST_CASE("Legacy flat user printers migrate to a distinct per-model printer_model", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    // Two system nozzle presets sharing model "Fixture Printer".
    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    write_printer_preset(def, sys, "Fixture Printer 0.6 nozzle", "Fixture Printer", "0.6", 0.6);
    // Two legacy flat USER presets that carry the inherited system model.
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    write_printer_preset(def, usr, "Fixture Printer 0.6 nozzle - Copy", "Fixture Printer", "0.6", 0.6, "Fixture Printer 0.6 nozzle");

    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle", "Fixture Printer 0.6 nozzle"});

    const int migrated = bundle.printers.migrate_user_models_for_variants("Copy");
    CHECK(migrated == 2);

    // User presets now share a distinct model; names are unchanged.
    const Preset *u04 = bundle.printers.find_preset("Fixture Printer 0.4 nozzle - Copy", false);
    const Preset *u06 = bundle.printers.find_preset("Fixture Printer 0.6 nozzle - Copy", false);
    REQUIRE(u04 != nullptr);
    REQUIRE(u06 != nullptr);
    CHECK(u04->config.opt_string("printer_model") == "Fixture Printer - Copy");
    CHECK(u06->config.opt_string("printer_model") == "Fixture Printer - Copy");

    // System presets are untouched.
    const Preset *s04 = bundle.printers.find_preset("Fixture Printer 0.4 nozzle", false);
    REQUIRE(s04 != nullptr);
    CHECK(s04->config.opt_string("printer_model") == "Fixture Printer");

    SECTION("migration is idempotent") {
        CHECK(bundle.printers.migrate_user_models_for_variants("Copy") == 0);
        CHECK(u04->config.opt_string("printer_model") == "Fixture Printer - Copy");
    }
}

TEST_CASE("Migration backfills an empty printer_variant from nozzle_diameter", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    // System + legacy user preset both lacking printer_variant, but carrying a real nozzle_diameter.
    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "", 0.4);
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "", 0.4, "Fixture Printer 0.4 nozzle");
    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle"});

    CHECK(bundle.printers.migrate_user_models_for_variants("Copy") == 1);

    // After migration every user variant carries a variant string, so a later rename can always derive
    // "<model> <variant> nozzle" (never a bare model). Guarantees the invariant the rename relies on.
    const Preset *u = bundle.printers.find_preset("Fixture Printer 0.4 nozzle - Copy", false);
    REQUIRE(u != nullptr);
    CHECK(u->config.opt_string("printer_variant") == "0.4");
}

TEST_CASE("Migration disambiguates same-model/same-variant collisions with a numeric suffix", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    // Two distinct user presets both derived from the same system model + variant.
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy (1)", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");

    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle"});

    CHECK(bundle.printers.migrate_user_models_for_variants("Copy") == 2);

    std::set<std::string> models;
    for (const Preset &p : bundle.printers)
        if (p.is_user())
            models.insert(p.config.opt_string("printer_model"));
    // One gets "Fixture Printer - Copy", the other a numeric-suffixed distinct model.
    CHECK(models.size() == 2);
    CHECK(models.count("Fixture Printer - Copy") == 1);
    CHECK(models.count("Fixture Printer - Copy 2") == 1);
}

TEST_CASE("find_custom_preset_by_model_and_variant matches user variants and excludes system presets", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle"});
    bundle.printers.migrate_user_models_for_variants("Copy");

    // Resolves to the user's own variant by its distinct model.
    const Preset *user = bundle.printers.find_custom_preset_by_model_and_variant("Fixture Printer - Copy", "0.4");
    REQUIRE(user != nullptr);
    CHECK(user->name == "Fixture Printer 0.4 nozzle - Copy");
    CHECK(user->is_user());

    // The system model must NOT resolve via the custom finder (is_user() guard) even though a system
    // preset with that model+variant exists.
    CHECK(bundle.printers.find_custom_preset_by_model_and_variant("Fixture Printer", "0.4") == nullptr);
}

TEST_CASE("rename_user_printer_model trims the new model name", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle"});
    bundle.printers.migrate_user_models_for_variants("Copy"); // -> "Fixture Printer - Copy"

    const std::string old_name = "Fixture Printer 0.4 nozzle - Copy";
    const fs::path    old_json = usr / PRESET_PRINTER_NAME / (old_name + ".json");
    REQUIRE(fs::exists(old_json));

    // A padded new name is trimmed before it is stamped, and the rename now performs a REAL rename:
    // the preset name + on-disk .json move to the system-style "<model> <variant> nozzle".
    std::vector<std::pair<std::string, std::string>> renames;
    CHECK(bundle.printers.rename_user_printer_model("Fixture Printer - Copy", "  My Printer  ", &renames) == 1);

    // The old name/file are gone; the preset now resolves under the new system-style name.
    CHECK(bundle.printers.find_preset(old_name, false) == nullptr);
    CHECK_FALSE(fs::exists(old_json));
    const Preset *u = bundle.printers.find_preset("My Printer 0.4 nozzle", false);
    REQUIRE(u != nullptr);
    CHECK(u->config.opt_string("printer_model") == "My Printer");
    CHECK(fs::exists(usr / PRESET_PRINTER_NAME / "My Printer 0.4 nozzle.json"));

    // The out-param reports the old->new mapping for the caller's forward fix-ups.
    REQUIRE(renames.size() == 1);
    CHECK(renames[0].first  == old_name);
    CHECK(renames[0].second == "My Printer 0.4 nozzle");

    // A whitespace-only new name is a no-op.
    CHECK(bundle.printers.rename_user_printer_model("My Printer", "   ") == 0);

    // Renaming onto a built-in (system) model name is refused (backstop; the dialog blocks it inline).
    CHECK(bundle.printers.rename_user_printer_model("My Printer", "Fixture Printer") == 0);
    CHECK(bundle.printers.find_preset("My Printer 0.4 nozzle", false)->config.opt_string("printer_model") == "My Printer");
}

TEST_CASE("PresetBundle::rename_user_printer_model repoints app-config keys old->new", "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle"});
    bundle.printers.migrate_user_models_for_variants("Copy"); // -> "Fixture Printer - Copy"
    bundle.printers.select_preset_by_name("Fixture Printer 0.4 nozzle - Copy", true);

    const std::string old_name = "Fixture Printer 0.4 nozzle - Copy";
    AppConfig config;
    config.set("presets", PRESET_PRINTER_NAME, old_name);                  // last-selected printer
    config.set_printer_setting(old_name, PRESET_PRINTER_NAME, old_name);   // self-referential name field
    config.set_printer_setting(old_name, "curr_bed_type", "Textured PEI Plate");

    const int n = bundle.rename_user_printer_model("Fixture Printer - Copy", "My Printer", config);
    CHECK(n == 1);

    const std::string new_name = "My Printer 0.4 nozzle";
    // The per-printer settings submap moved old->new (bed type preserved), its self-ref key repointed,
    // and the last-selected-printer key follows the rename — nothing orphaned under the old name.
    CHECK(config.get_printer_setting(new_name, "curr_bed_type") == "Textured PEI Plate");
    CHECK(config.get_printer_setting(new_name, PRESET_PRINTER_NAME) == new_name);
    CHECK_FALSE(config.has_printer_settings(old_name));
    CHECK(config.get("presets", PRESET_PRINTER_NAME) == new_name);
}

TEST_CASE("get_similar_printer_preset: user model with no system counterpart resolves to a user variant",
          "[Preset][Variants][12105]")
{
    TempPresetDir temp;
    PresetBundle  bundle;
    const auto   &def = bundle.printers.default_preset().config;
    const fs::path sys = temp.path / "sys", usr = temp.path / "usr";

    write_printer_preset(def, sys, "Fixture Printer 0.4 nozzle", "Fixture Printer", "0.4", 0.4);
    write_printer_preset(def, sys, "Fixture Printer 0.6 nozzle", "Fixture Printer", "0.6", 0.6);
    write_printer_preset(def, usr, "Fixture Printer 0.4 nozzle - Copy", "Fixture Printer", "0.4", 0.4, "Fixture Printer 0.4 nozzle");
    write_printer_preset(def, usr, "Fixture Printer 0.6 nozzle - Copy", "Fixture Printer", "0.6", 0.6, "Fixture Printer 0.6 nozzle");
    load_printers(bundle, sys, usr, {"Fixture Printer 0.4 nozzle", "Fixture Printer 0.6 nozzle"});
    bundle.printers.migrate_user_models_for_variants("Copy"); // user model -> "Fixture Printer - Copy"

    // A selection provides the resolver's variant/alias context.
    bundle.printers.select_preset_by_name("Fixture Printer 0.4 nozzle - Copy", true);

    // Empty variant + a USER model (no system counterpart) resolves among the user's own variants.
    const Preset* user_res = bundle.get_similar_printer_preset("Fixture Printer - Copy", "");
    REQUIRE(user_res != nullptr);
    CHECK(user_res->is_user());
    CHECK(user_res->config.opt_string("printer_model") == "Fixture Printer - Copy");

    // Empty variant + a model WITH a system counterpart keeps the system-only behavior.
    const Preset* sys_res = bundle.get_similar_printer_preset("Fixture Printer", "");
    REQUIRE(sys_res != nullptr);
    CHECK(sys_res->is_system);
}
