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
// author-selected process keys onto the edited preset. Mirrors the GUI load path
// (src/slic3r/GUI/Plater.cpp): Preset::normalize before load_config_model, then the
// published overlay in PresetBundle::load_config_file_config.
TEST_CASE("Published 3MF overlays only the author-selected process keys onto the edited preset", "[Preset][Bundle][Published]")
{
    // The file config the GUI builds from a .3mf's project settings.
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // The loader derives the filament count from filament_colour and throws when it is
        // empty ("Invalid configuration file"); a 3mf always carries it.
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
        // Process scalar key.
        config.opt_float("layer_height") = 0.28;
        // Process vector key, size 2 to match the edited preset's resized vector.
        config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values = { 140., 150. };
        // Process vector key, size 2: deliberately mismatched against the edited preset.
        config.opt<ConfigOptionStrings>("post_process")->values = { "script-a", "script-b" };
        // A filament key: published files may still carry legacy filament keys.
        config.opt<ConfigOptionInts>("nozzle_temperature")->values = { 220 };
        // A structural (denylisted) key: must be silently ignored even if a hand-crafted
        // file lists it as published. full_print_config() omits the *_settings_id keys (they
        // have no static counterpart), while a real 3mf project config carries it, so create
        // it explicitly.
        config.opt_string("print_settings_id", true) = "file process";
        // Project-level filament/purge data: must NOT cross over in published mode.
        config.opt<ConfigOptionFloats>("flush_multiplier")->values = { 2., 2. };
        // A project-level option, to pin the project_config.apply_only() invariant.
        config.opt<ConfigOptionFloats>("wipe_tower_x")->values = { 100. };
        // The author's bed type must NOT cross over either: the receiver keeps its own.
        config.option("curr_bed_type")->setInt(BedType::btPC);
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
    // Capture the ctor-seeded project_config values; the assertions below check that the
    // published load leaves them untouched rather than hardcoding the defaults.
    const std::vector<std::string> seed_filament_colour  = bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values;
    const std::vector<double>      seed_flush_multiplier = bundle.project_config.opt<ConfigOptionFloats>("flush_multiplier")->values;
    const int                      seed_bed_type         = bundle.project_config.option("curr_bed_type")->getInt();

    DynamicPrintConfig config = make_file_config();
    // The GUI normalizes the config before load; do the same so only the production path is exercised.
    Preset::normalize(config);

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = published_keys;
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // a) The process scalar is overlaid onto the edited preset.
    CHECK_THAT(bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 0.000001));

    // b) A matching-size process vector is applied; a size-mismatched one is neither applied
    // nor reported as skipped by accident — it lands in skipped_keys.
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionFloats>("wiping_volumes_extruders")->values == std::vector<double>{ 140., 150. });
    CHECK(bundle.prints.get_edited_preset().config.opt<ConfigOptionStrings>("post_process")->values == std::vector<std::string>{ "existing-script" });
    CHECK(contains_key(pub.skipped_keys, "post_process"));

    // c) A filament key is never applied anywhere and is reported as skipped (warning).
    CHECK(bundle.prints.get_edited_preset().config.option("nozzle_temperature") == nullptr);
    CHECK(contains_key(pub.skipped_keys, "nozzle_temperature"));

    // d) A structural key is silently ignored: neither applied nor reported as skipped.
    CHECK(bundle.prints.get_edited_preset().config.opt_string("print_settings_id") == "user process");
    CHECK_FALSE(contains_key(pub.skipped_keys, "print_settings_id"));

    // Applied keys are not reported as skipped.
    CHECK_FALSE(contains_key(pub.skipped_keys, "layer_height"));
    CHECK_FALSE(contains_key(pub.skipped_keys, "wiping_volumes_extruders"));

    // e) project_config.apply_only() still runs, but in published mode only the plate/bed
    // geometry crosses: the file's filament/purge data must NOT port to the receiver.
    // Filament colors do not port; project_config keeps its ctor-seeded values.
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values != std::vector<std::string>{ "#FF0000" });
    CHECK(bundle.project_config.opt<ConfigOptionStrings>("filament_colour")->values == seed_filament_colour);
    // Purge data does not port either, and update_multi_material_filament_presets() cannot
    // resurrect it (flush_multiplier stays at the ctor seed).
    CHECK(bundle.project_config.opt<ConfigOptionFloats>("flush_multiplier")->values == seed_flush_multiplier);
    // The author's bed type does not cross over: the receiver keeps its own.
    CHECK(bundle.project_config.option("curr_bed_type")->getInt() == seed_bed_type);
    // Plate/bed geometry still crosses.
    CHECK(bundle.project_config.opt<ConfigOptionFloats>("wipe_tower_x")->values == std::vector<double>{ 100. });

    // The published path keeps the user's currently-selected presets: the edited process
    // preset is the same preset as before the load.
    CHECK(bundle.prints.get_edited_preset().name == pre_load_name);
    CHECK(bundle.prints.size() == pre_load_size);

    // f) Non-published control: with published=false the overlay is disabled. The file's
    // presets are loaded and selected instead (the user's preset is not kept) and no
    // skipped_keys are produced.
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
    // The file's layer_height reached the edited preset through the normal preset import,
    // not through the published overlay.
    CHECK_THAT(control_bundle.prints.get_edited_preset().config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 0.000001));
}

