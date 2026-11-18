#include "InstallOps.hpp"

#include "Outcome.hpp"
#include "PresetOps.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/UnsavedChangesDialog.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetCacheFormat.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <exception>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wx/string.h>
#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace Ops {

using nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// A string member of a JSON object, empty where it has none.
std::string text(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The nozzles of a model as the dialog's data lists them, "0.4;0.6".
std::vector<std::string> split_nozzles(const std::string& list)
{
    std::vector<std::string> nozzles;
    std::size_t              start = 0;
    while (start <= list.size()) {
        const std::size_t end    = std::min(list.find(';', start), list.size());
        std::string       nozzle = boost::algorithm::trim_copy(list.substr(start, end - start));
        if (!nozzle.empty())
            nozzles.push_back(std::move(nozzle));
        start = end + 1;
    }
    return nozzles;
}

Refusal no_such_vendor(const std::string& vendor, std::vector<std::string> vendors)
{
    return {RefusalKind::Precondition, "no_such_vendor", L("No printer vendor is called \"%1%\"."), {vendor},
            closest_names(std::move(vendors), vendor)};
}

// The vendors of the models `profile` lists, each once.
std::vector<std::string> profile_vendors(const json& profile)
{
    std::set<std::string> vendors;
    if (const auto models = profile.find("model"); models != profile.end() && models->is_array())
        for (const json& entry : *models)
            if (entry.is_object())
                vendors.insert(text(entry, "vendor"));
    vendors.erase(std::string());
    return {vendors.begin(), vendors.end()};
}

std::size_t nozzle_count(const InstallSet& set)
{
    std::size_t count = 0;
    for (const auto& [vendor, models] : set.printers)
        for (const auto& [model, nozzles] : models)
            count += nozzles.size();
    return count;
}

// The Transfer of the prompt about unsaved changes that a Confirm opens (GUI_App::check_and_keep_current_preset_changes())
// with every change it lists ticked: each tab caches what it lists of its preset's changes, and the printer tab the
// extruder count where that changed, for GUI_App::apply_keeped_preset_modifications() to put on the presets the reload
// selects.
void cache_listed_changes()
{
    GUI_App&                app        = wxGetApp();
    const PrinterTechnology technology = app.preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : app.tabs_list) {
        if (!tab->supports_printer_technology(technology) || !tab->current_preset_is_dirty())
            continue;
        const PresetCollection&  presets = *tab->get_presets();
        std::vector<std::string> keys;
        for (const std::string& option : listed_dirty_options(presets))
            if (std::string key = option.substr(0, option.find('#')); std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(std::move(key));
        if (tab->type() == Preset::TYPE_PRINTER && technology == ptFFF) {
            const auto* saved  = presets.get_selected_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            const auto* edited = presets.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            if (saved != nullptr && edited != nullptr && saved->values.size() != edited->values.size())
                static_cast<TabPrinter*>(tab)->cache_extruder_cnt();
        }
        tab->cache_config_diff(keys);
    }
}

// The system presets of `presets` the selectors offer.
std::vector<std::string> offered(const PresetCollection& presets)
{
    std::vector<std::string> names;
    for (const Preset& preset : presets)
        if (preset.is_system && preset.is_visible)
            names.push_back(preset.name);
    return names;
}

// Whether the selectors offer a system printer preset of `vendor`'s `model` with `nozzle`.
bool offers_printer(const PresetBundle& bundle, const std::string& vendor, const std::string& model, const std::string& nozzle)
{
    for (const Preset& preset : bundle.printers)
        if (preset.is_system && preset.is_visible && preset.vendor != nullptr && preset.vendor->id == vendor &&
            preset.config.opt_string("printer_model") == model && preset.config.opt_string("printer_variant") == nozzle)
            return true;
    return false;
}

// The names of `a` that `b` lacks, in `a`'s order.
std::vector<std::string> missing_from(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    std::vector<std::string> out;
    for (const std::string& name : a)
        if (std::find(b.begin(), b.end(), name) == b.end())
            out.push_back(name);
    return out;
}

Outcome<InstalledPrinters> change_printers(const InstallPrintersArgs& args, bool install)
{
    assert(wxThread::IsMain());
    const Outcome<std::vector<ModelNozzles>> resolved = resolve_models(args.known, args.models);
    if (!resolved)
        return resolved.refusal();
    const InstallSet installed = installed_set();
    if (!install)
        if (std::optional<Refusal> refusal = uninstall_refusal(installed, resolved.value()))
            return *refusal;
    const InstallSet changed = install ? changed_install_set(installed, resolved.value(), {}) :
                                         changed_install_set(installed, {}, resolved.value());

    GUI_App&          app    = wxGetApp();
    PresetBundle&     bundle = *app.preset_bundle;
    InstalledPrinters out;
    out.models = resolved.value();
    if (changed.printers != installed.printers) {
        // The reload drops the presets' unsaved changes, which the Confirm asks about; a call says what becomes of them.
        if (args.changes == PresetChanges::Ask && app.has_current_preset_changes())
            return Refusal{RefusalKind::Precondition, "unsaved_preset_changes",
                           L("The presets have unsaved changes, which reloading the presets would drop.")};
        const std::vector<std::string> before = offered(bundle.printers), filaments_before = offered(bundle.filaments);
        const bool                     discards = args.changes == PresetChanges::Discard && app.has_current_preset_changes();
        try {
            const Outcome<Done> applied = apply_install_set(changed, args.changes);
            if (!applied && applied.refusal().code == std::string("vendors_not_installed"))
                // The answer about the unsaved changes and the copies of the bundles came before the failure.
                return Refusal{RefusalKind::Failed, "vendors_not_installed",
                               discards ? L("The printer profiles could not be installed, so the installed printers are as they "
                                            "were, but the presets' unsaved changes were discarded. Vendor profiles copied "
                                            "before the failure stay in the configuration folder.") :
                                          L("The printer profiles could not be installed, so the installed printers are as they "
                                            "were. Vendor profiles copied before the failure stay in the configuration folder.")};
            if (!applied)
                return applied.refusal();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "install_printers: reloading the presets failed: " << e.what();
            return Refusal{RefusalKind::Failed, "presets_not_reloaded",
                           // TRN %1% is the error's own text.
                           L("The installed printers were changed, but reloading the presets failed: %1%"), {e.what()}};
        }
        // What a person's Confirm refreshes once the dialog has closed (GUI_App::run_wizard()), and the app configuration
        // written, so that the change is kept however the app ends.
        app.load_current_presets();
        app.update_publish_status();
        if (app.mainframe != nullptr)
            app.mainframe->refresh_plugin_tips();
        app.app_config->save();
        const std::vector<std::string> after = offered(bundle.printers), filaments_after = offered(bundle.filaments);
        out.added                 = missing_from(after, before);
        out.removed               = missing_from(before, after);
        out.filaments_installed   = missing_from(filaments_after, filaments_before);
        out.filaments_uninstalled = missing_from(filaments_before, filaments_after);
    }
    out.selected = bundle.printers.get_selected_preset_name();
    for (const ModelNozzles& model : out.models) {
        ModelNozzles left{model.vendor, model.model, {}};
        for (const std::string& nozzle : model.nozzles)
            if (offers_printer(bundle, model.vendor, model.model, nozzle) != install)
                left.nozzles.push_back(nozzle);
        if (!left.nozzles.empty())
            out.missing.push_back(std::move(left));
    }
    return out;
}

// Whether the filaments section `section` installs `preset`: by its name, or by a name it was renamed from, as
// Preset::set_visible_from_appconfig() reads it.
bool installs(const std::map<std::string, std::string>& section, const Preset& preset)
{
    const auto has = [&section](const std::string& name) {
        const auto it = section.find(name);
        return it != section.end() && !it->second.empty();
    };
    return has(preset.name) || std::any_of(preset.renamed_from.begin(), preset.renamed_from.end(), has);
}

Outcome<InstalledFilaments> change_filaments(const InstallFilamentsArgs& args, bool install)
{
    assert(wxThread::IsMain());
    GUI_App&      app    = wxGetApp();
    PresetBundle& bundle = *app.preset_bundle;
    AppConfig&    config = *app.app_config;
    const Outcome<std::vector<std::string>> named = filaments_named(loaded_filaments(), args);
    if (!named)
        return named.refusal();
    if (!install)
        if (std::optional<Refusal> refusal = filament_uninstall_refusal(named.value(), bundle.filament_presets))
            return *refusal;

    // Without the section every filament is installed: an install changes nothing, and an uninstall writes the section
    // with every system filament the selectors offer, so that the others stay installed.
    std::map<std::string, std::string> section = installed_set_from(config, offered(bundle.filaments)).filaments;

    InstalledFilaments         out;
    std::vector<const Preset*> edited;
    for (const std::string& name : named.value()) {
        const Preset* preset = bundle.filaments.find_preset(name, false);
        if (preset == nullptr)
            continue;
        if (installs(section, *preset) == install) {
            out.unchanged.push_back(name);
            continue;
        }
        if (install) {
            section[name] = "true";
        } else {
            section.erase(name);
            for (const std::string& old_name : preset->renamed_from)
                section.erase(old_name);
        }
        edited.push_back(preset);
    }
    if (edited.empty())
        return out;

    // What is installed then is the app's rule, as the next start applies it: an installed printer that no installed
    // filament fits gets its default filaments again, into the section, and every filament is shown as the section says.
    // The rule takes the printers shown for installed, as they are at a start, where load_selections() has just shown
    // the installed ones (PresetBundle::load_installed_printers()). A running app also shows printers it does not
    // install, for this session only: one selected while not installed, a bound one in the sidebar's list. Those are
    // hidden while the rule runs and shown again after it, so that the section gets no filament for them.
    config.set_section(AppConfig::SECTION_FILAMENTS, section);
    {
        std::vector<bool> shown;
        for (const Preset& printer : bundle.printers)
            shown.push_back(printer.is_visible);
        ScopeGuard show_again([&bundle, &shown] {
            size_t i = 0;
            for (Preset& printer : bundle.printers)
                printer.is_visible = shown[i++];
        });
        // What load_installed_printers() does to the printers, without its rebuild of the alias and rename maps, which
        // counts a duplicate rename as a load error again.
        for (Preset& printer : bundle.printers)
            printer.set_visible_from_appconfig(config);
        bundle.load_installed_filaments(config);
    }
    // A filament a slot holds stays shown, installed or not, as the slot's selector shows it
    // (PlaterPresetComboBox::update()); the tabs' selectors are refreshed before the slots' are.
    for (const std::string& name : bundle.filament_presets)
        if (Preset* preset = bundle.filaments.find_preset(name, false); preset != nullptr)
            preset->is_visible = true;
    const std::map<std::string, std::string> result = config.get_section(AppConfig::SECTION_FILAMENTS);
    for (const Preset* preset : edited)
        if (installs(result, *preset) == install)
            out.changed.push_back(preset->name);
    for (const Preset& preset : bundle.filaments)
        if (preset.is_system && !installs(section, preset) && installs(result, preset))
            out.defaults_installed.push_back(preset.name);
    config.save();
    if (app.mainframe != nullptr)
        app.mainframe->update_side_preset_ui();
    return out;
}

} // namespace

