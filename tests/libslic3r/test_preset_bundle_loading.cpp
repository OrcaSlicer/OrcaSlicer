#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"

#include "test_utils.hpp"

#include <algorithm>

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

// Whether a key is listed in a vector of keys (published_keys / skipped_keys).
bool contains_key(const std::vector<std::string> &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Write a preset json carrying a name and an "inherits" value, using the given collection's
// default config so it loads back into that collection. Works for any preset type.
void write_preset_with_inherits(const DynamicPrintConfig &default_config, const fs::path &file,
                                const std::string &name, const std::string &inherits)
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// Mark an already-loaded preset as renamed from one or more former names.
void set_renamed_from(PresetCollection &coll, const std::string &preset_name, std::vector<std::string> old_names)
{
    for (auto it = coll.begin(); it != coll.end(); ++it)
        if (it->name == preset_name)
            it->renamed_from = std::move(old_names);
}

// A single-slot PLA file config for the published-material load tests (mirrors what the GUI
// builds from a 3mf's project settings before load_config_model).
DynamicPrintConfig published_pla_file_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
    config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
    config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
    config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
    config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
    return config;
}

// A standalone print preset collection that exposes the protected rename-map builder, so a
// renamed_from scenario can be set up without the full system-profile load pipeline.
// (PresetCollection is non-copyable - it holds a mutex - so it is constructed directly with
// the same type/keys/defaults PresetBundle uses for its print collection.)
struct RenameTestCollection : public PresetCollection
{
    RenameTestCollection()
        : PresetCollection(Preset::TYPE_PRINT, Preset::print_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    ScopedTemporaryDir         temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path() / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path() / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path() / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path().string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path() / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path() / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    ScopedTemporaryDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path() / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path() / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

TEST_CASE("Printer extruder count tolerates missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("find_preset resolves a system preset's renamed_from", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" is the current preset; it was renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // The rename map knows the old name...
    const std::string *renamed = coll.get_preset_name_renamed("Old Process");
    REQUIRE(renamed != nullptr);
    CHECK(*renamed == "New Process");

    // ...and plain find_preset() now follows it (the core of this PR; previously this
    // resolution lived only in find_preset2 and a few call sites).
    const Preset *resolved = coll.find_preset("Old Process");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "New Process");

    // A genuinely unknown name still returns null (no spurious match).
    CHECK(coll.find_preset("Totally Unknown") == nullptr);

    // A child that still inherits the OLD name resolves through the runtime walker,
    // which uses plain find_preset().
    Preset       &child  = add_inmemory_preset(coll, "Child Process", "Old Process");
    const Preset *parent = coll.get_preset_parent(child);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == "New Process");
}

TEST_CASE("find_preset resolves a preset renamed more than once", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" was renamed twice, so it carries both former names in renamed_from.
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Original Process", "Old Process" });
    coll.update_map_system_profile_renamed();

    // Each historical name resolves to the current preset.
    for (const char *old_name : { "Original Process", "Old Process" }) {
        INFO("resolving old name: " << old_name);
        const std::string *renamed = coll.get_preset_name_renamed(old_name);
        REQUIRE(renamed != nullptr);
        CHECK(*renamed == "New Process");

        const Preset *resolved = coll.find_preset(old_name);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == "New Process");
    }

    // A child inheriting either former name resolves through the runtime walker.
    Preset &child = add_inmemory_preset(coll, "Child Process", "Original Process");
    REQUIRE(coll.get_preset_parent(child) != nullptr);
    CHECK(coll.get_preset_parent(child)->name == "New Process");
}

TEST_CASE("find_preset2 auto-matches removed Generic vendor profiles to the library", "[Preset][Rename]")
{
    PresetBundle bundle;

    // The OrcaFilamentLibrary replacement that removed empty "<vendor> Generic" profiles map to.
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // Plain lookups do NOT fuzzy-match a removed vendor profile.
    CHECK(bundle.filaments.find_preset("Voron Generic PLA") == nullptr);
    CHECK(bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/false) == nullptr);

    // With auto_match, the removed "Voron Generic PLA" resolves to "Generic PLA @System".
    const Preset *matched = bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/true);
    REQUIRE(matched != nullptr);
    CHECK(matched->name == "Generic PLA @System");

    // No library preset exists for an unrelated material => still no match.
    CHECK(bundle.filaments.find_preset2("BrandX Generic PETG", /*auto_match=*/true) == nullptr);
}

TEST_CASE("Renamed parent is normalized into a loaded preset's inherits", "[Preset][Rename]")
{
    ScopedTemporaryDir   temp_dir;
    RenameTestCollection coll;

    // Current parent, renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // A user preset on disk that still inherits the OLD name.
    write_preset_with_inherits(coll.default_preset().config,
                               temp_dir.path() / PRESET_PRINT_NAME / "Child.json", "Child", "Old Process");

    PresetsConfigSubstitutions substitutions;
    coll.load_presets(temp_dir.path().string(), PRESET_PRINT_NAME, substitutions,
                      ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = coll.find_preset("Child");
    REQUIRE(child != nullptr);
    // The dangling "Old Process" was rewritten to the resolved parent name at load time,
    // so the runtime walker (plain find_preset) can resolve the chain.
    CHECK(child->inherits() == "New Process");
    REQUIRE(coll.get_preset_parent(*child) != nullptr);
    CHECK(coll.get_preset_parent(*child)->name == "New Process");
}

TEST_CASE("Removed Generic parent is normalized into a loaded filament's inherits", "[Preset][Rename]")
{
    ScopedTemporaryDir temp_dir;
    PresetBundle  bundle;

    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // A user filament that still inherits a removed "<vendor> Generic PLA" profile.
    write_preset_with_inherits(bundle.filaments.default_preset().config,
                               temp_dir.path() / PRESET_FILAMENT_NAME / "MyPLA.json", "MyPLA", "Voron Generic PLA");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(temp_dir.path().string(), PRESET_FILAMENT_NAME, substitutions,
                                  ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = bundle.filaments.find_preset("MyPLA");
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Generic PLA @System");
    REQUIRE(bundle.filaments.get_preset_parent(*child) != nullptr);
    CHECK(bundle.filaments.get_preset_parent(*child)->name == "Generic PLA @System");
}

namespace {

// A live reference to a preset's compatible_printers / compatible_prints list. Fetches the *stored*
// preset (real=true) so writes and reads hit the same object; creates the option if absent.
std::vector<std::string> &compatible_list(PresetCollection &coll, const std::string &preset_name, const char *field_key)
{
    Preset *preset = coll.find_preset(preset_name, /*first_visible_if_not_found=*/false, /*real=*/true);
    REQUIRE(preset != nullptr);
    return preset->config.option<ConfigOptionStrings>(field_key, true)->values;
}

} // namespace

TEST_CASE("Renamed printer/process names are normalized into compatible lists on load", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A user process still compatible with the OLD printer name.
    add_inmemory_preset(bundle.prints, "My Process");
    compatible_list(bundle.prints, "My Process", "compatible_printers") = { "Old Printer" };

    // A user filament referencing the OLD printer AND OLD process names, plus an unknown printer.
    add_inmemory_preset(bundle.filaments, "My Filament");
    compatible_list(bundle.filaments, "My Filament", "compatible_printers") = { "Old Printer", "Unknown Printer" };
    compatible_list(bundle.filaments, "My Filament", "compatible_prints")   = { "Old Process" };

    // Build the rename maps (done during system load in the real pipeline), then normalize.
    AppConfig app_config;
    bundle.load_installed_printers(app_config); // rebuilds every collection's rename map
    bundle.normalize_compatible_presets();

    // The stale printer name in a process' compatible_printers is rewritten to the current name.
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });

    // The stale process name in a filament's compatible_prints is rewritten (this field has no
    // runtime rename fallback, so load-time normalization is the only fix).
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_prints") == std::vector<std::string>{ "New Process" });

    // The renamed printer is rewritten while the unknown/deleted name is preserved as-is.
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_printers") ==
          (std::vector<std::string>{ "New Printer", "Unknown Printer" }));

    // Normalizing rewrites config in place without flagging the preset dirty.
    CHECK_FALSE(bundle.prints.find_preset("My Process", false, true)->is_dirty);

    // A system preset that already references the current name is left untouched (idempotent no-op).
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });
}

TEST_CASE("Renamed names are normalized into a SYSTEM preset's compatible lists", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A *system* (vendor) filament whose own compatible lists still reference the OLD names. A vendor
    // profile can point at a sibling preset that was later renamed, so system presets must be
    // normalized too (they are skipped by neither collection walk).
    add_inmemory_preset(bundle.filaments, "System Filament").is_system = true;
    compatible_list(bundle.filaments, "System Filament", "compatible_printers") = { "Old Printer" };
    compatible_list(bundle.filaments, "System Filament", "compatible_prints")   = { "Old Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps
    bundle.normalize_compatible_presets();

    // The stale references in the system preset are rewritten to the current names.
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_prints") ==
          std::vector<std::string>{ "New Process" });

    // The rewrite does not flag the system preset dirty, and is idempotent.
    CHECK_FALSE(bundle.filaments.find_preset("System Filament", false, true)->is_dirty);
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
}