// The published printer overlay is restricted to the publishable retraction/z-hop allowlist
// (publishable_printer_keys). Matching-size retraction vectors apply; mismatched vectors are
// reported as skipped; any other printer-class key (e.g. machine_start_gcode) is
// contract-excluded: never applied and never reported.
TEST_CASE("Published 3MF overlays only the allowlisted retraction and z-hop keys onto the edited printer preset", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    Preset::normalize(config);
    // Matching size (the receiver's default printer has one extruder).
    config.opt<ConfigOptionFloats>("retraction_length")->values = { 1.4 };
    // Size 2: mismatched against the single-extruder receiver.
    config.opt<ConfigOptionFloats>("retraction_speed")->values = { 45., 55. };
    // Printer-class but outside the allowlist: must be silently contract-excluded.
    config.opt_string("machine_start_gcode") = "G28 ; from file";

    PresetBundle bundle;
    bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values = { 0.8 };
    bundle.printers.get_edited_preset().config.opt_string("machine_start_gcode") = "G28 ; user";

    PublishedConfig pub;
    pub.published      = true;
    pub.published_keys = { "retraction_length", "retraction_speed", "machine_start_gcode" };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Matching-size retraction vector applied to the edited printer preset.
    CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{ 1.4 });
    // Mismatched vector not applied and reported as skipped.
    CHECK(bundle.printers.get_edited_preset().config.opt<ConfigOptionFloats>("retraction_speed")->values == std::vector<double>{ 30. });
    CHECK(contains_key(pub.skipped_keys, "retraction_speed"));
    // Contract-excluded printer key: silently ignored, absent from skipped_keys.
    CHECK(bundle.printers.get_edited_preset().config.opt_string("machine_start_gcode") == "G28 ; user");
    CHECK_FALSE(contains_key(pub.skipped_keys, "machine_start_gcode"));
}