// ---- Printer models ----

json profile_data_from(const PresetBundle& bundle)
{
    json data{{"model", json::array()}, {"machine", json::object()}, {"filament", json::object()}, {"process", json::array()}};

    // Models from vendor profiles
    for (const auto& [vendor_id, vp] : bundle.vendors) {
        for (const auto& model : vp.models) {
            std::string nozzle_str;
            for (const auto& v : model.variants) {
                if (!nozzle_str.empty())
                    nozzle_str += ";";
                nozzle_str += v.name;
            }
            const std::string       materials_str = boost::algorithm::join(model.default_materials, ";");
            boost::filesystem::path cover_path =
                (boost::filesystem::path(resources_dir()) / "profiles" / vp.id / (model.id + "_cover.png")).make_preferred();
            if (!boost::filesystem::exists(cover_path))
                cover_path = (boost::filesystem::path(resources_dir()) / "web/image/printer" / (model.id + "_cover.png")).make_preferred();

            json entry;
            entry["model"]           = model.id;
            entry["name"]            = model.name;
            entry["vendor"]          = vp.id;
            entry["nozzle_diameter"] = nozzle_str;
            entry["materials"]       = materials_str;
            entry["cover"]           = into_u8(file_url_from_path(cover_path));
            entry["nozzle_selected"] = "";
            entry["sub_path"]        = "";
            data["model"].push_back(entry);
        }
    }

    // Machine map: preset name -> {model, nozzle variant}
    for (const Preset& p : bundle.printers()) {
        if (!p.is_system || !p.vendor)
            continue;
        const auto* printer_model   = p.config.option<ConfigOptionString>("printer_model");
        const auto* printer_variant = p.config.option<ConfigOptionString>("printer_variant");
        if (!printer_model || printer_model->value.empty() || !printer_variant)
            continue;

        json mach;
        mach["model"]           = printer_model->value;
        mach["nozzle"]          = printer_variant->value;
        data["machine"][p.name] = mach;
    }

    // Filament map from system filament presets (vendor/type already resolved in config)
    const json& machines = data["machine"];
    for (const Preset& p : bundle.filaments()) {
        if (!p.is_system || !p.vendor)
            continue;
        const auto* fila_vendor     = p.config.option<ConfigOptionStrings>("filament_vendor");
        const auto* fila_type       = p.config.option<ConfigOptionStrings>("filament_type");
        const auto* compat_printers = p.config.option<ConfigOptionStrings>("compatible_printers");

        std::string vendor = (fila_vendor && !fila_vendor->values.empty()) ? fila_vendor->values[0] : "";
        std::string type   = (fila_type && !fila_type->values.empty()) ? fila_type->values[0] : "";

        std::string model_list;
        if (compat_printers) {
            for (const std::string& pname : compat_printers->values) {
                auto it = machines.find(pname);
                if (it != machines.end()) {
                    const std::string m = (*it)["model"];
                    const std::string n = (*it)["nozzle"];
                    model_list += "[" + m + "++" + n + "]";
                }
            }
        }

        json ff;
        ff["name"]               = p.name;
        ff["sub_path"]           = p.file;
        ff["vendor"]             = vendor;
        ff["type"]               = type;
        ff["models"]             = model_list;
        ff["selected"]           = 0;
        data["filament"][p.name] = ff;
    }

    // Process list from visible system print presets
    for (const Preset& p : bundle.prints()) {
        if (!p.is_system || !p.vendor || !p.is_visible)
            continue;
        json entry;
        entry["name"]     = p.name;
        entry["sub_path"] = p.file;
        data["process"].push_back(entry);
    }

    BOOST_LOG_TRIVIAL(info) << "printer_profile_data: built profile data (" << data["model"].size() << " models, "
                            << data["machine"].size() << " machines, " << data["filament"].size() << " filaments)";
    return data;
}