TEST_CASE("compatible_prints on SLA materials resolves against sla_prints, not prints", "[Preset][Rename]")
{
    PresetBundle bundle;

    // A renamed SLA process, and a same-named FFF process that must NOT be picked up: resolving the
    // SLA material's compatible_prints against `prints` would wrongly rewrite to "Wrong FFF Process".
    add_inmemory_preset(bundle.sla_prints, "New SLA Process");
    set_renamed_from(bundle.sla_prints, "New SLA Process", { "Old SLA Process" });
    add_inmemory_preset(bundle.prints, "Wrong FFF Process");
    set_renamed_from(bundle.prints, "Wrong FFF Process", { "Old SLA Process" });

    add_inmemory_preset(bundle.sla_materials, "My SLA Material");
    compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") = { "Old SLA Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config);
    bundle.normalize_compatible_presets();

    CHECK(compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") ==
          std::vector<std::string>{ "New SLA Process" });
}

TEST_CASE("Profile validator flags dangling and renamed preset references", "[Preset][Validate]")
{
    PresetBundle bundle;

    // Current printers: a real one, and a renamed one (its old name resolves via renamed_from).
    add_inmemory_preset(bundle.printers, "Real Printer");
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });

    // A real process, referenced from a filament's compatible_prints.
    add_inmemory_preset(bundle.prints, "Real Process").is_system = true;

    // A fully valid system filament: references only current names.
    add_inmemory_preset(bundle.filaments, "Good Filament").is_system = true;
    compatible_list(bundle.filaments, "Good Filament", "compatible_printers") = { "Real Printer" };
    compatible_list(bundle.filaments, "Good Filament", "compatible_prints")   = { "Real Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps

    // With only valid references, the validator is clean.
    CHECK_FALSE(bundle.check_preset_references());

    SECTION("deleted compatible_printers is flagged") {
        add_inmemory_preset(bundle.filaments, "Ghost Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Ghost Ref Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("renamed compatible_printers (old name) is flagged") {
        add_inmemory_preset(bundle.filaments, "Old Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Old Ref Filament", "compatible_printers") = { "Old Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted compatible_prints is flagged") {
        add_inmemory_preset(bundle.filaments, "Bad Process Ref").is_system = true;
        compatible_list(bundle.filaments, "Bad Process Ref", "compatible_prints") = { "Ghost Process" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted inherits parent is flagged") {
        add_inmemory_preset(bundle.filaments, "Orphan Filament", "Ghost Parent").is_system = true;
        CHECK(bundle.check_preset_references());
    }

    SECTION("non-system preset with a dangling reference is ignored") {
        add_inmemory_preset(bundle.filaments, "User Filament"); // is_system stays false
        compatible_list(bundle.filaments, "User Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK_FALSE(bundle.check_preset_references());
    }
}

// Under a shared override key, the last preset merged into the full config overwrote the others', so an
// edited slicing-pipeline override never reached Print::apply's diff and re-configuring a plugin never
// re-sliced. Per-type keys make that collision impossible; guard the scoping here.
TEST_CASE("Plugin capability override keys are scoped per preset type", "[Preset][Plugin]")
{
    // Pin the key names: presets and 3mf files store them verbatim, so a rename is a format change.
    CHECK(Preset::plugin_overrides_key(Preset::TYPE_PRINT)    == std::string("print_plugin_config_overrides"));
    CHECK(Preset::plugin_overrides_key(Preset::TYPE_PRINTER)  == std::string("printer_plugin_config_overrides"));
    CHECK(Preset::plugin_overrides_key(Preset::TYPE_FILAMENT) == std::string("filament_plugin_config_overrides"));

    // ...and each key lives on exactly its own preset type's option list, so no two ever share a slot.
    const std::pair<Preset::Type, const std::vector<std::string>*> scopes[] = {
        {Preset::TYPE_PRINT,    &Preset::print_options()},
        {Preset::TYPE_PRINTER,  &Preset::printer_options()},
        {Preset::TYPE_FILAMENT, &Preset::filament_options()},
    };
    for (const auto &owner : scopes)
        for (const auto &scoped : scopes) {
            const std::string key = Preset::plugin_overrides_key(scoped.first);
            CAPTURE(owner.first, key);
            CHECK(contains(*owner.second, key) == (owner.first == scoped.first));
        }
}

namespace {

// A standalone filament collection that exposes the protected library masking builder, so the Orca
// Filament Library scenario can be set up without the full system-profile load pipeline.
struct LibraryFilamentTestCollection : public PresetCollection
{
    LibraryFilamentTestCollection()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_library_profile_excluded_from;
};

} // namespace

// Orca: a filament in the Orca Filament Library that names its compatible printers has to hide the generic
// library filament sharing its alias, the same way a vendor owned filament does. Otherwise both are compatible
// with that printer and the plater combo box lists the shared alias twice.
TEST_CASE("A printer specific filament supersedes the generic library filament with the same alias", "[Preset][Bundle]")
{
    LibraryFilamentTestCollection filaments;
    PresetCollection              printers(Preset::TYPE_PRINTER, Preset::printer_options(),
                                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()));
    // The masking keys off the vendor name, which VendorProfile's constructor does not derive from the id.
    VendorProfile                 library(PresetBundle::ORCA_FILAMENT_LIBRARY);
    VendorProfile                 vendor("Vendor");
    library.name = PresetBundle::ORCA_FILAMENT_LIBRARY;
    vendor.name  = "Vendor";

    auto add_filament = [&filaments](const VendorProfile &owner, const std::string &name, std::vector<std::string> compatible_printers) {
        Preset &preset = add_inmemory_preset(filaments, name);
        preset.alias   = "Generic ABS";
        preset.vendor  = &owner;
        preset.config.option<ConfigOptionStrings>("compatible_printers", true)->values = std::move(compatible_printers);
    };

    add_filament(library, "Generic ABS @System", {});
    add_filament(library, "Generic ABS @Printer A", { "Printer A" });
    add_filament(vendor,  "Generic ABS @Printer B", { "Printer B" });

    filaments.update_library_profile_excluded_from();

    const Preset *generic = filaments.find_preset("Generic ABS @System");
    REQUIRE(generic != nullptr);
    CHECK(generic->m_excluded_from.count("Printer A") == 1);
    CHECK(generic->m_excluded_from.count("Printer B") == 1);
    CHECK(generic->m_excluded_from.size() == 2);

    // A printer specific profile names printers, so it is never the one being hidden - not even by itself.
    const Preset *specific = filaments.find_preset("Generic ABS @Printer A");
    REQUIRE(specific != nullptr);
    CHECK(specific->m_excluded_from.empty());

    // ...and the generic profile really drops out of the compatible set on the printer it is hidden from.
    add_inmemory_preset(printers, "Printer A");
    add_inmemory_preset(printers, "Printer C");
    const Preset *printer_a = printers.find_preset("Printer A");
    const Preset *printer_c = printers.find_preset("Printer C");
    REQUIRE(printer_a != nullptr);
    REQUIRE(printer_c != nullptr);

    const PresetWithVendorProfile generic_lib(*generic, &library);
    CHECK_FALSE(is_compatible_with_printer(generic_lib, PresetWithVendorProfile(*printer_a, nullptr)));
    CHECK(is_compatible_with_printer(generic_lib, PresetWithVendorProfile(*printer_c, nullptr)));
}

// A "published" 3MF keeps the user's currently-selected presets and overlays only the
// author-selected process keys onto the edited preset (mirrors the GUI load path: normalize
// before load_config_model, then the overlay in load_config_file_config).
TEST_CASE("Published 3MF overlays only the author-selected process keys onto the edited preset", "[Preset][Bundle][Published]")
{
    // The file config the GUI builds from a .3mf's project settings.
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // The loader derives the filament count from filament_colour and throws when it is
        // empty; a 3mf always carries it.
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt_float("layer_height") = 0.28;                       // process scalar
        config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values = { 140., 150. }; // matching-size vector
        config.opt<ConfigOptionStrings>("post_process")->values = { "script-a", "script-b" }; // mismatched vector
        config.opt<ConfigOptionInts>("nozzle_temperature")->values = { 220 }; // filament key (not applied anywhere)
        // Structural (denylisted) key: must be silently ignored. full_print_config() omits the
        // *_settings_id keys, so create one explicitly.
        config.opt_string("print_settings_id", true) = "file process";
        config.opt<ConfigOptionFloats>("flush_multiplier")->values = { 2., 2. }; // must NOT cross over
        config.opt<ConfigOptionFloats>("wipe_tower_x")->values = { 100. };       // plate geometry, does cross over
        config.opt<ConfigOptionFloat>("wipe_tower_rotation_angle")->value = 45.; // published-only plate geometry, crosses over
        config.option("curr_bed_type")->setInt(BedType::btPC);                   // must NOT cross over
        return config;
    };

    const std::vector<std::string> published_keys = {
        "layer_height", "wiping_volumes_extruders", "post_process", "nozzle_temperature", "print_settings_id"
    };

    PresetBundle bundle;
    const std::string pre_load_name = bundle.prints.get_edited_preset().name;
    const size_t      pre_load_size = bundle.prints.size();

    // The edited presets are the overlay targets: recognizable pre-load values.
    bundle.prints.get_edited_preset().config.opt_float("layer_height") = 0.1;
    bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values = { 10., 20. };
    bundle.prints.get_edited_preset().config.opt<ConfigOptionStrings>("post_process")->values = { "existing-script" };
    bundle.prints.get_edited_preset().config.opt_string("print_settings_id") = "user process";
    // Capture the ctor-seeded project_config values so the assertions below check the load
    // leaves them untouched rather than hardcoding the defaults.
    const std::vector<std::string> seed_filament_colour  = bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values;
    const std::vector<double>      seed_flush_multiplier = bundle.project_config.opt<ConfigOptionFloats>("flush_multiplier")->values;
    const int                      seed_bed_type         = bundle.project_config.option("curr_bed_type")->getInt();

    DynamicPrintConfig config = make_file_config();
    // The GUI normalizes the config before load; mirror that so only the production path runs.
    Preset::normalize(config);

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = published_keys;
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // a) Process scalar overlaid; matching-size vector applied, mismatched one lands in
    // skipped_keys; applied keys are not reported.
    CHECK_THAT(bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 0.000001));
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values == std::vector<double>{ 140., 150. });
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionStrings>("post_process")->values == std::vector<std::string>{ "existing-script" });
    CHECK(contains_key(pub.skipped_keys, "post_process"));
    CHECK_FALSE(contains_key(pub.skipped_keys, "layer_height"));
    CHECK_FALSE(contains_key(pub.skipped_keys, "wiping_volumes_extruders"));

    // b) A filament key is never applied anywhere and is reported as skipped.
    CHECK(bundle.prints.get_edited_preset().config.option("nozzle_temperature") == nullptr);
    CHECK(contains_key(pub.skipped_keys, "nozzle_temperature"));

    // c) A structural key is silently ignored: neither applied nor reported as skipped.
    CHECK(bundle.prints.get_edited_preset().config.opt_string("print_settings_id") == "user process");
    CHECK_FALSE(contains_key(pub.skipped_keys, "print_settings_id"));

    // d) Only plate/bed geometry crosses in published mode: filament/purge data and bed type
    // stay at the ctor seeds.
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values == seed_filament_colour);
    CHECK(bundle.project_config.opt<ConfigOptionFloats>("flush_multiplier")->values == seed_flush_multiplier);
    CHECK(bundle.project_config.option("curr_bed_type")->getInt() == seed_bed_type);
    CHECK(bundle.project_config.opt<ConfigOptionFloats>("wipe_tower_x")->values == std::vector<double>{ 100. });
    CHECK_THAT(bundle.project_config.opt<ConfigOptionFloat>("wipe_tower_rotation_angle")->value, Catch::Matchers::WithinAbs(45., 0.000001));

    // e) The published path keeps the user's currently-selected presets: same preset, same size.
    CHECK(bundle.prints.get_edited_preset().name == pre_load_name);
    CHECK(bundle.prints.size() == pre_load_size);

    // f) Non-published control: the overlay is disabled, the file's presets are imported
    // instead, and no skipped_keys are produced.
    PresetBundle     control_bundle;
    const size_t     control_pre_size = control_bundle.prints.size();
    PublishedConfig  control_pub;
    control_pub.published      = false;
    control_pub.published_keys = published_keys;
    DynamicPrintConfig control_config = make_file_config();
    Preset::normalize(control_config);
    control_bundle.load_config_model("test.3mf", std::move(control_config), Semver(), &control_pub);

    CHECK(control_pub.skipped_keys.empty());
    CHECK(control_bundle.prints.size() > control_pre_size);
    // The file's layer_height reached the edited preset via the normal import, not the overlay.
    CHECK_THAT(control_bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 0.000001));
}