// A published 3MF can carry material-qualified keys; on load they are applied to the
// receiver's filament presets whose material identity matches the author's (filament_id when
// both sides have one, filament_type + vendor fallback otherwise).
TEST_CASE("Published 3MF applies material retraction keys onto the receiver's matching filament presets", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Two filament slots; filament_diameter drives the normalized per-slot vector sizes.
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        // Keep the multi-extruder consistency validation happy for a 2-slot config.
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard" };
        // Author per-slot material identity (filament_ids feeds the loader's local copy).
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
    // Receiver materials with matching stable ids.
    Preset &pla  = add_inmemory_preset(bundle.filaments, "My PLA");
    pla.filament_id = "GFL99";
    pla.config.opt_string("filament_type", 0u)   = "PLA";
    pla.config.opt_string("filament_vendor", 0u) = "Generic";
    // In-memory preset configs carry the per-filament retraction keys as nullable options
    // (the type real filament presets hold), so access them through the nullable type.
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    pla.config.opt<ConfigOptionStrings>("filament_settings_id")->values = { "receiver-pla" };
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.filament_id = "GFT99";
    petg.config.opt_string("filament_type", 0u)   = "PETG";
    petg.config.opt_string("filament_vendor", 0u) = "Generic";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    // The z-hop key must exist on the receiver preset for the overlay to apply into it.
    petg.config.opt<ConfigOptionFloatsNullable>("filament_z_hop", true)->values = { 0.1 };
    bundle.filament_presets = { "My PLA", "My PETG" };

    PublishedMaterialEntry pla_entry;
    pla_entry.filament_type   = "PLA";
    pla_entry.filament_vendor = "Generic";
    pla_entry.filament_id     = "GFL99";
    pla_entry.slot            = 0; // the author's PLA slot
    pla_entry.keys            = { "filament_retraction_length", "filament_settings_id" };
    PublishedMaterialEntry petg_entry;
    petg_entry.filament_type   = "PETG";
    petg_entry.filament_vendor = "Generic";
    petg_entry.filament_id     = "GFT99";
    petg_entry.slot            = 1; // the author's PETG slot
    petg_entry.keys            = { "filament_retraction_length", "filament_z_hop" };
    // A material that does not exist on the author's side: whole entry skipped, no reporting.
    PublishedMaterialEntry abs_entry;
    abs_entry.filament_type = "ABS";
    abs_entry.filament_id   = "GFX99";
    abs_entry.keys          = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published      = true;
    pub.material_keys  = { pla_entry, petg_entry, abs_entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Per-slot scalar copy: the author's slot value lands in the matching receiver preset.
    CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
    CHECK(bundle.filaments.find_preset("My PETG")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 1.2 });
    CHECK(bundle.filaments.find_preset("My PETG")->config.opt<ConfigOptionFloatsNullable>("filament_z_hop")->values == std::vector<double>{ 0.3 });
    // Structural keys inside a material entry are silently ignored: the receiver's own
    // filament_settings_id is left untouched and nothing is reported for it.
    CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{ "receiver-pla" });
    CHECK_FALSE(contains_key(pub.skipped_keys, "material:GFL99 (filament_settings_id)"));
    // Everything applied; the unknown material entry produced no skipped entry.
    CHECK(pub.skipped_keys.empty());
}

// Filament-publishing v2: a "full publish" slot serializes the entire filament of the slot. On
// load the slot is matched positionally against the published (curated, vendor-agnostic) type:
// a matching receiver type leaves the slot untouched, a mismatched type replaces it with the
// first same-type visible preset (applying the author's full values on top), and a slot whose
// type cannot be found in the receiver's library falls back to the author's values in-memory.
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

    SECTION("type match leaves a full-published slot untouched") {
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

        // The receiver keeps its own material and its own values: the full dump is ignored.
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
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

        CHECK(bundle.filament_presets.size() == 1);
        CHECK(bundle.filament_presets[0] == "My ABS");
        // The author's slot-0 full values were applied onto the replacement.
        CHECK(bundle.filaments.find_preset("My ABS")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(pub.skipped_keys.empty());
        REQUIRE(pub.material_replacements.size() == 1);
    }

    SECTION("no same-type match creates a temporary project-embedded custom preset") {
        PresetBundle bundle;
        Preset &pla = add_inmemory_preset(bundle.filaments, "My PLA");
        pla.config.opt_string("filament_type", 0u) = "PLA";
        pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        bundle.filament_presets = { "My PLA" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_full_abs_entry() };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // No ABS in the library: a temporary embedded preset is created and selected.
        CHECK(bundle.filament_presets[0] == "ABS (Published)");
        Preset *created = bundle.filaments.find_preset("ABS (Published)");
        REQUIRE(created != nullptr);
        CHECK(created->is_project_embedded);
        CHECK(created->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        // The original user preset remains untouched. Re-fetch by name: load_preset's deque
        // insertion relocated the presets, so the pre-load `pla` reference points at the
        // newly created "ABS (Published)" slot.
        CHECK(bundle.filaments.find_preset("My PLA", false, true)->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(pub.skipped_keys.empty());
        REQUIRE(pub.material_replacements.size() == 1);
    }
}

// Filament-publishing v2: a partially-published slot can carry a curated type and/or colour.
// The colour is applied regardless of the type match; a type mismatch with no same-type
// replacement keeps the receiver's material and reports the slot's keys as skipped. The
// receiver's slot count grows only as far as the highest slot with published content.
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

        // Type matched: keys applied, colour applied.
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
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

        // Colour still applies (type-independent); the material is kept and the keys skipped.
        CHECK(bundle.filament_presets[0] == "My PLA");
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
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

        // The slot list was grown so author slot 1 has a material, automatically assigning the PETG preset.
        REQUIRE(bundle.filament_presets.size() == 2);
        CHECK(bundle.filament_presets[1] == "My PETG");
    }
}

