// Prints what the profile `include` key contributes to one vendor's system
// presets, so that the dump from OrcaSlicer and the one from BambuStudio can be
// diffed: the two copies of this file differ only in how they load the vendor.
// For every preset whose values pass through an included template, from its
// own file or an ancestor's, it prints the templates those files name, in the
// order they apply, then the loaded value of each key the templates set. Every
// line starts with the preset, so a plain diff names the preset of each
// difference.
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include "nlohmann/json.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

// What a sub-file states about where its values come from, and what it sets.
struct Entry
{
    std::string              inherits;
    std::vector<std::string> includes;
    std::vector<std::string> keys;
};

// Sub-files by name, per section of the vendor index: `inherits` and `include`
// both resolve within a section.
using Sections = std::map<std::string, std::map<std::string, Entry>>;

json read_json(const fs::path &path)
{
    boost::nowide::ifstream in(path.string());
    if (!in)
        throw std::runtime_error("Cannot read " + path.string());
    return json::parse(in);
}

Sections read_vendor(const fs::path &dir, const std::string &vendor)
{
    // Keys that name and place a sub-file rather than set a value.
    static const std::set<std::string> metadata = {"name", "type", "from", "instantiation", "inherits", "include"};
    Sections   sections;
    const json index = read_json(dir / (vendor + ".json"));
    for (const char *section : {"process_list", "filament_list", "machine_list"}) {
        const auto list = index.find(section);
        if (list == index.end())
            continue;
        for (const json &item : *list) {
            const json file  = read_json(dir / vendor / item.at("sub_path").get<std::string>());
            // A file without a name goes by its name in the index.
            Entry     &entry = sections[section][file.value("name", item.at("name").get<std::string>())];
            entry.inherits   = file.value("inherits", "");
            if (const auto include = file.find("include"); include != file.end()) {
                if (include->is_string())
                    entry.includes.push_back(include->get<std::string>());
                else
                    for (const json &name : *include)
                        entry.includes.push_back(name.get<std::string>());
            }
            for (auto it = file.begin(); it != file.end(); ++it)
                if (metadata.count(it.key()) == 0)
                    entry.keys.push_back(it.key());
        }
    }
    return sections;
}

const PresetCollection &presets_of(const PresetBundle &bundle, const std::string &section)
{
    if (section == "process_list")
        return bundle.prints;
    if (section == "filament_list")
        return bundle.filaments;
    return bundle.printers;
}

size_t dump(const PresetBundle &bundle, const Sections &sections, std::ostream &out)
{
    size_t dumped = 0;
    for (const auto &[section, entries] : sections) {
        std::vector<const Preset *> presets;
        for (const Preset &preset : presets_of(bundle, section).get_presets())
            if (preset.is_system)
                presets.push_back(&preset);
        std::sort(presets.begin(), presets.end(), [](const Preset *a, const Preset *b) { return a->name < b->name; });
        for (const Preset *preset : presets) {
            // The preset and its ancestors, root first: the order their includes apply in.
            std::vector<const Entry *> lineage;
            for (auto it = entries.find(preset->name); it != entries.end() && lineage.size() <= entries.size();
                 it = entries.find(it->second.inherits))
                lineage.insert(lineage.begin(), &it->second);
            std::string           templates;
            std::set<std::string> keys;
            for (const Entry *entry : lineage)
                for (const std::string &name : entry->includes) {
                    templates += (templates.empty() ? "" : "; ") + name;
                    if (const auto it = entries.find(name); it != entries.end())
                        keys.insert(it->second.keys.begin(), it->second.keys.end());
                }
            if (templates.empty())
                continue;
            const std::string prefix = section.substr(0, section.find('_')) + " | " + preset->name + " | ";
            out << prefix << "include = " << templates << "\n";
            for (const std::string &key : keys)
                out << prefix << key << " = " << (preset->config.has(key) ? preset->config.opt_serialize(key) : "<absent>") << "\n";
            ++dumped;
        }
    }
    return dumped;
}

// Orca: load the vendor as the app does, against the filament library when the
// directory has one: parsed from its JSON files or, with from_cache, from the
// preset cache the app reads on every launch after the first.
void load_vendor(PresetBundle &bundle, const std::string &dir, const std::string &vendor, bool from_cache)
{
    const auto          rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
    PresetBundle        library;
    const PresetBundle *base = nullptr;
    if (fs::is_regular_file(fs::path(dir) / (std::string(PresetBundle::ORCA_FILAMENT_LIBRARY) + ".json"))) {
        library.load_vendor_configs_from_json(dir, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, rule, nullptr, false);
        base = &library;
    }
    if (!from_cache) {
        bundle.load_vendor_configs_from_json(dir, vendor, PresetBundle::LoadSystem, rule, base, false);
        return;
    }
    // Parse once to write <vendor>.opc beside the profile, then load that
    // cache into a clean bundle. Any cache already there goes first, so that a
    // failed write cannot leave it to be loaded; one that was not there before
    // goes again afterwards.
    const fs::path cache     = fs::path(dir) / (vendor + ".opc");
    const bool     had_cache = fs::exists(cache);
    fs::remove(cache);
    PresetBundle parsed;
    parsed.set_is_validation_mode(true); // parse the JSON: validation never serves a cache
    parsed.set_generate_vendor_caches(true);
    parsed.load_vendor_configs_from_json(dir, vendor, PresetBundle::LoadSystem, rule, base);
    const bool loaded = bundle.load_vendor_cache(cache.string(), vendor, parsed.vendors.at(vendor).config_version, base);
    if (!had_cache)
        fs::remove(cache);
    if (!loaded)
        throw std::runtime_error("The preset cache " + cache.string() + " was not written or was rejected");
}

} // namespace

int main(int argc, char *argv[])
{
    const char *usage = "Usage: profile_include_dump -p <profiles dir> -o <dump file> [-v <vendor>] [-l <log level>] [-c]\n"
                        "  -v  vendor to dump, BBL if omitted\n"
                        "  -l  log level, 0 (fatal) to 5 (trace); 1 if omitted\n"
                        "  -c  load the presets from the vendor's preset cache, generated first, not the JSON\n";
    std::string dir, output, vendor = "BBL";
    unsigned    log_level  = 1;
    bool        from_cache = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-c") {
            from_cache = true;
            continue;
        }
        if (i + 1 == argc) {
            std::cerr << usage;
            return 1;
        }
        const std::string value = argv[++i];
        if (arg == "-p")
            dir = value;
        else if (arg == "-o")
            output = value;
        else if (arg == "-v")
            vendor = value;
        else if (arg == "-l" && value.size() == 1 && value[0] >= '0' && value[0] <= '5')
            log_level = unsigned(value[0] - '0');
        else {
            std::cerr << usage;
            return 1;
        }
    }
    if (dir.empty() || output.empty() || !fs::is_directory(dir)) {
        std::cerr << usage;
        return 1;
    }
    set_logging_level(log_level);

    try {
        PresetBundle bundle;
        load_vendor(bundle, dir, vendor, from_cache);
        boost::nowide::ofstream out(output);
        if (!out)
            throw std::runtime_error("Cannot write " + output);
        const size_t dumped = dump(bundle, read_vendor(dir, vendor), out);
        out.close();
        if (!out)
            throw std::runtime_error("Failed writing " + output);
        std::cerr << "Dumped " << dumped << " " << vendor << " presets that include a template\n";
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