// The published printer overlay is restricted to the publishable retraction/z-hop allowlist:
// matching-size vectors apply, mismatched vectors are reported as skipped, and any other
// printer-class key (e.g. machine_start_gcode) is contract-excluded (never applied, never
// reported).
TEST_CASE("Published 3MF overlays only the allowlisted retraction and z-hop keys onto the edited printer preset", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    Preset::normalize(config);
    config.opt<ConfigOptionFloats>("retraction_length")->values = { 1.4 };         // matching size (1 extruder)
    config.opt<ConfigOptionFloats>("retraction_speed")->values = { 45., 55. };     // size 2: mismatched
    config.opt_string("machine_start_gcode") = "G28 ; from file";                  // outside the allowlist

    PresetBundle bundle;
    bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.8 };
    // A recognizable non-default value: the skipped mismatch below must leave it untouched
    // (asserting the default instead would silently test PrintConfig's retraction_speed).
    bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_speed")->values = { 33. };
    bundle.printers.get_edited_preset().config.opt_string("machine_start_gcode") = "G28 ; user";

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = { "retraction_length", "retraction_speed", "machine_start_gcode" };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Matching-size retraction vector applied; mismatched vector reported as skipped and the
    // receiver's own value survives.
    CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{ 1.4 });
    CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_speed")->values == std::vector<double>{ 33. });
    CHECK(contains_key(pub.skipped_keys, "retraction_speed"));
    // Contract-excluded printer key: silently ignored, absent from skipped_keys.
    CHECK(bundle.printers.get_edited_preset().config.opt_string("machine_start_gcode") == "G28 ; user");
    CHECK_FALSE(contains_key(pub.skipped_keys, "machine_start_gcode"));
}

// A published 3MF carries per-slot material keys; on load they are applied positionally to the
// receiver's slot N (a key-only entry has no type gate), written onto the slot's stored preset
// in place.
TEST_CASE("Published 3MF applies positional material keys onto the receiver's material presets", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Two filament slots; filament_diameter drives the normalized per-slot vector sizes.
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        // Keep the multi-extruder consistency validation happy for a 2-slot config.
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PETG" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFT99" };
        // Author per-slot retraction values. These are per-filament override keys that are not
        // members of the static PrintRegionConfig, so full_print_config() omits them and they
        // must be created explicitly (as nullable, matching the real 3MF project config).
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
        config.option<ConfigOptionFloatsNullable>("filament_z_hop", true)->values = { 0.2, 0.3 };
        return config;
    };

    PresetBundle bundle;
    Preset &pla  = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.filament_id = "GFL99";
    pla.config.opt_string("filament_type", 0u)   = "PLA";
    pla.config.opt_string("filament_vendor", 0u) = "Generic";
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    pla.config.opt<ConfigOptionStrings>("filament_settings_id")->values = { "receiver-pla" };
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.filament_id = "GFT99";
    petg.config.opt_string("filament_type", 0u)   = "PETG";
    petg.config.opt_string("filament_vendor", 0u) = "Generic";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    petg.config.opt<ConfigOptionFloatsNullable>("filament_z_hop", true)->values = { 0.1 };
    bundle.filament_presets = { "My PLA", "My PETG" };

    PublishedMaterialEntry pla_entry;
    pla_entry.filament_id = "GFL99";
    pla_entry.slot        = 0; // the author's PLA slot
    pla_entry.keys        = { "filament_retraction_length", "filament_settings_id" };
    PublishedMaterialEntry petg_entry;
    petg_entry.filament_id = "GFT99";
    petg_entry.slot        = 1; // the author's PETG slot
    petg_entry.keys        = { "filament_retraction_length", "filament_z_hop" };
    // A slot-less entry (no slot field, only possible in hand-crafted files): silently skipped.
    PublishedMaterialEntry noslot_entry;
    noslot_entry.filament_type = "ABS";
    noslot_entry.keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published      = true;
    pub.material_keys  = { pla_entry, petg_entry, noslot_entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The author's slot values are written onto the receiver's stored presets in place.
    CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    CHECK(bundle.filaments.find_preset("My PETG")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 1.2 });
    CHECK(bundle.filaments.find_preset("My PETG")->config.opt<ConfigOptionFloatsNullable>("filament_z_hop")->values == std::vector<double>{ 0.3 });
    // Structural keys inside a material entry are silently ignored: the receiver's own
    // filament_settings_id is untouched and nothing is reported for it.
    CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{ "receiver-pla" });
    CHECK_FALSE(contains_key(pub.skipped_keys, "material:GFL99 (filament_settings_id)"));
    // Everything applied; the slot-less entry produced no skipped entry.
    CHECK(pub.skipped_keys.empty());
}