// The receiver's slot list grows only as far as the highest author slot that carries published
// content: a 4-filament file whose author published nothing (or only a low slot) must not pull
// filler materials into the receiver's setup, and the receiver never grows to the file's count.
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
        CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
    }

    // Slot 3 published: the receiver grows to 4 so the published slot exists. The unpublished
    // filler slots repeat the receiver's last preset ("Add one filament" behaviour); the
    // published slot gets a visible preset not used by another slot (with a single-preset
    // library it falls back to the receiver's last preset, aliasing being unavoidable).
    {
        PresetBundle bundle;
        add_pla_preset(bundle);
        bundle.filament_presets = { "My PLA" };
        const std::string filler = bundle.filaments.first_visible().name;

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_color_entry(3) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        REQUIRE(bundle.filament_presets.size() == 4);
        CHECK(bundle.filament_presets[1] == "My PLA");
        CHECK(bundle.filament_presets[2] == "My PLA");
        CHECK(bundle.filament_presets[3] == filler);
        // The project-level per-slot vectors were grown and seeded like "Add one filament":
        // fillers take their preset's colour, the published slot its published colour.
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

// The published overlay mutates stored filament presets in place per slot, so a slot carrying
// published content must never share its stored preset with another slot: its colour/keys
// would leak into the sibling slot - and, with "repeat the last preset" growth, into the
// receiver's own first slot. Regression for the slot-aliasing hazard.
TEST_CASE("Published 3MF gives grown published slots a distinct preset so values never leak", "[Preset][Bundle][Published]")
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

    // The receiver has one slot of its own material plus one more preset in the library; the
    // author publishes only slot 4 (Red). With naive repeat-last growth the new slot would
    // reference the receiver's own preset and the published red would recolor it; the grown
    // slot must point at a distinct preset.
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
    // The published slot references the unused library preset, not the receiver's own...
    CHECK(bundle.filament_presets[3] == "Other PLA");
    // ...so the published colour landed there and never recoloured the receiver's material.
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
// selection time. The published overlay modifies the collection presets in place, so the load
// must re-select the first slot's filament (mirroring a normal project load) for the applied
// colour/type/keys - and slot replacements - to surface in the GUI.
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
        CHECK(edited.config.opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });
        CHECK(edited.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
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

// Material-qualified keys whose receiver-side material match is missing or ambiguous must be
// reported as skipped (material-qualified) and never applied; a single unqualified type
// fallback still applies.
TEST_CASE("Published 3MF reports material keys with no unique receiver match as skipped", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Three author slots: PLA, PETG, ABS.
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2, 3 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PLA", "PETG", "ABS" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFL99", "GFT99", "GFA99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.9, 1.2, 1.5 };
        return config;
    };

    PresetBundle bundle;
    // Two receiver presets of the SAME type with no filament_id: the type fallback is ambiguous.
    Preset &pla_a = add_inmemory_preset(bundle.filaments, "My PLA A");
    pla_a.config.opt_string("filament_type", 0u)   = "PLA";
    pla_a.config.opt_string("filament_vendor", 0u) = "Generic";
    pla_a.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    Preset &pla_b = add_inmemory_preset(bundle.filaments, "My PLA B");
    pla_b.config.opt_string("filament_type", 0u)   = "PLA";
    pla_b.config.opt_string("filament_vendor", 0u) = "Generic";
    pla_b.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    // A unique PETG receiver preset: the single type fallback is unambiguous.
    Preset &petg = add_inmemory_preset(bundle.filaments, "My PETG");
    petg.config.opt_string("filament_type", 0u)   = "PETG";
    petg.config.opt_string("filament_vendor", 0u) = "Generic";
    petg.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.6 };
    bundle.filament_presets = { "My PLA A", "My PLA B", "My PETG" };

    auto make_entry = [](const std::string &type, const std::string &key) {
        PublishedMaterialEntry entry;
        entry.filament_type   = type;
        entry.filament_vendor = "Generic";
        entry.keys            = { key };
        return entry;
    };

    PublishedMaterialEntry pla_entry  = make_entry("PLA", "filament_retraction_length");
    pla_entry.slot = 0; // the author's PLA slot
    PublishedMaterialEntry petg_entry = make_entry("PETG", "filament_retraction_length");
    petg_entry.slot = 1; // the author's PETG slot
    PublishedMaterialEntry abs_entry  = make_entry("ABS", "filament_retraction_length"); // author slot exists, no receiver match
    abs_entry.slot = 2;

    PublishedConfig pub;
    pub.published      = true;
    pub.material_keys  = { pla_entry, petg_entry, abs_entry };
    DynamicPrintConfig config = make_file_config();
    Preset::normalize(config);
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // Ambiguous type fallback: neither PLA preset is touched, reported as skipped.
    CHECK(bundle.filaments.find_preset("My PLA A")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
    CHECK(bundle.filaments.find_preset("My PLA B")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
    CHECK(contains_key(pub.skipped_keys, "material:PLA (filament_retraction_length)"));
    // Unambiguous single fallback: applied.
    CHECK(bundle.filaments.find_preset("My PETG")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 1.2 });
    CHECK_FALSE(contains_key(pub.skipped_keys, "material:PETG (filament_retraction_length)"));
    // Receiver-side miss: the author slot exists but no receiver preset matches.
    CHECK(contains_key(pub.skipped_keys, "material:ABS (filament_retraction_length)"));
}