Outcome<json> printer_profile_data(const std::atomic<bool>* cancel)
{
    const boost::filesystem::path vendor_dir      = (boost::filesystem::path(data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    const boost::filesystem::path rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    try {
        // A vendor in the user's system dir shadows the bundled one of that name. vendor_names_in names a vendor by its
        // profile or, where a build ships preset caches instead, by its cache alone.
        std::map<std::string, boost::filesystem::path> vendor_sources;
        for (const boost::filesystem::path& dir : {vendor_dir, rsrc_vendor_dir}) {
            boost::system::error_code ec;
            if (boost::filesystem::exists(dir, ec))
                for (const std::string& name : vendor_names_in(dir))
                    vendor_sources.emplace(name, dir); // first dir wins
        }

        // The load order: the filament library first, because the others'
        // filaments inherit from it, then every versioned vendor — each loaded
        // from the directory it was found in, so a vendor that is not installed
        // is served from the shipped profiles. Each is stamped by name and
        // version alone: a profile change requires a version bump, so those two
        // determine content wherever the vendor's copy sits.
        std::vector<PresetBundle::VendorSource> ordered;
        json                                    stamps = json::array();
        auto add_vendor = [&ordered, &stamps](const std::string& name, const boost::filesystem::path& dir) {
            // The version a load from `dir` would serve: the profile's where one
            // exists (a cache is only served while it covers the profile beside
            // it), the cache's own stamp where the cache is the whole vendor.
            // A profile without a version (blacklist.json) carries no presets
            // and is passed over.
            const boost::filesystem::path profile = dir / (name + ".json");
            std::string                   version;
            if (boost::filesystem::exists(profile)) {
                const Semver v = get_version_from_json(profile.string());
                if (!v.valid())
                    return;
                version = v.to_string();
            } else {
                version = VendorCacheFile::peek_version((dir / (name + ".opc")).string(), name);
            }
            ordered.push_back({name, dir});
            stamps.push_back({name, version});
        };
        const std::string filament_library(PresetBundle::ORCA_FILAMENT_LIBRARY);
        if (auto it = vendor_sources.find(filament_library); it != vendor_sources.end())
            add_vendor(filament_library, it->second);
        for (const auto& [name, dir] : vendor_sources)
            if (name != filament_library)
                add_vendor(name, dir);
        if (ordered.empty())
            return Refusal{RefusalKind::Failed, "no_vendors", L("No printer profiles were found.")};

        // What follows is a pure function of that stamped set, so the derived
        // JSON is cached whole: a fresh cache makes the data one file read, with
        // no bundle built and no preset installed. Stale or absent, the bundle is
        // rebuilt below and the result written back.
        const boost::filesystem::path cache_file = boost::filesystem::path(data_dir()) / "cache" / "wizard_profile_data.json";
        try {
            // Slurped whole and parsed from the buffer — nlohmann's fastest
            // input path; a stream adapter costs real time on a multi-MB file.
            boost::nowide::ifstream ifs(cache_file.string(), std::ios::binary);
            if (ifs.is_open()) {
                const std::string text{std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
                json              cached = json::parse(text);
                if (cached.value("format", 0) == 1 && cached["vendors"] == stamps && !cached["profile"]["machine"].empty()) {
                    json data = json::object();
                    for (const char* key : {"model", "machine", "filament", "process"})
                        data[key] = std::move(cached["profile"][key]);
                    BOOST_LOG_TRIVIAL(info) << "printer_profile_data: served from " << cache_file;
                    return data;
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(info) << "printer_profile_data: rejecting the cached data: " << e.what();
        }

        // Each vendor comes from its preset cache where one covers it, which is
        // what makes this worth doing instead of the dialog's scan of the vendors' JSON files.
        PresetBundle             bundle;
        std::vector<std::string> failed;
        const std::string        errors = bundle.load_vendors(ordered, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                                              /*allow_cache=*/true, cancel, &failed)
                                       .second;
        if (cancel != nullptr && *cancel)
            return Refusal{RefusalKind::Failed, "cancelled", L("Reading the printer profiles was cancelled.")};
        if (bundle.vendors.empty())
            return Refusal{RefusalKind::Failed, "no_vendors", L("No printer profiles were found.")};
        if (!errors.empty())
            BOOST_LOG_TRIVIAL(warning) << "printer_profile_data: loading the vendors reported: " << errors;
        if (!failed.empty())
            return Refusal{RefusalKind::Failed, "vendors_not_loaded", L("These printer vendors could not be loaded: %1%."),
                           {boost::algorithm::join(failed, ", ")}, failed};
        json data = profile_data_from(bundle);
        if (data["machine"].empty())
            return Refusal{RefusalKind::Failed, "no_vendors", L("No printer profiles were found.")};

        // Written through a temp file and moved into place, as the preset caches
        // are: half a cache must never be readable.
        try {
            json out;
            out["format"]  = 1;
            out["vendors"] = std::move(stamps);
            out["profile"] = data;
            boost::filesystem::create_directories(cache_file.parent_path());
            const std::string text = out.dump(-1, ' ', false, json::error_handler_t::ignore);
            if (const std::error_code ec = write_file_atomically(cache_file.string(), text, /*binary=*/true))
                throw std::runtime_error(ec.message());
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "printer_profile_data: could not write the cache: " << e.what();
        }
        return data;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "printer_profile_data failed: " << e.what();
        // TRN %1% is the error's own text.
        return Refusal{RefusalKind::Failed, "profile_data_unreadable", L("The printer profiles could not be read: %1%"), {e.what()}};
    }
}

InstallSet installed_set_from(const AppConfig& config, const std::vector<std::string>& offered_filaments)
{
    InstallSet set;
    set.printers = config.vendors();
    if (config.has_section(AppConfig::SECTION_FILAMENTS))
        set.filaments = config.get_section(AppConfig::SECTION_FILAMENTS);
    else
        for (const std::string& name : offered_filaments)
            set.filaments[name] = "true";
    return set;
}

InstallSet installed_set()
{
    assert(wxThread::IsMain());
    return installed_set_from(*wxGetApp().app_config, offered(wxGetApp().preset_bundle->filaments));
}

InstallSet changed_install_set(const InstallSet& installed, const std::vector<ModelNozzles>& add, const std::vector<ModelNozzles>& remove)
{
    InstallSet changed = installed;
    for (const ModelNozzles& model : add)
        if (!model.nozzles.empty())
            changed.printers[model.vendor][model.model].insert(model.nozzles.begin(), model.nozzles.end());
    for (const ModelNozzles& model : remove) {
        const auto vendor = changed.printers.find(model.vendor);
        if (vendor == changed.printers.end())
            continue;
        const auto nozzles = vendor->second.find(model.model);
        if (nozzles == vendor->second.end())
            continue;
        const std::size_t had = nozzles->second.size();
        for (const std::string& nozzle : model.nozzles)
            nozzles->second.erase(nozzle);
        if (nozzles->second.size() == had)
            continue;
        if (nozzles->second.empty())
            vendor->second.erase(nozzles);
        if (vendor->second.empty())
            changed.printers.erase(vendor);
    }
    return changed;
}

std::optional<Refusal> uninstall_refusal(const InstallSet& installed, const std::vector<ModelNozzles>& remove)
{
    if (nozzle_count(installed) > 0 && nozzle_count(changed_install_set(installed, {}, remove)) == 0)
        return Refusal{RefusalKind::Precondition, "last_printer", L("At least one printer model must stay installed.")};
    return std::nullopt;
}

std::vector<PrinterModel> printer_models_from(const json& profile, const InstallSet& installed, const std::string& vendor,
                                              const std::string& query)
{
    std::vector<PrinterModel> models;
    const auto                entries = profile.find("model");
    if (entries == profile.end() || !entries->is_array())
        return models;
    const std::string wanted_vendor = lower(vendor);
    const std::string wanted        = lower(query);
    for (const json& entry : *entries) {
        if (!entry.is_object())
            continue;
        PrinterModel model;
        model.vendor = text(entry, "vendor");
        model.model  = text(entry, "model");
        if (!wanted_vendor.empty() && lower(model.vendor) != wanted_vendor)
            continue;
        if (!wanted.empty() && lower(model.model).find(wanted) == std::string::npos &&
            lower(text(entry, "name")).find(wanted) == std::string::npos)
            continue;
        model.nozzles = split_nozzles(text(entry, "nozzle_diameter"));
        if (const auto v = installed.printers.find(model.vendor); v != installed.printers.end())
            if (const auto m = v->second.find(model.model); m != v->second.end())
                for (const std::string& nozzle : model.nozzles)
                    if (m->second.count(nozzle) != 0)
                        model.installed_nozzles.push_back(nozzle);
        models.push_back(std::move(model));
    }
    return models;
}

Outcome<std::vector<PrinterModel>> printer_models(const PrinterModelsArgs& args)
{
    const Outcome<json> profile = printer_profile_data(args.cancel);
    if (!profile)
        return profile.refusal();
    if (!args.vendor.empty()) {
        const std::vector<std::string> vendors = profile_vendors(profile.value());
        if (std::none_of(vendors.begin(), vendors.end(), [&args](const std::string& v) { return lower(v) == lower(args.vendor); }))
            return no_such_vendor(args.vendor, vendors);
    }
    return printer_models_from(profile.value(), args.installed, args.vendor, args.query);
}

Outcome<std::vector<ModelNozzles>> resolve_models(const std::vector<PrinterModel>& known, const std::vector<ModelNozzles>& asked)
{
    std::vector<std::string> vendors;
    for (const PrinterModel& model : known)
        vendors.push_back(model.vendor);
    std::sort(vendors.begin(), vendors.end());
    vendors.erase(std::unique(vendors.begin(), vendors.end()), vendors.end());

    std::vector<ModelNozzles> resolved;
    for (const ModelNozzles& want : asked) {
        std::string vendor;
        if (!want.vendor.empty()) {
            const auto it = std::find_if(vendors.begin(), vendors.end(),
                                         [&want](const std::string& v) { return lower(v) == lower(want.vendor); });
            if (it == vendors.end())
                return no_such_vendor(want.vendor, vendors);
            vendor = *it;
        }
        // The models of that id, in any case, of the vendor named; one id the same in every case is its own.
        std::vector<const PrinterModel*> found;
        for (const PrinterModel& model : known)
            if ((vendor.empty() || model.vendor == vendor) && lower(model.model) == lower(want.model))
                found.push_back(&model);
        if (std::any_of(found.begin(), found.end(), [&want](const PrinterModel* m) { return m->model == want.model; }))
            found.erase(std::remove_if(found.begin(), found.end(), [&want](const PrinterModel* m) { return m->model != want.model; }),
                        found.end());
        if (found.empty()) {
            std::vector<std::string> names;
            for (const PrinterModel& model : known)
                if (vendor.empty() || model.vendor == vendor)
                    names.push_back(model.model);
            if (vendor.empty())
                return Refusal{RefusalKind::Precondition, "no_such_model", L("No printer model is called \"%1%\"."), {want.model},
                               closest_names(std::move(names), want.model)};
            // TRN %1% is a printer vendor, %2% the printer model asked for.
            return Refusal{RefusalKind::Precondition, "no_such_model", L("%1% has no printer model called \"%2%\"."),
                           {vendor, want.model}, closest_names(std::move(names), want.model)};
        }
        std::vector<std::string> owners;
        for (const PrinterModel* model : found)
            if (std::find(owners.begin(), owners.end(), model->vendor) == owners.end())
                owners.push_back(model->vendor);
        if (owners.size() > 1) {
            std::sort(owners.begin(), owners.end());
            return Refusal{RefusalKind::Precondition, "ambiguous_model",
                           L("\"%1%\" is a printer model of several vendors: name its vendor."), {want.model}, owners};
        }
        const PrinterModel& model = *found.front();
        ModelNozzles        named{model.vendor, model.model, {}};
        if (want.nozzles.empty())
            named.nozzles = model.nozzles;
        for (const std::string& nozzle : want.nozzles) {
            if (std::find(model.nozzles.begin(), model.nozzles.end(), nozzle) == model.nozzles.end())
                // TRN %1% is a printer model, %2% the nozzle asked for ("0.4").
                return Refusal{RefusalKind::Precondition, "no_such_nozzle", L("%1% has no nozzle \"%2%\"."), {model.model, nozzle},
                               model.nozzles};
            if (std::find(named.nozzles.begin(), named.nozzles.end(), nozzle) == named.nozzles.end())
                named.nozzles.push_back(nozzle);
        }
        resolved.push_back(std::move(named));
    }
    return resolved;
}

Outcome<Done> apply_install_set(const InstallSet& set, PresetChanges changes)
{
    assert(wxThread::IsMain());
    GUI_App&      app           = wxGetApp();
    AppConfig*    app_config    = app.app_config;
    PresetBundle* preset_bundle = app.preset_bundle;

    const auto& enabled_vendors       = set.printers;
    const auto  old_enabled_vendors   = app_config->vendors();
    const auto& enabled_filaments     = set.filaments;
    const auto  old_enabled_filaments = app_config->has_section(AppConfig::SECTION_FILAMENTS) ?
                                            app_config->get_section(AppConfig::SECTION_FILAMENTS) :
                                            std::map<std::string, std::string>();

    // The reload drops the presets' unsaved changes: they are asked about, or the caller's answer applies.
    bool keep_changes = false;
    if (enabled_vendors != old_enabled_vendors || enabled_filaments != old_enabled_filaments) {
        if (changes == PresetChanges::Ask) {
            const wxString header  = _L("The configuration package is changed in previous Config Guide");
            const wxString caption = _L("Configuration package changed");
            if (!app.check_and_keep_current_preset_changes(caption, header, ActionButtons::KEEP | ActionButtons::SAVE, &keep_changes))
                return Refusal{RefusalKind::Precondition, "selection_cancelled",
                               L("The question about the unsaved changes was cancelled.")};
        } else if (app.has_current_preset_changes()) {
            if (changes == PresetChanges::Discard) {
                app.discard_current_preset_changes();
            } else {
                cache_listed_changes();
                keep_changes = true;
            }
        }
    }

    std::string       preferred_model;
    std::string       preferred_variant;
    auto get_preferred_printer_model = [preset_bundle, &enabled_vendors, &old_enabled_vendors](const std::string& bundle_name,
                                                                                               std::string&       variant) {
        const auto config = enabled_vendors.find(bundle_name);
        if (config == enabled_vendors.end())
            return std::string();

        const VendorProfile&                                printer_profile = preset_bundle->vendors[bundle_name];
        const std::map<std::string, std::set<std::string>>& model_maps      = config->second;
        for (const auto& model_it : model_maps) {
            if (model_it.second.size() > 0) {
                variant = *model_it.second.begin();
                if (model_it.second.size() > 1) {
                    if (printer_profile.models.size() > 0) {
                        const auto is_model = [id = model_it.first](auto& m) { return m.id == id; };
                        const VendorProfile::PrinterModel& printer_model =
                            *std::find_if(printer_profile.models.begin(), printer_profile.models.end(), is_model);
                        for (auto& vt : printer_model.variants) {
                            if (std::find(model_it.second.begin(), model_it.second.end(), vt.name) != model_it.second.end()) {
                                variant = vt.name;
                                break;
                            }
                        }
                    } else if (variant != PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT) {
                        if (std::find(model_it.second.begin(), model_it.second.end(), PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT) !=
                            model_it.second.end())
                            variant = PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT;
                    }
                }

                const auto config_old = old_enabled_vendors.find(bundle_name);
                if (config_old == old_enabled_vendors.end())
                    return model_it.first;
                const auto model_it_old = config_old->second.find(model_it.first);
                if (model_it_old == config_old->second.end())
                    return model_it.first;
                else if (model_it_old->second != model_it.second) {
                    for (const auto& var : model_it.second)
                        if (model_it_old->second.find(var) == model_it_old->second.end()) {
                            variant = var;
                            return model_it.first;
                        }
                }
            }
        }
        if (!variant.empty())
            variant.clear();
        return std::string();
    };
    // Orca "custom" printers are considered first, then 3rd party.
    if (preferred_model = get_preferred_printer_model(PresetBundle::ORCA_DEFAULT_BUNDLE, preferred_variant); preferred_model.empty()) {
        for (const auto& bundle : enabled_vendors) {
            if (bundle.first == PresetBundle::ORCA_DEFAULT_BUNDLE) {
                continue;
            }
            if (preferred_model = get_preferred_printer_model(bundle.first, preferred_variant); !preferred_model.empty())
                break;
        }
    }

    // Installs the bundles from the shipped profiles, writes both sections and reloads the presets. A @System filament
    // is replaced by a vendor's own only where the vendor's profile lists default filaments, which an INI bundle does
    // and the shipped JSON profiles do not.
    BOOST_LOG_TRIVIAL(info) << "apply_install_set: calling apply_vendor_config";
    if (!preset_bundle->apply_vendor_config(enabled_vendors, enabled_filaments, app_config, true, preferred_model, preferred_variant))
        return Refusal{RefusalKind::Failed, "vendors_not_installed", L("The printer profiles could not be installed.")};

    if (keep_changes)
        app.apply_keeped_preset_modifications();
    app_config->set_legacy_datadir(false);
    app.update_mode();
    if (Plater* plater = app.plater())
        plater->normalize_bed_types(false);
    return Done{};
}

Outcome<InstalledPrinters> install_printers(const InstallPrintersArgs& args) { return change_printers(args, true); }

Outcome<InstalledPrinters> uninstall_printers(const InstallPrintersArgs& args) { return change_printers(args, false); }

// ---- System filaments ----

std::vector<FilamentRow> loaded_filaments()
{
    assert(wxThread::IsMain());
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const AppConfig&    config = *wxGetApp().app_config;
    const auto          first  = [](const Preset& preset, const char* key) {
        const auto* values = preset.config.option<ConfigOptionStrings>(key);
        return values != nullptr && !values->values.empty() ? values->values.front() : std::string();
    };
    // A printer is installed where the models section names its model and nozzle under its vendor.
    const auto installed = [&bundle, &config](const std::string& printer_name) {
        const Preset* printer = bundle.printers.find_preset(printer_name, false);
        return printer != nullptr && printer->is_system && printer->vendor != nullptr &&
               config.get_variant(printer->vendor->id, printer->config.opt_string("printer_model"),
                                  printer->config.opt_string("printer_variant"));
    };
    std::vector<FilamentRow> rows;
    for (const Preset& preset : bundle.filaments) {
        if (!preset.is_system || preset.vendor == nullptr || preset.vendor->id == PRESET_TEMPLATE_DIR)
            continue;
        FilamentRow row{preset.name, preset.alias.empty() ? preset.name : preset.alias, first(preset, "filament_vendor"),
                        first(preset, "filament_type")};
        const auto* printers = preset.config.option<ConfigOptionStrings>("compatible_printers");
        row.fits = printers == nullptr || printers->values.empty() ||
                   std::any_of(printers->values.begin(), printers->values.end(), installed);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> filament_presets_named(const std::vector<FilamentRow>& loaded, const std::string& name)
{
    if (std::any_of(loaded.begin(), loaded.end(), [&name](const FilamentRow& row) { return row.name == name; }))
        return {name};
    std::vector<std::string> presets;
    for (const FilamentRow& row : loaded)
        if (row.alias == name && row.fits)
            presets.push_back(row.name);
    return presets;
}

Outcome<std::vector<std::string>> filaments_named(const std::vector<FilamentRow>& loaded, const InstallFilamentsArgs& args)
{
    std::vector<std::string> presets;
    const auto               add = [&presets](const std::string& name) {
        if (std::find(presets.begin(), presets.end(), name) == presets.end())
            presets.push_back(name);
    };
    if (args.names.empty()) {
        const std::string vendor = lower(args.vendor), type = lower(args.filament_type);
        std::vector<std::string> vendors, types;
        for (const FilamentRow& row : loaded) {
            vendors.push_back(row.vendor);
            if (lower(row.vendor) != vendor || !row.fits)
                continue;
            if (std::find(types.begin(), types.end(), row.type) == types.end())
                types.push_back(row.type);
            if (lower(row.type) == type)
                add(row.name);
        }
        if (!presets.empty())
            return presets;
        if (std::none_of(loaded.begin(), loaded.end(), [&vendor](const FilamentRow& row) { return lower(row.vendor) == vendor; }))
            return Refusal{RefusalKind::Precondition, "no_filament_vendor", L("No system filament is of the vendor \"%1%\"."),
                           {args.vendor}, closest_names(std::move(vendors), args.vendor)};
        std::sort(types.begin(), types.end());
        return Refusal{RefusalKind::Precondition, "no_matching_filament",
                       // TRN %1% is a filament vendor ("Bambu Lab"), %2% a filament type ("PLA").
                       L("No system filament of %1% of type %2% fits an installed printer."), {args.vendor, args.filament_type}, types};
    }
    for (const std::string& name : args.names) {
        const std::vector<std::string> named = filament_presets_named(loaded, name);
        if (named.empty()) {
            std::vector<std::string> unfit, candidates;
            for (const FilamentRow& row : loaded) {
                if (row.alias == name)
                    unfit.push_back(row.name);
                candidates.push_back(row.alias);
                candidates.push_back(row.name);
            }
            if (!unfit.empty())
                return Refusal{RefusalKind::Precondition, "fits_no_printer", L("No installed printer fits the filament \"%1%\"."), {name},
                               unfit};
            return Refusal{RefusalKind::Precondition, "no_such_filament", L("No system filament is called \"%1%\"."), {name},
                           closest_names(std::move(candidates), name)};
        }
        for (const std::string& preset : named)
            add(preset);
    }
    return presets;
}

std::optional<Refusal> filament_uninstall_refusal(const std::vector<std::string>& presets, const std::vector<std::string>& slot_presets)
{
    for (std::size_t slot = 0; slot < slot_presets.size(); ++slot)
        if (std::find(presets.begin(), presets.end(), slot_presets[slot]) != presets.end())
            // TRN %1% is a filament slot's number, %2% the filament preset it uses.
            return Refusal{RefusalKind::Precondition, "filament_in_use", L("Filament slot %1% uses %2%."),
                           {std::to_string(slot + 1), slot_presets[slot]}, {slot_presets[slot]}};
    return std::nullopt;
}

Outcome<InstalledFilaments> install_filaments(const InstallFilamentsArgs& args) { return change_filaments(args, true); }

Outcome<InstalledFilaments> uninstall_filaments(const InstallFilamentsArgs& args) { return change_filaments(args, false); }

}}} // namespace Slic3r::GUI::Ops