// A "full publish" slot serializes the whole filament. On load the slot is matched positionally
// against the published type: a matching receiver type still receives the author's full values
// (like a normal save/load of the filament), a mismatched type replaces it with the first
// same-type visible preset (applying the author's full values on top), and a type not in the
// receiver's library falls back to the first available visible preset.
TEST_CASE("Published 3MF full-published slots replace or ignore the receiver material by type", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Two author slots; filament_diameter drives the normalized per-slot vector sizes.
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PETG" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFT99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
        return config;
    };
    // The full dump of slot 0, publishing the whole filament as type "ABS".
    auto make_full_abs_entry = [] {
        PublishedMaterialEntry entry;
        entry.slot                = 0;
        entry.full                = true;
        entry.publish_type        = true;
        entry.publish_type_value  = "ABS";
        entry.full_keys           = { "filament_retraction_length" };
        return entry;
    };

    SECTION("type match applies the full dump onto the receiver's material") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        bundle.filament_presets = { "My PLA", "My PLA" };

        PublishedMaterialEntry full = make_full_abs_entry();
        full.publish_type_value     = "PLA"; // author requires PLA, receiver slot is PLA

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { full };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // The type matches, but a full publish behaves like a normal save: the author's values
        // are written onto the slot's preset wholesale.
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(pub.skipped_keys.empty());
        CHECK(pub.material_replacements.empty());
    }

    SECTION("type mismatch replaces the slot with the first same-type preset and applies the full dump") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        Preset &abs = add_inmemory_preset(bundle.filaments, "My ABS");
        abs.config.opt_string("filament_type", 0u) = "ABS";
        abs.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.3 };
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_full_abs_entry() };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets.size() == 1);
        // The slot is re-pointed at the library's ABS preset and the author's full values are
        // written onto it in place (the original 0.3 is overwritten); the receiver's own
        // material is untouched.
        CHECK(bundle.filament_presets[0] == "My ABS");
        CHECK(bundle.filaments.find_preset("My ABS", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(pub.skipped_keys.empty());
        REQUIRE(pub.material_replacements.size() == 1);
        // The entry carries no identity fields, so the pick cannot be judged as a substitute.
        CHECK(pub.material_replacements[0] == "slot 0: My PLA -> My ABS");
    }

    SECTION("no same-type match falls back to the first available visible preset") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        Preset &other = add_inmemory_preset(bundle.filaments, "Other PLA");
        other.config.opt_string("filament_type", 0u) = "PLA";
        other.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.7 };
        bundle.filament_presets = { "My PLA" };

        PublishedMaterialEntry full = make_full_abs_entry();
        // The dump carries the identity too, so the fallback preset must take the author's type
        // and vendor.
        full.full_keys = { "filament_retraction_length", "filament_type", "filament_vendor" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { full };
        DynamicPrintConfig config = make_file_config();
        // The author's slot 0 really is ABS.
        config.opt<ConfigOptionStrings>("filament_type")->values = { "ABS", "PETG" };
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // No ABS in the library: the slot falls back to the first available visible preset (the
        // unused "Other PLA") and the author's full values are written onto it, type and vendor
        // included. The receiver's own material is untouched.
        CHECK(bundle.filament_presets[0] == "Other PLA");
        Preset *fallback = bundle.filaments.find_preset("Other PLA", false, true);
        REQUIRE(fallback != nullptr);
        CHECK(fallback->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(fallback->config.opt_string("filament_type", 0u) == "ABS");
        CHECK(fallback->config.opt_string("filament_vendor", 0u) == "Generic");
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt_string("filament_type", 0u) == "PLA");
        CHECK(pub.skipped_keys.empty());
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PLA -> Other PLA (substitute: no ABS available)");
    }
}