// A slotted material entry carries the author's per-slot overrides: on load it applies to the
// receiver's matching preset at the author's slot ordinal (first matching author slot -> first
// matching receiver preset, second -> second, ...). Legacy entries without a slot keep applying
// to every matching receiver preset.
TEST_CASE("Published material keys apply to the receiver's matching filament preset by author slot ordinal", "[Preset][Bundle][Published]")
{
    auto make_file_config = [] {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        // Three author slots of the SAME material (PETG) with distinct per-slot retraction.
        config.opt<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75 };
        config.opt<ConfigOptionInts>("filament_self_index")->values = { 1, 2, 3 };
        config.opt<ConfigOptionStrings>("filament_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" };
        config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF" };
        config.opt<ConfigOptionStrings>("filament_type")->values = { "PETG", "PETG", "PETG" };
        config.opt<ConfigOptionStrings>("filament_vendor")->values = { "Generic", "Generic", "Generic" };
        config.opt<ConfigOptionStrings>("filament_ids")->values = { "GFT99", "GFT99", "GFT99" };
        config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.7, 0.8, 0.9 };
        return config;
    };
    auto make_slotted_entry = [](int slot) {
        PublishedMaterialEntry entry;
        entry.filament_type   = "PETG";
        entry.filament_vendor = "Generic";
        entry.filament_id     = "GFT99";
        entry.slot            = slot;
        entry.keys            = { "filament_retraction_length" };
        return entry;
    };
    auto add_petg_preset = [](PresetBundle &bundle, const std::string &name) {
        Preset &preset = add_inmemory_preset(bundle.filaments, name);
        preset.filament_id = "GFT99";
        preset.config.opt_string("filament_type", 0u)   = "PETG";
        preset.config.opt_string("filament_vendor", 0u) = "Generic";
        preset.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
        return &preset;
    };

    // Three receiver presets, one per author slot: each gets its ordinal's value.
    {
        PresetBundle bundle;
        add_petg_preset(bundle, "My PETG 1");
        add_petg_preset(bundle, "My PETG 2");
        add_petg_preset(bundle, "My PETG 3");
        bundle.filament_presets = { "My PETG 1", "My PETG 2", "My PETG 3" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_slotted_entry(0), make_slotted_entry(1), make_slotted_entry(2) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        // Each author slot's value lands in the receiver preset at the same ordinal.
        CHECK(bundle.filaments.find_preset("My PETG 1")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.7 });
        CHECK(bundle.filaments.find_preset("My PETG 2")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.8 });
        CHECK(bundle.filaments.find_preset("My PETG 3")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.9 });
        CHECK(pub.skipped_keys.empty());
    }

    // A single receiver preset: only the first ordinal fits; the later slots are reported
    // with a slot-qualified label.
    {
        PresetBundle bundle;
        add_petg_preset(bundle, "My PETG 1");
        bundle.filament_presets = { "My PETG 1" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_slotted_entry(0), make_slotted_entry(1), make_slotted_entry(2) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filaments.find_preset("My PETG 1")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.7 });
        CHECK(contains_key(pub.skipped_keys, "material:GFT99 slot 1 (filament_retraction_length)"));
        CHECK(contains_key(pub.skipped_keys, "material:GFT99 slot 2 (filament_retraction_length)"));
        CHECK_FALSE(contains_key(pub.skipped_keys, "material:GFT99 slot 0 (filament_retraction_length)"));
    }

    // A legacy entry (no slot) applies to every matching receiver preset, from the first
    // author slot.
    {
        PresetBundle bundle;
        add_petg_preset(bundle, "My PETG 1");
        add_petg_preset(bundle, "My PETG 2");
        bundle.filament_presets = { "My PETG 1", "My PETG 2" };

        PublishedMaterialEntry legacy = make_slotted_entry(0);
        legacy.slot = -1; // legacy: no slot field
        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { legacy };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filaments.find_preset("My PETG 1")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.7 });
        CHECK(bundle.filaments.find_preset("My PETG 2")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.7 });
        CHECK(pub.skipped_keys.empty());
    }

    // An out-of-range author slot is silently skipped: nothing applied, nothing reported.
    {
        PresetBundle bundle;
        add_petg_preset(bundle, "My PETG 1");
        bundle.filament_presets = { "My PETG 1" };

        PublishedConfig pub;
        pub.published     = true;
        pub.material_keys = { make_slotted_entry(5) };
        DynamicPrintConfig config = make_file_config();
        Preset::normalize(config);
        bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

        CHECK(bundle.filaments.find_preset("My PETG 1")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
        CHECK(pub.skipped_keys.empty());
    }
}

// The published overlay must validate '#' variant indices: an out-of-range index must be
// reported as skipped and must NOT resize/corrupt the receiver's vector, and a variant suffix
// on a scalar key must be rejected instead of silently no-op'd.
TEST_CASE("Published 3MF rejects out-of-range vector variants and variant-suffixed scalar keys", "[Preset][Bundle][Published]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    // Vector key, size 2 (matches the receiver's resized vector); distinct values so the
    // applied element is observable.
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

// A receiver filament preset whose material identity fields are missing (hand-edited preset
// file) must not crash the material pass: the entry simply cannot match and is reported skipped.
TEST_CASE("Published 3MF survives a receiver filament preset missing its material identity", "[Preset][Bundle][Published]")
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
    pla.config.opt<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = { 0.5 };
    bundle.filament_presets = { "My PLA" };

    PublishedMaterialEntry entry;
    entry.filament_type   = "PLA";
    entry.filament_vendor = "Generic";
    entry.filament_id     = "GFL99";
    entry.slot            = 0;
    entry.keys            = { "filament_retraction_length" };

    PublishedConfig pub;
    pub.published     = true;
    pub.material_keys = { entry };
    bundle.load_config_model("test.3mf", std::move(config), Semver(), &pub);

    // No match possible without the identity fields: reported skipped, preset untouched.
    CHECK(contains_key(pub.skipped_keys, "material:GFL99 (filament_retraction_length)"));
    CHECK(bundle.filaments.find_preset("My PLA")->config.opt<ConfigOptionFloatsNullable>("filament_retraction_length")->values == std::vector<double>{ 0.5 });
}

// The printer publishable allowlist is the union of the printer tab's "Retraction" and
// "Z-Hop" optgroup option lists; lock the exact contents and order (Tab.cpp).
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