// Regression for the author's published material being skipped by a type-only replacement
// search: the slot must prefer the exact published material (filament_id) over the first other
// same-type preset, even when the exact preset is already referenced by another slot.
TEST_CASE("Published 3MF replaces a mismatched slot with the exact published material when available", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };

    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    // The receiver's second slot already uses the exact published material.
    Preset &generic_pla = add_inmemory_preset(bundle.filaments, "Generic PLA");
    generic_pla.filament_id = "GFL99";
    generic_pla.config.opt_string("filament_type", 0u) = "PLA";
    generic_pla.config.opt_string("filament_vendor", 0u) = "Generic";
    generic_pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // An unrelated PLA, unreferenced: a type-only search picks it because Generic PLA is
    // referenced by slot 1.
    Preset &bambu = add_inmemory_preset(bundle.filaments, "Bambu PLA Basic");
    bambu.filament_id = "GFB00";
    bambu.config.opt_string("filament_type", 0u) = "PLA";
    bambu.config.opt_string("filament_vendor", 0u) = "Bambu Lab";
    bambu.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.4 };
    bundle.filament_presets = { "My PETG", "Generic PLA" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.filament_id        = "GFL99";
    entry.filament_vendor    = "Generic";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The exact published material (id GFL99) wins over the unreferenced type-only preset.
    CHECK(bundle.filament_presets[0] == "Generic PLA");
    CHECK(bundle.filament_presets[1] == "Generic PLA");
    CHECK(bundle.filaments.find_preset("Generic PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    // The receiver's own material is untouched; the unrelated PLA too.
    CHECK(bundle.filaments.find_preset("My PETG", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.6 });
    CHECK(bundle.filaments.find_preset("Bambu PLA Basic", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.4 });
    // Accepted mutate tradeoff: the shared exact-material preset was mutated, so slot 1 also
    // carries the author's values (the leak is documented, not accidental).
    CHECK(bundle.filaments.find_preset("Generic PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    // An exact-material pick is reported without a substitute qualifier.
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA");
    CHECK(pub.skipped_keys.empty());
}

// The author's preset name travels in the file, so a type mismatch resolves to the exact
// published material even when the identity fields are absent or stale (older files): a
// "Generic PLA" author must land on the receiver's "Generic PLA", never on a same-type
// substitute like "Bambu PLA Basic".
TEST_CASE("Published 3MF replaces a mismatched slot with the exact preset by name", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "OGFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };

    auto add_pla = [](PresetBundle &bundle, const char *name, const char *id, const char *vendor, const char *setting_id) {
        Preset &preset = add_inmemory_preset(bundle.filaments, name);
        preset.filament_id = id;
        preset.setting_id  = setting_id;
        preset.config.opt_string("filament_type", 0u) = "PLA";
        preset.config.opt_string("filament_vendor", 0u) = vendor;
        preset.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        return &preset;
    };

    SECTION("full identity (name, setting_id, filament_id): the name match wins") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Generic PLA @System", "OGFL99", "Generic", "RcBNzytWgwRrwXXz");
        add_pla(bundle, "Bambu PLA Basic @System", "OGFA00", "Bambu Lab", "zkc85XTKi4cb6cOw");
        bundle.filament_presets = { "My PETG" };

        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.filament_id        = "OGFL99";
        entry.filament_vendor    = "Generic";
        entry.setting_id         = "RcBNzytWgwRrwXXz";
        entry.preset_name        = "Generic PLA @System";
        entry.full_keys          = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets[0] == "Generic PLA @System");
        CHECK(bundle.filaments.find_preset("Generic PLA @System", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(bundle.filaments.find_preset("My PETG", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.6 });
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @System");
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("only the name is present (broken/stale ids): still the exact preset") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Generic PLA @System", "OGFL99", "Generic", "RcBNzytWgwRrwXXz");
        add_pla(bundle, "Bambu PLA Basic @System", "OGFA00", "Bambu Lab", "zkc85XTKi4cb6cOw");
        bundle.filament_presets = { "My PETG" };

        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.preset_name        = "Generic PLA @System";
        entry.full_keys          = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets[0] == "Generic PLA @System");
        CHECK(bundle.filaments.find_preset("Bambu PLA Basic @System", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @System");
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("the exact preset is hidden in the library: the exact match still wins") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        Preset &generic_pla = *add_pla(bundle, "Generic PLA @System", "OGFL99", "Generic", "RcBNzytWgwRrwXXz");
        generic_pla.is_visible = false;
        add_pla(bundle, "Bambu PLA Basic @System", "OGFA00", "Bambu Lab", "zkc85XTKi4cb6cOw");
        bundle.filament_presets = { "My PETG" };

        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.preset_name        = "Generic PLA @System";
        entry.full_keys          = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets[0] == "Generic PLA @System");
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("grown slot (author slot 1) is seeded with the exact preset by name") {
        auto two_slot_config = [] {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
            config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
            config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
            config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#FFFF00" };
            config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PLA" };
            config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic" };
            config.opt<ConfigOptionStrings>("filament_ids")->values = { "OGFL99", "OGFL99" };
            config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 0.8 };
            return config;
        };

        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Generic PLA @System", "OGFL99", "Generic", "RcBNzytWgwRrwXXz");
        add_pla(bundle, "Bambu PLA Basic @System", "OGFA00", "Bambu Lab", "zkc85XTKi4cb6cOw");
        bundle.filament_presets = { "My PETG" };

        PublishedMaterialEntry entry;
        entry.slot               = 1;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.preset_name        = "Generic PLA @System";
        entry.full_keys          = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = two_slot_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets.size() == 2);
        CHECK(bundle.filament_presets[0] == "My PETG");
        CHECK(bundle.filament_presets[1] == "Generic PLA @System");
        // The full dump applies the author's slot-1 value onto the grown slot's preset.
        CHECK(bundle.filaments.find_preset("Generic PLA @System", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.8 });
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("no exact preset in the library: falls back to the substitute") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Bambu PLA Basic @System", "OGFA00", "Bambu Lab", "zkc85XTKi4cb6cOw");
        bundle.filament_presets = { "My PETG" };

        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.preset_name        = "Generic PLA @System";
        entry.filament_id        = "OGFL99";
        entry.full_keys          = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets[0] == "Bambu PLA Basic @System");
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Bambu PLA Basic @System (substitute: no exact material match)");
    }
}

// The receiver library can hold several presets that all look like "Generic PLA": a bare-named
// legacy copy, the author's exact vendor preset and the Orca library preset (whose alias is the
// bare name). The exact preset name must outrank the fuzzy bare/alias tier, so the author's
// "Generic PLA @Qidi Q2 0.4 nozzle" wins regardless of collection order.
TEST_CASE("Published 3MF prefers the exact preset over same-bare-name presets", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    // A legacy bundle preset literally named "Generic PLA": bare-name match, sorts first.
    Preset &bare = add_inmemory_preset(bundle.filaments, "Generic PLA");
    bare.config.opt_string("filament_type", 0u) = "PLA";
    bare.config.opt_string("filament_vendor", 0u) = "Generic";
    bare.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // The author's exact preset.
    Preset &qidi = add_inmemory_preset(bundle.filaments, "Generic PLA @Qidi Q2 0.4 nozzle");
    qidi.config.opt_string("filament_type", 0u) = "PLA";
    qidi.config.opt_string("filament_vendor", 0u) = "Generic";
    qidi.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // The Orca library preset: alias "Generic PLA" matches the bare-name tier too.
    Preset &sys = add_inmemory_preset(bundle.filaments, "Generic PLA @System");
    sys.config.opt_string("filament_type", 0u) = "PLA";
    sys.config.opt_string("filament_vendor", 0u) = "Generic";
    sys.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PETG" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Generic PLA @Qidi Q2 0.4 nozzle";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The exact preset beats the bare/alias matches even though "Generic PLA" sorts first.
    CHECK(bundle.filament_presets[0] == "Generic PLA @Qidi Q2 0.4 nozzle");
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @Qidi Q2 0.4 nozzle");
    CHECK(pub.skipped_keys.empty());
}

// The author's exact preset exists on the receiver but is incompatible with the active printer
// (a Qidi bundle preset on a non-Qidi printer); a same-family preset that IS compatible must
// win instead, so the slot never ends up with a filament the printer cannot use.
TEST_CASE("Published 3MF prefers a compatible preset over an exact but incompatible one", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    // The bare-named legacy preset (Bambu-printer only) and the exact author preset are both
    // incompatible with the receiver's printer. update_compatible() recomputes is_compatible
    // inside load_config_model, so the incompatibility is expressed the way production derives
    // it: a compatible_printers constraint no receiver printer satisfies (the fresh bundle's
    // active printer is the "Default Printer" placeholder).
    Preset &bare = add_inmemory_preset(bundle.filaments, "Generic PLA");
    bare.config.opt_string("filament_type", 0u) = "PLA";
    bare.config.opt_string("filament_vendor", 0u) = "Generic";
    bare.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bare.config.set_key_value("compatible_printers", new ConfigOptionStrings({ "Unrelated Printer" }));
    Preset &qidi = add_inmemory_preset(bundle.filaments, "Generic PLA @Qidi Q2 0.4 nozzle");
    qidi.config.opt_string("filament_type", 0u) = "PLA";
    qidi.config.opt_string("filament_vendor", 0u) = "Generic";
    qidi.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    qidi.config.set_key_value("compatible_printers", new ConfigOptionStrings({ "Unrelated Printer" }));
    // The receiver's own compatible library preset.
    Preset &sys = add_inmemory_preset(bundle.filaments, "Generic PLA @System");
    sys.config.opt_string("filament_type", 0u) = "PLA";
    sys.config.opt_string("filament_vendor", 0u) = "Generic";
    sys.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PETG" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Generic PLA @Qidi Q2 0.4 nozzle";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    CHECK(bundle.filament_presets[0] == "Generic PLA @System");
    // A same-family name-tier pick is not reported as a substitute.
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @System");
    CHECK(pub.skipped_keys.empty());
}

// When every candidate is incompatible with the receiver's printer, the search still falls back
// to the best (exact) match rather than leaving the slot on the mismatched type.
TEST_CASE("Published 3MF falls back to an incompatible exact preset when no compatible candidate exists", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    // The exact author preset is incompatible with the receiver's printer. update_compatible()
    // recomputes is_compatible inside load_config_model, so the incompatibility is expressed the
    // way production derives it: a compatible_printers constraint the receiver's printer (the
    // fresh bundle's "Default Printer" placeholder) does not satisfy.
    Preset &qidi = add_inmemory_preset(bundle.filaments, "Generic PLA @Qidi Q2 0.4 nozzle");
    qidi.config.opt_string("filament_type", 0u) = "PLA";
    qidi.config.opt_string("filament_vendor", 0u) = "Generic";
    qidi.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    qidi.config.set_key_value("compatible_printers", new ConfigOptionStrings({ "Unrelated Printer" }));
    bundle.filament_presets = { "My PETG" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Generic PLA @Qidi Q2 0.4 nozzle";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    CHECK(bundle.filament_presets[0] == "Generic PLA @Qidi Q2 0.4 nozzle");
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @Qidi Q2 0.4 nozzle");
    CHECK(pub.skipped_keys.empty());
}

// The overlay writes onto the edited layer only when that layer survives the load (slot 0's
// preset still matches the edited preset). When the user views a non-first slot's material and
// the published entry targets that slot, the final re-select would destroy the edited layer -
// so the values must land on the stored preset instead and survive.
TEST_CASE("Published 3MF writes to the stored preset when the edited layer is re-selected away", "[Preset][Bundle][Published]")
{
    // Two-slot author config: slot 1 carries the published retraction value.
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PETG", "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFT99", "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6, 0.9 };
        return config;
    };

    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.opt_string("filament_type", 0u) = "PLA";
    pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PETG", "My PLA" };
    // The user is viewing slot 1's material.
    REQUIRE(bundle.filaments.select_preset_by_name("My PLA", false));

    PublishedMaterialEntry entry;
    entry.slot                = 1;
    entry.publish_type        = true;
    entry.publish_type_value  = "PLA";
    entry.publish_color       = true;
    entry.color               = "#ABCDEF";
    entry.keys                = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The published values survived on the stored preset (the edited layer was re-selected to
    // slot 0's material and must not have been the only copy).
    Preset *stored = bundle.filaments.find_preset("My PLA", false, true);
    REQUIRE(stored != nullptr);
    CHECK(stored->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    CHECK(stored->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    // The load re-selected slot 0's material, mirroring a normal project load.
    CHECK(bundle.filaments.get_edited_preset().name == "My PETG");
    // Selecting the slot's material afterwards surfaces the applied values.
    REQUIRE(bundle.filaments.select_preset_by_name("My PLA", false));
    CHECK(bundle.filaments.get_edited_preset().config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    CHECK(bundle.filaments.get_edited_preset().config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    CHECK(pub.skipped_keys.empty());
}

// The author published "Generic PLA @System"; the receiver's copy "Generic PLA" drops the
// "@System" suffix (and has no setting_id, a different filament_id), so only the trimmed
// bare-name tier can reach it - the truncated form carries trailing whitespace that must not
// defeat the match.
TEST_CASE("Published 3MF name matching accepts suffix-less receiver presets", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    Preset &plain = add_inmemory_preset(bundle.filaments, "Generic PLA");
    plain.config.opt_string("filament_type", 0u) = "PLA";
    plain.config.opt_string("filament_vendor", 0u) = "Generic";
    plain.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // An unrelated same-type preset that would win a type-only search.
    Preset &bambu = add_inmemory_preset(bundle.filaments, "Bambu PLA Basic");
    bambu.config.opt_string("filament_type", 0u) = "PLA";
    bambu.config.opt_string("filament_vendor", 0u) = "Bambu Lab";
    bambu.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.4 };
    bundle.filament_presets = { "My PETG" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Generic PLA @System";
    entry.filament_id        = "GFL99";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The trimmed bare-name tier fires: the suffix-less copy wins over the type-only
    // candidate, without a substitute qualifier.
    CHECK(bundle.filament_presets[0] == "Generic PLA");
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA");
    CHECK(pub.skipped_keys.empty());
}

// The receiver's vendor renamed "Generic PLA @System" to "PLA Generic @System" (renamed_from
// records the old name); the published file still names the old preset, which resolves through
// the collection's rename map instead of degrading to a same-type substitute.
TEST_CASE("Published 3MF name matching follows the receiver's preset renames", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    add_inmemory_preset(bundle.filaments, "PLA Generic @System");
    bundle.filament_presets = { "My PETG" };

    set_renamed_from(bundle.filaments, "PLA Generic @System", { "Generic PLA @System" });
    AppConfig app_config;
    bundle.load_installed_printers(app_config); // rebuild the rename map

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Generic PLA @System"; // the pre-rename name
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // find_preset2 resolves the pre-rename name through the collection's rename map.
    CHECK(bundle.filament_presets[0] == "PLA Generic @System");
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> PLA Generic @System");
    CHECK(pub.skipped_keys.empty());
}

// The author's preset "Voron Generic PLA" (a vendor-specific generic) is gone from the
// receiver's profile set; find_preset2 auto-matches it to the Orca Filament Library instead of
// falling back to a same-type substitute.
TEST_CASE("Published 3MF name matching falls back to the library for removed vendor generics", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u) = "PETG";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");
    bundle.filament_presets = { "My PETG" };

    PublishedMaterialEntry entry;
    entry.slot               = 0;
    entry.full               = true;
    entry.publish_type       = true;
    entry.publish_type_value = "PLA";
    entry.preset_name        = "Voron Generic PLA";
    entry.full_keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    CHECK(bundle.filament_presets[0] == "Generic PLA @System");
    REQUIRE(pub.material_replacements.size() == 1);
    CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA @System");
    CHECK(pub.skipped_keys.empty());
}

// The replacement search prefers the published identity: exact filament_id, then vendor+type,
// then type only (collection order decides equal scores; the pick is reported as a substitute
// when it is not the exact published material).
TEST_CASE("Published 3MF prefers the published material identity when replacing a slot", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };
    auto make_entry = [] {
        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.filament_id        = "GFL99";
        entry.filament_vendor    = "Generic";
        entry.full_keys          = { "filament_retraction_length" };
        return entry;
    };
    auto add_pla = [](PresetBundle &bundle, const char *name, const char *id, const char *vendor) {
        Preset &preset = add_inmemory_preset(bundle.filaments, name);
        preset.filament_id = id;
        preset.config.opt_string("filament_type", 0u) = "PLA";
        preset.config.opt_string("filament_vendor", 0u) = vendor;
        preset.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        return &preset;
    };
    auto load = [&](PresetBundle &bundle, PublishedConfig &pub) {
        PublishedMaterialEntry entry = make_entry();
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);
    };

    SECTION("exact filament_id beats collection order") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        // "Bambu PLA Basic" sorts before "Zebra PLA"; only the latter carries the published id.
        add_pla(bundle, "Bambu PLA Basic", "GFB00", "Bambu Lab");
        add_pla(bundle, "Zebra PLA", "GFL99", "Generic");
        bundle.filament_presets = { "My PETG" };

        PublishedConfig pub;
        load(bundle, pub);
        CHECK(bundle.filament_presets[0] == "Zebra PLA");
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Zebra PLA");
    }

    SECTION("vendor and type beat a type-only preset, reported as a substitute") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Bambu PLA Basic", "GFB00", "Bambu Lab");
        add_pla(bundle, "Zebra PLA", "ZZZ99", "Generic"); // same vendor+type, different id
        bundle.filament_presets = { "My PETG" };

        PublishedConfig pub;
        load(bundle, pub);
        CHECK(bundle.filament_presets[0] == "Zebra PLA");
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Zebra PLA (substitute: no exact material match)");
    }

    SECTION("type-only candidates keep collection order and are reported as substitutes") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Bambu PLA Basic", "GFB00", "Bambu Lab");
        add_pla(bundle, "Zebra PLA", "ZZZ99", "Acme"); // no identity match at all
        bundle.filament_presets = { "My PETG" };

        PublishedConfig pub;
        load(bundle, pub);
        CHECK(bundle.filament_presets[0] == "Bambu PLA Basic");
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Bambu PLA Basic (substitute: no exact material match)");
    }
}

// "Generic PLA" and "Generic PLA Matte" share their inherited filament_id (OGFL99), so the
// exact variant can only be matched via the preset setting_id carried in the published file.
TEST_CASE("Published 3MF matches the exact published variant via setting_id", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "OGFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };
    auto add_pla = [](PresetBundle &bundle, const char *name, const char *setting_id) {
        Preset &preset = add_inmemory_preset(bundle.filaments, name);
        preset.setting_id  = setting_id;
        preset.filament_id = "OGFL99"; // shared by all Generic PLA variants
        preset.config.opt_string("filament_type", 0u) = "PLA";
        preset.config.opt_string("filament_vendor", 0u) = "Generic";
        preset.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        return &preset;
    };
    auto load = [&](PresetBundle &bundle, PublishedConfig &pub, const std::string &setting_id) {
        PublishedMaterialEntry entry;
        entry.slot               = 0;
        entry.full               = true;
        entry.publish_type       = true;
        entry.publish_type_value = "PLA";
        entry.filament_id        = "OGFL99";
        entry.filament_vendor    = "Generic";
        entry.setting_id         = setting_id;
        entry.full_keys          = { "filament_retraction_length" };
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);
    };

    SECTION("the published variant wins over its same-id sibling") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Generic PLA", "RcBNzytWgwRrwXXz");
        add_pla(bundle, "Generic PLA Matte", "RFs9eCKYOMUSmvZf");
        bundle.filament_presets = { "My PETG" };

        PublishedConfig pub;
        load(bundle, pub, "RFs9eCKYOMUSmvZf"); // the author published "Generic PLA Matte"
        CHECK(bundle.filament_presets[0] == "Generic PLA Matte");
        CHECK(bundle.filaments.find_preset("Generic PLA Matte", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(bundle.filaments.find_preset("Generic PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        REQUIRE(pub.material_replacements.size() == 1);
        CHECK(pub.material_replacements[0] == "slot 0: My PETG -> Generic PLA Matte");
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("without a setting_id the same-id siblings fall back to collection order") {
        PresetBundle bundle;
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
        add_pla(bundle, "Generic PLA", "RcBNzytWgwRrwXXz");
        add_pla(bundle, "Generic PLA Matte", "RFs9eCKYOMUSmvZf");
        bundle.filament_presets = { "My PETG" };

        PublishedConfig pub;
        load(bundle, pub, ""); // legacy file without the field
        CHECK(bundle.filament_presets[0] == "Generic PLA");
        CHECK(bundle.filaments.find_preset("Generic PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(pub.skipped_keys.empty());
    }
}

// A partially-published slot can carry a curated type and/or colour. The colour is applied
// regardless of the type match; a type mismatch with no same-type replacement keeps the
// receiver's material and reports the slot's keys as skipped. The receiver's slot count grows
// only as far as the highest slot with published content.
TEST_CASE("Published 3MF partial slots apply colour and gate keys by the published type", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PETG" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFT99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2 };
        return config;
    };

    SECTION("matching type applies the keys and the colour") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        bundle.filament_presets = { "My PLA" };

        PublishedMaterialEntry entry;
        entry.slot                = 0;
        entry.publish_type        = true;
        entry.publish_type_value  = "PLA";
        entry.publish_color       = true;
        entry.color               = "#ABCDEF";
        entry.keys                = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // Type matched: keys and colour applied onto the receiver's preset in place.
        Preset *pla_preset = bundle.filaments.find_preset("My PLA", false, true);
        REQUIRE(pla_preset != nullptr);
        CHECK(pla_preset->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(pla_preset->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        CHECK(pub.skipped_keys.empty());
        CHECK(pub.material_replacements.empty());
    }

    SECTION("type mismatch without a replacement keeps the material and skips the keys") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        bundle.filament_presets = { "My PLA" };

        PublishedMaterialEntry entry;
        entry.slot                = 0;
        entry.publish_type        = true;
        entry.publish_type_value  = "ABS"; // no ABS in the receiver library
        entry.publish_color       = true;
        entry.color               = "#ABCDEF";
        entry.keys                = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // The slot keeps the receiver's material: the colour applies in place, the keys are
        // skipped.
        CHECK(bundle.filament_presets[0] == "My PLA");
        Preset *pla_preset = bundle.filaments.find_preset("My PLA", false, true);
        REQUIRE(pla_preset != nullptr);
        CHECK(pla_preset->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(pla_preset->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        CHECK(contains_key(pub.skipped_keys, "material:ABS (filament_retraction_length)"));
        CHECK(pub.material_replacements.empty());
    }

    SECTION("receiver slot count grows to fit the highest published slot and assigns matching type preset") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
        petg.config.opt_string("filament_type", 0u) = "PETG";
        petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.8 };
        // The receiver has a single slot; the file carries two, only slot 1 is published.
        bundle.filament_presets = { "My PLA" };

        PublishedMaterialEntry entry;
        entry.slot                = 1;
        entry.publish_type        = true;
        entry.publish_type_value  = "PETG";
        entry.keys                = { "filament_retraction_length" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // The slot list was grown so author slot 1 has a material (the PETG preset), which
        // then receives the author's values in place.
        REQUIRE(bundle.filament_presets.size() == 2);
        CHECK(bundle.filament_presets[1] == "My PETG");
        CHECK(bundle.filaments.find_preset("My PETG", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 1.2 });
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
    }
}

// The receiver's slot list grows only as far as the highest published slot: a file whose author
// published nothing (or only a low slot) must not pull filler materials into the receiver's
// setup, and the receiver never grows to the file's count.
TEST_CASE("Published 3MF grows the receiver's slots only as far as the published slots", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Four author slots (a 4-filament model).
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2, 3, 4 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PLA", "PLA", "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic", "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFL99", "GFL99", "GFL99" };
        return config;
    };
    auto add_pla_preset = [](PresetBundle &bundle) {
        Preset &preset = add_inmemory_preset(bundle.filaments, "My PLA");
        preset.config.opt_string("filament_type", 0u) = "PLA";
        preset.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        return &preset;
    };
    auto make_color_entry = [](int slot) {
        PublishedMaterialEntry entry;
        entry.slot          = slot;
        entry.publish_color = true;
        entry.color         = "#ABCDEF";
        return entry;
    };

    // A file whose author published nothing for any slot: the receiver's setup is untouched.
    {
        PresetBundle bundle;
        add_pla_preset(bundle);
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published = true; // no material entries at all
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets.size() == 1);
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#123456" });
        CHECK(pub.skipped_keys.empty());
    }

    // Only slot 0 published: a single-slot receiver keeps its single slot; the file's other
    // three slots pull nothing in.
    {
        PresetBundle bundle;
        add_pla_preset(bundle);
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_color_entry(0) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filament_presets.size() == 1);
        // The published colour lands on the slot's preset in place.
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    }

    // Slot 3 published: the receiver grows to 4 so the published slot exists. Unpublished
    // filler slots repeat the receiver's last preset ("Add one filament" behaviour).
    {
        PresetBundle bundle;
        add_pla_preset(bundle);
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_color_entry(3) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets.size() == 4);
        CHECK(bundle.filament_presets[1] == "My PLA");
        CHECK(bundle.filament_presets[2] == "My PLA");
        // Only "My PLA" exists in the library, so the published slot keeps the aliasing and the
        // colour is written onto the shared preset (every slot references it).
        CHECK(bundle.filament_presets[3] == "My PLA");
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        // The project-level per-slot vectors were grown and seeded: fillers take their preset's
        // colour, the published slot its published colour.
        CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values.size() == 4);
        CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values[1] == "#123456");
        CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values[3] == "#ABCDEF");
        CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_multi_colour")->values.size() == 4);
        CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour_type")->values.size() == 4);
        CHECK(bundle.project_config.opt<ConfigOptionInts>("filament_map")->values.size() == 4);
        CHECK(bundle.project_config.opt<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);
    }

    // Slots 0 and 2 published: the receiver grows to 3, never to the file's 4.
    {
        PresetBundle bundle;
        add_pla_preset(bundle);
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_color_entry(0), make_color_entry(2) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets.size() == 3);
        CHECK(bundle.filament_presets[1] == "My PLA");
    }
}

// A published slot is seeded from an unused library preset and the values are written onto it
// in place, so the receiver's own material (slot 0) is never overwritten.
TEST_CASE("Published 3MF seeds published slots from unused presets and mutates them in place", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2, 3, 4 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PLA", "PLA", "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic", "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFL99", "GFL99", "GFL99" };
        return config;
    };

    // Receiver with its own material plus one more library preset; author publishes only slot 4
    // (Red). The grown slot is seeded from the unused library preset, so the published red
    // recolors that preset in place and never the receiver's own material.
    PresetBundle bundle;
    Preset &mine = add_inmemory_preset(bundle.filaments, "My PLA");
    mine.config.opt_string("filament_type", 0u) = "PLA";
    mine.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
    Preset &other = add_inmemory_preset(bundle.filaments, "Other PLA");
    other.config.opt_string("filament_type", 0u) = "PLA";
    other.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#654321" };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.slot          = 3;
    entry.publish_color = true;
    entry.color         = "#ABCDEF";
    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    REQUIRE(bundle.filament_presets.size() == 4);
    // Unpublished filler slots repeat the receiver's last preset ("Add one filament").
    CHECK(bundle.filament_presets[1] == "My PLA");
    CHECK(bundle.filament_presets[2] == "My PLA");
    // The published slot was seeded from the unused library preset; the published colour was
    // written onto it in place, never onto the receiver's own material.
    CHECK(bundle.filament_presets[3] == "Other PLA");
    CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#123456" });
    CHECK(bundle.filaments.find_preset("Other PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    // The project-level colours are sized and seeded for every grown slot.
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values.size() == 4);
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values[1] == "#123456");
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values[3] == "#ABCDEF");
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_multi_colour")->values.size() == 4);
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour_type")->values.size() == 4);
    CHECK(bundle.project_config.opt<ConfigOptionInts>("filament_map")->values.size() == 4);
}

// The GUI displays the edited preset, a snapshot of the selected collection preset taken at
// selection time. Since the overlay mutates the collection presets in place, the load must
// re-select the first slot's filament so the applied values - and slot replacements - surface
// in the GUI.
TEST_CASE("Published 3MF refreshes the edited preset so the applied material values surface", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };
    auto make_entry = [] {
        PublishedMaterialEntry entry;
        entry.slot                = 0;
        entry.publish_type        = true;
        entry.publish_type_value  = "PLA";
        entry.publish_color       = true;
        entry.color               = "#ABCDEF";
        entry.keys                = { "filament_retraction_length" };
        return entry;
    };

    SECTION("the edited preset carries the applied colour and keys") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        bundle.filament_presets = { "My PLA" };
        // Mirror the GUI: the displayed preset is the collection's edited preset.
        REQUIRE(bundle.filaments.select_preset_by_name("My PLA", false));

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_entry() };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        const Preset &edited = bundle.filaments.get_edited_preset();
        // The slot references the edited preset, so the overlay lands on the edited layer:
        // visible as a modification while the stored preset stays untouched.
        CHECK(edited.name == "My PLA");
        CHECK(edited.config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        CHECK(edited.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#123456" });
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        // The overlay is a visible, revertible modification of the edited preset.
        CHECK(bundle.filaments.current_is_dirty());
        CHECK(pub.skipped_keys.empty());
    }

    SECTION("a slot replacement is reflected in the edited preset") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        Preset &abs = add_inmemory_preset(bundle.filaments, "My ABS");
        abs.config.opt_string("filament_type", 0u) = "ABS";
        abs.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.3 };
        bundle.filament_presets = { "My PLA" };
        REQUIRE(bundle.filaments.select_preset_by_name("My PLA", false));

        PublishedMaterialEntry entry = make_entry();
        entry.publish_type_value     = "ABS"; // mismatch: replaced by the library's ABS
        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { entry };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets[0] == "My ABS");
        // The edited preset now displays the replacement with the author's values on top.
        const Preset &edited = bundle.filaments.get_edited_preset();
        CHECK(edited.name == "My ABS");
        CHECK(edited.config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        CHECK(edited.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    }
}

// The overlay lands on the edited layer when the slot references the collection's edited
// preset, so the user's unsaved in-memory edits on it survive a published load (only the
// published keys are touched) and the change shows as a visible, revertible modification.
TEST_CASE("Published 3MF preserves unsaved edits on the edited filament preset", "[Preset][Bundle][Published]")
{
    PresetBundle bundle;
    Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.opt_string("filament_type", 0u) = "PLA";
    pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    pla.config.opt<ConfigOptionFloatsNullable>("filament_z_hop", true)->values = { 0.1 };
    bundle.filament_presets = { "My PLA" };
    REQUIRE(bundle.filaments.select_preset_by_name("My PLA", false));

    // The user has unsaved in-memory edits on the preset being shown.
    bundle.filaments.get_edited_preset().config.opt<ConfigOptionFloatsNullable>("filament_z_hop")->values = { 0.7 };

    PublishedMaterialEntry entry;
    entry.slot                = 0;
    entry.publish_type        = true;
    entry.publish_type_value  = "PLA";
    entry.publish_color       = true;
    entry.color               = "#ABCDEF";
    entry.keys                = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    DynamicPrintConfig config = published_pla_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Published values land on the edited layer...
    const Preset &edited = bundle.filaments.get_edited_preset();
    CHECK(edited.name == "My PLA");
    CHECK(edited.config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    CHECK(edited.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    // ...the user's unsaved edit on a non-published key survives...
    CHECK(edited.config.opt<ConfigOptionFloatsNullable>("filament_z_hop")->values == std::vector<double>{ 0.7 });
    // ...and the stored preset is untouched.
    Preset *stored = bundle.filaments.find_preset("My PLA", false, true);
    REQUIRE(stored != nullptr);
    CHECK(stored->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#123456" });
    CHECK(stored->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
    CHECK(stored->config.opt<ConfigOptionFloatsNullable>("filament_z_hop")->values == std::vector<double>{ 0.1 });
    // The overlay is a visible, revertible modification of the edited preset.
    CHECK(bundle.filaments.current_is_dirty());
    CHECK(pub.skipped_keys.empty());
    CHECK(pub.material_replacements.empty());
}

// The published overlay must validate '#' variant indices: an out-of-range index is reported as
// skipped and must NOT resize/corrupt the receiver's vector, and a variant suffix on a scalar
// key is rejected instead of silently no-op'd.
TEST_CASE("Published 3MF rejects out-of-range vector variants and variant-suffixed scalar keys", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    // Vector key, size 2 (matches the receiver's resized vector); distinct values make the
    // applied element observable.
    config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values = { 140., 150. };
    config.opt_float("layer_height") = 0.28;
    Preset::normalize(config);

    PresetBundle bundle;
    bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values = { 10., 20. };
    bundle.prints.get_edited_preset().config.opt_float("layer_height") = 0.1;

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = { "wiping_volumes_extruders#5", "wiping_volumes_extruders#1", "layer_height#0" };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // In-range variant applied element-wise; the out-of-range one did not resize the vector.
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values == std::vector<double>{ 10., 150. });
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values.size() == 2);
    // Out-of-range variant and variant-suffixed scalar are reported as skipped.
    CHECK(contains_key(pub.skipped_keys, "wiping_volumes_extruders#5"));
    CHECK(contains_key(pub.skipped_keys, "layer_height#0"));
    CHECK_FALSE(contains_key(pub.skipped_keys, "wiping_volumes_extruders#1"));
    // The scalar was never applied.
    CHECK_THAT(bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.1, 0.000001));
}

// The print/printer overlay guards option types like the material pass does: a published key
// whose file-side option kind differs from the receiver's is reported as skipped instead of
// throwing ConfigurationError out of load_config_model, which would abort the whole project
// load. (Nullable variants share the type() of their non-nullable base, so this covers
// genuinely different option kinds - e.g. a string where a float vector is expected.)
TEST_CASE("Published 3MF reports type-mismatched keys as skipped instead of aborting", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    // The file carries the vector key as a string option (equal size to the receiver's)...
    config.set_key_value("wiping_volumes_extruders", new ConfigOptionStrings({ "140", "150" }));
    config.opt_float("layer_height") = 0.28;

    PresetBundle bundle;
    // ...while the receiver's edited print preset holds the float variant of the same key:
    // without the type guard, ConfigOptionVector::set() throws ConfigurationError out of
    // load_config_model.
    bundle.prints.get_edited_preset().config.set_key_value("wiping_volumes_extruders", new ConfigOptionFloats({ 10., 20. }));
    bundle.prints.get_edited_preset().config.opt_float("layer_height") = 0.1;

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = { "wiping_volumes_extruders", "layer_height" };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The load completes; the type-mismatched key is reported as skipped and the receiver's
    // value is untouched; the matching scalar key still applies.
    CHECK(contains_key(pub.skipped_keys, "wiping_volumes_extruders"));
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values == std::vector<double>{ 10., 20. });
    CHECK_THAT(bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 0.000001));
    CHECK_FALSE(contains_key(pub.skipped_keys, "layer_height"));
}

// A receiver filament preset missing its material identity (hand-edited file) must not crash
// the type gate: the gate reads it as a type mismatch, and the slot falls back to the
// "no replacement" path (keys skipped, colour still applied to the slot's preset).
TEST_CASE("Published 3MF survives a receiver preset missing its material identity", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
    config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
    config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
    config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
    config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
    Preset::normalize(config);

    PresetBundle bundle;
    Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    // Malformed receiver preset: the identity options are missing entirely.
    pla.config.erase("filament_type");
    pla.config.erase("filament_vendor");
    pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.filament_type        = "PLA";
    entry.filament_vendor      = "Generic";
    entry.filament_id          = "GFL99";
    entry.slot                 = 0;
    entry.publish_type         = true; // exercises the type gate against the missing identity
    // Require ABS: the receiver library (PLA-typed default preset, typeless slot preset) has no
    // ABS candidate, so the gate falls into the "no replacement" path (a PLA requirement would
    // legitimately replace the slot with the visible PLA default).
    entry.publish_type_value   = "ABS";
    entry.publish_color        = true;
    entry.color                = "#ABCDEF";
    entry.keys                 = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // The gate reads the missing identity as a type mismatch; no same-type replacement exists,
    // so the keys are skipped while the colour applies to the slot's preset in place. No crash.
    CHECK(contains_key(pub.skipped_keys, "material:GFL99 (filament_retraction_length)"));
    CHECK(bundle.filament_presets[0] == "My PLA");
    Preset *mine_preset = bundle.filaments.find_preset("My PLA", false, true);
    REQUIRE(mine_preset != nullptr);
    CHECK(mine_preset->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
}

// Lock the exact contents and order of the printer allowlist (the union of the tab's
// "Retraction" and "Z-Hop" optgroup lists, Tab.cpp).
TEST_CASE("Printer publishable allowlist matches the printer tab's Retraction and Z-Hop optgroups", "[Preset][Bundle][Published]")
{
    auto keys_of = [](const std::vector<PublishablePrinterOption>& opts) {
        std::vector<std::string> keys;
        keys.reserve(opts.size());
        for (const PublishablePrinterOption& opt : opts)
            keys.emplace_back(opt.key);
        return keys;
    };

    const std::vector<std::string> expected_retraction = {
        "retraction_length", "retract_restart_extra", "retraction_speed", "deretraction_speed",
        "retraction_minimum_travel", "retract_when_changing_layer", "wipe", "wipe_distance",
        "retract_before_wipe", "retract_after_wipe"
    };
    const std::vector<std::string> expected_z_hop = {
        "retract_lift_enforce", "z_hop_types", "z_hop", "travel_slope", "retract_lift_above",
        "retract_lift_below"
    };

    CHECK(keys_of(publishable_printer_retraction_options()) == expected_retraction);
    CHECK(keys_of(publishable_printer_z_hop_options()) == expected_z_hop);

    std::set<std::string> expected_union(expected_retraction.begin(), expected_retraction.end());
    expected_union.insert(expected_z_hop.begin(), expected_z_hop.end());
    CHECK(publishable_printer_keys() == expected_union);
}

// Loading the same published file twice must not compound values on the receiver's presets:
// each load re-applies the same absolute values, so the result is idempotent.
TEST_CASE("Published 3MF reloading does not compound values on the receiver's presets", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9 };
        return config;
    };
    auto make_entry = [] {
        PublishedMaterialEntry entry;
        entry.slot          = 0;
        entry.publish_color = true;
        entry.color         = "#ABCDEF";
        entry.keys          = { "filament_retraction_length" };
        return entry;
    };

    PresetBundle bundle;
    Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.config.opt_string("filament_type", 0u) = "PLA";
    pla.config.opt<ConfigOptionStrings>("filament_colour", true)->values = { "#123456" };
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    auto load = [&] {
        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_entry() };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);
    };
    load();
    // First load: the receiver's preset carries the published values (mutated in place).
    CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    load();
    // Each load re-applies the same values onto the (already mutated) preset: no accumulation.
    CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
}

// A receiver with several slots aliasing the same preset (multi-extruder profile with one
// filament) and an author publishing keys on several slots: each published slot is re-pointed
// at its own distinct preset so values never leak between slots.
TEST_CASE("Published 3MF gives each published slot its own preset on an aliased receiver", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2, 3, 4 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PLA", "PLA", "PLA" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic", "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFL99", "GFL99", "GFL99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6, 0.9, 1.2, 1.5 };
        return config;
    };
    auto make_key_entry = [](int slot) {
        PublishedMaterialEntry entry;
        entry.slot = slot;
        entry.keys = { "filament_retraction_length" };
        return entry;
    };

    // A 4-extruder receiver with a single filament preset: the slots alias [A, A, A, A] before
    // the published pass.
    PresetBundle bundle;
    Preset &mine = add_inmemory_preset(bundle.filaments, "My PLA");
    mine.config.opt_string("filament_type", 0u) = "PLA";
    mine.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // Spare library presets for the re-pointing to fall back on.
    for (const char *name : { "Extra PLA A", "Extra PLA B", "Extra PLA C" }) {
        Preset &extra = add_inmemory_preset(bundle.filaments, name);
        extra.config.opt_string("filament_type", 0u) = "PLA";
        extra.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    }
    bundle.filament_presets = { "My PLA", "My PLA", "My PLA", "My PLA" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { make_key_entry(0), make_key_entry(1), make_key_entry(2), make_key_entry(3) };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    REQUIRE(bundle.filament_presets.size() == 4);
    // Every published slot references its own distinct preset: slot 0 keeps the receiver's
    // material, slots 1-3 are re-pointed at the spare library presets.
    CHECK(bundle.filament_presets[0] == "My PLA");
    CHECK(bundle.filament_presets[1] != bundle.filament_presets[0]);
    CHECK(bundle.filament_presets[2] != bundle.filament_presets[0]);
    CHECK(bundle.filament_presets[2] != bundle.filament_presets[1]);
    CHECK(bundle.filament_presets[3] != bundle.filament_presets[0]);
    CHECK(bundle.filament_presets[3] != bundle.filament_presets[1]);
    CHECK(bundle.filament_presets[3] != bundle.filament_presets[2]);
    // Each slot's stored preset carries its own slot's retraction (mutated in place).
    const std::vector<double> expected = { 0.6, 0.9, 1.2, 1.5 };
    for (size_t slot = 0; slot < 4; ++slot) {
        Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[slot], false, true);
        REQUIRE(preset != nullptr);
        CHECK(preset->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ expected[slot] });
    }
    CHECK(pub.skipped_keys.empty());
}

// Printer retraction keys are published per-extruder ("#N"): a receiver with a different
// extruder count still receives the in-range elements; out-of-range variants are reported as
// skipped instead of corrupting the receiver's vector.
TEST_CASE("Published 3MF applies per-extruder printer keys across extruder-count mismatches", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        // Author has 4 extruders.
        config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.6, 0.9, 1.2, 1.5 };
        Preset::normalize(config);
        return config;
    };

    // Receiver with a single extruder: only "#0" is in range; "#1..#3" are skipped.
    {
        PresetBundle bundle;
        bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.8 };
        PublishedConfig pub;
        pub.published      = true;
        pub.published_keys = { "retraction_length#0", "retraction_length#1", "retraction_length#2", "retraction_length#3" };
        DynamicPrintConfig config = make_file_config();
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{ 0.6 });
        CHECK(contains_key(pub.skipped_keys, "retraction_length#1"));
        CHECK(contains_key(pub.skipped_keys, "retraction_length#2"));
        CHECK(contains_key(pub.skipped_keys, "retraction_length#3"));
        CHECK_FALSE(contains_key(pub.skipped_keys, "retraction_length#0"));
    }

    // Receiver with four extruders and a 1-extruder author: only "#0" is published; the
    // receiver's other extruders keep their own values.
    {
        PresetBundle bundle;
        bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.8, 0.8, 0.8, 0.8 };
        PublishedConfig pub;
        pub.published      = true;
        pub.published_keys = { "retraction_length#0" };
        DynamicPrintConfig config = make_file_config();
        // The author's file carries a single-extruder value.
        config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.7 };
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{ 0.7, 0.8, 0.8, 0.8 });
        CHECK(pub.skipped_keys.empty());
    }
}

